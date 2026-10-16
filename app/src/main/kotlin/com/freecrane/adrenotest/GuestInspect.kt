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

    // One-off look at the Play Core asset pack classes, so the next change uses real names.
    fun reflectPlay(a: android.app.Activity) {
        val tag = "GuestHost"
        val base = "com.google.android.play.core.assetpacks."
        val names = listOf("AssetPackManager", "AssetPackState", "AssetPackStates", "AssetPackLocation",
            "AssetPackManagerFactory")
        for (n in names) {
            try {
                val c = Class.forName(base + n, false, a.javaClass.classLoader)
                android.util.Log.i(tag, "reflect $n: super=${c.superclass?.name} " +
                    "interfaces=${c.interfaces.joinToString { it.simpleName }} " +
                    "abstract=${java.lang.reflect.Modifier.isAbstract(c.modifiers)}")
                val parts = ArrayList<String>()
                for (m in c.declaredMethods) {
                    val st = if (java.lang.reflect.Modifier.isStatic(m.modifiers)) "S " else ""
                    val ab = if (java.lang.reflect.Modifier.isAbstract(m.modifiers)) "A " else ""
                    parts.add("$st$ab${m.returnType.simpleName} ${m.name}(${m.parameterTypes.joinToString { it.simpleName }})")
                }
                parts.sorted().chunked(6).forEach { android.util.Log.i(tag, "reflect $n methods: " + it.joinToString("; ")) }
                val ctors = c.declaredConstructors.joinToString("; ") { k ->
                    "(" + k.parameterTypes.joinToString { it.simpleName } + ")" }
                android.util.Log.i(tag, "reflect $n ctors: $ctors")
            } catch (t: Throwable) {
                android.util.Log.w(tag, "reflect $n failed: $t")
            }
        }
        try {
            val f = Class.forName(base + "AssetPackManagerFactory", true, a.javaClass.classLoader)
            val mgr = f.getMethod("getInstance", android.content.Context::class.java).invoke(null, a)
            android.util.Log.i(tag, "reflect manager impl: ${mgr?.javaClass?.name}")
            mgr?.javaClass?.declaredMethods?.map {
                "${it.returnType.simpleName} ${it.name}(${it.parameterTypes.joinToString { p -> p.simpleName }})"
            }?.sorted()?.chunked(6)?.forEach { android.util.Log.i(tag, "reflect impl methods: " + it.joinToString("; ")) }
        } catch (t: Throwable) {
            android.util.Log.w(tag, "reflect manager failed: ${t.cause ?: t}")
        }
    }

    // What does Play Core itself say about each asset pack, with no Play Store to ask?
    fun packReport(a: android.app.Activity) {
        Thread {
            val tag = "GuestHost"
            val base = "com.google.android.play.core.assetpacks."
            try {
                val cl = a.javaClass.classLoader
                val f = Class.forName(base + "AssetPackManagerFactory", true, cl)
                val mgr = f.getMethod("getInstance", android.content.Context::class.java).invoke(null, a)!!
                val mc = Class.forName(base + "AssetPackManager", true, cl)
                val names = GuestRuntime.splitNames.toList()
                for (n in names) {
                    try {
                        val loc = mc.getMethod("getPackLocation", String::class.java).invoke(mgr, n)
                        if (loc == null) android.util.Log.i(tag, "pack report: location($n) = null")
                        else {
                            val lc = Class.forName(base + "AssetPackLocation", true, cl)
                            android.util.Log.i(tag, "pack report: location($n) storage=" +
                                lc.getMethod("packStorageMethod").invoke(loc) + " path=" +
                                lc.getMethod("assetsPath").invoke(loc))
                        }
                    } catch (t: Throwable) { android.util.Log.w(tag, "pack report: location($n) failed: ${t.cause ?: t}") }
                }
                val task = mc.getMethod("getPackStates", java.util.List::class.java).invoke(mgr, names)!!
                val tk = Class.forName("com.google.android.gms.tasks.Task", true, cl)
                var waited = 0
                while (!(tk.getMethod("isComplete").invoke(task) as Boolean) && waited < 8000) {
                    Thread.sleep(100); waited += 100
                }
                if (!(tk.getMethod("isComplete").invoke(task) as Boolean)) {
                    android.util.Log.i(tag, "pack report: getPackStates did not finish in 8 s"); return@Thread
                }
                if (tk.getMethod("isSuccessful").invoke(task) as Boolean) {
                    val res = tk.getMethod("getResult").invoke(task)!!
                    val rc = Class.forName(base + "AssetPackStates", true, cl)
                    val map = rc.getMethod("packStates").invoke(res) as Map<*, *>
                    val sc = Class.forName(base + "AssetPackState", true, cl)
                    for ((k, v) in map) {
                        android.util.Log.i(tag, "pack report: state($k) status=" + sc.getMethod("status").invoke(v) +
                            " error=" + sc.getMethod("errorCode").invoke(v) +
                            " bytes=" + sc.getMethod("bytesDownloaded").invoke(v) + "/" +
                            sc.getMethod("totalBytesToDownload").invoke(v))
                    }
                    android.util.Log.i(tag, "pack report: ${map.size} pack states")
                } else {
                    android.util.Log.i(tag, "pack report: getPackStates failed: " + tk.getMethod("getException").invoke(task))
                }
            } catch (t: Throwable) {
                android.util.Log.w(tag, "pack report failed: ${t.cause ?: t}")
            }
        }.start()
    }
}
