package com.motionforge.app.playback

import android.graphics.Bitmap
import android.graphics.SurfaceTexture
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.media.MediaBridge
import kotlinx.coroutines.suspendCancellableCoroutine
import org.json.JSONObject
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume

/**
 * GPU preview: one GL thread owns an EGL context, the preview window surface (a TextureView's SurfaceTexture), the
 * native compositor (gpu_compositor.cpp) and hardware video decoders that render straight into external OES textures.
 * The UI thread only posts requests; it never waits for a frame.
 */
class GpuRenderer {
    private val thread = HandlerThread("mf-gpu").apply { start() }
    private val handler = Handler(thread.looper)
    private val frameCallbacks = HandlerThread("mf-gpu-frames").apply { start() }  // SurfaceTexture.onFrameAvailable
    private val frameHandler = Handler(frameCallbacks.looper)

    private var display: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var context: EGLContext = EGL14.EGL_NO_CONTEXT
    private var config: EGLConfig? = null
    private var pbuffer: EGLSurface = EGL14.EGL_NO_SURFACE
    private var window: EGLSurface = EGL14.EGL_NO_SURFACE
    private var windowW = 0
    private var windowH = 0
    private val videos = LinkedHashMap<String, GpuVideoSource>(8, 0.75f, true)

    /** True once EGL and the shaders are ready. False means the caller must use the CPU preview. */
    @Volatile var ready = false
        private set
    @Volatile var lastError = ""
        private set

    /** Where the frame goes inside the window (view px, y down) — set from the UI as zoom/pan change. */
    @Volatile var dst = floatArrayOf(0f, 0f, 0f, 0f)
    @Volatile var checker = false
    @Volatile var hasWindow = false
        private set

    init {
        val latch = CountDownLatch(1)
        handler.post {
            try { initGl() } catch (e: Throwable) { lastError = e.message ?: e.toString(); Log.w(TAG, "GPU init failed", e) }
            latch.countDown()
        }
        latch.await(10, TimeUnit.SECONDS)
    }

