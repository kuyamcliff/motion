package com.motionforge.app.export

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMetadataRetriever
import android.media.MediaMuxer
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES20
import android.os.Build
import android.view.Surface
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.ProgressCallback
import com.motionforge.app.engine.jo
import org.json.JSONObject
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

data class ExportSettings(
    val format: String = "mp4",       // mp4 | webm | gif | png | wav | m4a
    val videoMime: String = MediaFormat.MIMETYPE_VIDEO_AVC,
    val width: Int = 1920,
    val height: Int = 1080,
    val fps: Double = 30.0,
    val bitrate: Int = 12_000_000,
    val bitrateMode: String = "vbr",  // vbr | cbr
    val audio: Boolean = true,
    val audioBitrate: Int = 192_000,
    val sampleRate: Int = 48000,
    val t0: Double = 0.0,
    val t1: Double = -1.0,            // -1 = composition end
    val transparent: Boolean = false,
) {
    fun toJson() = jo("format" to format, "videoMime" to videoMime, "width" to width, "height" to height, "fps" to fps, "bitrate" to bitrate,
        "bitrateMode" to bitrateMode, "audio" to audio, "audioBitrate" to audioBitrate, "sampleRate" to sampleRate, "t0" to t0, "t1" to t1, "transparent" to transparent)

    companion object {
        fun fromJson(j: JSONObject) = ExportSettings(
            j.optString("format", "mp4"), j.optString("videoMime", MediaFormat.MIMETYPE_VIDEO_AVC), j.optInt("width", 1920), j.optInt("height", 1080),
            j.optDouble("fps", 30.0), j.optInt("bitrate", 12_000_000), j.optString("bitrateMode", "vbr"), j.optBoolean("audio", true),
            j.optInt("audioBitrate", 192_000), j.optInt("sampleRate", 48000), j.optDouble("t0", 0.0), j.optDouble("t1", -1.0), j.optBoolean("transparent", false))
    }
}

/** Device codec capabilities used to offer only formats that actually work. */
object CodecCaps {
    data class Enc(val mime: String, val name: String, val hw: Boolean, val maxW: Int, val maxH: Int)

    val encoders: List<Enc> by lazy {
        val out = ArrayList<Enc>()
        for (info in MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos) {
            if (!info.isEncoder) continue
            for (type in info.supportedTypes) {
                if (!type.startsWith("video/") && !type.startsWith("audio/")) continue
                val caps = info.getCapabilitiesForType(type)
                var mw = 0
                var mh = 0
                caps.videoCapabilities?.let { mw = it.supportedWidths.upper; mh = it.supportedHeights.upper }
                val hw = if (Build.VERSION.SDK_INT >= 29) info.isHardwareAccelerated else !info.name.startsWith("OMX.google") && !info.name.startsWith("c2.android")
                out.add(Enc(type, info.name, hw, mw, mh))
            }
        }
        out
    }

    fun has(mime: String) = encoders.any { it.mime.equals(mime, true) }
    fun supportsSize(mime: String, w: Int, h: Int): Boolean {
        for (info in MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos) {
            if (!info.isEncoder || info.supportedTypes.none { it.equals(mime, true) }) continue
            val vc = info.getCapabilitiesForType(mime).videoCapabilities ?: continue
            if (vc.isSizeSupported(w, h)) return true
        }
        return false
    }

    fun videoOptions(): List<Pair<String, String>> = listOf(
        MediaFormat.MIMETYPE_VIDEO_AVC to "H.264 / AVC",
        MediaFormat.MIMETYPE_VIDEO_HEVC to "H.265 / HEVC",
        MediaFormat.MIMETYPE_VIDEO_VP9 to "VP9 (WebM)",
        "video/av01" to "AV1",
    ).filter { has(it.first) }

    fun json(): JSONObject = jo("encoders" to encoders.map { jo("mime" to it.mime, "name" to it.name, "hw" to it.hw, "maxW" to it.maxW, "maxH" to it.maxH) })
}

class ExportException(msg: String, val suggestions: List<String> = emptyList()) : Exception(msg)

/** Renders the composition frame by frame and encodes it. All output is validated before success is reported. */
class ExportEngine(private val duration: Double, private val compW: Int, private val compH: Int) {
    @Volatile var cancelled = false

