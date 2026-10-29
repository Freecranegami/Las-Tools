package com.freecrane.adrenotest

import android.content.Context
import android.net.Uri
import java.io.File

// Lossless Scaling frame generation: the user's own Lossless.dll (from their Steam copy of Lossless Scaling)
// is stored here; the shaders are read out of it at run time. Nothing of Lossless Scaling ships with the app.
object Lsfg {
    private var loaded = false

    fun dll(ctx: Context): File = File(File(ctx.filesDir, "lossless").apply { mkdirs() }, "Lossless.dll")

    fun installed(ctx: Context): Boolean = dll(ctx).let { it.exists() && it.length() > 0 }

    fun import(ctx: Context, uri: Uri): String {
        return try {
            val dst = dll(ctx)
            val tmp = File(dst.path + ".tmp")
            ctx.contentResolver.openInputStream(uri)?.use { i -> tmp.outputStream().use { o -> i.copyTo(o, 1 shl 16) } }
                ?: return "Couldn't open that file."
            if (tmp.length() < 100_000) { tmp.delete(); return "That file is too small to be Lossless.dll." }
            dst.delete()
            tmp.renameTo(dst)
            "Lossless.dll imported (${dst.length() / 1024} KB).\n\n" + check(ctx)
        } catch (t: Throwable) {
            "Import failed: $t"
        }
    }

    fun check(ctx: Context): String {
        if (!installed(ctx)) return "No Lossless.dll imported yet."
        return try {
            if (!loaded) { System.loadLibrary("lsfgcore"); loaded = true }
            probe(dll(ctx).path)
        } catch (t: Throwable) {
            "The frame generation library could not be loaded: $t"
        }
    }

    fun delete(ctx: Context): Boolean = dll(ctx).delete()

    @JvmStatic external fun probe(path: String): String
}
