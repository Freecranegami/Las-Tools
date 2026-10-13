package com.freecrane.adrenotest

import android.app.Activity
import android.app.AppComponentFactory
import android.content.Intent
import android.util.Log
import java.lang.reflect.Modifier

// The system asks this factory to create every activity of the app. When the request is
// for the game proxy, it creates the game's own activity class instead, from the game's
// class loader, so the game's Java code runs exactly as it would in its own app.
class GuestFactory : AppComponentFactory() {

    override fun instantiateActivity(cl: ClassLoader, className: String, intent: Intent?): Activity {
        if (className == GuestProxyActivity::class.java.name && intent != null) {
            val pkg = intent.getStringExtra(GuestRuntime.EXTRA_PKG)
            val target = intent.getStringExtra(GuestRuntime.EXTRA_ACTIVITY)
            if (pkg != null && target != null) {
                Log.i("GuestHost", "launch step 1 (real): $pkg -> $target")
                try {
                    val g = GuestRuntime.prepare(HostApp.instance, pkg)
                    val act = g.loader.loadClass(target).getDeclaredConstructor().newInstance() as Activity
                    describe(act)
                    return act
                } catch (t: Throwable) {
                    Log.e("GuestHost", "could not start the game's activity", t)
                    throw t
                }
            }
        }
        return super.instantiateActivity(cl, className, intent)
    }

    // Writes the game activity's class chain, native methods and static fields to the log,
    // so a failure inside the game's native code can be matched to its Java side.
    private fun describe(a: Activity) {
        try {
            var c: Class<*>? = a.javaClass
            while (c != null && !c.name.startsWith("android.")) {
                val natives = c.declaredMethods.filter { Modifier.isNative(it.modifiers) }.map { it.name }
                val statics = c.declaredFields.filter { Modifier.isStatic(it.modifiers) }.map { it.name }.take(12)
                Log.i("GuestHost", "class ${c.name}: natives=$natives statics=$statics")
                c = c.superclass
            }
        } catch (t: Throwable) {
            Log.w("GuestHost", "could not inspect ${a.javaClass.name}: $t")
        }
    }
}
