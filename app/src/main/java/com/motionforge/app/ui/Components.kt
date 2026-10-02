package com.motionforge.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Timer
import androidx.compose.material.icons.outlined.HelpOutline
import androidx.compose.material.icons.outlined.Timer
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.motionforge.app.Settings
import com.motionforge.app.engine.EditorState
import com.motionforge.app.engine.isAnimated
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.objects
import com.motionforge.app.engine.resolve
import org.json.JSONArray
import org.json.JSONObject

val Accent = Color(0xFFFF6A3D)
val Panel = Color(0xFF17181C)
val PanelHi = Color(0xFF22242A)
val TextDim = Color(0xFF9A9CA4)
val KeyColor = Color(0xFFFFC23D)

@Composable
fun MfTheme(content: @Composable () -> Unit) {
    val hc = Settings.highContrast
    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = Accent, secondary = Color(0xFF4FC3F7), background = if (hc) Color.Black else Color(0xFF101114),
            surface = if (hc) Color.Black else Panel, onSurface = Color.White, onBackground = Color.White, surfaceVariant = PanelHi,
        ),
        content = content,
    )
}

/** Icon button with mandatory accessibility label and a 48dp minimum touch target. */
@Composable
fun IconBtn(icon: ImageVector, label: String, enabled: Boolean = true, tint: Color = Color.White, modifier: Modifier = Modifier, onClick: () -> Unit) {
    IconButton(onClick = onClick, enabled = enabled, modifier = modifier.sizeIn(minWidth = 48.dp, minHeight = 48.dp)) {
        Icon(icon, contentDescription = label, tint = if (enabled) tint else tint.copy(alpha = 0.3f))
    }
}

@Composable
fun SectionTitle(text: String, help: String? = null) {
    var show by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(top = 10.dp, bottom = 4.dp)) {
        Text(text.uppercase(), color = TextDim, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
        if (help != null) IconBtn(Icons.Outlined.HelpOutline, "Help: $text") { show = true }
    }
    if (show && help != null) InfoDialog(text, help) { show = false }
}

@Composable
fun InfoDialog(title: String, text: String, onDismiss: () -> Unit) {
    AlertDialog(onDismissRequest = onDismiss, confirmButton = { TextButton(onClick = onDismiss) { Text("OK") } }, title = { Text(title) }, text = { Text(text) })
}

@Composable
fun ConfirmDialog(title: String, text: String, confirm: String = "Confirm", onDismiss: () -> Unit, onConfirm: () -> Unit) {
    AlertDialog(onDismissRequest = onDismiss, title = { Text(title) }, text = { Text(text) },
        confirmButton = { TextButton(onClick = { onConfirm(); onDismiss() }) { Text(confirm) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } })
}

@Composable
fun TextInputDialog(title: String, initial: String, label: String = "", multiline: Boolean = false, onDismiss: () -> Unit, onOk: (String) -> Unit) {
    var v by remember { mutableStateOf(initial) }
    AlertDialog(onDismissRequest = onDismiss, title = { Text(title) },
        text = { OutlinedTextField(v, { v = it }, label = { Text(label) }, singleLine = !multiline, modifier = Modifier.fillMaxWidth()) },
        confirmButton = { TextButton(onClick = { onOk(v); onDismiss() }) { Text("OK") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } })
}

@Composable
fun Chip(text: String, selected: Boolean = false, modifier: Modifier = Modifier, onClick: () -> Unit) {
    Box(
        modifier
            .padding(3.dp)
            .background(if (selected) Accent.copy(alpha = 0.25f) else PanelHi, RoundedCornerShape(16.dp))
            .border(1.dp, if (selected) Accent else Color.Transparent, RoundedCornerShape(16.dp))
            .clickable(onClick = onClick)
            .sizeIn(minHeight = 40.dp)
            .padding(horizontal = 12.dp, vertical = 9.dp)
            .semantics { stateDescription = if (selected) "selected" else "not selected" },
        contentAlignment = Alignment.Center,
    ) { Text(text, fontSize = 13.sp, color = Color.White) }
}

@Composable
fun LabeledSwitch(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth().sizeIn(minHeight = 48.dp).clickable { onChange(!checked) }) {
        Text(label, modifier = Modifier.weight(1f))
        Switch(checked, onChange, modifier = Modifier.semantics { contentDescription = label })
    }
}

