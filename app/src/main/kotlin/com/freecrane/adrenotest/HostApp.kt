package com.freecrane.adrenotest

import android.app.Activity
import android.app.Application
import android.os.Build
import android.os.Bundle
import android.util.Log
import java.io.File

// This app's Application. In the game process it also watches for the game's own
// activity being created, so the game's context can be put in place before its code runs.
class HostApp : Application() {

    companion object {
        lateinit var instance: HostApp
            private set
    }

    private fun inGuestProcess(): Boolean = try {
        File("/proc/self/cmdline").readText().trimEnd('\u0000').endsWith(":guest")
    } catch (t: Throwable) {
        false
    }

    // The game (and Play Core) reaches us through getApplicationContext(), so this app's own
    // package manager has to answer for the game too.
    override fun getPackageManager(): android.content.pm.PackageManager {
        val pm = super.getPackageManager()
        try {
            val g = GuestRuntime.current
            if (g != null && inGuestProcess()) GuestPm.install(pm, g)
        } catch (t: Throwable) {
            Log.w("GuestHost", "host package manager hook failed: $t")
        }
        return pm
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        if (inGuestProcess()) {
            HiddenApi.exemptAll()
            if (Build.VERSION.SDK_INT >= 29) registerActivityLifecycleCallbacks(GuestSwap)
        }
    }
}

private object GuestSwap : Application.ActivityLifecycleCallbacks {
    // API 29+: called after the activity is attached and before its onCreate.
    override fun onActivityPreCreated(activity: Activity, savedInstanceState: Bundle?) {
        GuestContext.install(activity)
        if (activity.javaClass.classLoader is GuestLoader) { GuestInspect.dump(activity, false); GuestInspect.probePlay(activity); GuestInspect.reflectPlay(activity) }
    }

    override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {
        if (activity.javaClass.classLoader is GuestLoader) {
            Log.i("GuestHost", "game activity created: " + activity.javaClass.name)
            GuestInspect.dump(activity, true)
        }
    }

    override fun onActivityStarted(activity: Activity) {}
    override fun onActivityResumed(activity: Activity) {}
    override fun onActivityPaused(activity: Activity) {}
    override fun onActivityStopped(activity: Activity) {}
    override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) {}
    override fun onActivityDestroyed(activity: Activity) {}
}
