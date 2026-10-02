package com.motionforge.app.media

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.media.ExifInterface
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMetadataRetriever
import android.net.Uri
import android.os.Build
import android.os.ParcelFileDescriptor
import android.util.Log
import com.motionforge.app.engine.NativeBridge
import org.json.JSONObject
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest

/**
 * Media access used by the native renderer (called from C++ through JNI) and by the importer.
 * Video is decoded with MediaCodec (sequential, seek-aware); MediaMetadataRetriever is the fallback.
 */
object MediaBridge {
    private const val TAG = "MediaBridge"
    lateinit var context: Context
        private set
    private val decoders = LinkedHashMap<String, VideoDecoder>(8, 0.75f, true)
    private val audioLocks = HashMap<String, Any>()

    fun init(ctx: Context) {
        context = ctx.applicationContext
    }

    private fun source(asset: JSONObject): String = asset.optString("path").ifEmpty { asset.optString("uri") }

    fun openFd(src: String): ParcelFileDescriptor? = try {
        if (src.startsWith("content:") || src.startsWith("file:")) context.contentResolver.openFileDescriptor(Uri.parse(src), "r")
        else ParcelFileDescriptor.open(File(src), ParcelFileDescriptor.MODE_READ_ONLY)
    } catch (e: Exception) {
        null
    }

    fun setDataSource(extractor: MediaExtractor, src: String) {
        if (src.startsWith("content:") || src.startsWith("file:")) extractor.setDataSource(context, Uri.parse(src), null)
        else extractor.setDataSource(src)
    }

    fun setDataSource(mmr: MediaMetadataRetriever, src: String) {
        if (src.startsWith("content:") || src.startsWith("file:")) mmr.setDataSource(context, Uri.parse(src)) else mmr.setDataSource(src)
    }

