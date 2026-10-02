package com.motionforge.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDownward
import androidx.compose.material.icons.filled.ArrowUpward
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material.icons.filled.Hearing
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.motionforge.app.AppState
import com.motionforge.app.engine.EditorState
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.arr
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.obj
import com.motionforge.app.engine.objects
import com.motionforge.app.engine.strings
import com.motionforge.app.toast
import org.json.JSONArray
import org.json.JSONObject

object Clipboard {
    var effects: JSONArray? = null
    var keyframes: JSONObject? = null
    var keyframePath: String = ""
}

@Composable
fun InspectorPanel(app: AppState, ui: EditorUi) {
    val st = app.editor
    // Property editors evaluate values at the playhead; during playback that would re-evaluate every frame and
    // starve the UI thread, so the inspector pauses (like a RAM preview) and resumes on pause.
    if (app.player?.playing == true) {
        Column(Modifier.fillMaxSize().padding(12.dp)) {
            Text("Playing… pause to edit properties.", color = TextDim)
        }
        return
    }
    val layer = st.selectedLayer
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 12.dp, vertical = 6.dp)) {
        Text(ui.tab.name.let { if (it == "ThreeD") "3D" else it } + (layer?.let { " — " + it.optString("name") } ?: ""), fontWeight = FontWeight.Bold)
        when (ui.tab) {
            InspectorTab.Add -> AddLayerPanel(app, ui)
            InspectorTab.Comp -> CompPanel(app)
            InspectorTab.Markers -> MarkersPanel(app)
            InspectorTab.History -> {}
            InspectorTab.None -> {}
            else -> if (layer == null) Text("Select a layer to edit it.", color = TextDim) else when (ui.tab) {
                InspectorTab.Layer -> LayerPanel(app, ui, layer)
                InspectorTab.Effects -> EffectsPanel(app, layer)
                InspectorTab.Masks -> MasksPanel(app, layer)
                InspectorTab.Text -> TextPanel(app, layer)
                InspectorTab.Shape -> ShapePanel(app, layer)
                InspectorTab.Audio -> AudioPanel(app, layer)
                InspectorTab.Time -> TimePanel(app, layer)
                InspectorTab.Transitions -> TransitionsPanel(app, layer)
                InspectorTab.Behaviors -> BehaviorsPanel(app, layer)
                InspectorTab.Keyframes -> KeyframesPanel(app, ui, layer)
                InspectorTab.ThreeD -> ThreeDPanel(app, layer)
                InspectorTab.Particles -> ParticlesPanel(app, layer)
                InspectorTab.Tracking -> TrackingPanel(app, ui, layer)
                InspectorTab.Captions -> Text("Open Caption Studio from the toolbar.")
                else -> {}
            }
        }
        Gap(40)
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun AddLayerPanel(app: AppState, ui: EditorUi) {
    val st = app.editor
    fun add(kind: String, opts: JSONObject = JSONObject(), tab: InspectorTab = InspectorTab.Layer) { if (st.addLayer(kind, opts) != null) ui.tab = tab }
    SectionTitle("Text & graphics")
    FlowRow {
        Chip("Text") { add("text", jo("text" to "Title"), InspectorTab.Text) }
        listOf("rect" to "Rectangle", "ellipse" to "Ellipse", "star" to "Star", "polygon" to "Polygon", "line" to "Line", "arrow" to "Arrow").forEach { (k, n) ->
            Chip(n) { add("shape", jo("shape" to k), InspectorTab.Shape) }
        }
        Chip("Solid") { add("solid") }
    }
    SectionTitle("Utility layers")
    FlowRow {
        Chip("Adjustment") { add("adjustment", tab = InspectorTab.Effects) }
        Chip("Null") { add("null") }
        Chip("Guide (not exported)") { st.addLayer("shape", jo("shape" to "line"))?.let { st.op("setLayer", "layer" to it, "fields" to jo("guide" to true, "name" to "Guide")) } }
        Chip("Caption track") { st.op("setCaptions", "items" to JSONArray()) }
    }
    SectionTitle("3D")
    FlowRow {
        Chip("Camera") { add("camera", tab = InspectorTab.ThreeD) }
        listOf("point", "spot", "directional", "ambient").forEach { k -> Chip("${k.replaceFirstChar { it.uppercase() }} light") { add("light", jo("light" to k), InspectorTab.ThreeD) } }
        st.registries.arr("primitives").strings().forEach { p -> Chip("3D $p") { add("model3d", jo("primitive" to p), InspectorTab.ThreeD) } }
    }
    SectionTitle("Particles")
    FlowRow { st.registries.arr("particlePresets").strings().forEach { p -> Chip(p.replaceFirstChar { it.uppercase() }) { add("particles", jo("preset" to p), InspectorTab.Particles) } } }
    SectionTitle("Nested compositions")
    FlowRow {
        st.doc.arr("comps").objects().filter { it.optString("id") != st.comp.optString("id") }.forEach { c ->
            Chip("Comp: " + c.optString("name")) { add("precomp", jo("comp" to c.optString("id"))) }
        }
    }
    SectionTitle("Capsules")
    val caps = remember { NativeBridge.call("capsules").arr("capsules").objects() }
    if (caps.isEmpty()) SmallLabel("No capsules yet. Select layers and choose Layer › Create Capsule.")
    FlowRow { caps.forEach { c -> Chip(c.optString("name")) { st.op("insertCapsule", "capsule" to c, "t" to st.playhead)?.let { st.selection = setOf(it.optString("layer")) } } } }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun LayerPanel(app: AppState, ui: EditorUi, layer: JSONObject) {
    val st = app.editor
    val id = layer.optString("id")
    val type = layer.optString("type")
    var rename by remember { mutableStateOf(false) }
    var blendSearch by remember { mutableStateOf(false) }
    var capsule by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(layer.optString("name"), modifier = Modifier.weight(1f).clickable { rename = true })
        TextButton(onClick = { rename = true }) { Text("Rename") }
    }
    SmallLabel("Type: $type · in ${"%.2f".format(layer.optDouble("in"))}s · out ${"%.2f".format(layer.optDouble("out"))}s")
    SectionTitle("Transform", "Position, scale, rotation and opacity. Tap the stopwatch to keyframe at the playhead. All values affect the final export.")
    val is3d = layer.optBoolean("threeD")
    PropEditor(st, layer, "transform.position", "Position", "vector", -2000.0, 4000.0, componentLabels = if (is3d) listOf("X", "Y", "Z") else listOf("X", "Y"))
    PropEditor(st, layer, "transform.scale", "Scale", "vector", 0.0, 400.0, componentLabels = if (is3d) listOf("X", "Y", "Z") else listOf("X", "Y"))
    PropEditor(st, layer, "transform.rotation", "Rotation", "angle", -360.0, 360.0)
    if (is3d) {
        PropEditor(st, layer, "transform.rotationX", "Rotation X", "angle", -360.0, 360.0)
        PropEditor(st, layer, "transform.rotationY", "Rotation Y", "angle", -360.0, 360.0)
    }
    PropEditor(st, layer, "transform.opacity", "Opacity", "number", 0.0, 100.0)
    PropEditor(st, layer, "transform.anchor", "Anchor point", "vector", -2000.0, 4000.0, componentLabels = listOf("X", "Y"))
    SectionTitle("Compositing")
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text("Blend mode", modifier = Modifier.weight(1f), color = TextDim)
        TextButton(onClick = { blendSearch = true }) { Text(layer.optString("blend", "normal")) }
    }
    LabeledSwitch("3D layer", is3d) { st.op("setLayer", "layer" to id, "fields" to jo("threeD" to it)) }
    LabeledSwitch("Motion blur", layer.optBoolean("motionBlur")) {
        st.op("setLayer", "layer" to id, "fields" to jo("motionBlur" to it))
        if (it && st.comp.obj("motionBlur").optBoolean("enabled").not()) app.toast("Enable motion blur in Composition settings to see it.")
    }
    LabeledSwitch("Guide layer (hidden in export)", layer.optBoolean("guide")) { st.op("setLayer", "layer" to id, "fields" to jo("guide" to it)) }
    val others = st.layers.filter { it.optString("id") != id }
    EnumPicker("Parent", if (layer.isNull("parent")) "" else layer.optString("parent"), listOf("" to "None") + others.map { it.optString("id") to it.optString("name") }) {
        st.op("parent", "layer" to id, "parent" to if (it.isEmpty()) null else it, "t" to st.playhead)
    }
    val matte = layer.optJSONObject("matte")
    EnumPicker("Track matte", matte?.optString("layer") ?: "", listOf("" to "None") + others.map { it.optString("id") to it.optString("name") }) {
        st.op("setLayer", "layer" to id, "fields" to jo("matte" to if (it.isEmpty()) null else jo("layer" to it, "mode" to (matte?.optString("mode") ?: "alpha"), "invert" to (matte?.optBoolean("invert") ?: false))))
    }
    if (matte != null) {
        EnumPicker("Matte mode", matte.optString("mode", "alpha"), listOf("alpha" to "Alpha", "luma" to "Luma")) {
            st.op("setLayer", "layer" to id, "fields" to jo("matte" to jo("layer" to matte.optString("layer"), "mode" to it, "invert" to matte.optBoolean("invert"))))
        }
        LabeledSwitch("Inverted matte", matte.optBoolean("invert")) {
            st.op("setLayer", "layer" to id, "fields" to jo("matte" to jo("layer" to matte.optString("layer"), "mode" to matte.optString("mode"), "invert" to it)))
        }
    }
    SectionTitle("Layer")
    FlowRow {
        Chip("Create Capsule…") { capsule = true }
        Chip("Precompose") { st.op("precompose", "layers" to st.selection.toList()) }
        Chip("Save as preset") { val r = NativeBridge.call("savePreset", jo("layer" to id, "name" to layer.optString("name") + " preset", "include" to jo("effects" to true, "behaviors" to true, "masks" to true))); app.toast(if (r.optBoolean("ok")) "Preset saved to library" else r.optString("error"), !r.optBoolean("ok")) }
        if (layer.has("precomp")) Chip("Edit internals") { st.op("setActiveComp", "comp" to layer.obj("precomp").optString("comp")); st.selection = emptySet() }
    }
    if (layer.has("precomp") && layer.obj("precomp").has("controlDefs")) CapsuleControls(st, layer)
    if (rename) TextInputDialog("Rename layer", layer.optString("name"), onDismiss = { rename = false }) { st.op("setLayer", "layer" to id, "fields" to jo("name" to it)) }
    if (blendSearch) SearchPickDialog("Blend mode", st.registries.arr("blendModes").strings(), onDismiss = { blendSearch = false }) { st.op("setLayer", "layer" to id, "fields" to jo("blend" to it)) }
    if (capsule) CreateCapsuleDialog(app, onDismiss = { capsule = false })
}

