package com.freecrane.adrenotest

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.widget.TextView
import java.io.File

// Reads this app's own log, keeps only the last game launch, drops noise,
// and keeps the lines that matter when it has to shorten the text.
object GuestLog {

    private val dropTokens = listOf(
        "MiuiMagicPointer", "FramePredict", "PerfMonitor", "HandWriting",
        "ScrollerOptimization", "CompatibilityChangeReporter", "MiuiProcessManager",
        "IconCustomizer", "ForceDark", "MessageMonitor", "VRI[", "DecorView",
        "libEGL", "perf_hint", "libboost", "MirrorManager", "IS_CTS_MODE",
        "MULTI_WINDOW_ENABLED", "NetworkSecurityConfig", "GraphicsEnvironment",
        "OpenGLRenderer", "PerfDebugMonitorImpl", "ErrnoException: open failed",
        "libcore.io.", "BlockGuardOs", "ActivityThread.main", "RuntimeInit",
        "ZygoteInit", "reflect.Method.invoke", "Os.open(", "Linux.open(",
        "system_exposed_libraries", "SurfaceSyncer", "SurfaceView", "InsetsController",
        "RenderInspector", "MIUINDBG", "Perf    :", "GameSDKManager",
        "framework-connectivity", "PackageParser",
        "asset in ", "reflect impl methods", "NativeGameActivity methods", "audioTrackData",
        // big crash-report blocks we don't need
        "runtime.cc:761", "verification.cc", "MemMap", "NOTE:", "CRASH   :     x", "DEBUG   :     x",
        "CRASH   :     lr", "DEBUG   :     lr", "Tombstone written"
    )

    private val important = listOf(
        " E ", " F ", "GuestHost", "GuestGame", "Unity", "JNI", "Abort", "signal", "vkshim",
        "CANNOT", "dlopen", "linker", "libvulkan", "libvkshim", "Vulkan", "vulkan",
        "clns", "snap[", "native thread", "asset packs", "split names", "asset", "splits on disk", "FindClass", "pm ", "play probe", "ctx probe", "Fatal", "FATAL", "Exception"
    )

    private val frameRe = Regex("#(\\d+) pc ")

    private fun isImportant(l: String): Boolean = important.any { l.contains(it) }

    fun read(ctx: Context): String {
        val saved = GuestLogFile.file(ctx)
        val fromFile = try {
            if (saved.exists() && saved.length() > 0) saved.readText() else null
        } catch (e: Throwable) { null }
        val raw = if (fromFile != null && fromFile.contains("GuestHost: launch step 1")) fromFile else try {
            val p = ProcessBuilder(
                "logcat", "-d", "-b", "main", "-b", "system", "-b", "crash", "-t", "30000",
                // only the tags that matter, so a long black screen does not push the launch out of range
                "*:S", "GuestHost:V", "PlayCore:V", "playcore:V", "libc:V", "DEBUG:V", "AndroidRuntime:V",
                "nativeloader:D", "ActivityThread:E", "art:W")
                .redirectErrorStream(true).start()
            p.inputStream.bufferedReader().readText()
        } catch (e: Throwable) {
            return "Can't read the log: $e"
        }
        val lines = raw.lines()
        val start = lines.indexOfLast { it.contains("GuestHost: launch step 1") }
        val tail = if (start >= 0) lines.subList(start, lines.size) else lines.takeLast(300)

        val kept = ArrayList<String>()
        for (l in tail) {
            if (dropTokens.any { l.contains(it) }) continue
            // crash reports: keep only the first frames of each backtrace
            val m = frameRe.find(l)
            if (m != null && (l.contains(" DEBUG ") || l.contains(" CRASH ")) &&
                m.groupValues[1].toInt() > 14) continue
            val cap = if (l.contains("GuestHost")) 700 else 220
            kept.add(if (l.length > cap) l.take(cap) else l)
        }

        val source = if (raw === fromFile) "file" else if (fromFile == null) "phone log, no game log file" else "phone log, game log file has no launch"
        // lines that repeat (polling) are shown once with a count
        val prefix = Regex("^\\S+\\s+\\S+\\s+\\d+\\s+\\d+\\s+[A-Z]\\s+")
        val seen = LinkedHashMap<String, Int>()
        val firstLine = HashMap<String, String>()
        val uniq = ArrayList<String>()
        for (l in kept) {
            val key = prefix.replaceFirst(l, "")
            val n = seen[key]
            if (n == null) { seen[key] = 1; firstLine[key] = l; uniq.add(key) } else seen[key] = n + 1
        }
        kept.clear()
        for (key in uniq) {
            val c = seen[key] ?: 1
            kept.add(if (c > 1) firstLine[key] + "   [x$c]" else firstLine[key]!!)
        }

        val header = if (start >= 0) "[log since the last game launch, from the $source]"
                     else "[no game launch found, showing the recent log, from the $source]"
        var text = header + "\n" + kept.joinToString("\n")
        if (text.length > 22000) {
            val trimmed = kept.filterIndexed { i, l -> isImportant(l) || i >= kept.size - 40 }
            text = "$header [trimmed]\n" + trimmed.joinToString("\n")
        }
        return if (text.length <= 22000) text
               else text.take(3000) + "\n...[cut]...\n" + text.takeLast(18000)
    }

    fun copy(activity: Activity, out: TextView) {
        out.text = "Reading game log..."
        Thread {
            val text = read(activity)
            activity.runOnUiThread {
                val cm = activity.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                cm.setPrimaryClip(ClipData.newPlainText("game log", text))
                out.text = "Copied ${text.length} characters of the game log. " +
                    "Preview of the end:\n\n" + text.takeLast(1500)
            }
        }.start()
    }
}
