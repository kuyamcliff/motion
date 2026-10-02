package com.motionforge.app

import android.graphics.Bitmap
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.motionforge.app.engine.EditorState
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.ProgressCallback
import com.motionforge.app.engine.arr
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.obj
import com.motionforge.app.engine.objects
import com.motionforge.app.export.ExportEngine
import com.motionforge.app.export.ExportSettings
import com.motionforge.app.media.Importer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * End-to-end tests of the real engine on device: JNI, command layer, undo, rendering, persistence,
 * MediaCodec audio decode + offline Whisper captions, and MediaCodec export with validation.
 */
@RunWith(AndroidJUnit4::class)
class EngineE2ETest {
    @get:org.junit.Rule val artifacts = FailureArtifacts()
    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var st: EditorState
    private var projectId = ""

    @Before
    fun setUp() {
        st = EditorState()
        val r = NativeBridge.call("createProject", jo("name" to "E2E ${System.nanoTime()}", "width" to 640, "height" to 360, "fps" to 30.0, "duration" to 3.0))
        assertTrue(r.toString(), r.optBoolean("ok"))
        projectId = r.optString("id")
        assertEquals(null, st.open(projectId))
    }

    @After
    fun tearDown() {
        if (st.projectId.isNotEmpty()) st.close()
        NativeBridge.call("deleteProject", jo("id" to projectId))
    }

    private fun countOpaque(b: Bitmap): Int {
        var n = 0
        for (y in 0 until b.height step 2) for (x in 0 until b.width step 2) if ((b.getPixel(x, y) ushr 24) > 0 && (b.getPixel(x, y) and 0xFFFFFF) != 0) n++
        return n
    }

    @Test
    fun editUndoRedoRenderAndPersist() {
        val id = st.addLayer("text", jo("text" to "Hello E2E", "size" to 80.0))
        assertNotNull(id)
        assertEquals(1, st.layers.size)
        assertTrue(st.canUndo)

        // Move the layer (one undo step) and verify through evaluation.
        st.setProp(id!!, "transform.position", listOf(100.0, 120.0, 0.0), "static")
        assertEquals(100.0, st.evalProps(id, listOf("transform.position")).getJSONArray("transform.position").getDouble(0), 1e-6)
        st.undo()
        assertEquals(320.0, st.evalProps(id, listOf("transform.position")).getJSONArray("transform.position").getDouble(0), 1e-6)
        st.redo()
        assertEquals(100.0, st.evalProps(id, listOf("transform.position")).getJSONArray("transform.position").getDouble(0), 1e-6)

        // Keyframes animate the value between times.
        st.playhead = 0.0
        st.setProp(id, "transform.opacity", 0.0, "key")
        st.playhead = 1.0
        st.setProp(id, "transform.opacity", 100.0, "key")
        st.playhead = 0.5
        val mid = st.evalProps(id, listOf("transform.opacity")).getDouble("transform.opacity")
        assertTrue("opacity mid=$mid", mid > 1 && mid < 99)

        // Split creates a second clip; undo restores one.
        st.playhead = 1.5
        assertNotNull(st.op("split", "layers" to listOf(id), "t" to 1.5))
        assertEquals(2, st.layers.size)
        st.undo()
        assertEquals(1, st.layers.size)

        // Render: the text must produce visible pixels.
        val bmp = Bitmap.createBitmap(320, 180, Bitmap.Config.ARGB_8888)
        val stats = NativeBridge.nativeRenderBitmap(bmp, 1.2, false, true)
        assertTrue(stats, countOpaque(bmp) > 20)

        // Save, close, reopen: the edit persists.
        assertTrue(st.save())
        st.close()
        assertEquals(null, st.open(projectId))
        assertEquals(1, st.layers.size)
        assertEquals("Hello E2E", st.layers[0].obj("text").obj("content").optString("v"))
        assertEquals(100.0, st.evalProps(id, listOf("transform.position")).getJSONArray("transform.position").getDouble(0), 1e-6)
    }

    @Test
    fun crashRecoveryFromJournal() {
        val id = st.addLayer("solid")!!
        st.save()
        st.op("setLayer", "layer" to id, "fields" to jo("name" to "After save"))
        // Simulate process death: the session lock and journal stay behind, nothing is saved.
        NativeBridge.call("simulateCrash")
        val info = NativeBridge.call("recoveryInfo", jo("id" to projectId))
        assertTrue(info.toString(), info.optBoolean("available") && info.optBoolean("abnormalExit"))
        // Reopen with recovery; the unsaved edit must be restored from the journal.
        assertEquals(null, st.open(projectId, recover = true))
        assertEquals("After save", st.layers.firstOrNull()?.optString("name"))
    }