@Composable
fun <T> EnumPicker(label: String, value: T, options: List<Pair<T, String>>, onPick: (T) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth().sizeIn(minHeight = 48.dp)) {
        Text(label, modifier = Modifier.weight(1f), color = TextDim)
        Box {
            TextButton(onClick = { open = true }) { Text(options.firstOrNull { it.first == value }?.second ?: value.toString()) }
            DropdownMenu(open, { open = false }) {
                options.forEach { (v, n) -> DropdownMenuItem(text = { Text(n) }, onClick = { onPick(v); open = false }) }
            }
        }
    }
}

/** Number editor: slider for coarse changes + text entry for exact values. Live preview while dragging, one undo step on release. */
@Composable
fun NumberRow(label: String, value: Double, min: Double, max: Double, animated: Boolean = false, keyed: Boolean = false,
              onKeyToggle: (() -> Unit)? = null, help: String? = null, onPreview: (Double) -> Unit, onCommit: (Double) -> Unit) {
    var edit by remember { mutableStateOf(false) }
    var showHelp by remember { mutableStateOf(false) }
    var dragging by remember { mutableStateOf<Double?>(null) }
    val shown = dragging ?: value
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            if (onKeyToggle != null) {
                IconBtn(if (animated) Icons.Filled.Timer else Icons.Outlined.Timer,
                    if (keyed) "Remove keyframe for $label" else if (animated) "Add keyframe for $label" else "Animate $label",
                    tint = if (keyed) KeyColor else if (animated) KeyColor.copy(alpha = 0.6f) else TextDim) { onKeyToggle() }
            }
            Text(label, modifier = Modifier.weight(1f), fontSize = 14.sp)
            if (help != null) IconBtn(Icons.Outlined.HelpOutline, "Help: $label") { showHelp = true }
            TextButton(onClick = { edit = true }, modifier = Modifier.semantics { contentDescription = "$label value ${"%.2f".format(shown)}" }) {
                Text(fmt(shown), color = if (animated) KeyColor else Color.White)
            }
        }
        val lo = minOf(min, shown)
        val hi = maxOf(max, shown)
        Slider(
            value = shown.toFloat().coerceIn(lo.toFloat(), hi.toFloat()),
            onValueChange = { dragging = it.toDouble(); onPreview(it.toDouble()) },
            onValueChangeFinished = { dragging?.let { onCommit(it) }; dragging = null },
            valueRange = lo.toFloat()..hi.toFloat(),
            modifier = Modifier.semantics { contentDescription = "$label slider" },
        )
    }
    if (edit) TextInputDialog(label, fmt(value), "Value") { s -> s.toDoubleOrNull()?.let(onCommit) }
    if (showHelp && help != null) InfoDialog(label, help) { showHelp = false }
}

fun fmt(v: Double): String = if (Math.abs(v - Math.rint(v)) < 1e-6) "%.0f".format(v) else "%.2f".format(v)

@Composable
fun ColorRow(label: String, rgba: DoubleArray, onCommit: (DoubleArray) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth().sizeIn(minHeight = 48.dp).clickable { open = true }) {
        Text(label, modifier = Modifier.weight(1f))
        Box(Modifier.size(32.dp).background(Color(rgba[0].toFloat(), rgba[1].toFloat(), rgba[2].toFloat(), rgba.getOrElse(3) { 1.0 }.toFloat()), CircleShape)
            .border(1.dp, Color.Gray, CircleShape).semantics { contentDescription = "$label color" })
    }
    if (open) ColorPickerDialog(label, rgba, { open = false }, onCommit)
}

@Composable
fun ColorPickerDialog(title: String, initial: DoubleArray, onDismiss: () -> Unit, onOk: (DoubleArray) -> Unit) {
    var c by remember { mutableStateOf(initial.copyOf(4).also { if (initial.size < 4) it[3] = 1.0 }) }
    var hex by remember { mutableStateOf(toHex(c)) }
    AlertDialog(onDismissRequest = onDismiss, title = { Text(title) }, text = {
        Column {
            Box(Modifier.fillMaxWidth().height(40.dp).background(Color(c[0].toFloat(), c[1].toFloat(), c[2].toFloat(), c[3].toFloat()), RoundedCornerShape(8.dp)))
            listOf("Red", "Green", "Blue", "Alpha").forEachIndexed { i, n ->
                Text("$n ${(c[i] * 255).toInt()}", fontSize = 12.sp, color = TextDim)
                Slider(c[i].toFloat(), { v -> c = c.copyOf().also { it[i] = v.toDouble() }; hex = toHex(c) }, modifier = Modifier.semantics { contentDescription = "$n channel" })
            }
            OutlinedTextField(hex, { h -> hex = h; parseHex(h)?.let { c = it } }, label = { Text("Hex #RRGGBBAA") }, singleLine = true)
            Row { listOf("#FFFFFF", "#000000", "#FF6A3D", "#4FC3F7", "#66BB6A", "#FFD54F", "#EC407A", "#7E57C2").forEach { h ->
                val p = parseHex(h)!!
                Box(Modifier.padding(3.dp).size(28.dp).background(Color(p[0].toFloat(), p[1].toFloat(), p[2].toFloat()), CircleShape).clickable { c = p; hex = h }
                    .semantics { contentDescription = "Swatch $h" })
            } }
        }
    }, confirmButton = { TextButton(onClick = { onOk(c); onDismiss() }) { Text("OK") } }, dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } })
}

