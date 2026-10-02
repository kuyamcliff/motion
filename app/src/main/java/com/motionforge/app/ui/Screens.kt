package com.motionforge.app.ui

import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.Star
import androidx.compose.material.icons.outlined.StarOutline
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.FileProvider
import com.motionforge.app.AppState
import com.motionforge.app.Screen
import com.motionforge.app.Settings
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.ProgressCallback
import com.motionforge.app.engine.arr
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.obj
import com.motionforge.app.engine.objects
import com.motionforge.app.engine.strings
import com.motionforge.app.export.CodecCaps
import com.motionforge.app.export.ExportQueue
import com.motionforge.app.export.ExportSettings
import com.motionforge.app.media.Importer
import com.motionforge.app.toast
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** Standard full-screen page: back button, title, scrollable content. */
@Composable
fun ScreenScaffold(app: AppState, title: String, actions: @Composable () -> Unit = {}, scroll: Boolean = true, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().background(Panel).padding(4.dp), verticalAlignment = Alignment.CenterVertically) {
            IconBtn(Icons.AutoMirrored.Filled.ArrowBack, "Back") { app.back() }
            Text(title, fontWeight = FontWeight.Bold, fontSize = 18.sp, modifier = Modifier.weight(1f))
            actions()
        }
        val m = Modifier.fillMaxSize().padding(horizontal = 12.dp, vertical = 6.dp)
        Column(if (scroll) m.verticalScroll(rememberScrollState()) else m, content = content)
    }
}

fun shareFile(ctx: android.content.Context, f: File, mime: String) {
    val uri = FileProvider.getUriForFile(ctx, ctx.packageName + ".files", f)
    ctx.startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).setType(mime).putExtra(Intent.EXTRA_STREAM, uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), "Share"))
}

// ====================================================================== Export
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ExportScreen(app: AppState) {
    val st = app.editor
    val ctx = LocalContext.current
    val comp = st.comp
    var format by remember { mutableStateOf("mp4") }
    var codec by remember { mutableStateOf(Settings.defaultCodec.takeIf { CodecCaps.has(it) } ?: "video/avc") }
    var res by remember { mutableIntStateOf(minOf(Settings.defaultResolution, comp.optInt("height", 1080))) }
    var fps by remember { mutableStateOf(comp.optDouble("fps", 30.0)) }
    var mbps by remember { mutableStateOf(12.0) }
    var audio by remember { mutableStateOf(true) }
    var range by remember { mutableStateOf("all") }
    var transparent by remember { mutableStateOf(false) }
    var name by remember { mutableStateOf(st.doc.obj("meta").optString("name", "export")) }
    val problems = remember(st.revision) { NativeBridge.call("diagnostics").arr("items").objects().filter { it.optString("severity") == "error" } }
    ScreenScaffold(app, "Export") {
        SectionTitle("Format")
        FlowRow {
            listOf("mp4" to "MP4 video", "webm" to "WebM", "gif" to "GIF", "png" to "PNG sequence", "m4a" to "Audio (M4A)", "wav" to "Audio (WAV)").forEach { (k, n) ->
                val ok = when (k) { "webm" -> CodecCaps.has("video/x-vnd.on2.vp9") || CodecCaps.has("video/x-vnd.on2.vp8"); "m4a" -> CodecCaps.has("audio/mp4a-latm"); else -> true }
                if (ok) Chip(n, format == k) { format = k; if (k == "webm") codec = if (CodecCaps.has("video/x-vnd.on2.vp9")) "video/x-vnd.on2.vp9" else "video/x-vnd.on2.vp8" else if (k == "mp4" && codec.contains("vp")) codec = "video/avc" }
            }
        }
        SmallLabel("Only formats this device can encode are listed.")
        OutlinedTextField(name, { name = it }, label = { Text("File name") }, singleLine = true, modifier = Modifier.fillMaxWidth())
        if (format == "mp4") EnumPicker("Video codec", codec, CodecCaps.videoOptions().filter { !it.first.contains("vp") }) { codec = it }
        if (format in setOf("mp4", "webm", "gif", "png")) {
            SectionTitle("Resolution")
            FlowRow {
                listOf(2160, 1440, 1080, 720, 540, 480, 360).filter { it <= maxOf(comp.optInt("height"), comp.optInt("width")) }.forEach { r ->
                    val w = (r.toDouble() * comp.optInt("width") / comp.optInt("height")).let { (Math.round(it / 2) * 2).toInt() }
                    val supported = format !in setOf("mp4", "webm") || CodecCaps.supportsSize(codec, w, r)
                    if (supported) Chip("${r}p", res == r) { res = r }
                }
            }
            SectionTitle("Frame rate")
            FlowRow { listOf(12.0, 15.0, 24.0, 25.0, 30.0, 50.0, 60.0).forEach { f -> Chip(fmt(f), fps == f) { fps = f } } }
        }
        if (format in setOf("mp4", "webm")) {
            NumberRow("Bitrate (Mbps)", mbps, 1.0, 80.0, onPreview = {}) { mbps = it }
            LabeledSwitch("Include audio", audio) { audio = it }
        }
        if (format == "png") LabeledSwitch("Transparent background", transparent) { transparent = it }
        SectionTitle("Range")
        val wa = comp.optJSONArray("workArea")
        FlowRow {
            Chip("Whole composition", range == "all") { range = "all" }
            if (wa != null) Chip("Work area", range == "work") { range = "work" }
            if (app.player?.rangeIn ?: -1.0 >= 0) Chip("Preview range", range == "preview") { range = "preview" }
        }
        val h = res
        val w = (h.toDouble() * comp.optInt("width") / comp.optInt("height")).let { (Math.round(it / 2) * 2).toInt() }
        val (t0, t1) = when (range) {
            "work" -> (wa?.optDouble(0) ?: 0.0) to (wa?.optDouble(1) ?: st.duration)
            "preview" -> (app.player?.rangeIn ?: 0.0) to (app.player?.rangeOut?.takeIf { it > 0 } ?: st.duration)
            else -> 0.0 to st.duration
        }
        val estBytes = when (format) { "mp4", "webm" -> (mbps * 1e6 / 8 * (t1 - t0)).toLong(); "wav" -> (48000 * 8 * (t1 - t0)).toLong(); "m4a" -> (24000 * (t1 - t0)).toLong(); else -> 0L }
        if (estBytes > 0) SmallLabel("Estimated size: ${"%.1f".format(estBytes / 1e6)} MB  •  ${w}×${h}  •  ${"%.1f".format(t1 - t0)} s")
        val free = ExportQueue.exportsDir().usableSpace
        if (estBytes > free) Text("Not enough free storage for this export (${free / 1_000_000} MB free).", color = Color(0xFFFF7A7A))
        problems.forEach { p -> Text("⚠ " + p.optString("message") + " — " + p.optString("recommendation"), color = Color(0xFFFFB74D), fontSize = 12.sp) }
        Gap()
        Button(onClick = {
            st.save()
            val s = ExportSettings(format = format, videoMime = codec, width = w, height = h, fps = fps, bitrate = (mbps * 1e6).toInt(), audio = audio, t0 = t0, t1 = t1, transparent = transparent)
            Settings.defaultCodec = codec; Settings.defaultResolution = res
            ExportQueue.enqueue(st.projectId, name, s)
            app.toast("Export queued. You can keep editing; the export uses a snapshot of the project.")
        }, modifier = Modifier.fillMaxWidth().semantics { contentDescription = "Start export" }) { Text("Export") }
        SectionTitle("Export queue")
        if (ExportQueue.jobs.isEmpty()) SmallLabel("No exports yet.")
        ExportQueue.jobs.reversed().forEach { j ->
            Column(Modifier.fillMaxWidth().padding(vertical = 4.dp).background(PanelHi, RoundedCornerShape(8.dp)).padding(8.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(File(j.outPath).name, fontWeight = FontWeight.Bold)
                        Text("${j.settings.format.uppercase()} ${j.settings.width}×${j.settings.height} • ${j.status}", fontSize = 12.sp, color = TextDim)
                    }
                    if (j.status == "done" && File(j.outPath).isFile) IconBtn(Icons.Filled.Share, "Share export") {
                        shareFile(ctx, File(j.outPath), when (j.settings.format) { "gif" -> "image/gif"; "wav" -> "audio/wav"; "m4a" -> "audio/mp4"; "webm" -> "video/webm"; else -> "video/mp4" })
                    }
                    if (j.status != "running") IconBtn(Icons.Filled.Delete, "Remove from queue") { ExportQueue.remove(j) }
                }
                if (j.status == "running" || j.status == "waiting") {
                    LinearProgressIndicator(progress = { j.progress }, modifier = Modifier.fillMaxWidth().semantics { contentDescription = "Export progress ${(j.progress * 100).toInt()}%" })
                    TextButton(onClick = { ExportQueue.cancel(j) }) { Text("Cancel") }
                }
                if (j.status == "done") {
                    val v = j.result.obj("validation")
                    Text("✓ Validated: ${v.optInt("frames", v.optInt("videoFrames"))} frames" + (if (v.has("duration")) ", ${"%.2f".format(v.optDouble("duration"))} s" else "") +
                        (if (j.savedUri.isNotEmpty()) " • saved to gallery" else "") + "\n" + j.outPath, fontSize = 12.sp, color = Color(0xFF66BB6A))
                }
                if (j.status == "failed" || j.status == "cancelled") {
                    Text(j.error, color = Color(0xFFFF7A7A), fontSize = 12.sp)
                    j.suggestions.forEach { Text("• $it", fontSize = 12.sp, color = TextDim) }
                    Row { TextButton(onClick = { ExportQueue.retry(j) }) { Text("Retry") }; TextButton(onClick = { ExportQueue.duplicate(j) }) { Text("Duplicate") } }
                }
            }
        }
    }
}

