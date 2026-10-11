package com.freecrane.adrenotest

import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONObject
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.util.zip.ZipFile

object Native {
    init { System.loadLibrary("probe") }

    external fun probe(
        tmpDir: String, hookDir: String,
        driverDir: String, driverName: String,
        custom: Boolean
    ): String

    external fun render(
        surface: Surface,
        tmpDir: String, hookDir: String,
        driverDir: String, driverName: String,
        custom: Boolean
    ): String

    external fun bindShim(): String
}

// A library that links libvulkan.so like a game would.
object Guest {
    fun load() { System.loadLibrary("guesttest") }
    external fun vulkanInfo(): String
}

class MainActivity : Activity() {

    private val pickRequest = 1001

    private var surface: Surface? = null
    private lateinit var outView: TextView

    @Volatile
    private var busy = false
    private var customOpened = false
    private var guestLoaded = false

    private fun installDriver(): Pair<File, String> {
        val dir = File(filesDir, "driver").apply { mkdirs() }
        val stamp = packageManager.getPackageInfo(packageName, 0).lastUpdateTime.toString()
        val marker = File(dir, ".installed")

        // Copy the driver only once per app install. Overwriting a .so that is
        // already loaded into this process crashes it.
        if (!marker.exists() || marker.readText() != stamp) {
            for (n in assets.list("driver") ?: emptyArray()) {
                assets.open("driver/$n").use { i ->
                    File(dir, n).outputStream().use { o -> i.copyTo(o) }
                }
            }
            marker.writeText(stamp)
        }

        val meta = JSONObject(File(dir, "meta.json").readText())
        return dir to meta.getString("libraryName")
    }

    // returns [tmpDir, hookDir, driverDir, driverName]
    private fun paths(custom: Boolean): List<String> {
        val tmp = File(codeCacheDir, "adreno").apply { mkdirs() }
        val hook = applicationInfo.nativeLibraryDir
        return if (custom) {
            val (dir, name) = installDriver()
            listOf("$tmp/", "$hook/", "$dir/", name)
        } else {
            listOf("$tmp/", "$hook/", "", "")
        }
    }

    // Streams input to output, renaming every "libvulkan.so" into "libvkshim.so"
    // (same length). Returns how many were renamed.
    private fun copyPatched(input: InputStream, output: OutputStream): Int {
        val from = "libvulkan.so".toByteArray()
        val to = "libvkshim.so".toByteArray()
        val n = from.size
        val buf = ByteArray(1 shl 20)
        var filled = 0
        var count = 0
        var eof = false
        while (!eof) {
            while (filled < buf.size) {
                val r = input.read(buf, filled, buf.size - filled)
                if (r < 0) { eof = true; break }
                filled += r
            }
            var i = 0
            while (i <= filled - n) {
                if (buf[i] == from[0]) {
                    var match = true
                    for (j in 1 until n) {
                        if (buf[i + j] != from[j]) { match = false; break }
                    }
                    if (match) {
                        System.arraycopy(to, 0, buf, i, n)
                        count++
                        i += n
                        continue
                    }
                }
                i++
            }
            val keep = if (eof) 0 else minOf(n - 1, filled)
            output.write(buf, 0, filled - keep)
            System.arraycopy(buf, filled - keep, buf, 0, keep)
            filled = keep
        }
        return count
    }

    private fun patchVulkanName(src: File, dst: File): Int =
        src.inputStream().use { i -> dst.outputStream().use { o -> copyPatched(i, o) } }

    private fun runProbe(custom: Boolean, out: TextView) {
        if (busy) { out.text = "Busy, wait for the current task to finish"; return }
        val p = paths(custom)
        if (custom) customOpened = true
        out.text = Native.probe(p[0], p[1], p[2], p[3], custom)
    }

    private fun runRender(custom: Boolean, out: TextView) {
        if (busy) { out.text = "Busy, wait for the current task to finish"; return }
        val s = surface
        if (s == null) {
            out.text = "No surface yet"
            return
        }
        val p = paths(custom)
        if (custom) customOpened = true
        busy = true
        out.text = "Rendering..."
        Thread {
            val result = Native.render(s, p[0], p[1], p[2], p[3], custom)
            runOnUiThread {
                out.text = result
                busy = false
            }
        }.start()
    }

