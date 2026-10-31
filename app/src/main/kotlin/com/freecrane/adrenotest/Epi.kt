package com.freecrane.adrenotest

import android.content.Context
import android.system.Os
import android.util.Log
import org.json.JSONObject
import java.io.File

// EpiShaders: ReShade effects (bundled .fx files) that run on the finished picture after the upscaler.
// The app stores which effects are on and their settings in Prefs (so each game can have its own); the game process writes a
// small preset file from them and hands it to the native library (libepifx.so) through LAS_EPI* variables.
object Epi {
    class Effect(val id: String, val name: String, val info: String)
    class Group(val title: String, val effects: List<Effect>)

    // Order here is the order the effects run in: sharpen / anti-alias first, colour next, bloom, vignette, CRT last.
    val groups = listOf(
        Group("Sharpening", listOf(
            Effect("LumaSharpen", "LumaSharpen", "Sharpens the picture without making edges glow"),
            Effect("CAS", "AMD CAS", "AMD's contrast adaptive sharpening"))),
        Group("Anti-aliasing", listOf(
            Effect("FXAA", "FXAA (EpiShaders)", "Smooths jagged edges, a ReShade version"),
            Effect("SMAA", "SMAA", "Sharper anti-aliasing that costs more"))),
        Group("Colour", listOf(
            Effect("FakeHDR", "HDR", "Fake HDR: brighter highlights, deeper shadows"),
            Effect("Vibrance", "Vibrance", "Boosts dull colours more than colourful ones"),
            Effect("Tonemap", "Tonemap", "Gamma, exposure, saturation and bleach"),
            Effect("Levels", "Colour tools: Levels", "Black and white points"),
            Effect("Curves", "Colour tools: Curves", "Contrast curve"),
            Effect("LiftGammaGain", "Colour tools: Lift Gamma Gain", "Shadows, mid-tones and highlights per colour"),
            Effect("ColorMatrix", "Colour tools: Colour matrix", "Mixes the red, green and blue channels"),
            Effect("Technicolor", "Colour tools: Technicolor", "Old film colour look"),
            Effect("Technicolor2", "Colour tools: Technicolor 2", "Another film colour look"),
            Effect("DPX", "Colour tools: DPX (cinema film)", "Film scan look"),
            Effect("Sepia", "Colour tools: Sepia", "Brown old photo tone"),
            Effect("Monochrome", "Colour tools: Monochrome", "Black and white"),
            Effect("LUT", "LUT", "Colour look from a lookup picture"))),
        Group("Light", listOf(
            Effect("Bloom", "Bloom", "Glow around bright areas"),
            Effect("MagicBloom", "Bloom (Magic, with lens dirt)", "A softer, wider glow"),
            Effect("Vignette", "Vignette", "Darkens the corners"))),
        Group("Style", listOf(
            Effect("Cartoon", "Cartoon", "Draws dark outlines"))),
        Group("RetroArch CRT", listOf(
            Effect("zfast_crt", "CRT: zfast (light)", "Cheapest CRT look, scanlines"),
            Effect("CRTEasymode", "CRT: Easymode", "Scanlines and mask"),
            Effect("CRTLottes", "CRT: Lottes", "Classic CRT with bloom"),
            Effect("CRTLottes2", "CRT: Lottes 2", "Lottes, second version"),
            Effect("CRTGeom", "CRT: Geom (curved screen)", "Curved glass and shadow mask, heavy"),
            Effect("CRTcgwg", "CRT: cgwg", "Mask and scanlines"),
            Effect("CRTAperture", "CRT: Aperture grille", "Trinitron style"),
            Effect("CRTCaligari", "CRT: Caligari", "Soft glowing scanlines"),
            Effect("CRTHyllian", "CRT: Hyllian", "Sharp scanlines"),
            Effect("CRTPi", "CRT: Pi (light)", "Scanlines made for weak hardware"))))

    val all: List<Effect> get() = groups.flatMap { it.effects }

    fun master(ctx: Context): Boolean = Prefs.get(ctx).getBoolean("epi_on", true)
    fun setMaster(ctx: Context, on: Boolean) { Prefs.get(ctx).edit().putBoolean("epi_on", on).commit() }
    fun on(ctx: Context, id: String): Boolean = Prefs.get(ctx).getBoolean("epi_e_$id", false)
    fun setOn(ctx: Context, id: String, v: Boolean) { Prefs.get(ctx).edit().putBoolean("epi_e_$id", v).commit() }
    fun enabledCount(ctx: Context): Int = all.count { on(ctx, it.id) }

    private fun key(id: String, u: String) = "epi_v_${id}__$u"
    fun value(ctx: Context, id: String, u: String): DoubleArray? =
        Prefs.get(ctx).getString(key(id, u), null)?.split(",")?.mapNotNull { it.toDoubleOrNull() }?.toDoubleArray()?.takeIf { it.isNotEmpty() }
    fun setValue(ctx: Context, id: String, u: String, v: DoubleArray) {
        Prefs.get(ctx).edit().putString(key(id, u), v.joinToString(",") { "%.6g".format(java.util.Locale.US, it) }).commit()
    }
    fun resetEffect(ctx: Context, id: String) {
        val e = Prefs.get(ctx).edit()
        for (k in Prefs.get(ctx).all.keys) if (k.startsWith("epi_v_${id}__")) e.remove(k)
        e.commit()
    }

