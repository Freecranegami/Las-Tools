package com.freecrane.adrenotest

import android.app.Activity
import android.app.AppComponentFactory
import android.content.Intent
import android.util.Log

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
                    return g.loader.loadClass(target).getDeclaredConstructor().newInstance() as Activity
                } catch (t: Throwable) {
                    Log.e("GuestHost", "could not start the game's activity", t)
                    throw t
                }
            }
        }
        return super.instantiateActivity(cl, className, intent)
    }
}
