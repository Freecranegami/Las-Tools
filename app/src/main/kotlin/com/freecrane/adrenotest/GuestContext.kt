package com.freecrane.adrenotest

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.content.res.AssetManager
import android.content.res.Resources
import android.util.Log
import android.view.ContextThemeWrapper
import java.io.File

// The context a game's own activity sees: the game's package name, resources, assets,
// class loader, APK path and OBB folder, on top of this app's real context.
class GuestContext(base: Context, private val g: GuestRuntime.Guest) : ContextWrapper(base) {

    override fun getPackageName(): String = g.pkg
    override fun getApplicationInfo(): ApplicationInfo = g.ai
    override fun getResources(): Resources = g.res
    override fun getAssets(): AssetManager = g.res.assets
    override fun getClassLoader(): ClassLoader = g.loader
    override fun getPackageCodePath(): String = g.apk.path
    override fun getPackageResourcePath(): String = g.apk.path
    override fun getObbDir(): File = g.obbDir
    override fun getObbDirs(): Array<File> = arrayOf(g.obbDir)
    override fun getApplicationContext(): Context = g.app ?: super.getApplicationContext()

    // Questions about the game's own package are answered from its APK (see GuestPm).
    override fun getPackageManager(): PackageManager {
        val pm = super.getPackageManager()
        GuestPm.install(pm, g)
        return pm
    }

    // Some system services identify the caller by the package name of the context they were made
    // for. The game mode service refuses a package that is not installed, so hand out this app's own.
    override fun getSystemService(name: String): Any? {
        if (name == "game") {
            try {
                val r = HostApp.instance.getSystemService(name)
                Log.i(TAG, "game mode service from the host app: " + (r != null))
                if (r != null) return r
            } catch (t: Throwable) {
                Log.w(TAG, "game mode service lookup failed: $t")
            }
        }
        return super.getSystemService(name)
    }

    companion object {
        private const val TAG = "GuestHost"

        private fun setField(cls: Class<*>, name: String, obj: Any, value: Any?): Boolean = try {
            val f = cls.getDeclaredField(name)
            f.isAccessible = true
            f.set(obj, value)
            true
        } catch (t: Throwable) {
            Log.w(TAG, "could not set ${cls.simpleName}.$name: $t")
            false
        }

        // Runs after the system has attached a game's activity and before its onCreate.
        // Swaps the activity's base context for the game's, and drops anything the
        // activity already cached from our own context during attach.
        // The game's native code asks the activity class for getActivity()/getContext(), which read
        // static fields normally filled in by the game's own startup. Fill them before it runs.
        private fun seedStatics(a: Activity) {
            var c: Class<*>? = a.javaClass
            while (c != null && c != Activity::class.java) {
                for (f in c.declaredFields) {
                    if (!java.lang.reflect.Modifier.isStatic(f.modifiers)) continue
                    if (java.lang.reflect.Modifier.isFinal(f.modifiers)) continue
                    val v: Any? = when {
                        f.name.contains("activity", true) && f.type.isAssignableFrom(a.javaClass) -> a
                        f.name.contains("context", true) && f.type.isAssignableFrom(a.javaClass) -> a
                        else -> null
                    }
                    if (v == null) continue
                    try { f.isAccessible = true; if (f.get(null) == null) { f.set(null, v); Log.i(TAG, "seeded ${c.simpleName}.${f.name}") } }
                    catch (e: Throwable) { Log.w(TAG, "seed ${f.name} failed: $e") }
                }
                c = c.superclass
            }
        }

        fun install(a: Activity) {
            val g = GuestRuntime.current ?: return
            if (a.javaClass.classLoader !== g.loader) return
            val base = setField(ContextWrapper::class.java, "mBase", a, GuestContext(a.baseContext, g))
            val res = setField(ContextThemeWrapper::class.java, "mResources", a, null)
            val theme = setField(ContextThemeWrapper::class.java, "mTheme", a, null)
            val infl = setField(ContextThemeWrapper::class.java, "mInflater", a, null)
            // Activity.getApplication() should hand back the game's Application, not ours.
            val gameApp = g.app
            val app = if (gameApp != null) setField(Activity::class.java, "mApplication", a, gameApp) else true
            Log.i(TAG, "game context installed for ${a.javaClass.name}: " +
                "base=$base resources=$res theme=$theme inflater=$infl application=$app")
            seedStatics(a)
        }
    }
}
