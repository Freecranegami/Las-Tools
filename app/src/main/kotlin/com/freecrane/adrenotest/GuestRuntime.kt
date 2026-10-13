package com.freecrane.adrenotest

import android.app.Activity
import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.content.res.Resources
import android.util.Log
import java.io.File

// Everything a game needs before its own code runs: the custom driver opened, the Vulkan
// shim next to its libraries, a class loader over its APKs, and its resources.
// A guest process hosts one game, so the result is kept.
object GuestRuntime {

    private const val TAG = "GuestHost"
    const val EXTRA_PKG = "pkg"
    const val EXTRA_ACTIVITY = "activity"

    class Guest(
        val pkg: String,
        val apk: File,
        val ai: ApplicationInfo,
        val res: Resources,
        val loader: GuestLoader,
        val obbDir: File)

    @Volatile
    var current: Guest? = null
        private set

    private fun nativeValue(root: File, key: String): String? {
        val f = File(root, "native.txt")
        if (!f.exists()) return null
        val v = f.readLines().firstOrNull { it.startsWith("$key=") }?.substring(key.length + 1)?.trim()
        return if (v.isNullOrEmpty()) null else v
    }

    // The game's own activity class (a NativeActivity subclass), or null when the game
    // uses plain NativeActivity or nothing was recorded at import time.
    fun nativeActivity(root: File): String? {
        val a = nativeValue(root, "activity") ?: return null
        return if (a == "android.app.NativeActivity") null else a
    }

    private fun mainLibPath(root: File, libDir: File): String? {
        val lib = nativeValue(root, "lib") ?: return null
        if (lib == "main") return null
        val candidates = listOf(
            File(libDir, "lib$lib.so"),
            File(libDir, lib),
            File(libDir, "lib" + lib.removePrefix("lib").removeSuffix(".so") + ".so"))
        return candidates.firstOrNull { it.exists() }?.path
    }

    @Synchronized
    fun prepare(app: Context, pkg: String): Guest {
        current?.let {
            if (it.pkg == pkg) return it
            throw IllegalStateException("this process already runs ${it.pkg}")
        }
        val root = File(app.filesDir, "guests/$pkg")
        val apk = File(root, "base.apk")
        val libDir = File(root, "lib")
        if (!apk.exists()) throw IllegalStateException("game not imported: $pkg")

        Log.i(TAG, "launch step 2 (real): custom driver")
        Util.ensureShim(app, libDir)
        val p = Util.paths(app, true)
        val status = Native.openCustom(p[0], p[1], p[2], p[3])
        if (status != "custom driver open") throw IllegalStateException(status)

        Log.i(TAG, "launch step 3 (real): class loader and assets")
        val dexPath = Util.guestDexPath(root)
        for (f in dexPath.split(":")) File(f).setReadOnly()
        val dexOut = File(app.codeCacheDir, "guestdex").apply { mkdirs() }
        val nativeLib = mainLibPath(root, libDir)
        Log.i(TAG, "native library: " + (nativeLib ?: "main (default)"))
        val loader = GuestLoader(
            dexPath, dexOut.path, libDir.path, Activity::class.java.classLoader, nativeLib)

        val pm = app.packageManager
        val pi = pm.getPackageArchiveInfo(apk.path, PackageManager.GET_META_DATA)
            ?: throw IllegalStateException("can't read the game's APK")
        val ai = pi.applicationInfo ?: throw IllegalStateException("the game's APK has no application info")
        val splits = Util.guestSplits(root).map { it.path }.toTypedArray()
        ai.sourceDir = apk.path
        ai.publicSourceDir = apk.path
        if (splits.isNotEmpty()) {
            ai.splitSourceDirs = splits
            ai.splitPublicSourceDirs = splits
        }
        ai.nativeLibraryDir = libDir.path
        ai.dataDir = File(app.filesDir, "guestdata/$pkg").apply { mkdirs() }.path
        val res = pm.getResourcesForApplication(ai)
        Log.i(TAG, "game application class: ${ai.className ?: "none"}, " +
            "target SDK ${ai.targetSdkVersion}, splits ${splits.size}")

        val g = Guest(pkg, apk, ai, res, loader, File(root, "obb").apply { mkdirs() })
        current = g
        return g
    }
}
