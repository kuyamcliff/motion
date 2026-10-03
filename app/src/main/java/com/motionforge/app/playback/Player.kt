package com.motionforge.app.playback

import android.graphics.Bitmap
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.motionforge.app.engine.EditorState
import com.motionforge.app.engine.NativeBridge
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.util.concurrent.Executors

/**
 * Preview renderer + transport. Frames render on a dedicated thread at an adaptive preview scale;
 * during playback the audio clock (AudioTrack position) drives the video time so A/V stay in sync.
 */
class Player(private val state: EditorState) {
    private val renderThread = Executors.newSingleThreadExecutor { r -> Thread(r, "mf-render").apply { priority = Thread.NORM_PRIORITY } }  // never above the UI thread
    private val renderDispatcher = renderThread.asCoroutineDispatcher()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var playJob: Job? = null
    private var audioJob: Job? = null
    private var audioTrack: AudioTrack? = null

    var frame by mutableStateOf<Bitmap?>(null)
        private set

    /** GPU preview (null when disabled in Settings or when EGL/shaders are unavailable: the CPU bitmap path is used). */
    var gpu: GpuRenderer? = null
        private set
    private var gpuTried = false
    /** True while the preview TextureView is bound to the GPU renderer. */
    var gpuWindow by mutableStateOf(false)
    var gpuFrames by mutableIntStateOf(0)
        private set

    fun gpuRenderer(): GpuRenderer? {
        if (!com.motionforge.app.Settings.gpuPreview) return null
        if (!gpuTried) {
            gpuTried = true
            gpu = try { GpuRenderer().takeIf { it.ready } } catch (e: Throwable) { null }
        }
        return gpu
    }
    var playing by mutableStateOf(false)
        private set
    var loop by mutableStateOf(true)
    var rate by mutableDoubleStateOf(1.0)  // J/K/L shuttle (negative = reverse)
    var renderMs by mutableFloatStateOf(0f)
    var fpsActual by mutableFloatStateOf(0f)
    var droppedFrames by mutableIntStateOf(0)
    var quality by mutableStateOf("Full")  // Full / Half / Quarter (adaptive)
    var lastStats by mutableStateOf(JSONObject())
    var rangeIn by mutableDoubleStateOf(-1.0)
    var rangeOut by mutableDoubleStateOf(-1.0)
    var meterPeak by mutableFloatStateOf(0f)

    private var previewWidth = 960
    private var adaptiveDivisor = 1
    private val bitmaps = arrayOfNulls<Bitmap>(2)
    private var bufIndex = 0
    private var pendingRender = false
    private var renderAgain = false
    var mode = "auto" // auto | full | half | quarter

    fun setViewportWidth(px: Int) {
        val w = px.coerceIn(160, 1920)
        if (w != previewWidth) {
            previewWidth = w
            requestRender()
        }
    }

    private fun targetSize(): Pair<Int, Int> {
        val div = when (mode) { "full" -> 1; "half" -> 2; "quarter" -> 4; else -> adaptiveDivisor }
        val cw = state.compWidth
        val ch = state.compHeight
        val w = (minOf(previewWidth, cw) / div).coerceAtLeast(64)
        val h = (w.toDouble() * ch / cw).toInt().coerceAtLeast(36)
        return w to h
    }

    /** Re-render the current playhead (coalesces bursts of requests). */
    fun requestRender() {
        if (pendingRender) { renderAgain = true; return }
        pendingRender = true
        val t = state.playhead
        scope.launch {
            renderFrame(t, draft = false)
            pendingRender = false
            if (renderAgain) { renderAgain = false; requestRender() }
        }
    }

    /** Renders on the GPU when its window is bound, otherwise into the CPU bitmap shown by Compose. */
    private suspend fun renderFrame(t: Double, draft: Boolean) {
        val g = gpu
        if (g != null && g.ready && g.hasWindow && gpuWindow) {
            val (w, _) = targetSize()
            val start = System.nanoTime()
            val r = g.render(t, w, com.motionforge.app.Settings.useProxies)
            if (r != null && r.optBoolean("ok")) {
                renderMs = (System.nanoTime() - start) / 1e6f
                lastStats = r
                quality = when { w >= minOf(previewWidth, state.compWidth) -> "Full"; w * 2 >= minOf(previewWidth, state.compWidth) -> "Half"; else -> "Quarter" }
                gpuFrames++
                return
            }
        }
        frame = renderAt(t, draft)
    }

    private suspend fun renderAt(t: Double, draft: Boolean): Bitmap? = withContext(renderDispatcher) {
        if (state.projectId.isEmpty()) return@withContext null
        val (w, h) = targetSize()
        bufIndex = 1 - bufIndex
        var b = bitmaps[bufIndex]
        if (b == null || b.width != w || b.height != h) {
            b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
            bitmaps[bufIndex] = b
        }
        val start = System.nanoTime()
        val stats = JSONObject(NativeBridge.nativeRenderBitmap(b!!, t, draft, false))
        renderMs = (System.nanoTime() - start) / 1e6f
        lastStats = stats
        quality = when { w >= minOf(previewWidth, state.compWidth) -> "Full"; w * 2 >= minOf(previewWidth, state.compWidth) -> "Half"; else -> "Quarter" }
        b
    }

    fun toggle() = if (playing) pause() else play()

