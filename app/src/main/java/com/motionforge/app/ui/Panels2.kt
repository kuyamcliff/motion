package com.motionforge.app.ui

import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.LastPage
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.Straighten
import androidx.compose.material.icons.filled.DeleteSweep
import androidx.compose.material.icons.filled.Diamond
import androidx.compose.material.icons.filled.ContentPaste
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.AddCircle
import androidx.compose.material.icons.filled.ContentCut
import androidx.compose.material.icons.filled.SwapHoriz
import androidx.compose.material.icons.filled.ViewWeek
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material.icons.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.KeyboardArrowLeft
import androidx.compose.material.icons.filled.PauseCircle
import androidx.compose.material.icons.filled.Compress
import androidx.compose.material.icons.filled.ViewInAr
import androidx.compose.material.icons.filled.LayersClear
import androidx.compose.material.icons.filled.Rule
import androidx.compose.material.icons.filled.FirstPage
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.Expand
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.motionforge.app.AppState
import com.motionforge.app.engine.EditorState
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.ProgressCallback
import com.motionforge.app.engine.arr
import com.motionforge.app.engine.isAnimated
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.obj
import com.motionforge.app.engine.objects
import com.motionforge.app.engine.resolve
import com.motionforge.app.engine.strings
import com.motionforge.app.toast
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

private fun cap(s: String) = s.replaceFirstChar { it.uppercase() }

// ====================================================================== Text
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun TextPanel(app: AppState, layer: JSONObject) {
    val st = app.editor
    val id = layer.optString("id")
    if (!layer.has("text")) { Text("Not a text layer.", color = TextDim); return }
    val td = layer.obj("text")
    var edit by remember { mutableStateOf(false) }
    var customAnim by remember { mutableStateOf(false) }
    val content = st.evalProps(id, listOf("text.content")).optString("text.content")
    SectionTitle("Content")
    Box(Modifier.fillMaxWidth().background(PanelHi, RoundedCornerShape(8.dp)).padding(10.dp).pointerInput(id) { detectTapGestures { edit = true } }
        .semantics { contentDescription = "Edit text content" }) { Text(content.ifEmpty { "(empty)" }) }
    if (edit) TextInputDialog("Text", content, multiline = true, onDismiss = { edit = false }) { st.op("setText", "layer" to id, "content" to it, "t" to st.playhead) }
    SectionTitle("Font")
    val fonts = remember(st.revision) { NativeBridge.call("fonts").arr("fonts").objects().map { it.optString("name") } }
    EnumPicker("Font", td.optString("font"), fonts.map { it to it }) { st.setProp(id, "text.font", it) }
    PropEditor(st, layer, "text.size", "Size", min = 8.0, max = 400.0)
    PropEditor(st, layer, "text.fill", "Fill", "color")
    PropEditor(st, layer, "text.strokeWidth", "Stroke width", min = 0.0, max = 40.0)
    PropEditor(st, layer, "text.strokeColor", "Stroke color", "color")
    PropEditor(st, layer, "text.tracking", "Tracking", min = -50.0, max = 200.0)
    PropEditor(st, layer, "text.leading", "Line spacing", min = 0.5, max = 3.0)
    PropEditor(st, layer, "text.baselineShift", "Baseline shift", min = -100.0, max = 100.0)
    EnumPicker("Alignment", td.optString("align", "center"), listOf("left" to "Left", "center" to "Center", "right" to "Right")) { st.setProp(id, "text.align", it) }
    NumberRow("Box width (0 = auto)", td.optDouble("boxWidth", 0.0), 0.0, st.compWidth.toDouble(), onPreview = {}) { st.setProp(id, "text.boxWidth", it) }
    LabeledSwitch("Faux italic", td.optBoolean("fauxItalic")) { st.setProp(id, "text.fauxItalic", it) }
    SectionTitle("Shadow & background")
    LabeledSwitch("Drop shadow", td.obj("shadow").optBoolean("enabled")) { st.setProp(id, "text.shadow.enabled", it) }
    if (td.obj("shadow").optBoolean("enabled")) {
        PropEditor(st, layer, "text.shadow.color", "Shadow color", "color")
        PropEditor(st, layer, "text.shadow.distance", "Distance", min = 0.0, max = 100.0)
        PropEditor(st, layer, "text.shadow.angle", "Angle", min = 0.0, max = 360.0)
        PropEditor(st, layer, "text.shadow.blur", "Softness", min = 0.0, max = 60.0)
    }
    LabeledSwitch("Background box", td.obj("background").optBoolean("enabled")) { st.setProp(id, "text.background.enabled", it) }
    if (td.obj("background").optBoolean("enabled")) {
        PropEditor(st, layer, "text.background.color", "Box color", "color")
        PropEditor(st, layer, "text.background.padding", "Padding", min = 0.0, max = 100.0)
        PropEditor(st, layer, "text.background.radius", "Corner radius", min = 0.0, max = 100.0)
    }
    LabeledSwitch("3D extrude (needs 3D layer)", td.obj("extrude").optBoolean("enabled")) {
        st.apply(jo("op" to "batch", "label" to "Extrude Text", "ops" to listOf(
            jo("op" to "setProp", "layer" to id, "path" to "text.extrude.enabled", "value" to it),
            jo("op" to "setLayer", "layer" to id, "fields" to jo("threeD" to (it || layer.optBoolean("threeD")))))))
    }
    if (td.obj("extrude").optBoolean("enabled")) {
        PropEditor(st, layer, "text.extrude.depth", "Depth", min = 0.0, max = 300.0)
        PropEditor(st, layer, "text.extrude.sideColor", "Side color", "color")
    }
    SectionTitle("Animation", "Presets add a text animator that starts at the playhead. Animators affect characters, words or lines through a range selector.")
    FlowRow {
        st.registries.arr("textPresets").strings().forEach { p ->
            Chip(p.replace(Regex("([A-Z])"), " $1").let(::cap)) { st.op("textPreset", "layer" to id, "preset" to p, "t" to st.playhead, "duration" to 1.0) }
        }
        IconAction(Icons.Filled.AddCircle, "+ Custom animator") { customAnim = true }
    }
    td.arr("animators").objects().forEach { a ->
        val aid = a.optString("id")
        val base = "text.animators.$aid"
        Column(Modifier.padding(vertical = 4.dp).background(PanelHi, RoundedCornerShape(8.dp)).padding(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Animator $aid", modifier = Modifier.weight(1f))
                val en = a.optBoolean("enabled", true)
                IconBtn(if (en) Icons.Filled.Visibility else Icons.Filled.VisibilityOff, if (en) "Disable animator" else "Enable animator") { st.setProp(id, "$base.enabled", !en) }
                IconBtn(Icons.Filled.Delete, "Remove animator") { st.op("removeAnimator", "layer" to id, "animator" to aid) }
            }
            val sel = a.obj("selector")
            EnumPicker("Based on", sel.optString("unit", "char"), listOf("char" to "Characters", "word" to "Words", "line" to "Lines")) { st.setProp(id, "$base.selector.unit", it) }
            EnumPicker("Shape", sel.optString("shape", "square"), listOf("square", "rampUp", "rampDown", "triangle", "round", "smooth").map { it to cap(it) }) { st.setProp(id, "$base.selector.shape", it) }
            PropEditor(st, layer, "$base.selector.start", "Range start", min = 0.0, max = 100.0)
            PropEditor(st, layer, "$base.selector.end", "Range end", min = 0.0, max = 100.0)
            PropEditor(st, layer, "$base.selector.offset", "Range offset", min = -100.0, max = 100.0)
            val props = a.obj("props")
            props.keys().forEach { k ->
                when (k) {
                    "position" -> PropEditor(st, layer, "$base.props.position", "Position offset", "vector", -500.0, 500.0)
                    "fill" -> PropEditor(st, layer, "$base.props.fill", "Fill color", "color")
                    "scramble", "clip" -> LabeledSwitch(cap(k), props.optBoolean(k)) { st.setProp(id, "$base.props.$k", it) }
                    else -> PropEditor(st, layer, "$base.props.$k", cap(k), min = if (k == "rotation") -360.0 else 0.0, max = if (k == "rotation") 360.0 else if (k == "scale") 400.0 else 100.0)
                }
            }
        }
    }
    if (customAnim) CustomAnimatorDialog(onDismiss = { customAnim = false }) { anim -> st.op("addAnimator", "layer" to id, "animator" to anim) }
}

