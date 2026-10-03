package com.motionforge.app.ui

import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.DeleteSweep
import androidx.compose.material.icons.filled.CleaningServices
import androidx.compose.material.icons.filled.DriveFileMove
import androidx.compose.material.icons.filled.CreateNewFolder
import androidx.compose.material.icons.filled.Speed
import androidx.compose.material.icons.filled.NoteAdd
import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.documentfile.provider.DocumentFile
import com.motionforge.app.AppState
import com.motionforge.app.engine.EditorState
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.arr
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.objects
import com.motionforge.app.media.Importer
import com.motionforge.app.media.MediaBridge
import com.motionforge.app.media.ProxyMaker
import com.motionforge.app.Settings
import androidx.compose.material3.Switch
import androidx.compose.runtime.rememberCoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.motionforge.app.toast
import org.json.JSONObject
import java.io.File

/** Media management: everything about the project's assets in one place. */
object MediaOps {
    /** Layer count using each asset, across all compositions. */
    fun usage(st: EditorState): Map<String, Int> {
        val m = HashMap<String, Int>()
        st.doc.arr("comps").objects().forEach { c -> c.arr("layers").objects().forEach { l -> l.optString("asset").takeIf { it.isNotEmpty() }?.let { m[it] = (m[it] ?: 0) + 1 } } }
        return m
    }

    fun unused(st: EditorState): List<JSONObject> {
        val u = usage(st)
        return st.doc.arr("assets").objects().filter { (u[it.optString("id")] ?: 0) == 0 }
    }

    /** Removes every asset no layer uses, as one undo step. */
    fun removeUnused(st: EditorState): Int {
        val ids = unused(st).map { it.optString("id") }
        if (ids.isEmpty()) return 0
        st.apply(jo("op" to "batch", "label" to "Remove Unused Media", "ops" to ids.map { jo("op" to "removeAsset", "asset" to it) }))
        return ids.size
    }

    /** Relinks an asset to a new file/URI (explicit, recorded; never silent). */
    fun relink(ctx: Context, st: EditorState, assetId: String, uri: Uri): Boolean {
        Importer.persist(ctx, uri)
        val a = st.asset(assetId) ?: return false
        val kind = when (a.optString("type")) { "video" -> Importer.Kind.VIDEO; "audio" -> Importer.Kind.AUDIO; else -> Importer.Kind.IMAGE }
        val probe = try { Importer.probe(ctx, uri, kind) } catch (e: Exception) { return false }
        val ok = st.apply(jo("op" to "relinkAsset", "asset" to assetId, "uri" to probe.optString("uri"), "checksum" to probe.optString("checksum"),
            "fields" to jo("path" to "", "name" to probe.optString("name"), "proxy" to JSONObject.NULL))) != null
        NativeBridge.call("clearCaches")
        return ok
    }

    /** Copies externally linked media (gallery/Files URIs) into the app's private storage so the project no longer depends on them. */
    fun collect(ctx: Context, st: EditorState): Pair<Int, Int> {
        val dir = File(ctx.filesDir, "media/" + st.projectId).apply { mkdirs() }
        var copied = 0
        var failed = 0
        val ops = ArrayList<JSONObject>()
        st.doc.arr("assets").objects().forEach { a ->
            val path = a.optString("path")
            val uri = a.optString("uri")
            if (a.has("generator") || (path.isNotEmpty() && path.startsWith(ctx.filesDir.absolutePath))) return@forEach
            val src = path.ifEmpty { uri }
            if (src.isEmpty()) return@forEach
            val name = a.optString("name", a.optString("id")).replace(Regex("[^A-Za-z0-9._ -]"), "_")
            val out = File(dir, a.optString("id") + "_" + name)
            val okCopy = try {
                MediaBridge.openStream(src)?.use { input -> out.outputStream().use { input.copyTo(it) } } != null
            } catch (e: Exception) { false }
            if (okCopy && out.length() > 0) {
                ops.add(jo("op" to "relinkAsset", "asset" to a.optString("id"), "path" to out.absolutePath, "fields" to jo("collected" to true)))
                copied++
            } else failed++
        }
        if (ops.isNotEmpty()) st.apply(jo("op" to "batch", "label" to "Collect Media", "ops" to ops))
        return copied to failed
    }

    /** Deletes decoded caches (audio PCM, frame caches); originals are untouched. Returns bytes freed. */
    fun clearCaches(ctx: Context): Long {
        val dirs = listOf(File(ctx.cacheDir, "audio"), File(ctx.cacheDir, "mf"))
        val bytes = dirs.sumOf { d -> d.walkBottomUp().filter { it.isFile }.sumOf { it.length() } }
        dirs.forEach { d -> d.walkBottomUp().filter { it.isFile }.forEach { it.delete() } }
        NativeBridge.call("clearCaches")
        MediaBridge.releaseAll()
        return bytes
    }

