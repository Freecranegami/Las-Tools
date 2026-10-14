package com.freecrane.adrenotest

import android.app.Activity
import android.util.Log
import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Modifier

// Writes what the game's activity class looks like to the log (its methods, and after
// onCreate its static fields and what its context getters return), so a failure inside
// the game's native code can be matched to the game's Java side.
object GuestInspect {
    private const val TAG = "GuestHost"

    private fun logList(label: String, items: List<String>) {
        var line = StringBuilder()
        for (item in items) {
            if (line.length + item.length > 180) {
                Log.i(TAG, "$label $line")
                line = StringBuilder()
            }
            line.append(item).append("; ")
        }
        if (line.isNotEmpty()) Log.i(TAG, "$label $line")
    }

    private fun show(v: Any?): String = when (v) {
        null -> "null"
        is String -> "\"" + v.take(40) + "\""
        is Number, is Boolean, is Char -> v.toString()
        else -> v.javaClass.simpleName
    }

    fun dump(a: Activity, afterCreate: Boolean) {
        try {
            var c: Class<*>? = a.javaClass
            while (c != null && !c.name.startsWith("android.")) {
                val short = c.name.substringAfterLast('.')
                val methods = c.declaredMethods.sortedBy { it.name }.map { m ->
                    (if (Modifier.isNative(m.modifiers)) "N" else "") +
                        (if (Modifier.isStatic(m.modifiers)) "S" else "") + " " + m.name + "(" +
                        m.parameterTypes.joinToString(",") { p -> p.simpleName } + ")"
                }
                logList("$short methods:", methods.take(120))
                if (afterCreate) {
                    val statics = c.declaredFields.filter { Modifier.isStatic(it.modifiers) }.map { f ->
                        try {
                            f.isAccessible = true
                            f.name + "=" + show(f.get(null))
                        } catch (t: Throwable) {
                            f.name + "=?"
                        }
                    }
                    logList("$short statics:", statics.take(80))
                }
                c = c.superclass
            }
            if (afterCreate) probeGetters(a)
        } catch (t: Throwable) {
            Log.w(TAG, "could not inspect ${a.javaClass.name}: $t")
        }
    }

    // The activity declares getContext/getActivity as native methods. Ask them what the
    // game's native side currently holds.
    private fun probeGetters(a: Activity) {
        for (m in a.javaClass.declaredMethods) {
            if ((m.name != "getContext" && m.name != "getActivity") || m.parameterTypes.isNotEmpty()) continue
            val result = try {
                m.isAccessible = true
                show(m.invoke(if (Modifier.isStatic(m.modifiers)) null else a))
            } catch (e: InvocationTargetException) {
                "threw " + e.targetException
            } catch (t: Throwable) {
                "failed " + t
            }
            Log.i(TAG, "probe ${m.name}() -> $result")
        }
    }

    // Call the game's Play Core entry point ourselves and log what happens, since the game's
    // native thread hides the Java error behind a JNI abort.
    fun probePlay(a: android.app.Activity) {
        try {
            Native.threadProbe("com/rockstargames/rdr/NativeGameActivity," +
                "com/google/android/play/core/assetpacks/AssetPackManagerFactory,java/lang/String")
        } catch (t: Throwable) { android.util.Log.w("GuestHost", "thread probe failed: $t") }
        val tag = "GuestHost"
        for ((label, ctx) in listOf<Pair<String, android.content.Context>>(
            "activity" to a, "base" to a.baseContext)) {
            try {
                val f = Class.forName("com.google.android.play.core.assetpacks.AssetPackManagerFactory",
                    true, a.javaClass.classLoader)
                val m = f.getMethod("getInstance", android.content.Context::class.java)
                val r = m.invoke(null, ctx)
                android.util.Log.i(tag, "play probe($label): AssetPackManagerFactory.getInstance -> $r")
                if (r != null) {
                    val st = r.javaClass.getMethod("getPackStates", java.util.List::class.java)
                        .invoke(r, listOf("install_time"))
                    android.util.Log.i(tag, "play probe($label): getPackStates -> $st")
                }
            } catch (t: Throwable) {
                val c = t.cause ?: t
                android.util.Log.w(tag, "play probe($label) failed: $c at " +
                    c.stackTrace.take(6).joinToString(" < "))
            }
        }
        for (n in listOf("getPackageName", "getApplicationContext", "getAssets", "getPackageManager", "getClassLoader")) {
            try {
                val v = a.javaClass.getMethod(n).invoke(a)
                android.util.Log.i(tag, "ctx probe $n -> ${v?.javaClass?.name}")
            } catch (t: Throwable) { android.util.Log.w(tag, "ctx probe $n failed: $t") }
        }
    }
}
