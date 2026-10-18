package com.freecrane.adrenotest

import android.content.Context
import android.net.Uri
import android.os.Build
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.io.InputStream
import java.util.zip.ZipInputStream

// Vulkan driver packages in the libadrenotools format: a zip with meta.json and the driver library
// (Mesa Turnip builds and Qualcomm proprietary Adreno builds use the same format). The bundled Turnip is the
// default; imported drivers live in files/drivers/<id>/ and one of them can be chosen for the game.
object Drivers {
    class Info(val id: String, val dir: File, val name: String, val library: String, val kind: String,
               val version: String, val minApi: Int, val bundled: Boolean) {
        val label: String get() = "$name  [$kind${if (version.isNotEmpty()) ", $version" else ""}]" + if (bundled) "  (bundled, default)" else ""
    }

    private const val PREFS = "las"
    private const val KEY = "driver_sel"
    private const val MAX_BYTES = 300L shl 20

    private fun root(ctx: Context) = File(ctx.filesDir, "drivers").apply { mkdirs() }

    private fun kindOf(meta: JSONObject): String {
        val s = (meta.optString("name") + " " + meta.optString("vendor") + " " + meta.optString("description") +
            " " + meta.optString("libraryName")).lowercase()
        return when {
            s.contains("turnip") || s.contains("mesa") || s.contains("freedreno") -> "Turnip"
            s.contains("qualcomm") || s.contains("qcom") || s.contains("adreno") -> "Qualcomm"
            else -> "custom"
        }
    }

    private fun read(dir: File, id: String, bundled: Boolean): Info? = try {
        val m = JSONObject(File(dir, "meta.json").readText())
        val lib = m.getString("libraryName")
        if (!File(dir, lib).isFile) null
        else Info(id, dir, m.optString("name", id), lib, kindOf(m), m.optString("driverVersion"), m.optInt("minApi", 0), bundled)
    } catch (t: Throwable) { null }

    fun bundled(ctx: Context): Info? {
        val (dir, _) = Util.installDriver(ctx)
        return read(dir, "", true)
    }

    fun list(ctx: Context): List<Info> {
        val out = ArrayList<Info>()
        bundled(ctx)?.let { out.add(it) }
        root(ctx).listFiles { f -> f.isDirectory }?.sortedBy { it.name }?.forEach { d -> read(d, d.name, false)?.let { out.add(it) } }
        return out
    }

