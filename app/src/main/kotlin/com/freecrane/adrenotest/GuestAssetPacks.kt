package com.freecrane.adrenotest

import android.util.Log
import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Method
import java.lang.reflect.Modifier
import java.lang.reflect.Proxy

// The game keeps its data in asset packs (data0, data1, game0 ...). Here they are splits that are
// already on the phone, so the pack manager is asked about packs that are local. For a normal
// install, Play Core answers from local state for those. This wraps the game's pack manager so
// questions about packs that are present as splits are answered from what is on disk; every call
// is logged so what the game asks for is visible.
object GuestAssetPacks {
    private const val TAG = "GuestHost"
    private const val PKG = "com.google.android.play.core.assetpacks."
    private var dumped = false

    @JvmStatic
    fun wrap(real: Any): Any {
        return try {
            doWrap(real)
        } catch (t: Throwable) {
            Log.w(TAG, "asset packs: wrap failed: $t")
            real
        }
    }

    private fun sig(m: Method) = (if (Modifier.isStatic(m.modifiers)) "static " else "") +
        m.returnType.simpleName + " " + m.name + "(" +
        m.parameterTypes.joinToString(",") { it.simpleName } + ")"

    private fun dump(ld: ClassLoader) {
        if (dumped) return
        dumped = true
        for (n in listOf("AssetPackManager", "AssetPackLocation", "AssetPackState", "AssetPackStates")) {
            try {
                val c = Class.forName(PKG + n, false, ld)
                Log.i(TAG, "asset packs: $n: " + c.declaredMethods.joinToString("; ") { sig(it) })
            } catch (t: Throwable) {
                Log.w(TAG, "asset packs: no class $n: $t")
            }
        }
    }

    private fun doWrap(real: Any): Any {
        val g = GuestRuntime.current ?: return real
        val ld = g.loader
        dump(ld)
        val mgr = Class.forName(PKG + "AssetPackManager", false, ld)
        val local = GuestRuntime.splitNames.toSet()
        Log.i(TAG, "asset packs: wrapping the manager, local packs=$local")
        val sizes = HashMap<String, Long>()
        for (f in Util.guestSplits(java.io.File(g.apk.parentFile, "."))) {
            sizes[f.name.removeSuffix(".apk").removePrefix("split_").removePrefix("split-")] = f.length()
        }
        return Proxy.newProxyInstance(ld, arrayOf(mgr)) { _, m, args ->
            val a: Array<Any?> = args ?: emptyArray()
            Log.i(TAG, "asset packs: call ${m.name}(" + a.joinToString(",") { describe(it) } + ")")
            try {
                val mine = answer(ld, m, a, local, sizes)
                if (mine != null) {
                    Log.i(TAG, "asset packs: ${m.name} answered from the local splits")
                    return@newProxyInstance mine
                }
            } catch (t: Throwable) {
                Log.w(TAG, "asset packs: local answer for ${m.name} failed: $t")
            }
            try {
                m.invoke(real, *a)
            } catch (e: InvocationTargetException) {
                throw e.targetException
            }
        }
    }

    private fun describe(o: Any?): String = when (o) {
        null -> "null"
        is Collection<*> -> o.joinToString(",", "[", "]")
        is String, is Number, is Boolean -> o.toString()
        else -> o.javaClass.simpleName
    }

    private fun answer(ld: ClassLoader, m: Method, a: Array<Any?>, local: Set<String>,
                       sizes: Map<String, Long>): Any? {
        when (m.name) {
            "getPackStates", "fetch" -> {
                val names = (a.firstOrNull() as? Collection<*>)?.map { it.toString() } ?: return null
                val mine = names.filter { it in local }
                if (mine.isEmpty()) return null
                val states = statesFor(ld, mine, sizes) ?: return null
                return task(ld, states)
            }
            "getPackLocation" -> {
                val n = a.firstOrNull() as? String ?: return null
                if (n !in local) return null
                return locationFor(ld, n)
            }
        }
        return null
    }

    private fun task(ld: ClassLoader, value: Any): Any? {
        val tasks = Class.forName("com.google.android.gms.tasks.Tasks", true, ld)
        return tasks.getMethod("forResult", Any::class.java).invoke(null, value)
    }

    private fun statesFor(ld: ClassLoader, names: List<String>, sizes: Map<String, Long>): Any? {
        val stateCls = Class.forName(PKG + "AssetPackState", false, ld)
        val statesCls = Class.forName(PKG + "AssetPackStates", false, ld)
        val mk = stateCls.declaredMethods.filter {
            Modifier.isStatic(it.modifiers) && stateCls.isAssignableFrom(it.returnType) &&
                it.parameterTypes.firstOrNull() == String::class.java
        }.maxByOrNull { it.parameterCount } ?: return null
        mk.isAccessible = true
        val map = HashMap<String, Any?>()
        var total = 0L
        for (n in names) {
            val size = sizes[n] ?: 0L
            total += size
            val ints = intArrayOf(4, 0, 100, 0)
            var ii = 0
            var li = 0
            var si = 0
            val args = mk.parameterTypes.map { t ->
                when (t) {
                    String::class.java -> if (si++ == 0) n else ""
                    Int::class.javaPrimitiveType -> ints.getOrElse(ii++) { 0 }
                    Long::class.javaPrimitiveType -> if (li++ < 2) size else 0L
                    else -> null
                }
            }
            map[n] = mk.invoke(null, *args.toTypedArray())
        }
        val mk2 = statesCls.declaredMethods.firstOrNull {
            Modifier.isStatic(it.modifiers) && statesCls.isAssignableFrom(it.returnType) &&
                it.parameterTypes.any { p -> Map::class.java.isAssignableFrom(p) }
        } ?: return null
        mk2.isAccessible = true
        val args = mk2.parameterTypes.map { t ->
            when {
                Map::class.java.isAssignableFrom(t) -> map
                t == Long::class.javaPrimitiveType -> total
                t == Int::class.javaPrimitiveType -> 0
                else -> null
            }
        }
        return mk2.invoke(null, *args.toTypedArray())
    }

    // Packs that are installed as splits are read through the asset manager, not from a folder.
    private fun locationFor(ld: ClassLoader, name: String): Any? {
        val c = Class.forName(PKG + "AssetPackLocation", false, ld)
        val cands = c.declaredMethods.filter {
            Modifier.isStatic(it.modifiers) && c.isAssignableFrom(it.returnType)
        }
        Log.i(TAG, "asset packs: location factories: " + cands.joinToString("; ") { sig(it) })
        return null
    }
}