    // ---- files ----

    fun root(ctx: Context) = File(ctx.filesDir, "epishaders")

    // Copies the bundled shaders and pictures out of the APK (again after an app update).
    fun ensure(ctx: Context): File {
        val r = root(ctx)
        val stamp = try { ctx.packageManager.getPackageInfo(ctx.packageName, 0).lastUpdateTime.toString() } catch (t: Throwable) { "0" }
        val sf = File(r, ".stamp")
        if (r.isDirectory && sf.exists() && sf.readText() == stamp) return r
        r.deleteRecursively()
        fun copy(path: String, dst: File) {
            val kids = ctx.assets.list(path)
            if (kids != null && kids.isNotEmpty()) {
                dst.mkdirs()
                for (k in kids) copy("$path/$k", File(dst, k))
            } else {
                dst.parentFile?.mkdirs()
                ctx.assets.open(path).use { i -> dst.outputStream().use { o -> i.copyTo(o, 1 shl 16) } }
            }
        }
        copy("epishaders", r)
        sf.writeText(stamp)
        return r
    }

    // The settings of one effect, for the UI. Compiles the effect with ReShade's compiler (a few hundred ms the first time).
    class Param(val name: String, val type: String, val rows: Int, val ui: String, val min: Double?, val max: Double?, val step: Double?,
                val label: String, val tip: String, val cat: String, val items: List<String>, val def: DoubleArray)

    private val cache = HashMap<String, List<Param>?>()
    private var loaded = false

    @JvmStatic external fun describe(root: String, file: String): String

    @Synchronized
    fun params(ctx: Context, id: String): List<Param>? {
        if (cache.containsKey(id)) return cache[id]
        val r = try {
            if (!loaded) { System.loadLibrary("epifx"); loaded = true }
            val j = JSONObject(describe(ensure(ctx).path, "$id.fx"))
            if (!j.optBoolean("ok")) { Log.w("GuestHost", "epishaders: $id: ${j.optString("log")}"); null }
            else {
                val a = j.getJSONArray("uniforms")
                (0 until a.length()).map { i ->
                    val o = a.getJSONObject(i)
                    val items = o.optJSONArray("items")?.let { ar -> (0 until ar.length()).map { ar.getString(it) } } ?: emptyList()
                    val d = o.getJSONArray("def")
                    Param(o.getString("n"), o.getString("t"), o.optInt("r", 1), o.optString("ui"),
                        if (o.has("min")) o.getDouble("min") else null, if (o.has("max")) o.getDouble("max") else null,
                        if (o.has("step")) o.getDouble("step") else null, o.optString("label"), o.optString("tip"), o.optString("cat"),
                        items, DoubleArray(d.length()) { d.getDouble(it) })
                }
            }
        } catch (t: Throwable) { Log.w("GuestHost", "epishaders: describe $id failed: $t"); null }
        cache[id] = r
        return r
    }

    // ---- hand-over to the game process ----

    // Called in the game process before its libraries load.
    fun apply(ctx: Context) {
        try {
            val enabled = if (master(ctx)) all.filter { on(ctx, it.id) } else emptyList()
            if (enabled.isEmpty()) { Os.setenv("LAS_EPI", "0", true); return }
            val r = ensure(ctx)
            val pre = File(ctx.filesDir, "epishaders_preset_" + (Prefs.scope ?: "default").replace(Regex("[^A-Za-z0-9_.]"), "_") + ".txt")
            val sb = StringBuilder()
            for (e in enabled) {
                if (!File(r, "Shaders/${e.id}.fx").exists()) continue
                sb.append("effect ").append(e.id).append('\n')
            }
            val all = Prefs.get(ctx).all
            for (e in enabled) {
                val p = "epi_v_${e.id}__"
                for ((k, v) in all) if (k.startsWith(p) && v is String) sb.append("set ").append(e.id).append(' ').append(k.removePrefix(p)).append(' ').append(v.replace(',', ' ')).append('\n')
            }
            pre.writeText(sb.toString())
            Os.setenv("LAS_EPI", "1", true)
            Os.setenv("LAS_EPI_LIB", File(ctx.applicationInfo.nativeLibraryDir, "libepifx.so").path, true)
            Os.setenv("LAS_EPI_ROOT", r.path, true)
            Os.setenv("LAS_EPI_PRESET", pre.path, true)
            Log.i("GuestHost", "epishaders: on, ${enabled.joinToString { it.id }}")
        } catch (t: Throwable) {
            Log.w("GuestHost", "epishaders: setup failed: $t")
            try { Os.setenv("LAS_EPI", "0", true) } catch (_: Throwable) { }
        }
    }
}
