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
        "CRASH   :     lr", "DEBUG   :     lr", "Tombstone written",
        // thousands of successful pack file lookups
        "native-open access(", "native-open stat("
    )

    // what the game asks about the device: always shown first, never cut
    private val identityTokens = listOf("native-hw", "native-prop", "native-gl", "vkshim: GPU", "vkshim: driver", "GPU spoof", "tweaks:", "cas:", "epifx:", "epishaders:", "sgsr:", "fxaa:", "resolution:", "lsfg:", "driver in use", "file hooks", "native-model")

    private val important = listOf(
        " E ", " F ", "GuestHost", "GuestGame", "Unity", "JNI", "Abort", "signal", "vkshim",
        "CANNOT", "dlopen", "linker", "libvulkan", "libvkshim", "Vulkan", "vulkan",
        "clns", "native-fail", "native-open", "crash-info", "crash-regs", "crash-code", "read-fix", "crash-mem", "crash-match", "crash-recent", "native-asset", "native-hw", "native-prop", "native-gl", "lifecycle", "native-stderr", "native-ioctl", "vkshim: kgsl", "vkshim: mem", "file hooks", "snap[", "native thread", "asset packs", "split names", "asset", "splits on disk", "FindClass", "pm ", "play probe", "ctx probe", "Fatal", "FATAL", "Exception"
    )

    // "diag: image WxH": only the ones that could be a render target (at least 600 wide)
    private fun it_big(l: String): Boolean {
        val m = Regex("diag: image (\\d+)x(\\d+)").find(l) ?: return false
        return m.groupValues[1].toInt() >= 600 && m.groupValues[2].toInt() >= 300
    }

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

        // dynamic-resolution diagnostics: own block, so they are neither capped with the identity lines
        // nor lost in the trim. Texture-sized "image" lines are summarised, not listed.
        val diagAll = kept.filter { it.contains("diag: ") }
        kept.removeAll { it.contains("diag: ") }
        val diag = diagAll.filter { !it.contains("diag: image ") && !it.contains("timestamps written") }.take(120) +
            diagAll.filter { it.contains("timestamps written") }.takeLast(15)
        val diagImages = diagAll.count { it.contains("diag: image ") }
        val diagBig = diagAll.filter { l -> it_big(l) }.take(40)
        val identity = kept.filter { l -> identityTokens.any { l.contains(it) } }.take(90)
        kept.removeAll { l -> identityTokens.any { l.contains(it) } }
        val diagBlock = if (diagAll.isEmpty()) "" else
            "--- diag: scene sizes and GPU timers ($diagImages image sizes not listed) ---\n" +
            diag.joinToString("\n") + "\n" + (if (diagBig.isEmpty()) "" else "--- large images ---\n" + diagBig.joinToString("\n") + "\n") +
            "--- end of diag ---\n"
        val header = if (start >= 0) "[log since the last game launch, from the $source]"
                     else "[no game launch found, showing the recent log, from the $source]"
        val idBlock = "--- device identity: what the game read and what it was told ---\n" +
            (if (identity.isEmpty()) "(nothing logged)\n" else identity.joinToString("\n") + "\n") + "--- end of identity ---\n" + diagBlock
        var text = header + "\n" + idBlock + kept.joinToString("\n")
        if (text.length > 22000) {
            // the lines right before the first fatal signal are the game's own last words
            val fatal = kept.indexOfFirst { it.contains("Fatal signal") }
            val from = if (fatal >= 0) maxOf(0, fatal - 80) else kept.size
            val trimmed = kept.filterIndexed { i, l ->
                (i >= from && i <= (if (fatal >= 0) fatal else kept.size)) || isImportant(l) || i >= kept.size - 40 }
            text = "$header [trimmed]\n" + idBlock + trimmed.joinToString("\n")
        }
        return if (text.length <= 22000) text
               else text.take(3000 + idBlock.length) + "\n...[cut]...\n" + text.takeLast(18000)
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