// ====================================================================== Caption Studio (offline Whisper)
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun CaptionStudioScreen(app: AppState) {
    val st = app.editor
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val media = st.layers.filter { it.optString("type") in setOf("video", "audio") }
    var source by remember { mutableStateOf(st.selectedLayer?.takeIf { it.optString("type") in setOf("video", "audio") }?.optString("id") ?: media.firstOrNull()?.optString("id") ?: "") }
    var lang by remember { mutableStateOf(Settings.asrLanguage) }
    var maxChars by remember { mutableStateOf(42.0) }
    var progress by remember { mutableStateOf<Float?>(null) }
    var lastRun by remember { mutableStateOf("") }
    var find by remember { mutableStateOf("") }
    var repl by remember { mutableStateOf("") }
    var editing by remember { mutableStateOf<JSONObject?>(null) }
    var selected by remember { mutableStateOf(setOf<String>()) }
    val capLayer = st.layers.firstOrNull { it.optString("type") == "captions" }
    val items = capLayer?.obj("captions")?.arr("items")?.objects() ?: emptyList()
    val style = capLayer?.obj("captions")?.obj("style") ?: JSONObject()
    var assetsReady by remember { mutableStateOf(com.motionforge.app.MfApplication.bundledAssetsReady) }
    LaunchedEffect(Unit) { if (!assetsReady) { withContext(Dispatchers.IO) { com.motionforge.app.MfApplication.awaitBundledAssets() }; assetsReady = true } }
    val model = remember(assetsReady) { Settings.asrModel.ifEmpty { File(ctx.filesDir, "models").listFiles()?.firstOrNull { it.name.endsWith(".bin") }?.absolutePath ?: "" } }
    val saveSub = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/plain")) { uri: Uri? ->
        if (uri != null) {
            val fmt = uri.toString().substringAfterLast('.').lowercase().takeIf { it in setOf("srt", "vtt", "ass") } ?: "srt"
            val r = NativeBridge.call("exportSubtitles", jo("format" to fmt))
            if (!r.optBoolean("ok")) app.toast(r.optString("error"), true)
            else { ctx.contentResolver.openOutputStream(uri)?.use { it.write(r.optString("text").toByteArray()) }; app.toast("Subtitles exported.") }
        }
    }
    ScreenScaffold(app, "Caption Studio") {
        SectionTitle("Generate captions offline", "Speech is recognized on this device with Whisper (whisper.cpp). Audio never leaves the phone and no network is used.")
        if (!assetsReady) SmallLabel("Preparing the offline speech model (first launch)…")
        else if (model.isEmpty()) {
            Text("No speech model installed.", color = Color(0xFFFF7A7A))
            Chip("Open AI Models") { app.go(Screen.Models) }
        } else SmallLabel("Model: ${File(model).name}")
        if (media.isEmpty()) {
            SmallLabel("Add a video or audio clip with speech first.")
            Chip("Add bundled speech sample") {
                val f = File(ctx.filesDir, "samples/speech.wav")
                if (!f.isFile) app.toast("Sample not found.", true) else { importIntoProject(app, ctx, Uri.fromFile(f)); source = st.selectedLayer?.optString("id") ?: "" }
            }
        } else {
            EnumPicker("Speech source", source, media.map { it.optString("id") to it.optString("name") }) { source = it }
        }
        EnumPicker("Language", lang, listOf("auto" to "Auto-detect", "en" to "English", "es" to "Spanish", "fr" to "French", "de" to "German", "it" to "Italian", "pt" to "Portuguese",
            "nl" to "Dutch", "ja" to "Japanese", "zh" to "Chinese", "ko" to "Korean", "ru" to "Russian", "ar" to "Arabic", "hi" to "Hindi")) { lang = it; Settings.asrLanguage = it }
        NumberRow("Max characters per line", maxChars, 16.0, 60.0, onPreview = {}) { maxChars = it }
        progress?.let { p ->
            Text("Transcribing… ${(p * 100).toInt()}%", fontSize = 13.sp)
            LinearProgressIndicator(progress = { p }, modifier = Modifier.fillMaxWidth().semantics { contentDescription = "Transcription progress" })
            TextButton(onClick = { progress = null }) { Text("Cancel") }
        }
        if (progress == null) Button(enabled = source.isNotEmpty() && model.isNotEmpty(), onClick = {
            progress = 0f
            scope.launch {
                val args = jo("layer" to source, "language" to lang, "maxChars" to maxChars.toInt(), "threads" to Runtime.getRuntime().availableProcessors().coerceIn(1, 4))
                if (model.isNotEmpty()) args.put("model", model)
                val r = withContext(Dispatchers.Default) { NativeBridge.task("transcribe", args, ProgressCallback { p -> progress?.let { progress = maxOf(it, p) }; progress != null }) }
                progress = null
                st.refresh()
                lastRun = when {
                    r.optBoolean("cancelled") -> "Cancelled. Nothing was changed."
                    !r.optBoolean("ok") -> "Failed: " + r.optString("error")
                    else -> "Created ${r.optInt("captions")} captions from ${r.optInt("words")} words (language: ${r.optString("language")}). " +
                        "${"%.1f".format(r.optDouble("audioSeconds"))} s of audio in ${"%.1f".format(r.optDouble("processingSeconds"))} s." +
                        (if (r.optInt("readingIssues") > 0) " ${r.optInt("readingIssues")} captions read too fast — review them below." else "")
                }
                if (r.optBoolean("ok")) app.toast("Captions generated (undo with one step).")
            }
        }, modifier = Modifier.fillMaxWidth().semantics { contentDescription = "Generate captions" }) { Text("Generate captions") }
        if (lastRun.isNotEmpty()) Text(lastRun, fontSize = 13.sp, color = if (lastRun.startsWith("Failed")) Color(0xFFFF7A7A) else Color(0xFF66BB6A))

        if (capLayer != null) {
            SectionTitle("Style")
            FlowRow {
                st.doc.arr("captionStyles").objects().forEach { s -> Chip(s.optString("name"), s.optString("name") == style.optString("name")) { st.op("setCaptionStyle", "style" to s) } }
                Chip("Clean") { st.op("setCaptionStyle", "style" to jo("name" to "Clean", "background" to false, "uppercase" to false, "highlightActiveWord" to true)) }
                Chip("Boxed") { st.op("setCaptionStyle", "style" to jo("name" to "Boxed", "background" to true, "strokeWidth" to 0)) }
                Chip("Bold social") { st.op("setCaptionStyle", "style" to jo("name" to "Bold social", "uppercase" to true, "size" to 80, "strokeWidth" to 8, "highlightColor" to listOf(1.0, 0.85, 0.1, 1.0), "animationIn" to "pop")) }
            }
            LabeledSwitch("Highlight active word", style.optBoolean("highlightActiveWord", true)) { st.op("setCaptionStyle", "style" to jo("highlightActiveWord" to it)) }
            LabeledSwitch("Background box", style.optBoolean("background")) { st.op("setCaptionStyle", "style" to jo("background" to it)) }
            LabeledSwitch("Uppercase", style.optBoolean("uppercase")) { st.op("setCaptionStyle", "style" to jo("uppercase" to it)) }
            NumberRow("Size", style.optDouble("size", 64.0), 20.0, 160.0, onPreview = {}) { st.op("setCaptionStyle", "style" to jo("size" to it)) }
            NumberRow("Vertical position", style.optJSONArray("position")?.optDouble(1) ?: 0.86, 0.05, 0.95, onPreview = {}) { st.op("setCaptionStyle", "style" to jo("position" to listOf(0.5, it))) }
            ColorRow("Text color", style.optJSONArray("color").toDoubles(4)) { st.op("setCaptionStyle", "style" to jo("color" to it.toList())) }
            ColorRow("Highlight color", style.optJSONArray("highlightColor").toDoubles(4)) { st.op("setCaptionStyle", "style" to jo("highlightColor" to it.toList())) }
            EnumPicker("Animation", style.optString("animationIn", "fade"), listOf("none", "fade", "pop", "slide", "typewriter").map { it to it.replaceFirstChar { c -> c.uppercase() } }) {
                st.op("setCaptionStyle", "style" to jo("animationIn" to it, "animationOut" to if (it == "typewriter" || it == "pop") "fade" else it))
            }
            Chip("Save style as preset") { st.op("setCaptionStyle", "style" to JSONObject(), "saveAsPreset" to true, "presetName" to "Custom ${st.doc.arr("captionStyles").length() + 1}") }

            SectionTitle("Find & replace")
            Row(verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(find, { find = it }, label = { Text("Find") }, singleLine = true, modifier = Modifier.weight(1f))
                OutlinedTextField(repl, { repl = it }, label = { Text("Replace") }, singleLine = true, modifier = Modifier.weight(1f))
            }
            Chip("Replace all") { st.op("captionReplace", "find" to find, "replace" to repl)?.let { app.toast("Replaced ${it.optInt("count")} occurrences.") } }

            SectionTitle("Captions (${items.size})", "Tap a caption to jump there; long-edit to change text or timing. Select captions and choose 'Remove from edit' to cut those sections out of the video (edit by transcript).")
            val issues = remember(st.revision) { NativeBridge.call("readingSpeed").arr("issues").objects().associateBy { it.optInt("index") } }
            FlowRow {
                Chip("Export SRT") { saveSub.launch("captions.srt") }
                Chip("Export VTT") { saveSub.launch("captions.vtt") }
                Chip("Export ASS") { saveSub.launch("captions.ass") }
                if (selected.size == 2) Chip("Merge") { val l = selected.toList().sortedBy { s -> items.indexOfFirst { it.optString("id") == s } }; st.op("mergeCaptions", "first" to l[0], "second" to l[1]); selected = emptySet() }
                if (selected.isNotEmpty()) Chip("Remove from edit (${selected.size})") { st.op("editByCaption", "captions" to selected.toList()); selected = emptySet() }
                if (selected.isNotEmpty()) Chip("Delete captions") { st.apply(jo("op" to "batch", "label" to "Delete Captions", "ops" to selected.map { jo("op" to "removeCaption", "caption" to it) })); selected = emptySet() }
            }
            items.forEachIndexed { i, c ->
                val cid = c.optString("id")
                val active = st.playhead >= c.optDouble("start") && st.playhead < c.optDouble("end")
                Row(Modifier.fillMaxWidth().padding(vertical = 2.dp).background(if (active) PanelHi else Color.Transparent, RoundedCornerShape(6.dp))
                    .clickable { st.playhead = c.optDouble("start") + 0.001 }.padding(6.dp), verticalAlignment = Alignment.CenterVertically) {
                    CheckRow("", cid in selected) { selected = if (it) selected + cid else selected - cid }
                    Column(Modifier.weight(1f)) {
                        Text("${st.timecode(c.optDouble("start"))} → ${st.timecode(c.optDouble("end"))}", fontSize = 11.sp, color = TextDim, fontFamily = FontFamily.Monospace)
                        Text(c.optString("text"))
                        issues[i]?.let { Text("⚠ " + it.optString("message"), fontSize = 11.sp, color = Color(0xFFFFB74D)) }
                    }
                    TextButton(onClick = { editing = c }) { Text("Edit") }
                }
            }
        }
    }
    editing?.let { c -> CaptionEditDialog(app, c) { editing = null } }
}

