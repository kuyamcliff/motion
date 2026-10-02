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
import com.motionforge.app.media.MediaBridge
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/** Imported video must decode (MediaCodec) and composite correctly in preview renders and exports. */
@RunWith(AndroidJUnit4::class)
class VideoLayerDeviceTest {
    @get:Rule val artifacts = FailureArtifacts()
    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    private fun newOpen(st: EditorState, name: String, dur: Double): String {
        val id = NativeBridge.call("createProject", jo("name" to name, "width" to 320, "height" to 180, "fps" to 24.0, "duration" to dur)).getString("id")
        assertEquals(null, st.open(id))
        return id
    }

    /** A 2 s clip: solid blue frame with a red square in the left half. */
    private fun makeClip(): File {
        val st = EditorState()
        val id = newOpen(st, "clip src", 2.0)
        try {
            st.addLayer("solid", jo("color" to listOf(0.0, 0.0, 1.0, 1.0)))
            val sq = st.addLayer("shape", jo("shape" to "rect", "color" to listOf(1.0, 0.0, 0.0, 1.0)))!!
            st.setProp(sq, "shape.items.0.size", listOf(60.0, 60.0), "static")
            st.setProp(sq, "transform.position", listOf(80.0, 90.0, 0.0), "static")
            val out = File(ctx.cacheDir, "videolayer-src.mp4").apply { delete() }
            ExportEngine(2.0, 320, 180).run(ExportSettings(format = "mp4", width = 320, height = 180, fps = 24.0, bitrate = 2_000_000, audio = false, t0 = 0.0, t1 = 2.0), out) { }
            return out
        } finally { st.close(); NativeBridge.call("deleteProject", jo("id" to id)) }
    }

    private fun isBlue(p: Int) = Color.blue(p) > 150 && Color.red(p) < 90 && Color.green(p) < 90
    private fun isRed(p: Int) = Color.red(p) > 150 && Color.blue(p) < 90 && Color.green(p) < 90

    @Test
    fun importedVideoDecodesAndComposites() {
        val clip = makeClip()
        val st = EditorState()
        val id = newOpen(st, "video layer", 2.0)
        try {
            val asset = Importer.probe(ctx, Uri.fromFile(clip), Importer.Kind.VIDEO)
            assertEquals(320, asset.optInt("width")); assertEquals(180, asset.optInt("height"))
            val aid = st.op("addAsset", "asset" to asset)!!.getString("asset")
            val stored = st.asset(aid)!!
            // 1) Decoder alone.
            val frame = MediaBridge.videoFrame(stored.toString(), 1.0, 320, 180)
            assertNotNull("MediaBridge.videoFrame returned null", frame)
            assertTrue("decoded frame should be blue at (250,90): ${Integer.toHexString(frame!!.getPixel(250 * frame.width / 320, 90 * frame.height / 180))}",
                isBlue(frame.getPixel(250 * frame.width / 320, 90 * frame.height / 180)))
            // 2) Engine composite of a video layer (preview path).
            st.apply(jo("op" to "addLayer", "kind" to "video", "options" to jo("asset" to aid), "at" to 0.0))
            val b = Bitmap.createBitmap(320, 180, Bitmap.Config.ARGB_8888)
            val stats = NativeBridge.nativeRenderBitmap(b, 1.0, false, true)
            assertTrue("render of video layer should show blue at (250,90): ${Integer.toHexString(b.getPixel(250, 90))} stats=$stats", isBlue(b.getPixel(250, 90)))
            assertTrue("and the red square at (80,90): ${Integer.toHexString(b.getPixel(80, 90))}", isRed(b.getPixel(80, 90)))
            // 3) Re-export (video layer → MP4) keeps the picture.
            val out = File(ctx.cacheDir, "videolayer-out.mp4").apply { delete() }
            ExportEngine(2.0, 320, 180).run(ExportSettings(format = "mp4", width = 320, height = 180, fps = 24.0, bitrate = 2_000_000, audio = false, t0 = 0.0, t1 = 2.0), out) { }
            val mmr = android.media.MediaMetadataRetriever().apply { setDataSource(out.absolutePath) }
            val f = mmr.getFrameAtTime(1_000_000, android.media.MediaMetadataRetriever.OPTION_CLOSEST)!!
            mmr.release()
            assertTrue("exported frame blue at (250,90): ${Integer.toHexString(f.getPixel(250, 90))}", isBlue(f.getPixel(250, 90)))
        } finally { st.close(); NativeBridge.call("deleteProject", jo("id" to id)) }
    }
}
