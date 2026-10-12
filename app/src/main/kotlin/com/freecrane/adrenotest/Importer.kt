package com.freecrane.adrenotest

import android.content.Context
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import java.io.File
import java.util.zip.ZipFile

// Imports a game from: one APK, several APKs (base + splits), or a bundle
// (.xapk / .apks / .apkm / .zip), plus any .obb data files.
object Importer {

    private class Opened(val zip: ZipFile, val pfd: ParcelFileDescriptor?, val tmp: File?) {
        fun close() {
            try { zip.close() } catch (e: Exception) { }
            try { pfd?.close() } catch (e: Exception) { }
            tmp?.delete()
        }
    }

    private fun displayName(ctx: Context, uri: Uri): String? = try {
        ctx.contentResolver.query(
            uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0) else null
        }
    } catch (e: Exception) {
        null
    }

    private fun safeName(s: String): String = s.replace(Regex("[^A-Za-z0-9._-]"), "_")

    private fun uniqueFile(dir: File, name: String): File {
        var f = File(dir, name)
        var i = 1
        while (f.exists()) {
            f = File(dir, "${i}_$name")
            i++
        }
        return f
    }

    private fun move(src: File, dst: File) {
        if (!src.renameTo(dst)) {
            src.copyTo(dst, overwrite = true)
            src.delete()
        }
    }

    // Opens a picked file as a zip, reading it in place when possible and
    // falling back to a temporary copy.
    private fun openZip(ctx: Context, uri: Uri, stage: File, n: Int): Opened? {
        try {
            val pfd = ctx.contentResolver.openFileDescriptor(uri, "r")
            if (pfd != null) {
                try {
                    return Opened(ZipFile("/proc/self/fd/" + pfd.fd), pfd, null)
                } catch (e: Exception) {
                    pfd.close()
                }
            }
        } catch (e: Exception) {
            // fall through to the copy
        }
        val tmp = File(stage, "tmp_$n.bin")
        val input = ctx.contentResolver.openInputStream(uri) ?: return null
        input.use { i -> tmp.outputStream().use { o -> i.copyTo(o, 1 shl 16) } }
        return try {
            Opened(ZipFile(tmp), null, tmp)
        } catch (e: Exception) {
            tmp.delete()
            null
        }
    }

    // Split APKs carry only resources or libraries for one device type.
    private fun looksLikeSplit(staged: String): Boolean {
        val n = staged.lowercase().replace(Regex("^(\\d+_)?[pb]\\d+_"), "")
        if (n.contains("config.") || n.contains("split_") || n.contains("split-")) return true
        if (n.startsWith("base-") && !n.startsWith("base-master")) return true
        return false
    }

    private fun collect(
        ctx: Context, uri: Uri, n: Int, stage: File,
        apks: MutableList<File>, obbs: MutableList<File>, notes: MutableList<String>
    ) {
        val name = displayName(ctx, uri) ?: "file$n"
        val obbDir = File(stage, "obb").apply { mkdirs() }

        // a loose data file
        if (name.endsWith(".obb", ignoreCase = true)) {
            val dst = File(obbDir, name)
            ctx.contentResolver.openInputStream(uri)!!.use { i ->
                dst.outputStream().use { o -> i.copyTo(o, 1 shl 16) }
            }
            obbs.add(dst)
            return
        }

        val opened = openZip(ctx, uri, stage, n)
            ?: throw IllegalStateException("$name isn't an APK or a bundle I can read")
        try {
            val zip = opened.zip
            val entries = zip.entries().toList()

            if (entries.any { it.name == "AndroidManifest.xml" }) {
                // a plain APK: keep the whole file
                val dst = File(stage, "p${n}_" + safeName(name))
                val t = opened.tmp
                if (t == null || !t.renameTo(dst)) {
                    ctx.contentResolver.openInputStream(uri)!!.use { i ->
                        dst.outputStream().use { o -> i.copyTo(o, 1 shl 16) }
                    }
                }
                apks.add(dst)
                return
            }

            // a bundle: take its APKs (and data files)
            val inner = entries.filter { !it.isDirectory && it.name.endsWith(".apk", ignoreCase = true) }
            val hasSplitsDir = inner.any { it.name.startsWith("splits/") }
            val chosen = inner.filter {
                !hasSplitsDir || it.name.startsWith("splits/") || it.name.startsWith("asset-slices/")
            }
            if (chosen.isEmpty()) throw IllegalStateException("$name contains no APK files")
            for (e in chosen) {
                val dst = uniqueFile(stage, "b${n}_" + safeName(e.name.substringAfterLast('/')))
                zip.getInputStream(e).use { i ->
                    dst.outputStream().use { o -> i.copyTo(o, 1 shl 16) }
                }
                apks.add(dst)
            }
            for (e in entries) {
                if (!e.isDirectory && e.name.endsWith(".obb", ignoreCase = true)) {
                    val dst = uniqueFile(obbDir, e.name.substringAfterLast('/'))
                    zip.getInputStream(e).use { i ->
                        dst.outputStream().use { o -> i.copyTo(o, 1 shl 16) }
                    }
                    obbs.add(dst)
                }
            }
            notes.add("Bundle $name: ${chosen.size} APK file(s)")
        } finally {
            opened.close()
        }
    }

    fun importAll(ctx: Context, uris: List<Uri>, progress: (String) -> Unit): String {
        Util.killGuestProcess(ctx)
        val root = File(ctx.filesDir, "guests").apply { mkdirs() }
        val stage = File(ctx.filesDir, "import_staging")
        stage.deleteRecursively()
        stage.mkdirs()
        try {
            val apks = ArrayList<File>()
            val obbs = ArrayList<File>()
            val notes = ArrayList<String>()
            for ((n, uri) in uris.withIndex()) {
                progress("Reading file ${n + 1} of ${uris.size}...")
                collect(ctx, uri, n, stage, apks, obbs, notes)
            }
            if (apks.isEmpty()) return "No APK files found in what you picked."
            return finish(ctx, root, apks, obbs, notes, progress)
        } finally {
            stage.deleteRecursively()
        }
    }

    private fun finish(
        ctx: Context, root: File, apks: List<File>, obbs: List<File>,
        notes: List<String>, progress: (String) -> Unit
    ): String {
        val pm = ctx.packageManager

        // The base APK holds the app's activities; split APKs (config.*, base-xxhdpi, ...)
        // don't. Rank by what each file contains, then by its name.
        var base: File? = null
        var info: PackageInfo? = null
        val counts = HashMap<File, Int>()
        for (f in apks) {
            counts[f] = try {
                pm.getPackageArchiveInfo(f.path, PackageManager.GET_ACTIVITIES)?.activities?.size ?: 0
            } catch (e: Exception) {
                0
            }
        }
        val ranked = apks.sortedWith(compareBy<File>(
            { if ((counts[it] ?: 0) > 0) 0 else 1 },
            { if (looksLikeSplit(it.name)) 1 else 0 }
        ))
        for (f in ranked) {
            val pi = pm.getPackageArchiveInfo(
                f.path, PackageManager.GET_ACTIVITIES or PackageManager.GET_META_DATA)
            if (pi != null) {
                base = f
                info = pi
                break
            }
        }
        val b = base
        val pi = info
        if (b == null || pi == null) {
            return "I couldn't find a base APK I can read among ${apks.size} file(s):\n" +
                apks.joinToString("\n") { "  " + it.name } +
                "\nIf these are only split files, pick the base APK too."
        }
        val ai = pi.applicationInfo!!
        ai.sourceDir = b.path
        ai.publicSourceDir = b.path
        val label = ai.loadLabel(pm).toString()
        val pkg = pi.packageName

        // lay out the game's folder
        val dir = File(root, pkg)
        dir.deleteRecursively()
        dir.mkdirs()
        val baseDst = File(dir, "base.apk")
        move(b, baseDst)
        val splitDir = File(dir, "splits").apply { mkdirs() }
        val splitDsts = ArrayList<File>()
        for (f in apks) {
            if (f == b) continue
            val dst = uniqueFile(splitDir, f.name.replace(Regex("^[pb]\\d+_"), ""))
            move(f, dst)
            splitDsts.add(dst)
        }
        val obbDir = File(dir, "obb").apply { mkdirs() }
        var obbBytes = 0L
        for (f in obbs) {
            val dst = uniqueFile(obbDir, f.name)
            move(f, dst)
            obbBytes += dst.length()
        }

        // libraries and code, from every APK
        val libDir = File(dir, "lib").apply { mkdirs() }
        val abis = sortedSetOf<String>()
        var dexCount = 0
        var libCount = 0
        var libBytes = 0L
        val vulkanLibs = ArrayList<String>()
        val dexApks = ArrayList<String>()
        val all = listOf(baseDst) + splitDsts
        for ((k, f) in all.withIndex()) {
            progress("Extracting libraries: file ${k + 1} of ${all.size}...")
            var hasDex = false
            ZipFile(f).use { zip ->
                val en = zip.entries()
                while (en.hasMoreElements()) {
                    val e = en.nextElement()
                    val name = e.name
                    if (name.startsWith("lib/") && name.endsWith(".so")) {
                        val parts = name.split("/")
                        if (parts.size == 3) {
                            abis.add(parts[1])
                            if (parts[1] == "arm64-v8a") {
                                val target = File(libDir, parts[2])
                                val refs = zip.getInputStream(e).use { i ->
                                    target.outputStream().use { o -> Util.copyPatched(i, o) }
                                }
                                libCount++
                                libBytes += target.length()
                                if (refs > 0 && !vulkanLibs.contains(parts[2])) vulkanLibs.add(parts[2])
                            }
                        }
                    } else if (name.matches(Regex("classes\\d*\\.dex"))) {
                        dexCount++
                        hasDex = true
                    }
                }
            }
            if (hasDex) dexApks.add(f.relativeTo(dir).path)
            f.setReadOnly()
        }
        File(dir, "dexapks.txt").writeText(dexApks.joinToString("\n"))
        if (libCount > 0) Util.ensureShim(ctx, libDir)

        val nativeActivity = pi.activities?.any { it.name == "android.app.NativeActivity" } == true
        val adapter = if (File(libDir, "libunity.so").exists()) "unity"
                      else if (nativeActivity) "native" else "unknown"
        File(dir, "adapter.txt").writeText(adapter)

        val sb = StringBuilder()
        sb.append("Imported: $label\n")
        sb.append("Package: $pkg\n")
        sb.append("Version: ${pi.versionName}\n")
        sb.append("Target SDK: ${ai.targetSdkVersion}\n")
        sb.append("Files: base APK + ${splitDsts.size} split(s)\n")
        for (nt in notes) sb.append(nt + "\n")
        if (obbs.isNotEmpty()) {
            sb.append("Data files (.obb): ${obbs.size} (${obbBytes / 1024 / 1024} MB)\n")
        }
        sb.append("Activities: ${pi.activities?.size ?: 0}")
        if (nativeActivity) sb.append(" (uses NativeActivity)")
        sb.append("\nJava code: $dexCount dex file(s)\n")
        sb.append("ABIs found: ${if (abis.isEmpty()) "none" else abis.joinToString(", ")}\n")
        if (!abis.contains("arm64-v8a")) {
            sb.append("No 64-bit libraries found. ")
            sb.append(if (abis.isEmpty())
                "If this is a bundle, make sure its ABI split was included.\n"
            else
                "This app is 32-bit only and can't run in this 64-bit app.\n")
        } else {
            sb.append("64-bit libs extracted: $libCount (${libBytes / 1024 / 1024} MB)\n")
            if (vulkanLibs.isEmpty()) {
                sb.append("Vulkan: no libvulkan.so reference found (likely OpenGL ES only)\n")
            } else {
                sb.append("Vulkan: yes, ${vulkanLibs.size} lib(s) reference libvulkan.so\n")
                sb.append("  " + vulkanLibs.take(6).joinToString(", ") +
                    (if (vulkanLibs.size > 6) ", ..." else "") + "\n")
                sb.append("Those references now point to the custom-driver shim.\n")
            }
        }
        sb.append("Launcher: $adapter\n")
        val report = sb.toString()
        File(dir, "report.txt").writeText(report)
        return report
    }
}
