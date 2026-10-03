package com.motionforge.app

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.KeyEvent
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Snackbar
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.CompositionLocalProvider
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.ProcessLifecycleOwner
import com.motionforge.app.engine.EditorState
import com.motionforge.app.engine.UiMessage
import com.motionforge.app.export.ExportQueue
import com.motionforge.app.playback.Player
import com.motionforge.app.ui.CommandPalette
import com.motionforge.app.ui.DevCenterScreen
import com.motionforge.app.ui.EditorScreen
import com.motionforge.app.ui.ExportScreen
import com.motionforge.app.ui.ExtensionsScreen
import com.motionforge.app.ui.FirstRunScreen
import com.motionforge.app.ui.FontsScreen
import com.motionforge.app.ui.HomeScreen
import com.motionforge.app.ui.LibraryScreen
import com.motionforge.app.ui.MfTheme
import com.motionforge.app.ui.ModelsScreen
import com.motionforge.app.ui.PerformanceScreen
import com.motionforge.app.ui.ProjectInspectorScreen
import com.motionforge.app.ui.ScriptStudioScreen
import com.motionforge.app.ui.SettingsScreen
import com.motionforge.app.ui.CaptionStudioScreen
import com.motionforge.app.ui.KeyboardShortcuts

sealed class Screen {
    data object Home : Screen()
    data object FirstRun : Screen()
    data object Editor : Screen()
    data object Export : Screen()
    data object Captions : Screen()
    data object Scripts : Screen()
    data object Extensions : Screen()
    data object Library : Screen()
    data object Fonts : Screen()
    data object Models : Screen()
    data object Settings : Screen()
    data object DevCenter : Screen()
    data object Inspector : Screen()
    data object Performance : Screen()
    data object Media : Screen()
}

/** App-wide navigation and shared state. */
class AppState {
    val stack = mutableStateListOf<Screen>(Screen.Home)
    val current: Screen get() = stack.last()
    val editor = EditorState()
    var player: Player? = null
    var paletteOpen by mutableStateOf(false)
    var pendingImport by mutableStateOf<Uri?>(null)

    fun go(s: Screen) { if (current != s) stack.add(s) }
    fun back(): Boolean {
        if (stack.size <= 1) return false
        val left = stack.removeAt(stack.size - 1)
        if (left == Screen.Editor) closeEditor()
        return true
    }

    fun openProject(id: String, recover: Boolean = false): String? {
        val err = editor.open(id, recover)
        if (err == null) {
            if (player == null) player = Player(editor)
            stack.removeAll { it != Screen.Home }
            stack.add(Screen.Editor)
            player?.requestRender()
        }
        return err
    }

    fun closeEditor() {
        player?.pause()
        editor.close()
        stack.removeAll { it != Screen.Home }
    }
}

class MainActivity : ComponentActivity() {
    val app = AppState()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ExportQueue.init(this)
        ExportQueue.openProjectId = { app.editor.projectId }
        handleIntent(intent)
        // Save when the app goes to background (OS may kill us afterwards).
        ProcessLifecycleOwner.get().lifecycle.addObserver(LifecycleEventObserver { _, e ->
            if (e == Lifecycle.Event.ON_STOP && app.editor.projectId.isNotEmpty()) { app.player?.pause(); app.editor.save() }
        })
        if (!Settings.firstRunDone) app.stack[0] = Screen.FirstRun
        // Draw behind the system bars (default on Android 15) with matching dark bars; insets are applied at the root.
        enableEdgeToEdge(
            statusBarStyle = androidx.activity.SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
            navigationBarStyle = androidx.activity.SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
        )
        setContent {
            val scale = Settings.uiScale
            val d = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(d.density * scale, d.fontScale)) {
                MfTheme { Root(app) }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(i: Intent?) {
        if (i?.action == Intent.ACTION_VIEW && i.data != null) app.pendingImport = i.data
        if (i?.getBooleanExtra("openExports", false) == true && app.editor.projectId.isNotEmpty()) app.go(Screen.Export)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN && app.current == Screen.Editor && KeyboardShortcuts.handle(app, event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun onDestroy() {
        if (isFinishing && app.editor.projectId.isNotEmpty()) app.closeEditor()
        super.onDestroy()
    }
}

@Composable
fun Root(app: AppState) {
    BackHandler(enabled = app.stack.size > 1 || app.paletteOpen) {
        if (app.paletteOpen) app.paletteOpen = false else app.back()
    }
    Box(Modifier.fillMaxSize().background(com.motionforge.app.ui.Bg).safeDrawingPadding()) {
        when (app.current) {
            Screen.Home -> HomeScreen(app)
            Screen.FirstRun -> FirstRunScreen(app)
            Screen.Editor -> EditorScreen(app)
            Screen.Export -> ExportScreen(app)
            Screen.Captions -> CaptionStudioScreen(app)
            Screen.Scripts -> ScriptStudioScreen(app)
            Screen.Extensions -> ExtensionsScreen(app)
            Screen.Library -> LibraryScreen(app)
            Screen.Fonts -> FontsScreen(app)
            Screen.Models -> ModelsScreen(app)
            Screen.Settings -> SettingsScreen(app)
            Screen.DevCenter -> DevCenterScreen(app)
            Screen.Inspector -> ProjectInspectorScreen(app)
            Screen.Performance -> PerformanceScreen(app)
            Screen.Media -> com.motionforge.app.ui.MediaManagerScreen(app)
        }
        if (app.paletteOpen) CommandPalette(app)
        val msg = app.editor.message
        if (msg != null) {
            LaunchedEffect(msg) { kotlinx.coroutines.delay(if (msg.isError) 6000 else 2500); if (app.editor.message == msg) app.editor.message = null }
            Snackbar(
                modifier = Modifier.align(Alignment.BottomCenter).padding(12.dp),
                containerColor = if (msg.isError) Color(0xFF5A1E1E) else Color(0xFF2A2C33),
                action = if (msg.action != null) ({ TextButton(onClick = { msg.onAction?.invoke(); app.editor.message = null }) { Text(msg.action) } }) else null,
                dismissAction = { TextButton(onClick = { app.editor.message = null }) { Text("Dismiss") } },
            ) { Text(msg.text) }
        }
    }
}

fun AppState.toast(text: String, error: Boolean = false) { editor.message = UiMessage(text, error) }