@Composable
private fun CustomAnimatorDialog(onDismiss: () -> Unit, onOk: (JSONObject) -> Unit) {
    val all = listOf("opacity", "position", "scale", "rotation", "fill", "tracking", "blur", "scramble", "clip")
    var chosen by remember { mutableStateOf(setOf("opacity")) }
    AlertDialog(onDismissRequest = onDismiss, title = { Text("New text animator") }, text = {
        Column(Modifier.verticalScroll(rememberScrollState())) {
            SmallLabel("Properties the animator changes on selected characters:")
            all.forEach { p -> CheckRow(cap(p), p in chosen) { chosen = if (it) chosen + p else chosen - p } }
        }
    }, confirmButton = { TextButton(onClick = {
        val props = JSONObject()
        chosen.forEach { p ->
            when (p) {
                "opacity" -> props.put(p, jo("v" to 0.0)); "position" -> props.put(p, jo("v" to listOf(0.0, 50.0)))
                "scale" -> props.put(p, jo("v" to 150.0)); "rotation" -> props.put(p, jo("v" to 45.0))
                "fill" -> props.put(p, jo("v" to listOf(1.0, 0.8, 0.2, 1.0))); "tracking" -> props.put(p, jo("v" to 20.0))
                "blur" -> props.put(p, jo("v" to 10.0)); else -> props.put(p, true)
            }
        }
        onOk(jo("enabled" to true, "selector" to jo("start" to jo("v" to 0.0), "end" to jo("v" to 100.0), "offset" to jo("v" to 0.0), "unit" to "char", "shape" to "square"), "props" to props))
        onDismiss()
    }) { Text("Add") } }, dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } })
}

// ====================================================================== Shape
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ShapePanel(app: AppState, layer: JSONObject) {
    val st = app.editor
    val id = layer.optString("id")
    if (!layer.has("shape")) { Text("Not a shape layer.", color = TextDim); return }
    val sh = layer.obj("shape")
    SectionTitle("Shapes", "Each item is combined with the ones above it using its merge mode (add, subtract, intersect).")
    FlowRow {
        listOf("rect", "ellipse", "star", "polygon").forEach { t ->
            Chip("+ ${cap(t)}") {
                val item = jo("type" to t, "op" to "add", "position" to jo("v" to listOf(0.0, 0.0)))
                when (t) {
                    "rect" -> { item.put("size", jo("v" to listOf(200.0, 200.0))); item.put("roundness", jo("v" to 0.0)) }
                    "ellipse" -> item.put("size", jo("v" to listOf(200.0, 200.0)))
                    else -> { item.put("points", jo("v" to 5.0)); item.put("outerRadius", jo("v" to 120.0)); item.put("rotation", jo("v" to 0.0)); if (t == "star") item.put("innerRadius", jo("v" to 50.0)) }
                }
                st.op("addShapeItem", "layer" to id, "item" to item)
            }
        }
    }
    val items = sh.arr("items").objects()
    items.forEach { it0 ->
        val iid = it0.optString("id")
        val base = "shape.items.$iid"
        val type = it0.optString("type")
        Column(Modifier.padding(vertical = 4.dp).background(PanelHi, RoundedCornerShape(8.dp)).padding(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("${cap(type)} $iid", modifier = Modifier.weight(1f))
                IconBtn(Icons.Filled.Delete, "Remove shape item", enabled = items.size > 1) { st.op("removeShapeItem", "layer" to id, "item" to iid) }
            }
            EnumPicker("Merge", it0.optString("op", "add"), listOf("add" to "Add", "subtract" to "Subtract", "intersect" to "Intersect")) { st.setProp(id, "$base.op", it) }
            PropEditor(st, layer, "$base.position", "Position", "vector", -1000.0, 1000.0)
            when (type) {
                "rect" -> { PropEditor(st, layer, "$base.size", "Size", "vector", 0.0, 2000.0, componentLabels = listOf("W", "H")); PropEditor(st, layer, "$base.roundness", "Roundness", min = 0.0, max = 500.0) }
                "ellipse" -> PropEditor(st, layer, "$base.size", "Size", "vector", 0.0, 2000.0, componentLabels = listOf("W", "H"))
                "star", "polygon" -> {
                    PropEditor(st, layer, "$base.points", "Points", min = 3.0, max = 40.0)
                    PropEditor(st, layer, "$base.outerRadius", "Outer radius", min = 0.0, max = 1000.0)
                    if (type == "star") PropEditor(st, layer, "$base.innerRadius", "Inner radius", min = 0.0, max = 1000.0)
                    PropEditor(st, layer, "$base.rotation", "Rotation", min = -360.0, max = 360.0)
                }
                "line", "arrow" -> {
                    PropEditor(st, layer, "$base.from", "From", "vector", -1000.0, 1000.0)
                    PropEditor(st, layer, "$base.to", "To", "vector", -1000.0, 1000.0)
                    if (type == "arrow") PropEditor(st, layer, "$base.headSize", "Head size", min = 0.0, max = 200.0)
                }
                "path" -> SmallLabel("Freeform path. Redraw it with the Pen tool, or animate it with Trim Paths below.")
            }
        }
    }
    SectionTitle("Fill")
    val fill = sh.obj("fill")
    LabeledSwitch("Fill", fill.optBoolean("enabled")) { st.setProp(id, "shape.fill.enabled", it) }
    if (fill.optBoolean("enabled")) {
        EnumPicker("Fill type", fill.optString("type", "solid"), listOf("solid" to "Solid", "linear" to "Linear gradient", "radial" to "Radial gradient")) { st.setProp(id, "shape.fill.type", it) }
        if (fill.optString("type", "solid") == "solid") PropEditor(st, layer, "shape.fill.color", "Color", "color")
        else {
            PropEditor(st, layer, "shape.fill.gradient.start", "Gradient start", "vector", -1000.0, 1000.0)
            PropEditor(st, layer, "shape.fill.gradient.end", "Gradient end", "vector", -1000.0, 1000.0)
            val stops = fill.obj("gradient").arr("stops")
            for (i in 0 until stops.length()) {
                val s = stops.getJSONArray(i)
                ColorRow("Stop ${i + 1}", doubleArrayOf(s.optDouble(1), s.optDouble(2), s.optDouble(3), s.optDouble(4, 1.0))) { c ->
                    val n = JSONArray(stops.toString())
                    n.put(i, JSONArray(listOf(s.optDouble(0), c[0], c[1], c[2], c[3])))
                    st.setProp(id, "shape.fill.gradient.stops", n)
                }
            }
        }
        PropEditor(st, layer, "shape.fill.opacity", "Fill opacity", min = 0.0, max = 100.0)
    }
    SectionTitle("Stroke")
    val stroke = sh.obj("stroke")
    LabeledSwitch("Stroke", stroke.optBoolean("enabled")) { st.setProp(id, "shape.stroke.enabled", it) }
    if (stroke.optBoolean("enabled")) {
        PropEditor(st, layer, "shape.stroke.color", "Color", "color")
        PropEditor(st, layer, "shape.stroke.width", "Width", min = 0.0, max = 100.0)
        PropEditor(st, layer, "shape.stroke.opacity", "Opacity", min = 0.0, max = 100.0)
        EnumPicker("Cap", stroke.optString("cap", "round"), listOf("butt", "round", "square").map { it to cap(it) }) { st.setProp(id, "shape.stroke.cap", it) }
        EnumPicker("Join", stroke.optString("join", "round"), listOf("miter", "round", "bevel").map { it to cap(it) }) { st.setProp(id, "shape.stroke.join", it) }
        LabeledSwitch("Dashed", stroke.arr("dash").length() > 0) { st.setProp(id, "shape.stroke.dash", if (it) JSONArray(listOf(30.0, 20.0)) else JSONArray()) }
        if (stroke.arr("dash").length() > 0) PropEditor(st, layer, "shape.stroke.dashOffset", "Dash offset", min = 0.0, max = 200.0)
    }
    SectionTitle("Path operators", "Non-destructive modifiers applied in order: trim, zig-zag, round corners, twist, offset, repeater.")
    OpSection(st, id, sh, "trim", "Trim paths") {
        PropEditor(st, layer, "shape.trim.start", "Start %", min = 0.0, max = 100.0)
        PropEditor(st, layer, "shape.trim.end", "End %", min = 0.0, max = 100.0)
        PropEditor(st, layer, "shape.trim.offset", "Offset °", min = -360.0, max = 360.0)
    }
    OpSection(st, id, sh, "zigzag", "Zig-zag") {
        PropEditor(st, layer, "shape.zigzag.size", "Size", min = 0.0, max = 100.0)
        PropEditor(st, layer, "shape.zigzag.ridges", "Ridges per segment", min = 0.0, max = 50.0)
        LabeledSwitch("Smooth", sh.obj("zigzag").optBoolean("smooth")) { st.setProp(id, "shape.zigzag.smooth", it) }
    }
    OpSection(st, id, sh, "roundCorners", "Round corners") { PropEditor(st, layer, "shape.roundCorners.radius", "Radius", min = 0.0, max = 300.0) }
    OpSection(st, id, sh, "twist", "Twist") { PropEditor(st, layer, "shape.twist.angle", "Angle", min = -720.0, max = 720.0) }
    OpSection(st, id, sh, "offsetPath", "Offset path") { PropEditor(st, layer, "shape.offsetPath.amount", "Amount", min = -100.0, max = 100.0) }
    OpSection(st, id, sh, "repeater", "Repeater") {
        PropEditor(st, layer, "shape.repeater.copies", "Copies", min = 1.0, max = 50.0)
        PropEditor(st, layer, "shape.repeater.offset", "Offset", min = -10.0, max = 10.0)
        PropEditor(st, layer, "shape.repeater.position", "Position step", "vector", -1000.0, 1000.0)
        PropEditor(st, layer, "shape.repeater.scale", "Scale step", "vector", 0.0, 200.0)
        PropEditor(st, layer, "shape.repeater.rotation", "Rotation step", min = -360.0, max = 360.0)
        PropEditor(st, layer, "shape.repeater.startOpacity", "Start opacity", min = 0.0, max = 100.0)
        PropEditor(st, layer, "shape.repeater.endOpacity", "End opacity", min = 0.0, max = 100.0)
    }
    OpSection(st, id, sh, "extrude", "3D extrude") {
        PropEditor(st, layer, "shape.extrude.depth", "Depth", min = 0.0, max = 400.0)
        PropEditor(st, layer, "shape.extrude.sideColor", "Side color", "color")
        if (!layer.optBoolean("threeD")) IconAction(Icons.Filled.ViewInAr, "Make layer 3D") { st.op("setLayer", "layer" to id, "fields" to jo("threeD" to true)) }
    }
}

