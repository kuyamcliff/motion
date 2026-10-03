package com.motionforge.app.ui

import android.view.HapticFeedbackConstants
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.LockOpen
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material.icons.filled.VolumeOff
import androidx.compose.material.icons.filled.VolumeUp
import androidx.compose.material.icons.filled.Headset
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.motionforge.app.AppState
import com.motionforge.app.Settings
import com.motionforge.app.engine.EditorState
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.objects
import org.json.JSONObject
import kotlin.math.roundToInt

private val typeColors = mapOf(
    "video" to Color(0xFF3E6FB0), "image" to Color(0xFF5B8C5A), "audio" to Color(0xFF2E8B78), "text" to Color(0xFFB0563E),
    "shape" to Color(0xFFB08A3E), "solid" to Color(0xFF7A5BB0), "adjustment" to Color(0xFF8A8A8A), "null" to Color(0xFF555555),
    "camera" to Color(0xFF4A4A6A), "light" to Color(0xFF8A7A3A), "particles" to Color(0xFFB03E8A), "model3d" to Color(0xFF3EA0B0),
    "captions" to Color(0xFF6A6A3E), "precomp" to Color(0xFF9A6A3A),
)

@Composable
fun Timeline(app: AppState, ui: EditorUi, modifier: Modifier) {
    val st = app.editor
    val player = app.player ?: return
    val view = LocalView.current
    var scrollX by remember { mutableFloatStateOf(0f) }
    var lens by remember { mutableStateOf<Offset?>(null) }  // precision lens anchor (track area coords)
    val headerW = 150.dp
    val rowH = if (ui.expandedTracks) 64.dp else 52.dp
    val pxPerSec = ui.timelineZoom
    fun tToX(t: Double) = (t * pxPerSec - scrollX).toFloat()
    fun xToT(x: Float) = ((x + scrollX) / pxPerSec).toDouble().coerceAtLeast(0.0)
    val markers = st.comp.optJSONArray("markers")?.objects() ?: emptyList()
    val snapTimes = remember(st.revision) {
        (NativeBridge.call("editPoints").optJSONArray("times")?.let { a -> (0 until a.length()).map { a.getDouble(it) } } ?: emptyList())
    }
    fun snapT(t: Double, excludeLayer: String? = null): Double {
        if (!Settings.snapping) return st.snap(t)
        val cands = snapTimes + listOf(st.playhead) + st.keyframeTimes(st.selectedLayer)
        val thr = 10.0 / pxPerSec
        val best = cands.minByOrNull { Math.abs(it - t) }
        if (best != null && Math.abs(best - t) < thr) {
            if (Settings.haptics) view.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)
            return best
        }
        return st.snap(t)
    }

    Column(modifier.background(Color(0xFF131418))) {
        // Ruler.
        Row(Modifier.height(26.dp)) {
            Box(Modifier.width(headerW).fillMaxSize(), contentAlignment = Alignment.CenterStart) {
                Text("  " + st.timecode(st.playhead), fontFamily = FontFamily.Monospace, fontSize = 11.sp, color = TextDim)
            }
            Canvas(Modifier.weight(1f).fillMaxSize().semantics { contentDescription = "Time ruler. Tap or drag to move the playhead." }
                .pointerInput(pxPerSec) {
                    detectTapGestures { p -> player.seek(snapT(xToT(p.x))) }
                }
                .pointerInput(pxPerSec) {
                    awaitEachGesture {
                        awaitFirstDown()
                        do {
                            val e = awaitPointerEvent()
                            val c = e.changes.first()
                            if (!c.pressed) break
                            st.playhead = st.snap(xToT(c.position.x).coerceAtMost(st.duration))
                            player.requestRender()
                            c.consume()
                        } while (true)
                    }
                }) {
                drawRuler(st, pxPerSec, scrollX)
                markers.forEach { m ->
                    val x = tToX(m.optDouble("t"))
                    val col = m.optJSONArray("color")?.let { Color(it.optDouble(0).toFloat(), it.optDouble(1).toFloat(), it.optDouble(2).toFloat()) } ?: KeyColor
                    drawPath(Path().apply { moveTo(x - 6, 0f); lineTo(x + 6, 0f); lineTo(x, 10f); close() }, col)
                }
                if (player.rangeIn >= 0 || player.rangeOut > 0) {
                    val a = tToX(maxOf(0.0, player.rangeIn)); val b = tToX(if (player.rangeOut > 0) player.rangeOut else st.duration)
                    drawRect(Accent.copy(alpha = 0.25f), Offset(a, size.height - 5), Size(b - a, 5f))
                }
                val px = tToX(st.playhead)
                drawLine(Accent, Offset(px, 0f), Offset(px, size.height), 2f)
            }
        }
        // Layer rows.
        Box(Modifier.weight(1f)) {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                if (st.layers.isEmpty()) Text("Empty composition. Use + Media or + Layer to start.", color = TextDim, modifier = Modifier.padding(16.dp))
                st.layers.forEach { layer ->
                    LayerRow(app, ui, layer, rowH, headerW, pxPerSec, scrollX, { scrollX = it }, ::snapT, { lens = it })
                }
            }
            lens?.let { a -> PrecisionLens(st, a, pxPerSec, scrollX, headerW) }
        }
        // Zoom / pan strip.
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()
            .semantics { contentDescription = "Timeline zoom"; stateDescription = "${ui.timelineZoom.roundToInt()} pixels per second" }
            .pointerInput(Unit) {
                awaitEachGesture {
                    awaitFirstDown()
                    do {
                        val e = awaitPointerEvent()
                        if (e.changes.none { it.pressed }) break
                        val z = e.calculateZoom()
                        val p = e.calculatePan()
                        ui.timelineZoom = (ui.timelineZoom * z).coerceIn(8f, 1200f)
                        scrollX = (scrollX - p.x).coerceAtLeast(0f)
                        e.changes.forEach { it.consume() }
                    } while (true)
                }
            }) {
            Chip("−") { ui.timelineZoom = (ui.timelineZoom / 1.5f).coerceAtLeast(8f) }
            Chip("+") { ui.timelineZoom = (ui.timelineZoom * 1.5f).coerceAtMost(1200f) }
            Chip("Fit") { ui.timelineZoom = (800f / st.duration.toFloat()).coerceIn(8f, 1200f); scrollX = 0f }
            Chip(if (ui.expandedTracks) "Compact" else "Expand") { ui.expandedTracks = !ui.expandedTracks }
            Chip(if (Settings.snapping) "Snap on" else "Snap off", Settings.snapping) { Settings.snapping = !Settings.snapping }
            Chip(if (ui.multiSelect) "Multi-select on" else "Multi-select", ui.multiSelect) { ui.multiSelect = !ui.multiSelect }
            SmallLabel("  pinch here to zoom · drag to pan")
        }
    }
}

