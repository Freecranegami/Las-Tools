package com.freecrane.adrenotest

import android.content.Context
import java.io.File
import android.system.Os
import android.util.Log

// Performance-related switches: light mode (no diagnostics) and EpiShaders effects on the picture the game presents.
// The choices are stored in SharedPreferences by the main app and handed to the game process through LAS_* variables.
object Tweaks {
    private const val PREFS = "las"

    fun light(ctx: Context): Boolean = Prefs.get(ctx).getBoolean("light_mode", false)
    fun setLight(ctx: Context, on: Boolean) { Prefs.get(ctx).edit().putBoolean("light_mode", on).commit() }

    // FXAA: anti-aliasing of the picture the game presents (before the upscale when SGSR is used).
    fun fxaa(ctx: Context): Boolean = Prefs.get(ctx).getBoolean("fxaa", false)
    fun setFxaa(ctx: Context, on: Boolean) { Prefs.get(ctx).edit().putBoolean("fxaa", on).commit() }

    // Upscaler: the game renders smaller than the screen (the Resolution set here, or the game's own resolution option)
    // and the picture is upscaled to the screen: 0 = off, 1 = Snapdragon Game Super Resolution 1, 2 = AMD FSR 1 (EASU + RCAS).
    val upscalers = listOf("Off", "SGSR 1 (Snapdragon Game Super Resolution)", "FSR 1 (AMD FidelityFX, EASU + RCAS)")
    fun upscaler(ctx: Context): Int {
        val p = Prefs.get(ctx)
        return p.getInt("upscaler", if (p.getBoolean("sgsr", false)) 1 else 0).coerceIn(0, 2)
    }
    fun setUpscaler(ctx: Context, i: Int) { Prefs.get(ctx).edit().putInt("upscaler", i).commit() }

    // FSR 1's RCAS sharpening, shown as 0..100 % (100 % = sharpest). The filter itself takes "stops" of reduction: 0 stops is
    // the sharpest, 2 stops is very soft; AMD suggests about 0.2 as a default.
    fun rcasPercent(ctx: Context): Int = Prefs.get(ctx).getInt("rcas_pct", 90).coerceIn(0, 100)
    fun setRcasPercent(ctx: Context, v: Int) { Prefs.get(ctx).edit().putInt("rcas_pct", v.coerceIn(0, 100)).commit() }
    fun rcasStops(pct: Int): Float = (100 - pct.coerceIn(0, 100)) / 100f * 2f

    // Frame generation (Lossless Scaling's algorithm, needs the user's Lossless.dll). Default off.
    fun lsfg(ctx: Context): Boolean = Prefs.get(ctx).getBoolean("lsfg", false)
    fun setLsfg(ctx: Context, on: Boolean) { Prefs.get(ctx).edit().putBoolean("lsfg", on).commit() }

    // Lock the game's dynamic resolution (its scene size is kept at the largest step). Default off.
    // Frame generation quality: false = performance (lighter shaders), true = quality. Flow scale in steps of 0.05 (0..20).
    fun lsfgQuality(ctx: Context): Boolean = Prefs.get(ctx).getBoolean("lsfg_quality", false)
    fun setLsfgQuality(ctx: Context, q: Boolean) { Prefs.get(ctx).edit().putBoolean("lsfg_quality", q).commit() }
    fun lsfgFlowSteps(ctx: Context): Int = Prefs.get(ctx).getInt("lsfg_flow", 10).coerceIn(0, 20)
    fun setLsfgFlowSteps(ctx: Context, s: Int) { Prefs.get(ctx).edit().putInt("lsfg_flow", s.coerceIn(0, 20)).commit() }

    fun drsLock(ctx: Context): Boolean = Prefs.get(ctx).getBoolean("drs_lock", false)
    fun setDrsLock(ctx: Context, on: Boolean) { Prefs.get(ctx).edit().putBoolean("drs_lock", on).commit() }

