package com.freecrane.adrenotest

import android.content.ComponentName
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.util.Log
import java.lang.reflect.InvocationHandler
import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Method
import java.lang.reflect.Proxy
import java.util.Collections
import java.util.WeakHashMap

// The game believes it is installed as its own package, but Android only knows this app.
// Questions the game asks Android about its own package ("is my asset-pack service
// enabled?", "what is my version?") would fail with "Unknown component" or
// NameNotFoundException, so they are answered from the game's own APK. Everything else
// goes to Android unchanged.
object GuestPm {
    private const val TAG = "GuestHost"
    private val hooked: MutableSet<PackageManager> =
        Collections.newSetFromMap(WeakHashMap<PackageManager, Boolean>())

    // A normally sideloaded copy of the game runs without Play's asset delivery: Play Core simply
    // finds no Play Store service to talk to. Make the guest see the same thing, instead of
    // letting Play answer a package it does not know with an error the game cannot handle.
    fun blocksPlay(intent: android.content.Intent?): Boolean {
        val pkg = intent?.component?.packageName ?: intent?.`package`
        val blocked = pkg == "com.android.vending" && GuestRuntime.current != null
        if (blocked) Log.i(TAG, "bindService to the Play Store refused: " + (intent?.action ?: intent?.component))
        return blocked
    }

    @Volatile
    private var logged = 0

    @Synchronized
    fun install(pm: PackageManager, g: GuestRuntime.Guest) {
        if (!hooked.add(pm)) return
        try {
            val f = pm.javaClass.getDeclaredField("mPM")
            f.isAccessible = true
            val orig = f.get(pm) ?: throw IllegalStateException("no package manager service")
            val iface = Class.forName("android.content.pm.IPackageManager")
            val proxy = Proxy.newProxyInstance(iface.classLoader, arrayOf(iface), Hook(orig, g, pm))
            f.set(pm, proxy)
            Log.i(TAG, "package manager hook installed")
        } catch (t: Throwable) {
            Log.w(TAG, "could not hook the package manager: $t")
        }
    }

    private fun note(m: Method, args: Array<out Any?>, what: String) {
        if (logged >= 60) return
        logged++
        val shown = args.joinToString(", ") {
            when (it) {
                is ComponentName -> it.flattenToShortString()
                is String, is Number, is Boolean -> it.toString()
                else -> it?.javaClass?.simpleName ?: "null"
            }
        }
        Log.i(TAG, "pm ${m.name}($shown) $what")
    }

    private class Hook(
        private val orig: Any,
        private val g: GuestRuntime.Guest,
        private val pm: PackageManager
    ) : InvocationHandler {

        private val infos = HashMap<Long, PackageInfo>()

        private fun packageInfo(args: Array<out Any?>): PackageInfo? {
            val flags = (args.getOrNull(1) as? Number)?.toLong() ?: 0L
            synchronized(infos) {
                infos[flags]?.let { return it }
                return try {
                    val pi = pm.getPackageArchiveInfo(g.apk.path, flags.toInt()) ?: return null
                    pi.applicationInfo = g.ai
                    val sn = GuestRuntime.splitNames
                    if (sn.isNotEmpty()) {
                        pi.splitNames = sn
                        pi.splitRevisionCodes = IntArray(sn.size)
                    }
                    infos[flags] = pi
                    pi
                } catch (t: Throwable) {
                    Log.w(TAG, "could not read the game's package info: $t")
                    null
                }
            }
        }

        override fun invoke(proxy: Any, m: Method, args: Array<out Any?>?): Any? {
            val a: Array<out Any?> = args ?: emptyArray()
            val cn = a.firstOrNull { it is ComponentName } as ComponentName?
            val mine = (cn != null && cn.packageName == g.pkg) || a.any { it is String && it == g.pkg }
            // The game's libraries build component names from whichever context they were given,
            // which can carry this app's package name with a class that only exists in the game.
            val gameComponent = cn != null &&
                (cn.packageName == g.pkg || !cn.className.startsWith("com.freecrane.adrenotest"))
            if (gameComponent && (m.name == "setComponentEnabledSetting" ||
                        m.name == "getComponentEnabledSetting")) {
                note(m, a, "-> game component, answered here")
                return if (m.name == "setComponentEnabledSetting") null
                       else PackageManager.COMPONENT_ENABLED_STATE_DEFAULT
            }
            if (mine) {
                when (m.name) {
                    "getComponentEnabledSetting" -> {
                        note(m, a, "-> default")
                        return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT
                    }
                    "setComponentEnabledSetting" -> {
                        note(m, a, "-> ignored")
                        return null
                    }
                    "getPackageInfo" -> {
                        val pi = packageInfo(a)
                        if (pi != null) {
                            note(m, a, "-> from the game's APK")
                            return pi
                        }
                    }
                    "getApplicationInfo" -> {
                        note(m, a, "-> from the game's APK")
                        return g.ai
                    }
                }
            }
            try {
                val r = m.invoke(orig, *a)
                if (mine) note(m, a, "-> passed to Android")
                return r
            } catch (e: InvocationTargetException) {
                val t = e.targetException
                if (mine) note(m, a, "-> Android threw ${t.javaClass.simpleName}")
                throw t
            }
        }
    }
}
