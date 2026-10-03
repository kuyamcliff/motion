package com.motionforge.app

import androidx.compose.ui.test.SemanticsNodeInteraction
import androidx.compose.ui.test.hasContentDescription
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createEmptyComposeRule
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onFirst
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.test.performTextInput
import androidx.compose.ui.test.performTextReplacement
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.arr
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.obj
import com.motionforge.app.engine.objects
import com.motionforge.app.export.ExportQueue
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Clicks through every screen and inspector panel like a user: open it, interact, then verify the result in the
 * engine document. Editing flows also save, return to Home, reopen the project and verify the change persisted.
 */
@RunWith(AndroidJUnit4::class)
class ScreensE2ETest {
    @get:Rule val compose = createEmptyComposeRule()
    @get:Rule val artifacts = FailureArtifacts { if (::scenario.isInitialized) scenario.close(); cleanup() }
    @org.junit.Before fun bundledAssets() { org.junit.Assert.assertTrue("bundled model/samples installed", MfApplication.awaitBundledAssets()) }
    private lateinit var scenario: ActivityScenario<MainActivity>
    private lateinit var app: AppState
    private val name = "Screens ${System.nanoTime() % 1_000_000}"
    private var projectId = ""
    private val timeout = 90_000L
    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Before
    fun launch() {
        Settings.firstRunDone = true
        scenario = ActivityScenario.launch(MainActivity::class.java)
        scenario.onActivity { app = it.app }
    }

    /** Runs after the activity is closed (and after failure capture); see FailureArtifacts. */
    private fun cleanup() {
        if (projectId.isNotEmpty()) NativeBridge.call("deleteProject", jo("id" to projectId))
    }

    // ------------------------------------------------------------ helpers
    private fun nodesExist(m: androidx.compose.ui.test.SemanticsMatcher) =
        try { compose.onAllNodes(m).fetchSemanticsNodes().isNotEmpty() } catch (e: IllegalStateException) { false }

    private fun waitFor(m: androidx.compose.ui.test.SemanticsMatcher) = compose.waitUntil(timeout) { nodesExist(m) }
    private fun waitText(t: String) = waitFor(hasText(t))
    private fun waitDesc(d: String) = waitFor(hasContentDescription(d, substring = true))
    private fun poll(what: String, ms: Long = timeout, detail: () -> String = { "" }, cond: () -> Boolean) {
        try { compose.waitUntil(ms) { try { cond() } catch (e: Exception) { false } } }
        catch (e: Throwable) { throw AssertionError("$what: not reached within $ms ms. ${detail()}", e) }
    }

    private fun SemanticsNodeInteraction.scrollClick() {
        try { performScrollTo() } catch (_: Throwable) { }
        performClick()
    }

    /** Taps the first node with exactly this text (scrolling it into view first). */
    private fun tap(text: String) { waitText(text); compose.onAllNodesWithText(text).onFirst().scrollClick() }
    private fun tapDesc(desc: String) {
        waitDesc(desc)
        compose.onAllNodes(hasContentDescription(desc, substring = true)).onFirst().scrollClick()
    }

    private fun newProject(): String {
        val r = NativeBridge.call("createProject", jo("name" to name, "width" to 640, "height" to 360, "fps" to 24.0, "duration" to 4.0))
        projectId = r.getString("id")
        compose.runOnIdle { assertEquals(null, app.openProject(projectId)) }
        waitDesc("Undo")
        return projectId
    }

    private fun layers() = app.editor.layers
    private fun layer(id: String) = app.editor.layer(id)!!

    /** Saves via the menu, goes back to Home, reopens the project from the grid. */
    private fun saveAndReopen() {
        tapDesc("More")
        tap("Save")
        poll("saved") { app.editor.saveStatus == "saved" }
        tapDesc("Back to projects")
        waitDesc("Project $name")
        tapDesc("Project $name")
        waitDesc("Undo")
        poll("project reopened") { app.editor.projectId == projectId }
    }

    private fun selectFirst(type: String) {
        compose.runOnIdle { app.editor.selection = setOf(layers().first { it.optString("type") == type }.optString("id")) }
    }

