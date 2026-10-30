package com.freecrane.adrenotest

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import android.net.Uri
import java.io.File

// The games the app knows (imported ones) with the name and icon shown on the home screen.
object GameMeta {
    private fun dir(ctx: Context, pkg: String) = File(File(ctx.filesDir, "guests"), pkg)

    fun games(ctx: Context): List<String> =
        File(ctx.filesDir, "guests").listFiles { f -> f.isDirectory && File(f, "base.apk").exists() }
            ?.map { it.name }?.sortedBy { name(ctx, it).lowercase() } ?: emptyList()

    fun name(ctx: Context, pkg: String): String {
        val d = dir(ctx, pkg)
        for (n in listOf("name.txt", "label.txt")) {
            val f = File(d, n)
            if (f.exists()) { val t = try { f.readText().trim() } catch (e: Exception) { "" }; if (t.isNotEmpty()) return t }
        }
        return pkg
    }

    fun setName(ctx: Context, pkg: String, name: String) {
        val f = File(dir(ctx, pkg), "name.txt")
        if (name.isBlank()) f.delete() else f.writeText(name.trim())
    }

    fun hasCustomIcon(ctx: Context, pkg: String) = File(dir(ctx, pkg), "icon.png").exists()

    fun clearIcon(ctx: Context, pkg: String) { File(dir(ctx, pkg), "icon.png").delete() }

    // Reads the game's own name and icon out of its APK once (call from a background thread).
    fun ensureDefaults(ctx: Context, pkg: String) {
        val d = dir(ctx, pkg)
        if (File(d, "meta_tried").exists()) return
        try {
            val pm = ctx.packageManager
            val apk = File(d, "base.apk").path
            val info = pm.getPackageArchiveInfo(apk, 0)
            val ai = info?.applicationInfo
            if (ai != null) {
                ai.sourceDir = apk
                ai.publicSourceDir = apk
                try {
                    val label = ai.loadLabel(pm).toString().trim()
                    if (label.isNotEmpty() && label != ai.packageName) File(d, "label.txt").writeText(label)
                } catch (t: Throwable) { }
                try {
                    val dr = ai.loadIcon(pm)
                    val b = Bitmap.createBitmap(256, 256, Bitmap.Config.ARGB_8888)
                    val c = Canvas(b)
                    dr.setBounds(0, 0, 256, 256)
                    dr.draw(c)
                    File(d, "icon_default.png").outputStream().use { b.compress(Bitmap.CompressFormat.PNG, 100, it) }
                } catch (t: Throwable) { }
            }
        } catch (t: Throwable) { }
        try { File(d, "meta_tried").writeText("1") } catch (t: Throwable) { }
    }

    // The icon to show: the picked picture, else the game's own, else a coloured letter.
    fun icon(ctx: Context, pkg: String, size: Int = 256): Bitmap {
        val d = dir(ctx, pkg)
        for (n in listOf("icon.png", "icon_default.png")) {
            val f = File(d, n)
            if (f.exists()) {
                val b = try { BitmapFactory.decodeFile(f.path) } catch (t: Throwable) { null }
                if (b != null) return if (b.width == size) b else Bitmap.createScaledBitmap(b, size, size, true)
            }
        }
        return placeholder(name(ctx, pkg), pkg, size)
    }

    fun placeholder(name: String, seed: String, size: Int): Bitmap {
        val b = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val c = Canvas(b)
        val hue = ((seed.hashCode() and 0x7fffffff) % 360).toFloat()
        c.drawColor(android.graphics.Color.HSVToColor(floatArrayOf(hue, 0.55f, 0.55f)))
        val p = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = android.graphics.Color.WHITE; textSize = size * 0.5f; textAlign = Paint.Align.CENTER; typeface = Typeface.DEFAULT_BOLD
        }
        val ch = name.trim().firstOrNull { it.isLetterOrDigit() }?.uppercaseChar()?.toString() ?: "?"
        c.drawText(ch, size / 2f, size / 2f - (p.descent() + p.ascent()) / 2f, p)
        return b
    }

    // Saves a picture picked by the user as the game's icon (cropped to a square, 256 x 256).
    fun setIconFrom(ctx: Context, pkg: String, uri: Uri): Boolean {
        return try {
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            ctx.contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, bounds) }
            var sample = 1
            while (bounds.outWidth / (sample * 2) >= 512 && bounds.outHeight / (sample * 2) >= 512) sample *= 2
            val src = ctx.contentResolver.openInputStream(uri)?.use {
                BitmapFactory.decodeStream(it, null, BitmapFactory.Options().apply { inSampleSize = sample })
            } ?: return false
            val side = minOf(src.width, src.height)
            val sq = Bitmap.createBitmap(src, (src.width - side) / 2, (src.height - side) / 2, side, side)
            val out = Bitmap.createScaledBitmap(sq, 256, 256, true)
            File(dir(ctx, pkg), "icon.png").outputStream().use { out.compress(Bitmap.CompressFormat.PNG, 100, it) }
            true
        } catch (t: Throwable) { false }
    }
}