    fun selectedId(ctx: Context): String = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY, "") ?: ""
    fun select(ctx: Context, id: String) { ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(KEY, id).commit() }

    // The driver the game uses: the chosen one, or the bundled Turnip when none is chosen or the files are gone.
    fun active(ctx: Context): Info {
        val id = selectedId(ctx)
        if (id.isNotEmpty()) {
            val d = read(File(root(ctx), id), id, false)
            if (d != null) { Log.i("GuestHost", "driver in use: ${d.label}"); return d }
            Log.w("GuestHost", "driver in use: chosen driver '$id' is missing, falling back to the bundled one")
        }
        val b = bundled(ctx) ?: throw IllegalStateException("bundled driver missing")
        Log.i("GuestHost", "driver in use: ${b.label}")
        return b
    }

    fun delete(ctx: Context, id: String): Boolean {
        if (id.isEmpty() || id.contains('/') || id.contains("..")) return false
        if (selectedId(ctx) == id) select(ctx, "")
        return File(root(ctx), id).deleteRecursively()
    }

    private fun isArm64Elf(h: ByteArray): Boolean =
        h.size >= 20 && h[0] == 0x7f.toByte() && h[1] == 'E'.code.toByte() && h[2] == 'L'.code.toByte() && h[3] == 'F'.code.toByte() &&
            h[4].toInt() == 2 && (h[18].toInt() and 0xff) == 0xB7 && h[19].toInt() == 0

    private fun safeName(s: String): String = s.replace(Regex("[^A-Za-z0-9._-]"), "_").take(60).ifEmpty { "driver" }

    // Imports a driver zip (meta.json + library files) or a bare .so. Returns a message for the user.
    fun import(ctx: Context, uri: Uri): String {
        var queried: String? = null
        try {
            ctx.contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                if (c.moveToFirst()) queried = c.getString(0)
            }
        } catch (t: Throwable) { }
        val name0: String = queried ?: "driver"
        val tmp = File(root(ctx), ".incoming").apply { deleteRecursively(); mkdirs() }
        try {
            val head = ByteArray(4)
            val isZip = ctx.contentResolver.openInputStream(uri)?.use { it.read(head) == 4 && head[0] == 'P'.code.toByte() && head[1] == 'K'.code.toByte() } ?: return "Can't open that file."
            var meta: JSONObject? = null
            if (isZip) {
                var total = 0L
                ctx.contentResolver.openInputStream(uri)!!.use { raw ->
                    ZipInputStream(raw.buffered()).use { z ->
                        while (true) {
                            val e = z.nextEntry ?: break
                            if (e.isDirectory) continue
                            val base = e.name.substringAfterLast('/')
                            val isMeta = base == "meta.json"
                            if (!isMeta && !base.endsWith(".so")) continue
                            val f = File(tmp, if (isMeta) "meta.json" else safeName(base))
                            f.outputStream().use { o ->
                                val buf = ByteArray(1 shl 16)
                                while (true) {
                                    val n = z.read(buf); if (n < 0) break
                                    total += n
                                    if (total > MAX_BYTES) throw IllegalStateException("The package is too large to be a driver.")
                                    o.write(buf, 0, n)
                                }
                            }
                        }
                    }
                }
                val mf = File(tmp, "meta.json")
                if (!mf.exists()) return "This zip has no meta.json, so it isn't a driver package in the libadrenotools format."
                meta = JSONObject(mf.readText())
            } else {
                // a bare library: describe it ourselves
                val lib = safeName(if (name0.endsWith(".so")) name0 else "$name0.so")
                ctx.contentResolver.openInputStream(uri)!!.use { i -> File(tmp, lib).outputStream().use { o -> i.copyTo(o) } }
                meta = JSONObject().put("schemaVersion", 1).put("name", name0.removeSuffix(".so")).put("libraryName", lib).put("vendor", "")
                File(tmp, "meta.json").writeText(meta.toString())
            }
            val lib = meta!!.optString("libraryName")
            if (lib.isEmpty()) return "meta.json has no libraryName."
            val libFile = File(tmp, safeName(lib))
            if (!libFile.isFile) return "The library '$lib' named in meta.json is not in the package."
            val hdr = ByteArray(20)
            libFile.inputStream().use { it.read(hdr) }
            if (!isArm64Elf(hdr)) return "'$lib' is not a 64-bit ARM (arm64) library, so this phone can't load it."
            if (safeName(lib) != lib) { meta.put("libraryName", safeName(lib)); File(tmp, "meta.json").writeText(meta.toString()) }
            val minApi = meta.optInt("minApi", 0)
            val id = safeName(meta.optString("name", name0)) + "_" + java.lang.Long.toHexString(libFile.length())
            val dest = File(root(ctx), id)
            dest.deleteRecursively()
            if (!tmp.renameTo(dest)) return "Couldn't store the driver."
            val info = read(dest, id, false) ?: return "The driver was stored but can't be read back."
            val warn = if (minApi > Build.VERSION.SDK_INT) "\nWarning: it asks for Android API $minApi and this phone has ${Build.VERSION.SDK_INT}." else ""
            return "Imported: ${info.label}$warn\nPick it in the Drivers list and press Use. It applies the next time the game starts."
        } catch (t: Throwable) {
            return "Import failed: ${t.message ?: t}"
        } finally {
            tmp.deleteRecursively()
        }
    }
}