    /** Creates a proxy for a video asset and records it on the asset (one undo step). Runs on the caller's thread. */
    fun makeProxy(ctx: Context, st: EditorState, assetId: String, progress: (Float) -> Boolean = { true }): String? {
        val a = st.asset(assetId) ?: return "Media not found."
        if (a.optString("type") != "video") return "Only video clips have proxies."
        val proxy = try { ProxyMaker.make(ctx, a, progress = progress) } catch (e: Throwable) { return e.message ?: e.toString() }
        return if (st.apply(jo("op" to "updateAsset", "label" to "Create Proxy", "asset" to assetId, "fields" to jo("proxy" to proxy))) != null) null
        else "The proxy could not be attached."
    }

    /** Detaches and deletes an asset's proxy. */
    fun removeProxy(st: EditorState, assetId: String) {
        val path = st.asset(assetId)?.optJSONObject("proxy")?.optString("path").orEmpty()
        st.apply(jo("op" to "updateAsset", "label" to "Remove Proxy", "asset" to assetId, "fields" to jo("proxy" to JSONObject.NULL)))
        if (path.isNotEmpty()) File(path).delete()
    }

    fun proxyBytes(ctx: Context): Long = ProxyMaker.dir(ctx).walkBottomUp().filter { it.isFile }.sumOf { it.length() }

    fun thumbnail(asset: JSONObject): Bitmap? = try {
        when (asset.optString("type")) {
            "video" -> MediaBridge.videoFrame(asset.toString(), minOf(1.0, asset.optDouble("duration", 0.0) / 2), 160, 90)
            "image" -> MediaBridge.image(asset.toString(), 160, 90)
            else -> null
        }
    } catch (e: Throwable) { null }