    private fun linkedTest(custom: Boolean, out: TextView) {
        if (busy) { out.text = "Busy, wait for the current task to finish"; return }
        if (guestLoaded) {
            out.text = "A linked test already ran in this launch.\n" +
                "Fully close and reopen the app to run another one."
            return
        }
        if (!custom && customOpened) {
            out.text = "The custom driver was already opened in this launch, " +
                "so this control would be misleading.\n" +
                "Fully close and reopen the app, then press Linked: system first."
            return
        }
        if (custom) {
            val p = paths(true)
            Native.probe(p[0], p[1], p[2], p[3], true)   // opens the custom driver
            customOpened = true
        }
        Guest.load()
        guestLoaded = true
        val label = if (custom) "[custom driver opened first]" else "[system only]"
        out.text = label + "\n" + Guest.vulkanInfo()
    }

    private fun linkedShim(out: TextView) {
        if (busy) { out.text = "Busy, wait for the current task to finish"; return }
        if (guestLoaded) {
            out.text = "A linked test already ran in this launch.\n" +
                "Fully close and reopen the app to run another one."
            return
        }
        try {
            val p = paths(true)
            Native.probe(p[0], p[1], p[2], p[3], true)   // opens the custom driver
            customOpened = true
            val bound = Native.bindShim()

            val src = File(applicationInfo.nativeLibraryDir, "libguesttest.so")
            val dir = File(filesDir, "guestlibs").apply { mkdirs() }
            val dst = File(dir, "libguesttest.so")
            val patched = patchVulkanName(src, dst)

            System.load(dst.absolutePath)
            guestLoaded = true
            out.text = "[shim]\n$bound\nrenamed $patched reference(s)\n" + Guest.vulkanInfo()
        } catch (e: Throwable) {
            out.text = "Shim test failed: $e"
        }
    }

    // ---------- guest APK import ----------

    private fun pickApk() {
        if (busy) { outView.text = "Busy, wait for the current task to finish"; return }
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        startActivityForResult(i, pickRequest)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data
        if (requestCode == pickRequest && resultCode == RESULT_OK && uri != null) {
            importApk(uri)
        }
    }

    private fun importApk(uri: Uri) {
        busy = true
        outView.text = "Importing... large games can take a few minutes."
        Thread {
            val msg = try {
                doImport(uri)
            } catch (e: Throwable) {
                "Import failed: $e"
            }
            runOnUiThread {
                outView.text = msg
                busy = false
            }
        }.start()
    }

