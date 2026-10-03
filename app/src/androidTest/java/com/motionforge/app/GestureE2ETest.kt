package com.motionforge.app

import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.SemanticsNodeInteraction
import androidx.compose.ui.test.TouchInjectionScope
import androidx.compose.ui.test.click
import androidx.compose.ui.test.pinch
import androidx.compose.ui.test.hasContentDescription
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onFirst
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTouchInput
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.arr
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.obj
import com.motionforge.app.engine.objects
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Drives the editor's touch gestures with real pointer input (down / move / up, multi-touch) and verifies the
 * resulting document edits, undo steps and view state: timeline trim, move, two-finger cancel and pinch zoom;
 * preview move / scale handles, mask rectangle, freehand draw and pen tool; graph-editor keyframe drag and the
 * bezier easing handles.
 */
@RunWith(AndroidJUnit4::class)
class GestureE2ETest {
    @get:Rule val compose = createEmptyComposeRule()
    @get:Rule val artifacts = FailureArtifacts { if (::scenario.isInitialized) scenario.close(); cleanup() }
    private lateinit var scenario: ActivityScenario<MainActivity>
    private lateinit var app: AppState
    private val name = "Gestures ${System.nanoTime() % 1_000_000}"
    private var projectId = ""
    private val timeout = 90_000L
    /** Timeline scale at the default zoom (EditorUi.timelineZoom), in pixels per second. */
    private val pxPerSec = 80f

    @Before
    fun launch() {
        Settings.firstRunDone = true
        Settings.snapping = false  // exact times; snapping has its own engine tests
        scenario = ActivityScenario.launch(MainActivity::class.java)
        scenario.onActivity { app = it.app }
    }

    private fun cleanup() {
        Settings.snapping = true
        if (projectId.isNotEmpty()) NativeBridge.call("deleteProject", jo("id" to projectId))
    }

    // ------------------------------------------------------------ helpers
    private fun exists(m: SemanticsMatcher) = try { compose.onAllNodes(m).fetchSemanticsNodes().isNotEmpty() } catch (e: IllegalStateException) { false }
    private fun waitDesc(d: String) = compose.waitUntil(timeout) { exists(hasContentDescription(d, substring = true)) }
    private fun node(desc: String): SemanticsNodeInteraction { waitDesc(desc); return compose.onAllNodes(hasContentDescription(desc, substring = true)).onFirst() }
    private fun tap(text: String) {
        compose.waitUntil(timeout) { exists(hasText(text)) }
        compose.onAllNodesWithText(text).onFirst().apply { try { performScrollTo() } catch (_: Throwable) {} }.performClick()
    }
    private fun poll(what: String, ms: Long = timeout, detail: () -> String = { "" }, cond: () -> Boolean) {
        try { compose.waitUntil(ms) { try { cond() } catch (e: Exception) { false } } }
        catch (e: Throwable) { throw AssertionError("$what: not reached within $ms ms. ${detail()}", e) }
    }

    private fun newProject() {
        projectId = NativeBridge.call("createProject", jo("name" to name, "width" to 640, "height" to 360, "fps" to 24.0, "duration" to 4.0)).getString("id")
        compose.runOnIdle { assertEquals(null, app.openProject(projectId)) }
        waitDesc("Undo")
    }

    private val st get() = app.editor
    private fun layer(id: String) = st.layer(id)!!

    /** A drag in small steps (as a finger produces), so slop detection and per-move previews behave as on a phone. */
    private fun TouchInjectionScope.drag(from: Offset, to: Offset, steps: Int = 12) {
        down(from)
        for (i in 1..steps) moveTo(from + (to - from) * (i.toFloat() / steps))
        up()
    }

    private fun stateOf(desc: String): String =
        compose.onAllNodes(hasContentDescription(desc, substring = true)).onFirst().fetchSemanticsNode()
            .config.getOrNull(SemanticsProperties.StateDescription) ?: ""

