package com.motionforge.app

import androidx.compose.ui.test.ExperimentalTestApi
import androidx.compose.ui.test.hasContentDescription
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTextReplacement
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.uiautomator.UiDevice
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.arr
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.objects
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/** Drives the real UI like a user: create project, add text, undo/redo, play/pause, frame step, save & reopen. */
@OptIn(ExperimentalTestApi::class)
@RunWith(AndroidJUnit4::class)
class UiE2ETest {
    @get:Rule val compose = createEmptyComposeRule()
    @get:Rule val artifacts = FailureArtifacts { scenario?.close(); cleanup() }
    private var scenario: ActivityScenario<MainActivity>? = null
    private val name = "UI Test ${System.currentTimeMillis() % 100000}"
    private val timeout = 60_000L

    @Before
    fun launch() {
        Settings.firstRunDone = true
        scenario = ActivityScenario.launch(MainActivity::class.java)
    }

    /** Runs after the activity is closed (and after failure capture); see FailureArtifacts. */
    private fun cleanup() {
        NativeBridge.call("listProjects").arr("projects").objects().filter { it.optString("name") == name }.forEach {
            NativeBridge.call("deleteProject", jo("id" to it.optString("id")))
        }
    }

    private fun waitDesc(prefix: String) = compose.waitUntil(timeout) {
        // Tolerate a momentary absence of the window (e.g. System UI restarting on a slow emulator).
        try { compose.onAllNodes(hasContentDescription(prefix, substring = true)).fetchSemanticsNodes().isNotEmpty() } catch (e: IllegalStateException) { false }
    }

    private fun exists(desc: String) =
        try { compose.onAllNodes(hasContentDescription(desc, substring = true)).fetchSemanticsNodes().isNotEmpty() } catch (e: IllegalStateException) { false }

    private fun poll(cond: () -> Boolean) {
        val end = System.currentTimeMillis() + timeout
        while (!cond()) {
            if (System.currentTimeMillis() > end) throw AssertionError("condition not met within $timeout ms")
            Thread.sleep(100)
        }
    }

    private fun tapDesc(device: UiDevice, desc: String) {
        val o = device.wait(androidx.test.uiautomator.Until.findObject(androidx.test.uiautomator.By.desc(desc)), timeout)
            ?: throw AssertionError("no node with description '$desc'")
        o.click()
    }

    private fun pressUntil(device: UiDevice, key: Int, cond: () -> Boolean) {
        repeat(3) {
            device.pressKeyCode(key)
            val end = System.currentTimeMillis() + 20_000
            while (System.currentTimeMillis() < end) { if (cond()) return; Thread.sleep(100) }
        }
        throw AssertionError("key $key had no effect after 3 presses")
    }

    private fun waitText(t: String) = compose.waitUntil(timeout) {
        try { compose.onAllNodesWithText(t, substring = true).fetchSemanticsNodes().isNotEmpty() } catch (e: IllegalStateException) { false }
    }

    @Test
    fun createEditUndoPlaySave() {
        // Home → New Project.
        waitDesc("New Project")
        compose.onNodeWithContentDescription("New Project").performClick()
        waitText("Create")
        compose.onNode(hasText("My Project")).performTextReplacement(name)
        // Typing raises the soft keyboard, which resizes the dialog; let the layout settle before tapping.
        compose.waitForIdle()
        compose.onNodeWithText("Create").performClick()
        // If the keyboard animation moved the button under the tap, tap once more (the dialog is still open).
        val opened = try { compose.waitUntil(20_000) { exists("Undo (nothing to undo)") }; true } catch (e: Throwable) { false }
        if (!opened && compose.onAllNodesWithText("Create").fetchSemanticsNodes().isNotEmpty()) compose.onNodeWithText("Create").performClick()

        // Editor opens with nothing to undo.
        waitDesc("Undo (nothing to undo)")

        // Add a text layer through the Add panel.
        compose.onNodeWithText("+ Layer").performClick()
        waitText("Rectangle")
        compose.onAllNodesWithText("Text")[0].performClick()
        var appRef: AppState? = null
        scenario!!.onActivity { act -> appRef = act.app }
        val app = appRef!!
        compose.waitUntil(timeout) { app.editor.layers.size == 1 }
        assertEquals("text", app.editor.layers[0].optString("type"))

        // Undo removes it, redo restores it.
        compose.onNode(hasContentDescription("Undo", substring = true)).performClick()
        compose.waitUntil(timeout) { app.editor.layers.isEmpty() }
        compose.onNode(hasContentDescription("Redo", substring = true)).performClick()
        compose.waitUntil(timeout) { app.editor.layers.size == 1 }

        // Frame stepping moves the playhead by exactly one frame.
        val before = app.editor.playhead
        compose.onNodeWithContentDescription("Next frame").performClick()
        compose.waitUntil(timeout) { Math.abs(app.editor.playhead - before - 1.0 / app.editor.fps) < 1e-6 }

        // Play then pause; the playhead advances while playing. Compose is never idle during playback
        // (a new frame every tick), so this phase polls app state and pauses with a key press.
        val t0 = app.editor.playhead
        compose.onNodeWithContentDescription("Play").performClick()
        poll { app.player?.playing == true && app.editor.playhead > t0 }
        // Pause by tapping the Pause button through UiAutomator (accessibility), which works while Compose is busy.
        val device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation())
        tapDesc(device, "Pause")
        poll { app.player?.playing == false }
        waitDesc("Play")

        // Space toggles playback (hardware keyboard path through MainActivity.dispatchKeyEvent). Key injection goes
        // to whichever window has input focus, which a slow emulator can briefly hand to System UI, so retry the press.
        pressUntil(device, android.view.KeyEvent.KEYCODE_SPACE) { app.player?.playing == true }
        pressUntil(device, android.view.KeyEvent.KEYCODE_SPACE) { app.player?.playing == false }
        waitDesc("Play")

        // Save via menu, go back home, reopen: the layer persists.
        compose.onNodeWithContentDescription("More").performClick()
        compose.onNodeWithText("Save").performClick()
        compose.waitUntil(timeout) { app.editor.saveStatus == "saved" }
        compose.onNodeWithContentDescription("Back to projects").performClick()
        waitDesc("Project $name")
        compose.onNodeWithContentDescription("Project $name").performClick()
        waitDesc("Undo")
        compose.waitUntil(timeout) { app.editor.layers.size == 1 }
        assertTrue(app.editor.layers[0].optString("type") == "text")
    }
}