    // ------------------------------------------------------------ called from native
    @JvmStatic
    fun videoFrame(assetJson: String, timeSec: Double, maxW: Int, maxH: Int): Bitmap? {
        val asset = JSONObject(assetJson)
        val id = asset.optString("id")
        val src = source(asset)
        if (src.isEmpty()) return null
        val dec = synchronized(decoders) {
            decoders[id] ?: try {
                VideoDecoder(src).also {
                    decoders[id] = it
                    while (decoders.size > 4) {
                        val eldest = decoders.entries.first()
                        eldest.value.release()
                        decoders.remove(eldest.key)
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "decoder init failed for $id: ${e.message}")
                null
            }
        }
        val bmp = try {
            dec?.frameAt((timeSec * 1_000_000).toLong(), maxW, maxH)
        } catch (e: Exception) {
            Log.w(TAG, "decode failed: ${e.message}")
            null
        }
        return bmp ?: retrieverFrame(src, timeSec, maxW, maxH)
    }

    private fun retrieverFrame(src: String, t: Double, w: Int, h: Int): Bitmap? = try {
        val mmr = MediaMetadataRetriever()
        setDataSource(mmr, src)
        val us = (t * 1_000_000).toLong()
        val b = if (Build.VERSION.SDK_INT >= 27) mmr.getScaledFrameAtTime(us, MediaMetadataRetriever.OPTION_CLOSEST, w, h)
        else mmr.getFrameAtTime(us, MediaMetadataRetriever.OPTION_CLOSEST)
        mmr.release()
        b?.let { if (it.config != Bitmap.Config.ARGB_8888) it.copy(Bitmap.Config.ARGB_8888, false) else it }
    } catch (e: Exception) {
        null
    }

    @JvmStatic
    fun image(assetJson: String, maxW: Int, maxH: Int): Bitmap? {
        val asset = JSONObject(assetJson)
        val src = source(asset)
        return try {
            val opts = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            openStream(src)?.use { BitmapFactory.decodeStream(it, null, opts) }
            var sample = 1
            while (opts.outWidth / (sample * 2) >= maxW && opts.outHeight / (sample * 2) >= maxH) sample *= 2
            val dec = BitmapFactory.Options().apply { inSampleSize = sample; inPreferredConfig = Bitmap.Config.ARGB_8888 }
            val bmp = openStream(src)?.use { BitmapFactory.decodeStream(it, null, dec) } ?: return null
            // Apply EXIF orientation so photos appear upright.
            val rot = try {
                openStream(src)?.use { ExifInterface(it).rotationDegrees } ?: 0
            } catch (e: Exception) {
                0
            }
            if (rot != 0) Bitmap.createBitmap(bmp, 0, 0, bmp.width, bmp.height, Matrix().apply { postRotate(rot.toFloat()) }, true) else bmp
        } catch (e: Exception) {
            Log.w(TAG, "image decode failed: ${e.message}")
            null
        }
    }

    fun openStream(src: String) = try {
        if (src.startsWith("content:") || src.startsWith("file:")) context.contentResolver.openInputStream(Uri.parse(src)) else FileInputStream(src)
    } catch (e: Exception) {
        null
    }

    @JvmStatic
    fun available(assetJson: String): Boolean {
        val src = source(JSONObject(assetJson))
        if (src.isEmpty()) return false
        return openFd(src)?.use { true } ?: false
    }

    /** Decodes the asset's audio track to raw float32 stereo 48 kHz in the cache (deletable). */
    @JvmStatic
    fun audioPcmPath(assetJson: String): String? {
        val asset = JSONObject(assetJson)
        val src = source(asset)
        if (src.isEmpty()) return null
        val key = sha1(src + asset.optString("checksum"))
        val lock = synchronized(audioLocks) { audioLocks.getOrPut(key) { Any() } }
        synchronized(lock) {
            val dir = File(context.cacheDir, "audio").apply { mkdirs() }
            val out = File(dir, "$key.f32")
            if (out.exists() && out.length() > 0) return out.absolutePath
            return try {
                decodeAudio(src, File(dir, "$key.tmp"))?.let { tmp -> if (tmp.renameTo(out)) out.absolutePath else null }
            } catch (e: Exception) {
                Log.w(TAG, "audio decode failed: ${e.message}")
                null
            }
        }
    }

    private fun decodeAudio(src: String, tmp: File): File? {
        val ex = MediaExtractor()
        setDataSource(ex, src)
        var track = -1
        for (i in 0 until ex.trackCount) if (ex.getTrackFormat(i).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true) { track = i; break }
        if (track < 0) { ex.release(); return null }
        ex.selectTrack(track)
        val fmt = ex.getTrackFormat(track)
        val mime = fmt.getString(MediaFormat.KEY_MIME)!!
        val codec = MediaCodec.createDecoderByType(mime)
        codec.configure(fmt, null, null, 0)
        codec.start()
        var inRate = fmt.getInteger(MediaFormat.KEY_SAMPLE_RATE)
        var inCh = fmt.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
        var pcmFloat = false
        val resampler = StereoResampler(48000)
        val info = MediaCodec.BufferInfo()
        var inDone = false
        var outDone = false
        FileOutputStream(tmp).buffered(1 shl 16).use { os ->
            val outBuf = ByteBuffer.allocate(1 shl 16).order(ByteOrder.LITTLE_ENDIAN)
            while (!outDone) {
                if (!inDone) {
                    val ii = codec.dequeueInputBuffer(5000)
                    if (ii >= 0) {
                        val buf = codec.getInputBuffer(ii)!!
                        val n = ex.readSampleData(buf, 0)
                        if (n < 0) { codec.queueInputBuffer(ii, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM); inDone = true }
                        else { codec.queueInputBuffer(ii, 0, n, ex.sampleTime, 0); ex.advance() }
                    }
                }
                val oi = codec.dequeueOutputBuffer(info, 5000)
                if (oi == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    val of = codec.outputFormat
                    inRate = of.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                    inCh = of.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                    pcmFloat = Build.VERSION.SDK_INT >= 24 && of.containsKey(MediaFormat.KEY_PCM_ENCODING) &&
                        of.getInteger(MediaFormat.KEY_PCM_ENCODING) == android.media.AudioFormat.ENCODING_PCM_FLOAT
                } else if (oi >= 0) {
                    val b = codec.getOutputBuffer(oi)!!.order(ByteOrder.LITTLE_ENDIAN)
                    b.position(info.offset); b.limit(info.offset + info.size)
                    val frames = if (pcmFloat) info.size / 4 / inCh else info.size / 2 / inCh
                    val samples = FloatArray(frames * 2)
                    for (f in 0 until frames) {
                        var l: Float; var r: Float
                        if (pcmFloat) { l = b.float; r = if (inCh > 1) b.float else l; repeat(maxOf(0, inCh - 2)) { b.float } }
                        else { l = b.short / 32768f; r = if (inCh > 1) b.short / 32768f else l; repeat(maxOf(0, inCh - 2)) { b.short } }
                        samples[f * 2] = l; samples[f * 2 + 1] = r
                    }
                    codec.releaseOutputBuffer(oi, false)
                    val res = resampler.process(samples, inRate)
                    outBuf.clear()
                    for (v in res) {
                        if (outBuf.remaining() < 4) { os.write(outBuf.array(), 0, outBuf.position()); outBuf.clear() }
                        outBuf.putFloat(v)
                    }
                    os.write(outBuf.array(), 0, outBuf.position())
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) outDone = true
                }
            }
        }
        codec.stop(); codec.release(); ex.release()
        return tmp
    }