@Composable
private fun OpSection(st: EditorState, id: String, sh: JSONObject, key: String, label: String, body: @Composable () -> Unit) {
    LabeledSwitch(label, sh.obj(key).optBoolean("enabled")) { st.setProp(id, "shape.$key.enabled", it) }
    if (sh.obj(key).optBoolean("enabled")) body()
}

// ====================================================================== Audio
@Composable
fun AudioPanel(app: AppState, layer: JSONObject) {
    val st = app.editor
    val id = layer.optString("id")
    if (!layer.has("audio")) { Text("This layer has no audio. Audio controls appear on video and audio layers.", color = TextDim); return }
    val au = layer.obj("audio")
    LabeledSwitch("Mute layer", layer.optBoolean("muted")) { st.op("setLayer", "layer" to id, "fields" to jo("muted" to it)) }
    PropEditor(st, layer, "audio.volume", "Volume (dB)", min = -60.0, max = 12.0, help = "Keyframe this to automate volume. 0 dB = unchanged.")
    PropEditor(st, layer, "audio.pan", "Pan", min = -100.0, max = 100.0)
    EnumPicker("Bus", au.optString("bus", "dialogue"), listOf("dialogue", "music", "effects").map { it to cap(it) }) { st.setProp(id, "audio.bus", it) }
    NumberRow("Fade in (s)", au.optDouble("fadeIn"), 0.0, 5.0, onPreview = {}) { st.setProp(id, "audio.fadeIn", it) }
    NumberRow("Fade out (s)", au.optDouble("fadeOut"), 0.0, 5.0, onPreview = {}) { st.setProp(id, "audio.fadeOut", it) }
    SectionTitle("Equalizer")
    listOf("low" to "Low (bass)", "mid" to "Mid", "high" to "High (treble)").forEach { (k, n) ->
        NumberRow("$n dB", au.obj("eq").optDouble(k), -18.0, 18.0, onPreview = {}) { st.setProp(id, "audio.eq.$k", it) }
    }
    SectionTitle("Dynamics & cleanup")
    LabeledSwitch("Compressor", au.obj("compressor").optBoolean("enabled")) { st.setProp(id, "audio.compressor.enabled", it) }
    if (au.obj("compressor").optBoolean("enabled")) {
        NumberRow("Threshold (dB)", au.obj("compressor").optDouble("threshold", -18.0), -60.0, 0.0, onPreview = {}) { st.setProp(id, "audio.compressor.threshold", it) }
        NumberRow("Ratio", au.obj("compressor").optDouble("ratio", 3.0), 1.0, 20.0, onPreview = {}) { st.setProp(id, "audio.compressor.ratio", it) }
    }
    NumberRow("Noise gate / denoise", au.optDouble("denoise"), 0.0, 100.0, help = "Attenuates quiet background noise between words.", onPreview = {}) { st.setProp(id, "audio.denoise", it) }
    NumberRow("Pitch (semitones)", au.optDouble("pitch"), -12.0, 12.0, onPreview = {}) { st.setProp(id, "audio.pitch", it) }
    SectionTitle("Ducking", "Lower this (music) layer automatically wherever a dialogue layer has speech.")
    val dialogue = st.layers.filter { it.has("audio") && it.optString("id") != id }
    if (dialogue.isEmpty()) SmallLabel("Add a dialogue clip to duck under it.")
    dialogue.forEach { d ->
        Chip("Duck under '${d.optString("name")}'") {
            val r = NativeBridge.call("speechRanges", jo("layer" to d.optString("id")))
            if (!r.optBoolean("ok")) app.toast(r.optString("error"), true)
            else if (r.arr("ranges").length() == 0) app.toast("No speech detected in '${d.optString("name")}'.", true)
            else st.op("addDuckingKeys", "layer" to id, "ranges" to r.arr("ranges"), "amount" to -12.0)
        }
    }
    SectionTitle("Mixer")
    AudioBuses(st)
}

