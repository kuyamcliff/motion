package com.motionforge.app

import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.motionforge.app.engine.EditorState
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.jo
import com.motionforge.app.export.ExportEngine
import com.motionforge.app.export.ExportSettings
import com.motionforge.app.media.Importer
import com.motionforge.app.playback.GpuRenderer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * The GPU preview (OpenGL ES compositor + hardware video decoding into textures) must show the same picture as the
 * CPU renderer. Each scene is rendered both ways at the same size and compared pixel by pixel.
 */
@RunWith(AndroidJUnit4::class)
class GpuPreviewTest {
    @get:Rule val artifacts = FailureArtifacts()
    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var gpu: GpuRenderer
    private val st = EditorState()
    private var pid = ""

    @Before fun setUp() {
        assertTrue(MfApplication.awaitBundledAssets())
        gpu = GpuRenderer()
        assertTrue("GPU renderer failed to start: ${gpu.lastError}", gpu.ready)
    }

    @After fun tearDown() {
        gpu.release()
        if (st.projectId.isNotEmpty()) st.close()
        if (pid.isNotEmpty()) NativeBridge.call("deleteProject", jo("id" to pid))
    }

    private fun open(name: String, w: Int = 640, h: Int = 360, dur: Double = 3.0) {
        pid = NativeBridge.call("createProject", jo("name" to name, "width" to w, "height" to h, "fps" to 24.0, "duration" to dur)).getString("id")
        assertEquals(null, st.open(pid))
    }

    /** Mean absolute channel difference (0–255) between GPU and CPU renders, and the share of strongly different pixels. */
    private fun compare(t: Double, label: String, w: Int = 640, h: Int = 360): Pair<Double, Double> {
        val g = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val stats = gpu.renderBlocking(t, w, true, g)
        assertNotNull("$label: GPU frame failed", stats)
        assertTrue("$label: readback failed $stats", stats!!.optBoolean("readback"))
        assertTrue("$label: GL error $stats", !stats.has("glError"))
        val c = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        NativeBridge.nativeRenderBitmap(c, t, false, false)
        var sum = 0.0
        var bad = 0
        val gp = IntArray(w * h); val cp = IntArray(w * h)
        g.getPixels(gp, 0, w, 0, 0, w, h); c.getPixels(cp, 0, w, 0, 0, w, h)
        for (i in gp.indices) {
            val a = gp[i]; val b = cp[i]
            val d = Math.abs(Color.red(a) - Color.red(b)) + Math.abs(Color.green(a) - Color.green(b)) + Math.abs(Color.blue(a) - Color.blue(b))
            sum += d / 3.0
            if (d / 3 > 64) bad++
        }
        val mean = sum / gp.size
        val badFrac = bad.toDouble() / gp.size
        if (mean > 3.0 || badFrac > 0.01) {
            TestArtifacts.savePng("gpu-$label-gpu.png", g)
            TestArtifacts.savePng("gpu-$label-cpu.png", c)
        }
        return mean to badFrac
    }

    private fun assertMatches(t: Double, label: String) {
        val (mean, bad) = compare(t, label)
        assertTrue("$label: GPU vs CPU mean diff %.2f, %.2f%% far off".format(mean, bad * 100), mean <= 3.0 && bad <= 0.01)
    }

    @Test
    fun layersBlendModesAnd3DMatchCpu() {
        open("gpu layers")
        st.addLayer("solid", jo("color" to listOf(0.1, 0.2, 0.6, 1.0)))
        val shape = st.addLayer("shape", jo("shape" to "star", "color" to listOf(1.0, 0.7, 0.1, 1.0)))!!
        st.playhead = 0.0; st.setProp(shape, "transform.rotation", 0.0, "key")
        st.playhead = 2.0; st.setProp(shape, "transform.rotation", 180.0, "key")
        val text = st.addLayer("text", jo("text" to "GPU", "size" to 90.0))!!
        st.op("setLayer", "layer" to text, "fields" to jo("blend" to "screen"))
        st.playhead = 0.0; st.setProp(text, "transform.position", listOf(120.0, 90.0, 0.0), "key")
        st.playhead = 2.0; st.setProp(text, "transform.position", listOf(520.0, 270.0, 0.0), "key")
        val ell = st.addLayer("shape", jo("shape" to "ellipse", "color" to listOf(0.0, 1.0, 0.4, 1.0)))!!
        st.op("setLayer", "layer" to ell, "fields" to jo("blend" to "multiply"))
        val sq = st.addLayer("solid", jo("color" to listOf(0.9, 0.1, 0.3, 1.0), "width" to 160, "height" to 160))!!
        st.op("setLayer", "layer" to sq, "fields" to jo("threeD" to true))
        st.setProp(sq, "transform.rotationY", 40.0, "static")
        st.addLayer("camera")
        st.setProp(sq, "transform.opacity", 70.0, "static")
        for (t in listOf(0.0, 1.0, 1.7)) assertMatches(t, "layers-t$t")
        // Second frame reuses cached layer textures (static content, animated transforms).
        val s2 = gpu.renderBlocking(1.2, 640, true, Bitmap.createBitmap(640, 360, Bitmap.Config.ARGB_8888))!!
        assertTrue("layer textures cached: $s2", s2.optInt("cachedTextures") >= 2)
        assertTrue("plan used cached rasters: $s2", s2.getJSONObject("plan").optInt("cached") >= 2)
    }

