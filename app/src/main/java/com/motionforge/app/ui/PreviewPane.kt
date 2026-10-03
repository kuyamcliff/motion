package com.motionforge.app.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateRotation
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChanged
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.semantics.semantics
import kotlin.math.roundToInt
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.motionforge.app.AppState
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.ProgressCallback
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.objects
import com.motionforge.app.engine.propValue
import com.motionforge.app.engine.resolve
import com.motionforge.app.toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import kotlin.math.atan2
import kotlin.math.hypot

@Composable
fun PreviewPane(app: AppState, ui: EditorUi, modifier: Modifier) {
    val st = app.editor
    val player = app.player ?: return
    var viewSize by remember { mutableStateOf(IntSize.Zero) }
    var zoom by remember { mutableFloatStateOf(1f) }
    var pan by remember { mutableStateOf(Offset.Zero) }
    val drawPts = remember { mutableStateListOf<Offset>() }  // comp space
    var dragRect by remember { mutableStateOf<Rect?>(null) }   // comp space (mask mode)
    val penPts = remember { mutableStateListOf<Offset>() }
    var tracking by remember { mutableStateOf<Float?>(null) }
    val scope = remember { CoroutineScope(Dispatchers.Main) }

    // Image rectangle in view coordinates (fit, then zoom/pan).
    fun imageRect(): Rect {
        val cw = st.compWidth.toFloat()
        val ch = st.compHeight.toFloat()
        if (viewSize.width == 0) return Rect.Zero
        val fit = minOf(viewSize.width / cw, viewSize.height / ch) * zoom
        val w = cw * fit
        val h = ch * fit
        val left = (viewSize.width - w) / 2 + pan.x
        val top = (viewSize.height - h) / 2 + pan.y
        return Rect(left, top, left + w, top + h)
    }
    fun toComp(p: Offset): Offset {
        val r = imageRect()
        return Offset((p.x - r.left) / r.width * st.compWidth, (p.y - r.top) / r.height * st.compHeight)
    }
    fun toView(p: Offset): Offset {
        val r = imageRect()
        return Offset(r.left + p.x / st.compWidth * r.width, r.top + p.y / st.compHeight * r.height)
    }

    val sel = st.selectedLayer
    val quad = remember(st.revision, st.playhead, sel?.optString("id")) {
        sel?.let { NativeBridge.call("layerQuad", jo("layer" to it.optString("id"), "t" to st.playhead)).optJSONArray("quad") }
    }

    Box(modifier.background(Color.Black).onSizeChanged { viewSize = it; player.setViewportWidth(it.width) }
        .semantics { contentDescription = "Preview canvas"; stateDescription = "Zoom ${(zoom * 100).roundToInt()}%" }
        .pointerInput(ui.previewMode, sel?.optString("id")) {
            detectTapGestures(
                onDoubleTap = { zoom = 1f; pan = Offset.Zero },
                onTap = { p ->
                    val c = toComp(p)
                    when (ui.previewMode) {
                        "pen" -> penPts.add(c)
                        "track" -> {
                            val layer = st.selectedLayer
                            if (layer == null || layer.optString("type") != "video") { app.toast("Select a video layer, then tap the feature to track.", true); return@detectTapGestures }
                            tracking = 0f
                            scope.launch {
                                val r = withContext(Dispatchers.Default) {
                                    NativeBridge.task("track", jo("layer" to layer.optString("id"), "x" to c.x.toDouble(), "y" to c.y.toDouble(), "t0" to st.playhead),
                                        ProgressCallback { pr -> tracking = pr; tracking != null })
                                }
                                tracking = null
                                if (!r.optBoolean("ok")) app.toast(r.optString("error"), true)
                                else {
                                    // Create Null from Track: a null whose position follows the feature.
                                    val nid = st.addLayer("null", jo("name" to "Track Null", "at" to layer.optDouble("in")))
                                    if (nid != null) st.op("setTrackKeyframes", "layer" to nid, "path" to "transform.position", "samples" to r.optJSONArray("samples"))
                                    app.toast("Tracked ${r.optJSONArray("samples")?.length() ?: 0} frames. " + r.optString("message") + " Parent layers to 'Track Null' to follow it.")
                                }
                                ui.previewMode = "select"
                            }
                        }
                        else -> {
                            val hit = NativeBridge.call("hitTest", jo("t" to st.playhead, "x" to c.x.toDouble(), "y" to c.y.toDouble())).optString("layer")
                            st.selection = if (hit.isEmpty()) emptySet() else setOf(hit)
                        }
                    }
                })
        }
        .pointerInput(ui.previewMode, sel?.optString("id"), st.revision) {
            awaitEachGesture {
                val down = awaitFirstDown(requireUnconsumed = false)
                val startComp = toComp(down.position)
                val layer = st.selectedLayer
                val lid = layer?.optString("id")
                val pos0 = (layer?.resolve("transform.position") as? org.json.JSONObject)?.let { st.evalProps(lid!!, listOf("transform.position")).optJSONArray("transform.position") }
                val scale0 = lid?.let { st.evalProps(it, listOf("transform.scale")).optJSONArray("transform.scale") }
                val rot0 = lid?.let { st.evalProps(it, listOf("transform.rotation")).optDouble("transform.rotation", 0.0) } ?: 0.0
                // Which handle (if any) was grabbed.
                var handle = "none"
                if (ui.previewMode == "select" && quad != null && quad.length() == 4) {
                    val corners = (0 until 4).map { toView(Offset(quad.getJSONArray(it).getDouble(0).toFloat(), quad.getJSONArray(it).getDouble(1).toFloat())) }
                    val center = corners.reduce { a, b -> a + b } / 4f
                    val rotHandle = (corners[0] + corners[1]) / 2f + (((corners[0] + corners[1]) / 2f) - center).let { it / (it.getDistance().coerceAtLeast(1f)) * 40f }
                    handle = when {
                        (down.position - rotHandle).getDistance() < 36f -> "rotate"
                        corners.any { (down.position - it).getDistance() < 36f } -> "scale"
                        inside(down.position, corners) -> "move"
                        else -> "none"
                    }
                }
                var moved = false
                var multi = false
                do {
                    val event = awaitPointerEvent()
                    val pressed = event.changes.filter { it.pressed }
                    if (pressed.size >= 2) {
                        multi = true
                        // Two fingers: pinch-zoom / pan the view, or rotate the selected layer if the gesture is mostly rotational.
                        val z = event.calculateZoom()
                        val p = event.calculatePan()
                        val r = event.calculateRotation()
                        if (lid != null && Math.abs(r) > 2f && handle == "move") {
                            st.previewProp(lid, "transform.rotation", rot0 + r)
                        } else {
                            zoom = (zoom * z).coerceIn(0.25f, 8f)
                            pan += p
                        }
                        event.changes.forEach { if (it.positionChanged()) it.consume() }
                        continue
                    }
                    val ch = event.changes.firstOrNull() ?: break
                    if (!ch.pressed) break
                    val c = toComp(ch.position)
                    if ((ch.position - down.position).getDistance() > 6f) moved = true
                    if (!moved || multi) continue
                    when (ui.previewMode) {
                        "select" -> if (lid != null && !layer.optBoolean("locked")) when (handle) {
                            "move" -> if (pos0 != null) {
                                val d = c - startComp
                                val fine = if (event.changes.size > 0 && ch.uptimeMillis - down.uptimeMillis > 1500) 0.25f else 1f
                                val nv = JSONArray(listOf(pos0.getDouble(0) + d.x * fine, pos0.getDouble(1) + d.y * fine, pos0.optDouble(2, 0.0)))
                                st.previewProp(lid, "transform.position", nv)
                            }
                            "scale" -> if (scale0 != null && quad != null) {
                                val center = (0 until 4).map { Offset(quad.getJSONArray(it).getDouble(0).toFloat(), quad.getJSONArray(it).getDouble(1).toFloat()) }.reduce { a, b -> a + b } / 4f
                                val k = (c - center).getDistance() / (startComp - center).getDistance().coerceAtLeast(1f)
                                st.previewProp(lid, "transform.scale", JSONArray(listOf(scale0.getDouble(0) * k, scale0.getDouble(1) * k, scale0.optDouble(2, 100.0))))
                            }
                            "rotate" -> if (quad != null) {
                                val center = (0 until 4).map { Offset(quad.getJSONArray(it).getDouble(0).toFloat(), quad.getJSONArray(it).getDouble(1).toFloat()) }.reduce { a, b -> a + b } / 4f
                                val a0 = atan2((startComp - center).y, (startComp - center).x)
                                val a1 = atan2((c - center).y, (c - center).x)
                                st.previewProp(lid, "transform.rotation", rot0 + Math.toDegrees((a1 - a0).toDouble()))
                            }
                            else -> pan += ch.position - ch.previousPosition
                        } else pan += ch.position - ch.previousPosition
                        "mask" -> dragRect = Rect(startComp, c).let { Rect(minOf(it.left, it.right), minOf(it.top, it.bottom), maxOf(it.left, it.right), maxOf(it.top, it.bottom)) }
                        "draw" -> drawPts.add(c)
                    }
                    ch.consume()
                } while (true)
                if (moved && !multi) when (ui.previewMode) {
                    "select" -> if (lid != null && handle != "none") st.commitPreview(when (handle) { "move" -> "Move Layer"; "scale" -> "Scale Layer"; else -> "Rotate Layer" })
                    "mask" -> dragRect?.let { r ->
                        if (lid != null && r.width > 4 && r.height > 4) {
                            val pts = NativeBridge.call("compToLayer", jo("layer" to lid, "t" to st.playhead, "points" to listOf(listOf(r.left, r.top), listOf(r.right, r.top), listOf(r.right, r.bottom), listOf(r.left, r.bottom))))
                                .optJSONArray("points")
                            if (pts != null) {
                                val v = JSONArray()
                                for (i in 0 until 4) v.put(JSONArray(listOf(pts.getJSONArray(i).getDouble(0), pts.getJSONArray(i).getDouble(1), 0, 0, 0, 0)))
                                st.op("addMask", "layer" to lid, "path" to jo("closed" to true, "v" to v))
                            }
                        } else if (lid == null) app.toast("Select a layer to mask first.", true)
                        dragRect = null
                    }
                    "draw" -> {
                        if (drawPts.size > 2) createDrawing(app, drawPts.toList())
                        drawPts.clear()
                    }
                }
                else if (multi && lid != null) st.commitPreview("Rotate Layer")
            }
        }) {
        val bmp = player.frame
        val r = imageRect()
        if (ui.checkerboard) Canvas(Modifier.fillMaxSize()) {
            val s = 16f
            var y = r.top
            var row = 0
            while (y < r.bottom) {
                var x = r.left
                var col = row % 2
                while (x < r.right) { if (col % 2 == 0) drawRect(Color(0xFF3A3A3A), Offset(x, y), Size(minOf(s, r.right - x), minOf(s, r.bottom - y))); x += s; col++ }
                y += s; row++
            }
        }
        if (bmp != null) {
            val img = remember(bmp, player.frame) { bmp.asImageBitmap() }
            Canvas(Modifier.fillMaxSize()) {
                drawImage(img, srcOffset = IntOffset.Zero, srcSize = IntSize(img.width, img.height), dstOffset = IntOffset(r.left.toInt(), r.top.toInt()), dstSize = IntSize(r.width.toInt(), r.height.toInt()))
            }
        }
        Canvas(Modifier.fillMaxSize()) {
            if (ui.showSafe) {
                listOf(0.9f to Color(0x88FFFFFF), 0.8f to Color(0x88FFC23D)).forEach { (k, c) ->
                    val w = r.width * k; val h = r.height * k
                    drawRect(c, Offset(r.center.x - w / 2, r.center.y - h / 2), Size(w, h), style = Stroke(1.5f))
                }
                drawLine(Color(0x55FFFFFF), Offset(r.center.x, r.top), Offset(r.center.x, r.bottom))
                drawLine(Color(0x55FFFFFF), Offset(r.left, r.center.y), Offset(r.right, r.center.y))
            }
            if (ui.showGrid) for (i in 1..2) {
                drawLine(Color(0x66FFFFFF), Offset(r.left + r.width * i / 3, r.top), Offset(r.left + r.width * i / 3, r.bottom))
                drawLine(Color(0x66FFFFFF), Offset(r.left, r.top + r.height * i / 3), Offset(r.right, r.top + r.height * i / 3))
            }
            if (ui.showRulers) {
                val step = 100
                for (x in 0..st.compWidth step step) { val vx = toView(Offset(x.toFloat(), 0f)).x; drawLine(Color.Gray, Offset(vx, r.top), Offset(vx, r.top + if (x % 500 == 0) 14f else 7f)) }
                for (y in 0..st.compHeight step step) { val vy = toView(Offset(0f, y.toFloat())).y; drawLine(Color.Gray, Offset(r.left, vy), Offset(r.left + if (y % 500 == 0) 14f else 7f, vy)) }
            }
            st.comp.optJSONArray("guides")?.objects()?.forEach { g ->
                val pos = g.optDouble("pos").toFloat()
                if (g.optString("axis") == "x") toView(Offset(pos, 0f)).x.let { drawLine(Color.Cyan, Offset(it, r.top), Offset(it, r.bottom)) }
                else toView(Offset(0f, pos)).y.let { drawLine(Color.Cyan, Offset(r.left, it), Offset(r.right, it)) }
            }
            if (quad != null && quad.length() == 4 && ui.previewMode == "select") {
                val pts = (0 until 4).map { toView(Offset(quad.getJSONArray(it).getDouble(0).toFloat(), quad.getJSONArray(it).getDouble(1).toFloat())) }
                val path = Path().apply { moveTo(pts[0].x, pts[0].y); pts.drop(1).forEach { lineTo(it.x, it.y) }; close() }
                drawPath(path, Accent, style = Stroke(2f))
                pts.forEach { drawRect(Color.White, it - Offset(7f, 7f), Size(14f, 14f)) }
                val center = pts.reduce { a, b -> a + b } / 4f
                val top = (pts[0] + pts[1]) / 2f
                val rot = top + (top - center).let { it / it.getDistance().coerceAtLeast(1f) * 40f }
                drawLine(Accent, top, rot, 2f)
                drawCircle(Accent, 9f, rot)
            }
            dragRect?.let { d -> val a = toView(d.topLeft); val b = toView(d.bottomRight); drawRect(Color.Yellow, a, Size(b.x - a.x, b.y - a.y), style = Stroke(2f, pathEffect = PathEffect.dashPathEffect(floatArrayOf(10f, 6f)))) }
            if (drawPts.size > 1) {
                val path = Path().apply { val p0 = toView(drawPts[0]); moveTo(p0.x, p0.y); drawPts.drop(1).forEach { val v = toView(it); lineTo(v.x, v.y) } }
                drawPath(path, Color.White, style = Stroke(4f))
            }
            penPts.forEachIndexed { i, p -> val v = toView(p); drawCircle(Color.Yellow, 7f, v); if (i > 0) drawLine(Color.Yellow, toView(penPts[i - 1]), v, 2f) }
        }
        // Mode bar & status.
        Row(Modifier.align(Alignment.TopStart).horizontalScroll(rememberScrollState()).padding(2.dp)) {
            listOf("select" to "Select", "mask" to "Mask", "draw" to "Draw", "pen" to "Pen", "track" to "Track").forEach { (k, n) -> Chip(n, ui.previewMode == k) { ui.previewMode = k; penPts.clear() } }
            if (ui.previewMode == "pen" && penPts.size >= 3) {
                Chip("Make shape") { createPenShape(app, penPts.toList(), asMask = false); penPts.clear() }
                Chip("Make mask") { createPenShape(app, penPts.toList(), asMask = true); penPts.clear() }
            }
            if (zoom != 1f) Chip("Fit (${(zoom * 100).toInt()}%)") { zoom = 1f; pan = Offset.Zero }
            Chip("100%") { zoom = st.compWidth.toFloat() / (viewSize.width.coerceAtLeast(1)); pan = Offset.Zero }
        }
        if (ui.showDiagnostics) {
            val s = player.lastStats
            Column(Modifier.align(Alignment.BottomStart).background(Color(0xAA000000)).padding(6.dp)) {
                Text("render %.1f ms · layers %d · passes %d".format(player.renderMs, s.optInt("layers"), s.optInt("passes")), fontSize = 10.sp)
                Text("cache hit/miss %d/%d · dropped %d · fps %.1f".format(s.optInt("cacheHits"), s.optInt("cacheMisses"), player.droppedFrames, player.fpsActual), fontSize = 10.sp)
                Text("mem %d MB used".format((Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory()) / 1048576), fontSize = 10.sp)
            }
        }
        tracking?.let {
            Row(Modifier.align(Alignment.Center).background(Color(0xCC000000)).padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("Tracking… ${(it * 100).toInt()}%  ")
                Chip("Cancel") { tracking = null }
            }
        }
    }
}