@Composable
private fun CaptionEditDialog(app: AppState, c: JSONObject, onDismiss: () -> Unit) {
    val st = app.editor
    var text by remember { mutableStateOf(c.optString("text")) }
    var start by remember { mutableStateOf(fmt3(c.optDouble("start"))) }
    var end by remember { mutableStateOf(fmt3(c.optDouble("end"))) }
    var speaker by remember { mutableStateOf(c.optInt("speaker").toString()) }
    AlertDialog(onDismissRequest = onDismiss, title = { Text("Edit caption") }, text = {
        Column {
            OutlinedTextField(text, { text = it }, label = { Text("Text") }, minLines = 2, modifier = Modifier.fillMaxWidth())
            Row {
                OutlinedTextField(start, { start = it }, label = { Text("Start s") }, singleLine = true, modifier = Modifier.weight(1f), keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal))
                OutlinedTextField(end, { end = it }, label = { Text("End s") }, singleLine = true, modifier = Modifier.weight(1f), keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal))
            }
            OutlinedTextField(speaker, { speaker = it }, label = { Text("Speaker #") }, singleLine = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
            Row {
                TextButton(onClick = { st.op("splitCaption", "caption" to c.optString("id"), "t" to st.playhead.takeIf { it > c.optDouble("start") && it < c.optDouble("end") }); onDismiss() }) { Text("Split at playhead") }
                TextButton(onClick = { st.op("removeCaption", "caption" to c.optString("id")); onDismiss() }) { Text("Delete") }
            }
        }
    }, confirmButton = { TextButton(onClick = {
        val s = start.toDoubleOrNull(); val e = end.toDoubleOrNull()
        if (s == null || e == null || e <= s) app.toast("End must be after start.", true)
        else { st.op("updateCaption", "caption" to c.optString("id"), "fields" to jo("text" to text, "start" to s, "end" to e, "speaker" to (speaker.toIntOrNull() ?: 0))); onDismiss() }
    }) { Text("Save") } }, dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } })
}

private fun fmt3(v: Double) = "%.3f".format(v)

// ====================================================================== Script Studio
/** Hand-off for a script opened from a .mfsx package. */
object ScriptInbox {
    var name = ""
    var source: String? = null
    var permissions: List<String> = emptyList()
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ScriptStudioScreen(app: AppState) {
    val st = app.editor
    val examples = remember { NativeBridge.call("exampleScripts").arr("scripts").objects() }
    val api = remember { NativeBridge.call("scriptApi").obj("api") }
    val inbox = remember { ScriptInbox.source?.let { src -> Triple(ScriptInbox.name, src, ScriptInbox.permissions.toSet()).also { ScriptInbox.source = null } } }
    var name by remember { mutableStateOf(inbox?.first ?: examples.firstOrNull()?.optString("name") ?: "My Script") }
    var source by remember { mutableStateOf(inbox?.second ?: examples.firstOrNull()?.optString("source") ?: "mf.log('hello');\n") }
    var perms by remember { mutableStateOf(inbox?.third ?: examples.firstOrNull()?.arr("permissions")?.strings()?.toSet() ?: setOf("PROJECT_READ")) }
    var console by remember { mutableStateOf("") }
    var argsText by remember { mutableStateOf("{}") }
    var confirmRun by remember { mutableStateOf(false) }
    var showApi by remember { mutableStateOf(false) }
    // A script can declare a panel of inputs (mf.ui.panel); the values are passed back in mf.args on the next run.
    var panel by remember { mutableStateOf<JSONArray?>(null) }
    val panelValues = remember { mutableStateOf(JSONObject()) }
    val ctx = LocalContext.current
    val importMedia = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris -> uris.forEach { importIntoProject(app, ctx, it) } }
    fun handleActions(actions: JSONArray) {
        actions.objects().forEach { a ->
            when (a.optString("type")) {
                "panel" -> { panel = a.obj("spec").optJSONArray("fields") ?: JSONArray(); val v = JSONObject(); panel!!.objects().forEach { f -> v.put(f.optString("name"), f.opt("default") ?: "") }; panelValues.value = v }
                "select" -> st.selection = a.arr("layers").strings().filter { st.layer(it) != null }.toSet()
                "importRequest" -> importMedia.launch(arrayOf("video/*", "image/*", "audio/*"))
                "export" -> app.go(Screen.Export)
            }
        }
    }
    fun run() {
        val args = try { JSONObject(argsText) } catch (e: Exception) { app.toast("Arguments must be a JSON object.", true); return }
        panelValues.value.keys().forEach { k -> if (!args.has(k)) args.put(k, panelValues.value.opt(k)) }
        val r = NativeBridge.call("runScript", jo("name" to name, "source" to source, "permissions" to perms.toList(), "selection" to st.selection.toList(), "t" to st.playhead,
            "storage" to JSONObject(Settings.scriptStorage), "args" to args))
        val sb = StringBuilder()
        r.arr("logs").strings().forEach { sb.append(it).append('\n') }
        r.arr("alerts").strings().forEach { sb.append("[alert] ").append(it).append('\n') }
        if (r.optBoolean("ok")) {
            Settings.scriptStorage = r.obj("storage").toString()
            sb.append("✓ Finished in ${r.optDouble("ms").toInt()} ms; ${r.optInt("ops")} operations" + (if (r.optBoolean("changed")) " (one undo step: \"Run Script: $name\")" else ", project unchanged"))
            if (r.has("returnValue") && !r.isNull("returnValue")) sb.append("\nreturn: ").append(r.opt("returnValue"))
        } else sb.append("✗ ").append(r.optString("error"))
        console = sb.toString()
        st.refresh()
        r.optJSONArray("actions")?.let { handleActions(it) }
    }
    ScreenScaffold(app, "Script Studio", actions = { TextButton(onClick = { showApi = true }) { Text("API") } }) {
        SmallLabel("Scripts run in a sandbox (QuickJS) with only the permissions you grant. Each run is a single undoable step.")
        FlowRow { examples.forEach { e -> Chip(e.optString("name"), name == e.optString("name")) { name = e.optString("name"); source = e.optString("source"); perms = e.arr("permissions").strings().toSet() } } }
        OutlinedTextField(name, { name = it }, label = { Text("Script name") }, singleLine = true, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(source, { source = it }, modifier = Modifier.fillMaxWidth().height(260.dp).semantics { contentDescription = "Script source" },
            textStyle = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 13.sp, color = Color.White))
        OutlinedTextField(argsText, { argsText = it }, label = { Text("Arguments (mf.args JSON)") }, singleLine = true, modifier = Modifier.fillMaxWidth())
        panel?.let { fields ->
            SectionTitle("Script panel", "Inputs declared by the script with mf.ui.panel(). Run again to use them.")
            fields.objects().forEach { f ->
                val n = f.optString("name")
                val label = f.optString("label", n)
                when (f.optString("type")) {
                    "number" -> NumberRow(label, panelValues.value.optDouble(n, 0.0), f.optDouble("min", 0.0), f.optDouble("max", 100.0), onPreview = {}) { v -> panelValues.value = JSONObject(panelValues.value.toString()).put(n, v) }
                    "bool" -> LabeledSwitch(label, panelValues.value.optBoolean(n)) { v -> panelValues.value = JSONObject(panelValues.value.toString()).put(n, v) }
                    "color" -> ColorRow(label, panelValues.value.optJSONArray(n).toDoubles(4)) { v -> panelValues.value = JSONObject(panelValues.value.toString()).put(n, JSONArray(v.toList())) }
                    "choice" -> EnumPicker(label, panelValues.value.optString(n), f.arr("options").strings().map { it to it }) { v -> panelValues.value = JSONObject(panelValues.value.toString()).put(n, v) }
                    else -> OutlinedTextField(panelValues.value.optString(n), { v -> panelValues.value = JSONObject(panelValues.value.toString()).put(n, v) }, label = { Text(label) }, singleLine = true, modifier = Modifier.fillMaxWidth())
                }
            }
        }
        SectionTitle("Permissions")
        FlowRow { st.registries.arr("permissions").strings().forEach { p -> Chip(p, p in perms) { perms = if (p in perms) perms - p else perms + p } } }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = {
                val v = NativeBridge.call("validateScript", jo("source" to source))
                console = if (v.optBoolean("ok")) "✓ Syntax OK" else "✗ Line ${v.optInt("line")}: ${v.optString("error")}"
            }) { Text("Check") }
            Button(onClick = { if (perms.any { it.endsWith("WRITE") || it == "FILE_EXPORT" }) confirmRun = true else run() }, modifier = Modifier.semantics { contentDescription = "Run script" }) { Text("Run") }
            TextButton(onClick = { console = "" }) { Text("Clear console") }
        }
        Box(Modifier.fillMaxWidth().background(Color.Black, RoundedCornerShape(6.dp)).padding(8.dp)) {
            Text(console.ifEmpty { "Console output appears here." }, fontFamily = FontFamily.Monospace, fontSize = 12.sp, color = if (console.contains("✗")) Color(0xFFFF7A7A) else Color(0xFFB0F0B0))
        }
    }
    if (confirmRun) ConfirmDialog("Allow script to modify the project?", "\"$name\" requests: ${perms.joinToString()}. You can undo it in one step.", "Run",
        onDismiss = { confirmRun = false }) { confirmRun = false; run() }
    if (showApi) InfoDialog("Script API v${api.optInt("apiVersion")} (${api.arr("functions").length()} functions)",
        api.arr("functions").strings().joinToString("\n") + "\n\n" + api.arr("globals").objects().joinToString("\n\n") { g ->
            "${g.optString("name")} ${g.optString("signature")}\n  ${g.optString("doc")}" + (g.optString("permission").takeIf { it.isNotEmpty() }?.let { "  [requires $it]" } ?: "")
        }) { showApi = false }
}

