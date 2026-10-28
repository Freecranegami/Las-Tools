package com.freecrane.adrenotest

import android.app.Activity
import android.app.AlertDialog
import android.content.ClipData
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.text.InputType
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import java.io.File
import java.text.DateFormat
import java.util.Date

// A small file manager for the app's own storage (files/), where the game's data lives: copy, cut, paste, delete, rename,
// new folder/file, import files from the phone, export a file out, replace a file with one from the phone, view a text file.
// Editing is done outside (export, edit, replace). Changes take effect the next time the game starts.
class FileManagerActivity : Activity() {
    private lateinit var root: File
    private lateinit var cwd: File
    private val selected = LinkedHashSet<File>()
    private var clip: List<File> = emptyList()
    private var clipCut = false
    private var entries: List<File> = emptyList()

    private lateinit var listView: ListView
    private lateinit var pathText: TextView
    private lateinit var statusText: TextView
    private lateinit var pasteBtn: Button
    private val adapter = Adapter()

    private var pendingFile: File? = null
    private var pendingBackup = true

    private val reqExport = 2001
    private val reqReplace = 2002
    private val reqImport = 2003

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    private fun btn(label: String, onClick: () -> Unit): Button = Button(this).apply {
        text = label
        isAllCaps = false
        textSize = 12f
        setOnClickListener { onClick() }
    }

    private fun row(vararg b: Button): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        for (x in b) addView(x, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        title = "File manager"
        root = filesDir.canonicalFile
        cwd = root
        startDir()

        pathText = TextView(this).apply { textSize = 13f; setPadding(dp(8), dp(6), dp(8), 0) }
        statusText = TextView(this).apply { textSize = 12f; setPadding(dp(8), 0, dp(8), dp(4)) }
        listView = ListView(this).apply {
            adapter = this@FileManagerActivity.adapter
            setOnItemClickListener { _, _, pos, _ -> onItemClick(entries[pos]) }
            setOnItemLongClickListener { _, _, pos, _ -> toggle(entries[pos]); true }
        }
        pasteBtn = btn("Paste") { paste() }

        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(row(btn("Up") { up() }, pasteBtn, btn("New folder") { newItem(true) }, btn("New file") { newItem(false) }))
            addView(row(btn("Copy") { setClip(false) }, btn("Cut") { setClip(true) }, btn("Delete") { deleteSelected() }, btn("Select all") { selectAll() }))
            addView(row(btn("App files") { go(root) }, btn("Game files") { go(gameDir("ext/data/files", true)) },
                btn("Asset packs") { go(gameDir("packs", false)) }, btn("Import here") { importHere() }))
        }
        val rootView = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(bar)
            addView(pathText)
            addView(statusText)
            addView(listView, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        setContentView(rootView)
        refresh()
    }

    // starts in the game's own data folder when it exists
    private fun startDir() {
        val d = gameDir("ext/data/files", false)
        if (d != null && d.isDirectory) cwd = d
    }

    private fun gameDir(sub: String, create: Boolean): File? {
        val pkg = File(root, "guests").listFiles()?.filter { it.isDirectory }?.minByOrNull { it.name } ?: return null
        val d = File(pkg, sub)
        if (create) d.mkdirs()
        return if (d.isDirectory) d else null
    }

    // ---------- navigation ----------

    private fun inside(f: File): Boolean {
        return try {
            val c = f.canonicalPath
            c == root.path || c.startsWith(root.path + File.separator)
        } catch (t: Throwable) { false }
    }

    private fun go(d: File?) {
        if (d == null || !d.isDirectory || !inside(d)) { toast("That folder doesn't exist yet."); return }
        cwd = d.canonicalFile
        selected.clear()
        refresh()
    }

    private fun up() {
        if (cwd.path == root.path) { toast("This is the top of the app's storage."); return }
        go(cwd.parentFile)
    }

