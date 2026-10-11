package com.freecrane.adrenotest

import android.app.Activity
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
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
}

class MainActivity : Activity() {

    private fun installDriver(): Pair<File, String> {
        val dir = File(filesDir, "driver").apply { mkdirs() }
        for (n in assets.list("driver") ?: emptyArray()) {
            assets.open("driver/$n").use { i ->
                File(dir, n).outputStream().use { o -> i.copyTo(o) }
            }
        }
        val meta = JSONObject(File(dir, "meta.json").readText())
        return dir to meta.getString("libraryName")
    }

    private fun runProbe(custom: Boolean): String {
        val tmp = File(codeCacheDir, "adreno").apply { mkdirs() }
        val hook = applicationInfo.nativeLibraryDir
        return if (custom) {
            val (dir, name) = installDriver()
            Native.probe("$tmp/", "$hook/", "$dir/", name, true)
        } else {
            Native.probe("$tmp/", "$hook/", "", "", false)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val out = TextView(this).apply {
            textSize = 15f
            setTextIsSelectable(true)
        }
        val sys = Button(this).apply { text = "Probe system driver" }
        val cus = Button(this).apply { text = "Probe custom driver" }
        sys.setOnClickListener { out.text = runProbe(false) }
        cus.setOnClickListener { out.text = runProbe(true) }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 48, 32, 32)
            addView(sys)
            addView(cus)
            addView(ScrollView(this@MainActivity).apply { addView(out) })
        }
        setContentView(root)
    }
}