private fun inside(p: Offset, poly: List<Offset>): Boolean {
    var c = false
    var j = poly.size - 1
    for (i in poly.indices) {
        if ((poly[i].y > p.y) != (poly[j].y > p.y) && p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x) c = !c
        j = i
    }
    return c
}

/** Freehand drawing becomes an editable vector shape layer with an optional Live Draw (animated trim) reveal. */
fun createDrawing(app: AppState, pts: List<Offset>) {
    val st = app.editor
    // Simplify: keep points at least 3 px apart.
    val simp = ArrayList<Offset>()
    for (p in pts) if (simp.isEmpty() || (p - simp.last()).getDistance() > 3f) simp.add(p)
    val cx = st.compWidth / 2.0
    val cy = st.compHeight / 2.0
    val v = JSONArray()
    simp.forEach { v.put(JSONArray(listOf(it.x - cx, it.y - cy, 0, 0, 0, 0))) }
    val id = st.addLayer("shape", jo("shape" to "path", "path" to jo("closed" to false, "v" to v), "name" to "Drawing")) ?: return
    st.op("setLayer", "layer" to id, "fields" to jo("name" to "Drawing"))
    app.editor.message = com.motionforge.app.engine.UiMessage("Drawing added as a vector shape.", action = "Live Draw") {
        val l = st.layer(id) ?: return@UiMessage
        st.apply(jo("op" to "batch", "label" to "Live Draw", "ops" to listOf(
            jo("op" to "setProp", "layer" to id, "path" to "shape.trim.enabled", "value" to true),
            jo("op" to "addKeyframe", "layer" to id, "path" to "shape.trim.end", "t" to l.optDouble("in"), "value" to 0.0),
            jo("op" to "addKeyframe", "layer" to id, "path" to "shape.trim.end", "t" to l.optDouble("in") + 1.5, "value" to 100.0),
        )))
    }
}

fun createPenShape(app: AppState, pts: List<Offset>, asMask: Boolean) {
    val st = app.editor
    if (asMask) {
        val lid = st.selectedLayer?.optString("id") ?: return app.toast("Select a layer to mask first.", true)
        val r = NativeBridge.call("compToLayer", jo("layer" to lid, "t" to st.playhead, "points" to pts.map { listOf(it.x.toDouble(), it.y.toDouble()) })).optJSONArray("points") ?: return
        val v = JSONArray()
        for (i in 0 until r.length()) v.put(JSONArray(listOf(r.getJSONArray(i).getDouble(0), r.getJSONArray(i).getDouble(1), 0, 0, 0, 0)))
        st.op("addMask", "layer" to lid, "path" to jo("closed" to true, "v" to v))
    } else {
        val cx = st.compWidth / 2.0
        val cy = st.compHeight / 2.0
        val v = JSONArray()
        pts.forEach { v.put(JSONArray(listOf(it.x - cx, it.y - cy, 0, 0, 0, 0))) }
        st.addLayer("shape", jo("shape" to "path", "path" to jo("closed" to true, "v" to v)))
    }
}
