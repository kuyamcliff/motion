package com.motionforge.app.engine

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONArray
import org.json.JSONObject

/**
 * UI-side mirror of the open project. Every edit goes through the native command layer
 * ([apply] / gesture previews); this class only re-reads the resulting document.
 */
class EditorState {
    var doc by mutableStateOf(JSONObject())
        private set
    var revision by mutableIntStateOf(0)
        private set
    var canUndo by mutableStateOf(false)
        private set
    var canRedo by mutableStateOf(false)
        private set
    var undoLabel by mutableStateOf("")
        private set
    var redoLabel by mutableStateOf("")
        private set
    var saveStatus by mutableStateOf("saved")
        private set
    var selection by mutableStateOf<Set<String>>(emptySet())
    var playhead by mutableDoubleStateOf(0.0)
    var message by mutableStateOf<UiMessage?>(null)
    var projectId by mutableStateOf("")
        private set

    /** Effect/behavior/transition registries. Reloaded when extensions change (they contribute effects). */
    var registries: JSONObject by mutableStateOf(NativeBridge.call("registries"))
        private set
    fun reloadRegistries() { registries = NativeBridge.call("registries") }

    val comp: JSONObject
        get() {
            val id = doc.obj("settings").optString("activeComp")
            val comps = doc.arr("comps").objects()
            return comps.firstOrNull { it.optString("id") == id } ?: comps.firstOrNull() ?: JSONObject()
        }
    val layers: List<JSONObject> get() = comp.arr("layers").objects()
    val fps: Double get() = comp.optDouble("fps", 30.0).coerceAtLeast(1.0)
    val duration: Double get() = comp.optDouble("duration", 10.0)
    val compWidth: Int get() = comp.optInt("width", 1920)
    val compHeight: Int get() = comp.optInt("height", 1080)
    fun layer(id: String): JSONObject? = layers.firstOrNull { it.optString("id") == id }
    val selectedLayer: JSONObject? get() = selection.firstOrNull()?.let { layer(it) }
    fun asset(id: String): JSONObject? = doc.arr("assets").objects().firstOrNull { it.optString("id") == id }

    fun open(id: String, recover: Boolean = false): String? {
        val r = NativeBridge.call("openProject", jo("id" to id, "recover" to recover))
        if (!r.optBoolean("ok")) return r.optString("error")
        projectId = id
        selection = emptySet()
        playhead = 0.0
        refresh()
        return null
    }

    fun close() {
        NativeBridge.call("closeProject")
        projectId = ""
    }

    fun refresh() {
        val r = NativeBridge.call("doc")
        doc = r.obj("doc")
        applyState(r.obj("state"))
        // Drop selection of layers that no longer exist (e.g. after undo).
        val ids = layers.map { it.optString("id") }.toSet()
        if (!ids.containsAll(selection)) selection = selection.filter { it in ids }.toSet()
        if (playhead > duration) playhead = duration
    }

    private fun applyState(s: JSONObject) {
        canUndo = s.optBoolean("canUndo")
        canRedo = s.optBoolean("canRedo")
        undoLabel = s.optString("undoLabel")
        redoLabel = s.optString("redoLabel")
        saveStatus = s.optString("saveStatus", "saved")
        revision = s.optInt("revision")
    }

    /** Applies an operation. Returns the result data or null on error (error shown to the user). */
    fun apply(op: JSONObject, quiet: Boolean = false): JSONObject? {
        if (!op.has("comp")) op.put("comp", comp.optString("id"))
        val r = NativeBridge.call("apply", op)
        if (!r.optBoolean("ok")) {
            if (!quiet) message = UiMessage(r.optString("error"), isError = true)
            return null
        }
        refresh()
        return r.obj("data")
    }

    fun preview(op: JSONObject) {
        if (!op.has("comp")) op.put("comp", comp.optString("id"))
        val r = NativeBridge.call("preview", op)
        if (r.optBoolean("ok")) {
            doc = NativeBridge.call("doc").obj("doc")
            revision++
        }
    }

    fun commitPreview(label: String) {
        NativeBridge.call("commitPreview", jo("label" to label))
        refresh()
    }

    fun cancelPreview() {
        NativeBridge.call("cancelPreview")
        refresh()
    }

    fun undo() { NativeBridge.call("undo"); refresh() }
    fun redo() { NativeBridge.call("redo"); refresh() }
    fun jump(index: Int) { NativeBridge.call("jump", jo("index" to index)); refresh() }

    fun save(version: Boolean = false): Boolean {
        val r = NativeBridge.call("save", jo("version" to version))
        refresh()
        if (!r.optBoolean("ok")) message = UiMessage("Save failed: " + r.optString("error"), isError = true)
        return r.optBoolean("ok")
    }

    fun autosave() {
        val r = NativeBridge.call("autosave")
        if (r.optBoolean("saved")) refresh()
    }

    // ------------------------------------------------------------ convenience
    fun op(name: String, vararg pairs: Pair<String, Any?>): JSONObject? = apply(jo("op" to name, *pairs))

    fun setProp(layerId: String, path: String, value: Any?, mode: String = "auto") =
        op("setProp", "layer" to layerId, "path" to path, "value" to value, "t" to playhead, "mode" to mode)

    fun previewProp(layerId: String, path: String, value: Any?) =
        preview(jo("op" to "setProp", "layer" to layerId, "path" to path, "value" to value, "t" to playhead))

    fun addLayer(kind: String, options: JSONObject = JSONObject()): String? {
        val d = apply(jo("op" to "addLayer", "kind" to kind, "options" to options, "at" to playhead)) ?: return null
        val id = d.optString("layer")
        selection = setOf(id)
        return id
    }

    fun localTime(layer: JSONObject): Double = playhead - layer.optDouble("start", 0.0)

    fun keyframeTimes(layer: JSONObject?): List<Double> {
        val r = NativeBridge.call("keyframeTimes", if (layer != null) jo("layer" to layer.optString("id")) else JSONObject())
        return r.arr("times").doubles()
    }

    fun snap(t: Double): Double = Math.round(t * fps) / fps
    fun frame(t: Double): Int = Math.floor(t * fps + 1e-6).toInt()
    fun timecode(t: Double): String {
        val f = frame(t)
        val fpsI = Math.round(fps).toInt().coerceAtLeast(1)
        val s = f / fpsI
        return "%02d:%02d:%02d:%02d".format(s / 3600, (s / 60) % 60, s % 60, f % fpsI)
    }

    fun effectInfo(type: String): JSONObject? = registries.arr("effects").objects().firstOrNull { it.optString("type") == type }
    fun behaviorInfo(type: String): JSONObject? = registries.arr("behaviors").objects().firstOrNull { it.optString("type") == type }
    fun transitionInfo(type: String): JSONObject? = registries.arr("transitions").objects().firstOrNull { it.optString("type") == type }

    fun evalProps(layerId: String, paths: List<String>): JSONObject =
        NativeBridge.call("evalProps", jo("layer" to layerId, "t" to playhead, "paths" to paths)).obj("values")
}

data class UiMessage(val text: String, val isError: Boolean = false, val action: String? = null, val onAction: (() -> Unit)? = null)

fun JSONArray.toDoubleArray(): DoubleArray = DoubleArray(length()) { optDouble(it) }