// ====================================================================== Extensions
@Composable
fun ExtensionsScreen(app: AppState) {
    val ctx = LocalContext.current
    var refresh by remember { mutableIntStateOf(0) }
    val list = remember(refresh) { NativeBridge.call("extensions").arr("extensions").objects() }
    var pending by remember { mutableStateOf<Pair<String, JSONObject>?>(null) }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            val f = Importer.copyToLocal(ctx, uri, "incoming") ?: return@rememberLauncherForActivityResult
            val info = NativeBridge.call("packageInfo", jo("path" to f.absolutePath))
            if (!info.optBoolean("ok")) app.toast(info.optString("error"), true)
            else if (info.optString("kind") != "extension") app.toast("This file is not an extension package (.mfext).", true)
            else pending = f.absolutePath to info
        }
    }
    ScreenScaffold(app, "Extensions", actions = { TextButton(onClick = { pick.launch(arrayOf("*/*")) }) { Text("Install…") } }) {
        SmallLabel("Extensions add scripts, presets and composite effects. They run sandboxed with declared permissions. Native code is not allowed in this build.")
        if (Settings.developerMode) Text("Developer Mode is on: unsigned extensions can be installed.", color = Color(0xFFFFB74D), fontSize = 12.sp)
        if (list.isEmpty()) Text("No extensions installed.", color = TextDim)
        list.forEach { e ->
            val id = e.optString("_id")
            Column(Modifier.fillMaxWidth().padding(vertical = 4.dp).background(PanelHi, RoundedCornerShape(8.dp)).padding(8.dp)) {
                Text(e.optString("name", id), fontWeight = FontWeight.Bold)
                Text("v${e.optString("version")} • ${e.optString("author")}", fontSize = 12.sp, color = TextDim)
                Text(e.optString("description"), fontSize = 13.sp)
                Text("Permissions: " + e.arr("permissions").strings().joinToString().ifEmpty { "none" }, fontSize = 12.sp, color = TextDim)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    // weight(): LabeledSwitch fills its width and would otherwise push Uninstall off-screen.
                    Box(Modifier.weight(1f)) { LabeledSwitch("Enabled", e.optBoolean("_enabled")) { NativeBridge.call("setExtensionEnabled", jo("id" to id, "enabled" to it)); app.editor.reloadRegistries(); refresh++ } }
                    TextButton(onClick = { NativeBridge.call("uninstallExtension", jo("id" to id)); app.editor.reloadRegistries(); refresh++ }) { Text("Uninstall") }
                }
            }
        }
    }
    pending?.let { (path, info) ->
        val m = info.obj("manifest")
        AlertDialog(onDismissRequest = { pending = null }, title = { Text("Install ${m.optString("name")}?") }, text = {
            Column {
                Text("Publisher: ${m.optString("author")}  •  v${m.optString("version")}")
                Text(if (info.optBoolean("signed")) (if (info.optBoolean("signatureValid")) "Signed (signature valid). Signer: ${info.optString("signer").take(16)}…" else "Signature INVALID") else "Unsigned package",
                    color = if (info.optBoolean("signed") && info.optBoolean("signatureValid")) Color(0xFF66BB6A) else Color(0xFFFFB74D))
                Text("Requested permissions:", fontWeight = FontWeight.Bold)
                m.arr("permissions").strings().forEach { Text("• $it") }
            }
        }, confirmButton = { TextButton(onClick = {
            val r = NativeBridge.call("installExtension", jo("path" to path, "confirmed" to true, "devMode" to Settings.developerMode, "trustedKeys" to Settings.trustedKeys.toList()))
            app.toast(if (r.optBoolean("ok")) "Extension installed." else r.optString("error"), !r.optBoolean("ok"))
            app.editor.reloadRegistries()
            pending = null; refresh++
        }) { Text("Install") } }, dismissButton = { TextButton(onClick = { pending = null }) { Text("Cancel") } })
    }
}