    @Test
    fun whisperCaptionsFromBundledSpeech() {
        val wav = File(ctx.filesDir, "samples/speech.wav")
        assertTrue("bundled sample missing", wav.isFile)
        val model = File(ctx.filesDir, "models").listFiles()?.firstOrNull { it.name.endsWith(".bin") }
        assertNotNull("bundled whisper model missing", model)
        val asset = Importer.probe(ctx, Uri.fromFile(wav), Importer.Kind.AUDIO)
        val a = st.op("addAsset", "asset" to asset)!!
        val layer = st.addLayer("audio", jo("asset" to a.optString("asset")))!!
        var lastP = 0f
        val r = NativeBridge.task("transcribe", jo("layer" to layer, "language" to "en", "threads" to 4, "model" to model!!.absolutePath), ProgressCallback { p -> lastP = p; true })
        assertTrue(r.toString(), r.optBoolean("ok"))
        st.refresh()
        val caps = st.layers.first { it.optString("type") == "captions" }.obj("captions").arr("items").objects()
        val text = caps.joinToString(" ") { it.optString("text") }.lowercase()
        assertTrue("transcript: $text", text.contains("ask not what your country can do for you"))
        assertTrue(caps.all { it.optDouble("end") > it.optDouble("start") })
        // One undo step removes the generated captions.
        st.undo()
        assertFalse(st.layers.any { it.optString("type") == "captions" && it.obj("captions").arr("items").length() > 0 })
        // Subtitle export.
        st.redo()
        val srt = NativeBridge.call("exportSubtitles", jo("format" to "srt"))
        assertTrue(srt.optString("text").contains("-->"))
    }

    @Test
    fun exportMp4IsValidated() {
        st.addLayer("solid", jo("color" to listOf(0.9, 0.2, 0.2, 1.0)))
        val tid = st.addLayer("text", jo("text" to "Export"))!!
        st.playhead = 0.0; st.setProp(tid, "transform.rotation", 0.0, "key")
        st.playhead = 1.0; st.setProp(tid, "transform.rotation", 90.0, "key")
        val out = File(ctx.cacheDir, "e2e.mp4").apply { delete() }
        val eng = ExportEngine(st.duration, st.compWidth, st.compHeight)
        val res = eng.run(ExportSettings(format = "mp4", width = 320, height = 180, fps = 15.0, bitrate = 1_000_000, audio = true, t0 = 0.0, t1 = 1.0), out) { }
        assertTrue(out.isFile && out.length() > 1000)
        val v = res.obj("validation")
        assertTrue(res.toString(), v.optInt("videoFrames", v.optInt("frames")) >= 14)
        val ex = MediaExtractor()
        ex.setDataSource(out.absolutePath)
        val mimes = (0 until ex.trackCount).map { ex.getTrackFormat(it).getString(MediaFormat.KEY_MIME) ?: "" }
        ex.release()
        assertTrue(mimes.toString(), mimes.any { it.startsWith("video/") })
    }

    @Test
    fun exportGifIsValidated() {
        st.addLayer("shape", jo("shape" to "star"))
        val out = File(ctx.cacheDir, "e2e.gif").apply { delete() }
        val res = ExportEngine(st.duration, st.compWidth, st.compHeight).run(ExportSettings(format = "gif", width = 160, height = 90, fps = 10.0, t0 = 0.0, t1 = 1.0), out) { }
        assertTrue(res.toString(), res.optBoolean("ok"))
        assertTrue(res.obj("validation").optBoolean("validated"))
    }

    @Test
    fun scriptRunsAsSingleUndoStep() {
        val src = "var id = mf.layer.add('text', {text: 'From script'});\nmf.keyframe.add(id, 'transform.opacity', 0, 0);\nreturn id;"
        val r = NativeBridge.call("runScript", jo("name" to "e2e", "source" to src, "permissions" to listOf("PROJECT_READ", "TIMELINE_WRITE")))
        assertTrue(r.toString(), r.optBoolean("ok") && r.optBoolean("changed"))
        st.refresh()
        assertEquals(1, st.layers.size)
        st.undo()
        assertEquals(0, st.layers.size)
        // Permission denied without TIMELINE_WRITE.
        val denied = NativeBridge.call("runScript", jo("name" to "e2e2", "source" to src, "permissions" to listOf("PROJECT_READ")))
        assertFalse(denied.optBoolean("ok"))
    }

    @Test
    fun packageRoundTrip() {
        st.addLayer("text", jo("text" to "Packaged"))
        st.save()
        val f = File(ctx.cacheDir, "e2e.mforge").apply { delete() }
        val r = NativeBridge.call("exportProject", jo("path" to f.absolutePath, "mode" to "collected", "password" to "secret"))
        assertTrue(r.toString(), r.optBoolean("ok"))
        assertTrue(NativeBridge.call("packageEncrypted", jo("path" to f.absolutePath)).optBoolean("encrypted"))
        assertFalse(NativeBridge.call("importProject", jo("path" to f.absolutePath, "password" to "wrong")).optBoolean("ok"))
        val imp = NativeBridge.call("importProject", jo("path" to f.absolutePath, "password" to "secret"))
        assertTrue(imp.toString(), imp.optBoolean("ok"))
        NativeBridge.call("deleteProject", jo("id" to imp.optString("id")))
    }
}
