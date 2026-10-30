package com.freecrane.adrenotest

import android.content.Context
import android.content.SharedPreferences

// The settings store. Every setting lives in the "las" preferences (the defaults). While a game's own settings are in
// use (Prefs.scope = its package name) reads come from that game's file first and fall back to the defaults, and
// writes go to the game's file only. The game process sets the scope before it reads any setting.
object Prefs {
    @Volatile var scope: String? = null

    fun fileFor(pkg: String) = "las_game_" + pkg.replace(Regex("[^A-Za-z0-9_.]"), "_")

    fun get(ctx: Context): SharedPreferences {
        val base = ctx.getSharedPreferences("las", Context.MODE_PRIVATE)
        val s = scope ?: return base
        return ScopedPrefs(base, ctx.getSharedPreferences(fileFor(s), Context.MODE_PRIVATE))
    }

    fun gameOnly(ctx: Context, pkg: String): SharedPreferences =
        ctx.getSharedPreferences(fileFor(pkg), Context.MODE_PRIVATE)
}

class ScopedPrefs(private val g: SharedPreferences, private val p: SharedPreferences) : SharedPreferences {
    override fun getAll(): MutableMap<String, *> {
        val m = HashMap<String, Any?>(g.all)
        m.putAll(p.all)
        return m
    }
    override fun getString(key: String?, defValue: String?): String? = if (p.contains(key)) p.getString(key, defValue) else g.getString(key, defValue)
    override fun getStringSet(key: String?, defValues: MutableSet<String>?): MutableSet<String>? = if (p.contains(key)) p.getStringSet(key, defValues) else g.getStringSet(key, defValues)
    override fun getInt(key: String?, defValue: Int): Int = if (p.contains(key)) p.getInt(key, defValue) else g.getInt(key, defValue)
    override fun getLong(key: String?, defValue: Long): Long = if (p.contains(key)) p.getLong(key, defValue) else g.getLong(key, defValue)
    override fun getFloat(key: String?, defValue: Float): Float = if (p.contains(key)) p.getFloat(key, defValue) else g.getFloat(key, defValue)
    override fun getBoolean(key: String?, defValue: Boolean): Boolean = if (p.contains(key)) p.getBoolean(key, defValue) else g.getBoolean(key, defValue)
    override fun contains(key: String?): Boolean = p.contains(key) || g.contains(key)
    override fun edit(): SharedPreferences.Editor = p.edit()
    override fun registerOnSharedPreferenceChangeListener(l: SharedPreferences.OnSharedPreferenceChangeListener?) {
        p.registerOnSharedPreferenceChangeListener(l); g.registerOnSharedPreferenceChangeListener(l)
    }
    override fun unregisterOnSharedPreferenceChangeListener(l: SharedPreferences.OnSharedPreferenceChangeListener?) {
        p.unregisterOnSharedPreferenceChangeListener(l); g.unregisterOnSharedPreferenceChangeListener(l)
    }
}