    @Test
    fun effectsMattesAdjustmentMatchCpu() {
        open("gpu fx")
        st.addLayer("solid", jo("color" to listOf(0.2, 0.2, 0.2, 1.0)))
        val a = st.addLayer("shape", jo("shape" to "rect", "color" to listOf(1.0, 0.2, 0.2, 1.0)))!!
        st.op("addEffect", "layer" to a, "type" to "blur.gaussian")
        val fill = st.addLayer("solid", jo("color" to listOf(0.1, 0.9, 0.9, 1.0)))!!
        val m = st.addLayer("text", jo("text" to "MATTE", "size" to 100.0))!!
        st.op("setLayer", "layer" to fill, "fields" to jo("matte" to jo("layer" to m, "mode" to "alpha", "invert" to false)))
        assertMatches(1.0, "fx")
        val adj = st.addLayer("adjustment")!!
        st.op("addEffect", "layer" to adj, "type" to "color.invert")
        val stats = gpu.renderBlocking(1.0, 640, true, Bitmap.createBitmap(640, 360, Bitmap.Config.ARGB_8888))!!
        assertTrue("adjustment layer falls back to one CPU frame: $stats", stats.getJSONObject("plan").optBoolean("fallback"))
        assertMatches(1.0, "adjustment")
    }

    @Test
    fun videoDecodesOnGpuAndMatchesCpu() {
        // Source clip: blue frame with a red square, exported through the app's own encoder.
        open("gpu clip src", 640, 360, 2.0)
        st.addLayer("solid", jo("color" to listOf(0.0, 0.0, 1.0, 1.0)))
        val sq = st.addLayer("shape", jo("shape" to "rect", "color" to listOf(1.0, 0.0, 0.0, 1.0)))!!
        st.setProp(sq, "shape.items.0.size", listOf(120.0, 120.0), "static")
        st.setProp(sq, "transform.position", listOf(160.0, 180.0, 0.0), "static")
        val clip = File(ctx.cacheDir, "gpu-src.mp4").apply { delete() }
        ExportEngine(2.0, 640, 360).run(ExportSettings(format = "mp4", width = 640, height = 360, fps = 24.0, bitrate = 4_000_000, audio = false, t0 = 0.0, t1 = 2.0), clip) { }
        st.close(); NativeBridge.call("deleteProject", jo("id" to pid))

        open("gpu video", 640, 360, 2.0)
        val aid = st.op("addAsset", "asset" to Importer.probe(ctx, Uri.fromFile(clip), Importer.Kind.VIDEO))!!.getString("asset")
        val v = st.apply(jo("op" to "addLayer", "kind" to "video", "options" to jo("asset" to aid), "at" to 0.0))!!.getString("layer")
        st.setProp(v, "transform.scale", listOf(80.0, 80.0, 100.0), "static")
        st.setProp(v, "transform.rotation", 10.0, "static")
        val title = st.addLayer("text", jo("text" to "OVER VIDEO", "size" to 50.0))!!
        st.setProp(title, "transform.position", listOf(320.0, 60.0, 0.0), "static")
        val g = Bitmap.createBitmap(640, 360, Bitmap.Config.ARGB_8888)
        val stats = gpu.renderBlocking(1.0, 640, true, g)!!
        assertEquals("video went through the hardware decoder into a GPU texture: $stats", 1, stats.optInt("videoGpu"))
        assertTrue("blue footage visible", Color.blue(g.getPixel(450, 200)) > 150 && Color.red(g.getPixel(450, 200)) < 90)
        // Hardware decode + GPU sampling vs CPU decode + CPU warp: compare with a slightly wider tolerance (codec/YUV paths differ).
        val (mean, bad) = compare(1.0, "video")
        assertTrue("video: GPU vs CPU mean diff %.2f, %.2f%% far off".format(mean, bad * 100), mean <= 6.0 && bad <= 0.02)
        // Scrubbing backwards and forwards keeps decoding correct frames.
        for (t in listOf(0.2, 1.8, 0.5, 1.0)) {
            val s = gpu.renderBlocking(t, 640, true, g)!!
            assertEquals("t=$t decoded on GPU", 1, s.optInt("videoGpu"))
            assertTrue("t=$t blue footage", Color.blue(g.getPixel(450, 200)) > 150)
        }
    }
}