    // ------------------------------------------------------------ timeline
    @Test
    fun timeline_trimMoveTwoFingerCancelAndPinch() {
        newProject()
        val id = compose.runOnIdle { st.addLayer("solid")!! }   // 0 s → 4 s
        val clip = { node("Clip ") }
        clip()  // rendered
        val y = { h: Float -> h / 2 }

        // Drag the out handle (x = 4 s) to 2 s: one "Trim Clip" undo step.
        clip().performTouchInput { drag(Offset(4 * pxPerSec - 2, y(height.toFloat())), Offset(2 * pxPerSec, y(height.toFloat()))) }
        poll("trimmed out to 2 s", detail = { "out=${layer(id).optDouble("out")}" }) { Math.abs(layer(id).optDouble("out") - 2.0) < 0.05 }
        assertEquals("Trim Clip", st.undoLabel)
        compose.runOnIdle { st.undo() }
        poll("undo restores out") { Math.abs(layer(id).optDouble("out") - 4.0) < 1e-6 }

        // Tap to select, then drag the body 1 s to the right: one "Move Layer" step.
        clip().performTouchInput { click(Offset(1.25f * pxPerSec, y(height.toFloat()))) }
        poll("selected") { id in st.selection }
        clip().performTouchInput { drag(Offset(1.25f * pxPerSec, y(height.toFloat())), Offset(2.25f * pxPerSec, y(height.toFloat()))) }
        poll("moved by 1 s", detail = { "in=${layer(id).optDouble("in")}" }) { Math.abs(layer(id).optDouble("in") - 1.0) < 0.05 }
        assertEquals("Move Layer", st.undoLabel)
        compose.runOnIdle { st.undo() }
        poll("undo restores in") { Math.abs(layer(id).optDouble("in")) < 1e-6 }

        // A second finger during a trim cancels it: nothing is committed.
        val labelBefore = st.undoLabel
        clip().performTouchInput {
            val h = y(height.toFloat())
            down(0, Offset(4 * pxPerSec - 2, h))
            for (i in 1..8) moveTo(0, Offset(4 * pxPerSec - 2 - i * 15f, h))
            down(1, Offset(width - 40f, h))
            moveTo(1, Offset(width - 80f, h))
            up(1); up(0)
        }
        compose.waitForIdle()
        assertEquals("cancelled trim must not change the clip", 4.0, layer(id).optDouble("out"), 1e-6)
        assertEquals("cancelled trim must not add an undo step", labelBefore, st.undoLabel)

        // Pinch out on the zoom strip zooms the timeline in.
        assertEquals("80 pixels per second", stateOf("Timeline zoom"))
        node("Timeline zoom").performTouchInput {
            val c = Offset(width / 2f, height / 2f)
            pinch(c - Offset(40f, 0f), c - Offset(200f, 0f), c + Offset(40f, 0f), c + Offset(200f, 0f))
        }
        poll("timeline zoomed in", detail = { stateOf("Timeline zoom") }) { stateOf("Timeline zoom").substringBefore(' ').toInt() > 150 }
    }

