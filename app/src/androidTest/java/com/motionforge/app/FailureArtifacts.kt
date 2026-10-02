package com.motionforge.app

import androidx.test.platform.app.InstrumentationRegistry
import org.junit.rules.TestWatcher
import org.junit.runner.Description
import java.io.File

/**
 * On any test failure, saves a screenshot and the accessibility window dump to
 * <app files>/test-failures/ (debug build; extracted with run-as), which scripts/android/run-tests.sh pulls back for inspection.
 */
class FailureArtifacts : TestWatcher() {
    override fun failed(e: Throwable?, d: Description) {
        try {
            val inst = InstrumentationRegistry.getInstrumentation()
            val dir = File(inst.targetContext.filesDir, "test-failures").apply { mkdirs() }
            val base = "${d.testClass.simpleName}.${d.methodName}"
            inst.uiAutomation.takeScreenshot()?.let { bmp ->
                File(dir, "$base.png").outputStream().use { bmp.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }
            }
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
            File(dir, "$base.txt").writeText(sb.toString() + "\n\nfailure: " + (e?.toString() ?: ""))
        } catch (_: Throwable) {
        }
    }
}
