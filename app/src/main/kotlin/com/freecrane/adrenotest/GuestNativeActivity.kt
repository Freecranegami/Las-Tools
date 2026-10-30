package com.freecrane.adrenotest

import android.app.Activity
import android.app.NativeActivity
import android.content.res.AssetManager
import android.content.res.Resources
import android.os.Bundle
import android.util.Log
import java.io.File

// Hosts a NativeActivity game. NativeActivity looks up its library through the activity's
// class loader, so we hand it one that points at the game's own private folder (and maps
// "main" to the game's real library), open the custom driver first, and give it the
// game's assets (including split APKs) and data folder.
class GuestNativeActivity : NativeActivity() {

    private val logTag = "GuestHost"
    private var guestLoader: ClassLoader? = null
    private var guestRes: Resources? = null
    private var guestObb: File? = null

    override fun getClassLoader(): ClassLoader = guestLoader ?: super.getClassLoader()
    override fun getResources(): Resources = guestRes ?: super.getResources()
    override fun getAssets(): AssetManager = guestRes?.assets ?: super.getAssets()
    override fun getObbDir(): File = guestObb ?: super.getObbDir()

    override fun onCreate(savedInstanceState: Bundle?) {
        Log.i(logTag, "launch step 1 (native)")
        try {
            prepare()
        } catch (t: Throwable) {
            Log.e(logTag, "prepare failed", t)
            throw t
        }
        super.onCreate(savedInstanceState)
        Log.i(logTag, "native activity created")
    }

    // The game's declared library name (from native.txt), as a real file, or null.
    private fun mainLibPath(root: File, libDir: File): String? {
        val f = File(root, "native.txt")
        if (!f.exists()) return null
        val lib = f.readLines().firstOrNull { it.startsWith("lib=") }?.substring(4)?.trim()
        if (lib == null || lib.isEmpty() || lib == "main") return null
        val candidates = listOf(
            File(libDir, "lib$lib.so"),
            File(libDir, lib),
            File(libDir, "lib" + lib.removePrefix("lib").removeSuffix(".so") + ".so"))
        return candidates.firstOrNull { it.exists() }?.path
    }

    private fun prepare() {
        val app = applicationContext
        val pkg = intent.getStringExtra("pkg") ?: throw IllegalStateException("no game chosen")
        Prefs.scope = pkg
        val root = File(filesDir, "guests/$pkg")
        val apk = File(root, "base.apk")
        val libDir = File(root, "lib")
        if (!apk.exists()) throw IllegalStateException("game not imported: $pkg")

        Log.i(logTag, "launch step 2 (native): custom driver")
        Util.ensureShim(app, libDir)
        val p = Util.paths(app, true)
        val status = Native.openCustom(p[0], p[1], p[2], p[3])
        if (status != "custom driver open") throw IllegalStateException(status)

        Log.i(logTag, "launch step 3 (native): class loader and assets")
        val dexPath = Util.guestDexPath(root)
        for (f in dexPath.split(":")) File(f).setReadOnly()
        val dexOut = File(codeCacheDir, "guestdex").apply { mkdirs() }
        val nativeLib = mainLibPath(root, libDir)
        Log.i(logTag, "native library: " + (nativeLib ?: "main (default)"))
        guestLoader = GuestLoader(
            dexPath, dexOut.path, libDir.path, Activity::class.java.classLoader, nativeLib)

        // the game's own assets and resources (base APK plus splits)
        try {
            val ai = packageManager.getPackageArchiveInfo(apk.path, 0)?.applicationInfo
            if (ai != null) {
                ai.sourceDir = apk.path
                ai.publicSourceDir = apk.path
                val sp = Util.guestSplits(root).map { it.path }.toTypedArray()
                if (sp.isNotEmpty()) {
                    ai.splitSourceDirs = sp
                    ai.splitPublicSourceDirs = sp
                }
                guestRes = packageManager.getResourcesForApplication(ai)
            }
        } catch (t: Throwable) {
            Log.w(logTag, "could not load the game's assets, using ours", t)
        }
        guestObb = File(root, "obb").apply { mkdirs() }
    }
}
