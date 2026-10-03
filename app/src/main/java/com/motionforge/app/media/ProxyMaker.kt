package com.motionforge.app.media

import android.content.Context
import android.graphics.Bitmap
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import com.motionforge.app.export.EglRenderer
import org.json.JSONObject
import java.io.File
import java.nio.ByteBuffer

/**
 * Proxies: low-resolution H.264 copies of video clips, decoded by the preview instead of the original (the engine
 * substitutes `asset.proxy` when rendering outside export mode). Export always reads the original file.
 * Proxies are video-only; audio keeps coming from the original.
 */
object ProxyMaker {
    const val DEFAULT_HEIGHT = 540

    fun dir(ctx: Context) = File(ctx.filesDir, "proxies").apply { mkdirs() }

    /** Proxy size for a source: scaled to [maxH] lines (never upscaled), even dimensions as encoders require. */
    fun size(w: Int, h: Int, maxH: Int = DEFAULT_HEIGHT): Pair<Int, Int> {
        val s = minOf(1.0, maxH.toDouble() / maxOf(1, h))
        return Pair(maxOf(2, (w * s).toInt() and 1.inv()), maxOf(2, (h * s).toInt() and 1.inv()))
    }

    /**
     * Transcodes [asset] (an engine asset object) to a proxy file and returns the `proxy` field to store on the asset:
     * {"path", "width", "height", "createdAt"}. [progress] returns false to cancel. Throws on failure.
     */
    fun make(ctx: Context, asset: JSONObject, maxH: Int = DEFAULT_HEIGHT, progress: (Float) -> Boolean = { true }): JSONObject {
        val src = asset.optString("path").ifEmpty { asset.optString("uri") }
        require(src.isNotEmpty()) { "Asset has no source file." }
        val (pw, ph) = size(asset.optInt("width", 1920), asset.optInt("height", 1080), maxH)
        val fps = asset.optDouble("fps", 30.0).coerceIn(1.0, 30.0)
        val duration = asset.optDouble("duration", 0.0)
        require(duration > 0) { "Asset has no duration." }
        val out = File(dir(ctx), "${asset.optString("id")}-${MediaBridge.sha1(src).take(10)}-${ph}p.mp4")
        val tmp = File(out.path + ".part").apply { delete() }

        val fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, pw, ph).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, maxOf(500_000, pw * ph * 3))
            setInteger(MediaFormat.KEY_FRAME_RATE, Math.round(fps).toInt())
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)  // short GOPs: fast scrubbing is the point of a proxy
        }
        val enc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        enc.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        val surface = enc.createInputSurface()
        enc.start()
        val gl = EglRenderer(surface, pw, ph)
        val mux = MediaMuxer(tmp.path, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        val dec = VideoDecoder(src)
        var track = -1
        var started = false
        var ok = false
        val info = MediaCodec.BufferInfo()
        fun drain(eos: Boolean) {
            var waits = 0
            while (true) {
                val idx = enc.dequeueOutputBuffer(info, if (eos) 10_000 else 0)
                when {
                    idx == MediaCodec.INFO_TRY_AGAIN_LATER -> if (!eos || ++waits > 500) return  // 5 s without the EOS buffer
                    idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> { track = mux.addTrack(enc.outputFormat); mux.start(); started = true }
                    idx >= 0 -> {
                        val buf = enc.getOutputBuffer(idx)!!
                        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) info.size = 0
                        if (info.size > 0 && started) { buf.position(info.offset); buf.limit(info.offset + info.size); mux.writeSampleData(track, buf, info) }
                        enc.releaseOutputBuffer(idx, false)
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) return
                    }
                }
            }
        }
        try {
            val frames = maxOf(1, (duration * fps).toInt())
            val rgba = ByteBuffer.allocateDirect(pw * ph * 4)
            for (i in 0 until frames) {
                val t = i / fps
                val b = dec.frameAt((t * 1_000_000).toLong(), pw, ph) ?: continue
                val sized = if (b.width == pw && b.height == ph) b else Bitmap.createScaledBitmap(b, pw, ph, true)
                rgba.clear(); sized.copyPixelsToBuffer(rgba)
                gl.drawFrame(rgba, (t * 1e9).toLong())
                drain(false)
                if (!progress((i + 1).toFloat() / frames)) throw InterruptedException("Proxy creation cancelled.")
            }
            enc.signalEndOfInputStream()
            drain(true)
            ok = started
        } finally {
            dec.release()
            gl.release()
            try { enc.stop() } catch (_: Exception) {}
            enc.release()
            surface.release()
            try { if (started) mux.stop() } catch (_: Exception) { ok = false }
            mux.release()
            if (ok) { out.delete(); tmp.renameTo(out) } else tmp.delete()
        }
        if (!ok) throw IllegalStateException("The encoder produced no output.")
        return JSONObject().put("path", out.absolutePath).put("width", pw).put("height", ph).put("createdAt", System.currentTimeMillis())
    }
}
