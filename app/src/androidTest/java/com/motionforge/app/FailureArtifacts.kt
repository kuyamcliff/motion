package com.motionforge.app

import android.content.ContentValues
import android.os.Build
import android.provider.MediaStore
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.rules.TestWatcher
import org.junit.runner.Description
import java.io.File

/** Saves diagnostic files where scripts/android/run-tests.sh can collect them. */
object TestArtifacts {
    /**
     * Writes to <app files>/test-failures (pulled with run-as on debuggable builds) and, on API 29+, to the
     * shared Download/mf-test-failures collection (pulled with adb on any build, including non-debuggable uitest).
     */
    fun save(name: String, bytes: ByteArray) {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        try { File(ctx.filesDir, "test-failures").apply { mkdirs() }.resolve(name).writeBytes(bytes) } catch (_: Throwable) {}
        if (Build.VERSION.SDK_INT >= 29) try {
            val cv = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, name)
                put(MediaStore.Downloads.RELATIVE_PATH, "Download/mf-test-failures")
            }
            ctx.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, cv)?.let { uri ->
                ctx.contentResolver.openOutputStream(uri)?.use { it.write(bytes) }
            }
        } catch (_: Throwable) {}
    }

    fun savePng(name: String, bmp: android.graphics.Bitmap) {
        val out = java.io.ByteArrayOutputStream()
        bmp.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, out)
        save(name, out.toByteArray())
    }
}

/** On any test failure, saves a screenshot and the accessibility window dump via [TestArtifacts]. */
class FailureArtifacts(private val closeUi: () -> Unit = {}) : TestWatcher() {
    // JUnit runs @After before a rule's failed(), so a test that closes its activity in @After would leave only the
    // launcher on screen. Tests pass their activity teardown here instead; it runs after the capture.
    override fun finished(d: Description) { closeUi() }

    override fun failed(e: Throwable?, d: Description) {
        try {
            val inst = InstrumentationRegistry.getInstrumentation()
            val base = "${d.testClass.simpleName}.${d.methodName}"
            inst.uiAutomation.takeScreenshot()?.let { TestArtifacts.savePng("$base.png", it) }
            val root = inst.uiAutomation.rootInActiveWindow
            val sb = StringBuilder("window: ${root?.packageName}\n")
            fun walk(n: android.view.accessibility.AccessibilityNodeInfo?, depth: Int) {
                if (n == null || depth > 40) return
                val t = n.text?.toString() ?: ""
                val c = n.contentDescription?.toString() ?: ""
                if (t.isNotEmpty() || c.isNotEmpty()) sb.append("  ".repeat(depth)).append(t).append(if (c.isNotEmpty()) " [$c]" else "").append('\n')
                for (i in 0 until n.childCount) walk(n.getChild(i), depth + 1)
            }
            walk(root, 0)
            TestArtifacts.save("$base.txt", (sb.toString() + "\n\nfailure: " + (e?.toString() ?: "")).toByteArray())
        } catch (_: Throwable) {
        }
    }
}