private fun DrawScope.drawRuler(st: EditorState, pxPerSec: Float, scrollX: Float) {
    val stepSec = when { pxPerSec > 400 -> 1.0 / st.fps * 5; pxPerSec > 150 -> 0.5; pxPerSec > 60 -> 1.0; pxPerSec > 20 -> 5.0; else -> 10.0 }
    var t = Math.floor(scrollX / pxPerSec / stepSec) * stepSec
    val paint = android.graphics.Paint().apply { color = android.graphics.Color.GRAY; textSize = 22f; isAntiAlias = true }
    while (true) {
        val x = (t * pxPerSec - scrollX).toFloat()
        if (x > size.width) break
        if (x >= 0) {
            drawLine(Color.Gray, Offset(x, size.height * 0.5f), Offset(x, size.height), 1f)
            drawContext.canvas.nativeCanvas.drawText(if (stepSec < 1) "%.2f".format(t) else "${t.toInt()}s", x + 3, size.height * 0.45f, paint)
        }
        t += stepSec
    }
    val end = (st.duration * pxPerSec - scrollX).toFloat()
    drawRect(Color(0x55000000), Offset(end, 0f), Size(maxOf(0f, size.width - end), size.height))
}

@Composable
private fun LayerRow(app: AppState, ui: EditorUi, layer: JSONObject, rowH: androidx.compose.ui.unit.Dp, headerW: androidx.compose.ui.unit.Dp, pxPerSec: Float,
                     scrollX: Float, setScroll: (Float) -> Unit, snapT: (Double, String?) -> Double, setLens: (Offset?) -> Unit) {
    val view = LocalView.current
    val st = app.editor
    val player = app.player!!
    val id = layer.optString("id")
    val selected = id in st.selection
    val type = layer.optString("type")
    var menu by remember { mutableStateOf(false) }
    val keyTimes = remember(st.revision, id) { st.keyframeTimes(layer) }
    val peaks = remember(layer.optString("asset")) {
        if (type == "audio" || (type == "video" && layer.has("audio"))) NativeBridge.call("peaks", jo("asset" to layer.optString("asset"), "buckets" to 400)).optJSONArray("peaks") else null
    }
    Row(Modifier.height(rowH).fillMaxWidth().background(if (selected) Color(0xFF23252D) else Color.Transparent)) {
        // Header: name + visibility/solo/lock/mute (states shown by icon, not color alone).
        Column(Modifier.width(headerW).fillMaxSize().padding(start = 4.dp)) {
            Text("${st.layers.indexOf(layer) + 1}. ${layer.optString("name")}", maxLines = 1, fontSize = 12.sp, fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
                modifier = Modifier.semantics { contentDescription = "Layer ${layer.optString("name")}, $type" })
            Row {
                val en = layer.optBoolean("enabled", true)
                IconBtn(if (en) Icons.Filled.Visibility else Icons.Filled.VisibilityOff, if (en) "Hide ${layer.optString("name")}" else "Show ${layer.optString("name")}", modifier = Modifier.width(36.dp)) {
                    st.op("setLayer", "layer" to id, "fields" to jo("enabled" to !en))
                }
                val solo = layer.optBoolean("solo")
                IconBtn(Icons.Filled.Headset, if (solo) "Unsolo" else "Solo", tint = if (solo) Accent else TextDim, modifier = Modifier.width(36.dp)) {
                    st.op("setLayer", "layer" to id, "fields" to jo("solo" to !solo))
                }
                val locked = layer.optBoolean("locked")
                IconBtn(if (locked) Icons.Filled.Lock else Icons.Filled.LockOpen, if (locked) "Unlock" else "Lock", modifier = Modifier.width(36.dp)) {
                    st.op("setLayer", "layer" to id, "fields" to jo("locked" to !locked))
                }
                if (layer.has("audio")) {
                    val muted = layer.optBoolean("muted")
                    IconBtn(if (muted) Icons.Filled.VolumeOff else Icons.Filled.VolumeUp, if (muted) "Unmute" else "Mute", modifier = Modifier.width(36.dp)) {
                        st.op("setLayer", "layer" to id, "fields" to jo("muted" to !muted))
                    }
                }
            }
        }
        Box(Modifier.weight(1f).fillMaxSize()) {
            var dragMode by remember { mutableStateOf("") }
            Canvas(Modifier.fillMaxSize()
                .semantics { contentDescription = "Clip ${layer.optString("name")} from ${"%.2f".format(layer.optDouble("in"))} to ${"%.2f".format(layer.optDouble("out"))} seconds" }
                .pointerInput(id, pxPerSec, scrollX) {
                    detectTapGestures(
                        onTap = { p ->
                            val t = ((p.x + scrollX) / pxPerSec).toDouble()
                            val onClip = t >= layer.optDouble("in") && t <= layer.optDouble("out")
                            when {
                                // Multi-select mode: taps add/remove clips; drags then move every selected clip.
                                ui.multiSelect && onClip -> st.selection = if (id in st.selection) st.selection - id else st.selection + id
                                onClip -> st.selection = setOf(id)
                                else -> { if (!ui.multiSelect) st.selection = emptySet(); player.seek(st.snap(t)) }
                            }
                        },
                        onDoubleTap = { st.selection = setOf(id); ui.tab = InspectorTab.Layer },
                        onLongPress = { p ->
                            setLens(Offset(p.x, 0f))
                            menu = true
                        },
                    )
                }
                .pointerInput(id, pxPerSec, scrollX, st.revision) {
                    awaitEachGesture {
                        val down = awaitFirstDown(requireUnconsumed = false)
                        val inX = (layer.optDouble("in") * pxPerSec - scrollX).toFloat()
                        val outX = (layer.optDouble("out") * pxPerSec - scrollX).toFloat()
                        val edge = 28f
                        val mode = when {
                            layer.optBoolean("locked") -> "pan"
                            Math.abs(down.position.x - inX) < edge -> "trimIn"
                            Math.abs(down.position.x - outX) < edge -> "trimOut"
                            down.position.x in inX..outX && id in st.selection -> "move"
                            else -> "pan"
                        }
                        var moved = false
                        var cancelled = false
                        var autoScroll = 0f
                        do {
                            val e = awaitPointerEvent()
                            if (e.changes.count { it.pressed } >= 2) {
                                // A second finger cancels an in-progress trim/move (nothing is committed) and
                                // turns the gesture into two-finger pan + pinch zoom of the timeline.
                                if (moved && (mode == "trimIn" || mode == "trimOut" || mode == "move") && !cancelled) {
                                    st.cancelPreview(); cancelled = true; setLens(null)
                                    if (Settings.haptics) view.performHapticFeedback(if (android.os.Build.VERSION.SDK_INT >= 30) HapticFeedbackConstants.REJECT else HapticFeedbackConstants.LONG_PRESS)
                                }
                                ui.timelineZoom = (ui.timelineZoom * e.calculateZoom()).coerceIn(8f, 1200f)
                                setScroll((scrollX - e.calculatePan().x).coerceAtLeast(0f)); e.changes.forEach { it.consume() }; continue
                            }
                            val c = e.changes.first()
                            if (!c.pressed) break
                            if (cancelled) { c.consume(); continue }
                            val dx = c.position.x - down.position.x + autoScroll
                            if (Math.abs(dx) > 8) moved = true
                            if (!moved) continue
                            dragMode = mode
                            // Edge auto-scroll: dragging a clip/handle near either edge scrolls the timeline.
                            if (mode != "pan") {
                                val edgeZone = 48f
                                val step = when { c.position.x > size.width - edgeZone -> 12f; c.position.x < edgeZone && scrollX > 0 -> -12f; else -> 0f }
                                if (step != 0f) { setScroll((scrollX + step).coerceAtLeast(0f)); autoScroll += step }
                            }
                            val t = ((c.position.x + scrollX + autoScroll) / pxPerSec).toDouble()
                            when (mode) {
                                "trimIn" -> { setLens(Offset(c.position.x, 0f)); st.preview(jo("op" to "trimLayer", "layer" to id, "edge" to "in", "t" to snapT(t, id))) }
                                "trimOut" -> { setLens(Offset(c.position.x, 0f)); st.preview(jo("op" to "trimLayer", "layer" to id, "edge" to "out", "t" to snapT(t, id))) }
                                "move" -> {
                                    val dt = dx / pxPerSec
                                    val newIn = snapT(layer.optDouble("in") + dt, id)
                                    val moving = if (st.selection.size > 1 && id in st.selection) st.selection.toList() else listOf(id)
                                    st.preview(jo("op" to "moveLayerTime", "layers" to moving, "dt" to (newIn - layer.optDouble("in"))))
                                }
                                else -> setScroll((scrollX - (c.position.x - c.previousPosition.x)).coerceAtLeast(0f))
                            }
                            c.consume()
                        } while (true)
                        setLens(null)
                        if (moved && !cancelled) when (mode) {
                            "trimIn", "trimOut" -> st.commitPreview("Trim Clip")
                            "move" -> st.commitPreview(if (st.selection.size > 1) "Move ${st.selection.size} Layers" else "Move Layer")
                        }
                        dragMode = ""
                    }
                }) {
                val x0 = (layer.optDouble("in") * pxPerSec - scrollX).toFloat()
                val x1 = (layer.optDouble("out") * pxPerSec - scrollX).toFloat()
                val col = typeColors[type] ?: Color.Gray
                val en = layer.optBoolean("enabled", true)
                drawRoundRect(col.copy(alpha = if (en) 0.85f else 0.3f), Offset(x0, 6f), Size(maxOf(2f, x1 - x0), size.height - 12f), CornerRadius(8f))
                if (selected) drawRoundRect(Color.White, Offset(x0, 6f), Size(maxOf(2f, x1 - x0), size.height - 12f), CornerRadius(8f), style = Stroke(3f))
                // Trim handles.
                drawRect(Color.White.copy(alpha = 0.5f), Offset(x0, 6f), Size(6f, size.height - 12f))
                drawRect(Color.White.copy(alpha = 0.5f), Offset(x1 - 6f, 6f), Size(6f, size.height - 12f))
                // Waveform.
                if (peaks != null && peaks.length() > 0) {
                    val asset = st.asset(layer.optString("asset"))
                    val dur = asset?.optDouble("duration", 1.0) ?: 1.0
                    val mid = size.height / 2
                    val n = peaks.length()
                    var x = maxOf(0f, x0)
                    while (x < minOf(x1, size.width)) {
                        val ct = (x + scrollX) / pxPerSec
                        val srcT = (ct - layer.optDouble("start")) * layer.optDouble("speed", 1.0)
                        val i = ((srcT / dur) * n).toInt()
                        if (i in 0 until n) {
                            val a = peaks.optDouble(i).toFloat() * (size.height / 2 - 8)
                            drawLine(Color(0xAAFFFFFF), Offset(x, mid - a), Offset(x, mid + a), 1f)
                        }
                        x += 2f
                    }
                }
                // Transitions.
                layer.optJSONObject("transitionIn")?.let { tr -> drawRect(Color(0x66FFFFFF), Offset(x0, 6f), Size((tr.optDouble("duration") * pxPerSec).toFloat(), 6f)) }
                layer.optJSONObject("transitionOut")?.let { tr -> val w = (tr.optDouble("duration") * pxPerSec).toFloat(); drawRect(Color(0x66FFFFFF), Offset(x1 - w, 6f), Size(w, 6f)) }
                // Indicators (shape-coded, not color only): effects ▲, masks ●, parent ◆ in the clip label.
                val paint = android.graphics.Paint().apply { color = android.graphics.Color.WHITE; textSize = 24f; isAntiAlias = true }
                val flags = buildString {
                    if ((layer.optJSONArray("effects")?.length() ?: 0) > 0) append("fx ")
                    if ((layer.optJSONArray("masks")?.length() ?: 0) > 0) append("◐ ")
                    if (!layer.isNull("parent") && layer.optString("parent").isNotEmpty()) append("⛓ ")
                    if (layer.optBoolean("threeD")) append("3D ")
                    if (layer.optDouble("speed", 1.0) != 1.0) append("${fmt(layer.optDouble("speed") * 100)}% ")
                    if (layer.optBoolean("reverse")) append("⟲ ")
                    if (layer.optJSONArray("behaviors")?.length() ?: 0 > 0) append("∿ ")
                }
                drawContext.canvas.nativeCanvas.drawText(flags + layer.optString("name"), maxOf(x0, 0f) + 10, size.height / 2 + 8, paint)
                // Keyframes.
                keyTimes.forEach { kt ->
                    val kx = (kt * pxPerSec - scrollX).toFloat()
                    val y = size.height - 12f
                    drawPath(Path().apply { moveTo(kx, y - 7); lineTo(kx + 7, y); lineTo(kx, y + 7); lineTo(kx - 7, y); close() }, KeyColor)
                }
                val px = (st.playhead * pxPerSec - scrollX).toFloat()
                drawLine(Accent, Offset(px, 0f), Offset(px, size.height), 2f)
            }
            DropdownMenu(menu, { menu = false; setLens(null) }) {
                val items = listOf(
                    "Split at playhead" to { st.op("split", "layers" to listOf(id), "t" to st.playhead) },
                    "Trim start to playhead" to { st.op("trimLayer", "layer" to id, "edge" to "in", "t" to st.playhead) },
                    "Trim end to playhead" to { st.op("trimLayer", "layer" to id, "edge" to "out", "t" to st.playhead) },
                    "Duplicate" to { st.op("duplicateLayers", "layers" to listOf(id)) },
                    "Move up" to { st.op("reorderLayer", "layer" to id, "index" to maxOf(0, st.layers.indexOf(layer) - 1)) },
                    "Move down" to { st.op("reorderLayer", "layer" to id, "index" to st.layers.indexOf(layer) + 1) },
                    "Ripple delete" to { st.op("rippleDelete", "layer" to id) },
                    "Delete" to { st.op("removeLayers", "layers" to listOf(id)) },
                    "Open inspector" to { st.selection = setOf(id); ui.tab = InspectorTab.Layer },
                )
                items.forEach { (t, f) -> DropdownMenuItem(text = { Text(t) }, onClick = { menu = false; setLens(null); f() }) }
            }
        }
    }
}

