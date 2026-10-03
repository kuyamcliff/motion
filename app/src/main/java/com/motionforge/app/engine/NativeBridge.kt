package com.motionforge.app.engine

import android.graphics.Bitmap
import org.json.JSONArray
import org.json.JSONObject
import java.nio.ByteBuffer

/** Progress callback for long native tasks. Return false to cancel. */
fun interface ProgressCallback {
    fun onProgress(p: Float): Boolean
}

/** Thin JNI surface over the C++ engine. Most calls are JSON in / JSON out through [call]. */
object NativeBridge {
    init {
        System.loadLibrary("motionforge")
    }

    @JvmStatic external fun nativeInit(config: String): String
    @JvmStatic external fun nativeCall(method: String, args: String): String
    @JvmStatic external fun nativeRenderBitmap(bitmap: Bitmap, t: Double, draft: Boolean, exportMode: Boolean): String
    // GPU preview compositor (call on the GL thread with its EGL context current; see playback/GpuRenderer.kt).
    @JvmStatic external fun nativeGpuInit(): Boolean
    @JvmStatic external fun nativeGpuRelease()
    @JvmStatic external fun nativeGpuPlan(t: Double, outW: Int, exportMode: Boolean, useProxies: Boolean): String
    @JvmStatic external fun nativeGpuComposite(videoTex: IntArray?, videoMtx: FloatArray?, viewW: Int, viewH: Int, dx: Float, dy: Float, dw: Float, dh: Float,
                                               checker: Boolean, clear: FloatArray?): String
    @JvmStatic external fun nativeGpuReadback(bitmap: Bitmap): Boolean
    @JvmStatic external fun nativeRenderRgba(buffer: ByteBuffer, w: Int, h: Int, t: Double): Boolean
    @JvmStatic external fun nativeMixAudio(t0: Double, frames: Int, out: FloatArray, sampleRate: Int): Float
    @JvmStatic external fun nativeResetAudio()
    @JvmStatic external fun nativeYuvToBitmap(
        y: ByteBuffer, u: ByteBuffer, v: ByteBuffer, yStride: Int, uvStride: Int, uvPixelStride: Int,
        w: Int, h: Int, bt709: Boolean, fullRange: Boolean, bitmap: Bitmap,
    ): Boolean
    @JvmStatic external fun nativeLongTask(method: String, args: String, cb: ProgressCallback?): String

    fun call(method: String, args: JSONObject = JSONObject()): JSONObject =
        JSONObject(nativeCall(method, args.toString()))

    fun task(method: String, args: JSONObject, cb: ProgressCallback?): JSONObject =
        JSONObject(nativeLongTask(method, args.toString(), cb))
}

// ---------------------------------------------------------------- small JSON helpers
fun jo(vararg pairs: Pair<String, Any?>): JSONObject = JSONObject().apply {
    for ((k, v) in pairs) put(k, wrap(v))
}

fun ja(vararg values: Any?): JSONArray = JSONArray().apply { values.forEach { put(wrap(it)) } }

private fun wrap(v: Any?): Any? = when (v) {
    null -> JSONObject.NULL
    is List<*> -> JSONArray().apply { v.forEach { put(wrap(it)) } }
    is DoubleArray -> JSONArray().apply { v.forEach { put(it) } }
    is FloatArray -> JSONArray().apply { v.forEach { put(it.toDouble()) } }
    is IntArray -> JSONArray().apply { v.forEach { put(it) } }
    is Map<*, *> -> JSONObject().apply { v.forEach { (k, x) -> put(k.toString(), wrap(x)) } }
    else -> v
}

fun JSONArray.objects(): List<JSONObject> = (0 until length()).mapNotNull { optJSONObject(it) }
fun JSONArray.strings(): List<String> = (0 until length()).map { optString(it) }
fun JSONArray.doubles(): List<Double> = (0 until length()).map { optDouble(it) }
fun JSONObject.arr(key: String): JSONArray = optJSONArray(key) ?: JSONArray()
fun JSONObject.obj(key: String): JSONObject = optJSONObject(key) ?: JSONObject()

/** Static value of an animatable property (first keyframe if animated). */
fun JSONObject.propValue(): Any? {
    if (has("k")) {
        val k = optJSONArray("k")
        if (k != null && k.length() > 0) return k.getJSONObject(0).opt("v")
    }
    return opt("v")
}

fun JSONObject.isAnimated(): Boolean = (optJSONArray("k")?.length() ?: 0) > 0

/** Resolve "transform.position" / "effects.E3.params.radius" inside a layer object (mirrors the engine). */
fun JSONObject.resolve(path: String): Any? {
    var cur: Any? = this
    for (seg in path.split('.', '/').filter { it.isNotEmpty() }) {
        cur = when (cur) {
            is JSONObject -> cur.opt(seg)
            is JSONArray -> seg.toIntOrNull()?.let { cur.opt(it) } ?: cur.objects().firstOrNull { it.optString("id") == seg }
            else -> return null
        }
    }
    return cur
}
