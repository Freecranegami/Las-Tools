package com.freecrane.adrenotest

import android.app.ActivityManager
import android.content.Context
import org.json.JSONObject
import java.io.File
import java.io.InputStream
import java.io.OutputStream

object Util {

    // Copies the driver out of the app's assets once per app install. Overwriting a .so
    // that is already loaded into a process crashes it.
    fun installDriver(ctx: Context): Pair<File, String> {
        val dir = File(ctx.filesDir, "driver").apply { mkdirs() }
        val stamp = ctx.packageManager.getPackageInfo(ctx.packageName, 0).lastUpdateTime.toString()
        val marker = File(dir, ".installed")
        if (!marker.exists() || marker.readText() != stamp) {
            for (n in ctx.assets.list("driver") ?: emptyArray()) {
                ctx.assets.open("driver/$n").use { i ->
                    File(dir, n).outputStream().use { o -> i.copyTo(o) }
                }
            }
            marker.writeText(stamp)
        }
        val meta = JSONObject(File(dir, "meta.json").readText())
        return dir to meta.getString("libraryName")
    }

    // returns [tmpDir, hookDir, driverDir, driverName]
    fun paths(ctx: Context, custom: Boolean): List<String> {
        val tmp = File(ctx.codeCacheDir, "adreno").apply { mkdirs() }
        val hook = ctx.applicationInfo.nativeLibraryDir
        return if (custom) {
            val (dir, name) = installDriver(ctx)
            listOf("$tmp/", "$hook/", "$dir/", name)
        } else {
            listOf("$tmp/", "$hook/", "", "")
        }
    }

    // Streams input to output, renaming every "libvulkan.so" into "libvkshim.so"
    // (same length). Returns how many were renamed.
    fun copyPatched(input: InputStream, output: OutputStream): Int {
        val from = "libvulkan.so".toByteArray()
        val to = "libvkshim.so".toByteArray()
        val n = from.size
        val buf = ByteArray(1 shl 20)
        var filled = 0
        var count = 0
        var eof = false
        while (!eof) {
            while (filled < buf.size) {
                val r = input.read(buf, filled, buf.size - filled)
                if (r < 0) { eof = true; break }
                filled += r
            }
            var i = 0
            while (i <= filled - n) {
                if (buf[i] == from[0]) {
                    var match = true
                    for (j in 1 until n) {
                        if (buf[i + j] != from[j]) { match = false; break }
                    }
                    if (match) {
                        System.arraycopy(to, 0, buf, i, n)
                        count++
                        i += n
                        continue
                    }
                }
                i++
            }
            val keep = if (eof) 0 else minOf(n - 1, filled)
            output.write(buf, 0, filled - keep)
            System.arraycopy(buf, filled - keep, buf, 0, keep)
            filled = keep
        }
        return count
    }

    fun patchVulkanName(src: File, dst: File): Int =
        src.inputStream().use { i -> dst.outputStream().use { o -> copyPatched(i, o) } }

    // Puts a copy of the Vulkan shim next to a game's libraries, so the game's own
    // class loader can find it. Copied once per app install.
    fun ensureShim(ctx: Context, libDir: File) {
        libDir.mkdirs()
        val stamp = ctx.packageManager.getPackageInfo(ctx.packageName, 0).lastUpdateTime.toString()
        val target = File(libDir, "libvkshim.so")
        val marker = File(libDir, ".shim_stamp")
        if (!target.exists() || !marker.exists() || marker.readText() != stamp) {
            File(ctx.applicationInfo.nativeLibraryDir, "libvkshim.so")
                .copyTo(target, overwrite = true)
            marker.writeText(stamp)
        }
    }

    // Stops the game process so its files can be replaced safely.
    fun killGuestProcess(ctx: Context) {
        val am = ctx.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        for (p in am.runningAppProcesses ?: emptyList<ActivityManager.RunningAppProcessInfo>()) {
            if (p.processName.endsWith(":guest")) android.os.Process.killProcess(p.pid)
        }
    }
}
