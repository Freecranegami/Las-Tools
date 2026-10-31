package com.freecrane.adrenotest

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

// Virtual gamepad (an Xbox 360 controller drawn on the screen). Settings and the layout live in Prefs, so every game can have its own.
object Pad {
    // kind: stick, dpad, btn (round), shoulder (wide), small (round, small)
    class El(val id: String, val kind: String, val label: String, var x: Float, var y: Float, var size: Float, var vis: Boolean = true)

    private fun ctx(): Context = HostApp.instance

    fun on(c: Context = ctx()): Boolean = Prefs.get(c).getBoolean("pad_on", false)
    fun setOn(c: Context, v: Boolean) { Prefs.get(c).edit().putBoolean("pad_on", v).commit() }
    fun opacity(c: Context = ctx()): Int = Prefs.get(c).getInt("pad_opacity", 70).coerceIn(15, 100)
    fun setOpacity(c: Context, v: Int) { Prefs.get(c).edit().putInt("pad_opacity", v.coerceIn(15, 100)).commit() }
    fun haptics(c: Context = ctx()): Boolean = Prefs.get(c).getBoolean("pad_haptics", true)
    fun setHaptics(c: Context, v: Boolean) { Prefs.get(c).edit().putBoolean("pad_haptics", v).commit() }

    // x, y: centre as a fraction of the screen; size: diameter (width for shoulders) as a fraction of the shorter screen side
    fun defaults(): MutableList<El> = mutableListOf(
        El("lt", "shoulder", "LT", 0.07f, 0.08f, 0.17f),
        El("lb", "shoulder", "LB", 0.07f, 0.20f, 0.17f),
        El("rt", "shoulder", "RT", 0.93f, 0.08f, 0.17f),
        El("rb", "shoulder", "RB", 0.93f, 0.20f, 0.17f),
        El("dpad", "dpad", "", 0.14f, 0.44f, 0.30f),
        El("ls", "stick", "", 0.15f, 0.77f, 0.34f),
        El("back", "small", "BACK", 0.40f, 0.90f, 0.085f),
        El("guide", "small", "XBOX", 0.50f, 0.90f, 0.085f),
        El("start", "small", "START", 0.60f, 0.90f, 0.085f),
        El("l3", "small", "L3", 0.29f, 0.62f, 0.075f),
        El("r3", "small", "R3", 0.71f, 0.62f, 0.075f),
        El("rs", "stick", "", 0.67f, 0.77f, 0.34f),
        El("y", "btn", "Y", 0.87f, 0.31f, 0.13f),
        El("x", "btn", "X", 0.79f, 0.44f, 0.13f),
        El("b", "btn", "B", 0.95f, 0.44f, 0.13f),
        El("a", "btn", "A", 0.87f, 0.57f, 0.13f))

    fun load(c: Context = ctx()): MutableList<El> {
        val l = defaults()
        try {
            val s = Prefs.get(c).getString("pad_layout", null) ?: return l
            val a = JSONArray(s)
            for (i in 0 until a.length()) {
                val o = a.getJSONObject(i)
                val e = l.firstOrNull { it.id == o.getString("id") } ?: continue
                e.x = o.getDouble("x").toFloat().coerceIn(0f, 1f)
                e.y = o.getDouble("y").toFloat().coerceIn(0f, 1f)
                e.size = o.getDouble("s").toFloat().coerceIn(0.04f, 0.8f)
                e.vis = o.optBoolean("v", true)
            }
        } catch (t: Throwable) { }
        return l
    }

    fun save(c: Context, l: List<El>) {
        val a = JSONArray()
        for (e in l) a.put(JSONObject().put("id", e.id).put("x", e.x.toDouble()).put("y", e.y.toDouble()).put("s", e.size.toDouble()).put("v", e.vis))
        Prefs.get(c).edit().putString("pad_layout", a.toString()).commit()
    }

    fun resetLayout(c: Context) { Prefs.get(c).edit().remove("pad_layout").commit() }
}