    fun releaseAll() = synchronized(decoders) {
        decoders.values.forEach { it.release() }
        decoders.clear()
    }

    fun sha1(s: String): String = MessageDigest.getInstance("SHA-1").digest(s.toByteArray()).joinToString("") { "%02x".format(it) }

    /** Fast content fingerprint: size + SHA-256 of the first and last MiB (full hashing of 4K video would block import). */
    fun quickChecksum(src: String): String {
        return try {
            openFd(src)?.use { pfd ->
                val size = pfd.statSize
                val md = MessageDigest.getInstance("SHA-256")
                md.update(size.toString().toByteArray())
                FileInputStream(pfd.fileDescriptor).channel.use { ch ->
                    val buf = ByteBuffer.allocate(1 shl 20)
                    ch.read(buf, 0); buf.flip(); md.update(buf)
                    if (size > (2 shl 20)) { buf.clear(); ch.read(buf, size - (1 shl 20)); buf.flip(); md.update(buf) }
                }
                "qsha256:" + md.digest().joinToString("") { "%02x".format(it) }
            } ?: ""
        } catch (e: Exception) {
            ""
        }
    }
}

/** Linear resampler to 48 kHz stereo that keeps fractional phase across chunks. */
class StereoResampler(private val outRate: Int) {
    private var phase = 0.0
    private var lastL = 0f
    private var lastR = 0f
    fun process(input: FloatArray, inRate: Int): FloatArray {
        if (inRate == outRate) return input
        val n = input.size / 2
        val step = inRate.toDouble() / outRate
        val out = ArrayList<Float>((n / step).toInt() * 2 + 4)
        while (phase < n) {
            val i = phase.toInt()
            val f = (phase - i).toFloat()
            val l0 = if (i == 0) lastL else input[(i - 1) * 2]
            val r0 = if (i == 0) lastR else input[(i - 1) * 2 + 1]
            val l1 = input[i * 2]
            val r1 = input[i * 2 + 1]
            out.add(l0 + (l1 - l0) * f); out.add(r0 + (r1 - r0) * f)
            phase += step
        }
        phase -= n
        if (n > 0) { lastL = input[(n - 1) * 2]; lastR = input[(n - 1) * 2 + 1] }
        return out.toFloatArray()
    }
}

/** Sequential MediaCodec decoder with seek handling; converts YUV output to RGBA natively. */
class VideoDecoder(src: String) {
    private val extractor = MediaExtractor()
    private val codec: MediaCodec
    private val track: Int
    private val width: Int
    private val height: Int
    private val bt709: Boolean
    private var lastPts = Long.MIN_VALUE
    private var lastBitmap: Bitmap? = null
    private var inputDone = false
    private val frameUs: Long

