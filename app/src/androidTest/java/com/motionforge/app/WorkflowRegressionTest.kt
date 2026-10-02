package com.motionforge.app

import android.graphics.Bitmap
import android.graphics.Color
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMetadataRetriever
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
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * The full creator workflow on a real device, ending with automatic inspection of the exported video:
 * Create project → import video → trim → split → add text → animate → add FX → mask → keyframes → add audio →
 * offline captions → export MP4 → close → reopen → verify document → decode the export and check pixels/tracks.
 *
 * The "camera footage" is generated first by exporting a known synthetic clip (blue frame, moving red square,
 * speech audio), so pixel expectations are exact rather than guessed.
 */
@RunWith(AndroidJUnit4::class)
class WorkflowRegressionTest {
    @get:org.junit.Rule val artifacts = FailureArtifacts()
    @org.junit.Before fun bundledAssets() { org.junit.Assert.assertTrue("bundled model/samples installed", MfApplication.awaitBundledAssets()) }
    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    private fun createAndOpen(st: EditorState, name: String, dur: Double): String {
        val r = NativeBridge.call("createProject", jo("name" to name, "width" to 640, "height" to 360, "fps" to 24.0, "duration" to dur))
        assertTrue(r.toString(), r.optBoolean("ok"))
        val id = r.getString("id")
        assertEquals(null, st.open(id))
        return id
    }

    private fun importMedia(st: EditorState, f: File, kind: Importer.Kind): String {
        val asset = Importer.probe(ctx, Uri.fromFile(f), kind)
        return st.op("addAsset", "asset" to asset)!!.getString("asset")
    }

    private fun makeSourceFootage(): File {
        val st = EditorState()
        val id = createAndOpen(st, "Source footage", 4.0)
        try {
            st.addLayer("solid", jo("color" to listOf(0.0, 0.0, 1.0, 1.0)))
            val sq = st.addLayer("shape", jo("shape" to "rect", "color" to listOf(1.0, 0.0, 0.0, 1.0)))!!
            st.setProp(sq, "shape.items.0.size", listOf(100.0, 100.0), "static")
            st.playhead = 0.0; st.setProp(sq, "transform.position", listOf(80.0, 180.0, 0.0), "key")
            st.playhead = 4.0; st.setProp(sq, "transform.position", listOf(560.0, 180.0, 0.0), "key")
            val a = importMedia(st, File(ctx.filesDir, "samples/speech.wav"), Importer.Kind.AUDIO)
            st.addLayer("audio", jo("asset" to a))
            val out = File(ctx.cacheDir, "source-footage.mp4").apply { delete() }
            ExportEngine(st.duration, st.compWidth, st.compHeight).run(ExportSettings(format = "mp4", width = 640, height = 360, fps = 24.0, bitrate = 4_000_000, t0 = 0.0, t1 = 4.0), out) { }
            return out
        } finally {
            st.close()
            NativeBridge.call("deleteProject", jo("id" to id))
        }
    }

    private fun Int.r() = Color.red(this)
    private fun Int.g() = Color.green(this)
    private fun Int.b() = Color.blue(this)