    /** Imports every supported media file from a folder (Storage Access Framework tree). */
    fun importFolder(app: AppState, ctx: Context, tree: Uri): Int {
        val root = DocumentFile.fromTreeUri(ctx, tree) ?: return 0
        var n = 0
        root.listFiles().filter { it.isFile }.sortedBy { it.name }.forEach { f ->
            val kind = Importer.classify(ctx, f.uri)
            if (kind == Importer.Kind.VIDEO || kind == Importer.Kind.AUDIO || kind == Importer.Kind.IMAGE) { importIntoProject(app, ctx, f.uri); n++ }
        }
        return n
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun MediaManagerScreen(app: AppState) {
    val st = app.editor
    val ctx = LocalContext.current
    var refresh by remember { mutableIntStateOf(0) }
    var relinkFor by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()
    var proxyJobs by remember { mutableStateOf(mapOf<String, Float>()) }  // asset id → progress
    var useProxies by remember { mutableStateOf(Settings.useProxies) }
    fun startProxy(id: String) {
        if (id in proxyJobs) return
        proxyJobs = proxyJobs + (id to 0f)
        scope.launch {
            val e = withContext(Dispatchers.Default) { MediaOps.makeProxy(ctx, st, id) { p -> scope.launch { if (id in proxyJobs) proxyJobs = proxyJobs + (id to p) }; true } }
            proxyJobs = proxyJobs - id
            if (e != null) app.toast("Proxy failed: $e", true)
            refresh++
        }
    }
    val usage = remember(st.revision, refresh) { MediaOps.usage(st) }
    val assets = st.doc.arr("assets").objects()
    val importFiles = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris -> uris.forEach { importIntoProject(app, ctx, it) }; refresh++ }
    val importFolder = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { tree ->
        if (tree != null) { val n = MediaOps.importFolder(app, ctx, tree); app.toast("Imported $n media files from the folder."); refresh++ }
    }
    val relink = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        val id = relinkFor
        if (uri != null && id != null) app.toast(if (MediaOps.relink(ctx, st, id, uri)) "Media replaced. Edits are preserved." else "That file could not be read.", false)
        relinkFor = null; refresh++
    }
    ScreenScaffold(app, "Media") {
        FlowRow {
            IconAction(Icons.Filled.NoteAdd, "Import files") { importFiles.launch(arrayOf("video/*", "audio/*", "image/*")) }
            IconAction(Icons.Filled.CreateNewFolder, "Import folder") { importFolder.launch(null) }
            IconAction(Icons.Filled.DeleteSweep, "Remove unused") { val n = MediaOps.removeUnused(st); app.toast(if (n == 0) "No unused media." else "Removed $n unused media items (undoable).") }
            IconAction(Icons.Filled.DriveFileMove, "Collect into project") {
                val (ok, bad) = MediaOps.collect(ctx, st)
                app.toast("Copied $ok media files into the project" + if (bad > 0) "; $bad could not be read" else ".", bad > 0)
            }
            IconAction(Icons.Filled.CleaningServices, "Clear media caches") { val b = MediaOps.clearCaches(ctx); app.toast("Freed ${b / 1_000_000} MB of decoded caches.") }
            IconAction(Icons.Filled.Speed, "Make proxies for all video") {
                assets.filter { it.optString("type") == "video" && it.optJSONObject("proxy") == null }.forEach { startProxy(it.optString("id")) }
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Switch(useProxies, { useProxies = it; Settings.useProxies = it; app.player?.requestRender() },
                modifier = Modifier.semantics { contentDescription = "Use proxies in preview" })
            Text("  Use proxies in preview (export always uses originals) · ${remember(refresh) { MediaOps.proxyBytes(ctx) } / 1_000_000} MB of proxies",
                fontSize = 12.sp, color = TextDim)
        }
        SmallLabel("${assets.size} media items · ${assets.count { (usage[it.optString("id")] ?: 0) == 0 }} unused")
        if (assets.isEmpty()) Text("No media yet. Import from the gallery, Files, or a whole folder.", color = TextDim)
        assets.forEach { a ->
            val id = a.optString("id")
            val used = usage[id] ?: 0
            val available = remember(id, a.optString("uri"), a.optString("path"), refresh) { a.has("generator") || MediaBridge.available(a.toString()) }
            val thumb = remember(id, a.optString("uri"), a.optString("path"), refresh) { MediaOps.thumbnail(a) }
            Row(Modifier.fillMaxWidth().padding(vertical = 4.dp).background(PanelHi, RoundedCornerShape(8.dp)).padding(6.dp), verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(80.dp, 45.dp).background(Color.Black, RoundedCornerShape(4.dp)), contentAlignment = Alignment.Center) {
                    if (thumb != null) Image(thumb.asImageBitmap(), "Thumbnail ${a.optString("name")}")
                    else Text(a.optString("type").uppercase(), fontSize = 10.sp, color = TextDim)
                }
                Column(Modifier.weight(1f).padding(start = 8.dp)) {
                    Text(a.optString("name"), fontWeight = FontWeight.Bold, maxLines = 1)
                    Text(buildString {
                        append(a.optString("type"))
                        if (a.has("width")) append(" · ${a.optInt("width")}×${a.optInt("height")}")
                        if (a.has("duration")) append(" · %.1f s".format(a.optDouble("duration")))
                        if (a.has("fps")) append(" · ${fmt(a.optDouble("fps"))} fps")
                        if (a.optLong("size") > 0) append(" · ${a.optLong("size") / 1_000_000} MB")
                        if (a.optBoolean("vfr")) append(" · VFR")
                        if (a.optBoolean("hdr")) append(" · HDR")
                        if (a.optBoolean("collected")) append(" · in project")
                        a.optJSONObject("proxy")?.let { append(" · proxy ${it.optInt("height")}p") }
                    }, fontSize = 12.sp, color = TextDim)
                    Text(if (!available) "MISSING — relink it" else if (used == 0) "Unused" else "Used by $used layer${if (used == 1) "" else "s"}",
                        fontSize = 12.sp, color = if (!available) Color(0xFFFF7A7A) else if (used == 0) Color(0xFFFFB74D) else Color(0xFF66BB6A))
                }
                Column {
                    TextButton(onClick = { relinkFor = id; relink.launch(arrayOf("*/*")) }) { Text(if (available) "Replace" else "Relink") }
                    if (a.optString("type") == "video" && available) when {
                        id in proxyJobs -> Text("Proxy ${(proxyJobs[id]!! * 100).toInt()}%", fontSize = 12.sp, color = TextDim,
                            modifier = Modifier.padding(8.dp).semantics { contentDescription = "Creating proxy for ${a.optString("name")}" })
                        a.optJSONObject("proxy") != null -> TextButton(onClick = { MediaOps.removeProxy(st, id); refresh++ }) { Text("Remove proxy") }
                        else -> TextButton(onClick = { startProxy(id) }, modifier = Modifier.semantics { contentDescription = "Make proxy for ${a.optString("name")}" }) { Text("Make proxy") }
                    }
                    if (used == 0) TextButton(onClick = { st.op("removeAsset", "asset" to id) }) { Text("Remove") }
                    else TextButton(onClick = { st.selection = st.layers.filter { it.optString("asset") == id }.map { it.optString("id") }.toSet(); app.back() }) { Text("Show") }
                }
            }
        }
    }
}