    // ------------------------------------------------------------ inspector panels
    @Test
    fun textPanel_presetShadowAndPersist() {
        newProject()
        tap("Text")  // context toolbar: adds a text layer and opens the Text panel
        poll("text layer added") { layers().size == 1 }
        tap("Fade Up")
        poll("animator added") { layers()[0].obj("text").arr("animators").length() == 1 }
        tapDesc("Drop shadow")
        poll("shadow on") { layers()[0].obj("text").obj("shadow").optBoolean("enabled") }
        saveAndReopen()
        val t = layers()[0].obj("text")
        assertEquals(1, t.arr("animators").length())
        assertTrue(t.obj("shadow").optBoolean("enabled"))
    }

    @Test
    fun shapePanel_itemsAndPathOperators() {
        newProject()
        tap("Shape")
        poll("shape added") { layers().size == 1 }
        tap("+ Star")
        poll("second item") { layers()[0].obj("shape").arr("items").length() == 2 }
        tapDesc("Trim paths")
        poll("trim on") { layers()[0].obj("shape").obj("trim").optBoolean("enabled") }
        tapDesc("Repeater")
        poll("repeater on") { layers()[0].obj("shape").obj("repeater").optBoolean("enabled") }
        saveAndReopen()
        assertEquals(2, layers()[0].obj("shape").arr("items").length())
        assertTrue(layers()[0].obj("shape").obj("repeater").optBoolean("enabled"))
    }

    @Test
    fun effectsPanel_browserAddBypassPreset() {
        newProject()
        compose.runOnIdle { app.editor.addLayer("solid") }
        tap("Effects")
        tap("+ Add effect")
        compose.onNode(hasSetTextAction()).performTextInput("glow")
        tap("Glow")
        poll("glow added") { layers()[0].arr("effects").objects().any { it.optString("type") == "stylize.glow" } }
        tap("Bypass all")
        poll("bypassed") { layers()[0].arr("effects").objects().all { !it.optBoolean("enabled", true) } }
        saveAndReopen()
        assertEquals("stylize.glow", layers()[0].arr("effects").objects()[0].optString("type"))
    }

    @Test
    fun keyframesPanel_graphEditorEasingExpression() {
        newProject()
        compose.runOnIdle { app.editor.addLayer("shape", jo("shape" to "ellipse")) }
        tap("Keyframes")
        waitDesc("Graph editor for transform.position")
        tap("Add key at playhead")
        poll("keyframe") { (layers()[0].obj("transform").obj("position").arr("k").length()) == 1 }
        tap("bounce")
        poll("easing set") { layers()[0].obj("transform").obj("position").arr("k").getJSONObject(0).optString("o") == "bounce" }
        tap("wiggle(2, 30)")
        tap("Apply")
        poll("expression") { layers()[0].obj("transform").obj("position").optString("x") == "wiggle(2, 30)" }
        saveAndReopen()
        val p = layers()[0].obj("transform").obj("position")
        assertEquals("wiggle(2, 30)", p.optString("x"))
        assertEquals("bounce", p.arr("k").getJSONObject(0).optString("o"))
    }

    @Test
    fun masksTransitionsBehaviors() {
        newProject()
        compose.runOnIdle { app.editor.addLayer("solid") }
        tap("Masks")
        tap("+ Ellipse")
        poll("mask") { layers()[0].arr("masks").length() == 1 }
        tapDesc("Inverted")
        poll("mask inverted") { layers()[0].arr("masks").getJSONObject(0).optBoolean("inverted") }
        tap("Transitions")
        tap("Linear Wipe")
        poll("transition") { layers()[0].optJSONObject("transitionIn")?.optString("type") == "wipe" }
        tap("Behaviors")
        tap("Shake")
        poll("behavior") { layers()[0].arr("behaviors").length() == 1 }
        tap("Bake to keyframes")
        poll("baked") { layers()[0].arr("behaviors").length() == 0 || layers()[0].obj("transform").obj("position").has("k") }
        saveAndReopen()
        assertEquals(1, layers()[0].arr("masks").length())
        assertEquals("wipe", layers()[0].getJSONObject("transitionIn").optString("type"))
    }