    fun run(s: ExportSettings, out: File, progress: (Float) -> Unit): JSONObject {
        val t1 = if (s.t1 > s.t0) s.t1 else duration
        NativeBridge.call("checkpoint", jo("reason" to "before export"))
        NativeBridge.call("exportBegin")
        try { return runFrozen(s, out, progress, t1) } finally { NativeBridge.call("exportEnd") }
    }

    private fun runFrozen(s: ExportSettings, out: File, progress: (Float) -> Unit, t1: Double): JSONObject {
        return when (s.format) {
            "gif" -> nativeTask("exportGif", jo("path" to out.absolutePath, "fps" to s.fps, "width" to s.width, "t0" to s.t0, "t1" to t1), progress)
            "png" -> nativeTask("exportPngSequence", jo("path" to out.absolutePath, "fps" to s.fps, "width" to s.width, "t0" to s.t0, "t1" to t1, "transparent" to s.transparent), progress)
            "wav" -> nativeTask("exportWav", jo("path" to out.absolutePath, "t0" to s.t0, "t1" to t1, "sampleRate" to s.sampleRate), progress)
            "m4a" -> encode(s.copy(t1 = t1), out, progress, videoOn = false)
            else -> encode(s.copy(t1 = t1), out, progress, videoOn = true)
        }
    }

    private fun nativeTask(m: String, a: JSONObject, progress: (Float) -> Unit): JSONObject {
        val r = NativeBridge.task(m, a, ProgressCallback { p -> progress(p); !cancelled })
        if (r.optBoolean("cancelled")) throw ExportException("Export cancelled.")
        if (!r.optBoolean("ok")) throw ExportException(r.optString("error", "Export failed."))
        return r
    }

