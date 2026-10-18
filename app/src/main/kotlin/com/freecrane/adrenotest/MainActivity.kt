package com.freecrane.adrenotest

import android.app.Activity
import android.app.AlertDialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.io.File
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
    external fun threadProbe(names: String)
    external fun installFindClassHook(loader: ClassLoader): String
    external fun snapshot(tag: String)
    external fun installFileHooks(pkg: String, extDir: String)

    external fun openCustom(
        tmpDir: String, hookDir: String,
        driverDir: String, driverName: String
    ): String
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

    private fun runProbe(custom: Boolean, out: TextView) {
        if (busy) { out.text = "Busy, wait for the current task to finish"; return }
        val p = Util.paths(this, custom)
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
        val p = Util.paths(this, custom)
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
            val p = Util.paths(this, true)
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
            val p = Util.paths(this, true)
            Native.probe(p[0], p[1], p[2], p[3], true)   // opens the custom driver
            customOpened = true
            val bound = Native.bindShim()

            val src = File(applicationInfo.nativeLibraryDir, "libguesttest.so")
            val dir = File(filesDir, "guestlibs").apply { mkdirs() }
            val dst = File(dir, "libguesttest.so")
            val patched = Util.patchVulkanName(src, dst)

            System.load(dst.absolutePath)
            guestLoaded = true
            out.text = "[shim]\n$bound\nrenamed $patched reference(s)\n" + Guest.vulkanInfo()
        } catch (e: Throwable) {
            out.text = "Shim test failed: $e"
        }
    }

    // ---------- guest import (one APK, several APKs, or a bundle) ----------

    private fun pickApk() {
        if (busy) { outView.text = "Busy, wait for the current task to finish"; return }
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        }
        startActivityForResult(i, pickRequest)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != pickRequest || resultCode != RESULT_OK || data == null) return
        val uris = ArrayList<Uri>()
        val clip = data.clipData
        if (clip != null) {
            for (k in 0 until clip.itemCount) uris.add(clip.getItemAt(k).uri)
        } else {
            val one = data.data
            if (one != null) uris.add(one)
        }
        if (uris.isNotEmpty()) importFiles(uris)
    }

    private fun importFiles(uris: List<Uri>) {
        busy = true
        outView.text = "Importing... large games can take several minutes."
        Thread {
            val msg = try {
                Importer.importAll(this, uris) { p -> runOnUiThread { outView.text = p } }
            } catch (e: Throwable) {
                "Import failed: $e"
            }
            runOnUiThread {
                outView.text = msg
                busy = false
            }
        }.start()
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

    // ---------- launching a game ----------

    private fun launchGame() {
        if (busy) { outView.text = "Busy, wait for the current task to finish"; return }
        val dirs = File(filesDir, "guests").listFiles { f ->
            f.isDirectory && File(f, "base.apk").exists()
        }
        if (dirs == null || dirs.isEmpty()) {
            outView.text = "Import a game first."
            return
        }
        val names = dirs.map { it.name }.toTypedArray()
        if (names.size == 1) {
            chooseMode(names[0])
        } else {
            AlertDialog.Builder(this)
                .setTitle("Launch which game?")
                .setItems(names) { _, which -> chooseMode(names[which]) }
                .show()
        }
    }

    private fun chooseMode(pkg: String) {
        val dir = File(filesDir, "guests/$pkg")
        val adapter = try {
            File(dir, "adapter.txt").readText().trim()
        } catch (e: Exception) {
            "unity"
        }
        if (adapter == "unknown") {
            outView.text = "Looking at the game's files to find its launcher..."
            busy = true
            Thread {
                val found = try {
                    Importer.reanalyze(this, pkg)
                } catch (e: Throwable) {
                    "unknown"
                }
                runOnUiThread {
                    busy = false
                    if (found == "unknown") {
                        outView.text = "This game has no supported launcher yet " +
                            "(no Unity, and no NativeActivity found in its settings or code)."
                    } else {
                        outView.text = "Found its launcher: $found"
                        chooseMode(pkg)
                    }
                }
            }.start()
            return
        }
        if (adapter == "native") {
            val real = GuestRuntime.nativeActivity(dir)
            if (real == null || Build.VERSION.SDK_INT < 29) {
                startLegacyNative(pkg)
                return
            }
            AlertDialog.Builder(this)
                .setTitle("Launch mode")
                .setItems(arrayOf(
                    "1. Run the game's own activity (new)",
                    "2. Basic native launcher (old)")) { _, which ->
                    if (which == 0) startReal(pkg, real) else startLegacyNative(pkg)
                }
                .show()
            return
        }
        val modes = arrayOf(
            "1. Vulkan (forced)",
            "2. Auto graphics (Unity chooses)")
        AlertDialog.Builder(this)
            .setTitle("Launch mode")
            .setItems(modes) { _, which ->
                val i = Intent(this, GuestActivity::class.java)
                i.putExtra("pkg", pkg)
                if (which == 0) i.putExtra("unity", "-force-vulkan")
                startActivity(i)
            }
            .show()
    }

    private fun startLegacyNative(pkg: String) {
        val i = Intent(this, GuestNativeActivity::class.java)
        i.putExtra("pkg", pkg)
        startActivity(i)
    }

    // Starts the game's own activity class through the proxy. The game process is stopped
    // first so every launch begins clean (drivers and game libraries load once per process).
    private fun startReal(pkg: String, activity: String) {
        outView.text = "Starting $activity ..."
        Util.killGuestProcess(this)
        Handler(Looper.getMainLooper()).postDelayed({
            val i = Intent(this, GuestProxyActivity::class.java)
            i.putExtra(GuestRuntime.EXTRA_PKG, pkg)
            i.putExtra(GuestRuntime.EXTRA_ACTIVITY, activity)
            startActivity(i)
        }, 600)
    }

    // Used by the build script: adb shell am start -n <app>/.MainActivity --es launch <pkg> [--es mode legacy]
    private fun autoLaunch(pkg: String, mode: String?) {
        Log.i("GuestHost", "auto launch requested: $pkg mode=${mode ?: "real"}")
        val dir = File(filesDir, "guests/$pkg")
        if (!File(dir, "base.apk").exists()) {
            outView.text = "Auto launch: game not imported: $pkg"
            Log.e("GuestHost", "auto launch: game not imported: $pkg")
            return
        }
        Thread {
            var act = GuestRuntime.nativeActivity(dir)
            if (act == null) {
                try { Importer.reanalyze(this, pkg) } catch (t: Throwable) { }
                act = GuestRuntime.nativeActivity(dir)
            }
            val found = act
            runOnUiThread {
                if (mode == "legacy") {
                    startLegacyNative(pkg)
                } else if (found != null && Build.VERSION.SDK_INT >= 29) {
                    startReal(pkg, found)
                } else {
                    outView.text = "Auto launch: no game activity recorded for $pkg"
                    Log.e("GuestHost", "auto launch: no game activity recorded for $pkg")
                }
            }
        }.start()
    }

    // ---------- log viewer ----------

    private fun readLog(lines: Int): String = try {
        val p = ProcessBuilder("logcat", "-d", "-t", lines.toString())
            .redirectErrorStream(true).start()
        p.inputStream.bufferedReader().readText()
    } catch (e: Throwable) {
        "Can't read the log: $e"
    }

    private fun showLog() {
        outView.text = "Reading log..."
        Thread {
            val text = readLog(250)
            runOnUiThread { outView.text = text }
        }.start()
    }

    private fun copyLog() {
        Thread {
            val text = readLog(700)
            runOnUiThread {
                val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                cm.setPrimaryClip(ClipData.newPlainText("log", text))
                outView.text = "Copied the last 700 log lines to the clipboard."
            }
        }.start()
    }

    private fun chooseGpu(out: TextView) {
        val labels = GpuSpoof.presets.map { it.label }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("GPU the game sees")
            .setSingleChoiceItems(labels, GpuSpoof.index(this)) { d, which ->
                GpuSpoof.set(this, which)
                out.text = "GPU spoof: ${GpuSpoof.presets[which].label}\nApplies the next time the game starts."
                d.dismiss()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun chooseDevice(out: TextView) {
        val labels = GpuSpoof.devices.map { it.label }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("Phone the game sees (also sets GPU + CPU)")
            .setSingleChoiceItems(labels, GpuSpoof.deviceIndex(this)) { d, which ->
                GpuSpoof.setDevice(this, which)
                out.text = "Device spoof: ${GpuSpoof.devices[which].label}\nGPU: ${GpuSpoof.presets[GpuSpoof.index(this)].label}\n" +
                    "CPU: ${GpuSpoof.cpus[GpuSpoof.cpuIndex(this)].label}\nApplies the next time the game starts."
                d.dismiss()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun toggleFocusGuard(out: TextView) {
        val on = !FocusGuard.enabled(this)
        FocusGuard.set(this, on)
        out.text = if (on) "Focus guard ON: the game keeps running (and is not told it lost focus) when the control centre or notification shade opens. Applies the next time the game starts."
                   else "Focus guard OFF: the game is told when it loses focus. Applies the next time the game starts."
    }

    private fun chooseCpu(out: TextView) {
        val labels = GpuSpoof.cpus.map { it.label }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("CPU the game sees")
            .setSingleChoiceItems(labels, GpuSpoof.cpuIndex(this)) { d, which ->
                GpuSpoof.setCpu(this, which)
                out.text = "CPU spoof: ${GpuSpoof.cpus[which].label}\nApplies the next time the game starts."
                d.dismiss()
            }
            .setNegativeButton("Cancel", null)
            .show()
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
            textSize = 13f
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
                btn("Import game") { pickApk() },
                btn("Imported list") { showImported(out) },
                btn("Launch game") { launchGame() }))
            addView(row(
                btn("Show log") { showLog() },
                btn("Copy log") { copyLog() },
                btn("Guest log") { GuestLog.copy(this@MainActivity, out) }))
            addView(row(
                btn("GPU spoof") { chooseGpu(out) },
                btn("CPU spoof") { chooseCpu(out) },
                btn("Device spoof") { chooseDevice(out) }))
            addView(row(
                btn("Focus guard on/off") { toggleFocusGuard(out) }))
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
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 2f))
            addView(sv, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        setContentView(root)

        val auto = intent?.getStringExtra("launch")
        if (auto != null) autoLaunch(auto, intent?.getStringExtra("mode"))
    }
}