    @Test
    fun addPanel_3dCameraLightParticles() {
        newProject()
        tap("+ Layer")
        tap("3D cube")
        poll("model") { layers().any { it.optString("type") == "model3d" } }
        tap("3D")
        tapDesc("Wireframe")
        poll("wireframe") { layers().first { it.optString("type") == "model3d" }.obj("model").optBoolean("wireframe") }
        compose.runOnIdle { app.editor.selection = emptySet() }
        tap("+ Layer")
        tap("Camera")
        poll("camera") { layers().any { it.optString("type") == "camera" } }
        tap("Push in")
        poll("camera animated") { layers().first { it.optString("type") == "camera" }.obj("transform").obj("position").has("k") }
        compose.runOnIdle { app.editor.selection = emptySet() }
        tap("+ Layer")
        tap("Snow")
        poll("particles") { layers().any { it.optString("type") == "particles" } }
        tapDesc("Glow")
        poll("particle glow") { layers().first { it.optString("type") == "particles" }.obj("particles").optBoolean("glow") }
        saveAndReopen()
        assertTrue(layers().first { it.optString("type") == "model3d" }.obj("model").optBoolean("wireframe"))
        assertTrue(layers().any { it.optString("type") == "camera" })
    }

    @Test
    fun timePanelAndCommandPalette() {
        newProject()
        compose.runOnIdle { app.editor.addLayer("solid") }
        tapDesc("Command palette")
        compose.onNode(hasContentDescription("Command search")).performTextInput("Add null")
        // The search field also contains "Add null": tap the command row, not the field.
        waitFor(hasText("Add null") and !hasSetTextAction())
        compose.onAllNodes(hasText("Add null") and !hasSetTextAction()).onFirst().performClick()
        poll("null via palette") { layers().any { it.optString("type") == "null" } }
        selectFirst("solid")
        compose.runOnIdle { app.editor.playhead = 1.0 }
        tap("Split")
        poll("split") { layers().count { it.optString("type") == "solid" } == 2 }
    }

    // ------------------------------------------------------------ screens
    @Test
    fun captionStudio_stylesReplaceEditExport() {
        newProject()
        compose.runOnIdle {
            app.editor.op("setCaptions", "items" to listOf(jo("start" to 0.2, "end" to 1.4, "text" to "hello world"), jo("start" to 1.6, "end" to 2.8, "text" to "second line")))
        }
        tapDesc("More")
        tap("Caption Studio")
        waitText("Caption Studio")
        tap("Boxed")
        poll("boxed style") { layers().first { it.optString("type") == "captions" }.obj("captions").obj("style").optBoolean("background") }
        tapDesc("Uppercase")
        poll("uppercase") { layers().first { it.optString("type") == "captions" }.obj("captions").obj("style").optBoolean("uppercase") }
        compose.onNode(hasText("Find") and hasSetTextAction()).performTextInput("world")
        compose.onNode(hasText("Replace") and hasSetTextAction()).performTextInput("there")
        tap("Replace all")
        poll("replaced") { layers().first { it.optString("type") == "captions" }.obj("captions").arr("items").getJSONObject(0).optString("text") == "hello there" }
        val srt = NativeBridge.call("exportSubtitles", jo("format" to "srt")).optString("text")
        assertTrue(srt, srt.contains("hello there"))
        compose.runOnIdle { app.back() }
        saveAndReopen()
        val cap = layers().first { it.optString("type") == "captions" }.obj("captions")
        assertTrue(cap.obj("style").optBoolean("background"))
        assertEquals("hello there", cap.arr("items").getJSONObject(0).optString("text"))
    }

    @Test
    fun scriptStudio_runExampleIsOneUndoStep() {
        newProject()
        tapDesc("More")
        tap("Script Studio")
        tap("Title Generator")
        tapDesc("Run script")
        // Permission confirmation dialog: its "Run" button is the last node with that text.
        waitFor(hasText("Allow script to modify the project?"))
        compose.onAllNodesWithText("Run").let { it[it.fetchSemanticsNodes().size - 1] }.performClick()
        waitFor(hasText("Finished", substring = true))
        poll("script added a layer") { layers().size == 1 }
        compose.runOnIdle { app.back() }
        tapDesc("Undo Run Script")
        poll("one undo removes it") { layers().isEmpty() }
    }