    init {
        MediaBridge.setDataSource(extractor, src)
        var t = -1
        for (i in 0 until extractor.trackCount) if (extractor.getTrackFormat(i).getString(MediaFormat.KEY_MIME)?.startsWith("video/") == true) { t = i; break }
        require(t >= 0) { "no video track" }
        track = t
        extractor.selectTrack(track)
        val fmt = extractor.getTrackFormat(track)
        width = fmt.getInteger(MediaFormat.KEY_WIDTH)
        height = fmt.getInteger(MediaFormat.KEY_HEIGHT)
        bt709 = height >= 720
        val fps = if (fmt.containsKey(MediaFormat.KEY_FRAME_RATE)) fmt.getInteger(MediaFormat.KEY_FRAME_RATE) else 30
        frameUs = 1_000_000L / maxOf(1, fps)
        codec = MediaCodec.createDecoderByType(fmt.getString(MediaFormat.KEY_MIME)!!)
        fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, android.media.MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
        codec.configure(fmt, null, null, 0)
        codec.start()
    }

    @Synchronized
    fun frameAt(us: Long, maxW: Int, maxH: Int): Bitmap? {
        val cached = lastBitmap
        if (cached != null && us >= lastPts && us < lastPts + frameUs) return cached
        if (lastPts == Long.MIN_VALUE || us < lastPts || us > lastPts + 1_500_000) {
            extractor.seekTo(us, MediaExtractor.SEEK_TO_PREVIOUS_SYNC)
            codec.flush()
            inputDone = false
            lastBitmap = null
        }
        val info = MediaCodec.BufferInfo()
        var spins = 0
        while (spins++ < 2000) {
            if (!inputDone) {
                val ii = codec.dequeueInputBuffer(2000)
                if (ii >= 0) {
                    val buf = codec.getInputBuffer(ii)!!
                    val n = extractor.readSampleData(buf, 0)
                    if (n < 0) { codec.queueInputBuffer(ii, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM); inputDone = true }
                    else { codec.queueInputBuffer(ii, 0, n, extractor.sampleTime, 0); extractor.advance() }
                }
            }
            val oi = codec.dequeueOutputBuffer(info, 4000)
            if (oi >= 0) {
                val eos = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                val pts = info.presentationTimeUs
                if (pts + frameUs / 2 >= us || eos) {
                    val bmp = convert(oi, maxW, maxH)
                    codec.releaseOutputBuffer(oi, false)
                    if (bmp != null) { lastPts = pts; lastBitmap = bmp }
                    return bmp ?: lastBitmap
                }
                codec.releaseOutputBuffer(oi, false)
                if (eos) return lastBitmap
            }
        }
        return lastBitmap
    }

    private fun convert(index: Int, maxW: Int, maxH: Int): Bitmap? {
        val img = codec.getOutputImage(index) ?: return null
        val crop = img.cropRect
        val w = crop.width()
        val h = crop.height()
        val scale = minOf(1.0, maxW.toDouble() / w, maxH.toDouble() / h).let { if (it <= 0) 1.0 else it }
        val bw = maxOf(1, (w * scale).toInt())
        val bh = maxOf(1, (h * scale).toInt())
        val bmp = Bitmap.createBitmap(bw, bh, Bitmap.Config.ARGB_8888)
        val p = img.planes
        val yb = p[0].buffer
        yb.position(crop.top * p[0].rowStride + crop.left)
        val ub = p[1].buffer
        ub.position((crop.top / 2) * p[1].rowStride + (crop.left / 2) * p[1].pixelStride)
        val vb = p[2].buffer
        vb.position((crop.top / 2) * p[2].rowStride + (crop.left / 2) * p[2].pixelStride)
        val ok = NativeBridge.nativeYuvToBitmap(yb.slice(), ub.slice(), vb.slice(), p[0].rowStride, p[1].rowStride, p[1].pixelStride, w, h, bt709, false, bmp)
        img.close()
        return if (ok) bmp else null
    }

    fun release() {
        try { codec.stop() } catch (_: Exception) {}
        codec.release()
        extractor.release()
    }
}
