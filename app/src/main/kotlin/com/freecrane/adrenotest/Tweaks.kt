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

    // Custom resolution: the game window is given a fixed size (WindowManager FLAG_SCALED) and the system scales it
    // up to the screen. 0x0 = the screen's own size.
    fun resW(ctx: Context): Int = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt("res_w", 0)
    fun resH(ctx: Context): Int = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt("res_h", 0)
    fun setRes(ctx: Context, w: Int, h: Int) { ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putInt("res_w", w).putInt("res_h", h).commit() }

    // The screen size in landscape order (the game runs in landscape).
    fun screenSize(ctx: Context): Pair<Int, Int> {
        val wm = ctx.getSystemService(Context.WINDOW_SERVICE) as android.view.WindowManager
        val b = wm.maximumWindowMetrics.bounds
        return Pair(maxOf(b.width(), b.height()), minOf(b.width(), b.height()))
    }

    // Applied to the game's window before its content is set.
    fun applyResolution(a: android.app.Activity) {
        try {
            val w = resW(a); val h = resH(a)
            if (w <= 0 || h <= 0) return
            a.window.addFlags(android.view.WindowManager.LayoutParams.FLAG_SCALED)
            a.window.setLayout(w, h)
            Log.i("GuestHost", "resolution: window fixed at ${w}x$h (screen ${screenSize(a).first}x${screenSize(a).second})")
        } catch (t: Throwable) { Log.w("GuestHost", "resolution failed: $t") }
    }

    // Called in the game process before the game's libraries load.
    fun apply(ctx: Context) {
        try {
            val l = light(ctx)
            val c = casLevels[casIndex(ctx)]
            Os.setenv("LAS_LIGHT", if (l) "1" else "0", true)
            Os.setenv("LAS_CAS", if (c.strength > 0f) c.strength.toString() else "0", true)
            Log.i("GuestHost", "tweaks: light mode ${if (l) "on" else "off"}; CAS ${c.label}")
        } catch (t: Throwable) {
            Log.w("GuestHost", "tweaks failed: $t")
        }
    }
}