    @Test
    fun exportScreen_gifAndMp4ThroughQueue() {
        newProject()
        compose.runOnIdle { app.editor.addLayer("text", jo("text" to "Export me")) }
        tapDesc("Export")
        tap("GIF")
        tap("360p")
        compose.onNode(hasText(name) and hasSetTextAction()).performTextReplacement("ui-gif-$projectId")
        tapDesc("Start export")
        // Exports render every frame; a software-emulated device needs minutes, a phone seconds.
        poll("gif done", 15 * 60_000L, { ExportQueue.jobs.joinToString { "${it.name}:${it.status}:${(it.progress * 100).toInt()}%:${it.error}" } }) {
            ExportQueue.jobs.any { it.name == "ui-gif-$projectId" && it.status in setOf("done", "failed") }
        }
        val gif = ExportQueue.jobs.first { it.name == "ui-gif-$projectId" }
        assertEquals(gif.error, "done", gif.status)
        assertTrue(File(gif.outPath).length() > 1000)
        tap("MP4 video")
        compose.onNode(hasText("ui-gif-$projectId") and hasSetTextAction()).performTextReplacement("ui-mp4-$projectId")
        tapDesc("Start export")
        poll("mp4 done", 15 * 60_000L, { ExportQueue.jobs.joinToString { "${it.name}:${it.status}:${(it.progress * 100).toInt()}%:${it.error}" } }) {
            ExportQueue.jobs.any { it.name == "ui-mp4-$projectId" && it.status in setOf("done", "failed") }
        }
        val mp4 = ExportQueue.jobs.first { it.name == "ui-mp4-$projectId" }
        assertEquals(mp4.error, "done", mp4.status)
        assertTrue(mp4.result.obj("validation").optBoolean("ok"))
        waitFor(hasText("Validated", substring = true))
    }

    @Test
    fun projectInspectorAndPerformance() {
        newProject()
        compose.runOnIdle { app.editor.addLayer("text", jo("text" to "Inspect")) }
        tapDesc("More")
        tap("Project inspector")
        waitText("HEALTH")
        waitFor(hasText("No problems found", substring = true))
        tap("Save version now")
        poll("version saved") { NativeBridge.call("versions", jo("id" to projectId)).arr("versions").length() > 0 }
        compose.runOnIdle { app.back() }
        tapDesc("More")
        tap("Performance monitor")
        waitFor(hasText("CPU cores", substring = true))
    }

    @Test
    fun homeScreens_fontsModelsSettingsDevCenter() {
        Settings.favoriteFonts = emptySet()  // state persists across runs; start from a known state
        waitDesc("Fonts")
        tapDesc("Fonts")
        waitText("DejaVuSans")
        tapDesc("Favorite DejaVuSans")
        poll("font favorited") { "DejaVuSans" in Settings.favoriteFonts || Settings.favoriteFonts.any { it.startsWith("DejaVuSans") } }
        compose.runOnIdle { app.back() }

        tapDesc("Settings")
        val before = Settings.snapping
        tapDesc("Timeline snapping")
        poll("snapping toggled") { Settings.snapping != before }
        tapDesc("Timeline snapping")
        tap("AI Models (Whisper)")
        waitFor(hasText("(active)", substring = true))
        waitFor(hasText("English-only", substring = true))
        compose.runOnIdle { app.back(); app.back() }

        tapDesc("Documentation and developer center")
        tap("Operations")
        waitFor(hasText("addLayer", substring = true))
        tap("Scenarios")
        tap("Run scenario")
        waitFor(hasText("PASS", substring = true))
    }

    @Test
    fun library_presetFavoriteDuplicateDelete() {
        newProject()
        lateinit var file: String
        compose.runOnIdle {
            val id = app.editor.addLayer("solid")!!
            app.editor.op("addEffect", "layer" to id, "type" to "color.tint")
            val r = NativeBridge.call("savePreset", jo("layer" to id, "name" to "UITestPreset", "include" to jo("effects" to true)))
            assertTrue(r.toString(), r.optBoolean("ok"))
            file = r.optString("file")
        }
        tapDesc("Back to projects")
        tapDesc("Presets and capsules library")
        waitText("UITestPreset")
        tapDesc("Favorite UITestPreset")
        poll("favorited") { NativeBridge.call("presets").arr("presets").objects().first { it.optString("_file") == file }.optBoolean("favorite") }
        tap("Duplicate")
        poll("duplicated") { NativeBridge.call("presets").arr("presets").objects().any { it.optString("name") == "UITestPreset copy" } }
        tapDesc("Delete UITestPreset copy")
        tapDesc("Delete UITestPreset")
        poll("deleted") { NativeBridge.call("presets").arr("presets").objects().none { it.optString("name").startsWith("UITestPreset") } }
    }