// ====================================================================== Library
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun LibraryScreen(app: AppState) {
    val ctx = LocalContext.current
    var tab by remember { mutableStateOf("presets") }
    var refresh by remember { mutableIntStateOf(0) }
    var query by remember { mutableStateOf("") }
    val presets = remember(refresh) { NativeBridge.call("presets").arr("presets").objects() }
    val capsules = remember(refresh) { NativeBridge.call("capsules").arr("capsules").objects() }
    val templates = remember(refresh) { NativeBridge.call("templates").arr("templates").objects() }
    var exportItem by remember { mutableStateOf<Pair<String, JSONObject>?>(null) }
    var rename by remember { mutableStateOf<JSONObject?>(null) }
    var templateFor by remember { mutableStateOf<JSONObject?>(null) }
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        val item = exportItem ?: return@rememberLauncherForActivityResult
        if (uri != null) {
            val tmp = File(ctx.cacheDir, "lib-export.tmp")
            val r = if (item.first == "capsule") {
                // Capsules embed their fonts and media; content:// media is copied to a local file first.
                val files = JSONObject()
                item.second.optJSONArray("assets")?.objects()?.forEach { a ->
                    val src = a.optString("uri").ifEmpty { a.optString("path") }
                    if (src.startsWith("content:") || src.startsWith("file:"))
                        Importer.copyToLocal(ctx, Uri.parse(src), "capsule-export")?.let { files.put(a.optString("id"), it.absolutePath) }
                }
                NativeBridge.call("exportCapsulePackage", jo("path" to tmp.absolutePath, "capsule" to item.second, "assetFiles" to files, "signingKey" to Settings.signingSecret))
                    .also { res -> res.optJSONArray("warnings")?.strings()?.forEach { w -> app.toast(w) } }
            } else NativeBridge.call("exportJsonPackage", jo("path" to tmp.absolutePath, "kind" to item.first, "content" to item.second,
                "manifest" to jo("name" to item.second.optString("name")), "signingKey" to Settings.signingSecret))
            if (r.optBoolean("ok")) { ctx.contentResolver.openOutputStream(uri)?.use { o -> tmp.inputStream().use { it.copyTo(o) } }; app.toast("Exported.") } else app.toast(r.optString("error"), true)
            tmp.delete()
        }
        exportItem = null
    }
    val open = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> if (uri != null) { handlePackageImport(app, ctx, uri, Importer.classify(ctx, uri)); refresh++ } }
    ScreenScaffold(app, "Library", actions = { TextButton(onClick = { open.launch(arrayOf("*/*")) }) { Text("Import…") } }) {
        Row { listOf("presets" to "Presets", "capsules" to "Capsules", "templates" to "Templates").forEach { (k, n) -> Chip(n, tab == k) { tab = k } } }
        OutlinedTextField(query, { query = it }, placeholder = { Text("Search") }, singleLine = true, modifier = Modifier.fillMaxWidth())
        when (tab) {
            "presets" -> {
                SmallLabel("Save presets from a layer's Effects panel. Apply them from Effects › Apply preset.")
                presets.filter { query.isBlank() || it.optString("name").contains(query, true) }.sortedByDescending { it.optBoolean("favorite") }.forEach { p ->
                    val file = p.optString("_file")
                    LibRow(p.optString("name"), p.arr("tags").strings().joinToString(), p.optBoolean("favorite"),
                        onFav = { NativeBridge.call("updatePreset", jo("file" to file, "fields" to jo("favorite" to !p.optBoolean("favorite")))); refresh++ },
                        onRename = { rename = p }, onDup = { NativeBridge.call("duplicatePreset", jo("file" to file)); refresh++ },
                        onExport = { exportItem = "preset" to p; save.launch(p.optString("name") + ".mffx") },
                        onDelete = { NativeBridge.call("deletePreset", jo("file" to file)); refresh++ })
                }
                if (presets.isEmpty()) Text("No presets yet.", color = TextDim)
            }
            "capsules" -> {
                SmallLabel("Capsules are reusable animated graphics with simple controls. Create them from Layer › Create Capsule.")
                capsules.filter { query.isBlank() || it.optString("name").contains(query, true) }.forEach { c ->
                    LibRow(c.optString("name"), "${c.arr("controls").length()} controls", null, onFav = {}, onRename = null, onDup = null,
                        onExport = { exportItem = "capsule" to c; save.launch(c.optString("name") + ".mfcapsule") },
                        onDelete = { NativeBridge.call("deleteCapsule", jo("file" to c.optString("_file"))); refresh++ })
                }
                if (capsules.isEmpty()) Text("No capsules yet.", color = TextDim)
            }
            else -> {
                SmallLabel("Templates are whole projects with replaceable media/text placeholders. Save one from the editor's More menu.")
                templates.filter { query.isBlank() || it.optString("name").contains(query, true) }.forEach { t ->
                    Row(Modifier.fillMaxWidth().padding(vertical = 4.dp).background(PanelHi, RoundedCornerShape(8.dp)).padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) { Text(t.optString("name"), fontWeight = FontWeight.Bold); Text("${t.arr("placeholders").length()} placeholders", fontSize = 12.sp, color = TextDim) }
                        TextButton(onClick = { templateFor = t }) { Text("Use") }
                    }
                }
                if (templates.isEmpty()) Text("No templates yet.", color = TextDim)
            }
        }
    }
    rename?.let { p -> TextInputDialog("Rename preset", p.optString("name"), onDismiss = { rename = null }) { n -> NativeBridge.call("updatePreset", jo("file" to p.optString("_file"), "fields" to jo("name" to n))); refresh++ } }
    templateFor?.let { t ->
        val values = remember { JSONObject() }
        AlertDialog(onDismissRequest = { templateFor = null }, title = { Text("New from '${t.optString("name")}'") }, text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                t.arr("placeholders").objects().filter { it.optString("type") == "text" }.forEach { ph ->
                    var v by remember { mutableStateOf(ph.optString("default")) }
                    OutlinedTextField(v, { v = it; values.put(ph.optString("id"), it) }, label = { Text(ph.optString("name")) }, singleLine = true)
                }
                SmallLabel("Media placeholders can be replaced in the editor (Layer › Replace source).")
            }
        }, confirmButton = { TextButton(onClick = {
            val r = NativeBridge.call("newFromTemplate", jo("file" to t.optString("_file"), "values" to values))
            templateFor = null
            if (!r.optBoolean("ok")) app.toast(r.optString("error"), true) else app.openProject(r.optString("id"))?.let { app.toast(it, true) }
        }) { Text("Create") } }, dismissButton = { TextButton(onClick = { templateFor = null }) { Text("Cancel") } })
    }
}

@Composable
private fun LibRow(name: String, sub: String, fav: Boolean?, onFav: () -> Unit, onRename: (() -> Unit)?, onDup: (() -> Unit)?, onExport: () -> Unit, onDelete: () -> Unit) {
    Row(Modifier.fillMaxWidth().padding(vertical = 4.dp).background(PanelHi, RoundedCornerShape(8.dp)).padding(6.dp), verticalAlignment = Alignment.CenterVertically) {
        if (fav != null) IconBtn(if (fav) Icons.Filled.Star else Icons.Outlined.StarOutline, if (fav) "Unfavorite $name" else "Favorite $name", tint = if (fav) KeyColor else TextDim, onClick = onFav)
        Column(Modifier.weight(1f)) { Text(name, fontWeight = FontWeight.Bold); if (sub.isNotEmpty()) Text(sub, fontSize = 12.sp, color = TextDim) }
        onRename?.let { TextButton(onClick = it) { Text("Rename") } }
        onDup?.let { TextButton(onClick = it) { Text("Duplicate") } }
        TextButton(onClick = onExport) { Text("Export") }
        IconBtn(Icons.Filled.Delete, "Delete $name", onClick = onDelete)
    }
}

/** Imports presets, capsules, templates, scripts and extensions from package files. */
fun handlePackageImport(app: AppState, ctx: android.content.Context, uri: Uri, kind: Importer.Kind) {
    val f = Importer.copyToLocal(ctx, uri, "incoming") ?: return app.toast("Could not read the file.", true)
    val pkgKind = when (kind) {
        Importer.Kind.PRESET -> "preset"; Importer.Kind.CAPSULE -> "capsule"; Importer.Kind.TEMPLATE -> "template"; Importer.Kind.SCRIPT -> "script"
        Importer.Kind.EXTENSION -> { app.go(Screen.Extensions); app.toast("Use Install… to review and install the extension."); return }
        Importer.Kind.PROJECT -> { app.toast("Open project packages from the Home screen (Import Package).", true); return }
        else -> { app.toast("Unsupported file type: ${f.name}", true); return }
    }
    if (pkgKind == "capsule") {
        val c = NativeBridge.call("importCapsulePackage", jo("path" to f.absolutePath))
        if (!c.optBoolean("ok")) return app.toast("Import failed: " + c.optString("error"), true)
        app.toast("Capsule '${c.obj("capsule").optString("name")}' added (${c.optInt("fonts")} fonts). Insert it from + Layer › Capsules.")
        c.arr("warnings").strings().forEach { app.toast(it) }
        return
    }
    val r = NativeBridge.call("importJsonPackage", jo("path" to f.absolutePath, "kind" to pkgKind, "devMode" to true, "trustedKeys" to Settings.trustedKeys.toList()))
    if (!r.optBoolean("ok")) return app.toast("Import failed: " + r.optString("error"), true)
    val content = r.obj("content")
    when (pkgKind) {
        "preset" -> {
            File(ctx.filesDir, "mf/presets").mkdirs()
            File(ctx.filesDir, "mf/presets/" + content.optString("name", "Imported").replace(Regex("[^A-Za-z0-9 _-]"), "_") + ".json").writeText(content.toString())
            app.toast("Preset '${content.optString("name")}' added to the library.")
        }
        "template" -> {
            File(ctx.filesDir, "mf/templates").mkdirs()
            File(ctx.filesDir, "mf/templates/" + content.optString("name", "Imported").replace(Regex("[^A-Za-z0-9 _-]"), "_") + ".json").writeText(content.toString())
            app.toast("Template '${content.optString("name")}' added to the library.")
        }
        "script" -> {
            ScriptInbox.name = content.optString("name", r.obj("manifest").optString("name", "Imported script"))
            ScriptInbox.source = content.optString("source")
            ScriptInbox.permissions = content.arr("permissions").strings().ifEmpty { r.obj("manifest").arr("permissions").strings() }
            app.go(Screen.Scripts); app.toast("Script imported. Review its code and permissions before running.")
        }
    }
    r.arr("warnings").strings().forEach { app.toast(it) }
}

/** Exports a project as a .mforge package (collected / linked / portable; optional encryption and signing). */
@Composable
fun PackageExportDialog(app: AppState, onDismiss: () -> Unit, projectId: String) {
    val ctx = LocalContext.current
    var mode by remember { mutableStateOf("collected") }
    var password by remember { mutableStateOf("") }
    var sign by remember { mutableStateOf(Settings.signingSecret.isNotEmpty()) }
    var busy by remember { mutableStateOf(false) }
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri == null) { onDismiss(); return@rememberLauncherForActivityResult }
        busy = true
        val wasOpen = app.editor.projectId == projectId
        if (!wasOpen) NativeBridge.call("openProject", jo("id" to projectId))
        else app.editor.save()
        val tmp = File(ctx.cacheDir, "pkg-export.mforge")
        val r = NativeBridge.call("exportProject", jo("path" to tmp.absolutePath, "mode" to mode, "password" to password, "signingKey" to if (sign) Settings.signingSecret else ""))
        if (!wasOpen) NativeBridge.call("closeProject")
        if (r.optBoolean("ok")) {
            ctx.contentResolver.openOutputStream(uri)?.use { o -> tmp.inputStream().use { it.copyTo(o) } }
            app.toast("Project package exported (${tmp.length() / 1024} KB).")
        } else app.toast("Package export failed: " + r.optString("error"), true)
        tmp.delete()
        busy = false
        onDismiss()
    }
    AlertDialog(onDismissRequest = onDismiss, title = { Text("Export project package") }, text = {
        Column {
            EnumPicker("Media", mode, listOf("collected" to "Collect all media (portable)", "linked" to "Link media (small file)", "portable" to "Proxies only (smallest portable)")) { mode = it }
            OutlinedTextField(password, { password = it }, label = { Text("Password (optional, encrypts the package)") }, singleLine = true, visualTransformation = PasswordVisualTransformation())
            if (Settings.signingSecret.isNotEmpty()) CheckRow("Sign with my publisher key", sign) { sign = it }
            if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        }
    }, confirmButton = { TextButton(enabled = !busy, onClick = { save.launch("project.mforge") }) { Text("Export…") } }, dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } })
}