fun toHex(c: DoubleArray) = "#" + c.take(4).joinToString("") { "%02X".format((it.coerceIn(0.0, 1.0) * 255).toInt()) }
fun parseHex(h: String): DoubleArray? {
    val s = h.removePrefix("#")
    if (s.length != 6 && s.length != 8) return null
    val v = s.toLongOrNull(16) ?: return null
    return if (s.length == 6) doubleArrayOf(((v shr 16) and 255) / 255.0, ((v shr 8) and 255) / 255.0, (v and 255) / 255.0, 1.0)
    else doubleArrayOf(((v shr 24) and 255) / 255.0, ((v shr 16) and 255) / 255.0, ((v shr 8) and 255) / 255.0, (v and 255) / 255.0)
}

fun JSONArray?.toDoubles(n: Int = 0): DoubleArray {
    if (this == null) return DoubleArray(n)
    return DoubleArray(maxOf(n, length())) { if (it < length()) optDouble(it) else 0.0 }
}

/**
 * Generic editor for an animatable property at [path] inside a layer. Handles number, vector (per component), color,
 * bool, enum and string values, keyframe toggling at the playhead, and gesture previews.
 */
@Composable
fun PropEditor(state: EditorState, layer: JSONObject, path: String, label: String, kind: String = "number", min: Double = 0.0, max: Double = 100.0,
               options: List<String> = emptyList(), help: String? = null, componentLabels: List<String> = listOf("X", "Y", "Z")) {
    val id = layer.optString("id")
    val prop = layer.resolve(path)
    val obj = prop as? JSONObject
    val animated = obj?.isAnimated() == true
    val lt = state.localTime(layer)
    val keyed = animated && obj!!.optJSONArray("k")!!.objects().any { Math.abs(it.optDouble("t") - lt) < 0.5 / state.fps }
    val current: Any? = if (obj != null && (obj.has("v") || obj.has("k"))) state.evalProps(id, listOf(path)).opt(path) else prop
    val keyToggle: () -> Unit = {
        if (keyed) state.op("removeKeyframe", "layer" to id, "path" to path, "t" to state.playhead)
        else state.op("addKeyframe", "layer" to id, "path" to path, "t" to state.playhead)
    }
    when (kind) {
        "number", "angle", "percent" -> {
            val v = (current as? Number)?.toDouble() ?: 0.0
            NumberRow(label, v, min, max, animated, keyed, if (obj != null) keyToggle else null, help,
                onPreview = { state.previewProp(id, path, it) }, onCommit = { state.commitPreview("Change $label"); state.setProp(id, path, it) })
        }
        "vector", "point" -> {
            val arr = (current as? JSONArray).toDoubles()
            componentLabels.take(arr.size).forEachIndexed { i, cl ->
                NumberRow("$label $cl", arr[i], if (kind == "point") 0.0 else min, if (kind == "point") 1.0 else max, animated, keyed, if (i == 0 && obj != null) keyToggle else null, help,
                    onPreview = { nv -> state.previewProp(id, path, JSONArray(arr.copyOf().also { it[i] = nv }.toList())) },
                    onCommit = { nv -> state.commitPreview("Change $label"); state.setProp(id, path, JSONArray(arr.copyOf().also { it[i] = nv }.toList())) })
            }
        }
        "color" -> {
            val arr = (current as? JSONArray).toDoubles(4)
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (obj != null) IconBtn(if (animated) Icons.Filled.Timer else Icons.Outlined.Timer, "Animate $label", tint = if (keyed) KeyColor else TextDim) { keyToggle() }
                Box(Modifier.weight(1f)) { ColorRow(label, arr) { state.setProp(id, path, JSONArray(it.toList())) } }
            }
        }
        "bool" -> LabeledSwitch(label, (current as? Boolean) ?: ((current as? Number)?.toDouble() ?: 0.0) != 0.0) { state.setProp(id, path, it) }
        "enum" -> EnumPicker(label, (current as? String) ?: options.firstOrNull() ?: "", options.map { it to it }) { state.setProp(id, path, it, "static") }
        "string" -> {
            var edit by remember { mutableStateOf(false) }
            Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth().clickable { edit = true }.sizeIn(minHeight = 48.dp)) {
                Text(label, color = TextDim, modifier = Modifier.width(110.dp))
                Text((current as? String) ?: "", modifier = Modifier.weight(1f), maxLines = 2)
            }
            if (edit) TextInputDialog(label, (current as? String) ?: "", multiline = true, onDismiss = { edit = false }) { state.setProp(id, path, it) }
        }
        "curve" -> CurveEditor(label, (current as? JSONArray) ?: JSONArray("[[0,0],[1,1]]")) { state.setProp(id, path, it, "static") }
        "asset" -> {
            val luts = state.doc.optJSONArray("assets")?.objects()?.filter { it.optString("type") == "lut" } ?: emptyList()
            EnumPicker(label, (current as? String) ?: "", listOf("" to "None") + luts.map { it.optString("id") to it.optString("name") }) { state.setProp(id, path, it, "static") }
        }
    }
}