// ====================================================================== Time
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun TimePanel(app: AppState, layer: JSONObject) {
    val st = app.editor
    val id = layer.optString("id")
    val speed = layer.optDouble("speed", 1.0)
    SectionTitle("Timing")
    Text("In ${st.timecode(layer.optDouble("in"))}  •  Out ${st.timecode(layer.optDouble("out"))}  •  Length ${"%.2f".format(layer.optDouble("out") - layer.optDouble("in"))}s", fontSize = 13.sp)
    FlowRow {
        IconAction(Icons.Filled.ContentCut, "Split at playhead") { st.op("split", "layers" to listOf(id), "t" to st.playhead) }
        IconAction(Icons.Filled.FirstPage, "Trim start to playhead") { st.op("trimLayer", "layer" to id, "edge" to "in", "t" to st.playhead) }
        IconAction(Icons.Filled.LastPage, "Trim end to playhead") { st.op("trimLayer", "layer" to id, "edge" to "out", "t" to st.playhead) }
        IconAction(Icons.Filled.FirstPage, "Move start to playhead") { st.op("moveLayerTime", "layers" to listOf(id), "dt" to st.playhead - layer.optDouble("in")) }
        IconAction(Icons.Filled.DeleteSweep, "Ripple delete") { st.op("rippleDelete", "layer" to id) }
    }
    SectionTitle("Speed")
    NumberRow("Speed %", speed * 100, 10.0, 400.0, help = "Changes clip duration. Keyframes are re-timed with the clip.", onPreview = {}) { st.op("setSpeed", "layer" to id, "speed" to it / 100.0) }
    FlowRow {
        listOf(0.25, 0.5, 1.0, 2.0, 4.0).forEach { s -> Chip("${(s * 100).toInt()}%", Math.abs(speed - s) < 1e-6) { st.op("setSpeed", "layer" to id, "speed" to s) } }
    }
    LabeledSwitch("Reverse", layer.optBoolean("reverse")) { st.op("reverse", "layer" to id, "on" to it) }
    if (layer.optString("type") in setOf("video", "precomp")) {
        IconAction(Icons.Filled.PauseCircle, "Freeze frame (2 s) at playhead") { st.op("freezeFrame", "layer" to id, "t" to st.playhead, "duration" to 2.0) }
        SectionTitle("Speed ramps & time remap", "Time remapping maps layer time to source time with keyframes for smooth speed ramps.")
        FlowRow { st.registries.arr("speedRamps").strings().forEach { p -> Chip(cap(p), layer.obj("timeRemap").optString("preset") == p) { st.op("speedRampPreset", "layer" to id, "preset" to p) } } }
        val tr = layer.optJSONObject("timeRemap")
        LabeledSwitch("Time remap", tr?.optBoolean("enabled") == true) { st.op("setTimeRemap", "layer" to id, "enabled" to it) }
        if (tr?.optBoolean("enabled") == true) PropEditor(st, layer, "timeRemap.prop", "Source time (s)", min = 0.0, max = (layer.optDouble("out") - layer.optDouble("start")) * speed)
    }
    SectionTitle("Frame blending & echo")
    SmallLabel("Use Effects › Time (Echo, Posterize Time) and the composition's Motion Blur switch for time-based looks.")
}

// ====================================================================== Transitions
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun TransitionsPanel(app: AppState, layer: JSONObject) {
    val st = app.editor
    val id = layer.optString("id")
    val reg = st.registries.arr("transitions").objects()
    listOf("in" to "Transition in", "out" to "Transition out").forEach { (edge, label) ->
        val key = if (edge == "in") "transitionIn" else "transitionOut"
        val cur = layer.optJSONObject(key)
        SectionTitle(label)
        FlowRow {
            Chip("None", cur == null) { st.op("setTransition", "layer" to id, "edge" to edge, "transition" to null) }
            reg.forEach { t ->
                Chip(t.optString("name"), cur?.optString("type") == t.optString("type")) {
                    val params = JSONObject()
                    t.arr("params").objects().forEach { p -> params.put(p.optString("name"), p.opt("def")) }
                    st.op("setTransition", "layer" to id, "edge" to edge, "transition" to jo("type" to t.optString("type"), "duration" to (cur?.optDouble("duration") ?: 0.5), "params" to params))
                }
            }
        }
        if (cur != null) {
            NumberRow("Duration (s)", cur.optDouble("duration", 0.5), 0.05, 3.0, onPreview = {}) { d ->
                st.op("setTransition", "layer" to id, "edge" to edge, "transition" to JSONObject(cur.toString()).put("duration", d))
            }
            val info = st.transitionInfo(cur.optString("type"))
            info?.arr("params")?.objects()?.forEach { p ->
                val pname = p.optString("name")
                val v = cur.obj("params").opt(pname) ?: p.opt("def")
                fun setParam(x: Any?) = st.op("setTransition", "layer" to id, "edge" to edge, "transition" to JSONObject(cur.toString()).also { it.put("params", JSONObject(it.obj("params").toString()).put(pname, x)) })
                when (p.optString("kind")) {
                    "color" -> ColorRow(p.optString("label"), (v as? JSONArray).toDoubles(4)) { setParam(JSONArray(it.toList())) }
                    "bool" -> LabeledSwitch(p.optString("label"), v == true) { setParam(it) }
                    "enum" -> EnumPicker(p.optString("label"), v?.toString() ?: "", p.arr("options").strings().map { it to it }) { setParam(it) }
                    else -> NumberRow(p.optString("label"), (v as? Number)?.toDouble() ?: 0.0, p.optDouble("min", 0.0), p.optDouble("max", 100.0), onPreview = {}) { setParam(it) }
                }
            }
        }
    }
}

// ====================================================================== Behaviors
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun BehaviorsPanel(app: AppState, layer: JSONObject) {
    val st = app.editor
    val id = layer.optString("id")
    SectionTitle("Add behavior", "Behaviors animate procedurally without keyframes (wiggle, bounce, orbit…). Bake to convert them to editable keyframes.")
    FlowRow { st.registries.arr("behaviors").objects().forEach { b -> Chip(b.optString("name")) { st.op("addBehavior", "layer" to id, "type" to b.optString("type")) } } }
    layer.arr("behaviors").objects().forEach { b ->
        val bid = b.optString("id")
        val info = st.behaviorInfo(b.optString("type"))
        Column(Modifier.padding(vertical = 4.dp).background(PanelHi, RoundedCornerShape(8.dp)).padding(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(info?.optString("name") ?: b.optString("type"), modifier = Modifier.weight(1f))
                val en = b.optBoolean("enabled", true)
                IconBtn(if (en) Icons.Filled.Visibility else Icons.Filled.VisibilityOff, if (en) "Disable behavior" else "Enable behavior") { st.op("setBehavior", "layer" to id, "behavior" to bid, "fields" to jo("enabled" to !en)) }
                IconBtn(Icons.Filled.Delete, "Remove behavior") { st.op("removeBehavior", "layer" to id, "behavior" to bid) }
            }
            info?.optString("help")?.takeIf { it.isNotEmpty() }?.let { SmallLabel(it) }
            if (info != null) ParamList(st, layer, "behaviors.$bid.params", info.arr("params"))
            val link = b.optJSONObject("audioLink")
            EnumPicker("React to audio", link?.optString("band") ?: "", listOf("" to "Off", "amplitude" to "Amplitude", "bass" to "Bass", "mid" to "Mid", "treble" to "Treble", "beat" to "Beat")) { band ->
                st.op("setBehavior", "layer" to id, "behavior" to bid, "fields" to jo("audioLink" to if (band.isEmpty()) null else jo("band" to band, "amount" to 1.0)))
            }
            IconAction(Icons.Filled.Diamond, "Bake to keyframes") { st.op("bakeBehavior", "layer" to id, "behavior" to bid) }
        }
    }
}

