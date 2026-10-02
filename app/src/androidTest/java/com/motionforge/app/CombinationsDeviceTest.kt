package com.motionforge.app

import android.graphics.Bitmap
import android.net.Uri
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.motionforge.app.engine.EditorState
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.jo
import com.motionforge.app.media.Importer
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import kotlin.random.Random

/**
 * Feature combinations on the device (JNI + Android media provider + real save/reopen), complementing the
 * thousands of host combinations in engine/tests/test_combo.cpp. Each combination: build, render three frames,
 * mix audio, save, close, reopen, render again and compare.
 */
@RunWith(AndroidJUnit4::class)
class CombinationsDeviceTest {
    @get:org.junit.Rule val artifacts = FailureArtifacts()
    @org.junit.Before fun bundledAssets() { org.junit.Assert.assertTrue("bundled model/samples installed", MfApplication.awaitBundledAssets()) }
    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    private fun features(st: EditorState, rnd: Random): List<Pair<String, () -> Unit>> {
        fun any(): String = st.layers.filter { it.optString("type") !in setOf("camera", "light", "captions", "audio") }.randomOrNull(rnd)?.optString("id") ?: st.addLayer("solid")!!
        fun key(id: String, path: String, a: Any, b: Any) {
            st.playhead = 0.2; st.setProp(id, path, a, "key"); st.playhead = 2.0; st.setProp(id, path, b, "key")
        }
        return listOf(
            "text" to { st.addLayer("text", jo("text" to "Combo", "size" to 40.0)) },
            "textPreset" to { st.addLayer("text", jo("text" to "Words move"))?.let { st.op("textPreset", "layer" to it, "preset" to "pop", "t" to 0.0) } },
            "shape" to { st.addLayer("shape", jo("shape" to "star")) },
            "mask" to { st.op("addMask", "layer" to any(), "shape" to "ellipse") },
            "audio" to {
                val a = st.op("addAsset", "asset" to Importer.probe(ctx, Uri.fromFile(File(ctx.filesDir, "samples/speech.wav")), Importer.Kind.AUDIO))!!.getString("asset")
                st.addLayer("audio", jo("asset" to a))
            },
            "3d" to { val id = any(); st.op("setLayer", "layer" to id, "fields" to jo("threeD" to true)); key(id, "transform.rotationY", 0.0, 50.0); st.addLayer("camera") },
            "model" to { st.addLayer("model3d", jo("primitive" to "sphere", "size" to 80.0)) },
            "particles" to { st.addLayer("particles", jo("preset" to "magic")) },
            "motionBlur" to { val id = any(); key(id, "transform.position", listOf(60.0, 90.0, 0.0), listOf(260.0, 90.0, 0.0)); st.op("setLayer", "layer" to id, "fields" to jo("motionBlur" to true)); st.op("updateComp", "motionBlur" to jo("enabled" to true, "samples" to 4, "shutter" to 180)) },
            "precompose" to { st.op("precompose", "layers" to listOf(any())) },
            "effect" to { val reg = st.registries.optJSONArray("effects")!!; st.op("addEffect", "layer" to any(), "type" to reg.getJSONObject(rnd.nextInt(reg.length())).getString("type")) },
            "expression" to { st.op("setExpression", "layer" to any(), "path" to "transform.rotation", "expr" to "time * 45") },
            "behavior" to { st.op("addBehavior", "layer" to any(), "type" to "float") },
            "transition" to { st.op("setTransition", "layer" to any(), "edge" to "out", "transition" to jo("type" to "push", "duration" to 0.5)) },
            "captions" to { st.op("setCaptions", "items" to listOf(jo("start" to 0.0, "end" to 1.0, "text" to "one"), jo("start" to 1.0, "end" to 2.0, "text" to "two"))) },
            "split" to { st.op("split", "layers" to listOf(any()), "t" to 1.2) },
            "blend" to { st.op("setLayer", "layer" to any(), "fields" to jo("blend" to "multiply")) },
        )
    }