// ====================================================================== Fonts
@Composable
fun FontsScreen(app: AppState) {
    val ctx = LocalContext.current
    var refresh by remember { mutableIntStateOf(0) }
    var query by remember { mutableStateOf("") }
    var license by remember { mutableStateOf<String?>(null) }
    val fonts = remember(refresh) { NativeBridge.call("fonts").arr("fonts").objects() }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            val f = Importer.copyToLocal(ctx, uri, "fonts") ?: return@rememberLauncherForActivityResult
            val r = NativeBridge.call("registerFont", jo("path" to f.absolutePath, "name" to f.nameWithoutExtension))
            app.toast(if (r.optBoolean("ok")) "Font added. Importing a font does not grant a license to use it." else r.optString("error"), !r.optBoolean("ok"))
            refresh++
        }
    }
    ScreenScaffold(app, "Fonts", actions = { TextButton(onClick = { pick.launch(arrayOf("font/*", "application/octet-stream", "*/*")) }) { Text("Import…") } }) {
        OutlinedTextField(query, { query = it }, placeholder = { Text("Search fonts") }, singleLine = true, modifier = Modifier.fillMaxWidth())
        val favs = Settings.favoriteFonts
        fonts.filter { query.isBlank() || it.optString("name").contains(query, true) }.sortedByDescending { it.optString("name") in favs }.forEach { f ->
            val n = f.optString("name")
            Row(Modifier.fillMaxWidth().padding(vertical = 3.dp).background(PanelHi, RoundedCornerShape(8.dp)).padding(6.dp), verticalAlignment = Alignment.CenterVertically) {
                IconBtn(if (n in favs) Icons.Filled.Star else Icons.Outlined.StarOutline, "Favorite $n", tint = if (n in favs) KeyColor else TextDim) {
                    Settings.favoriteFonts = if (n in favs) favs - n else favs + n; refresh++
                }
                Column(Modifier.weight(1f)) {
                    Text(n, fontWeight = FontWeight.Bold)
                    Text(f.optString("family") + (if (f.optBoolean("bundled")) " • bundled (DejaVu, free license)" else " • imported"), fontSize = 12.sp, color = TextDim)
                    Settings.fontLicenseNote(n).takeIf { it.isNotEmpty() }?.let { Text("License: $it", fontSize = 12.sp, color = TextDim) }
                }
                TextButton(onClick = { license = n }) { Text("License note") }
                if (!f.optBoolean("bundled")) IconBtn(Icons.Filled.Delete, "Remove font $n") { NativeBridge.call("unregisterFont", jo("name" to n)); refresh++ }
            }
        }
    }
    license?.let { n -> TextInputDialog("License note for $n", Settings.fontLicenseNote(n), onDismiss = { license = null }) { Settings.setFontLicenseNote(n, it) } }
}

// ====================================================================== AI Models
@Composable
fun ModelsScreen(app: AppState) {
    val ctx = LocalContext.current
    var refresh by remember { mutableIntStateOf(0) }
    val dir = File(ctx.filesDir, "models").apply { mkdirs() }
    val models = remember(refresh) { dir.listFiles()?.filter { it.isFile && it.name.endsWith(".bin") }?.sortedBy { it.name } ?: emptyList() }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            val f = Importer.copyToLocal(ctx, uri, "models")
            if (f != null) {
                val info = NativeBridge.call("modelInfo", jo("path" to f.absolutePath)).obj("info")
                if (!info.optBoolean("ok")) { f.delete(); app.toast("Not a valid Whisper (ggml) model: " + info.optString("error"), true) } else app.toast("Model installed.")
                refresh++
            }
        }
    }
    ScreenScaffold(app, "AI Models", actions = { TextButton(onClick = { pick.launch(arrayOf("*/*")) }) { Text("Import model…") } }) {
        SmallLabel("Speech-to-text runs fully offline with Whisper (whisper.cpp, ggml format). A small English model is bundled. Import larger or multilingual ggml models (e.g. ggml-base.bin, ggml-small-q5_1.bin) from your files; there are no downloads in the app.")
        val active = Settings.asrModel.ifEmpty { models.firstOrNull()?.absolutePath ?: "" }
        models.forEach { m ->
            val info = remember(m.absolutePath, refresh) { NativeBridge.call("modelInfo", jo("path" to m.absolutePath)).obj("info") }
            Column(Modifier.fillMaxWidth().padding(vertical = 4.dp).background(PanelHi, RoundedCornerShape(8.dp)).padding(8.dp)) {
                Text(m.name + if (m.absolutePath == active) "  (active)" else "", fontWeight = FontWeight.Bold)
                Text("${info.optString("type")} • ${if (info.optBoolean("multilingual")) "multilingual" else "English-only"} • ${"%.0f".format(m.length() / 1e6)} MB" +
                    (if (info.optBoolean("quantized")) " • quantized" else "") + (if (!info.optBoolean("ok")) " • INVALID: ${info.optString("error")}" else ""), fontSize = 12.sp, color = TextDim)
                Row {
                    if (m.absolutePath != active) TextButton(onClick = { Settings.asrModel = m.absolutePath; refresh++ }) { Text("Use") }
                    TextButton(onClick = { if (Settings.asrModel == m.absolutePath) Settings.asrModel = ""; m.delete(); refresh++ }) { Text("Delete") }
                }
            }
        }
        if (models.isEmpty()) Text("No models installed. Captions cannot be generated until a model is imported.", color = Color(0xFFFF7A7A))
        SectionTitle("Storage")
        SmallLabel("Models use ${"%.0f".format(models.sumOf { it.length() } / 1e6)} MB. Free: ${dir.usableSpace / 1_000_000} MB.")
    }
}

// ====================================================================== Settings
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun SettingsScreen(app: AppState) {
    var tick by remember { mutableIntStateOf(0) }
    var keyDialog by remember { mutableStateOf(false) }
    var trustKey by remember { mutableStateOf(false) }
    ScreenScaffold(app, "Settings") {
        key(tick) {
            SectionTitle("Editing")
            EnumPicker("Editing style", Settings.editingStyle, listOf("quick" to "Quick (simple)", "pro" to "Pro (all controls)")) { Settings.editingStyle = it; tick++ }
            LabeledSwitch("Timeline snapping", Settings.snapping) { Settings.snapping = it; tick++ }
            LabeledSwitch("Haptic feedback", Settings.haptics) { Settings.haptics = it; tick++ }
            NumberRow("Default still duration (s)", Settings.defaultStillDuration.toDouble(), 0.5, 30.0, onPreview = {}) { Settings.defaultStillDuration = it.toFloat(); tick++ }
            NumberRow("Autosave interval (s)", Settings.autosaveSeconds.toDouble(), 5.0, 120.0, onPreview = {}) { Settings.autosaveSeconds = it.toInt(); tick++ }
            SectionTitle("Preview & performance")
            EnumPicker("Preview quality", Settings.previewQuality, listOf("auto" to "Adaptive", "full" to "Full", "half" to "Half", "quarter" to "Quarter")) {
                Settings.previewQuality = it; app.player?.mode = it; app.player?.requestRender(); tick++
            }
            Chip("Clear render caches") { NativeBridge.call("clearCaches"); app.toast("Caches cleared.") }
            Chip("Performance monitor") { app.go(Screen.Performance) }
            SectionTitle("Accessibility")
            NumberRow("Interface scale", Settings.uiScale.toDouble(), 0.8, 1.6, onPreview = {}) { Settings.uiScale = it.toFloat(); tick++ }
            LabeledSwitch("Reduced motion", Settings.reducedMotion) { Settings.reducedMotion = it; tick++ }
            LabeledSwitch("High contrast", Settings.highContrast) { Settings.highContrast = it; tick++ }
            SectionTitle("Speech recognition")
            Chip("AI Models (Whisper)") { app.go(Screen.Models) }
            SectionTitle("Developer")
            LabeledSwitch("Developer mode (allow unsigned extensions)", Settings.developerMode) { Settings.developerMode = it; tick++ }
            if (Settings.signingPublic.isNotEmpty()) Text("My publisher key: ${Settings.signingPublic.take(24)}…", fontSize = 12.sp, fontFamily = FontFamily.Monospace)
            FlowRow {
                Chip(if (Settings.signingPublic.isEmpty()) "Create publisher key" else "Replace publisher key") { keyDialog = true }
                Chip("Trust a publisher key") { trustKey = true }
            }
            Settings.trustedKeys.forEach { k ->
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(k.take(24) + "…", fontFamily = FontFamily.Monospace, fontSize = 12.sp, modifier = Modifier.weight(1f))
                    TextButton(onClick = { Settings.trustedKeys = Settings.trustedKeys - k; tick++ }) { Text("Revoke") }
                }
            }
            Chip("Developer Center") { app.go(Screen.DevCenter) }
            SectionTitle("About")
            val caps = remember { NativeBridge.call("capabilities").obj("caps") }
            SmallLabel("MOTIONFORGE Mobile • engine ${caps.optString("engine")} • project format v${caps.optInt("formatVersion")} • ${caps.optString("renderer").uppercase()} renderer, ${caps.optInt("threads")} threads • ${caps.optInt("effects")} effects • offline speech: ${if (caps.optBoolean("asr")) "Whisper" else "unavailable"}")
            SmallLabel("No account, no network access. All projects, media and models stay on this device.")
        }
    }
    if (keyDialog) ConfirmDialog("Create publisher key?", "A new Ed25519 key pair is generated on this device to sign your packages. Replacing it means previous recipients must trust the new key.", "Create",
        onDismiss = { keyDialog = false }) {
        val k = NativeBridge.call("keygen"); Settings.signingSecret = k.optString("secret"); Settings.signingPublic = k.optString("public"); keyDialog = false; tick++
    }
    if (trustKey) TextInputDialog("Trusted publisher public key (hex)", "", onDismiss = { trustKey = false }) { k ->
        if (k.trim().length == 64) { Settings.trustedKeys = Settings.trustedKeys + k.trim(); tick++ } else app.toast("Public keys are 64 hex characters.", true)
    }
}