    private fun encode(s: ExportSettings, out: File, progress: (Float) -> Unit, videoOn: Boolean): JSONObject {
        val webm = s.format == "webm"
        if (videoOn) {
            if (!CodecCaps.has(s.videoMime)) throw ExportException("The selected ${s.videoMime.removePrefix("video/")} encoder is unavailable on this device.", listOf("H.264", "Lower resolution"))
            if (!CodecCaps.supportsSize(s.videoMime, s.width, s.height))
                throw ExportException("The encoder does not support ${s.width}x${s.height} on this device.", listOf("Lower resolution", "H.264"))
            if (webm && s.videoMime != MediaFormat.MIMETYPE_VIDEO_VP9 && s.videoMime != MediaFormat.MIMETYPE_VIDEO_VP8)
                throw ExportException("WebM requires VP9 (or VP8) video.", listOf("Choose VP9", "Use MP4"))
        }
        val audioMime = if (webm) MediaFormat.MIMETYPE_AUDIO_OPUS else MediaFormat.MIMETYPE_AUDIO_AAC
        val audioOn = s.audio && CodecCaps.has(audioMime)
        if (!videoOn && !audioOn) throw ExportException("No audio encoder is available for this format.")
        val tmp = File(out.parentFile, out.name + ".partial")
        tmp.delete()
        val muxer = MediaMuxer(tmp.absolutePath, if (webm) MediaMuxer.OutputFormat.MUXER_OUTPUT_WEBM else MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        var videoTrack = -1
        var audioTrack = -1
        var muxerStarted = false
        val pendingFormats = HashMap<String, MediaFormat>()
        var venc: MediaCodec? = null
        var aenc: MediaCodec? = null
        var gl: EglRenderer? = null
        val frames = Math.max(1, Math.floor((s.t1 - s.t0) * s.fps + 1e-6).toInt())
        val info = MediaCodec.BufferInfo()
        val wantTracks = (if (videoOn) 1 else 0) + (if (audioOn) 1 else 0)
        fun tryStartMuxer() {
            if (muxerStarted || pendingFormats.size < wantTracks) return
            pendingFormats["video"]?.let { videoTrack = muxer.addTrack(it) }
            pendingFormats["audio"]?.let { audioTrack = muxer.addTrack(it) }
            muxer.start()
            muxerStarted = true
        }
        val pendingSamples = ArrayList<Triple<String, ByteBuffer, MediaCodec.BufferInfo>>()
        fun drain(codec: MediaCodec, kind: String, endOfStream: Boolean) {
            while (true) {
                val idx = codec.dequeueOutputBuffer(info, if (endOfStream) 10_000 else 0)
                when {
                    idx == MediaCodec.INFO_TRY_AGAIN_LATER -> { if (!endOfStream) return }
                    idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> { pendingFormats[kind] = codec.outputFormat; tryStartMuxer() }
                    idx >= 0 -> {
                        val buf = codec.getOutputBuffer(idx)!!
                        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) info.size = 0
                        if (info.size > 0) {
                            buf.position(info.offset); buf.limit(info.offset + info.size)
                            if (muxerStarted) {
                                // Flush samples queued before the muxer could start.
                                for ((k, b, i) in pendingSamples) muxer.writeSampleData(if (k == "video") videoTrack else audioTrack, b, i)
                                pendingSamples.clear()
                                muxer.writeSampleData(if (kind == "video") videoTrack else audioTrack, buf, info)
                            } else {
                                val copy = ByteBuffer.allocateDirect(info.size); copy.put(buf); copy.flip()
                                pendingSamples.add(Triple(kind, copy, MediaCodec.BufferInfo().apply { set(0, info.size, info.presentationTimeUs, info.flags) }))
                            }
                        }
                        val eos = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                        codec.releaseOutputBuffer(idx, false)
                        if (eos) return
                    }
                }
            }
        }
        try {
            if (videoOn) {
                val fmt = MediaFormat.createVideoFormat(s.videoMime, s.width, s.height).apply {
                    setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                    setInteger(MediaFormat.KEY_BIT_RATE, s.bitrate)
                    setFloat(MediaFormat.KEY_FRAME_RATE, s.fps.toFloat())
                    setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
                    setInteger(MediaFormat.KEY_BITRATE_MODE, if (s.bitrateMode == "cbr") MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR else MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
                }
                venc = MediaCodec.createEncoderByType(s.videoMime)
                try {
                    venc.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                } catch (e: Exception) {
                    fmt.removeKey(MediaFormat.KEY_BITRATE_MODE)
                    venc.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                }
                val surface = venc.createInputSurface()
                venc.start()
                gl = EglRenderer(surface, s.width, s.height)
            }
            if (audioOn) {
                val af = MediaFormat.createAudioFormat(audioMime, s.sampleRate, 2).apply {
                    setInteger(MediaFormat.KEY_BIT_RATE, s.audioBitrate)
                    if (!webm) setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
                    setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16384)
                }
                aenc = MediaCodec.createEncoderByType(audioMime)
                aenc.configure(af, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                aenc.start()
                NativeBridge.nativeResetAudio()
            }
            val rgba = ByteBuffer.allocateDirect(s.width * s.height * 4).order(ByteOrder.nativeOrder())
            val audioFramesTotal = ((s.t1 - s.t0) * s.sampleRate).toLong()
            var audioFramesDone = 0L
            var audioEos = !audioOn
            val chunk = 1024
            val abuf = FloatArray(chunk * 2)
            val units = frames + 1
            for (i in 0..frames) {
                if (cancelled) throw ExportException("Export cancelled.")
                if (videoOn && i < frames) {
                    val t = s.t0 + i / s.fps
                    if (!NativeBridge.nativeRenderRgba(rgba, s.width, s.height, t)) throw ExportException("Rendering failed at ${"%.2f".format(t)} s.")
                    gl!!.drawFrame(rgba, ((i / s.fps) * 1e9).toLong())
                    drain(venc!!, "video", false)
                }
                // Feed audio up to the current video time.
                val audioTarget = if (i < frames) ((i + 1) / s.fps * s.sampleRate).toLong().coerceAtMost(audioFramesTotal) else audioFramesTotal
                while (aenc != null && !audioEos && audioFramesDone < audioTarget) {
                    val ii = aenc.dequeueInputBuffer(10_000)
                    if (ii < 0) { drain(aenc, "audio", false); continue }
                    val n = minOf(chunk.toLong(), audioFramesTotal - audioFramesDone).toInt()
                    val pts = audioFramesDone * 1_000_000L / s.sampleRate
                    if (n <= 0) {
                        aenc.queueInputBuffer(ii, 0, 0, pts, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                        audioEos = true
                        break
                    }
                    NativeBridge.nativeMixAudio(s.t0 + audioFramesDone.toDouble() / s.sampleRate, n, abuf, s.sampleRate)
                    val inBuf = aenc.getInputBuffer(ii)!!.order(ByteOrder.LITTLE_ENDIAN)
                    inBuf.clear()
                    for (k in 0 until n * 2) inBuf.putShort((abuf[k].coerceIn(-1f, 1f) * 32767f).toInt().toShort())
                    aenc.queueInputBuffer(ii, 0, n * 4, pts, 0)
                    audioFramesDone += n
                    drain(aenc, "audio", false)
                }
                progress(i.toFloat() / units)
            }
            if (aenc != null && !audioEos) {
                var ii: Int
                do { ii = aenc.dequeueInputBuffer(10_000); if (ii < 0) drain(aenc, "audio", false) } while (ii < 0)
                aenc.queueInputBuffer(ii, 0, 0, audioFramesDone * 1_000_000L / s.sampleRate, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
            }
            if (venc != null) { venc.signalEndOfInputStream(); drain(venc, "video", true) }
            if (aenc != null) drain(aenc, "audio", true)
            if (!muxerStarted) throw ExportException("The encoder produced no output.")
        } catch (e: ExportException) {
            cleanup(venc, aenc, gl, muxer, muxerStarted); tmp.delete(); throw e
        } catch (e: Exception) {
            cleanup(venc, aenc, gl, muxer, muxerStarted); tmp.delete()
            throw ExportException("Export failed: ${e.message}", listOf("H.264", "Lower resolution"))
        }
        cleanup(venc, aenc, gl, muxer, muxerStarted)
        val validation = validate(tmp, s, videoOn, audioOn, frames)
        if (!validation.optBoolean("ok")) {
            tmp.delete()
            throw ExportException("Export validation failed: " + validation.optString("error"))
        }
        out.delete()
        if (!tmp.renameTo(out)) throw ExportException("Could not move the finished file into place.")
        progress(1f)
        return jo("ok" to true, "validation" to validation, "audioIncluded" to audioOn, "warnings" to if (s.audio && !audioOn) listOf("No $audioMime encoder on this device; exported without audio.") else emptyList<String>())
    }

    private fun cleanup(v: MediaCodec?, a: MediaCodec?, gl: EglRenderer?, mux: MediaMuxer, started: Boolean) {
        try { v?.stop() } catch (_: Exception) {}
        v?.release()
        try { a?.stop() } catch (_: Exception) {}
        a?.release()
        gl?.release()
        try { if (started) mux.stop() } catch (_: Exception) {}
        mux.release()
    }

    /** Validates container, tracks, resolution, duration and frame count; decodes a sample frame. */
    fun validate(file: File, s: ExportSettings, video: Boolean, audio: Boolean, frames: Int): JSONObject {
        if (!file.exists() || file.length() == 0L) return jo("ok" to false, "error" to "output file missing")
        val ex = MediaExtractor()
        try {
            ex.setDataSource(file.absolutePath)
            var vt = -1
            var at = -1
            for (i in 0 until ex.trackCount) {
                val m = ex.getTrackFormat(i).getString(MediaFormat.KEY_MIME) ?: ""
                if (m.startsWith("video/")) vt = i
                if (m.startsWith("audio/")) at = i
            }
            if (video && vt < 0) return jo("ok" to false, "error" to "no video track")
            if (audio && at < 0) return jo("ok" to false, "error" to "no audio track")
            var count = 0
            var lastPts = 0L
            if (vt >= 0) {
                val f = ex.getTrackFormat(vt)
                if (f.getInteger(MediaFormat.KEY_WIDTH) != s.width || f.getInteger(MediaFormat.KEY_HEIGHT) != s.height)
                    return jo("ok" to false, "error" to "resolution mismatch")
                ex.selectTrack(vt)
                while (ex.sampleTime >= 0) { count++; lastPts = maxOf(lastPts, ex.sampleTime); if (!ex.advance()) break }
                if (count != frames) return jo("ok" to false, "error" to "frame count $count, expected $frames")
            }
            val expected = s.t1 - s.t0
            val mmr = MediaMetadataRetriever()
            mmr.setDataSource(file.absolutePath)
            val durMs = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toLongOrNull() ?: 0
            var sampleOk = true
            if (video) {
                val bmp = mmr.getFrameAtTime(((expected / 2) * 1e6).toLong(), MediaMetadataRetriever.OPTION_CLOSEST_SYNC)
                sampleOk = bmp != null
            }
            mmr.release()
            if (Math.abs(durMs / 1000.0 - expected) > maxOf(0.15, 2.0 / s.fps)) return jo("ok" to false, "error" to "duration ${durMs / 1000.0}s, expected $expected s")
            if (!sampleOk) return jo("ok" to false, "error" to "sample frame could not be decoded")
            return jo("ok" to true, "frames" to count, "durationMs" to durMs, "videoTrack" to (vt >= 0), "audioTrack" to (at >= 0), "bytes" to file.length())
        } catch (e: Exception) {
            return jo("ok" to false, "error" to (e.message ?: "unreadable container"))
        } finally {
            ex.release()
        }
    }
}

/** Minimal EGL/GLES2 pipeline that uploads RGBA frames and draws them into the encoder's input surface. */
class EglRenderer(surface: Surface, private val w: Int, private val h: Int) {
    private val display: EGLDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
    private val context: EGLContext
    private val eglSurface: EGLSurface
    private val program: Int
    private val tex: Int
    private val quad: FloatBuffer

    init {
        val ver = IntArray(2)
        EGL14.eglInitialize(display, ver, 0, ver, 1)
        val attribs = intArrayOf(EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            0x3142 /* EGL_RECORDABLE_ANDROID */, 1, EGL14.EGL_NONE)
        val configs = arrayOfNulls<EGLConfig>(1)
        val num = IntArray(1)
        EGL14.eglChooseConfig(display, attribs, 0, configs, 0, 1, num, 0)
        context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT, intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0)
        eglSurface = EGL14.eglCreateWindowSurface(display, configs[0], surface, intArrayOf(EGL14.EGL_NONE), 0)
        EGL14.eglMakeCurrent(display, eglSurface, eglSurface, context)
        val vs = "attribute vec2 p; varying vec2 uv; void main(){ uv = vec2((p.x+1.0)*0.5, (1.0-p.y)*0.5); gl_Position = vec4(p,0.0,1.0); }"
        val fs = "precision mediump float; varying vec2 uv; uniform sampler2D t; void main(){ vec4 c = texture2D(t, uv); gl_FragColor = vec4(c.rgb + (1.0 - c.a) * 0.0, 1.0); }"
        program = GLES20.glCreateProgram().also { p ->
            GLES20.glAttachShader(p, shader(GLES20.GL_VERTEX_SHADER, vs))
            GLES20.glAttachShader(p, shader(GLES20.GL_FRAGMENT_SHADER, fs))
            GLES20.glLinkProgram(p)
        }
        val t = IntArray(1)
        GLES20.glGenTextures(1, t, 0)
        tex = t[0]
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tex)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_NEAREST)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_NEAREST)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, w, h, 0, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null)
        quad = ByteBuffer.allocateDirect(8 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply { put(floatArrayOf(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f)); position(0) }
    }

    private fun shader(type: Int, src: String): Int = GLES20.glCreateShader(type).also { GLES20.glShaderSource(it, src); GLES20.glCompileShader(it) }

    fun drawFrame(rgba: ByteBuffer, ptsNs: Long) {
        rgba.position(0)
        GLES20.glViewport(0, 0, w, h)
        GLES20.glUseProgram(program)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, tex)
        GLES20.glTexSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, w, h, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, rgba)
        val loc = GLES20.glGetAttribLocation(program, "p")
        GLES20.glEnableVertexAttribArray(loc)
        GLES20.glVertexAttribPointer(loc, 2, GLES20.GL_FLOAT, false, 0, quad)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        EGLExt.eglPresentationTimeANDROID(display, eglSurface, ptsNs)
        EGL14.eglSwapBuffers(display, eglSurface)
    }

    fun release() {
        EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
        EGL14.eglDestroySurface(display, eglSurface)
        EGL14.eglDestroyContext(display, context)
        EGL14.eglTerminate(display)
    }
}
