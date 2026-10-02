package com.motionforge.app

import android.app.Application
import android.content.ComponentCallbacks2
import android.content.Context
import android.content.SharedPreferences
import com.motionforge.app.engine.NativeBridge
import com.motionforge.app.engine.jo
import com.motionforge.app.media.MediaBridge
import java.io.File

class MfApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        instance = this
        MediaBridge.init(this)
        Settings.init(this)
        installBundledAssets(this)
        val cfg = jo(
            "dataDir" to File(filesDir, "mf").absolutePath,
            "cacheDir" to File(cacheDir, "mf").absolutePath,
            "fontsDir" to File(filesDir, "fonts").absolutePath,
            "modelsDir" to File(filesDir, "models").absolutePath,
            "bundledFontsDir" to File(filesDir, "bundled_fonts").absolutePath,
            "threads" to (Runtime.getRuntime().availableProcessors() - 1).coerceIn(1, 8),  // keep a core free for the UI thread
            "autosaveInterval" to Settings.autosaveSeconds.toDouble(),
            "developer" to Settings.developerMode,
        )
        NativeBridge.nativeInit(cfg.toString())
    }

    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)
        if (level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW) {
            // Release decoded frame caches instead of crashing from cache greed.
            NativeBridge.call("memoryPressure")
            MediaBridge.releaseAll()
        }
    }

    companion object {
        lateinit var instance: MfApplication
            private set

        /** Copies fonts, the offline speech model and the sample clip out of the APK once. */
        fun installBundledAssets(ctx: Context) {
            val am = ctx.assets
            fun copyDir(assetDir: String, outDir: File) {
                outDir.mkdirs()
                for (name in am.list(assetDir) ?: emptyArray()) {
                    val out = File(outDir, name)
                    if (out.exists() && out.length() > 0) continue
                    val tmp = File(outDir, "$name.tmp")
                    am.open("$assetDir/$name").use { input -> tmp.outputStream().use { input.copyTo(it) } }
                    tmp.renameTo(out)
                }
            }
            copyDir("fonts", File(ctx.filesDir, "bundled_fonts"))
            copyDir("models", File(ctx.filesDir, "models"))
            copyDir("samples", File(ctx.filesDir, "samples"))
        }
    }
}

/** User preferences (local only). */
object Settings {
    private lateinit var prefs: SharedPreferences
    fun init(ctx: Context) {
        prefs = ctx.getSharedPreferences("motionforge", Context.MODE_PRIVATE)
    }

    var firstRunDone: Boolean
        get() = prefs.getBoolean("firstRun", false)
        set(v) = prefs.edit().putBoolean("firstRun", v).apply()
    var editingStyle: String
        get() = prefs.getString("style", "quick") ?: "quick"
        set(v) = prefs.edit().putString("style", v).apply()
    var autosaveSeconds: Int
        get() = prefs.getInt("autosave", 15)
        set(v) = prefs.edit().putInt("autosave", v).apply()
    var developerMode: Boolean
        get() = prefs.getBoolean("dev", false)
        set(v) = prefs.edit().putBoolean("dev", v).apply()
    var haptics: Boolean
        get() = prefs.getBoolean("haptics", true)
        set(v) = prefs.edit().putBoolean("haptics", v).apply()
    var highContrast: Boolean
        get() = prefs.getBoolean("contrast", false)
        set(v) = prefs.edit().putBoolean("contrast", v).apply()
    var reducedMotion: Boolean
        get() = prefs.getBoolean("reducedMotion", false)
        set(v) = prefs.edit().putBoolean("reducedMotion", v).apply()
    var uiScale: Float
        get() = prefs.getFloat("uiScale", 1f)
        set(v) = prefs.edit().putFloat("uiScale", v).apply()
    var previewQuality: String
        get() = prefs.getString("previewQuality", "auto") ?: "auto"
        set(v) = prefs.edit().putString("previewQuality", v).apply()
    var snapping: Boolean
        get() = prefs.getBoolean("snapping", true)
        set(v) = prefs.edit().putBoolean("snapping", v).apply()
    var defaultStillDuration: Float
        get() = prefs.getFloat("stillDur", 5f)
        set(v) = prefs.edit().putFloat("stillDur", v).apply()
    var defaultCodec: String
        get() = prefs.getString("codec", "video/avc") ?: "video/avc"
        set(v) = prefs.edit().putString("codec", v).apply()
    var defaultResolution: Int
        get() = prefs.getInt("res", 1080)
        set(v) = prefs.edit().putInt("res", v).apply()
    var asrLanguage: String
        get() = prefs.getString("asrLang", "auto") ?: "auto"
        set(v) = prefs.edit().putString("asrLang", v).apply()
    var asrModel: String
        get() = prefs.getString("asrModel", "") ?: ""
        set(v) = prefs.edit().putString("asrModel", v).apply()
    var trustedKeys: Set<String>
        get() = prefs.getStringSet("trusted", emptySet()) ?: emptySet()
        set(v) = prefs.edit().putStringSet("trusted", v).apply()
    var signingSecret: String
        get() = prefs.getString("signSk", "") ?: ""
        set(v) = prefs.edit().putString("signSk", v).apply()
    var signingPublic: String
        get() = prefs.getString("signPk", "") ?: ""
        set(v) = prefs.edit().putString("signPk", v).apply()
    var recentCommands: List<String>
        get() = (prefs.getString("recentCmds", "") ?: "").split('\n').filter { it.isNotEmpty() }
        set(v) = prefs.edit().putString("recentCmds", v.take(20).joinToString("\n")).apply()
    var scriptStorage: String
        get() = prefs.getString("scriptStorage", "{}") ?: "{}"
        set(v) = prefs.edit().putString("scriptStorage", v).apply()
    var favoriteFonts: Set<String>
        get() = prefs.getStringSet("favFonts", emptySet()) ?: emptySet()
        set(v) = prefs.edit().putStringSet("favFonts", v).apply()
    fun fontLicenseNote(name: String): String = prefs.getString("fontLic:$name", "") ?: ""
    fun setFontLicenseNote(name: String, note: String) = prefs.edit().putString("fontLic:$name", note).apply()
}