@Composable
private fun key(k: Any, content: @Composable () -> Unit) = androidx.compose.runtime.key(k) { content() }

// ====================================================================== Developer Center
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun DevCenterScreen(app: AppState) {
    var section by remember { mutableStateOf("Overview") }
    val reg = remember { NativeBridge.call("registries") }
    val api = remember { NativeBridge.call("scriptApi").obj("api") }
    var scenario by remember { mutableStateOf("CREATE project 640x360 30fps duration 2\nADD text \"Hello\"\nEXPECT layers == 1\nUNDO\nEXPECT layers == 0\nREDO\nEXPECT layers == 1\nRENDER frame.png 0.5\nSAVE\nREOPEN\nEXPECT layers == 1\n") }
    var scenarioOut by remember { mutableStateOf("") }
    ScreenScaffold(app, "Developer Center") {
        // FlowRow: seven tabs do not fit a phone width; a plain Row clipped "Scenarios" off-screen.
        FlowRow(Modifier.fillMaxWidth()) { listOf("Overview", "Operations", "Effects", "Scripting", "Expressions", "Formats", "Scenarios").forEach { s -> Chip(s, section == s) { section = s } } }
        Gap()
        when (section) {
            "Overview" -> DocText("""MOTIONFORGE is built from a C++ engine (project model, command/undo layer, CPU compositor, audio mixer, Whisper ASR, packages) and a Kotlin/Compose shell.

Every edit is an operation applied through the command layer; each produces exactly one undo step. Gestures preview live and commit once on release.

Projects are JSON documents (format "mforge", version shown in Settings › About). Saves are atomic, an edit journal with CRC32 checksums enables crash recovery, and rolling versions are kept.

Packages (.mforge, .mffx, .mfcapsule, .mftemplate, .mfsx, .mfext) are zip archives with a manifest and BLAKE2b checksums; they can be signed (Ed25519) and encrypted (Argon2 + XChaCha20-Poly1305).""")
            "Operations" -> DocText("Operations accepted by the command layer (mf.op in scripts):\n\n" + reg.arr("ops").strings().joinToString("\n") { "• $it" })
            "Effects" -> reg.arr("effects").objects().forEach { e ->
                Text("${e.optString("name")}  (${e.optString("type")})", fontWeight = FontWeight.Bold)
                Text(e.optString("help") + "\nParameters: " + e.arr("params").objects().joinToString { it.optString("name") }, fontSize = 12.sp, color = TextDim)
                Gap(4)
            }
            "Scripting" -> DocText("Script API v${api.optInt("apiVersion")}\n\n" + api.arr("functions").strings().joinToString("\n") + "\n\n" + api.arr("globals").objects().joinToString("\n\n") { "${it.optString("name")} ${it.optString("signature")}\n  ${it.optString("doc")}" } +
                "\n\nPermissions: " + reg.arr("permissions").strings().joinToString())
            "Expressions" -> DocText("""Expressions are JavaScript evaluated per frame (QuickJS).

Variables: time, value, thisLayer, thisComp, index, inPoint, outPoint, fps.
Functions: wiggle(freq, amp), loopOut(type), loopIn(type), linear(t, tMin, tMax, a, b), ease(...), clamp(v, a, b), random(seed), noise(x), valueAtTime(t), audio("band").
Vector math works on arrays: value + [10, 0], [1, 2] * 3.

Errors never break rendering: the keyframed value is used and the problem is listed in Project Inspector.""")
            "Formats" -> DocText("""Import: MP4/MOV/MKV/WebM/3GP video, MP3/WAV/M4A/AAC/OGG/FLAC/Opus audio, PNG/JPEG/WebP/HEIF/GIF images, TTF/OTF fonts, .cube/.3dl LUTs, GLB/glTF/OBJ models, SRT/VTT/ASS subtitles.
Export: MP4 (H.264/HEVC/AV1 where the device has encoders), WebM (VP9/VP8), GIF, PNG sequence, M4A, WAV, SRT/VTT/ASS.
Not supported: FBX (proprietary), native-code extensions.""")
            "Scenarios" -> {
                SmallLabel("Run an .mftest scenario against the engine (uses a temporary project).")
                OutlinedTextField(scenario, { scenario = it }, modifier = Modifier.fillMaxWidth().height(200.dp), textStyle = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 12.sp, color = Color.White))
                Button(onClick = {
                    if (app.editor.projectId.isNotEmpty()) { app.toast("Close the open project first (scenarios use their own project).", true); return@Button }
                    val r = NativeBridge.call("runScenario", jo("script" to scenario))
                    scenarioOut = (if (r.optBoolean("ok")) "PASS\n" else "FAIL: ${r.optString("error")}\n") + r.arr("log").strings().joinToString("\n")
                }) { Text("Run scenario") }
                Text(scenarioOut, fontFamily = FontFamily.Monospace, fontSize = 12.sp, color = if (scenarioOut.startsWith("PASS")) Color(0xFF66BB6A) else Color(0xFFFF7A7A))
            }
        }
    }
}

@Composable
private fun DocText(s: String) = Text(s, fontSize = 14.sp, lineHeight = 20.sp)

// ====================================================================== Project Inspector
@Composable
fun ProjectInspectorScreen(app: AppState) {
    val st = app.editor
    val ctx = LocalContext.current
    var refresh by remember { mutableIntStateOf(0) }
    val diags = remember(refresh, st.revision) { NativeBridge.call("diagnostics").arr("items").objects() }
    val problems = remember(refresh, st.revision) { NativeBridge.call("validate").arr("problems").strings() }
    val footprint = remember(refresh) { NativeBridge.call("footprint", jo("id" to st.projectId)) }
    var relinkFor by remember { mutableStateOf<String?>(null) }
    val relink = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val aid = relinkFor
        if (uri != null && aid != null) {
            if (MediaOps.relink(ctx, st, aid, uri)) refresh++ else app.toast("That file could not be read.", true)
        }
        relinkFor = null
    }
    ScreenScaffold(app, "Project Inspector") {
        SectionTitle("Health")
        if (diags.isEmpty() && problems.isEmpty()) Text("✓ No problems found.", color = Color(0xFF66BB6A))
        problems.forEach { Text("✗ $it", color = Color(0xFFFF7A7A)) }
        diags.forEach { d ->
            Column(Modifier.fillMaxWidth().padding(vertical = 3.dp).background(PanelHi, RoundedCornerShape(8.dp)).padding(8.dp)) {
                Text((if (d.optString("severity") == "error") "✗ " else "⚠ ") + d.optString("message"), color = if (d.optString("severity") == "error") Color(0xFFFF7A7A) else Color(0xFFFFB74D))
                Text(d.optString("recommendation"), fontSize = 12.sp, color = TextDim)
                val target = d.optString("target")
                if (d.optString("code") == "MISSING_ASSET" && target.isNotEmpty()) TextButton(onClick = { relinkFor = target; relink.launch(arrayOf("*/*")) }) { Text("Relink…") }
                else if (target.isNotEmpty() && st.layer(target) != null) TextButton(onClick = { st.selection = setOf(target); app.back() }) { Text("Show layer") }
            }
        }
        SectionTitle("Media (${st.doc.arr("assets").length()})")
        st.doc.arr("assets").objects().forEach { a ->
            Row(Modifier.fillMaxWidth().padding(vertical = 2.dp), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(a.optString("name"))
                    Text("${a.optString("type")} • ${if (a.has("width")) "${a.optInt("width")}×${a.optInt("height")} • " else ""}${if (a.has("duration")) "%.1f s".format(a.optDouble("duration")) else ""}", fontSize = 12.sp, color = TextDim)
                }
                TextButton(onClick = { relinkFor = a.optString("id"); relink.launch(arrayOf("*/*")) }) { Text("Replace…") }
            }
        }
        SectionTitle("Storage")
        SmallLabel("Project folder (document, journal, versions, thumbnails): ${footprint.optLong("bytes") / 1024} KB")
        Row {
            Chip("Save version now") { st.save(version = true); refresh++; app.toast("Version saved.") }
            Chip("Clear caches") { NativeBridge.call("clearCaches"); refresh++ }
        }
        SectionTitle("Statistics")
        SmallLabel("${st.doc.arr("comps").length()} compositions • ${st.layers.size} layers in this comp • ${st.layers.sumOf { it.arr("effects").length() }} effects • ${st.keyframeTimes(null).size} keyframe times")
    }
}