    private fun doImport(uri: Uri): String {
        val root = File(filesDir, "guests").apply { mkdirs() }
        val tmp = File(root, "incoming.apk")
        contentResolver.openInputStream(uri)!!.use { i ->
            tmp.outputStream().use { o -> i.copyTo(o, 1 shl 16) }
        }

        val pi = packageManager.getPackageArchiveInfo(
            tmp.path, PackageManager.GET_ACTIVITIES or PackageManager.GET_META_DATA)
        if (pi == null) {
            tmp.delete()
            return "That file isn't a readable single-file APK.\n" +
                "Bundles (.xapk, .apks, split APKs) aren't supported yet."
        }
        val ai = pi.applicationInfo!!
        ai.sourceDir = tmp.path
        ai.publicSourceDir = tmp.path
        val label = ai.loadLabel(packageManager).toString()
        val pkg = pi.packageName

        val dir = File(root, pkg)
        dir.deleteRecursively()
        dir.mkdirs()
        val apk = File(dir, "base.apk")
        if (!tmp.renameTo(apk)) {
            tmp.copyTo(apk, overwrite = true)
            tmp.delete()
        }
        val libDir = File(dir, "lib").apply { mkdirs() }

        val abis = sortedSetOf<String>()
        var dexCount = 0
        var extracted = 0
        var bytes = 0L
        val vulkanLibs = mutableListOf<String>()

        ZipFile(apk).use { zip ->
            val entries = zip.entries()
            while (entries.hasMoreElements()) {
                val e = entries.nextElement()
                val name = e.name
                if (name.startsWith("lib/") && name.endsWith(".so")) {
                    val parts = name.split("/")
                    if (parts.size == 3) {
                        abis.add(parts[1])
                        if (parts[1] == "arm64-v8a") {
                            val target = File(libDir, parts[2])
                            val refs = zip.getInputStream(e).use { i ->
                                target.outputStream().use { o -> copyPatched(i, o) }
                            }
                            extracted++
                            bytes += target.length()
                            if (refs > 0) vulkanLibs.add(parts[2])
                        }
                    }
                } else if (name.matches(Regex("classes\\d*\\.dex"))) {
                    dexCount++
                }
            }
        }

        val nativeActivity = pi.activities?.any { it.name == "android.app.NativeActivity" } == true
        val sb = StringBuilder()
        sb.append("Imported: $label\n")
        sb.append("Package: $pkg\n")
        sb.append("Version: ${pi.versionName}\n")
        sb.append("Target SDK: ${ai.targetSdkVersion}\n")
        sb.append("Activities: ${pi.activities?.size ?: 0}")
        if (nativeActivity) sb.append(" (uses NativeActivity)")
        sb.append("\nJava code: $dexCount dex file(s)\n")
        sb.append("ABIs in APK: ${if (abis.isEmpty()) "none" else abis.joinToString(", ")}\n")
        if (!abis.contains("arm64-v8a")) {
            sb.append("No 64-bit libraries found. ")
            sb.append(if (abis.isEmpty())
                "This may be a split bundle with libraries in a separate file.\n"
            else
                "This app is 32-bit only and can't run in this 64-bit app.\n")
        } else {
            sb.append("64-bit libs extracted: $extracted (${bytes / 1024 / 1024} MB)\n")
            if (vulkanLibs.isEmpty()) {
                sb.append("Vulkan: no libvulkan.so reference found (likely OpenGL ES only)\n")
            } else {
                sb.append("Vulkan: yes, ${vulkanLibs.size} lib(s) reference libvulkan.so\n")
                sb.append("  " + vulkanLibs.take(6).joinToString(", ") +
                    (if (vulkanLibs.size > 6) ", ..." else "") + "\n")
                sb.append("Those references now point to the custom-driver shim.\n")
            }
        }
        val report = sb.toString()
        File(dir, "report.txt").writeText(report)
        return report
    }

    private fun showImported(out: TextView) {
        val dirs = File(filesDir, "guests").listFiles { f -> f.isDirectory }
        if (dirs == null || dirs.isEmpty()) {
            out.text = "Nothing imported yet."
            return
        }
        out.text = dirs.joinToString("\n\n") { d ->
            val r = File(d, "report.txt")
            if (r.exists()) r.readText().trim() else d.name
        }
    }

    // ---------- UI ----------

    private fun btn(label: String, onClick: () -> Unit): Button =
        Button(this).apply {
            text = label
            isAllCaps = false
            textSize = 12f
            setOnClickListener { onClick() }
        }

    private fun row(vararg buttons: Button): LinearLayout =
        LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            for (b in buttons) {
                addView(b, LinearLayout.LayoutParams(
                    0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val out = TextView(this).apply {
            textSize = 14f
            setTextIsSelectable(true)
        }
        outView = out

        val sv = SurfaceView(this)
        sv.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) { surface = h.surface }
            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) {}
            override fun surfaceDestroyed(h: SurfaceHolder) { surface = null }
        })

        val scroller = ScrollView(this).apply { addView(out) }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(16, 32, 16, 16)
            addView(row(
                btn("Import APK") { pickApk() },
                btn("Imported list") { showImported(out) }))
            addView(row(
                btn("Probe: system") { runProbe(false, out) },
                btn("Probe: custom") { runProbe(true, out) }))
            addView(row(
                btn("Render: system") { runRender(false, out) },
                btn("Render: custom") { runRender(true, out) }))
            addView(row(
                btn("Linked: system") { linkedTest(false, out) },
                btn("Linked: custom") { linkedTest(true, out) },
                btn("Linked: shim") { linkedShim(out) }))
            addView(scroller, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
            addView(sv, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        setContentView(root)
    }
}