// ====================================================================== Keyframes / graph editor
private val animPaths = listOf(
    "transform.position" to "Position", "transform.scale" to "Scale", "transform.rotation" to "Rotation", "transform.opacity" to "Opacity", "transform.anchor" to "Anchor",
)

/** All animated property paths within a layer (recursive), for the graph editor's property list. */
fun animatedPaths(layer: JSONObject): List<String> {
    val out = ArrayList<String>()
    fun walk(o: Any?, path: String) {
        when (o) {
            is JSONObject -> {
                if (o.isAnimated()) { out.add(path); return }
                o.keys().forEach { k -> walk(o.opt(k), if (path.isEmpty()) k else "$path.$k") }
            }
            is JSONArray -> for (i in 0 until o.length()) {
                val c = o.optJSONObject(i) ?: continue
                walk(c, "$path.${c.optString("id").ifEmpty { i.toString() }}")
            }
        }
    }
    walk(layer, "")
    return out
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun KeyframesPanel(app: AppState, ui: EditorUi, layer: JSONObject) {
    val st = app.editor
    val id = layer.optString("id")
    val paths = (animatedPaths(layer) + animPaths.map { it.first }).distinct()
    val path = ui.graphPath?.takeIf { it in paths } ?: paths.first()
    SectionTitle("Property", "Pick a property. Animated properties are marked with ◆.")
    FlowRow {
        paths.forEach { p ->
            val animated = (layer.resolve(p) as? JSONObject)?.isAnimated() == true
            Chip((if (animated) "◆ " else "") + (animPaths.firstOrNull { it.first == p }?.second ?: p.substringAfterLast('.')), p == path) { ui.graphPath = p }
        }
    }
    val prop = layer.resolve(path) as? JSONObject
    val keys = prop?.arr("k")?.objects() ?: emptyList()
    val start = layer.optDouble("start")
    GraphEditor(st, layer, path, keys)
    val atKey = keys.firstOrNull { Math.abs(it.optDouble("t") + start - st.playhead) < 0.5 / st.fps }
    FlowRow {
        Chip(if (atKey != null) "Remove key" else "Add key at playhead") {
            st.op(if (atKey != null) "removeKeyframe" else "addKeyframe", "layer" to id, "path" to path, "t" to st.playhead)
        }
        if (keys.isNotEmpty()) {
            IconAction(Icons.Filled.SkipPrevious, "◀ Prev key") { keys.map { it.optDouble("t") + start }.lastOrNull { it < st.playhead - 1e-6 }?.let { st.playhead = it } }
            IconAction(Icons.Filled.SkipNext, "Next key ▶") { keys.map { it.optDouble("t") + start }.firstOrNull { it > st.playhead + 1e-6 }?.let { st.playhead = it } }
            IconAction(Icons.Filled.ContentCopy, "Copy keys") {
                val r = NativeBridge.call("copyKeyframes", jo("layer" to id, "path" to path))
                if (r.optBoolean("ok")) { Clipboard.keyframes = r.obj("clip"); Clipboard.keyframePath = path; app.toast("Copied ${r.obj("clip").arr("k").length()} keyframes") } else app.toast(r.optString("error"), true)
            }
            IconAction(Icons.Filled.SwapHoriz, "Reverse") { st.op("reverseKeyframes", "layer" to id, "path" to path) }
            IconAction(Icons.Filled.ViewWeek, "Distribute evenly") { st.op("distributeKeyframes", "layer" to id, "path" to path) }
            IconAction(Icons.Filled.Expand, "Stretch ×2") { st.op("scaleKeyframes", "layer" to id, "path" to path, "factor" to 2.0, "pivot" to (keys.first().optDouble("t") + start)) }
            IconAction(Icons.Filled.Compress, "Squeeze ×½") { st.op("scaleKeyframes", "layer" to id, "path" to path, "factor" to 0.5, "pivot" to (keys.first().optDouble("t") + start)) }
            IconAction(Icons.Filled.KeyboardArrowRight, "Nudge +1f") { st.op("nudgeKeyframes", "layer" to id, "path" to path, "dt" to 1.0 / st.fps) }
            IconAction(Icons.Filled.KeyboardArrowLeft, "Nudge −1f") { st.op("nudgeKeyframes", "layer" to id, "path" to path, "dt" to -1.0 / st.fps) }
            IconAction(Icons.Filled.LayersClear, "Clear animation") { st.op("clearKeyframes", "layer" to id, "path" to path, "t" to st.playhead) }
        }
        Clipboard.keyframes?.let { clip -> IconAction(Icons.Filled.ContentPaste, "Paste at playhead") { st.op("pasteKeyframes", "layer" to id, "path" to path, "clip" to clip, "t" to st.playhead) } }
    }
    if (keys.isNotEmpty()) {
        SectionTitle("Easing", "Applies to the keyframe at the playhead (the segment leaving it), or to all keyframes.")
        var all by remember { mutableStateOf(false) }
        CheckRow("Apply to all keyframes", all) { all = it }
        FlowRow {
            st.registries.arr("easings").strings().forEach { e ->
                Chip(e, atKey?.opt("o")?.toString() == e) {
                    if (!all && atKey == null) app.toast("Move the playhead onto a keyframe (use Prev/Next key).", true)
                    else st.op("setKeyframeInterp", "layer" to id, "path" to path, "t" to st.playhead, "interp" to e, "all" to all)
                }
            }
        }
        val o = atKey?.opt("o") as? JSONObject
        val bez = if (o?.optString("type") == "bezier") o.arr("p").toDoubles(4) else doubleArrayOf(0.33, 0.0, 0.67, 1.0)
        SmallLabel("Custom bezier (drag the handles):")
        BezierEditor(bez) { p ->
            if (!all && atKey == null) app.toast("Move the playhead onto a keyframe first.", true)
            else st.op("setKeyframeInterp", "layer" to id, "path" to path, "t" to st.playhead, "interp" to jo("type" to "bezier", "p" to p.toList()), "all" to all)
        }
    }
    SectionTitle("Expression", "JavaScript evaluated per frame. Use time, value, wiggle(freq, amp), loopOut(), thisComp.layer(\"Name\"). Vector math works: value + [10, 0].")
    ExpressionEditor(app, layer, path)
}

@Composable
fun GraphEditor(st: EditorState, layer: JSONObject, path: String, keys: List<JSONObject>) {
    val id = layer.optString("id")
    val start = layer.optDouble("start")
    val t0 = layer.optDouble("in")
    val t1 = maxOf(layer.optDouble("out"), t0 + 0.1)
    val samples = remember(st.revision, path, id) {
        val r = NativeBridge.call("sampleProp", jo("layer" to id, "path" to path, "t0" to t0, "t1" to t1, "n" to 120))
        r.arr("values").let { a -> (0 until a.length()).map { i -> when (val v = a.opt(i)) { is Number -> doubleArrayOf(v.toDouble()); is JSONArray -> v.toDoubles(); else -> DoubleArray(0) } } }
    }
    val dims = samples.maxOfOrNull { it.size } ?: 0
    var lo = Double.MAX_VALUE; var hi = -Double.MAX_VALUE
    samples.forEach { s -> s.take(minOf(3, s.size)).forEach { lo = minOf(lo, it); hi = maxOf(hi, it) } }
    if (lo > hi) { lo = 0.0; hi = 1.0 }
    if (hi - lo < 1e-6) { hi += 1; lo -= 1 }
    val colors = listOf(Color(0xFFFF6A6A), Color(0xFF6AFF8A), Color(0xFF6AA8FF))
    var dragKey by remember { mutableStateOf<Double?>(null) }
    var dragX by remember { mutableStateOf(0f) }
    Box(Modifier.fillMaxWidth().height(180.dp).background(Color(0xFF0C0D10), RoundedCornerShape(8.dp))
        .semantics { contentDescription = "Graph editor for $path" }
        .pointerInput(path, st.revision) {
            detectTapGestures { p -> st.playhead = st.snap(t0 + (p.x / size.width) * (t1 - t0)) }
        }
        .pointerInput(path, st.revision) {
            detectDragGestures(onDragStart = { p ->
                val tt = t0 + (p.x / size.width) * (t1 - t0)
                dragKey = keys.map { it.optDouble("t") + start }.minByOrNull { Math.abs(it - tt) }?.takeIf { Math.abs(it - tt) < (t1 - t0) * 0.04 }
                dragX = p.x
            }, onDragEnd = {
                dragKey?.let { from ->
                    val to = st.snap(t0 + (dragX / size.width) * (t1 - t0))
                    if (Math.abs(to - from) > 1e-6) st.op("moveKeyframe", "layer" to id, "path" to path, "from" to from, "to" to to)
                }
                dragKey = null
            }, onDragCancel = { dragKey = null }) { ch, d -> ch.consume(); dragX += d.x }
        }) {
        Canvas(Modifier.matchParentSize()) {
            val w = size.width; val h = size.height
            fun y(v: Double) = (h - 8 - (v - lo) / (hi - lo) * (h - 16)).toFloat()
            for (c in 0 until minOf(3, dims)) {
                val path2 = Path()
                samples.forEachIndexed { i, s -> if (c < s.size) { val x = w * i / (samples.size - 1); if (i == 0) path2.moveTo(x, y(s[c])) else path2.lineTo(x, y(s[c])) } }
                drawPath(path2, if (dims == 1) Accent else colors[c], style = Stroke(2.dp.toPx()))
            }
            keys.forEach { k ->
                val kt = k.optDouble("t") + start
                val x = ((kt - t0) / (t1 - t0) * w).toFloat()
                drawLine(KeyColor.copy(alpha = 0.3f), Offset(x, 0f), Offset(x, h))
                drawCircle(KeyColor, 5.dp.toPx(), Offset(x, h - 8.dp.toPx()))
            }
            dragKey?.let { drawLine(Color.White, Offset(dragX, 0f), Offset(dragX, h), 2f) }
            val px = ((st.playhead - t0) / (t1 - t0) * w).toFloat()
            drawLine(Accent, Offset(px, 0f), Offset(px, h), 2f)
        }
        Text("%.1f – %.1f".format(lo, hi), fontSize = 10.sp, color = TextDim, modifier = Modifier.padding(4.dp))
    }
}


@Composable
fun BezierEditor(p: DoubleArray, onCommit: (DoubleArray) -> Unit) {
    var pts by remember(p.toList()) { mutableStateOf(p.copyOf()) }
    var which by remember { mutableStateOf(-1) }
    Box(Modifier.size(180.dp).background(Color(0xFF0C0D10), RoundedCornerShape(8.dp)).semantics { contentDescription = "Bezier easing editor" }
        .pointerInput(Unit) {
            detectDragGestures(onDragStart = { o ->
                val pad = 20f; val s = size.width - 2 * pad
                val h1 = Offset(pad + pts[0].toFloat() * s, pad + (1 - pts[1].toFloat()) * s)
                val h2 = Offset(pad + pts[2].toFloat() * s, pad + (1 - pts[3].toFloat()) * s)
                which = if ((o - h1).getDistance() < (o - h2).getDistance()) 0 else 1
            }, onDragEnd = { onCommit(pts); which = -1 }) { ch, d ->
                ch.consume()
                val pad = 20f; val s = size.width - 2 * pad
                val n = pts.copyOf()
                val i = which * 2
                if (i >= 0) {
                    n[i] = (n[i] + d.x / s).coerceIn(0.0, 1.0)
                    n[i + 1] = (n[i + 1] - d.y / s).coerceIn(-0.5, 1.5)
                    pts = n
                }
            }
        }) {
        Canvas(Modifier.size(180.dp)) {
            val pad = 20f; val s = size.width - 2 * pad
            fun pt(x: Double, y: Double) = Offset(pad + x.toFloat() * s, pad + (1 - y.toFloat()) * s)
            val a = pt(0.0, 0.0); val b = pt(1.0, 1.0); val c1 = pt(pts[0], pts[1]); val c2 = pt(pts[2], pts[3])
            drawRect(Color(0xFF22242A), topLeft = pt(0.0, 1.0), size = androidx.compose.ui.geometry.Size(s, s), style = Stroke(1f))
            val path = Path().apply { moveTo(a.x, a.y); cubicTo(c1.x, c1.y, c2.x, c2.y, b.x, b.y) }
            drawPath(path, Accent, style = Stroke(3f))
            drawLine(TextDim, a, c1); drawLine(TextDim, b, c2)
            drawCircle(KeyColor, 8f, c1); drawCircle(KeyColor, 8f, c2)
        }
    }
    Text("cubic-bezier(${pts.joinToString(", ") { "%.2f".format(it) }})", fontSize = 11.sp, color = TextDim)
}

@Composable
fun ExpressionEditor(app: AppState, layer: JSONObject, path: String) {
    val st = app.editor
    val id = layer.optString("id")
    val prop = layer.resolve(path) as? JSONObject
    var text by remember(path, id, st.revision) { mutableStateOf(prop?.optString("x") ?: "") }
    var status by remember(path, id) { mutableStateOf("") }
    OutlinedTextField(text, { text = it }, modifier = Modifier.fillMaxWidth().semantics { contentDescription = "Expression for $path" },
        placeholder = { Text("e.g. wiggle(2, 30)") }, textStyle = androidx.compose.ui.text.TextStyle(fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace, fontSize = 13.sp, color = Color.White), minLines = 2)
    if (status.isNotEmpty()) Text(status, color = if (status.startsWith("OK")) Color(0xFF66BB6A) else Color(0xFFFF7A7A), fontSize = 12.sp)
    FlowRow {
        IconAction(Icons.Filled.Rule, "Check") {
            val r = NativeBridge.call("validateExpression", jo("source" to text))
            status = if (r.optBoolean("ok")) "OK — expression compiles." else r.optString("error")
        }
        IconAction(Icons.Filled.Check, "Apply") { if (st.op("setExpression", "layer" to id, "path" to path, "expr" to text) != null) status = if (text.isBlank()) "Expression removed." else "OK — applied." }
        if (prop?.has("x") == true) {
            val en = prop.optBoolean("xe", true)
            Chip(if (en) "Disable" else "Enable") { st.op("setExpression", "layer" to id, "path" to path, "expr" to prop.optString("x"), "enabled" to !en) }
            IconAction(Icons.Filled.Delete, "Remove") { st.op("setExpression", "layer" to id, "path" to path, "expr" to "") }
        }
        listOf("wiggle(2, 30)", "loopOut()", "time * 90", "value + [0, Math.sin(time * 4) * 40]").forEach { ex -> Chip(ex) { text = ex } }
    }
}

// ====================================================================== 3D
@Composable
fun ThreeDPanel(app: AppState, layer: JSONObject) {
    val st = app.editor
    val id = layer.optString("id")
    val type = layer.optString("type")
    LabeledSwitch("3D layer", layer.optBoolean("threeD")) { st.op("setLayer", "layer" to id, "fields" to jo("threeD" to it)) }
    if (layer.optBoolean("threeD") && type !in setOf("camera", "light")) {
        PropEditor(st, layer, "transform.position", "Position", "vector", -5000.0, 5000.0)
        PropEditor(st, layer, "transform.orientation", "Orientation", "vector", -360.0, 360.0)
        PropEditor(st, layer, "transform.rotationX", "X rotation", min = -360.0, max = 360.0)
        PropEditor(st, layer, "transform.rotationY", "Y rotation", min = -360.0, max = 360.0)
    }
    when (type) {
        "camera" -> {
            SectionTitle("Camera")
            PropEditor(st, layer, "transform.position", "Position", "vector", -10000.0, 10000.0)
            PropEditor(st, layer, "camera.poi", "Point of interest", "vector", -10000.0, 10000.0)
            PropEditor(st, layer, "camera.zoom", "Zoom", min = 100.0, max = 10000.0)
            EnumPicker("Auto-orient", layer.obj("camera").optString("autoOrient", "poi"), listOf("poi" to "Towards point of interest", "none" to "Off")) { st.setProp(id, "camera.autoOrient", it) }
            LabeledSwitch("Depth of field", layer.obj("camera").optBoolean("dof")) { st.setProp(id, "camera.dof", it) }
            if (layer.obj("camera").optBoolean("dof")) {
                PropEditor(st, layer, "camera.focusDistance", "Focus distance", min = 10.0, max = 10000.0)
                PropEditor(st, layer, "camera.aperture", "Aperture (blur)", min = 0.0, max = 100.0)
            }
            SectionTitle("Camera moves")
            Row {
                Chip("Push in") { cameraMove(st, layer, 0.0, 0.0, 600.0) }
                Chip("Orbit") { cameraOrbit(st, layer) }
                Chip("Truck") { cameraMove(st, layer, 500.0, 0.0, 0.0) }
            }
        }
        "light" -> {
            SectionTitle("Light")
            val lt = layer.obj("light")
            EnumPicker("Type", lt.optString("kind"), listOf("point", "spot", "directional", "ambient").map { it to cap(it) }) { st.setProp(id, "light.kind", it) }
            PropEditor(st, layer, "transform.position", "Position", "vector", -10000.0, 10000.0)
            PropEditor(st, layer, "light.color", "Color", "color")
            PropEditor(st, layer, "light.intensity", "Intensity %", min = 0.0, max = 400.0)
            if (lt.optString("kind") == "spot") {
                PropEditor(st, layer, "light.poi", "Target", "vector", -10000.0, 10000.0)
                PropEditor(st, layer, "light.coneAngle", "Cone angle", min = 1.0, max = 180.0)
                PropEditor(st, layer, "light.coneFeather", "Cone feather", min = 0.0, max = 100.0)
            }
            EnumPicker("Falloff", lt.optString("falloff", "none"), listOf("none" to "None", "smooth" to "Smooth", "inverseSquare" to "Inverse square")) { st.setProp(id, "light.falloff", it) }
            if (lt.optString("falloff", "none") != "none") PropEditor(st, layer, "light.radius", "Radius", min = 10.0, max = 10000.0)
            LabeledSwitch("Casts shadows", lt.optBoolean("castShadows")) { st.setProp(id, "light.castShadows", it) }
            if (lt.optBoolean("castShadows")) PropEditor(st, layer, "light.shadowDarkness", "Shadow darkness", min = 0.0, max = 100.0)
        }
        "model3d" -> {
            SectionTitle("Model")
            val m = layer.obj("model")
            EnumPicker("Primitive", m.optString("primitive"), listOf("" to "Imported model") + st.registries.arr("primitives").strings().map { it to cap(it) }) { st.setProp(id, "model.primitive", it) }
            PropEditor(st, layer, "model.size", "Size", min = 10.0, max = 2000.0)
            SectionTitle("Material")
            PropEditor(st, layer, "model.material.baseColor", "Base color", "color")
            PropEditor(st, layer, "model.material.metallic", "Metallic", min = 0.0, max = 1.0)
            PropEditor(st, layer, "model.material.roughness", "Roughness", min = 0.0, max = 1.0)
            PropEditor(st, layer, "model.material.emission", "Emission color", "color")
            PropEditor(st, layer, "model.material.emissionStrength", "Emission strength", min = 0.0, max = 10.0)
            PropEditor(st, layer, "model.material.opacity", "Opacity", min = 0.0, max = 100.0)
            LabeledSwitch("Casts shadows", m.optBoolean("castShadows", true)) { st.setProp(id, "model.castShadows", it) }
            LabeledSwitch("Receives shadows", m.optBoolean("receiveShadows", true)) { st.setProp(id, "model.receiveShadows", it) }
            LabeledSwitch("Wireframe", m.optBoolean("wireframe")) { st.setProp(id, "model.wireframe", it) }
        }
    }
    if (st.layers.none { it.optString("type") == "camera" } && layer.optBoolean("threeD")) {
        Gap(); SmallLabel("No camera yet: the default 50 mm view is used.")
        IconAction(Icons.Filled.Videocam, "Add camera") { st.addLayer("camera") }
    }
}

private fun cameraMove(st: EditorState, cam: JSONObject, dx: Double, dy: Double, dz: Double) {
    val id = cam.optString("id")
    val p = st.evalProps(id, listOf("transform.position", "camera.poi"))
    val pos = p.optJSONArray("transform.position").toDoubles(3)
    val poi = p.optJSONArray("camera.poi").toDoubles(3)
    val t0 = st.playhead
    val t1 = minOf(st.duration, t0 + 3.0)
    val end = listOf(pos[0] + dx, pos[1] + dy, pos[2] + dz)
    val endPoi = listOf(poi[0] + dx, poi[1] + dy, poi[2] + if (dz != 0.0) 0.0 else dz)
    st.apply(jo("op" to "batch", "label" to "Camera Move", "ops" to listOf(
        jo("op" to "addKeyframe", "layer" to id, "path" to "transform.position", "t" to t0, "value" to pos.toList(), "interp" to "easeInOut"),
        jo("op" to "addKeyframe", "layer" to id, "path" to "transform.position", "t" to t1, "value" to end, "interp" to "easeInOut"),
        jo("op" to "addKeyframe", "layer" to id, "path" to "camera.poi", "t" to t0, "value" to poi.toList(), "interp" to "easeInOut"),
        jo("op" to "addKeyframe", "layer" to id, "path" to "camera.poi", "t" to t1, "value" to endPoi, "interp" to "easeInOut"))))
}

private fun cameraOrbit(st: EditorState, cam: JSONObject) {
    val id = cam.optString("id")
    val p = st.evalProps(id, listOf("transform.position", "camera.poi"))
    val pos = p.optJSONArray("transform.position").toDoubles(3)
    val poi = p.optJSONArray("camera.poi").toDoubles(3)
    val r = Math.hypot(pos[0] - poi[0], pos[2] - poi[2])
    val a0 = Math.atan2(pos[2] - poi[2], pos[0] - poi[0])
    val t0 = st.playhead
    val ops = (0..8).map { i ->
        val a = a0 + i / 8.0 * Math.PI / 2
        jo("op" to "addKeyframe", "layer" to id, "path" to "transform.position", "t" to minOf(st.duration, t0 + i * 0.5), "value" to listOf(poi[0] + Math.cos(a) * r, pos[1], poi[2] + Math.sin(a) * r), "interp" to "linear")
    }
    st.apply(jo("op" to "batch", "label" to "Camera Orbit", "ops" to ops))
}

// ====================================================================== Particles
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ParticlesPanel(app: AppState, layer: JSONObject) {
    val st = app.editor
    val id = layer.optString("id")
    if (!layer.has("particles")) { Text("Not a particle layer. Add one from + Layer › Particles.", color = TextDim); return }
    val p = layer.obj("particles")
    SectionTitle("Emitter")
    PropEditor(st, layer, "particles.rate", "Birth rate /s", min = 0.0, max = 500.0)
    PropEditor(st, layer, "particles.lifetime", "Lifetime (s)", min = 0.1, max = 10.0)
    PropEditor(st, layer, "particles.emitterSize", "Emitter size", "vector", 0.0, 2000.0, componentLabels = listOf("W", "H"))
    PropEditor(st, layer, "particles.direction", "Direction °", min = -180.0, max = 180.0)
    PropEditor(st, layer, "particles.spread", "Spread °", min = 0.0, max = 360.0)
    PropEditor(st, layer, "particles.velocity", "Velocity", min = 0.0, max = 2000.0)
    SectionTitle("Physics")
    PropEditor(st, layer, "particles.gravity", "Gravity", min = -1000.0, max = 1000.0)
    PropEditor(st, layer, "particles.turbulence", "Turbulence", min = 0.0, max = 300.0)
    LabeledSwitch("Attractor", p.obj("attractor").optBoolean("enabled")) { st.setProp(id, "particles.attractor.enabled", it) }
    if (p.obj("attractor").optBoolean("enabled")) {
        PropEditor(st, layer, "particles.attractor.position", "Attractor position", "vector", -2000.0, 2000.0)
        PropEditor(st, layer, "particles.attractor.strength", "Attractor strength", min = -1000.0, max = 1000.0)
    }
    SectionTitle("Appearance")
    EnumPicker("Particle shape", p.optString("shape", "circle"), listOf("circle", "square", "star", "spark", "glow").map { it to cap(it) }) { st.setProp(id, "particles.shape", it) }
    PropEditor(st, layer, "particles.size", "Size at birth", min = 0.0, max = 100.0)
    PropEditor(st, layer, "particles.sizeEnd", "Size at death", min = 0.0, max = 100.0)
    PropEditor(st, layer, "particles.opacityStart", "Opacity at birth", min = 0.0, max = 100.0)
    PropEditor(st, layer, "particles.opacityEnd", "Opacity at death", min = 0.0, max = 100.0)
    PropEditor(st, layer, "particles.colorStart", "Color at birth", "color")
    PropEditor(st, layer, "particles.colorEnd", "Color at death", "color")
    PropEditor(st, layer, "particles.spin", "Spin °/s", min = -720.0, max = 720.0)
    PropEditor(st, layer, "particles.trails", "Trails", min = 0.0, max = 100.0)
    LabeledSwitch("Glow", p.optBoolean("glow")) { st.setProp(id, "particles.glow", it) }
    LabeledSwitch("Multicolor", p.optBoolean("multicolor")) { st.setProp(id, "particles.multicolor", it) }
    NumberRow("Random seed", p.optDouble("seed", 1.0), 1.0, 100.0, onPreview = {}) { st.setProp(id, "particles.seed", it.toInt()) }
    NumberRow("Max particles", p.optDouble("maxParticles", 4000.0), 100.0, 20000.0, help = "Upper bound protects playback speed on slower phones.", onPreview = {}) { st.setProp(id, "particles.maxParticles", it.toInt()) }
}

// ====================================================================== Tracking
@Composable
fun TrackingPanel(app: AppState, ui: EditorUi, layer: JSONObject) {
    val st = app.editor
    val id = layer.optString("id")
    val scope = rememberCoroutineScope()
    var progress by remember { mutableStateOf<Float?>(null) }
    var smoothing by remember { mutableStateOf(0.5) }
    var rotation by remember { mutableStateOf(true) }
    if (layer.optString("type") != "video") { Text("Select a video layer to track or stabilize it.", color = TextDim); return }
    SectionTitle("Point tracking", "Tap Track, then tap a high-contrast feature in the preview. A 'Track Null' follows it; parent text or graphics to the null.")
    Chip(if (ui.previewMode == "track") "Tap a feature in the preview…" else "Track a point", ui.previewMode == "track") { ui.previewMode = if (ui.previewMode == "track") "select" else "track" }
    SectionTitle("Stabilize", "Smooths camera shake by counter-animating position/rotation and scaling slightly to hide edges.")
    NumberRow("Smoothing (s)", smoothing, 0.1, 3.0, onPreview = {}) { smoothing = it }
    CheckRow("Stabilize rotation", rotation) { rotation = it }
    progress?.let {
        LinearProgressIndicator(progress = { it }, modifier = Modifier.fillMaxWidth())
        TextButton(onClick = { progress = null }) { Text("Cancel") }
    }
    if (progress == null) IconAction(Icons.Filled.Straighten, "Stabilize clip") {
        progress = 0f
        scope.launch {
            val r = withContext(Dispatchers.Default) {
                NativeBridge.task("stabilize", jo("layer" to id, "smoothing" to smoothing, "rotation" to rotation), ProgressCallback { p -> progress = p; progress != null })
            }
            val cancelled = progress == null
            progress = null
            if (cancelled) { app.toast("Stabilization cancelled."); return@launch }
            if (!r.optBoolean("ok")) { app.toast(r.optString("error").ifEmpty { "Stabilization failed." }, true); return@launch }
            val samples = r.arr("samples")
            val base = st.evalProps(id, listOf("transform.position", "transform.rotation", "transform.scale"))
            val pos = base.optJSONArray("transform.position").toDoubles(2)
            val rot = base.optDouble("transform.rotation", 0.0)
            val scl = base.optJSONArray("transform.scale").toDoubles(2)
            val posS = JSONArray(); val rotS = JSONArray(); var maxScale = 100.0
            for (i in 0 until samples.length()) {
                val s = samples.getJSONArray(i)
                posS.put(JSONArray(listOf(s.getDouble(0), pos[0] + s.getDouble(1), pos[1] + s.getDouble(2))))
                rotS.put(JSONArray(listOf(s.getDouble(0), rot + s.getDouble(3))))
                maxScale = maxOf(maxScale, s.optDouble(4, 100.0))
            }
            val ops = mutableListOf(jo("op" to "setTrackKeyframes", "layer" to id, "path" to "transform.position", "samples" to posS))
            if (rotation) ops.add(jo("op" to "setTrackKeyframes", "layer" to id, "path" to "transform.rotation", "samples" to rotS))
            ops.add(jo("op" to "setProp", "layer" to id, "path" to "transform.scale", "value" to listOf(scl[0] * maxScale / 100, scl[1] * maxScale / 100, 100.0), "mode" to "static"))
            if (st.apply(jo("op" to "batch", "label" to "Stabilize", "ops" to ops)) != null) app.toast("Stabilized ${samples.length()} frames (crop ${"%.0f".format(maxScale)}%).")
        }
    }
}