// ====================================================================== Performance
@Composable
fun PerformanceScreen(app: AppState) {
    val p = app.player
    var mem by remember { mutableStateOf("") }
    LaunchedEffect(Unit) {
        while (true) {
            val rt = Runtime.getRuntime()
            mem = "Java heap ${(rt.totalMemory() - rt.freeMemory()) / 1_000_000} / ${rt.maxMemory() / 1_000_000} MB • native ${android.os.Debug.getNativeHeapAllocatedSize() / 1_000_000} MB"
            kotlinx.coroutines.delay(1000)
        }
    }
    ScreenScaffold(app, "Performance") {
        if (p == null) { Text("Open a project to see playback statistics.", color = TextDim) } else {
            Text("Preview: ${p.quality}  •  render ${"%.1f".format(p.renderMs)} ms/frame  •  ${"%.1f".format(p.fpsActual)} fps  •  dropped ${p.droppedFrames}", fontFamily = FontFamily.Monospace, fontSize = 13.sp)
            p.lastStats.keys().forEach { k -> Text("$k: ${p.lastStats.opt(k)}", fontFamily = FontFamily.Monospace, fontSize = 12.sp, color = TextDim) }
            Gap()
            EnumPicker("Preview resolution", p.mode, listOf("auto" to "Adaptive", "full" to "Full", "half" to "Half", "quarter" to "Quarter")) { p.mode = it; Settings.previewQuality = it; p.requestRender() }
        }
        Text(mem, fontFamily = FontFamily.Monospace, fontSize = 12.sp)
        Text("CPU cores: ${Runtime.getRuntime().availableProcessors()}", fontSize = 12.sp, color = TextDim)
        Chip("Free memory (clear caches)") { NativeBridge.call("memoryPressure"); System.gc() }
        SmallLabel("Tips: lower preview resolution for heavy effects; precompose complex groups (precomps are cached); disable motion blur while editing.")
    }
}

// ====================================================================== Command palette
data class Command(val name: String, val category: String, val run: () -> Unit)

fun buildCommands(app: AppState): List<Command> {
    val st = app.editor
    val p = app.player
    val sel = st.selectedLayer
    val out = mutableListOf(
        Command("Play / Pause", "Playback") { p?.toggle() },
        Command("Go to start", "Playback") { p?.pause(); st.playhead = 0.0 },
        Command("Go to end", "Playback") { p?.pause(); st.playhead = st.duration },
        Command("Undo", "Edit") { st.undo() },
        Command("Redo", "Edit") { st.redo() },
        Command("Save", "File") { st.save() },
        Command("Save version", "File") { st.save(version = true) },
        Command("Export", "File") { app.go(Screen.Export) },
        Command("Caption Studio", "Tools") { app.go(Screen.Captions) },
        Command("Script Studio", "Tools") { app.go(Screen.Scripts) },
        Command("Project Inspector", "Tools") { app.go(Screen.Inspector) },
        Command("Performance", "Tools") { app.go(Screen.Performance) },
        Command("Library", "Tools") { app.go(Screen.Library) },
        Command("Settings", "App") { app.go(Screen.Settings) },
        Command("Add text layer", "Layer") { st.addLayer("text", jo("text" to "Title")) },
        Command("Add solid", "Layer") { st.addLayer("solid") },
        Command("Add adjustment layer", "Layer") { st.addLayer("adjustment") },
        Command("Add null", "Layer") { st.addLayer("null") },
        Command("Add camera", "Layer") { st.addLayer("camera") },
        Command("Add marker at playhead", "Timeline") { st.op("addMarker", "t" to st.playhead, "name" to "") },
        Command("Split all at playhead", "Timeline") { st.op("split", "t" to st.playhead) },
    )
    if (sel != null) {
        val id = sel.optString("id")
        out += listOf(
            Command("Split layer at playhead", "Layer") { st.op("split", "layers" to listOf(id), "t" to st.playhead) },
            Command("Duplicate layer", "Layer") { st.op("duplicateLayers", "layers" to listOf(id)) },
            Command("Delete layer", "Layer") { st.op("removeLayers", "layers" to listOf(id)) },
            Command("Precompose layer", "Layer") { st.op("precompose", "layers" to listOf(id)) },
            Command("Freeze frame", "Layer") { st.op("freezeFrame", "layer" to id, "t" to st.playhead, "duration" to 2.0) },
            Command("Add keyframe: position", "Animation") { st.op("addKeyframe", "layer" to id, "path" to "transform.position", "t" to st.playhead) },
            Command("Add keyframe: scale", "Animation") { st.op("addKeyframe", "layer" to id, "path" to "transform.scale", "t" to st.playhead) },
            Command("Add keyframe: opacity", "Animation") { st.op("addKeyframe", "layer" to id, "path" to "transform.opacity", "t" to st.playhead) },
            Command("Fade in (1s)", "Animation") {
                st.apply(jo("op" to "batch", "label" to "Fade In", "ops" to listOf(
                    jo("op" to "setProp", "layer" to id, "path" to "transform.opacity", "value" to 0.0, "t" to sel.optDouble("in"), "mode" to "key"),
                    jo("op" to "setProp", "layer" to id, "path" to "transform.opacity", "value" to 100.0, "t" to sel.optDouble("in") + 1.0, "mode" to "key"))))
            },
        )
        st.registries.arr("effects").objects().forEach { e -> out += Command("Effect: ${e.optString("name")}", "Effects") { st.op("addEffect", "layer" to id, "type" to e.optString("type")) } }
    }
    return out
}

@Composable
fun CommandPalette(app: AppState) {
    var q by remember { mutableStateOf("") }
    val cmds = remember(app.editor.revision, app.editor.selection) { buildCommands(app) }
    val recent = Settings.recentCommands
    val shown = (if (q.isBlank()) cmds.sortedByDescending { recent.indexOf(it.name).let { i -> if (i < 0) -1 else 100 - i } }
        else cmds.filter { c -> q.lowercase().split(' ').all { w -> c.name.lowercase().contains(w) || c.category.lowercase().contains(w) } }).take(40)
    Box(Modifier.fillMaxSize().background(Color(0x99000000)).clickable { app.paletteOpen = false }) {
        Column(Modifier.align(Alignment.TopCenter).padding(top = 48.dp).width(520.dp).background(Panel, RoundedCornerShape(12.dp)).clickable(enabled = false) {}.padding(12.dp)) {
            OutlinedTextField(q, { q = it }, placeholder = { Text("Type a command…") }, singleLine = true, modifier = Modifier.fillMaxWidth().semantics { contentDescription = "Command search" })
            Column(Modifier.verticalScroll(rememberScrollState()).height(360.dp)) {
                shown.forEach { c ->
                    Row(Modifier.fillMaxWidth().clickable {
                        app.paletteOpen = false
                        Settings.recentCommands = listOf(c.name) + Settings.recentCommands.filter { it != c.name }
                        c.run()
                    }.padding(vertical = 10.dp, horizontal = 6.dp)) {
                        Text(c.name, modifier = Modifier.weight(1f))
                        Text(c.category, color = TextDim, fontSize = 12.sp)
                    }
                }
                if (shown.isEmpty()) Text("No matching commands.", color = TextDim)
            }
        }
    }
}

// ====================================================================== Keyboard shortcuts (hardware keyboards / Chromebooks)
object KeyboardShortcuts {
    fun handle(app: AppState, e: android.view.KeyEvent): Boolean {
        val st = app.editor
        val p = app.player
        val ctrl = e.isCtrlPressed || e.isMetaPressed
        val sel = st.selectedLayer?.optString("id")
        when (e.keyCode) {
            android.view.KeyEvent.KEYCODE_SPACE -> { p?.toggle(); return true }
            android.view.KeyEvent.KEYCODE_Z -> if (ctrl) { if (e.isShiftPressed) st.redo() else st.undo(); return true }
            android.view.KeyEvent.KEYCODE_Y -> if (ctrl) { st.redo(); return true }
            android.view.KeyEvent.KEYCODE_S -> if (ctrl) { st.save(); app.toast("Saved"); return true }
            android.view.KeyEvent.KEYCODE_K -> if (ctrl) { app.paletteOpen = true; return true } else { p?.pause(); return true }
            android.view.KeyEvent.KEYCODE_J -> { p?.let { it.rate = if (it.rate > 0) -1.0 else it.rate * 2; it.play() }; return true }
            android.view.KeyEvent.KEYCODE_L -> { p?.let { it.rate = if (it.rate < 0) 1.0 else minOf(8.0, it.rate * 2); it.play() }; return true }
            android.view.KeyEvent.KEYCODE_DPAD_LEFT -> { p?.pause(); st.playhead = maxOf(0.0, st.playhead - (if (e.isShiftPressed) 10 else 1) / st.fps); return true }
            android.view.KeyEvent.KEYCODE_DPAD_RIGHT -> { p?.pause(); st.playhead = minOf(st.duration, st.playhead + (if (e.isShiftPressed) 10 else 1) / st.fps); return true }
            android.view.KeyEvent.KEYCODE_MOVE_HOME -> { st.playhead = 0.0; return true }
            android.view.KeyEvent.KEYCODE_MOVE_END -> { st.playhead = st.duration; return true }
            android.view.KeyEvent.KEYCODE_D -> if (ctrl && sel != null) { st.op("duplicateLayers", "layers" to listOf(sel)); return true }
            android.view.KeyEvent.KEYCODE_B -> if (ctrl) { if (sel != null) st.op("split", "layers" to listOf(sel), "t" to st.playhead) else st.op("split", "t" to st.playhead); return true }
            android.view.KeyEvent.KEYCODE_DEL, android.view.KeyEvent.KEYCODE_FORWARD_DEL -> if (sel != null) { st.op("removeLayers", "layers" to st.selection.toList()); return true }
            android.view.KeyEvent.KEYCODE_M -> { st.op("addMarker", "t" to st.playhead, "name" to ""); return true }
            android.view.KeyEvent.KEYCODE_E -> if (ctrl && e.isShiftPressed) { app.go(Screen.Export); return true }
        }
        return false
    }
}