    fun play() {
        if (playing || state.projectId.isEmpty()) return
        val dur = state.duration
        if (state.playhead >= (if (rangeOut > 0) rangeOut else dur) - 1e-6) state.playhead = if (rangeIn >= 0) rangeIn else 0.0
        playing = true
        val startT = state.playhead
        val startNs = System.nanoTime()
        startAudio(startT)
        playJob = scope.launch {
            var frames = 0
            var fpsWindowStart = System.nanoTime()
            val frameDur = 1.0 / state.fps
            var lastShown = -1
            while (isActive && playing) {
                val clock = audioClock() ?: ((System.nanoTime() - startNs) / 1e9 * rate)
                var t = startT + clock
                val end = if (rangeOut > 0) rangeOut else state.duration
                val begin = if (rangeIn >= 0) rangeIn else 0.0
                if (t >= end || t < 0) {
                    if (loop && rate > 0) {
                        stopAudio(); playJob = null; playing = false
                        state.playhead = begin
                        play()
                        return@launch
                    }
                    state.playhead = t.coerceIn(0.0, end)
                    pause()
                    break
                }
                val fi = state.frame(t)
                if (fi == lastShown) {
                    // Sleep until the next frame boundary instead of spinning the main thread.
                    val waitMs = (((fi + 1) * frameDur - t) / maxOf(1e-3, Math.abs(rate)) * 1000).toLong()
                    delay(waitMs.coerceIn(1, 50))
                    continue
                }
                if (lastShown >= 0 && fi > lastShown + 1) droppedFrames += fi - lastShown - 1
                lastShown = fi
                t = fi * frameDur
                state.playhead = t
                renderFrame(t, draft = true)
                frames++
                // Adaptive preview quality: lower resolution if we miss real time, recover when there is headroom.
                if (mode == "auto") {
                    if (renderMs > frameDur * 1000 * 1.1 && adaptiveDivisor < 4) adaptiveDivisor *= 2
                    else if (renderMs < frameDur * 1000 * 0.35 && adaptiveDivisor > 1) adaptiveDivisor /= 2
                }
                val now = System.nanoTime()
                if (now - fpsWindowStart > 1_000_000_000L) {
                    fpsActual = frames * 1e9f / (now - fpsWindowStart)
                    frames = 0
                    fpsWindowStart = now
                }
            }
        }
    }

    fun pause() {
        playing = false
        playJob?.cancel()
        playJob = null
        stopAudio()
        adaptiveDivisor = 1
        state.playhead = state.snap(state.playhead)
        requestRender()
    }

    /** Hold-to-play (long press): plays while held. */
    fun holdStart() = play()
    fun holdEnd() = pause()

    fun stop() {
        pause()
        state.playhead = if (rangeIn >= 0) rangeIn else 0.0
        requestRender()
    }

    fun seek(t: Double) {
        val wasPlaying = playing
        if (wasPlaying) pause()
        state.playhead = state.snap(t.coerceIn(0.0, state.duration))
        requestRender()
        if (wasPlaying) play()
    }

    fun stepFrames(n: Int) {
        if (playing) pause()
        val f = state.frame(state.playhead) + n
        state.playhead = (f / state.fps).coerceIn(0.0, state.duration)
        requestRender()
    }

    // ------------------------------------------------------------ audio
    private var audioStartFrames = 0L
    private val sampleRate = 48000

    private fun startAudio(t0: Double) {
        if (rate != 1.0) return  // shuttle speeds play silently
        NativeBridge.nativeResetAudio()
        val minBuf = AudioTrack.getMinBufferSize(sampleRate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_FLOAT)
        val track = try {
            AudioTrack.Builder()
                .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MOVIE).build())
                .setAudioFormat(AudioFormat.Builder().setSampleRate(sampleRate).setEncoding(AudioFormat.ENCODING_PCM_FLOAT).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
                .setBufferSizeInBytes(maxOf(minBuf, sampleRate / 5 * 8))
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
        } catch (e: Exception) {
            null
        } ?: return
        audioTrack = track
        audioStartFrames = 0
        track.play()
        // The writer coroutine owns the track: it is released only after the last write returns,
        // so stopping playback can never free the native AudioTrack under a blocked write().
        audioJob = scope.launch(Dispatchers.IO) {
            val chunk = 2048
            val buf = FloatArray(chunk * 2)
            var t = t0
            try {
                while (isActive && playing && audioTrack === track) {
                    val peak = NativeBridge.nativeMixAudio(t, chunk, buf, sampleRate)
                    meterPeak = peak
                    val n = try { track.write(buf, 0, buf.size, AudioTrack.WRITE_BLOCKING) } catch (e: IllegalStateException) { -1 }
                    if (n < 0) break
                    t += chunk.toDouble() / sampleRate
                }
            } finally {
                try { track.stop() } catch (_: Exception) {}
                track.release()
            }
        }
    }

    private fun audioClock(): Double? {
        val tr = audioTrack ?: return null
        val pos = tr.playbackHeadPosition.toLong() and 0xffffffffL
        // Until the device reports a moving playback head, follow the wall clock.
        if (pos <= 0) return null
        return pos.toDouble() / sampleRate
    }

    private fun stopAudio() {
        val job = audioJob
        val track = audioTrack
        audioJob = null
        audioTrack = null
        // Pause + flush unblocks a pending write(); the writer coroutine then stops and releases the track.
        try { track?.pause(); track?.flush() } catch (_: Exception) {}
        if (job != null) job.cancel() else track?.release()
        meterPeak = 0f
    }

    fun release() {
        pause()
        gpu?.release()
        gpu = null
        scope.launch(renderDispatcher) { }
        renderThread.shutdown()
    }
}
