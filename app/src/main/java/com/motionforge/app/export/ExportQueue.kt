package com.motionforge.app.export

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Environment
import android.os.IBinder
import android.provider.MediaStore
import androidx.compose.runtime.mutableStateListOf
import com.motionforge.app.MainActivity
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.jo
import com.motionforge.app.engine.objects
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.UUID
import java.util.concurrent.Executors

data class ExportJob(
    val id: String,
    val projectId: String,
    val name: String,
    val settings: ExportSettings,
    val outPath: String,
    var status: String = "waiting",  // waiting | running | done | failed | cancelled
    var progress: Float = 0f,
    var error: String = "",
    var suggestions: List<String> = emptyList(),
    var result: JSONObject = JSONObject(),
    var savedUri: String = "",
) {
    fun toJson() = jo("id" to id, "projectId" to projectId, "name" to name, "settings" to settings.toJson(), "outPath" to outPath, "status" to status,
        "progress" to progress.toDouble(), "error" to error, "suggestions" to suggestions, "result" to result, "savedUri" to savedUri)

    companion object {
        fun fromJson(j: JSONObject) = ExportJob(j.getString("id"), j.optString("projectId"), j.optString("name"), ExportSettings.fromJson(j.optJSONObject("settings") ?: JSONObject()),
            j.optString("outPath"), j.optString("status", "waiting"), j.optDouble("progress", 0.0).toFloat(), j.optString("error"),
            (0 until (j.optJSONArray("suggestions")?.length() ?: 0)).map { j.getJSONArray("suggestions").getString(it) }, j.optJSONObject("result") ?: JSONObject(), j.optString("savedUri"))
    }
}

/** Persistent export queue. Jobs run one at a time inside a foreground service; interrupted jobs can be retried. */
object ExportQueue {
    val jobs = mutableStateListOf<ExportJob>()
    private lateinit var ctx: Context
    private val worker = Executors.newSingleThreadExecutor { Thread(it, "mf-export") }
    @Volatile private var current: ExportEngine? = null
    var listener: ((ExportJob) -> Unit)? = null
    var openProjectId: () -> String = { "" }

    private fun file() = File(ctx.filesDir, "exports.json")

    fun init(context: Context) {
        ctx = context.applicationContext
        if (jobs.isNotEmpty()) return
        try {
            val arr = JSONArray(file().readText())
            arr.objects().forEach { j ->
                val job = ExportJob.fromJson(j)
                // A job that was running when the app died is not silently reported as finished.
                if (job.status == "running") { job.status = "failed"; job.error = "Interrupted (app closed or killed). Tap Retry to export again." }
                jobs.add(job)
            }
        } catch (_: Exception) {}
    }

    private fun persist() {
        val arr = JSONArray()
        jobs.forEach { arr.put(it.toJson()) }
        try { file().writeText(arr.toString()) } catch (_: Exception) {}
    }

    fun exportsDir(): File = File(ctx.getExternalFilesDir(null) ?: ctx.filesDir, "exports").apply { mkdirs() }

    fun enqueue(projectId: String, name: String, s: ExportSettings): ExportJob {
        val ext = when (s.format) { "gif" -> "gif"; "png" -> "png"; "wav" -> "wav"; "m4a" -> "m4a"; "webm" -> "webm"; else -> "mp4" }
        val base = name.replace(Regex("[^A-Za-z0-9_-]"), "_").ifEmpty { "export" }
        var out = File(exportsDir(), "$base.$ext")
        var n = 1
        while (out.exists() || jobs.any { it.outPath == out.absolutePath }) out = File(exportsDir(), "${base}_${n++}.$ext")
        if (s.format == "png") out = File(exportsDir(), out.nameWithoutExtension + "_frames")
        val job = ExportJob(UUID.randomUUID().toString(), projectId, name, s, out.absolutePath)
        jobs.add(job)
        persist()
        startService()
        return job
    }

    fun cancel(job: ExportJob) {
        if (job.status == "running") current?.cancelled = true
        else if (job.status == "waiting") update(job) { it.status = "cancelled" }
    }

    fun retry(job: ExportJob) { update(job) { it.status = "waiting"; it.progress = 0f; it.error = "" }; startService() }
    fun duplicate(job: ExportJob) = enqueue(job.projectId, job.name, job.settings)
    fun remove(job: ExportJob) { if (job.status != "running") { jobs.remove(job); persist() } }

    private fun update(job: ExportJob, f: (ExportJob) -> Unit) {
        val i = jobs.indexOfFirst { it.id == job.id }
        if (i < 0) return
        val j = jobs[i].copy()
        f(j)
        jobs[i] = j
        persist()
        listener?.invoke(j)
    }