    private fun refresh() {
        val all = cwd.listFiles()?.toList() ?: emptyList()
        entries = all.sortedWith(compareBy<File>({ !it.isDirectory }, { it.name.lowercase() }))
        selected.retainAll(entries.toSet())
        val rel = if (cwd.path == root.path) "files/" else "files/" + cwd.path.removePrefix(root.path + File.separator) + "/"
        pathText.text = rel
        statusText.text = "${entries.size} items" + (if (selected.isNotEmpty()) " - ${selected.size} selected" else "") +
            (if (clip.isNotEmpty()) " - clipboard: ${clip.size} (${if (clipCut) "cut" else "copy"})" else "") +
            "   (long-press to select)"
        pasteBtn.text = if (clip.isEmpty()) "Paste" else "Paste (${clip.size})"
        adapter.notifyDataSetChanged()
    }

    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_SHORT).show()

    @Suppress("OVERRIDE_DEPRECATION")
    override fun onBackPressed() {
        when {
            selected.isNotEmpty() -> { selected.clear(); refresh() }
            cwd.path != root.path -> up()
            else -> super.onBackPressed()
        }
    }

    private fun toggle(f: File) { if (!selected.add(f)) selected.remove(f); refresh() }
    private fun selectAll() { if (selected.size == entries.size) selected.clear() else { selected.clear(); selected.addAll(entries) }; refresh() }

    private fun onItemClick(f: File) {
        when {
            selected.isNotEmpty() -> toggle(f)
            f.isDirectory -> go(f)
            else -> fileActions(f)
        }
    }

    // ---------- list ----------

    private inner class Adapter : BaseAdapter() {
        override fun getCount() = entries.size
        override fun getItem(position: Int): Any = entries[position]
        override fun getItemId(position: Int) = position.toLong()
        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val f = entries[position]
            val name = TextView(this@FileManagerActivity).apply {
                textSize = 15f
                text = if (f.isDirectory) f.name + "/" else f.name
                if (f.isDirectory) setTypeface(typeface, android.graphics.Typeface.BOLD)
            }
            val info = TextView(this@FileManagerActivity).apply {
                textSize = 11f
                val date = DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(Date(f.lastModified()))
                text = (if (f.isDirectory) "${f.list()?.size ?: 0} items" else human(f.length())) + "  -  " + date
            }
            return LinearLayout(this@FileManagerActivity).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(12), dp(8), dp(12), dp(8))
                if (f in selected) setBackgroundColor(0x553D7BFF)
                addView(name); addView(info)
            }
        }
    }

    private fun human(n: Long): String = when {
        n >= 1L shl 30 -> "%.2f GB".format(n / 1073741824.0)
        n >= 1L shl 20 -> "%.1f MB".format(n / 1048576.0)
        n >= 1L shl 10 -> "%.1f KB".format(n / 1024.0)
        else -> "$n B"
    }

    // ---------- file actions ----------

    private fun fileActions(f: File) {
        val items = arrayOf("View as text", "Export (save a copy to the phone)...", "Replace with a file from the phone...",
            "Copy", "Cut", "Rename", "Delete", "Info")
        AlertDialog.Builder(this).setTitle(f.name).setItems(items) { _, which ->
            when (which) {
                0 -> viewText(f)
                1 -> { pendingFile = f; startActivityForResult(Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                        .setType("*/*").putExtra(Intent.EXTRA_TITLE, f.name), reqExport) }
                2 -> replaceDialog(f)
                3 -> { clip = listOf(f); clipCut = false; refresh() }
                4 -> { clip = listOf(f); clipCut = true; refresh() }
                5 -> rename(f)
                6 -> { selected.clear(); selected.add(f); deleteSelected() }
                7 -> info(f)
            }
        }.show()
    }

    private fun viewText(f: File) {
        val text = try {
            val buf = ByteArray(minOf(f.length(), 300_000L).toInt())
            f.inputStream().use { var o = 0; while (o < buf.size) { val r = it.read(buf, o, buf.size - o); if (r < 0) break; o += r } }
            if (buf.take(4096).any { it.toInt() == 0 }) null
            else String(buf, Charsets.UTF_8) + (if (f.length() > 300_000L) "\n\n... (only the first 300 KB are shown)" else "")
        } catch (t: Throwable) { "Couldn't read it: $t" }
        if (text == null) { toast("This doesn't look like a text file."); return }
        val tv = TextView(this).apply { this.text = text; textSize = 11f; typeface = android.graphics.Typeface.MONOSPACE; setPadding(dp(12), dp(8), dp(12), dp(8)); setTextIsSelectable(true) }
        AlertDialog.Builder(this).setTitle(f.name).setView(ScrollView(this).apply { addView(tv) }).setPositiveButton("Close", null).show()
    }

    private fun info(f: File) {
        val tv = TextView(this).apply { textSize = 13f; setPadding(dp(16), dp(8), dp(16), dp(8)); setTextIsSelectable(true) }
        val base = "Path: ${f.path}\nModified: ${Date(f.lastModified())}\n"
        if (f.isDirectory) {
            tv.text = base + "Counting..."
            Thread {
                var files = 0L; var bytes = 0L
                f.walkTopDown().forEach { if (it.isFile) { files++; bytes += it.length() } }
                runOnUiThread { tv.text = base + "$files files, ${human(bytes)}" }
            }.start()
        } else tv.text = base + "Size: ${human(f.length())} (${f.length()} bytes)"
        AlertDialog.Builder(this).setTitle(f.name).setView(tv).setPositiveButton("Close", null).show()
    }

    private fun rename(f: File) {
        val box = EditText(this).apply { setText(f.name); setSelectAllOnFocus(true) }
        AlertDialog.Builder(this).setTitle("Rename").setView(box).setPositiveButton("Rename") { _, _ ->
            val n = box.text.toString().trim()
            val t = File(f.parentFile, n)
            when {
                n.isEmpty() || n.contains('/') -> toast("Not a valid name.")
                t.exists() -> toast("That name is already used.")
                f.renameTo(t) -> { selected.clear(); refresh() }
                else -> toast("Couldn't rename it.")
            }
        }.setNegativeButton("Cancel", null).show()
    }

    private fun newItem(folder: Boolean) {
        val box = EditText(this).apply { hint = if (folder) "folder name" else "file name"; inputType = InputType.TYPE_CLASS_TEXT }
        AlertDialog.Builder(this).setTitle(if (folder) "New folder" else "New empty file").setView(box).setPositiveButton("Create") { _, _ ->
            val n = box.text.toString().trim()
            val t = File(cwd, n)
            when {
                n.isEmpty() || n.contains('/') -> toast("Not a valid name.")
                t.exists() -> toast("That name is already used.")
                else -> { val ok = try { if (folder) t.mkdir() else t.createNewFile() } catch (e: Throwable) { false }
                          if (!ok) toast("Couldn't create it."); refresh() }
            }
        }.setNegativeButton("Cancel", null).show()
    }

    // ---------- copy / cut / paste / delete ----------

    private fun setClip(cut: Boolean) {
        if (selected.isEmpty()) { toast("Long-press files to select them first (or tap one for its menu)."); return }
        clip = selected.toList(); clipCut = cut; selected.clear(); refresh()
        toast("${clip.size} item(s) on the clipboard.")
    }

    private fun uniqueName(dir: File, name: String): File {
        var t = File(dir, name)
        if (!t.exists()) return t
        val dot = name.lastIndexOf('.')
        val stem = if (dot > 0) name.substring(0, dot) else name
        val ext = if (dot > 0) name.substring(dot) else ""
        var i = 1
        while (true) { t = File(dir, "$stem ($i)$ext"); if (!t.exists()) return t; i++ }
    }

    // 0 = replace, 1 = keep both, 2 = skip
    private fun askConflict(count: Int, then: (Int) -> Unit) {
        AlertDialog.Builder(this).setTitle("$count item(s) already exist here")
            .setItems(arrayOf("Replace them", "Keep both (rename the new ones)", "Skip them")) { _, w -> then(w) }
            .setNegativeButton("Cancel", null).show()
    }

    private fun paste() {
        if (clip.isEmpty()) { toast("The clipboard is empty."); return }
        val dest = cwd
        val src = clip.filter { it.exists() }
        for (s in src) {
            if (s.isDirectory && (dest.canonicalPath == s.canonicalPath || dest.canonicalPath.startsWith(s.canonicalPath + File.separator))) {
                toast("A folder can't be pasted into itself."); return
            }
        }
        val todo = src.filterNot { clipCut && it.parentFile?.canonicalPath == dest.canonicalPath }   // cut into the same folder: nothing to do
        val clashes = todo.count { File(dest, it.name).exists() }
        if (clashes > 0) askConflict(clashes) { mode -> runPaste(todo, dest, mode) } else runPaste(todo, dest, 1)
    }

    private fun runPaste(items: List<File>, dest: File, mode: Int) {
        val cut = clipCut
        work(if (cut) "Moving..." else "Copying...") { progress, cancelled ->
            var failed = 0
            for (s in items) {
                if (cancelled()) break
                var t = File(dest, s.name)
                if (t.exists()) {
                    when (mode) {
                        0 -> deleteRecursive(t)
                        1 -> t = uniqueName(dest, s.name)
                        else -> continue
                    }
                }
                progress(s.name)
                try {
                    if (cut && s.renameTo(t)) continue
                    copyRecursive(s, t, cancelled)
                    if (cut && !cancelled()) deleteRecursive(s)
                } catch (e: Throwable) { failed++ }
            }
            runOnUiThread {
                if (cut) clip = emptyList()
                if (failed > 0) toast("$failed item(s) failed.")
                refresh()
            }
        }
    }

    private fun copyRecursive(src: File, dst: File, cancelled: () -> Boolean) {
        if (src.isDirectory) {
            dst.mkdirs()
            src.listFiles()?.forEach { if (!cancelled()) copyRecursive(it, File(dst, it.name), cancelled) }
        } else {
            src.inputStream().use { i -> dst.outputStream().use { o ->
                val buf = ByteArray(256 * 1024)
                while (true) { if (cancelled()) break; val r = i.read(buf); if (r < 0) break; o.write(buf, 0, r) }
            } }
        }
    }

    private fun deleteRecursive(f: File) {
        if (f.isDirectory) f.listFiles()?.forEach { deleteRecursive(it) }
        f.delete()
    }

    private fun deleteSelected() {
        if (selected.isEmpty()) { toast("Long-press files to select them first (or tap one for its menu)."); return }
        val items = selected.toList()
        val what = if (items.size == 1) items[0].name else "${items.size} items"
        AlertDialog.Builder(this).setTitle("Delete $what?").setMessage("This can't be undone.")
            .setPositiveButton("Delete") { _, _ ->
                work("Deleting...") { progress, _ ->
                    for (f in items) { progress(f.name); deleteRecursive(f) }
                    runOnUiThread { selected.clear(); refresh() }
                }
            }.setNegativeButton("Cancel", null).show()
    }

    // runs a job on a thread with a small progress dialog
    private fun work(title: String, job: (progress: (String) -> Unit, cancelled: () -> Boolean) -> Unit) {
        val cancel = java.util.concurrent.atomic.AtomicBoolean(false)
        val dlg = AlertDialog.Builder(this).setTitle(title).setMessage("").setCancelable(false)
            .setNegativeButton("Cancel") { _, _ -> cancel.set(true) }.show()
        Thread {
            try { job({ s -> runOnUiThread { dlg.setMessage(s) } }, { cancel.get() }) }
            catch (t: Throwable) { runOnUiThread { toast("Failed: $t") } }
            runOnUiThread { try { dlg.dismiss() } catch (t: Throwable) {} ; refresh() }
        }.start()
    }

    // ---------- the phone's own storage (export / replace / import) ----------

    private fun replaceDialog(f: File) {
        val cb = CheckBox(this).apply { text = "Keep a backup (${f.name}.bak)"; isChecked = true; setPadding(dp(16), dp(8), dp(16), 0) }
        AlertDialog.Builder(this).setTitle("Replace ${f.name}").setMessage("Pick the file that should take its place.").setView(cb)
            .setPositiveButton("Pick file...") { _, _ ->
                pendingFile = f; pendingBackup = cb.isChecked
                startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), reqReplace)
            }.setNegativeButton("Cancel", null).show()
    }

    private fun importHere() {
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
            .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true), reqImport)
    }

    private fun displayName(u: Uri): String {
        try {
            contentResolver.query(u, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
                if (it.moveToFirst()) { val n = it.getString(0); if (!n.isNullOrEmpty()) return n.replace('/', '_') }
            }
        } catch (t: Throwable) {}
        return u.lastPathSegment?.substringAfterLast('/') ?: "imported"
    }

    private fun copyFromUri(u: Uri, dst: File) {
        contentResolver.openInputStream(u)!!.use { i -> dst.outputStream().use { o -> i.copyTo(o, 256 * 1024) } }
    }

    @Suppress("OVERRIDE_DEPRECATION")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK || data == null) return
        when (requestCode) {
            reqExport -> {
                val f = pendingFile; val u = data.data
                if (f == null || u == null) return
                work("Exporting...") { progress, _ ->
                    progress(f.name)
                    try {
                        contentResolver.openOutputStream(u, "w")!!.use { o -> f.inputStream().use { it.copyTo(o, 256 * 1024) } }
                        runOnUiThread { toast("Saved a copy of ${f.name}.") }
                    } catch (t: Throwable) { runOnUiThread { toast("Export failed: $t") } }
                }
            }
            reqReplace -> {
                val f = pendingFile; val u = data.data
                if (f == null || u == null) return
                val backup = pendingBackup
                work("Replacing...") { progress, _ ->
                    progress(f.name)
                    try {
                        val tmp = File(f.parentFile, f.name + ".replace-tmp")
                        copyFromUri(u, tmp)
                        if (f.exists()) {
                            val bak = File(f.parentFile, f.name + ".bak")
                            if (backup && !bak.exists()) f.renameTo(bak) else f.delete()
                        }
                        if (!tmp.renameTo(f)) throw java.io.IOException("couldn't move the new file into place")
                        runOnUiThread { toast("${f.name} replaced.") }
                    } catch (t: Throwable) { runOnUiThread { toast("Replace failed: $t") } }
                }
            }
            reqImport -> {
                val uris = ArrayList<Uri>()
                val cd: ClipData? = data.clipData
                if (cd != null) for (i in 0 until cd.itemCount) uris.add(cd.getItemAt(i).uri) else data.data?.let { uris.add(it) }
                if (uris.isEmpty()) return
                val dest = cwd
                val names = uris.map { displayName(it) }
                val clashes = names.count { File(dest, it).exists() }
                val doImport = { mode: Int ->
                    work("Importing...") { progress, _ ->
                        var failed = 0
                        for ((k, u) in uris.withIndex()) {
                            var t = File(dest, names[k])
                            if (t.exists()) when (mode) { 0 -> deleteRecursive(t); 1 -> t = uniqueName(dest, names[k]); else -> continue }
                            progress(names[k])
                            try { copyFromUri(u, t) } catch (e: Throwable) { failed++ }
                        }
                        if (failed > 0) runOnUiThread { toast("$failed file(s) failed.") }
                    }
                }
                if (clashes > 0) askConflict(clashes) { doImport(it) } else doImport(1)
            }
        }
    }
}