    private fun initGl() {
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        val v = IntArray(2)
        check(EGL14.eglInitialize(display, v, 0, v, 1)) { "eglInitialize failed" }
        val attribs = intArrayOf(EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT or EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE)
        val configs = arrayOfNulls<EGLConfig>(1)
        val n = IntArray(1)
        check(EGL14.eglChooseConfig(display, attribs, 0, configs, 0, 1, n, 0) && n[0] > 0) { "no EGL config" }
        config = configs[0]
        context = EGL14.eglCreateContext(display, config, EGL14.EGL_NO_CONTEXT, intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0)
        check(context != EGL14.EGL_NO_CONTEXT) { "eglCreateContext failed" }
        pbuffer = EGL14.eglCreatePbufferSurface(display, config, intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0)
        check(EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context)) { "eglMakeCurrent failed" }
        check(NativeBridge.nativeGpuInit()) { "shader setup failed" }
        ready = true
    }

    /** Binds the preview window (TextureView surface). */
    fun attach(surface: SurfaceTexture, w: Int, h: Int) {
        hasWindow = ready  // frames requested from now on queue behind the attach below
        handler.post { attachOnGlThread(surface, w, h) }
    }

    private fun attachOnGlThread(surface: SurfaceTexture, w: Int, h: Int) {
        if (!ready) return
        releaseWindow()
        window = EGL14.eglCreateWindowSurface(display, config, surface, intArrayOf(EGL14.EGL_NONE), 0)
        windowW = w; windowH = h
        hasWindow = window != EGL14.EGL_NO_SURFACE
    }

    fun resize(w: Int, h: Int) = handler.post { windowW = w; windowH = h }

    /** Must complete before the SurfaceTexture is destroyed. */
    fun detach() {
        val latch = CountDownLatch(1)
        handler.post { releaseWindow(); latch.countDown() }
        latch.await(2, TimeUnit.SECONDS)
    }

    private fun releaseWindow() {
        if (window != EGL14.EGL_NO_SURFACE) {
            EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context)
            EGL14.eglDestroySurface(display, window)
        }
        window = EGL14.EGL_NO_SURFACE
        hasWindow = false
    }

    /**
     * Renders time [t] with output width [outW] (comp aspect kept) and presents it in the window when one is bound.
     * Suspends the caller (render coroutine), never the UI thread. Returns stats, or null when the GPU is unavailable.
     */
    suspend fun render(t: Double, outW: Int, useProxies: Boolean, readback: Bitmap? = null): JSONObject? {
        if (!ready) return null
        return suspendCancellableCoroutine { cont ->
            handler.post { cont.resume(try { renderOnGlThread(t, outW, useProxies, readback) } catch (e: Throwable) { Log.w(TAG, "GPU frame failed", e); null }) }
        }
    }

    /** Blocking variant for tests and tools. */
    fun renderBlocking(t: Double, outW: Int, useProxies: Boolean, readback: Bitmap?): JSONObject? {
        if (!ready) return null
        var r: JSONObject? = null
        val latch = CountDownLatch(1)
        handler.post { r = try { renderOnGlThread(t, outW, useProxies, readback) } catch (e: Throwable) { Log.w(TAG, "GPU frame failed", e); null }; latch.countDown() }
        latch.await(60, TimeUnit.SECONDS)
        return r
    }

    private fun renderOnGlThread(t: Double, outW: Int, useProxies: Boolean, readback: Bitmap?): JSONObject {
        val present = window != EGL14.EGL_NO_SURFACE && readback == null
        if (present) EGL14.eglMakeCurrent(display, window, window, context) else EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context)
        val plan = JSONObject(NativeBridge.nativeGpuPlan(t, outW, false, useProxies))
        val items = plan.optJSONArray("items")
        val n = items?.length() ?: 0
        val tex = IntArray(n)
        val mtx = FloatArray(n * 16)
        var videoDecoded = 0
        for (i in 0 until n) {
            val it = items!!.getJSONObject(i)
            if (it.optString("kind") != "video") continue
            val asset = it.getJSONObject("asset")
            val src = sourceFor(asset, useProxies) ?: continue  // 0 → native decodes on the CPU
            val v = video(asset.optString("id") + "|" + src, src) ?: continue
            if (v.frameAt((it.optDouble("t") * 1_000_000).toLong())) {
                tex[i] = v.texture
                System.arraycopy(v.matrix, 0, mtx, i * 16, 16)
                videoDecoded++
            }
        }
        val d = dst
        val res = JSONObject(NativeBridge.nativeGpuComposite(tex, mtx, if (present) windowW else 0, if (present) windowH else 0,
            d[0], d[1], d[2], d[3], checker, CLEAR))
        if (present) EGL14.eglSwapBuffers(display, window)
        if (readback != null) res.put("readback", NativeBridge.nativeGpuReadback(readback))
        res.put("videoGpu", videoDecoded)
        res.put("plan", plan.apply { remove("items") })
        return res
    }

    private fun sourceFor(asset: JSONObject, useProxies: Boolean): String? {
        if (asset.has("generator")) return null
        val proxy = asset.optJSONObject("proxy")?.optString("path").orEmpty()
        if (useProxies && proxy.isNotEmpty() && File(proxy).exists()) return proxy
        return asset.optString("path").ifEmpty { asset.optString("uri") }.ifEmpty { null }
    }

    private fun video(key: String, src: String): GpuVideoSource? {
        videos[key]?.let { return it }
        val v = try { GpuVideoSource(src, frameHandler) } catch (e: Exception) { Log.w(TAG, "GPU decoder failed for $src: ${e.message}"); return null }
        videos[key] = v
        while (videos.size > 4) { val e = videos.entries.first(); e.value.release(); videos.remove(e.key) }
        return v
    }

    /** Drops decoders (relink, project close, memory pressure). */
    fun releaseVideos() = handler.post { videos.values.forEach { it.release() }; videos.clear() }

    fun release() {
        val latch = CountDownLatch(1)
        handler.post {
            videos.values.forEach { it.release() }; videos.clear()
            releaseWindow()
            if (ready) { EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context); NativeBridge.nativeGpuRelease() }
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            if (pbuffer != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, pbuffer)
            if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context)
            EGL14.eglTerminate(display)
            ready = false
            latch.countDown()
        }
        latch.await(3, TimeUnit.SECONDS)
        thread.quitSafely(); frameCallbacks.quitSafely()
    }

    companion object {
        private const val TAG = "GpuRenderer"
        private val CLEAR = floatArrayOf(0f, 0f, 0f, 1f)
    }
}