    @Test
    fun extensions_installEnableUseUninstall() {
        // Build an unsigned composite-effect extension package and install it in developer mode.
        val pkg = File(ctx.cacheDir, "uitest.mfext").apply { delete() }
        val content = jo("effects" to listOf(jo("id" to "warmglow", "name" to "Warm Glow (UI test)", "chain" to listOf(jo("type" to "color.tint"), jo("type" to "stylize.glow")))))
        val manifest = jo("id" to "uitest.ext", "name" to "UI Test Extension", "version" to "1.0", "author" to "tests", "apiVersion" to 1, "permissions" to emptyList<String>())
        assertTrue(NativeBridge.call("exportJsonPackage", jo("path" to pkg.absolutePath, "kind" to "extension", "content" to content, "manifest" to manifest)).optBoolean("ok"))
        val inst = NativeBridge.call("installExtension", jo("path" to pkg.absolutePath, "confirmed" to true, "devMode" to true))
        assertTrue(inst.toString(), inst.optBoolean("ok"))
        compose.runOnIdle { app.editor.reloadRegistries() }
        try {
            // The extension's effect is offered in the editor's effect browser and can be applied.
            newProject()
            compose.runOnIdle { app.editor.addLayer("solid") }
            tap("Effects")
            tap("+ Add effect")
            tap("Extensions")
            tap("Warm Glow (UI test)")
            poll("extension effect applied") { layers()[0].arr("effects").objects().any { it.optString("type") == "ext.uitest.ext.warmglow" } }
            tapDesc("Back to projects")
            tapDesc("Extensions")
            waitText("UI Test Extension")
            tapDesc("Enabled")
            poll("disabled") { NativeBridge.call("extensions").arr("extensions").objects().first { it.optString("_id") == "uitest.ext" }.optBoolean("_enabled").not() }
            assertFalse(app.editor.registries.arr("effects").objects().any { it.optString("type") == "ext.uitest.ext.warmglow" })
            tap("Uninstall")
            poll("uninstalled") { NativeBridge.call("extensions").arr("extensions").objects().none { it.optString("_id") == "uitest.ext" } }
        } finally {
            NativeBridge.call("uninstallExtension", jo("id" to "uitest.ext"))
        }
    }

    @Test
    fun mediaManager_relinkCollectRemoveUnused() {
        newProject()
        val wav = File(ctx.filesDir, "samples/speech.wav")
        // Two imports: one used by a layer, one left unused.
        compose.runOnIdle {
            com.motionforge.app.ui.importIntoProject(app, ctx, android.net.Uri.fromFile(wav))
            val unusedAsset = com.motionforge.app.media.Importer.probe(ctx, android.net.Uri.fromFile(wav), com.motionforge.app.media.Importer.Kind.AUDIO)
            app.editor.op("addAsset", "asset" to unusedAsset.put("name", "spare.wav"))
        }
        poll("two assets") { app.editor.doc.arr("assets").length() == 2 }
        tapDesc("More")
        tap("Media manager")
        waitFor(hasText("Used by 1 layer", substring = true))
        waitFor(hasText("Unused", substring = true))
        tap("Remove unused")
        poll("unused removed") { app.editor.doc.arr("assets").length() == 1 }
        // Relink the remaining asset to a copy of the file (explicit replace keeps the layer and its edits).
        val copy = File(ctx.cacheDir, "speech-copy.wav").apply { wav.copyTo(this, overwrite = true) }
        val aid = app.editor.doc.arr("assets").getJSONObject(0).getString("id")
        compose.runOnIdle { assertTrue(com.motionforge.app.ui.MediaOps.relink(ctx, app.editor, aid, android.net.Uri.fromFile(copy))) }
        poll("relinked") { app.editor.asset(aid)!!.optString("uri").contains("speech-copy") }
        tap("Collect into project")
        poll("collected") { app.editor.asset(aid)!!.optString("path").startsWith(ctx.filesDir.absolutePath) }
        assertTrue(File(app.editor.asset(aid)!!.optString("path")).length() == wav.length())
        // Undo restores the pre-collect link in one step.
        compose.runOnIdle { app.editor.undo() }
        poll("undo collect") { !app.editor.asset(aid)!!.optString("path").startsWith(ctx.filesDir.absolutePath) }
    }
}
