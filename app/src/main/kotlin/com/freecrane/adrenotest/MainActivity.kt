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

class MainActivity : Activity() {

    private var surface: Surface? = null

    @Volatile
    private var busy = false

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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val out = TextView(this).apply {
            textSize = 14f
            setTextIsSelectable(true)
        }
        val probeSys = Button(this).apply { text = "Probe system driver" }
        val probeCus = Button(this).apply { text = "Probe custom driver" }
        val renderSys = Button(this).apply { text = "Render test (system driver)" }
        val renderCus = Button(this).apply { text = "Render test (custom driver)" }

        probeSys.setOnClickListener { runProbe(false, out) }
        probeCus.setOnClickListener { runProbe(true, out) }
        renderSys.setOnClickListener { runRender(false, out) }
        renderCus.setOnClickListener { runRender(true, out) }

        val sv = SurfaceView(this)
        sv.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) { surface = h.surface }
            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) {}
            override fun surfaceDestroyed(h: SurfaceHolder) { surface = null }
        })

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 40, 24, 24)
            addView(probeSys)
            addView(probeCus)
            addView(renderSys)
            addView(renderCus)
            addView(out)
            addView(sv, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        setContentView(root)
    }
}
