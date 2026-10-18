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