@Composable
fun SearchPickDialog(title: String, items: List<String>, labels: (String) -> String = { it }, onDismiss: () -> Unit, onPick: (String) -> Unit) {
    var q by remember { mutableStateOf("") }
    AlertDialog(onDismissRequest = onDismiss, title = { Text(title) }, text = {
        Column {
            OutlinedTextField(q, { q = it }, placeholder = { Text("Search") }, singleLine = true)
            Column(Modifier.verticalScroll(rememberScrollState())) {
                items.filter { q.isBlank() || labels(it).contains(q, true) || it.contains(q, true) }.forEach { i ->
                    TextButton(onClick = { onPick(i); onDismiss() }, modifier = Modifier.fillMaxWidth()) { Text(labels(i), modifier = Modifier.fillMaxWidth()) }
                }
            }
        }
    }, confirmButton = { TextButton(onClick = onDismiss) { Text("Close") } })
}

@Composable
fun CapsuleControls(st: EditorState, layer: JSONObject) {
    val p = layer.obj("precomp")
    SectionTitle("Capsule controls", "Simple controls exposed by the capsule author. Use Edit internals to change the underlying layers.")
    p.arr("controlDefs").objects().forEach { d ->
        val name = d.optString("name")
        val v = p.obj("controls").opt(name)
        when (d.optString("type")) {
            "text" -> {
                var edit by remember { mutableStateOf(false) }
                Row(Modifier.fillMaxWidth().clickable { edit = true }.padding(vertical = 12.dp)) { Text(name, color = TextDim, modifier = Modifier.weight(1f)); Text(v?.toString() ?: "") }
                if (edit) TextInputDialog(name, v?.toString() ?: "", onDismiss = { edit = false }) { st.op("setCapsuleControl", "layer" to layer.optString("id"), "control" to name, "value" to it) }
            }
            "color" -> ColorRow(name, (v as? JSONArray).toDoubles(4)) { st.op("setCapsuleControl", "layer" to layer.optString("id"), "control" to name, "value" to JSONArray(it.toList())) }
            "position" -> {
                val xy = (v as? JSONArray).toDoubles(2)
                listOf("X", "Y").forEachIndexed { i, axis ->
                    NumberRow("$name $axis", xy[i], -1000.0, 1000.0, onPreview = {}) { nv ->
                        st.op("setCapsuleControl", "layer" to layer.optString("id"), "control" to name, "value" to JSONArray(xy.copyOf().also { it[i] = nv }.toList()))
                    }
                }
            }
            else -> NumberRow(name, (v as? Number)?.toDouble() ?: 0.0, d.optDouble("min", 0.0), d.optDouble("max", 100.0), onPreview = {}) {
                st.op("setCapsuleControl", "layer" to layer.optString("id"), "control" to name, "value" to it)
            }
        }
    }
}

