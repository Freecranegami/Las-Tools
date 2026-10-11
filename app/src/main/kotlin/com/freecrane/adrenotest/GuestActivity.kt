package com.freecrane.adrenotest

import android.app.Activity
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.content.res.AssetManager
import android.content.res.Configuration
import android.content.res.Resources
import android.os.Bundle
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.view.Window
import android.view.WindowManager
import android.widget.ScrollView
import android.widget.TextView
import dalvik.system.DexClassLoader
import java.io.File
import java.lang.reflect.InvocationHandler
import java.lang.reflect.Proxy

// Hosts a Unity game inside this app. The activity pretends to be the game's own
// activity: it reports the game's package name, resources, assets, class loader and
// APK path, then drives the game's UnityPlayer the same way UnityPlayerActivity does.
class GuestActivity : Activity() {

    private val logTag = "GuestHost"

    private var ready = false
    private lateinit var guestPkg: String
    private lateinit var guestApk: File
    private lateinit var guestAi: ApplicationInfo
    private lateinit var guestRes: Resources
    private lateinit var guestLoader: ClassLoader
    private var player: Any? = null

    override fun getResources(): Resources = if (ready) guestRes else super.getResources()
    override fun getAssets(): AssetManager = if (ready) guestRes.assets else super.getAssets()
    override fun getApplicationInfo(): ApplicationInfo = if (ready) guestAi else super.getApplicationInfo()
    override fun getPackageName(): String = if (ready) guestPkg else super.getPackageName()
    override fun getPackageCodePath(): String = if (ready) guestApk.path else super.getPackageCodePath()
    override fun getClassLoader(): ClassLoader = if (ready) guestLoader else super.getClassLoader()

    override fun onCreate(savedInstanceState: Bundle?) {
        requestWindowFeature(Window.FEATURE_NO_TITLE)
        super.onCreate(null)
        window.setFlags(
            WindowManager.LayoutParams.FLAG_FULLSCREEN,
            WindowManager.LayoutParams.FLAG_FULLSCREEN)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        try {
            launchGuest()
        } catch (t: Throwable) {
            Log.e(logTag, "launch failed", t)
            showError(t)
        }
    }

    private fun launchGuest() {
        val app = applicationContext      // always the host app, never overridden
        val pkg = intent.getStringExtra("pkg") ?: throw IllegalStateException("no game chosen")
        val root = File(filesDir, "guests/$pkg")
        val apk = File(root, "base.apk")
        val libDir = File(root, "lib")
        if (!apk.exists()) throw IllegalStateException("game not imported: $pkg")

        // 1. Shim next to the game's libraries, then open the custom driver.
        Util.ensureShim(app, libDir)
        val p = Util.paths(app, true)
        val status = Native.openCustom(p[0], p[1], p[2], p[3])
        if (status != "custom driver open") throw IllegalStateException(status)

        // 2. Describe the game to the system.
        val pi = packageManager.getPackageArchiveInfo(apk.path, 0)
            ?: throw IllegalStateException("can't read the game's APK")
        val ai = pi.applicationInfo!!
        ai.sourceDir = apk.path
        ai.publicSourceDir = apk.path
        ai.nativeLibraryDir = libDir.path
        ai.dataDir = File(filesDir, "guestdata/$pkg").apply { mkdirs() }.path
        val res = packageManager.getResourcesForApplication(ai)

        // 3. Load the game's Java code. Its parent is the system loader only, so our
        //    own classes stay hidden from the game.
        apk.setReadOnly()
        val dexOut = File(codeCacheDir, "guestdex").apply { mkdirs() }
        val loader = DexClassLoader(
            apk.path, dexOut.path, libDir.path, Activity::class.java.classLoader)

        guestPkg = pkg
        guestApk = apk
        guestAi = ai
        guestRes = res
        guestLoader = loader
        ready = true

        // 4. Start the game.
        val obj = createPlayer()
        player = obj
        val view: View = (obj as? View)
            ?: (obj.javaClass.getMethod("getFrameLayout").invoke(obj) as View)
        setContentView(view)
        view.requestFocus()
    }

    private fun createPlayer(): Any {
        val names = listOf(
            "com.unity3d.player.UnityPlayerForActivityOrService",
            "com.unity3d.player.UnityPlayer")
        var cls: Class<*>? = null
        for (n in names) {
            try {
                cls = guestLoader.loadClass(n)
                break
            } catch (e: ClassNotFoundException) {
                // try the next name
            }
        }
        if (cls == null) {
            throw IllegalStateException("No Unity player class found: this doesn't look like a Unity game")
        }
        Log.i(logTag, "using ${cls.name}")

        val me = this.javaClass
        val ctors = cls.constructors
        val two = ctors.firstOrNull {
            val t = it.parameterTypes
            t.size == 2 && t[0].isAssignableFrom(me) && t[1].isInterface
        }
        if (two != null) {
            val iface = two.parameterTypes[1]
            val proxy = Proxy.newProxyInstance(
                iface.classLoader, arrayOf(iface),
                InvocationHandler { self, m, a ->
                    when (m.name) {
                        "onUnityPlayerUnloaded" -> { runOnUiThread { moveTaskToBack(true) }; null }
                        "onUnityPlayerQuitted" -> { runOnUiThread { finish() }; null }
                        "hashCode" -> System.identityHashCode(self)
                        "equals" -> self === a?.get(0)
                        "toString" -> "GuestLifecycle"
                        else -> null
                    }
                })
            return two.newInstance(this, proxy)
        }
        val one = ctors.firstOrNull {
            it.parameterTypes.size == 1 && it.parameterTypes[0].isAssignableFrom(me)
        } ?: throw IllegalStateException(
            "No usable Unity player constructor: " + ctors.joinToString { it.toString() })
        return one.newInstance(this)
    }

    private fun showError(t: Throwable) {
        val tv = TextView(this).apply {
            text = "Could not start the game:\n\n" + Log.getStackTraceString(t)
            textSize = 12f
            setTextIsSelectable(true)
            setPadding(24, 48, 24, 24)
        }
        setContentView(ScrollView(this).apply { addView(tv) })
    }

    // Calls a method on the Unity player if it exists in this Unity version.
    private fun call(name: String, vararg args: Any?): Any? {
        val p = player ?: return null
        val m = p.javaClass.methods.firstOrNull {
            it.name == name && it.parameterTypes.size == args.size
        } ?: return null
        return try {
            m.invoke(p, *args)
        } catch (t: Throwable) {
            Log.e(logTag, "$name failed", t)
            null
        }
    }

    override fun onStart() { super.onStart(); call("onStart") }
    override fun onResume() { super.onResume(); call("resume") }
    override fun onPause() { super.onPause(); call("pause") }
    override fun onStop() { call("onStop"); super.onStop() }
    override fun onDestroy() { call("destroy"); super.onDestroy() }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        call("windowFocusChanged", hasFocus)
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        call("configurationChanged", newConfig)
    }

    override fun onLowMemory() {
        super.onLowMemory()
        call("lowMemory")
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        call("newIntent", intent)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_MULTIPLE) {
            val r = call("injectEvent", event)
            if (r is Boolean) return r
        }
        return super.dispatchKeyEvent(event)
    }
}
