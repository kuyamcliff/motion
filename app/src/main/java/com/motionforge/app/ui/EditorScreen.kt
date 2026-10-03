package com.motionforge.app.ui

import androidx.compose.material.icons.filled.Close
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.material.icons.filled.MoreHoriz
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.material.icons.filled.Deselect
import androidx.compose.material.icons.filled.PauseCircle
import androidx.compose.material.icons.filled.Inventory2
import androidx.compose.material.icons.filled.Waves
import androidx.compose.material.icons.filled.SwapHoriz
import androidx.compose.material.icons.filled.Grain
import androidx.compose.material.icons.filled.ViewInAr
import androidx.compose.material.icons.filled.GpsFixed
import androidx.compose.material.icons.filled.Speed
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material.icons.filled.Masks
import androidx.compose.material.icons.filled.AutoAwesome
import androidx.compose.material.icons.filled.DeleteSweep
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.Bookmark
import androidx.compose.material.icons.filled.FormatAlignLeft
import androidx.compose.material.icons.filled.ContentCut
import androidx.compose.material.icons.filled.Gesture
import androidx.compose.material.icons.filled.ClosedCaption
import androidx.compose.material.icons.filled.Category
import androidx.compose.material.icons.filled.TextFields
import androidx.compose.material.icons.filled.Layers
import androidx.compose.material.icons.filled.VideoLibrary
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Redo
import androidx.compose.material.icons.automirrored.filled.Undo
import androidx.compose.material.icons.filled.CloudDone
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.ErrorOutline
import androidx.compose.material.icons.filled.FastForward
import androidx.compose.material.icons.filled.FastRewind
import androidx.compose.material.icons.filled.FileUpload
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Repeat
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.NavigateBefore
import androidx.compose.material.icons.filled.NavigateNext
import androidx.compose.material.icons.filled.FirstPage
import androidx.compose.material.icons.filled.LastPage
import androidx.compose.material.icons.filled.Diamond
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.motionforge.app.AppState
import com.motionforge.app.Screen
import com.motionforge.app.engine.EditorState
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.objects
import com.motionforge.app.playback.Player
import com.motionforge.app.toast
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.foundation.gestures.detectTapGestures

enum class InspectorTab { None, Add, Layer, Effects, Masks, Text, Shape, Audio, Time, Transitions, Behaviors, Keyframes, ThreeD, Particles, Captions, Tracking, Comp, History, Markers }

class EditorUi {
    var tab by mutableStateOf(InspectorTab.None)
    var previewMode by mutableStateOf("select")  // select | mask | draw | track | pen
    var showSafe by mutableStateOf(false)
    var showGrid by mutableStateOf(false)
    var showRulers by mutableStateOf(false)
    var showDiagnostics by mutableStateOf(false)
    var checkerboard by mutableStateOf(false)
    var timelineZoom by mutableStateOf(80f)   // px per second
    var expandedTracks by mutableStateOf(false)
    var graphPath by mutableStateOf<String?>(null)
    var historyOpen by mutableStateOf(false)
    var multiSelect by mutableStateOf(false)
}