    @Test
    fun fullCreatorWorkflowEndsInVerifiedExport() {
        val footage = makeSourceFootage()
        assertTrue(footage.length() > 10_000)

        val st = EditorState()
        val pid = createAndOpen(st, "Workflow regression", 6.0)
        try {
            // Import video (same path as gallery/Files import, minus the system picker).
            val vAsset = importMedia(st, footage, Importer.Kind.VIDEO)
            val clip = st.apply(jo("op" to "addLayer", "kind" to "video", "options" to jo("asset" to vAsset), "at" to 0.0))!!.getString("layer")
            st.op("setLayer", "layer" to clip, "fields" to jo("muted" to true))  // dialogue comes from the separate audio layer
            // Trim the end, then split into two clips.
            assertNotNull(st.op("trimLayer", "layer" to clip, "edge" to "out", "t" to 3.5))
            assertNotNull(st.op("split", "layers" to listOf(clip), "t" to 2.0))
            val clips = st.layers.filter { it.optString("type") == "video" }.sortedBy { it.optDouble("in") }
            assertEquals(2, clips.size)
            val first = clips[0].getString("id"); val second = clips[1].getString("id")
            // FX: invert colors on the second clip (blue → yellow). Mask: ellipse on the first clip (corners black).
            assertNotNull(st.op("addEffect", "layer" to second, "type" to "color.invert"))
            assertNotNull(st.op("addMask", "layer" to first, "shape" to "ellipse"))
            // Title text, animated opacity + keyframed scale with easing.
            val title = st.addLayer("text", jo("text" to "WORKFLOW", "size" to 56.0))!!
            st.setProp(title, "transform.position", listOf(320.0, 50.0, 0.0), "static")
            st.playhead = 0.0; st.setProp(title, "transform.opacity", 0.0, "key")
            st.playhead = 0.5; st.setProp(title, "transform.opacity", 100.0, "key")
            st.playhead = 0.0; st.setProp(title, "transform.scale", listOf(80.0, 80.0, 100.0), "key")
            st.playhead = 1.0; st.setProp(title, "transform.scale", listOf(100.0, 100.0, 100.0), "key")
            assertNotNull(st.op("setKeyframeInterp", "layer" to title, "path" to "transform.scale", "t" to 0.0, "interp" to "backOut", "all" to true))
            // Audio + offline captions from it (Whisper on device).
            val aAsset = importMedia(st, File(ctx.filesDir, "samples/speech.wav"), Importer.Kind.AUDIO)
            val audio = st.apply(jo("op" to "addLayer", "kind" to "audio", "options" to jo("asset" to aAsset), "at" to 0.0))!!.getString("layer")
            val model = File(ctx.filesDir, "models").listFiles()!!.first { it.name.endsWith(".bin") }.absolutePath
            val tr = NativeBridge.task("transcribe", jo("layer" to audio, "language" to "en", "threads" to 4, "model" to model), ProgressCallback { true })
            assertTrue(tr.toString(), tr.optBoolean("ok"))
            st.refresh()
            val captionCount = st.layers.first { it.optString("type") == "captions" }.obj("captions").arr("items").length()
            assertTrue(captionCount > 0)

            // Export the edit.
            val out = File(ctx.cacheDir, "workflow-export.mp4").apply { delete() }
            val res = ExportEngine(st.duration, st.compWidth, st.compHeight)
                .run(ExportSettings(format = "mp4", width = 640, height = 360, fps = 24.0, bitrate = 6_000_000, audio = true, t0 = 0.0, t1 = 3.5), out) { }
            assertTrue(res.toString(), res.obj("validation").optBoolean("ok"))
            assertTrue(res.optBoolean("audioIncluded"))

            // Close and reopen: every edit persisted.
            assertTrue(st.save())
            st.close()
            assertEquals(null, st.open(pid))
            val vids = st.layers.filter { it.optString("type") == "video" }.sortedBy { it.optDouble("in") }
            assertEquals(2, vids.size)
            assertEquals(3.5, vids[1].optDouble("out"), 1e-6)
            assertEquals("color.invert", vids[1].arr("effects").objects()[0].optString("type"))
            assertEquals(1, vids[0].arr("masks").length())
            val t = st.layers.first { it.optString("type") == "text" }
            assertEquals(2, t.obj("transform").obj("scale").arr("k").length())
            assertEquals(captionCount, st.layers.first { it.optString("type") == "captions" }.obj("captions").arr("items").length())

            // Inspect the exported video itself.
            val ex = MediaExtractor()
            ex.setDataSource(out.absolutePath)
            val mimes = (0 until ex.trackCount).map { ex.getTrackFormat(it).getString(MediaFormat.KEY_MIME) ?: "" }
            ex.release()
            assertTrue(mimes.toString(), mimes.any { it.startsWith("video/") } && mimes.any { it.startsWith("audio/") })
            val mmr = MediaMetadataRetriever()
            mmr.setDataSource(out.absolutePath)
            val durMs = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)!!.toLong()
            assertEquals(3500.0, durMs.toDouble(), 200.0)
            fun frame(sec: Double): Bitmap = mmr.getFrameAtTime((sec * 1e6).toLong(), MediaMetadataRetriever.OPTION_CLOSEST)!!
            val f1 = frame(1.0)
            val f3 = frame(3.0)
            mmr.release()
            // Keep the inspected frames as artifacts (pulled by run-tests.sh) for diagnosis.
            TestArtifacts.savePng("workflow-f1.png", f1)
            TestArtifacts.savePng("workflow-f3.png", f3)
            // First clip: blue footage inside the ellipse mask, black outside it (corner).
            val inside = f1.getPixel(320, 120)  // inside the default video mask (ellipse ~192x108 radii), clear of the square
            assertTrue("inside mask should be blue: ${Integer.toHexString(inside)}", inside.b() > 150 && inside.r() < 90 && inside.g() < 90)
            val corner = f1.getPixel(4, 4)
            assertTrue("corner should be masked to black: ${Integer.toHexString(corner)}", corner.r() < 50 && corner.g() < 50 && corner.b() < 50)
            // Second clip: invert effect turns blue into yellow.
            val inverted = f3.getPixel(320, 120)
            assertTrue("inverted should be yellow: ${Integer.toHexString(inverted)}", inverted.r() > 150 && inverted.g() > 150 && inverted.b() < 100)
            // Title is visible near the top centre (white text pixels).
            var white = 0
            for (y in 25..75) for (x in 180..460) { val p = f1.getPixel(x, y); if (p.r() > 200 && p.g() > 200 && p.b() > 200) white++ }
            assertTrue("title pixels found: $white", white > 50)
        } finally {
            if (st.projectId.isNotEmpty()) st.close()
            NativeBridge.call("deleteProject", jo("id" to pid))
        }
    }
}