@Composable
fun CreateCapsuleDialog(app: AppState, onDismiss: () -> Unit) {
    val st = app.editor
    var name by remember { mutableStateOf("My Capsule") }
    // Candidate controls: text content, fill colors and opacities of selected layers.
    val cands = remember {
        st.selection.mapNotNull { st.layer(it) }.flatMap { l ->
            val out = ArrayList<JSONObject>()
            val id = l.optString("id")
            if (l.has("text")) { out.add(jo("name" to "Title", "type" to "text", "layer" to id, "path" to "text.content")); out.add(jo("name" to "Text Color", "type" to "color", "layer" to id, "path" to "text.fill")) }
            if (l.has("shape")) out.add(jo("name" to "${l.optString("name")} Color", "type" to "color", "layer" to id, "path" to "shape.fill.color"))
            if (l.has("solid")) out.add(jo("name" to "${l.optString("name")} Color", "type" to "color", "layer" to id, "path" to "solid.color"))
            out.add(jo("name" to "${l.optString("name")} Opacity", "type" to "number", "layer" to id, "path" to "transform.opacity", "min" to 0, "max" to 100))
            out.add(jo("name" to "${l.optString("name")} Size", "type" to "size", "layer" to id, "path" to "transform.scale", "min" to 10, "max" to 300, "base" to 100, "default" to 100))
            out.add(jo("name" to "${l.optString("name")} Position", "type" to "position", "layer" to id, "path" to "transform.position", "default" to listOf(0.0, 0.0)))
            l.arr("effects").objects().forEach { e ->
                e.obj("params").keys().asSequence().firstOrNull()?.let { k -> out.add(jo("name" to "${st.effectInfo(e.optString("type"))?.optString("name") ?: "Effect"} Intensity", "type" to "intensity", "layer" to id, "path" to "effects.${e.optString("id")}.params.$k", "min" to 0, "max" to 200, "base" to 100, "default" to 100)) }
            }
            out
        } + listOfNotNull(st.selection.firstOrNull()?.let {
            jo("name" to "Speed", "type" to "speed", "layer" to it, "path" to "", "min" to 25, "max" to 400, "base" to 100, "default" to 100)
        })
    }
    // Simple, user-facing controls are on by default; per-layer opacity/position are opt-in.
    val chosen = remember { mutableStateOf(cands.map { it.optString("type") in setOf("text", "color", "size", "speed", "intensity") }) }
    AlertDialog(onDismissRequest = onDismiss, title = { Text("Create Forge Capsule") }, text = {
        Column(Modifier.verticalScroll(rememberScrollState())) {
            OutlinedTextField(name, { name = it }, label = { Text("Capsule name") }, singleLine = true)
            SmallLabel("Expose these properties as simple controls:")
            cands.forEachIndexed { i, c -> CheckRow(c.optString("name"), chosen.value[i]) { v -> chosen.value = chosen.value.toMutableList().also { it[i] = v } } }
        }
    }, confirmButton = { TextButton(onClick = {
        val controls = cands.filterIndexed { i, _ -> chosen.value[i] }
        val r = NativeBridge.call("createCapsule", jo("layers" to st.selection.toList(), "name" to name, "controls" to controls))
        app.toast(if (r.optBoolean("ok")) "Capsule '$name' saved to the library. Insert it from + Layer › Capsules." else r.optString("error"), !r.optBoolean("ok"))
        onDismiss()
    }) { Text("Create") } }, dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } })
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun EffectsPanel(app: AppState, layer: JSONObject) {
    val st = app.editor
    val id = layer.optString("id")
    var browse by remember { mutableStateOf(false) }
    var presets by remember { mutableStateOf(false) }
    val expanded = remember { mutableStateOf(setOf<String>()) }
    val effects = layer.arr("effects").objects()
    FlowRow {
        Chip("+ Add effect") { browse = true }
        Chip("Apply preset") { presets = true }
        if (effects.isNotEmpty()) Chip("Copy all") { Clipboard.effects = layer.arr("effects"); app.toast("Copied ${effects.size} effects") }
        if (Clipboard.effects != null) Chip("Paste") { st.op("pasteEffects", "layer" to id, "effects" to Clipboard.effects) }
        if (effects.isNotEmpty()) Chip("Bypass all") { st.apply(jo("op" to "batch", "label" to "Bypass Effects", "ops" to effects.map { jo("op" to "setEffect", "layer" to id, "effect" to it.optString("id"), "fields" to jo("enabled" to false)) })) }
    }
    if (effects.isEmpty()) SmallLabel("No effects. Effects are building blocks: stack several (e.g. Blur → Displacement → Glow → Color) to build your own look.")
    effects.forEachIndexed { i, e ->
        val eid = e.optString("id")
        val info = st.effectInfo(e.optString("type"))
        val open = eid in expanded.value
        Column(Modifier.padding(vertical = 4.dp).background(PanelHi, RoundedCornerShape(8.dp)).padding(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                IconBtn(if (open) Icons.Filled.ExpandLess else Icons.Filled.ExpandMore, if (open) "Collapse" else "Expand") { expanded.value = if (open) expanded.value - eid else expanded.value + eid }
                Text(info?.optString("name") ?: e.optString("type"), modifier = Modifier.weight(1f), fontSize = 14.sp, color = if (e.optBoolean("enabled", true)) Color.White else TextDim)
                val en = e.optBoolean("enabled", true)
                IconBtn(if (en) Icons.Filled.Visibility else Icons.Filled.VisibilityOff, if (en) "Disable effect" else "Enable effect") { st.op("setEffect", "layer" to id, "effect" to eid, "fields" to jo("enabled" to !en)) }
                IconBtn(Icons.Filled.Hearing, if (e.optBoolean("solo")) "Unsolo effect" else "Solo effect", tint = if (e.optBoolean("solo")) Accent else TextDim) { st.op("setEffect", "layer" to id, "effect" to eid, "fields" to jo("solo" to !e.optBoolean("solo"))) }
            }
            if (open) {
                Row {
                    IconBtn(Icons.Filled.ArrowUpward, "Move effect up", enabled = i > 0) { st.op("moveEffect", "layer" to id, "effect" to eid, "index" to i - 1) }
                    IconBtn(Icons.Filled.ArrowDownward, "Move effect down", enabled = i < effects.size - 1) { st.op("moveEffect", "layer" to id, "effect" to eid, "index" to i + 1) }
                    IconBtn(Icons.Filled.ContentCopy, "Duplicate effect") { st.op("duplicateEffect", "layer" to id, "effect" to eid) }
                    IconBtn(Icons.Filled.Refresh, "Reset effect") { st.op("resetEffect", "layer" to id, "effect" to eid) }
                    IconBtn(Icons.Filled.Delete, "Remove effect") { st.op("removeEffect", "layer" to id, "effect" to eid) }
                }
                info?.optString("help")?.takeIf { it.isNotEmpty() }?.let { SmallLabel(it) }
                NumberRow("Mix with original", e.optDouble("mix", 100.0), 0.0, 100.0, onPreview = {}) { v -> st.op("setEffect", "layer" to id, "effect" to eid, "fields" to jo("mix" to v)) }
                if (info != null) ParamList(st, layer, "effects.$eid.params", info.arr("params"))
                else st.layer(id)?.let { l -> e.obj("params").keys().forEach { k -> PropEditor(st, l, "effects.$eid.params.$k", k) } }
            }
        }
    }
    if (browse) EffectBrowser(st, onDismiss = { browse = false }) { type -> st.op("addEffect", "layer" to id, "type" to type)?.let { expanded.value = expanded.value + it.optString("effect") } }
    if (presets) PresetPickDialog(app, id) { presets = false }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun EffectBrowser(st: EditorState, onDismiss: () -> Unit, onPick: (String) -> Unit) {
    var q by remember { mutableStateOf("") }
    var cat by remember { mutableStateOf("") }
    val all = remember(st.registries) { st.registries.arr("effects").objects() }
    val cats = remember(all) { all.map { it.optString("category") }.distinct() }
    val shown = remember(all, q, cat) { all.filter { (cat.isEmpty() || it.optString("category") == cat) && (q.isBlank() || it.optString("name").contains(q, true) || it.optString("type").contains(q, true)) } }
    AlertDialog(onDismissRequest = onDismiss, title = { Text("Effects") }, text = {
        Column {
            OutlinedTextField(q, { q = it }, placeholder = { Text("Search effects (e.g. glow, blur, key)") }, singleLine = true)
            FlowRow { Chip("All", cat.isEmpty()) { cat = "" }; cats.forEach { c -> Chip(c, cat == c) { cat = c } } }
            // Lazy: only visible rows are composed (opening the browser must stay fast on low-end phones).
            androidx.compose.foundation.lazy.LazyColumn(Modifier.heightIn(max = 420.dp)) {
                items(shown.size, key = { shown[it].optString("type") }) { i ->
                    val e = shown[i]
                    Row(Modifier.fillMaxWidth().clickable { onPick(e.optString("type")); onDismiss() }.padding(vertical = 10.dp)) {
                        Column(Modifier.weight(1f)) {
                            Text(e.optString("name"))
                            SmallLabel(e.optString("category") + if (e.optInt("cost") >= 3) " · heavy" else "")
                        }
                    }
                }
            }
        }
    }, confirmButton = { TextButton(onClick = onDismiss) { Text("Close") } })
}

@Composable
fun PresetPickDialog(app: AppState, layerId: String, onDismiss: () -> Unit) {
    val st = app.editor
    val presets = remember { NativeBridge.call("presets").arr("presets").objects() }
    var mapping by remember { mutableStateOf("skip") }
    AlertDialog(onDismissRequest = onDismiss, title = { Text("Apply preset") }, text = {
        Column(Modifier.verticalScroll(rememberScrollState())) {
            if (presets.isEmpty()) Text("No presets yet. Save one from Layer › Save as preset, or import a .mffx file in the Library.")
            EnumPicker("Incompatible properties", mapping, listOf("skip" to "Skip", "approximate" to "Approximate", "remap" to "Remap", "cancel" to "Cancel")) { mapping = it }
            presets.forEach { p ->
                TextButton(onClick = {
                    st.op("applyPreset", "layer" to layerId, "preset" to p, "mapping" to mapping)?.let { d ->
                        val skipped = d.optJSONArray("skipped")?.length() ?: 0
                        if (skipped > 0) app.toast("Preset applied; skipped $skipped unknown effect(s).")
                    }
                    onDismiss()
                }) { Text(p.optString("name") + if (p.optBoolean("favorite")) " ★" else "") }
            }
        }
    }, confirmButton = { TextButton(onClick = onDismiss) { Text("Close") } })
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun MasksPanel(app: AppState, layer: JSONObject) {
    val st = app.editor
    val id = layer.optString("id")
    FlowRow {
        Chip("+ Rectangle") { st.op("addMask", "layer" to id, "shape" to "rect") }
        Chip("+ Ellipse") { st.op("addMask", "layer" to id, "shape" to "ellipse") }
        SmallLabel("Or use Mask / Pen mode on the preview to draw masks directly.")
    }
    layer.arr("masks").objects().forEach { m ->
        val mid = m.optString("id")
        Column(Modifier.padding(vertical = 4.dp).background(PanelHi, RoundedCornerShape(8.dp)).padding(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(m.optString("name"), modifier = Modifier.weight(1f))
                IconBtn(Icons.Filled.Delete, "Delete ${m.optString("name")}") { st.op("removeMask", "layer" to id, "mask" to mid) }
            }
            EnumPicker("Mode", m.optString("mode", "add"), listOf("add", "subtract", "intersect", "lighten", "darken", "difference", "none").map { it to it }) {
                st.op("setMask", "layer" to id, "mask" to mid, "fields" to jo("mode" to it))
            }
            LabeledSwitch("Inverted", m.optBoolean("inverted")) { st.op("setMask", "layer" to id, "mask" to mid, "fields" to jo("inverted" to it)) }
            PropEditor(st, layer, "masks.$mid.feather", "Feather", "number", 0.0, 200.0)
            PropEditor(st, layer, "masks.$mid.expansion", "Expansion", "number", -200.0, 200.0)
            PropEditor(st, layer, "masks.$mid.opacity", "Opacity", "number", 0.0, 100.0)
            Row {
                Chip("Keyframe path") { st.op("addKeyframe", "layer" to id, "path" to "masks.$mid.path", "t" to st.playhead) }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun CompPanel(app: AppState) {
    val st = app.editor
    val c = st.comp
    var edit by remember { mutableStateOf<String?>(null) }
    SectionTitle("Composition")
    listOf("name" to "Name", "width" to "Width", "height" to "Height", "fps" to "Frame rate", "duration" to "Duration (s)").forEach { (k, n) ->
        Row(Modifier.fillMaxWidth().clickable { edit = k }.padding(vertical = 12.dp)) { Text(n, color = TextDim, modifier = Modifier.weight(1f)); Text(c.optString(k)) }
    }
    ColorRow("Background", c.optJSONArray("bg").toDoubles(4)) { st.op("updateComp", "bg" to JSONArray(it.toList())) }
    val mb = c.obj("motionBlur")
    LabeledSwitch("Motion blur", mb.optBoolean("enabled")) { st.op("updateComp", "motionBlur" to jo("enabled" to it, "samples" to mb.optInt("samples", 8), "shutter" to mb.optDouble("shutter", 180.0))) }
    NumberRow("Motion blur samples", mb.optDouble("samples", 8.0), 2.0, 32.0, help = "Higher values improve blur smoothness but multiply render work. Preview uses at most 3 samples.", onPreview = {}) {
        st.op("updateComp", "motionBlur" to jo("enabled" to mb.optBoolean("enabled"), "samples" to it.toInt(), "shutter" to mb.optDouble("shutter", 180.0)))
    }
    NumberRow("Shutter angle", mb.optDouble("shutter", 180.0), 0.0, 720.0, onPreview = {}) {
        st.op("updateComp", "motionBlur" to jo("enabled" to mb.optBoolean("enabled"), "samples" to mb.optInt("samples", 8), "shutter" to it))
    }
    SectionTitle("Compositions & delivery versions", "A project can hold several compositions, e.g. 16:9, 9:16 and 1:1 versions of the same edit.")
    FlowRow {
        st.doc.arr("comps").objects().forEach { cc -> Chip(cc.optString("name") + " ${cc.optInt("width")}x${cc.optInt("height")}", cc.optString("id") == c.optString("id")) { st.op("setActiveComp", "comp" to cc.optString("id")); st.selection = emptySet() } }
    }
    FlowRow {
        listOf("9:16" to (1080 to 1920), "1:1" to (1080 to 1080), "4:5" to (1080 to 1350), "16:9" to (1920 to 1080), "4:3" to (1440 to 1080)).forEach { (n, wh) ->
            Chip("+ $n version") { makeDeliveryVersion(st, n, wh.first, wh.second) }
        }
    }
    SectionTitle("Guides")
    FlowRow {
        Chip("+ Vertical center guide") { st.op("addGuide", "axis" to "x", "pos" to c.optInt("width") / 2.0) }
        Chip("+ Horizontal center guide") { st.op("addGuide", "axis" to "y", "pos" to c.optInt("height") / 2.0) }
        c.arr("guides").objects().forEach { g -> Chip("Remove ${g.optString("axis")}=${g.optInt("pos")}") { st.op("removeGuide", "guide" to g.optString("id")) } }
    }
    SectionTitle("Audio buses")
    AudioBuses(st)
    edit?.let { k ->
        TextInputDialog(k, c.optString(k), onDismiss = { edit = null }) { v ->
            val value: Any? = if (k == "name") v else if (k == "width" || k == "height") v.toIntOrNull() else v.toDoubleOrNull()
            if (value == null) app.toast("Enter a valid number.", true) else st.op("updateComp", k to value)
        }
    }
}

/** Multi-aspect delivery: new composition containing the main edit as a precomp, scaled to fill the new frame (center preserved). */
fun makeDeliveryVersion(st: EditorState, name: String, w: Int, h: Int) {
    val main = st.comp
    val r = st.op("addComp", "name" to "${main.optString("name")} $name", "width" to w, "height" to h, "fps" to main.optDouble("fps"), "duration" to main.optDouble("duration")) ?: return
    val cid = r.optString("comp")
    val mainId = main.optString("id")
    st.op("setActiveComp", "comp" to cid)
    val lr = st.apply(jo("op" to "addLayer", "comp" to cid, "kind" to "precomp", "options" to jo("comp" to mainId, "at" to 0.0))) ?: return
    val s = maxOf(w.toDouble() / main.optInt("width"), h.toDouble() / main.optInt("height")) * 100
    st.apply(jo("op" to "setProp", "comp" to cid, "layer" to lr.optString("layer"), "path" to "transform.scale", "value" to listOf(s, s, 100.0), "mode" to "static"))
    st.message = com.motionforge.app.engine.UiMessage("Created $name version. Reposition the nested edit to preserve your subject.")
}

@Composable
fun AudioBuses(st: EditorState) {
    val buses = st.comp.obj("audio").obj("buses")
    listOf("master", "dialogue", "music", "effects").forEach { b ->
        NumberRow("${b.replaceFirstChar { it.uppercase() }} gain (dB)", buses.obj(b).optDouble("gain", 0.0), -40.0, 12.0, onPreview = {}) {
            st.op("setCompAudio", "audio" to jo("buses" to jo(b to jo("gain" to it))))
        }
    }
    val duck = buses.obj("music").obj("duck")
    LabeledSwitch("Auto-duck music under dialogue", duck.optBoolean("enabled")) { st.op("setCompAudio", "audio" to jo("buses" to jo("music" to jo("duck" to jo("enabled" to it))))) }
    if (duck.optBoolean("enabled")) NumberRow("Duck amount (dB)", duck.optDouble("amount", -12.0), -40.0, 0.0, onPreview = {}) {
        st.op("setCompAudio", "audio" to jo("buses" to jo("music" to jo("duck" to jo("amount" to it)))))
    }
    LabeledSwitch("Master limiter", buses.obj("master").optBoolean("limiter", true)) { st.op("setCompAudio", "audio" to jo("buses" to jo("master" to jo("limiter" to it)))) }
}

@Composable
fun MarkersPanel(app: AppState) {
    val st = app.editor
    var edit by remember { mutableStateOf<JSONObject?>(null) }
    RowButtons("+ Marker at playhead" to { st.op("addMarker", "t" to st.playhead, "title" to "Marker") })
    st.comp.arr("markers").objects().sortedBy { it.optDouble("t") }.forEach { m ->
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            TextButton(onClick = { app.player?.seek(m.optDouble("t")) }) { Text(st.timecode(m.optDouble("t"))) }
            Text(m.optString("title"), modifier = Modifier.weight(1f).clickable { edit = m })
            IconBtn(Icons.Filled.Delete, "Delete marker ${m.optString("title")}") { st.op("removeMarker", "marker" to m.optString("id")) }
        }
    }
    edit?.let { m -> TextInputDialog("Marker title", m.optString("title"), onDismiss = { edit = null }) { st.op("updateMarker", "marker" to m.optString("id"), "fields" to jo("title" to it)) } }
}
