package com.freecrane.adrenotest

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.widget.TextView

// Reads this app's own log, keeps only the last game process, drops noise.
object GuestLog {

    private val dropTokens = listOf(
        "MiuiMagicPointer", "FramePredict", "PerfMonitor", "HandWriting",
        "ScrollerOptimization", "CompatibilityChangeReporter", "MiuiProcessManager",
        "IconCustomizer", "ForceDark", "MessageMonitor", "VRI[", "DecorView",
        "libEGL", "perf_hint", "libboost", "MirrorManager", "IS_CTS_MODE",
        "MULTI_WINDOW_ENABLED", "NetworkSecurityConfig", "GraphicsEnvironment",
        "OpenGLRenderer", "PerfDebugMonitorImpl", "ErrnoException: open failed",
        "libcore.io.", "BlockGuardOs", "ActivityThread.main", "RuntimeInit",
        "ZygoteInit", "reflect.Method.invoke", "Os.open(", "Linux.open("
    )

    fun read(): String {
        val raw = try {
            val p = ProcessBuilder(
                "logcat", "-d", "-b", "main", "-b", "system", "-b", "crash", "-t", "5000")
                .redirectErrorStream(true).start()
            p.inputStream.bufferedReader().readText()
        } catch (e: Throwable) {
            return "Can't read the log: $e"
        }
        val lines = raw.lines()
        val start = lines.indexOfLast { it.contains(":guest: Late-enabling") }
        val tail = if (start >= 0) lines.subList(start, lines.size) else lines.takeLast(300)
        val kept = tail.filter { l -> dropTokens.none { l.contains(it) } }
        val header = if (start >= 0) "[log of the last game process]"
                     else "[no game process found, showing the recent log]"
        val text = header + "\n" + kept.joinToString("\n")
        return if (text.length <= 14000) text
               else text.take(4000) + "\n...[cut]...\n" + text.takeLast(10000)
    }

    fun copy(activity: Activity, out: TextView) {
        out.text = "Reading game log..."
        Thread {
            val text = read()
            activity.runOnUiThread {
                val cm = activity.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                cm.setPrimaryClip(ClipData.newPlainText("game log", text))
                out.text = "Copied ${text.length} characters of the game log. " +
                    "Preview of the end:\n\n" + text.takeLast(1500)
            }
        }.start()
    }
}