    // ------------------------------------------------------------ preview
    @Test
    fun preview_moveScaleMaskDrawAndPen() {
        newProject()
        val id = compose.runOnIdle {
            val l = st.addLayer("shape", jo("shape" to "rect", "color" to listOf(1.0, 0.0, 0.0, 1.0)))!!
            st.setProp(l, "shape.items.0.size", listOf(100.0, 100.0), "static")
            st.setProp(l, "transform.position", listOf(320.0, 180.0, 0.0), "static")
            st.selection = setOf(l)
            l
        }
        val preview = { node("Preview canvas") }
        fun pos() = st.evalProps(id, listOf("transform.position")).getJSONArray("transform.position")
        fun scale() = st.evalProps(id, listOf("transform.scale")).getJSONArray("transform.scale").getDouble(0)
        // View geometry: the 640×360 comp is fitted into the canvas and centred (zoom 1, no pan).
        var fit = 1f; var cx = 0f; var cy = 0f
        preview().performTouchInput { fit = minOf(width / 640f, height / 360f); cx = width / 2f; cy = height / 2f }

        // Drag the layer body 80 comp px right and 40 down: one "Move Layer" step.
        poll("selection handles ready") { st.selection == setOf(id) }
        compose.waitForIdle()
        preview().performTouchInput { drag(Offset(cx, cy), Offset(cx + 80 * fit, cy + 40 * fit)) }
        poll("layer moved", detail = { "pos=${pos()}" }) { Math.abs(pos().getDouble(0) - 400) < 4 && Math.abs(pos().getDouble(1) - 220) < 4 }
        assertEquals("Move Layer", st.undoLabel)
        compose.runOnIdle { st.undo() }
        poll("undo move") { Math.abs(pos().getDouble(0) - 320) < 1e-6 }

        // Drag a corner handle outward to 2× distance from the centre: "Scale Layer" ≈ 200 %.
        compose.waitForIdle()
        preview().performTouchInput { drag(Offset(cx + 50 * fit, cy + 50 * fit), Offset(cx + 100 * fit, cy + 100 * fit)) }
        poll("layer scaled", detail = { "scale=${scale()}" }) { Math.abs(scale() - 200) < 10 }
        assertEquals("Scale Layer", st.undoLabel)
        compose.runOnIdle { st.undo() }
        poll("undo scale") { Math.abs(scale() - 100) < 1e-6 }

        // Mask mode: drag a rectangle over the layer → a closed 4-point mask.
        tap("Mask")
        preview().performTouchInput { drag(Offset(cx - 30 * fit, cy - 30 * fit), Offset(cx + 30 * fit, cy + 30 * fit)) }
        poll("mask added") { layer(id).arr("masks").length() == 1 }

        // Draw mode: a freehand stroke becomes a vector "Drawing" shape layer.
        tap("Select"); tap("Draw")
        val before = st.layers.size
        preview().performTouchInput {
            down(Offset(cx - 150 * fit, cy))
            for (i in 1..30) moveTo(Offset(cx - 150 * fit + i * 10 * fit, cy + (if (i % 2 == 0) 20 else -20) * fit))
            up()
        }
        poll("drawing layer") { st.layers.size == before + 1 && st.layers.any { it.optString("name") == "Drawing" } }

        // Pen mode: three taps then "Make shape".
        tap("Pen")
        listOf(Offset(-100f, -60f), Offset(100f, -60f), Offset(0f, 80f)).forEach { o ->
            preview().performTouchInput { click(Offset(cx + o.x * fit, cy + o.y * fit)) }
            compose.waitForIdle()
        }
        tap("Make shape")
        poll("pen shape layer") { st.layers.size == before + 2 }
        tap("Select")
    }

    // ------------------------------------------------------------ graph editor & bezier
    @Test
    fun graphEditor_dragKeyframeAndBezierHandles() {
        newProject()
        val id = compose.runOnIdle {
            val l = st.addLayer("shape", jo("shape" to "ellipse"))!!
            st.playhead = 0.0; st.setProp(l, "transform.position", listOf(100.0, 180.0, 0.0), "key")
            st.playhead = 2.0; st.setProp(l, "transform.position", listOf(540.0, 180.0, 0.0), "key")
            st.playhead = 0.0
            st.selection = setOf(l)
            l
        }
        fun keyTimes() = layer(id).obj("transform").obj("position").arr("k").objects().map { it.optDouble("t") }
        tap("Keyframes")
        val graph = node("Graph editor for transform.position")
        graph.performScrollTo()
        compose.waitForIdle()
        // The graph spans the layer's 0–4 s; drag the key at 2 s (x = w/2) to 3 s (x = 3w/4).
        graph.performTouchInput { drag(Offset(width * 0.5f, height - 20f), Offset(width * 0.75f, height - 20f), steps = 32) }
        poll("keyframe moved to 3 s", detail = { "keys=${keyTimes()}" }) { keyTimes().size == 2 && Math.abs(keyTimes()[1] - 3.0) < 0.05 }

        // Drag the first bezier handle (playhead is on the key at 0 s) → custom bezier easing on that key.
        val bez = node("Bezier easing editor")
        bez.performScrollTo()
        compose.waitForIdle()
        bez.performTouchInput {
            // Handles start at the current easing's control points; grab near the lower-left one and pull up-right.
            drag(Offset(width * 0.3f, height * 0.7f), Offset(width * 0.55f, height * 0.25f), steps = 10)
        }
        poll("bezier easing set", detail = { layer(id).obj("transform").obj("position").arr("k").getJSONObject(0).opt("o").toString() }) {
            layer(id).obj("transform").obj("position").arr("k").getJSONObject(0).optJSONObject("o")?.optString("type") == "bezier"
        }

        // Both gesture edits persist.
        compose.runOnIdle { assertTrue(st.save()); app.closeEditor(); assertEquals(null, app.openProject(projectId)) }
        waitDesc("Undo")
        assertEquals(3.0, keyTimes()[1], 0.05)
        assertEquals("bezier", layer(id).obj("transform").obj("position").arr("k").getJSONObject(0).getJSONObject("o").optString("type"))
    }
}