/**
 * Hardware decoder rendering into an external OES texture (no YUV→RGB conversion or copies on the CPU).
 * Created and used on the GL thread.
 */
class GpuVideoSource(src: String, frameHandler: Handler) {
    val texture: Int
    val matrix = FloatArray(16)
    private val surfaceTexture: SurfaceTexture
    private val surface: Surface
    private val extractor = MediaExtractor()
    private val codec: MediaCodec
    private val frameUs: Long
    private val lock = Object()
    private var available = false
    private var lastPts = Long.MIN_VALUE
    private var inputDone = false
    private var producedOutput = false

    init {
        val t = IntArray(1)
        GLES20.glGenTextures(1, t, 0)
        texture = t[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texture)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        surfaceTexture = SurfaceTexture(texture)
        surfaceTexture.setOnFrameAvailableListener({ synchronized(lock) { available = true; lock.notifyAll() } }, frameHandler)
        surface = Surface(surfaceTexture)
        try {
            MediaBridge.setDataSource(extractor, src)
            var track = -1
            for (i in 0 until extractor.trackCount) if (extractor.getTrackFormat(i).getString(MediaFormat.KEY_MIME)?.startsWith("video/") == true) { track = i; break }
            require(track >= 0) { "no video track" }
            extractor.selectTrack(track)
            val fmt = extractor.getTrackFormat(track)
            val fps = if (fmt.containsKey(MediaFormat.KEY_FRAME_RATE)) fmt.getInteger(MediaFormat.KEY_FRAME_RATE) else 30
            frameUs = 1_000_000L / maxOf(1, fps)
            codec = MediaCodec.createDecoderByType(fmt.getString(MediaFormat.KEY_MIME)!!)
            codec.configure(fmt, surface, null, 0)
            codec.start()
        } catch (e: Exception) {
            surface.release(); surfaceTexture.release(); GLES20.glDeleteTextures(1, intArrayOf(texture), 0); extractor.release()
            throw e
        }
    }

    /** Decodes the frame shown at [us] into the texture. False when nothing could be decoded. */
    fun frameAt(us: Long): Boolean {
        if (lastPts != Long.MIN_VALUE && us >= lastPts && us < lastPts + frameUs) return true
        if (lastPts == Long.MIN_VALUE || us < lastPts || us > lastPts + 1_500_000) {
            extractor.seekTo(us, MediaExtractor.SEEK_TO_PREVIOUS_SYNC)
            if (producedOutput) codec.flush()  // never flush before the first output: it drops the H.264 SPS/PPS
            inputDone = false
        }
        val info = MediaCodec.BufferInfo()
        var spins = 0
        while (spins++ < 3000) {
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
            if (oi >= 0 || oi == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) producedOutput = true
            if (oi < 0) continue
            val eos = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
            val show = info.presentationTimeUs + frameUs / 2 >= us || eos
            if (!show) { codec.releaseOutputBuffer(oi, false); continue }
            synchronized(lock) { available = false }
            codec.releaseOutputBuffer(oi, info.size > 0)
            if (info.size > 0) {
                synchronized(lock) { val end = System.currentTimeMillis() + 1000; while (!available && System.currentTimeMillis() < end) lock.wait(50) }
                surfaceTexture.updateTexImage()
                surfaceTexture.getTransformMatrix(matrix)
                lastPts = info.presentationTimeUs
                return true
            }
            return lastPts != Long.MIN_VALUE
        }
        return lastPts != Long.MIN_VALUE
    }

    fun release() {
        try { codec.stop() } catch (_: Exception) {}
        codec.release(); extractor.release(); surface.release(); surfaceTexture.release()
        GLES20.glDeleteTextures(1, intArrayOf(texture), 0)
    }
}
