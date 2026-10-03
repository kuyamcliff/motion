package com.motionforge.app.ui

import androidx.compose.foundation.layout.height
import androidx.compose.material.icons.filled.AutoAwesome
import androidx.compose.material.icons.filled.Storage
import androidx.compose.material.icons.filled.SortByAlpha
import androidx.compose.material.icons.filled.Schedule
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Movie
import androidx.compose.material.icons.filled.Home
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.clickable
import androidx.compose.foundation.border
import android.graphics.BitmapFactory
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.FileOpen
import androidx.compose.material.icons.filled.FontDownload
import androidx.compose.material.icons.filled.Code
import androidx.compose.material.icons.filled.Extension
import androidx.compose.material.icons.filled.LibraryBooks
import androidx.compose.material.icons.filled.MenuBook
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Sort
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.motionforge.app.AppState
import com.motionforge.app.Screen
import com.motionforge.app.Settings
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.arr
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.objects
import com.motionforge.app.media.Importer
import com.motionforge.app.toast
import org.json.JSONObject
import java.io.File
import java.text.DateFormat
import java.util.Date

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun HomeScreen(app: AppState) {
    val ctx = LocalContext.current
    var refresh by remember { mutableIntStateOf(0) }
    var query by remember { mutableStateOf("") }
    var sort by remember { mutableStateOf("recent") }
    var showNew by remember { mutableStateOf(false) }
    var menuFor by remember { mutableStateOf<JSONObject?>(null) }
    var recoverFor by remember { mutableStateOf<JSONObject?>(null) }
    var passwordFor by remember { mutableStateOf<String?>(null) }
    val projects = remember(refresh) { NativeBridge.call("listProjects").arr("projects").objects() }
    val shown = projects.filter { query.isBlank() || it.optString("name").contains(query, true) }
        .let { l -> when (sort) { "name" -> l.sortedBy { it.optString("name").lowercase() }; "size" -> l.sortedByDescending { it.optLong("bytes") }; else -> l } }

    fun importPackage(uri: Uri, password: String = "") {
        val local = Importer.copyToLocal(ctx, uri, "incoming") ?: return app.toast("Could not read the file.", true)
        when (Importer.classify(ctx, uri)) {
            Importer.Kind.PROJECT -> {
                if (password.isEmpty() && NativeBridge.call("packageEncrypted", jo("path" to local.absolutePath)).optBoolean("encrypted")) { passwordFor = local.absolutePath; return }
                val r = NativeBridge.call("importProject", jo("path" to local.absolutePath, "password" to password))
                if (!r.optBoolean("ok")) app.toast("Import failed: " + r.optString("error"), true)
                else {
                    val w = r.arr("warnings").let { a -> (0 until a.length()).map { a.getString(it) } }
                    app.toast("Project imported" + if (w.isNotEmpty()) " — " + w.joinToString("; ") else "")
                    refresh++
                }
            }
            else -> app.toast("Open scripts, extensions, presets and capsules from inside the editor.", false)
        }
    }

    val openPkg = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> if (uri != null) importPackage(uri) }
    LaunchedEffect(app.pendingImport) { app.pendingImport?.let { importPackage(it); app.pendingImport = null } }

    Column(Modifier.fillMaxSize().background(Bg)) {
        // Header: brand, docs and fonts shortcuts.
        Row(Modifier.fillMaxWidth().padding(start = 16.dp, end = 4.dp, top = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            LogoMark(34.dp)
            Column(Modifier.weight(1f).padding(start = 10.dp)) {
                Text("MOTIONFORGE", fontWeight = FontWeight.Black, fontSize = 18.sp, color = Color.White, maxLines = 1)
                Text("Edit. Create. Share.", fontSize = 11.sp, color = TextDim, maxLines = 1)
            }
            IconBtn(Icons.Filled.FontDownload, "Fonts") { app.go(Screen.Fonts) }
            IconBtn(Icons.Filled.MenuBook, "Documentation and developer center") { app.go(Screen.DevCenter) }
        }
        // Search + filters.
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp).background(PanelHi, RoundedCornerShape(14.dp)).border(1.dp, Stroke, RoundedCornerShape(14.dp)),
            verticalAlignment = Alignment.CenterVertically) {
            androidx.compose.material3.Icon(Icons.Filled.Search, null, tint = TextDim, modifier = Modifier.padding(start = 12.dp))
            androidx.compose.foundation.text.BasicTextField(query, { query = it }, singleLine = true,
                textStyle = androidx.compose.ui.text.TextStyle(color = Color.White, fontSize = 15.sp),
                cursorBrush = androidx.compose.ui.graphics.SolidColor(Accent),
                modifier = Modifier.weight(1f).padding(horizontal = 10.dp, vertical = 13.dp).semantics { contentDescription = "Search projects" },
                decorationBox = { inner -> Box { if (query.isEmpty()) Text("Search projects", color = TextDim, fontSize = 15.sp); inner() } })
        }
        Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            IconAction(Icons.Filled.Schedule, "Sort: recent", sort == "recent") { sort = "recent" }
            IconAction(Icons.Filled.SortByAlpha, "Sort: name", sort == "name") { sort = "name" }
            IconAction(Icons.Filled.Storage, "Sort: size", sort == "size") { sort = "size" }
            androidx.compose.foundation.layout.Spacer(Modifier.weight(1f))
            IconAction(Icons.Filled.FileOpen, "Import Package") { openPkg.launch(arrayOf("*/*")) }
            IconAction(Icons.Filled.AutoAwesome, "Sample Project", tint = KeyColor) {
                val id = createSampleProject(ctx)
                if (id == null) app.toast("Could not create the sample project.", true) else { refresh++; app.openProject(id) }
            }
        }
        androidx.compose.foundation.layout.Spacer(Modifier.height(6.dp))
        LazyVerticalGrid(GridCells.Adaptive(156.dp), modifier = Modifier.weight(1f).padding(horizontal = 10.dp),
            contentPadding = androidx.compose.foundation.layout.PaddingValues(bottom = 12.dp)) {
            item(key = "__new") { NewProjectTile { showNew = true } }
            items(shown, key = { it.optString("id") }) { p ->
                ProjectCard(p, onClick = {
                    if (p.optBoolean("needsRecovery")) recoverFor = p
                    else app.openProject(p.optString("id"))?.let { app.toast(it, true) }
                }, onLong = { menuFor = p })
            }
        }
        HomeBottomBar(app, onNew = { showNew = true })
    }
    if (showNew) NewProjectDialog(onDismiss = { showNew = false }) { name, w, h, fps, dur ->
        val r = NativeBridge.call("createProject", jo("name" to name, "width" to w, "height" to h, "fps" to fps, "duration" to dur))
        if (!r.optBoolean("ok")) app.toast(r.optString("error"), true) else app.openProject(r.optString("id"))
    }
    recoverFor?.let { p ->
        val info = NativeBridge.call("recoveryInfo", jo("id" to p.optString("id")))
        AlertDialog(onDismissRequest = { recoverFor = null }, title = { Text("Recover project?") },
            text = { Text(info.optString("message") + "\nRecovered state from " + DateFormat.getDateTimeInstance().format(Date((info.optDouble("recoveredTime") * 1000).toLong())) +
                ". The last saved version is kept until recovery succeeds.") },
            confirmButton = { TextButton(onClick = { recoverFor = null; app.openProject(p.optString("id"), recover = true)?.let { app.toast(it, true) } }) { Text("Recover") } },
            dismissButton = { TextButton(onClick = {
                recoverFor = null
                NativeBridge.call("discardRecovery", jo("id" to p.optString("id")))
                app.openProject(p.optString("id"))?.let { app.toast(it, true) }
            }) { Text("Open last saved") } })
    }
    menuFor?.let { p -> ProjectMenu(app, p, onDismiss = { menuFor = null }, onChanged = { refresh++ }) }
    passwordFor?.let { path ->
        TextInputDialog("Package password", "", "Password", onDismiss = { passwordFor = null }) { pw ->
            val r = NativeBridge.call("importProject", jo("path" to path, "password" to pw))
            if (!r.optBoolean("ok")) app.toast(r.optString("error"), true) else { app.toast("Project imported"); refresh++ }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
fun ProjectCard(p: JSONObject, onClick: () -> Unit, onLong: () -> Unit) {
    val thumb = remember(p.optString("thumbnail"), p.optDouble("modified")) {
        p.optString("thumbnail").takeIf { it.isNotEmpty() && File(it).exists() }?.let { BitmapFactory.decodeFile(it)?.asImageBitmap() }
    }
    Column(Modifier.padding(6.dp).clip(RoundedCornerShape(16.dp)).background(Panel).border(1.dp, Stroke, RoundedCornerShape(16.dp))
        .combinedClickable(onClick = onClick, onLongClick = onLong)
        .semantics { contentDescription = "Project ${p.optString("name")}" }) {
        Box(Modifier.fillMaxWidth().aspectRatio(16f / 10f).background(Color.Black)) {
            if (thumb != null) Image(thumb, null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
            else androidx.compose.material3.Icon(Icons.Filled.Movie, null, tint = Stroke, modifier = Modifier.align(Alignment.Center).size(40.dp))
            Text(durationText(p.optDouble("duration")), fontSize = 10.sp, color = Color.White,
                modifier = Modifier.align(Alignment.BottomEnd).padding(6.dp).background(Color(0xAA000000), RoundedCornerShape(6.dp)).padding(horizontal = 6.dp, vertical = 2.dp))
            if (p.optBoolean("needsRecovery")) Row(Modifier.align(Alignment.TopStart).padding(6.dp).background(Color(0xCC8A5A00), RoundedCornerShape(6.dp)).padding(4.dp),
                verticalAlignment = Alignment.CenterVertically) {
                androidx.compose.material3.Icon(Icons.Filled.Warning, "Recovery available", tint = Color.White, modifier = Modifier.size(14.dp))
                Text(" Recovery", fontSize = 10.sp)
            }
        }
        Column(Modifier.padding(horizontal = 10.dp, vertical = 8.dp)) {
            Text(p.optString("name"), fontWeight = FontWeight.SemiBold, fontSize = 13.sp, maxLines = 1, overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis)
            Text("%d×%d · %s".format(p.optInt("width"), p.optInt("height"), bytesText(p.optLong("bytes"))), fontSize = 11.sp, color = TextDim, maxLines = 1)
            Text(ago(p.optDouble("modified")), fontSize = 11.sp, color = TextDim, maxLines = 1)
        }
    }
}

@Composable
fun NewProjectTile(onClick: () -> Unit) {
    Column(Modifier.padding(6.dp).clip(RoundedCornerShape(16.dp)).background(Accent.copy(alpha = 0.10f))
        .border(1.dp, Accent.copy(alpha = 0.6f), RoundedCornerShape(16.dp)).clickable(onClick = onClick)
        .semantics(mergeDescendants = true) { contentDescription = "New Project" }
        .aspectRatio(16f / 13.2f), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
        Box(Modifier.size(64.dp).background(AccentGradient, androidx.compose.foundation.shape.CircleShape), contentAlignment = Alignment.Center) {
            androidx.compose.material3.Icon(Icons.Filled.Add, null, tint = Color.White, modifier = Modifier.size(36.dp))
        }
    }
}

/** Brand mark: the "M" monogram on the accent gradient. */
@Composable
fun LogoMark(size: androidx.compose.ui.unit.Dp) {
    Box(Modifier.size(size).background(AccentGradient, RoundedCornerShape(size / 4)), contentAlignment = Alignment.Center) {
        Text("M", fontWeight = FontWeight.Black, color = Color.White, fontSize = (size.value * 0.55f).sp)
    }
}

@Composable
fun HomeBottomBar(app: AppState, onNew: () -> Unit) {
    Row(Modifier.fillMaxWidth().background(Panel).border(1.dp, Stroke, RoundedCornerShape(0.dp)).navigationBarsPadding().padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceEvenly, verticalAlignment = Alignment.CenterVertically) {
        ToolButton(Icons.Filled.Home, "Home", selected = true) { }
        ToolButton(Icons.Filled.LibraryBooks, "Library", description = "Presets and capsules library") { app.go(Screen.Library) }
        Box(Modifier.size(52.dp).background(AccentGradient, androidx.compose.foundation.shape.CircleShape).clickable(onClick = onNew)
            .semantics { contentDescription = "Create a new project" }, contentAlignment = Alignment.Center) {
            androidx.compose.material3.Icon(Icons.Filled.Add, null, tint = Color.White, modifier = Modifier.size(28.dp))
        }
        ToolButton(Icons.Filled.Extension, "Extensions") { app.go(Screen.Extensions) }
        ToolButton(Icons.Filled.Settings, "Settings") { app.go(Screen.Settings) }
    }
}

fun ago(epochSec: Double): String {
    val d = System.currentTimeMillis() / 1000.0 - epochSec
    return when {
        d < 60 -> "just now"
        d < 3600 -> "${(d / 60).toInt()} min ago"
        d < 86400 -> "${(d / 3600).toInt()} h ago"
        d < 86400 * 7 -> "${(d / 86400).toInt()} d ago"
        else -> DateFormat.getDateInstance(DateFormat.SHORT).format(Date((epochSec * 1000).toLong()))
    }
}

fun durationText(s: Double) = "%d:%02d".format((s / 60).toInt(), (s % 60).toInt())
fun bytesText(b: Long) = when { b > 1 shl 30 -> "%.1f GB".format(b / 1073741824.0); b > 1 shl 20 -> "%.1f MB".format(b / 1048576.0); else -> "%d KB".format(b / 1024) }

@Composable
fun ProjectMenu(app: AppState, p: JSONObject, onDismiss: () -> Unit, onChanged: () -> Unit) {
    val ctx = LocalContext.current
    val id = p.optString("id")
    var rename by remember { mutableStateOf(false) }
    var confirmDelete by remember { mutableStateOf(false) }
    var versions by remember { mutableStateOf(false) }
    var exportPkg by remember { mutableStateOf(false) }
    AlertDialog(onDismissRequest = onDismiss, title = { Text(p.optString("name")) }, text = {
        Column(Modifier.verticalScroll(rememberScrollState())) {
            listOf(
                "Open" to { onDismiss(); app.openProject(id)?.let { app.toast(it, true) }; Unit },
                "Rename" to { rename = true },
                "Duplicate" to { NativeBridge.call("duplicateProject", jo("id" to id, "name" to p.optString("name") + " copy")); onChanged(); onDismiss() },
                "Export package (.mforge)" to { exportPkg = true },
                "Recover version…" to { versions = true },
                "Inspect" to { onDismiss(); if (app.openProject(id) == null) app.go(Screen.Inspector) },
                "Delete" to { confirmDelete = true },
            ).forEach { (t, f) -> TextButton(onClick = f, modifier = Modifier.fillMaxWidth()) { Text(t, modifier = Modifier.fillMaxWidth()) } }
            SmallLabel("Storage: " + bytesText(p.optLong("bytes")))
        }
    }, confirmButton = { TextButton(onClick = onDismiss) { Text("Close") } })
    if (rename) TextInputDialog("Rename project", p.optString("name"), onDismiss = { rename = false }) {
        NativeBridge.call("renameProject", jo("id" to id, "name" to it)); onChanged(); onDismiss()
    }
    if (confirmDelete) ConfirmDialog("Delete project?", "\"${p.optString("name")}\" will be moved to the app's trash. Linked media files on your device are not touched.", "Delete",
        onDismiss = { confirmDelete = false }) { NativeBridge.call("deleteProject", jo("id" to id)); onChanged(); onDismiss() }
    if (versions) {
        val vs = remember { NativeBridge.call("versions", jo("id" to id)).arr("versions").objects() }
        AlertDialog(onDismissRequest = { versions = false }, title = { Text("Saved versions") }, text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                if (vs.isEmpty()) Text("No saved versions yet.")
                vs.forEach { v ->
                    TextButton(onClick = {
                        val r = NativeBridge.call("restoreVersion", jo("id" to id, "version" to v.optString("name")))
                        app.toast(if (r.optBoolean("ok")) "Version restored (previous state kept as backup)" else r.optString("error"), !r.optBoolean("ok"))
                        versions = false; onChanged(); onDismiss()
                    }) { Text(DateFormat.getDateTimeInstance().format(Date((v.optDouble("time") * 1000).toLong()))) }
                }
            }
        }, confirmButton = { TextButton(onClick = { versions = false }) { Text("Close") } })
    }
    if (exportPkg) {
        // Open the project briefly to package it (engine packages the open project).
        PackageExportDialog(app, onDismiss = { exportPkg = false; onDismiss() }, projectId = id)
    }
}

@Composable
fun NewProjectDialog(onDismiss: () -> Unit, onCreate: (String, Int, Int, Double, Double) -> Unit) {
    var name by remember { mutableStateOf("My Project") }
    var preset by remember { mutableStateOf(0) }
    var fps by remember { mutableStateOf(30.0) }
    var dur by remember { mutableStateOf("15") }
    var cw by remember { mutableStateOf("1920") }
    var ch by remember { mutableStateOf("1080") }
    val presets = listOf("16:9 1080p" to (1920 to 1080), "9:16 Vertical" to (1080 to 1920), "1:1 Square" to (1080 to 1080), "4:5 Portrait" to (1080 to 1350),
        "4:3" to (1440 to 1080), "16:9 4K" to (3840 to 2160), "16:9 720p" to (1280 to 720), "Custom" to (0 to 0))
    AlertDialog(onDismissRequest = onDismiss, title = { Text("New Project") }, text = {
        Column(Modifier.verticalScroll(rememberScrollState())) {
            OutlinedTextField(name, { name = it }, label = { Text("Name") }, singleLine = true)
            SmallLabel("Aspect / resolution")
            androidx.compose.foundation.layout.FlowRow { presets.forEachIndexed { i, (n, _) -> Chip(n, preset == i) { preset = i } } }
            if (presets[preset].second.first == 0) Row {
                OutlinedTextField(cw, { cw = it }, label = { Text("Width") }, modifier = Modifier.width(110.dp), singleLine = true)
                OutlinedTextField(ch, { ch = it }, label = { Text("Height") }, modifier = Modifier.width(110.dp), singleLine = true)
            }
            SmallLabel("Frame rate")
            androidx.compose.foundation.layout.FlowRow { listOf(24.0, 25.0, 29.97, 30.0, 50.0, 60.0).forEach { f -> Chip(fmt(f), fps == f) { fps = f } } }
            OutlinedTextField(dur, { dur = it }, label = { Text("Duration (seconds)") }, singleLine = true)
        }
    }, confirmButton = {
        TextButton(onClick = {
            val (w, h) = presets[preset].second.let { if (it.first == 0) (cw.toIntOrNull() ?: 1920) to (ch.toIntOrNull() ?: 1080) else it }
            onCreate(name, w, h, fps, dur.toDoubleOrNull() ?: 15.0); onDismiss()
        }) { Text("Create") }
    }, dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } })
}

