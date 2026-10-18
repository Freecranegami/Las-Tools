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

    override fun bindService(service: android.content.Intent, conn: android.content.ServiceConnection, flags: Int): Boolean {
        if (GuestPm.blocksPlay(service)) return false
        return super.bindService(service, conn, flags)
    }

    override fun bindService(service: android.content.Intent, flags: Int, executor: java.util.concurrent.Executor,
                             conn: android.content.ServiceConnection): Boolean {
        if (GuestPm.blocksPlay(service)) return false
        return super.bindService(service, flags, executor, conn)
    }

    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)
        if (inGuestProcess()) Log.i("GuestHost", "lifecycle: onTrimMemory level=$level")
    }

    override fun onLowMemory() {
        super.onLowMemory()
        if (inGuestProcess()) Log.i("GuestHost", "lifecycle: onLowMemory")
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        if (inGuestProcess()) {
            GuestLogFile.start(this)
            HiddenApi.exemptAll()
            if (Build.VERSION.SDK_INT >= 29) registerActivityLifecycleCallbacks(GuestSwap)
        }
    }
}

private object GuestSwap : Application.ActivityLifecycleCallbacks {
    // API 29+: called after the activity is attached and before its onCreate.
    override fun onActivityPreCreated(activity: Activity, savedInstanceState: Bundle?) {
        GuestContext.install(activity)
        if (activity.javaClass.classLoader is GuestLoader) { GuestInspect.dump(activity, false); GuestInspect.probePlay(activity); GuestInspect.reflectPlay(activity); GuestInspect.packReport(activity) }
    }

    override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {
        if (activity.javaClass.classLoader is GuestLoader) {
            Log.i("GuestHost", "game activity created: " + activity.javaClass.name)
            GuestInspect.dump(activity, true)
        }
    }

    // Lifecycle events are logged so a black screen can be matched against pauses and surface loss.
    private fun life(what: String, a: Activity) { if (a.javaClass.classLoader is GuestLoader) Log.i("GuestHost", "lifecycle: $what ${a.javaClass.simpleName}") }
    override fun onActivityStarted(activity: Activity) = life("started", activity)
    override fun onActivityResumed(activity: Activity) = life("resumed", activity)
    override fun onActivityPaused(activity: Activity) = life("PAUSED", activity)
    override fun onActivityStopped(activity: Activity) = life("STOPPED", activity)
    override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) {}
    override fun onActivityDestroyed(activity: Activity) = life("DESTROYED", activity)
}

// The phone's log buffer is small, so by the time the log is read after a long run the launch lines
// are gone. The game process keeps its own copy of the lines that matter in a file.
object GuestLogFile {
    private const val MAX = 4_000_000L
    fun file(ctx: android.content.Context) = File(ctx.filesDir, "guest-log.txt")

    fun start(ctx: android.content.Context) {
        // Starting logcat takes a moment; the game's launch lines are written in that time. Ask for
        // everything from this moment on, not from whenever logcat happens to be ready.
        val since = java.text.SimpleDateFormat("MM-dd HH:mm:ss.SSS", java.util.Locale.US).format(java.util.Date())
        Thread {
            try {
                val f = file(ctx)
                if (f.exists() && f.length() > MAX) f.delete()
                // everything this process logs (the game's own messages say why it aborts), plus crash reports
                val pid = android.os.Process.myPid().toString()
                val p = ProcessBuilder("logcat", "-v", "threadtime", "-T", since, "--pid=$pid", "-b", "main", "-b", "crash")
                    .redirectErrorStream(true).start()
                val out = java.io.FileOutputStream(f, true)
                p.inputStream.bufferedReader().useLines { lines ->
                    for (l in lines) {
                        out.write((l + "\n").toByteArray()); out.flush()
                    }
                }
            } catch (t: Throwable) {
                Log.w("GuestHost", "log capture failed: $t")
            }
        }.also { it.isDaemon = true; it.name = "las-logfile" }.start()
    }
}
