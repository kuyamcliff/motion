package com.motionforge.app.ui

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

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val wide = maxWidth > 700.dp
        val fullHeight = maxHeight
        Column(Modifier.fillMaxSize()) {
            EditorTopBar(app, ui)
            if (wide) {
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
                PreviewPane(app, ui, Modifier.weight(if (ui.tab == InspectorTab.None) 1f else 0.8f).fillMaxWidth())
                Transport(st, player, ui)
                ContextToolbar(app, ui)
                if (ui.tab != InspectorTab.None) {
                    Column(Modifier.weight(1f).fillMaxWidth().background(Panel)) { InspectorPanel(app, ui) }
                } else {
                    Timeline(app, ui, Modifier.weight(0.7f).fillMaxWidth())
                }
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
    Row(Modifier.fillMaxWidth().background(Color(0xFF0C0D10)).padding(horizontal = 2.dp), verticalAlignment = Alignment.CenterVertically) {
        IconBtn(Icons.AutoMirrored.Filled.ArrowBack, "Back to projects") { app.back() }
        Column(Modifier.weight(1f).combinedClickable(onClick = { renameProject = true })) {
            Text(st.doc.optJSONObject("meta")?.optString("name") ?: "", fontWeight = FontWeight.SemiBold, maxLines = 1, fontSize = 14.sp)
            Row(verticalAlignment = Alignment.CenterVertically) {
                val (icon, label) = when (st.saveStatus) { "saved" -> Icons.Filled.CloudDone to "Saved"; "error" -> Icons.Filled.ErrorOutline to "Save error"; else -> Icons.Filled.Edit to "Unsaved changes" }
                Icon(icon, label, tint = if (st.saveStatus == "error") Color.Red else TextDim, modifier = Modifier.size(14.dp))
                Text(" ${st.comp.optString("name")} · ${st.timecode(st.playhead)}", fontSize = 11.sp, color = TextDim, fontFamily = FontFamily.Monospace)
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
        IconBtn(Icons.Filled.Search, "Command palette") { app.paletteOpen = true }
        IconBtn(Icons.Filled.Tune, "Composition settings") { ui.tab = InspectorTab.Comp }
        IconBtn(Icons.Filled.FileUpload, "Export") { app.player?.pause(); app.go(Screen.Export) }
        Box {
            IconBtn(Icons.Filled.MoreVert, "More") { more = true }
            DropdownMenu(more, { more = false }) {
                listOf(
                    "Save" to { st.save(); app.toast("Saved") },
                    "Save version (checkpoint)" to { st.save(version = true); app.toast("Version saved") },
                    "History" to { ui.historyOpen = true },
                    "Caption Studio" to { app.player?.pause(); app.go(Screen.Captions) },
                    "Script Studio" to { app.player?.pause(); app.go(Screen.Scripts) },
                    "Markers" to { ui.tab = InspectorTab.Markers },
                    "Project inspector" to { app.go(Screen.Inspector) },
                    "Performance monitor" to { app.go(Screen.Performance) },
                    "Export project package" to { app.go(Screen.Export) },
                    (if (ui.showSafe) "Hide safe areas" else "Show safe areas") to { ui.showSafe = !ui.showSafe },
                    (if (ui.showGrid) "Hide grid" else "Show grid") to { ui.showGrid = !ui.showGrid },
                    (if (ui.showRulers) "Hide rulers" else "Show rulers") to { ui.showRulers = !ui.showRulers },
                    (if (ui.checkerboard) "Hide transparency grid" else "Show transparency grid") to { ui.checkerboard = !ui.checkerboard },
                    (if (ui.showDiagnostics) "Hide diagnostics overlay" else "Show diagnostics overlay") to { ui.showDiagnostics = !ui.showDiagnostics },
                    "Help & documentation" to { app.go(Screen.DevCenter) },
                ).forEach { (t, f) -> DropdownMenuItem(text = { Text(t) }, onClick = { more = false; f() }) }
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
    Row(Modifier.fillMaxWidth().background(Color(0xFF0C0D10)).horizontalScroll(rememberScrollState()), verticalAlignment = Alignment.CenterVertically) {
        IconBtn(Icons.Filled.FirstPage, "Jump to beginning") { player.seek(0.0) }
        IconBtn(Icons.Filled.SkipPrevious, "Previous edit point") {
            jumpTo(NativeBridge.call("editPoints").optJSONArray("times")?.let { a -> (0 until a.length()).map { a.getDouble(it) } } ?: emptyList(), false)
        }
        IconBtn(Icons.Filled.Diamond, "Previous keyframe", tint = KeyColor) { jumpTo(st.keyframeTimes(st.selectedLayer), false) }
        RepeatButton(Icons.Filled.NavigateBefore, "Previous frame") { player.stepFrames(-1) }
        PlayButton(player)
        RepeatButton(Icons.Filled.NavigateNext, "Next frame") { player.stepFrames(1) }
        IconBtn(Icons.Filled.Diamond, "Next keyframe", tint = KeyColor) { jumpTo(st.keyframeTimes(st.selectedLayer), true) }
        IconBtn(Icons.Filled.SkipNext, "Next edit point") {
            jumpTo(NativeBridge.call("editPoints").optJSONArray("times")?.let { a -> (0 until a.length()).map { a.getDouble(it) } } ?: emptyList(), true)
        }
        IconBtn(Icons.Filled.LastPage, "Jump to end") { player.seek(st.duration) }
        IconBtn(Icons.Filled.Stop, "Stop and reset") { player.stop() }
        IconBtn(Icons.Filled.Repeat, if (player.loop) "Loop on" else "Loop off", tint = if (player.loop) Accent else TextDim) { player.loop = !player.loop }
        TextButton(onClick = { player.rangeIn = st.playhead }) { Text("In", color = if (player.rangeIn >= 0) Accent else Color.White) }
        TextButton(onClick = { player.rangeOut = st.playhead }) { Text("Out", color = if (player.rangeOut > 0) Accent else Color.White) }
        if (player.rangeIn >= 0 || player.rangeOut > 0) TextButton(onClick = { player.rangeIn = -1.0; player.rangeOut = -1.0 }) { Text("Clear range") }
        Column(Modifier.padding(horizontal = 8.dp)) {
            Text(st.timecode(st.playhead), fontFamily = FontFamily.Monospace, fontSize = 13.sp)
            Text("F ${st.frame(st.playhead)} · ${if (player.playing) "%.0f fps".format(player.fpsActual) else "%.0f ms".format(player.renderMs)} · ${player.quality}",
                fontSize = 10.sp, color = TextDim)
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
        Icon(if (player.playing) Icons.Filled.Pause else Icons.Filled.PlayArrow, null, tint = Accent, modifier = Modifier.size(34.dp))
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
    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        if (sel == null) {
            Chip("+ Media") { importMedia.launch(arrayOf("video/*", "audio/*", "image/*", "*/*")) }
            Chip("+ Layer") { ui.tab = InspectorTab.Add }
            Chip("Text") { st.addLayer("text", jo("text" to "Title")); ui.tab = InspectorTab.Text }
            Chip("Shape") { st.addLayer("shape", jo("shape" to "rect")); ui.tab = InspectorTab.Shape }
            Chip("Captions") { app.player?.pause(); app.go(Screen.Captions) }
            Chip("Draw") { ui.previewMode = if (ui.previewMode == "draw") "select" else "draw" }
            Chip("Split all") { st.op("split", "t" to st.playhead) }
            Chip("Delete gap") { st.op("deleteGap", "t" to st.playhead) }
            Chip("Marker") { st.op("addMarker", "t" to st.playhead, "title" to "Marker") }
        } else {
            val id = sel.optString("id")
            val type = sel.optString("type")
            Chip("Inspector", ui.tab == InspectorTab.Layer) { ui.tab = InspectorTab.Layer }
            Chip("Split") { st.op("split", "layers" to listOf(id), "t" to st.playhead) }
            Chip("Duplicate") { st.op("duplicateLayers", "layers" to listOf(id)) }
            Chip("Delete") { st.op("removeLayers", "layers" to listOf(id)); st.selection = emptySet() }
            Chip("Ripple delete") { st.op("rippleDelete", "layer" to id); st.selection = emptySet() }
            Chip("Effects", ui.tab == InspectorTab.Effects) { ui.tab = InspectorTab.Effects }
            Chip("Keyframes", ui.tab == InspectorTab.Keyframes) { ui.tab = InspectorTab.Keyframes }
            Chip("Masks", ui.tab == InspectorTab.Masks) { ui.tab = InspectorTab.Masks }
            if (type == "text") Chip("Text", ui.tab == InspectorTab.Text) { ui.tab = InspectorTab.Text }
            if (type == "shape") Chip("Shape", ui.tab == InspectorTab.Shape) { ui.tab = InspectorTab.Shape }
            if (sel.has("audio")) Chip("Audio", ui.tab == InspectorTab.Audio) { ui.tab = InspectorTab.Audio }
            if (type == "video" || type == "audio" || type == "precomp" || type == "image") Chip("Speed", ui.tab == InspectorTab.Time) { ui.tab = InspectorTab.Time }
            if (type == "video") Chip("Track", ui.tab == InspectorTab.Tracking) { ui.tab = InspectorTab.Tracking }
            if (type == "model3d" || type == "camera" || type == "light" || sel.optBoolean("threeD")) Chip("3D", ui.tab == InspectorTab.ThreeD) { ui.tab = InspectorTab.ThreeD }
            if (type == "particles") Chip("Particles", ui.tab == InspectorTab.Particles) { ui.tab = InspectorTab.Particles }
            if (type == "captions") Chip("Captions") { app.go(Screen.Captions) }
            Chip("Transitions", ui.tab == InspectorTab.Transitions) { ui.tab = InspectorTab.Transitions }
            Chip("Behaviors", ui.tab == InspectorTab.Behaviors) { ui.tab = InspectorTab.Behaviors }
            Chip("Precompose") { st.op("precompose", "layers" to st.selection.toList())?.let { st.selection = setOf(it.optString("layer")) } }
            Chip("Freeze frame") { st.op("freezeFrame", "layer" to id, "t" to st.playhead, "duration" to 2.0) }
            Chip("Deselect") { st.selection = emptySet(); ui.tab = InspectorTab.None }
        }
        if (ui.tab != InspectorTab.None) Chip("Close panel") { ui.tab = InspectorTab.None }
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
