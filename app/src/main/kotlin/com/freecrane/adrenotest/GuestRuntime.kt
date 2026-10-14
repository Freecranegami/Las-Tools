package com.freecrane.adrenotest

import android.app.Activity
import android.app.Application
import android.content.Context
import android.content.ContextWrapper
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
        val obbDir: File) {
        // The game's own Application object, once started.
        @Volatile
        var app: Application? = null
    }

    @Volatile
    @Volatile
    var splitNames: Array<String> = emptyArray()

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
        try {
            val sd = File(root, "splits")
            val list = sd.listFiles()?.sortedBy { it.name } ?: emptyList()
            Log.i(TAG, "splits on disk: " + list.joinToString("; ") { f ->
                val n = try { java.util.zip.ZipFile(f).use { z ->
                    z.entries().asSequence().count { it.name.startsWith("assets/") } } } catch (t: Throwable) { -1 }
                "${f.name} ${f.length() / 1024}KB assets=$n"
            })
        } catch (t: Throwable) { Log.w(TAG, "split listing failed: $t") }
        try { Log.i(TAG, Native.installFindClassHook(loader)) }
        catch (t: Throwable) { Log.w(TAG, "FindClass hook failed: $t") }

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
        // Describe the splits the way Android does for an installed app, so libraries that ask
        // "which splits (asset packs) are installed?" see the ones that are on disk.
        val names = Util.guestSplits(root).map {
            it.name.removeSuffix(".apk").removePrefix("split_").removePrefix("split-")
        }.toTypedArray()
        splitNames = names
        try {
            val f = ApplicationInfo::class.java.getDeclaredField("splitNames")
            f.isAccessible = true
            f.set(ai, names)
        } catch (t: Throwable) { Log.w(TAG, "could not set the split names: $t") }
        Log.i(TAG, "split names: " + names.joinToString(","))
        ai.nativeLibraryDir = libDir.path
        ai.dataDir = File(app.filesDir, "guestdata/$pkg").apply { mkdirs() }.path
        val res = pm.getResourcesForApplication(ai)
        Log.i(TAG, "game application class: ${ai.className ?: "none"}, " +
            "target SDK ${ai.targetSdkVersion}, splits ${splits.size}")

        Log.i(TAG, "native.txt: lib=${nativeValue(root, "lib") ?: "-"} " +
            "func=${nativeValue(root, "func") ?: "-"} activity=${nativeValue(root, "activity") ?: "-"}")

        val g = Guest(pkg, apk, ai, res, loader, File(root, "obb").apply { mkdirs() })
        current = g
        startGameApplication(app, g)
        return g
    }

    // Android starts the app's Application before any of its activities. Games keep
    // state there (and in statics it sets), so start the game's own one the same way.
    private fun startGameApplication(host: Context, g: Guest) {
        val name = g.ai.className
        if (name.isNullOrEmpty() || name == "android.app.Application") {
            Log.i(TAG, "game has no Application class of its own")
            return
        }
        Log.i(TAG, "starting the game's Application: $name")
        try {
            val a = g.loader.loadClass(name).getDeclaredConstructor().newInstance() as Application
            val attach = ContextWrapper::class.java.getDeclaredMethod("attachBaseContext", Context::class.java)
            attach.isAccessible = true
            attach.invoke(a, GuestContext(host, g))
            g.app = a
            a.onCreate()
            Log.i(TAG, "game Application started")
        } catch (t: Throwable) {
            Log.e(TAG, "the game's Application failed to start", t)
            throw t
        }
    }
}
