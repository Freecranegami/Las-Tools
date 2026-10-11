package com.freecrane.adrenotest

import android.app.Activity
import android.os.Bundle
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import org.json.JSONObject
import java.io.File

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
}

// A library that links libvulkan.so like a game would. Loaded on demand, so we control
// whether the custom driver was opened before it was loaded.
object Guest {
    fun load() { System.loadLibrary("guesttest") }
    external fun vulkanInfo(): String
}

class MainActivity : Activity() {

    private var surface: Surface? = null

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

    private fun runProbe(custom: Boolean, out: TextView) {
        if (busy) { out.text = "Busy, wait for the current test to finish"; return }
        val p = paths(custom)
        if (custom) customOpened = true
        out.text = Native.probe(p[0], p[1], p[2], p[3], custom)
    }

    private fun runRender(custom: Boolean, out: TextView) {
        if (busy) { out.text = "Busy, wait for the current test to finish"; return }
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
        if (busy) { out.text = "Busy, wait for the current test to finish"; return }
        if (guestLoaded) {
            out.text = "The linked test already ran in this launch.\n" +
                "Fully close and reopen the app to run the other mode."
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

        val sv = SurfaceView(this)
        sv.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) { surface = h.surface }
            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) {}
            override fun surfaceDestroyed(h: SurfaceHolder) { surface = null }
        })

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(16, 32, 16, 16)
            addView(row(
                btn("Probe: system") { runProbe(false, out) },
                btn("Probe: custom") { runProbe(true, out) }))
            addView(row(
                btn("Render: system") { runRender(false, out) },
                btn("Render: custom") { runRender(true, out) }))
            addView(row(
                btn("Linked: system") { linkedTest(false, out) },
                btn("Linked: custom") { linkedTest(true, out) }))
            addView(out)
            addView(sv, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        setContentView(root)
    }
}