    // Stretch to fit (default on): a game picture whose ratio differs from the screen's is stretched over the whole screen.
    // Off: the ratio is kept and black bars fill the rest (the picture then goes through the shim's own full-size swapchain).
    fun stretch(ctx: Context): Boolean = Prefs.get(ctx).getBoolean("stretch", true)
    fun setStretch(ctx: Context, on: Boolean) { Prefs.get(ctx).edit().putBoolean("stretch", on).commit() }

    // Custom resolution: the game is told the screen is this size (window size queries, Vulkan surface size, touch
    // positions) so it renders smaller and the system scales the picture up. 0x0 = the screen's own size.
    fun resW(ctx: Context): Int = Prefs.get(ctx).getInt("res_w", 0)
    fun resH(ctx: Context): Int = Prefs.get(ctx).getInt("res_h", 0)
    fun setRes(ctx: Context, w: Int, h: Int) { Prefs.get(ctx).edit().putInt("res_w", w).putInt("res_h", h).commit() }

    // The screen size in landscape order (the game runs in landscape).
    fun screenSize(ctx: Context): Pair<Int, Int> {
        val wm = ctx.getSystemService(Context.WINDOW_SERVICE) as android.view.WindowManager
        val b = wm.maximumWindowMetrics.bounds
        return Pair(maxOf(b.width(), b.height()), minOf(b.width(), b.height()))
    }

    // Called in the game process before the game's libraries load.
    fun apply(ctx: Context) {
        try {
            val l = light(ctx)
            Os.setenv("LAS_LIGHT", if (l) "1" else "0", true)
            val rw = resW(ctx); val rh = resH(ctx)
            Os.setenv("LAS_RES_W", if (rw > 0 && rh > 0) rw.toString() else "0", true)
            Os.setenv("LAS_RES_H", if (rw > 0 && rh > 0) rh.toString() else "0", true)
            if (Lsfg.installed(ctx)) Os.setenv("LAS_LSFG_DLL", Lsfg.dll(ctx).path, true)
            Os.setenv("LAS_LSFG", if (lsfg(ctx) && Lsfg.installed(ctx)) "1" else "0", true)
            Os.setenv("LAS_LSFG_LIB", File(ctx.applicationInfo.nativeLibraryDir, "liblsfgcore.so").path, true)
            Os.setenv("LAS_LSFG_PERF", if (lsfgQuality(ctx)) "0" else "1", true)
            Os.setenv("LAS_LSFG_FLOW", "%.2f".format(java.util.Locale.US, lsfgFlowSteps(ctx) * 0.05), true)
            Os.setenv("LAS_DRS", if (drsLock(ctx)) "1" else "0", true)
            Os.setenv("LAS_FXAA", if (fxaa(ctx)) "1" else "0", true)
            val up = upscaler(ctx)
            Os.setenv("LAS_SGSR", if (up == 1) "1" else "0", true)
            Os.setenv("LAS_FSR", if (up == 2) "1" else "0", true)
            Os.setenv("LAS_KEEP", if (stretch(ctx)) "0" else "1", true)
            Os.setenv("LAS_RCAS", rcasStops(rcasPercent(ctx)).toString(), true)
            Os.setenv("LAS_CAS", "0", true)
            Epi.apply(ctx)
            Log.i("GuestHost", "tweaks: light mode ${if (l) "on" else "off"}; resolution ${if (rw > 0 && rh > 0) "${rw}x$rh" else "native"}; upscaler ${upscalers[upscaler(ctx)].substringBefore(" (")}${if (upscaler(ctx) == 2) " (RCAS ${rcasPercent(ctx)}%)" else ""}; ${if (stretch(ctx)) "stretch to fit" else "keep ratio"}; FXAA ${if (fxaa(ctx)) "on" else "off"}; steady resolution ${if (drsLock(ctx)) "on" else "off"}; frame generation ${if (lsfg(ctx) && Lsfg.installed(ctx)) "on" else "off"} (${if (lsfgQuality(ctx)) "quality" else "performance"}, flow ${"%.2f".format(java.util.Locale.US, lsfgFlowSteps(ctx) * 0.05)})")
        } catch (t: Throwable) {
            Log.w("GuestHost", "tweaks failed: $t")
        }
    }
}
