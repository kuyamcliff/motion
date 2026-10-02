package com.motionforge.app.media

import android.content.Context
import android.content.Intent
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMetadataRetriever
import android.net.Uri
import android.provider.OpenableColumns
import com.motionforge.app.engine.jo
import org.json.JSONObject
import java.io.File

/** Classifies, probes and (for small resource files) copies imported files. Media stays linked by URI. */
object Importer {
    enum class Kind { VIDEO, AUDIO, IMAGE, FONT, LUT, MODEL, SUBTITLE, PROJECT, SCRIPT, EXTENSION, PRESET, CAPSULE, TEMPLATE, UNKNOWN }

    fun displayName(ctx: Context, uri: Uri): String {
        if (uri.scheme == "file") return File(uri.path ?: "").name
        ctx.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) return c.getString(0) ?: "file"
        }
        return uri.lastPathSegment ?: "file"
    }

    fun classify(ctx: Context, uri: Uri): Kind {
        val name = displayName(ctx, uri).lowercase()
        val ext = name.substringAfterLast('.', "")
        when (ext) {
            "ttf", "otf", "ttc" -> return Kind.FONT
            "cube", "3dl" -> return Kind.LUT
            "glb", "gltf", "obj", "fbx" -> return Kind.MODEL
            "srt", "vtt", "ass", "ssa" -> return Kind.SUBTITLE
            "mforge" -> return Kind.PROJECT
            "mfsx" -> return Kind.SCRIPT
            "mfext" -> return Kind.EXTENSION
            "mffx" -> return Kind.PRESET
            "mfcapsule" -> return Kind.CAPSULE
            "mftemplate" -> return Kind.TEMPLATE
            "png", "jpg", "jpeg", "webp", "bmp", "heic", "heif", "gif" -> return Kind.IMAGE
            "mp3", "wav", "m4a", "aac", "ogg", "flac", "opus" -> return Kind.AUDIO
            "mp4", "mov", "mkv", "webm", "3gp", "m4v" -> return Kind.VIDEO
        }
        val mime = ctx.contentResolver.getType(uri) ?: ""
        return when {
            mime.startsWith("video/") -> Kind.VIDEO
            mime.startsWith("audio/") -> Kind.AUDIO
            mime.startsWith("image/") -> Kind.IMAGE
            else -> Kind.UNKNOWN
        }
    }

    fun persist(ctx: Context, uri: Uri) {
        if (uri.scheme != "content") return
        try {
            ctx.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (_: SecurityException) {
            // Provider does not offer persistable grants; the asset will be flagged missing if access is lost later.
        }
    }

    /** Copies a small resource file (font, LUT, model, subtitle, package) into app storage; returns the local path. */
    fun copyToLocal(ctx: Context, uri: Uri, subdir: String): File? {
        val dir = File(ctx.filesDir, subdir).apply { mkdirs() }
        val name = displayName(ctx, uri).replace(Regex("[^A-Za-z0-9._ -]"), "_")
        var out = File(dir, name)
        var n = 1
        while (out.exists()) out = File(dir, name.substringBeforeLast('.') + "_" + (n++) + "." + name.substringAfterLast('.', "bin"))
        return try {
            ctx.contentResolver.openInputStream(uri)?.use { input -> out.outputStream().use { input.copyTo(it) } }
            out
        } catch (e: Exception) {
            null
        }
    }

    /** Probes a media file and returns an engine asset description (metadata only; nothing is decoded fully). */
    fun probe(ctx: Context, uri: Uri, kind: Kind): JSONObject {
        val src = uri.toString()
        val asset = jo("type" to when (kind) { Kind.VIDEO -> "video"; Kind.AUDIO -> "audio"; else -> "image" }, "name" to displayName(ctx, uri), "uri" to src)
        asset.put("checksum", MediaBridge.quickChecksum(src))
        MediaBridge.openFd(src)?.use { asset.put("size", it.statSize) }
        if (kind == Kind.IMAGE) {
            val o = android.graphics.BitmapFactory.Options().apply { inJustDecodeBounds = true }
            MediaBridge.openStream(src)?.use { android.graphics.BitmapFactory.decodeStream(it, null, o) }
            if (o.outWidth <= 0) throw IllegalArgumentException("Unsupported or damaged image.")
            var w = o.outWidth
            var h = o.outHeight
            val rot = try { MediaBridge.openStream(src)?.use { android.media.ExifInterface(it).rotationDegrees } ?: 0 } catch (e: Exception) { 0 }
            if (rot == 90 || rot == 270) { val t = w; w = h; h = t }
            asset.put("width", w); asset.put("height", h)
            return asset
        }
        val mmr = MediaMetadataRetriever()
        try {
            MediaBridge.setDataSource(mmr, src)
            val dur = (mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toLongOrNull() ?: 0L) / 1000.0
            asset.put("duration", dur)
            val hasVideo = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_HAS_VIDEO) == "yes"
            val hasAudio = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_HAS_AUDIO) == "yes"
            asset.put("hasVideo", hasVideo); asset.put("hasAudio", hasAudio)
            if (kind == Kind.VIDEO && !hasVideo) throw IllegalArgumentException("The file has no video track.")
            if (kind == Kind.AUDIO && !hasAudio) throw IllegalArgumentException("The file has no audio track.")
            mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)?.toIntOrNull()?.let { asset.put("width", it) }
            mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)?.toIntOrNull()?.let { asset.put("height", it) }
            mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)?.toIntOrNull()?.let { asset.put("rotation", it) }
            mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_BITRATE)?.toIntOrNull()?.let { asset.put("bitrate", it) }
        } finally {
            mmr.release()
        }
        val ex = MediaExtractor()
        try {
            MediaBridge.setDataSource(ex, src)
            for (i in 0 until ex.trackCount) {
                val f = ex.getTrackFormat(i)
                val mime = f.getString(MediaFormat.KEY_MIME) ?: continue
                if (mime.startsWith("video/")) {
                    asset.put("codec", mime.removePrefix("video/"))
                    if (f.containsKey(MediaFormat.KEY_FRAME_RATE)) asset.put("fps", f.getInteger(MediaFormat.KEY_FRAME_RATE).toDouble())
                    if (f.containsKey(MediaFormat.KEY_COLOR_TRANSFER)) {
                        val tr = f.getInteger(MediaFormat.KEY_COLOR_TRANSFER)
                        asset.put("hdr", tr == MediaFormat.COLOR_TRANSFER_ST2084 || tr == MediaFormat.COLOR_TRANSFER_HLG)
                    }
                    if (f.containsKey(MediaFormat.KEY_COLOR_STANDARD)) asset.put("colorSpace", when (f.getInteger(MediaFormat.KEY_COLOR_STANDARD)) {
                        MediaFormat.COLOR_STANDARD_BT2020 -> "bt2020"; MediaFormat.COLOR_STANDARD_BT709 -> "bt709"; else -> "bt601"
                    })
                    // Variable frame rate detection from sample timestamps.
                    ex.selectTrack(i)
                    val deltas = ArrayList<Long>()
                    var last = -1L
                    var count = 0
                    while (count < 240 && ex.sampleTime >= 0) {
                        val t = ex.sampleTime
                        if (last >= 0) deltas.add(t - last)
                        last = t; count++
                        if (!ex.advance()) break
                    }
                    ex.unselectTrack(i)
                    if (deltas.size > 10) {
                        val sorted = deltas.sorted()
                        val med = sorted[sorted.size / 2].toDouble()
                        if (!asset.has("fps") && med > 0) asset.put("fps", Math.round(1_000_000.0 / med * 100) / 100.0)
                        val outliers = deltas.count { Math.abs(it - med) > med * 0.25 }
                        asset.put("vfr", outliers > deltas.size / 10)
                    }
                } else if (mime.startsWith("audio/")) {
                    asset.put("sampleRate", f.getInteger(MediaFormat.KEY_SAMPLE_RATE))
                    asset.put("channels", f.getInteger(MediaFormat.KEY_CHANNEL_COUNT))
                    asset.put("audioCodec", mime.removePrefix("audio/"))
                }
            }
        } catch (e: Exception) {
            if (kind == Kind.VIDEO) throw IllegalArgumentException("Unsupported or damaged video: ${e.message}")
        } finally {
            ex.release()
        }
        if (!asset.has("fps")) asset.put("fps", 30.0)
        return asset
    }
}