@Composable
fun FirstRunScreen(app: AppState) {
    var style by remember { mutableStateOf(Settings.editingStyle) }
    Column(Modifier.fillMaxSize().padding(24.dp), verticalArrangement = Arrangement.Center) {
        Text("Welcome to MOTIONFORGE", fontSize = 26.sp, fontWeight = FontWeight.Bold)
        Gap()
        Text("Choose your editing style. This only changes the suggested tools — every feature stays available.", color = TextDim)
        Gap(16)
        listOf("quick" to "Quick Edit", "motion" to "Motion Graphics", "pro" to "Professional Timeline", "vfx" to "3D / VFX").forEach { (k, n) ->
            Chip(n, style == k, Modifier.fillMaxWidth()) { style = k }
        }
        Gap(16)
        Button(onClick = { Settings.editingStyle = style; Settings.firstRunDone = true; app.stack[0] = Screen.Home }, modifier = Modifier.fillMaxWidth()) { Text("Continue") }
        SmallLabel("No account. No cloud. Your projects stay on this device.")
    }
}

/** Builds the bundled onboarding project: video, shape, text, keyframes, mask, effect, audio, caption and a 3D object. */
fun createSampleProject(ctx: android.content.Context): String? {
    val r = NativeBridge.call("createProject", jo("name" to "Sample — MOTIONFORGE tour", "width" to 1280, "height" to 720, "fps" to 30.0, "duration" to 11.0))
    if (!r.optBoolean("ok")) return null
    val id = r.optString("id")
    if (!NativeBridge.call("openProject", jo("id" to id)).optBoolean("ok")) return null
    fun ap(op: JSONObject) = NativeBridge.call("apply", op).optJSONObject("data") ?: JSONObject()
    val vid = ap(jo("op" to "addAsset", "asset" to jo("type" to "video", "name" to "Procedural test clip", "generator" to "counter", "width" to 1280, "height" to 720,
        "fps" to 30, "duration" to 11.0, "hasAudio" to false))).optString("asset")
    ap(jo("op" to "addLayer", "kind" to "video", "options" to jo("asset" to vid)))
    val speech = File(ctx.filesDir, "samples/speech.wav")
    if (speech.exists()) {
        val aud = ap(jo("op" to "addAsset", "asset" to jo("type" to "audio", "name" to "Speech (public domain)", "path" to speech.absolutePath, "duration" to 11.0,
            "hasAudio" to true, "sampleRate" to 16000, "channels" to 1))).optString("asset")
        ap(jo("op" to "addLayer", "kind" to "audio", "options" to jo("asset" to aud)))
    }
    val shape = ap(jo("op" to "addLayer", "kind" to "shape", "options" to jo("shape" to "star", "color" to listOf(1.0, 0.42, 0.24, 1.0)))).optString("layer")
    ap(jo("op" to "addMask", "layer" to shape, "shape" to "ellipse", "rect" to listOf(-150, -150, 300, 300)))
    ap(jo("op" to "addBehavior", "layer" to shape, "type" to "float"))
    val text = ap(jo("op" to "addLayer", "kind" to "text", "options" to jo("text" to "MOTIONFORGE", "size" to 96.0))).optString("layer")
    ap(jo("op" to "addKeyframe", "layer" to text, "path" to "transform.position", "t" to 0.0, "value" to listOf(640.0, 200.0, 0.0), "interp" to "easeOut"))
    ap(jo("op" to "addKeyframe", "layer" to text, "path" to "transform.position", "t" to 1.5, "value" to listOf(640.0, 120.0, 0.0)))
    ap(jo("op" to "textPreset", "layer" to text, "preset" to "fadeUp", "duration" to 1.0))
    ap(jo("op" to "addEffect", "layer" to text, "type" to "stylize.glow"))
    val cube = ap(jo("op" to "addLayer", "kind" to "model3d", "options" to jo("primitive" to "cube", "size" to 160.0, "color" to listOf(0.3, 0.7, 1.0, 1.0)))).optString("layer")
    ap(jo("op" to "setProp", "layer" to cube, "path" to "transform.position", "value" to listOf(1050.0, 520.0, 0.0)))
    ap(jo("op" to "addKeyframe", "layer" to cube, "path" to "transform.rotationY", "t" to 0.0, "value" to 0.0, "interp" to "linear"))
    ap(jo("op" to "addKeyframe", "layer" to cube, "path" to "transform.rotationY", "t" to 11.0, "value" to 360.0))
    ap(jo("op" to "setCaptions", "items" to listOf(jo("start" to 0.3, "end" to 3.0, "text" to "Captions work offline — try Caption Studio"))))
    NativeBridge.call("save", jo("version" to true))
    NativeBridge.call("closeProject")
    return id
}