@Composable
fun EditorScreen(app: AppState) {
    val st = app.editor
    val player = app.player ?: return
    val ui = remember { EditorUi() }
    // Re-render whenever the document or playhead changes (while paused).
    LaunchedEffect(st.revision, st.playhead) { if (!player.playing) player.requestRender() }
    // Periodic autosave tick (engine decides based on interval + dirty state).
    LaunchedEffect(st.projectId) { while (true) { delay(5000); if (!player.playing) st.autosave() } }

    BoxWithConstraints(Modifier.fillMaxSize().background(Bg)) {
        val wide = maxWidth > 700.dp
        val shortHeight = maxHeight < 520.dp
        val fullHeight = maxHeight
        Column(Modifier.fillMaxSize()) {
            EditorTopBar(app, ui)
            if (wide && shortHeight) {
                // Landscape phone: preview + transport on the left; timeline (or the open panel) and tools on the right.
                Row(Modifier.weight(1f)) {
                    Column(Modifier.weight(0.56f).fillMaxHeight()) {
                        PreviewPane(app, ui, Modifier.weight(1f).fillMaxWidth())
                        Transport(st, player, ui)
                    }
                    Column(Modifier.weight(0.44f).fillMaxHeight().background(Panel)) {
                        if (ui.tab != InspectorTab.None) Column(Modifier.weight(1f).fillMaxWidth()) { InspectorPanel(app, ui) }
                        else Timeline(app, ui, Modifier.weight(1f).fillMaxWidth())
                        ContextToolbar(app, ui)
                    }
                }
            } else if (wide) {
                Row(Modifier.weight(1f)) {
                    Column(Modifier.weight(1f)) {
                        PreviewPane(app, ui, Modifier.weight(1f).fillMaxWidth())
                        Transport(st, player, ui)
                        ContextToolbar(app, ui)
                        Timeline(app, ui, Modifier.height(fullHeight * 0.32f).fillMaxWidth())
                    }
                    if (ui.tab != InspectorTab.None) {
                        Column(Modifier.width(360.dp).fillMaxHeight().background(Panel)) { InspectorPanel(app, ui) }
                    }
                }
            } else {
                // Phone layout, in priority order: Preview → Timeline → Properties → Tools (bottom, thumb reach).
                // Opening properties keeps a compact timeline so the edit context never disappears.
                val panelOpen = ui.tab != InspectorTab.None
                PreviewPane(app, ui, Modifier.weight(if (panelOpen) 0.75f else 1f).fillMaxWidth())
                Transport(st, player, ui)
                if (panelOpen) {
                    Timeline(app, ui, Modifier.height(maxOf(fullHeight * 0.16f, 140.dp)).fillMaxWidth())  // ruler + one row + zoom strip
                    Column(Modifier.weight(1f).fillMaxWidth().background(Panel)) { InspectorPanel(app, ui) }
                } else {
                    Timeline(app, ui, Modifier.weight(0.7f).fillMaxWidth())
                }
                ContextToolbar(app, ui)
            }
        }
    }
    if (ui.historyOpen) HistoryDialog(st) { ui.historyOpen = false }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun EditorTopBar(app: AppState, ui: EditorUi) {
    val st = app.editor
    var more by remember { mutableStateOf(false) }
    var renameProject by remember { mutableStateOf(false) }
    // On phone widths seven 48 dp buttons would leave the title a few dp; secondary actions move into the menu there.
    BoxWithConstraints(Modifier.fillMaxWidth()) {
    val compact = maxWidth < 480.dp
    val roomy = maxWidth >= 360.dp
    Row(Modifier.fillMaxWidth().background(Panel).statusBarsPadding().padding(horizontal = 2.dp), verticalAlignment = Alignment.CenterVertically) {
        IconBtn(Icons.AutoMirrored.Filled.ArrowBack, "Back to projects") { app.back() }
        Column(Modifier.weight(1f).combinedClickable(onClick = { renameProject = true })) {
            Text(st.doc.optJSONObject("meta")?.optString("name") ?: "", fontWeight = FontWeight.SemiBold, maxLines = 1, fontSize = 14.sp,
                color = Color.White, overflow = TextOverflow.Ellipsis)
            Row(verticalAlignment = Alignment.CenterVertically) {
                val (icon, label) = when (st.saveStatus) { "saved" -> Icons.Filled.CloudDone to "Saved"; "error" -> Icons.Filled.ErrorOutline to "Save error"; else -> Icons.Filled.Edit to "Unsaved changes" }
                Icon(icon, label, tint = if (st.saveStatus == "error") Color.Red else TextDim, modifier = Modifier.size(14.dp))
                if (!compact) Text(" ${st.comp.optString("name")} · ${st.timecode(st.playhead)}",
                    fontSize = 11.sp, color = TextDim, fontFamily = FontFamily.Monospace, maxLines = 1, softWrap = false, overflow = TextOverflow.Clip)
            }
        }
        // Undo/redo: tap = act, long press = history list.
        Box(Modifier.sizeIn(minWidth = 48.dp, minHeight = 48.dp).combinedClickable(enabled = true, onClick = { if (st.canUndo) st.undo() }, onLongClick = { ui.historyOpen = true })
            .semantics { contentDescription = if (st.canUndo) "Undo ${st.undoLabel}" else "Undo (nothing to undo)" }, contentAlignment = Alignment.Center) {
            Icon(Icons.AutoMirrored.Filled.Undo, null, tint = if (st.canUndo) Color.White else Color.White.copy(alpha = 0.3f))
        }
        Box(Modifier.sizeIn(minWidth = 48.dp, minHeight = 48.dp).combinedClickable(onClick = { if (st.canRedo) st.redo() }, onLongClick = { ui.historyOpen = true })
            .semantics { contentDescription = if (st.canRedo) "Redo ${st.redoLabel}" else "Redo (nothing to redo)" }, contentAlignment = Alignment.Center) {
            Icon(Icons.AutoMirrored.Filled.Redo, null, tint = if (st.canRedo) Color.White else Color.White.copy(alpha = 0.3f))
        }
        if (!compact) {
            IconBtn(Icons.Filled.Search, "Command palette") { app.paletteOpen = true }
            IconBtn(Icons.Filled.Tune, "Composition settings") { ui.tab = InspectorTab.Comp }
        }
        Box(Modifier.padding(horizontal = 4.dp).size(42.dp).background(AccentGradient, androidx.compose.foundation.shape.CircleShape)
            .clickable { app.player?.pause(); app.go(Screen.Export) }.semantics { contentDescription = "Export" }, contentAlignment = Alignment.Center) {
            Icon(Icons.Filled.FileUpload, null, tint = Color.White, modifier = Modifier.size(22.dp))
        }
        Box {
            IconBtn(Icons.Filled.MoreVert, "More") { more = true }
            DropdownMenu(more, { more = false }) {
                val phoneOnly: List<Pair<String, () -> Unit>> =
                    if (compact) listOf("Command palette" to { app.paletteOpen = true }, "Composition settings" to { ui.tab = InspectorTab.Comp }) else emptyList()
                (phoneOnly + listOf(
                    "Save" to { st.save(); app.toast("Saved") },
                    "Save version (checkpoint)" to { st.save(version = true); app.toast("Version saved") },
                    "History" to { ui.historyOpen = true },
                    "Caption Studio" to { app.player?.pause(); app.go(Screen.Captions) },
                    "Script Studio" to { app.player?.pause(); app.go(Screen.Scripts) },
                    "Markers" to { ui.tab = InspectorTab.Markers },
                    "Media manager" to { app.player?.pause(); app.go(Screen.Media) },
                    "Project inspector" to { app.go(Screen.Inspector) },
                    "Performance monitor" to { app.go(Screen.Performance) },
                    "Export project package" to { app.go(Screen.Export) },
                    (if (ui.showSafe) "Hide safe areas" else "Show safe areas") to { ui.showSafe = !ui.showSafe },
                    (if (ui.showGrid) "Hide grid" else "Show grid") to { ui.showGrid = !ui.showGrid },
                    (if (ui.showRulers) "Hide rulers" else "Show rulers") to { ui.showRulers = !ui.showRulers },
                    (if (ui.checkerboard) "Hide transparency grid" else "Show transparency grid") to { ui.checkerboard = !ui.checkerboard },
                    (if (ui.showDiagnostics) "Hide diagnostics overlay" else "Show diagnostics overlay") to { ui.showDiagnostics = !ui.showDiagnostics },
                    "Help & documentation" to { app.go(Screen.DevCenter) },
                )).forEach { (t, f) -> DropdownMenuItem(text = { Text(t) }, onClick = { more = false; f() }) }
            }
        }
    }
    }
    if (renameProject) TextInputDialog("Rename project", st.doc.optJSONObject("meta")?.optString("name") ?: "", onDismiss = { renameProject = false }) {
        st.op("setProjectName", "name" to it)
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun Transport(st: EditorState, player: Player, ui: EditorUi) {
    fun jumpTo(times: List<Double>, forward: Boolean) {
        val t = st.playhead
        val target = if (forward) times.firstOrNull { it > t + 1e-6 } else times.lastOrNull { it < t - 1e-6 }
        if (target != null) player.seek(target)
    }
    var moreOpen by remember { mutableStateOf(false) }
    fun editPoints() = NativeBridge.call("editPoints").optJSONArray("times")?.let { a -> (0 until a.length()).map { a.getDouble(it) } } ?: emptyList()
    // Timecode left, the core transport centred, loop + extras right. Below 400 dp the keyframe jumps move into the
    // menu so everything fits a 320 dp phone without scrolling.
    BoxWithConstraints(Modifier.fillMaxWidth()) {
    val narrow = maxWidth < 400.dp
    Row(Modifier.fillMaxWidth().background(Panel).padding(horizontal = 6.dp, vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            // Hours are dropped for comps under an hour so the timecode fits beside the transport on phones.
            Text(st.timecode(st.playhead).let { if (st.duration < 3600 && it.length > 8) it.substring(3) else it }, fontFamily = FontFamily.Monospace,
                fontSize = 12.sp, maxLines = 1, softWrap = false,
                overflow = androidx.compose.ui.text.style.TextOverflow.Clip)
            Text("${if (player.gpuWindow) "GPU · " else "CPU · "}${if (player.playing) "%.0f fps".format(player.fpsActual) else "%.0f ms".format(player.renderMs)}",
                fontSize = 10.sp, color = if (player.gpuWindow) Color(0xFF6EE7B7) else TextDim, maxLines = 1, softWrap = false)
        }
        if (!narrow) IconBtn(Icons.Filled.Diamond, "Previous keyframe", tint = KeyColor) { jumpTo(st.keyframeTimes(st.selectedLayer), false) }
        RepeatButton(Icons.Filled.NavigateBefore, "Previous frame") { player.stepFrames(-1) }
        PlayButton(player)
        RepeatButton(Icons.Filled.NavigateNext, "Next frame") { player.stepFrames(1) }
        if (!narrow) IconBtn(Icons.Filled.Diamond, "Next keyframe", tint = KeyColor) { jumpTo(st.keyframeTimes(st.selectedLayer), true) }
        Row(Modifier.weight(1f), horizontalArrangement = Arrangement.End, verticalAlignment = Alignment.CenterVertically) {
            IconBtn(Icons.Filled.Repeat, if (player.loop) "Loop on" else "Loop off", tint = if (player.loop) Accent else TextDim) { player.loop = !player.loop }
            Box {
                IconBtn(Icons.Filled.MoreHoriz, "Transport options") { moreOpen = true }
                DropdownMenu(moreOpen, { moreOpen = false }) {
                    ((if (narrow) listOf<Pair<String, () -> Unit>>("Previous keyframe" to { jumpTo(st.keyframeTimes(st.selectedLayer), false) },
                        "Next keyframe" to { jumpTo(st.keyframeTimes(st.selectedLayer), true) }) else emptyList()) + listOf(
                        "Jump to beginning" to { player.seek(0.0) },
                        "Jump to end" to { player.seek(st.duration) },
                        "Previous edit point" to { jumpTo(editPoints(), false) },
                        "Next edit point" to { jumpTo(editPoints(), true) },
                        "Stop and reset" to { player.stop() },
                        "Set range in" to { player.rangeIn = st.playhead },
                        "Set range out" to { player.rangeOut = st.playhead },
                        "Clear range" to { player.rangeIn = -1.0; player.rangeOut = -1.0 },
                    )).forEach { (t, f) -> DropdownMenuItem(text = { Text(t) }, onClick = { moreOpen = false; f() }) }
                }
            }
        }
    }
    }
}

/** Button that repeats while held (frame stepping): tap = one frame, hold = repeat. */
@Composable
fun RepeatButton(icon: androidx.compose.ui.graphics.vector.ImageVector, label: String, onStep: () -> Unit) {
    val scope = androidx.compose.runtime.rememberCoroutineScope()
    Box(Modifier.sizeIn(minWidth = 48.dp, minHeight = 48.dp).semantics { contentDescription = label }
        .androidx_pointerRepeat(scope, onStep), contentAlignment = Alignment.Center) { Icon(icon, null) }
}

fun Modifier.androidx_pointerRepeat(scope: kotlinx.coroutines.CoroutineScope, onStep: () -> Unit): Modifier =
    this.then(Modifier.pointerInput(Unit) {
        detectTapGestures(onPress = {
            onStep()
            val job = scope.launch { delay(400); while (true) { onStep(); delay(70) } }
            tryAwaitRelease()
            job.cancel()
        })
    })

/** Play button: tap toggles play/pause, press-and-hold plays only while held. */
@Composable
fun PlayButton(player: Player) {
    Box(Modifier.sizeIn(minWidth = 56.dp, minHeight = 48.dp).semantics { contentDescription = if (player.playing) "Pause" else "Play" }
        .pointerInput(Unit) {
            detectTapGestures(onTap = { player.toggle() }, onLongPress = { if (!player.playing) { heldMode = true; player.holdStart() } }, onPress = {
                val released = tryAwaitRelease()
                if (released && player.playing && heldMode) player.holdEnd()
                heldMode = false
            })
        }, contentAlignment = Alignment.Center) {
        Box(Modifier.size(46.dp).background(AccentGradient, androidx.compose.foundation.shape.CircleShape), contentAlignment = Alignment.Center) {
            Icon(if (player.playing) Icons.Filled.Pause else Icons.Filled.PlayArrow, null, tint = Color.White, modifier = Modifier.size(28.dp))
        }
    }
}
private var heldMode = false

@Composable
fun ContextToolbar(app: AppState, ui: EditorUi) {
    val st = app.editor
    val ctx = LocalContext.current
    val sel = st.selectedLayer
    val importMedia = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris: List<Uri> ->
        uris.forEach { importIntoProject(app, ctx, it) }
    }
    // Bottom tool bar (thumb reach): icon + label buttons, scrollable on narrow screens.
    @Composable fun T(icon: androidx.compose.ui.graphics.vector.ImageVector, label: String, selected: Boolean = false, onClick: () -> Unit) =
        ToolButton(icon, label, selected, onClick = onClick)
    Row(Modifier.fillMaxWidth().background(Panel).navigationBarsPadding().horizontalScroll(rememberScrollState()).padding(horizontal = 4.dp, vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically) {
        if (sel == null) {
            T(Icons.Filled.VideoLibrary, "+ Media") { importMedia.launch(arrayOf("video/*", "audio/*", "image/*", "*/*")) }
            T(Icons.Filled.Layers, "+ Layer", ui.tab == InspectorTab.Add) { ui.tab = InspectorTab.Add }
            T(Icons.Filled.TextFields, "Text") { st.addLayer("text", jo("text" to "Title")); ui.tab = InspectorTab.Text }
            T(Icons.Filled.Category, "Shape") { st.addLayer("shape", jo("shape" to "rect")); ui.tab = InspectorTab.Shape }
            T(Icons.Filled.ClosedCaption, "Captions") { app.player?.pause(); app.go(Screen.Captions) }
            T(Icons.Filled.Gesture, "Draw", ui.previewMode == "draw") { ui.previewMode = if (ui.previewMode == "draw") "select" else "draw" }
            T(Icons.Filled.ContentCut, "Split all") { st.op("split", "t" to st.playhead) }
            T(Icons.Filled.FormatAlignLeft, "Delete gap") { st.op("deleteGap", "t" to st.playhead) }
            T(Icons.Filled.Bookmark, "Marker") { st.op("addMarker", "t" to st.playhead, "title" to "Marker") }
        } else {
            val id = sel.optString("id")
            val type = sel.optString("type")
            T(Icons.Filled.Tune, "Inspector", ui.tab == InspectorTab.Layer) { ui.tab = InspectorTab.Layer }
            T(Icons.Filled.ContentCut, "Split") { st.op("split", "layers" to listOf(id), "t" to st.playhead) }
            T(Icons.Filled.ContentCopy, "Duplicate") { st.op("duplicateLayers", "layers" to listOf(id)) }
            T(Icons.Filled.Delete, "Delete") { st.op("removeLayers", "layers" to listOf(id)); st.selection = emptySet() }
            T(Icons.Filled.DeleteSweep, "Ripple delete") { st.op("rippleDelete", "layer" to id); st.selection = emptySet() }
            T(Icons.Filled.AutoAwesome, "Effects", ui.tab == InspectorTab.Effects) { ui.tab = InspectorTab.Effects }
            T(Icons.Filled.Diamond, "Keyframes", ui.tab == InspectorTab.Keyframes) { ui.tab = InspectorTab.Keyframes }
            T(Icons.Filled.Masks, "Masks", ui.tab == InspectorTab.Masks) { ui.tab = InspectorTab.Masks }
            if (type == "text") T(Icons.Filled.TextFields, "Text", ui.tab == InspectorTab.Text) { ui.tab = InspectorTab.Text }
            if (type == "shape") T(Icons.Filled.Category, "Shape", ui.tab == InspectorTab.Shape) { ui.tab = InspectorTab.Shape }
            if (sel.has("audio")) T(Icons.Filled.GraphicEq, "Audio", ui.tab == InspectorTab.Audio) { ui.tab = InspectorTab.Audio }
            if (type == "video" || type == "audio" || type == "precomp" || type == "image") T(Icons.Filled.Speed, "Speed", ui.tab == InspectorTab.Time) { ui.tab = InspectorTab.Time }
            if (type == "video") T(Icons.Filled.GpsFixed, "Track", ui.tab == InspectorTab.Tracking) { ui.tab = InspectorTab.Tracking }
            if (type == "model3d" || type == "camera" || type == "light" || sel.optBoolean("threeD")) T(Icons.Filled.ViewInAr, "3D", ui.tab == InspectorTab.ThreeD) { ui.tab = InspectorTab.ThreeD }
            if (type == "particles") T(Icons.Filled.Grain, "Particles", ui.tab == InspectorTab.Particles) { ui.tab = InspectorTab.Particles }
            if (type == "captions") T(Icons.Filled.ClosedCaption, "Captions") { app.go(Screen.Captions) }
            T(Icons.Filled.SwapHoriz, "Transitions", ui.tab == InspectorTab.Transitions) { ui.tab = InspectorTab.Transitions }
            T(Icons.Filled.Waves, "Behaviors", ui.tab == InspectorTab.Behaviors) { ui.tab = InspectorTab.Behaviors }
            T(Icons.Filled.Inventory2, "Precompose") { st.op("precompose", "layers" to st.selection.toList())?.let { st.selection = setOf(it.optString("layer")) } }
            T(Icons.Filled.PauseCircle, "Freeze frame") { st.op("freezeFrame", "layer" to id, "t" to st.playhead, "duration" to 2.0) }
            T(Icons.Filled.Deselect, "Deselect") { st.selection = emptySet(); ui.tab = InspectorTab.None }
        }
        if (ui.tab != InspectorTab.None) T(Icons.Filled.Close, "Close panel") { ui.tab = InspectorTab.None }
    }
}

fun importIntoProject(app: AppState, ctx: android.content.Context, uri: Uri) {
    val st = app.editor
    try {
        when (val kind = com.motionforge.app.media.Importer.classify(ctx, uri)) {
            com.motionforge.app.media.Importer.Kind.VIDEO, com.motionforge.app.media.Importer.Kind.AUDIO, com.motionforge.app.media.Importer.Kind.IMAGE -> {
                com.motionforge.app.media.Importer.persist(ctx, uri)
                val asset = com.motionforge.app.media.Importer.probe(ctx, uri, kind)
                if (kind == com.motionforge.app.media.Importer.Kind.IMAGE) asset.put("duration", com.motionforge.app.Settings.defaultStillDuration.toDouble())
                val a = st.op("addAsset", "asset" to asset) ?: return
                val layerKind = when (kind) { com.motionforge.app.media.Importer.Kind.VIDEO -> "video"; com.motionforge.app.media.Importer.Kind.AUDIO -> "audio"; else -> "image" }
                val opts = jo("asset" to a.optString("asset"))
                if (kind == com.motionforge.app.media.Importer.Kind.IMAGE) opts.put("duration", com.motionforge.app.Settings.defaultStillDuration.toDouble())
                val r = st.apply(jo("op" to "addLayer", "kind" to layerKind, "options" to opts, "at" to st.playhead, "extendComp" to true)) ?: return
                st.selection = setOf(r.optString("layer"))
                if (asset.optBoolean("vfr")) app.toast("Imported '${asset.optString("name")}'. It has a variable frame rate; timing follows its timestamps.")
                if (asset.optBoolean("hdr")) app.toast("Imported HDR video '${asset.optString("name")}'. It is tone-mapped to the SDR working space.")
            }
            com.motionforge.app.media.Importer.Kind.FONT -> {
                val f = com.motionforge.app.media.Importer.copyToLocal(ctx, uri, "fonts") ?: return
                val r = NativeBridge.call("registerFont", jo("path" to f.absolutePath, "name" to f.nameWithoutExtension))
                app.toast(if (r.optBoolean("ok")) "Font '${f.nameWithoutExtension}' added. Importing a font does not grant a license." else r.optString("error"), !r.optBoolean("ok"))
            }
            com.motionforge.app.media.Importer.Kind.LUT -> {
                val f = com.motionforge.app.media.Importer.copyToLocal(ctx, uri, "luts") ?: return
                val chk = NativeBridge.call("checkLut", jo("path" to f.absolutePath))
                if (!chk.optBoolean("ok")) { app.toast("LUT rejected: " + chk.optString("error"), true); f.delete(); return }
                st.op("addAsset", "asset" to jo("type" to "lut", "name" to f.name, "path" to f.absolutePath))
                app.toast("LUT added (${chk.optInt("size")}³). Apply it with Effects › LUT.")
            }
            com.motionforge.app.media.Importer.Kind.MODEL -> {
                val f = com.motionforge.app.media.Importer.copyToLocal(ctx, uri, "models3d") ?: return
                if (f.extension.lowercase() == "fbx") { app.toast("FBX is proprietary and not supported. Convert to GLB/glTF or OBJ.", true); f.delete(); return }
                val a = st.op("addAsset", "asset" to jo("type" to "model", "name" to f.name, "path" to f.absolutePath)) ?: return
                st.addLayer("model3d", jo("asset" to a.optString("asset")))
            }
            com.motionforge.app.media.Importer.Kind.SUBTITLE -> {
                val text = ctx.contentResolver.openInputStream(uri)?.bufferedReader()?.readText() ?: ""
                val ext = com.motionforge.app.media.Importer.displayName(ctx, uri).substringAfterLast('.', "").lowercase()
                val r = NativeBridge.call("parseSubtitles", jo("text" to text, "ext" to ext))
                if (!r.optBoolean("ok")) app.toast("Subtitle import failed: " + r.optString("error"), true)
                else st.op("setCaptions", "items" to r.optJSONArray("items"), "label" to "Import Subtitles")
            }
            else -> handlePackageImport(app, ctx, uri, kind)
        }
    } catch (e: Exception) {
        app.toast("Import failed: ${e.message}", true)
    }
}

@Composable
fun HistoryDialog(st: EditorState, onDismiss: () -> Unit) {
    val h = remember(st.revision) { NativeBridge.call("history") }
    val entries = h.optJSONArray("entries")?.objects() ?: emptyList()
    val index = h.optInt("index")
    var preview by remember { mutableStateOf<Int?>(null) }
    AlertDialog(onDismissRequest = onDismiss, title = { Text("History") }, text = {
        Column(Modifier.verticalScroll(rememberScrollState())) {
            SmallLabel("Tap an entry to preview it, then confirm to jump. Steps after the current position are the redo stack.")
            Row(Modifier.fillMaxWidth().combinedClickable(onClick = { preview = 0 }).padding(8.dp)) { Text(if (index == 0) "● Original state" else "Original state", color = if (preview == 0) Accent else Color.White) }
            entries.forEach { e ->
                val i = e.optInt("index")
                val time = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US).format(java.util.Date((e.optDouble("time") * 1000).toLong()))
                Row(Modifier.fillMaxWidth().combinedClickable(onClick = { preview = i }).padding(8.dp)) {
                    Text(time, fontFamily = FontFamily.Monospace, fontSize = 12.sp, color = TextDim, modifier = Modifier.width(72.dp))
                    Text((if (i == index) "● " else "") + e.optString("label"), color = if (preview == i) Accent else if (i > index) TextDim else Color.White)
                }
            }
        }
    }, confirmButton = { TextButton(enabled = preview != null, onClick = { preview?.let { st.jump(it) }; onDismiss() }) { Text("Jump here") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Close") } })
}