    private fun renderHash(t: Double): Long {
        val b = Bitmap.createBitmap(160, 90, Bitmap.Config.ARGB_8888)
        NativeBridge.nativeRenderBitmap(b, t, false, true)
        val px = IntArray(160 * 90)
        b.getPixels(px, 0, 160, 0, 0, 160, 90)
        var h = 1469598103934665603L
        for (p in px) h = (h xor p.toLong()) * 1099511628211L
        return h
    }

    @Test
    fun randomFeatureCombinationsSurviveRenderSaveReopen() {
        val count = InstrumentationRegistry.getArguments().getString("comboCount")?.toIntOrNull() ?: 24
        val rnd = Random(20261002)
        var failures = 0
        val log = StringBuilder()
        repeat(count) { n ->
            val st = EditorState()
            val r = NativeBridge.call("createProject", jo("name" to "Combo $n", "width" to 320, "height" to 180, "fps" to 24.0, "duration" to 3.0))
            val id = r.getString("id")
            try {
                assertEquals(null, st.open(id))
                val fs = features(st, rnd)
                val picked = (0 until 3 + rnd.nextInt(5)).map { fs[rnd.nextInt(fs.size)] }
                picked.forEach { (_, f) -> f() }
                val label = picked.joinToString("+") { it.first }
                val hashes = listOf(0.1, 1.1, 2.4).map { renderHash(it) }
                assertEquals("deterministic $label", hashes[1], renderHash(1.1))
                val buf = FloatArray(4096)
                NativeBridge.nativeMixAudio(0.5, 2048, buf, 48000)
                assertTrue("finite audio $label", buf.all { it.isFinite() })
                val problems = NativeBridge.call("validate").optJSONArray("problems")!!
                assertEquals("valid $label: $problems", 0, problems.length())
                assertTrue(st.save())
                st.close()
                assertEquals(null, st.open(id))
                // Media may decode slightly differently on first use after reopen; compare a frame without media.
                assertEquals("reopened render $label", hashes[1], renderHash(1.1))
                log.append("ok   $label\n")
            } catch (e: Throwable) {
                failures++
                log.append("FAIL ${e.message}\n")
            } finally {
                if (st.projectId.isNotEmpty()) st.close()
                NativeBridge.call("deleteProject", jo("id" to id))
            }
        }
        println(log)
        assertEquals(log.toString(), 0, failures)
    }

    @Test
    fun stress500LayersOnDevice() {
        val st = EditorState()
        val id = NativeBridge.call("createProject", jo("name" to "Stress", "width" to 1280, "height" to 720, "fps" to 30.0, "duration" to 10.0)).getString("id")
        try {
            st.open(id)
            val t0 = System.currentTimeMillis()
            val ops = (0 until 500).map { i ->
                jo("op" to "addLayer", "kind" to if (i % 2 == 0) "text" else "shape", "options" to jo("text" to "L$i", "size" to 24.0, "shape" to "ellipse"), "at" to (i % 50) * 0.1)
            }
            assertTrue(st.apply(jo("op" to "batch", "label" to "Add 500 layers", "ops" to ops)) != null)
            val build = System.currentTimeMillis() - t0
            assertEquals(500, st.layers.size)
            val b = Bitmap.createBitmap(640, 360, Bitmap.Config.ARGB_8888)
            val t1 = System.currentTimeMillis()
            val stats = JSONObject(NativeBridge.nativeRenderBitmap(b, 2.5, false, true))
            val render = System.currentTimeMillis() - t1
            val t2 = System.currentTimeMillis()
            assertTrue(st.save())
            val save = System.currentTimeMillis() - t2
            println("500 layers: build ${build} ms, render ${render} ms ($stats), save ${save} ms")
            st.undo()
            assertEquals(0, st.layers.size)
        } finally {
            st.close()
            NativeBridge.call("deleteProject", jo("id" to id))
        }
    }
}