/** Magnified timeline strip shown while trimming / long-pressing: exact frame, nearby keyframes and edit points. */
@Composable
private fun PrecisionLens(st: EditorState, anchor: Offset, pxPerSec: Float, scrollX: Float, headerW: androidx.compose.ui.unit.Dp) {
    val t = ((anchor.x + scrollX) / pxPerSec).toDouble()
    val mag = 8f
    val keys = st.keyframeTimes(st.selectedLayer)
    val edits = st.layers.flatMap { listOf(it.optDouble("in"), it.optDouble("out")) }
    Box(Modifier.offset { IntOffset(0, 0) }.padding(start = headerW).fillMaxWidth().height(70.dp).background(Color(0xEE0B0C0F))) {
        Canvas(Modifier.fillMaxSize()) {
            val center = size.width / 2
            val fd = 1.0 / st.fps
            val f0 = st.frame(t)
            for (k in -12..12) {
                val ft = (f0 + k) * fd
                val x = center + ((ft - t) * pxPerSec * mag).toFloat()
                drawLine(if (k == 0) Accent else Color.Gray, Offset(x, if ((f0 + k) % Math.round(st.fps).toInt() == 0) 10f else 30f), Offset(x, size.height - 20f), if (k == 0) 3f else 1f)
            }
            (keys).forEach { kt -> val x = center + ((kt - t) * pxPerSec * mag).toFloat(); drawCircle(KeyColor, 7f, Offset(x, size.height - 12f)) }
            edits.forEach { et -> val x = center + ((et - t) * pxPerSec * mag).toFloat(); drawLine(Color.Cyan, Offset(x, 0f), Offset(x, size.height), 2f) }
        }
        Text("${st.timecode(t)}  ·  frame ${st.frame(t)}", modifier = Modifier.align(Alignment.TopCenter), fontFamily = FontFamily.Monospace, fontSize = 12.sp)
    }
}