/** Editor for the parameter list of an effect / behavior / transition from registry metadata. */
@Composable
fun ParamList(state: EditorState, layer: JSONObject, basePath: String, params: JSONArray) {
    params.objects().forEach { p ->
        val kind = when (p.optString("kind")) {
            "point" -> "point"; "color" -> "color"; "bool" -> "bool"; "enum" -> "enum"; "curve" -> "curve"; "asset" -> "asset"; else -> "number"
        }
        val opts = p.optJSONArray("options")?.let { a -> (0 until a.length()).map { a.getString(it) } } ?: emptyList()
        PropEditor(state, layer, basePath + "." + p.optString("name"), p.optString("label"), kind, p.optDouble("min", 0.0), p.optDouble("max", 100.0),
            opts, p.optString("help").ifEmpty { null })
    }
}

@Composable
fun CurveEditor(label: String, points: JSONArray, onCommit: (JSONArray) -> Unit) {
    // Simple per-point editor (x/y in 0..1). The graph editor provides direct manipulation for keyframe curves.
    val pts = (0 until points.length()).map { points.getJSONArray(it).let { a -> a.optDouble(0) to a.optDouble(1) } }
    Column {
        Text(label, color = TextDim)
        pts.forEachIndexed { i, (x, y) ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Point ${i + 1}", modifier = Modifier.width(70.dp), fontSize = 12.sp)
                Slider(y.toFloat(), { v ->
                    val n = JSONArray(); pts.forEachIndexed { j, p -> n.put(JSONArray(listOf(p.first, if (j == i) v.toDouble() else p.second))) }; onCommit(n)
                }, modifier = Modifier.weight(1f).semantics { contentDescription = "$label point ${i + 1} output" })
            }
        }
        TextButton(onClick = {
            val n = JSONArray(); pts.forEach { n.put(JSONArray(listOf(it.first, it.second))) }
            n.put(JSONArray(listOf(0.5, 0.5)))
            val sorted = (0 until n.length()).map { n.getJSONArray(it) }.sortedBy { it.optDouble(0) }
            onCommit(JSONArray(sorted))
        }) { Text("Add point") }
    }
}

@Composable
fun SmallLabel(text: String) = Text(text, color = TextDim, fontSize = 12.sp)

@Composable
fun Gap(h: Int = 8) = Spacer(Modifier.height(h.dp))

fun opJson(name: String, vararg p: Pair<String, Any?>) = jo("op" to name, *p)

@Composable
fun RowButtons(vararg items: Pair<String, () -> Unit>) {
    Row(horizontalArrangement = Arrangement.spacedBy(4.dp), modifier = Modifier.fillMaxWidth()) {
        items.forEach { (t, f) -> Chip(t, onClick = f) }
    }
}

@Composable
fun CheckRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.clickable { onChange(!checked) }) {
        Checkbox(checked, onChange)
        Text(label)
    }
}
