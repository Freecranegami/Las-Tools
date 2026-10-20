package com.freecrane.adrenotest

import android.content.Context
import android.system.Os
import android.util.Log

// Performance-related switches: light mode (no diagnostics) and CAS sharpening of the picture the game presents.
// The choices are stored in SharedPreferences by the main app and handed to the game process through LAS_* variables.
object Tweaks {
    private const val PREFS = "las"

    class Cas(val label: String, val strength: Float)
    val casLevels = listOf(
        Cas("Off", 0f),
        Cas("Light (0.25)", 0.25f),
        Cas("Medium (0.5)", 0.5f),
        Cas("Strong (0.8)", 0.8f),
    )

    fun light(ctx: Context): Boolean = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean("light_mode", false)
    fun setLight(ctx: Context, on: Boolean) { ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean("light_mode", on).commit() }

    fun casIndex(ctx: Context): Int = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt("cas_level", 0).coerceIn(0, casLevels.size - 1)
    fun setCas(ctx: Context, i: Int) { ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putInt("cas_level", i).commit() }

    // SGSR: the game renders at the custom resolution and the picture is upscaled to the screen by Snapdragon Game Super
    // Resolution 1 (needs a custom resolution below the screen size).
    fun sgsr(ctx: Context): Boolean = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean("sgsr", false)
    fun setSgsr(ctx: Context, on: Boolean) { ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean("sgsr", on).commit() }

    // Custom resolution: the game is told the screen is this size (window size queries, Vulkan surface size, touch
    // positions) so it renders smaller and the system scales the picture up. 0x0 = the screen's own size.
    fun resW(ctx: Context): Int = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt("res_w", 0)
    fun resH(ctx: Context): Int = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt("res_h", 0)
    fun setRes(ctx: Context, w: Int, h: Int) { ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putInt("res_w", w).putInt("res_h", h).commit() }

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
            val c = casLevels[casIndex(ctx)]
            Os.setenv("LAS_LIGHT", if (l) "1" else "0", true)
            val rw = resW(ctx); val rh = resH(ctx)
            Os.setenv("LAS_RES_W", if (rw > 0 && rh > 0) rw.toString() else "0", true)
            Os.setenv("LAS_RES_H", if (rw > 0 && rh > 0) rh.toString() else "0", true)
            Os.setenv("LAS_SGSR", if (sgsr(ctx)) "1" else "0", true)
            Os.setenv("LAS_CAS", if (c.strength > 0f) c.strength.toString() else "0", true)
            Log.i("GuestHost", "tweaks: light mode ${if (l) "on" else "off"}; CAS ${c.label}; resolution ${if (rw > 0 && rh > 0) "${rw}x$rh" else "native"}; SGSR ${if (sgsr(ctx)) "on" else "off"}")
        } catch (t: Throwable) {
            Log.w("GuestHost", "tweaks failed: $t")
        }
    }
}
