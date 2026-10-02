package com.motionforge.app

import android.graphics.Bitmap
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.motionforge.app.engine.EditorState
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.arr
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.obj
import com.motionforge.app.engine.objects
import com.motionforge.app.engine.strings
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/** Forge Capsules v2 and the Mobile Motion script API, exercised through JNI on the device. */
@RunWith(AndroidJUnit4::class)
class CapsuleScriptDeviceTest {
    @get:Rule val artifacts = FailureArtifacts()
    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    private fun hash(t: Double): Long {
        val b = Bitmap.createBitmap(160, 90, Bitmap.Config.ARGB_8888)
        NativeBridge.nativeRenderBitmap(b, t, false, true)
        val px = IntArray(160 * 90); b.getPixels(px, 0, 160, 0, 0, 160, 90)
        var h = 1469598103934665603L
        for (p in px) h = (h xor p.toLong()) * 1099511628211L
        return h
    }

    private fun withProject(block: (EditorState) -> Unit) {
        val st = EditorState()
        val id = NativeBridge.call("createProject", jo("name" to "Capsule test", "width" to 320, "height" to 180, "fps" to 24.0, "duration" to 3.0)).getString("id")
        try { assertEquals(null, st.open(id)); block(st) } finally { if (st.projectId.isNotEmpty()) st.close(); NativeBridge.call("deleteProject", jo("id" to id)) }
    }

    @Test
    fun capsuleTypedControlsAndPackageRoundTrip() {
        val pkg = File(ctx.cacheDir, "test.mfcapsule").apply { delete() }
        lateinit var capFile: String
        withProject { st ->
            // Author a capsule: animated shape + title, with every control type.
            val shape = st.addLayer("shape", jo("shape" to "ellipse", "color" to listOf(1.0, 0.3, 0.2, 1.0)))!!
            st.setProp(shape, "shape.items.0.size", listOf(50.0, 50.0), "static")
            st.playhead = 0.0; st.setProp(shape, "transform.position", listOf(60.0, 90.0, 0.0), "key")
            st.playhead = 2.0; st.setProp(shape, "transform.position", listOf(260.0, 90.0, 0.0), "key")
            val title = st.addLayer("text", jo("text" to "CAPSULE", "size" to 28.0))!!
            val controls = listOf(
                jo("name" to "Title", "type" to "text", "layer" to title, "path" to "text.content"),
                jo("name" to "Color", "type" to "color", "layer" to shape, "path" to "shape.fill.color"),
                jo("name" to "Size", "type" to "size", "layer" to shape, "path" to "transform.scale", "base" to 100, "default" to 100),
                jo("name" to "Position", "type" to "position", "layer" to shape, "path" to "transform.position", "default" to listOf(0.0, 0.0)),
                jo("name" to "Speed", "type" to "speed", "layer" to shape, "path" to "", "base" to 100, "default" to 100),
            )
            val r = NativeBridge.call("createCapsule", jo("layers" to listOf(shape, title), "name" to "DeviceCapsule", "controls" to controls))
            assertTrue(r.toString(), r.optBoolean("ok"))
            capFile = NativeBridge.call("capsules").arr("capsules").objects().first { it.optString("name") == "DeviceCapsule" }.optString("_file")
            val exp = NativeBridge.call("exportCapsulePackage", jo("path" to pkg.absolutePath, "capsule" to r.obj("capsule")))
            assertTrue(exp.toString(), exp.optBoolean("ok"))
        }
        NativeBridge.call("deleteCapsule", jo("file" to capFile))
        // Import the package (fonts registered, capsule saved to the library), then use it in a new project.
        val imp = NativeBridge.call("importCapsulePackage", jo("path" to pkg.absolutePath))
        assertTrue(imp.toString(), imp.optBoolean("ok"))
        assertTrue(imp.optInt("fonts") >= 1)
        withProject { st ->
            val cap = NativeBridge.call("capsules").arr("capsules").objects().first { it.optString("name") == "DeviceCapsule" }
            val lid = st.op("insertCapsule", "capsule" to cap, "t" to 0.0)!!.getString("layer")
            val base = hash(1.0)
            fun after(control: String, value: Any): Long {
                st.op("setCapsuleControl", "layer" to lid, "control" to control, "value" to value)
                val h = hash(1.0)
                st.undo()
                return h
            }
            assertNotEquals(base, after("Title", "CHANGED"))
            assertNotEquals(base, after("Color", listOf(0.1, 0.3, 1.0, 1.0)))
            assertNotEquals(base, after("Size", 220.0))
            assertNotEquals(base, after("Position", listOf(0.0, 50.0)))
            assertNotEquals(base, after("Speed", 200.0))
            assertEquals(base, hash(1.0))  // undo restores the author's look exactly
            st.op("setCapsuleControl", "layer" to lid, "control" to "Speed", "value" to 200.0)
            assertEquals("200% speed at 0.5 s equals 100% at 1.0 s", base, hash(0.5))
            NativeBridge.call("deleteCapsule", jo("file" to cap.optString("_file")))
        }
    }

    @Test
    fun scriptApiV2DrivesEveryAreaAsOneUndoStep() {
        withProject { st ->
            val api = NativeBridge.call("scriptApi").obj("api")
            assertEquals(2, api.optInt("apiVersion"))
            assertTrue(api.arr("functions").length() > 100)
            val src = """
                var t = mf.layer.add('text', {text: 'API'}, 0);
                mf.keyframe.add(t, 'transform.position', 0, [40, 90, 0]);
                mf.keyframe.add(t, 'transform.position', 2, [280, 90, 0]);
                mf.keyframe.interpAll(t, 'transform.position', 'easeOut');
                var s = mf.layer.add('shape', {shape: 'star'}, 0);
                var fx = mf.effect.add(s, 'stylize.glow');
                mf.mask.add(s, 'ellipse');
                mf.camera.add();
                mf.light.add('point');
                mf.model.primitive('cube', {size: 60});
                mf.caption.set([{start: 0, end: 1, text: 'from script'}]);
                mf.marker.add(1, 'beat');
                mf.ui.panel({fields: [{name: 'x', type: 'number', default: 1}]});
                return mf.prop.get(t, 'transform.position', 1)[0];
            """.trimIndent()
            val r = NativeBridge.call("runScript", jo("name" to "api", "source" to src, "permissions" to listOf("PROJECT_READ", "TIMELINE_WRITE", "PROJECT_WRITE")))
            assertTrue(r.toString(), r.optBoolean("ok") && r.optBoolean("changed"))
            assertTrue(r.optDouble("returnValue") in 41.0..279.0)
            assertTrue(r.arr("actions").objects().any { it.optString("type") == "panel" })
            st.refresh()
            val types = st.layers.map { it.optString("type") }.toSet()
            assertTrue(types.toString(), types.containsAll(listOf("text", "shape", "camera", "light", "model3d", "captions")))
            st.undo()
            assertEquals("whole script undone in one step", 0, st.layers.size)
            assertTrue(NativeBridge.call("exampleScripts").arr("scripts").objects().map { it.optString("name") }.any { it.startsWith("Lower Third") })
        }
    }
}