    private fun startService() {
        val i = Intent(ctx, ExportService::class.java)
        if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(i) else ctx.startService(i)
    }

    /** Runs waiting jobs for the open project. Returns when the queue is idle. */
    fun runPending(onProgress: (ExportJob) -> Unit) {
        while (true) {
            val job = jobs.firstOrNull { it.status == "waiting" && it.projectId == openProjectId() } ?: return
            update(job) { it.status = "running" }
            val doc = NativeBridge.call("doc").getJSONObject("doc")
            val comps = doc.getJSONArray("comps").objects()
            val comp = comps.firstOrNull { it.optString("id") == doc.optJSONObject("settings")?.optString("activeComp") } ?: comps.first()
            val engine = ExportEngine(comp.optDouble("duration", 1.0), comp.optInt("width"), comp.optInt("height"))
            current = engine
            var lastReport = 0L
            try {
                val res = engine.run(job.settings, File(job.outPath)) { p ->
                    val now = System.currentTimeMillis()
                    if (now - lastReport > 250) {
                        lastReport = now
                        update(job) { it.progress = p }
                        onProgress(jobs.first { it.id == job.id })
                    }
                }
                val saved = publishToGallery(job)
                update(job) { it.status = "done"; it.progress = 1f; it.result = res; it.savedUri = saved }
            } catch (e: ExportException) {
                update(job) { it.status = if (e.message == "Export cancelled.") "cancelled" else "failed"; it.error = e.message ?: "Export failed"; it.suggestions = e.suggestions }
            } catch (e: Throwable) {
                update(job) { it.status = "failed"; it.error = "Export failed: ${e.message}" }
            } finally {
                current = null
            }
            onProgress(jobs.first { it.id == job.id })
        }
    }

    /** Copies finished videos/GIFs into the shared Movies/Pictures collection (no storage permission needed on API 29+). */
    private fun publishToGallery(job: ExportJob): String {
        val f = File(job.outPath)
        if (!f.isFile || Build.VERSION.SDK_INT < 29) return ""
        val (collection, mime, dir) = when (job.settings.format) {
            "gif" -> Triple(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, "image/gif", Environment.DIRECTORY_PICTURES)
            "wav", "m4a" -> Triple(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, if (job.settings.format == "wav") "audio/wav" else "audio/mp4", Environment.DIRECTORY_MUSIC)
            "webm" -> Triple(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, "video/webm", Environment.DIRECTORY_MOVIES)
            else -> Triple(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, "video/mp4", Environment.DIRECTORY_MOVIES)
        }
        return try {
            val values = ContentValues().apply {
                put(MediaStore.MediaColumns.DISPLAY_NAME, f.name)
                put(MediaStore.MediaColumns.MIME_TYPE, mime)
                put(MediaStore.MediaColumns.RELATIVE_PATH, "$dir/MotionForge")
                put(MediaStore.MediaColumns.IS_PENDING, 1)
            }
            val uri = ctx.contentResolver.insert(collection, values) ?: return ""
            ctx.contentResolver.openOutputStream(uri)?.use { os -> f.inputStream().use { it.copyTo(os) } }
            values.clear()
            values.put(MediaStore.MediaColumns.IS_PENDING, 0)
            ctx.contentResolver.update(uri, values, null, null)
            uri.toString()
        } catch (e: Exception) {
            ""
        }
    }
}

class ExportService : Service() {
    private val exec = Executors.newSingleThreadExecutor()

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= 26) nm.createNotificationChannel(NotificationChannel("export", "Exports", NotificationManager.IMPORTANCE_LOW))
        val notif = buildNotification("Preparing export…", 0)
        if (Build.VERSION.SDK_INT >= 29) startForeground(1, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC) else startForeground(1, notif)
        exec.execute {
            ExportQueue.runPending { job ->
                val text = when (job.status) {
                    "running" -> "Exporting ${job.name} — ${(job.progress * 100).toInt()}%"
                    "done" -> "Exported ${job.name}"
                    else -> "${job.name}: ${job.status}"
                }
                nm.notify(1, buildNotification(text, (job.progress * 100).toInt()))
            }
            stopForeground(STOP_FOREGROUND_DETACH)
            stopSelf()
        }
        return START_NOT_STICKY
    }

    private fun buildNotification(text: String, progress: Int): Notification {
        val pi = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java).putExtra("openExports", true),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val b = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(this, "export") else @Suppress("DEPRECATION") Notification.Builder(this)
        return b.setContentTitle("MOTIONFORGE").setContentText(text).setSmallIcon(android.R.drawable.stat_sys_upload)
            .setProgress(100, progress, progress == 0).setOngoing(progress in 1..99).setContentIntent(pi).build()
    }
}
