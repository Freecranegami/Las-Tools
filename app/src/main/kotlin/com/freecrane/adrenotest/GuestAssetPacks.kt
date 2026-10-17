package com.freecrane.adrenotest

import android.net.Uri
import android.util.Log
import java.io.File
import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Method
import java.lang.reflect.Modifier
import java.lang.reflect.Proxy
import java.util.zip.ZipFile

// The game keeps its data in asset packs (data0, data1, game0 ...). Here they are splits that are
// already on the phone. This wraps the game's pack manager so it never talks to the Play Core
// service (which is unavailable in the host). Local packs are answered from disk, and their
// assets are unpacked into a real folder that getPackLocation points at.
object GuestAssetPacks {
    private const val TAG = "GuestHost"
    private const val PKG = "com.google.android.play.core.assetpacks."
    private var dumped = false

    private val proxies = java.util.IdentityHashMap<Any, Any>()

    // Called from the native hook when the game's native code calls fetch / getPackStates / getPackLocation
    // on the real pack manager. Packs that are on the phone are answered here; null means "go ahead as normal".
    @JvmStatic
    fun localCall(real: Any, name: String, arg: Any?): Any? {
        return try {
            if (real is Proxy) return null
            val proxy = synchronized(proxies) { proxies.getOrPut(real) { doWrap(real) } }
            if (proxy === real) return null
            val ld = GuestRuntime.current?.loader ?: real.javaClass.classLoader
            val mgr = Class.forName(PKG + "AssetPackManager", false, ld)
            val m = mgr.methods.firstOrNull { it.name == name && it.parameterCount == 1 } ?: return null
            try { m.invoke(proxy, arg) } catch (e: InvocationTargetException) { throw e.targetException }
        } catch (t: Throwable) {
            Log.w(TAG, "asset packs: local $name failed: $t")
            null
        }
    }

    @JvmStatic
    fun wrap(real: Any): Any {
        return try {
            doWrap(real)
        } catch (t: Throwable) {
            Log.w(TAG, "asset packs: wrap failed: $t")
            real
        }
    }

    private fun sig(m: Method) = (if (Modifier.isStatic(m.modifiers)) "static " else "") +
        m.returnType.simpleName + " " + m.name + "(" +
        m.parameterTypes.joinToString(",") { it.simpleName } + ")"

    private fun dump(ld: ClassLoader) {
        if (dumped) return
        dumped = true
        for (n in listOf("AssetPackManager", "AssetPackLocation", "AssetPackState", "AssetPackStates")) {
            try {
                val c = Class.forName(PKG + n, false, ld)
                Log.i(TAG, "asset packs: $n: " + c.declaredMethods.joinToString("; ") { sig(it) })
            } catch (t: Throwable) {
                Log.w(TAG, "asset packs: no class $n: $t")
            }
        }
    }

    // A split APK's split name: the part after the split_ / split- prefix, without .apk.
    // e.g. split_data0-master.apk -> data0-master (matches ApplicationInfo.splitNames).
    private fun rawName(f: File): String =
        f.name.removeSuffix(".apk").removePrefix("split_").removePrefix("split-")

    // A split's canonical asset-pack name: the part before the first '-' qualifier.
    // e.g. data0-master, data0-arm64_v8a -> data0 (the name the game asks for).
    private fun canonicalName(f: File): String = rawName(f).substringBefore('-')

    private fun doWrap(real: Any): Any {
        val g = GuestRuntime.current ?: return real
        val ld = g.loader
        dump(ld)
        val mgr = Class.forName(PKG + "AssetPackManager", false, ld)
        val root = g.apk.parentFile

        // Map every local asset pack (by both canonical and raw split name) to the split files
        // that hold it, plus the total on-disk size. Matching on both lets the game find a pack
        // whether it asks for "data0" or "data0-master".
        val packs = HashMap<String, MutableList<File>>()
        val sizes = HashMap<String, Long>()
        for (f in Util.guestSplits(root)) {
            for (key in linkedSetOf(canonicalName(f), rawName(f))) {
                packs.getOrPut(key) { ArrayList() }.add(f)
                sizes[key] = (sizes[key] ?: 0L) + f.length()
            }
        }
        val local = packs.keys
        Log.i(TAG, "asset packs: wrapping the manager, local packs=$local")

        return Proxy.newProxyInstance(ld, arrayOf(mgr)) { _, m, args ->
            val a: Array<Any?> = args ?: emptyArray()
            Log.i(TAG, "asset packs: call ${m.name}(" + a.joinToString(",") { describe(it) } + ")")
            try {
                val mine = answer(ld, m, a, local, sizes, packs, root)
                if (mine != null) {
                    Log.i(TAG, "asset packs: ${m.name} answered from the local packs")
                    return@newProxyInstance mine
                }
            } catch (t: Throwable) {
                Log.w(TAG, "asset packs: local answer for ${m.name} failed: $t")
            }
            try {
                m.invoke(real, *a)
            } catch (e: InvocationTargetException) {
                throw e.targetException
            }
        }
    }

    private fun describe(o: Any?): String = when (o) {
        null -> "null"
        is Collection<*> -> o.joinToString(",", "[", "]")
        is String, is Number, is Boolean -> o.toString()
        else -> o.javaClass.simpleName
    }

    private fun answer(ld: ClassLoader, m: Method, a: Array<Any?>, local: Set<String>,
                      sizes: Map<String, Long>, packs: Map<String, List<File>>, root: File): Any? {
        when (m.name) {
            "getPackStates", "fetch" -> {
                val names = (a.firstOrNull() as? Collection<*>)?.map { it.toString() } ?: return null
                val mine = names.filter { it in local }
                if (mine.isEmpty()) return null
                for (n in mine) ensureExtracted(n, root, packs[n] ?: emptyList())
                val states = statesFor(ld, mine, sizes) ?: return null
                return task(ld, states)
            }
            "getPackLocation" -> {
                val n = a.firstOrNull() as? String ?: return null
                if (n !in local) return null
                return locationFor(ld, n, packs[n] ?: emptyList(), root)
            }
        }
        return null
    }

    private fun task(ld: ClassLoader, value: Any): Any? {
        val tasks = Class.forName("com.google.android.gms.tasks.Tasks", true, ld)
        return tasks.getMethod("forResult", Any::class.java).invoke(null, value)
    }

    private fun statesFor(ld: ClassLoader, names: List<String>, sizes: Map<String, Long>): Any? {
        val stateCls = Class.forName(PKG + "AssetPackState", false, ld)
        val statesCls = Class.forName(PKG + "AssetPackStates", false, ld)
        val mk = stateCls.declaredMethods.filter {
            Modifier.isStatic(it.modifiers) && stateCls.isAssignableFrom(it.returnType) &&
                it.parameterTypes.firstOrNull() == String::class.java
        }.maxByOrNull { it.parameterCount } ?: return null
        mk.isAccessible = true
        val map = HashMap<String, Any?>()
        var total = 0L
        for (n in names) {
            val size = sizes[n] ?: 0L
            total += size
            val ints = intArrayOf(4, 0, 100, 0)
            var ii = 0
            var li = 0
            var si = 0
            val args = mk.parameterTypes.map { t ->
                when (t) {
                    String::class.java -> if (si++ == 0) n else ""
                    Int::class.javaPrimitiveType -> ints.getOrElse(ii++) { 0 }
                    Long::class.javaPrimitiveType -> if (li++ < 2) size else 0L
                    Double::class.javaPrimitiveType -> 1.0   // transfer progress: finished
                    Float::class.javaPrimitiveType -> 1.0f
                    Boolean::class.javaPrimitiveType -> false
                    else -> null
                }
            }
            map[n] = mk.invoke(null, *args.toTypedArray())
        }
        fun fill(types: Array<Class<*>>): Array<Any?> = types.map { t ->
            when {
                Map::class.java.isAssignableFrom(t) -> map
                t == Long::class.javaPrimitiveType -> total
                t == Int::class.javaPrimitiveType -> 0
                t == Double::class.javaPrimitiveType -> 1.0
                t == Float::class.javaPrimitiveType -> 1.0f
                t == Boolean::class.javaPrimitiveType -> false
                else -> null
            }
        }.toTypedArray()

        // 1. a static factory that takes the map
        val mk2 = statesCls.declaredMethods.firstOrNull {
            Modifier.isStatic(it.modifiers) && statesCls.isAssignableFrom(it.returnType) &&
                it.parameterTypes.any { p -> Map::class.java.isAssignableFrom(p) }
        }
        if (mk2 != null) {
            mk2.isAccessible = true
            return mk2.invoke(null, *fill(mk2.parameterTypes))
        }
        // 2. a concrete subclass of AssetPackStates in the game's own Play Core, built through its constructor
        val impl = findStatesImpl(ld, statesCls)
        val ctor = impl?.declaredConstructors?.firstOrNull { c -> c.parameterTypes.any { Map::class.java.isAssignableFrom(it) } }
        if (ctor != null) {
            ctor.isAccessible = true
            return ctor.newInstance(*fill(ctor.parameterTypes))
        }
        // 3. nothing fits: say what is there, so the next attempt can use the real names
        Log.w(TAG, "asset packs: no way to build AssetPackStates. methods: " +
            statesCls.declaredMethods.joinToString("; ") { sig(it) })
        Log.w(TAG, "asset packs: AssetPackStates constructors: " +
            statesCls.declaredConstructors.joinToString("; ") { c -> c.parameterTypes.joinToString(",") { it.simpleName } } +
            "; subclass=" + (impl?.name ?: "none found"))
        return null
    }

    private val implCache = HashMap<Class<*>, Class<*>?>()
    @Volatile private var rootDir: File? = null
    private val locCache = HashMap<String, Any>()

    // The scan walks the whole game dex (about half a second), so do it once per class.
    private fun findStatesImpl(ld: ClassLoader, statesCls: Class<*>): Class<*>? {
        synchronized(implCache) {
            if (implCache.containsKey(statesCls)) return implCache[statesCls]
            val r = scanImpl(ld, statesCls)
            implCache[statesCls] = r
            return r
        }
    }

    // Build every answer before the game starts, so the game's main thread never waits on us.
    @JvmStatic
    fun prewarm(ld: ClassLoader, root: File) {
        try {
            rootDir = root
            val t0 = System.currentTimeMillis()
            val packs = LinkedHashMap<String, MutableList<File>>()
            for (f in Util.guestSplits(root)) packs.getOrPut(canonicalName(f)) { ArrayList() }.add(f)
            for ((n, fs) in packs) locationFor(ld, n, fs, root)
            val st = Class.forName(PKG + "AssetPackStates", false, ld)
            findStatesImpl(ld, st)
            Log.i(TAG, "asset packs: answers prepared in ${System.currentTimeMillis() - t0} ms")
        } catch (t: Throwable) {
            Log.w(TAG, "asset packs: prewarm failed: $t")
        }
    }

    private fun scanImpl(ld: ClassLoader, statesCls: Class<*>): Class<*>? {
        val root = rootDir ?: GuestRuntime.current?.apk?.parentFile ?: return null
        for (path in Util.guestDexPath(root).split(':')) {
            try {
                @Suppress("DEPRECATION")
                val df = dalvik.system.DexFile(path)
                val en = df.entries()
                while (en.hasMoreElements()) {
                    val n = en.nextElement()
                    if (!n.startsWith(PKG)) continue
                    val c = try { Class.forName(n, false, ld) } catch (t: Throwable) { continue }
                    if (c != statesCls && statesCls.isAssignableFrom(c) && !Modifier.isAbstract(c.modifiers)) return c
                }
            } catch (t: Throwable) {
                Log.w(TAG, "asset packs: scanning $path failed: $t")
            }
        }
        return null
    }

    // Unpack the split APK(s) of an asset pack into a plain folder (guests/<pkg>/packs/<name>/),
    // so the game can read its data with ordinary file paths. Done lazily once per pack; a marker
    // file skips re-extraction until the game is re-imported (which wipes this folder).
    // Unpack every pack up front (before the game boots), so no game thread ever waits on one.
    @JvmStatic
    fun extractAll(root: File) {
        val packs = LinkedHashMap<String, MutableList<File>>()
        for (f in Util.guestSplits(root)) packs.getOrPut(canonicalName(f)) { ArrayList() }.add(f)
        Log.i(TAG, "asset packs: unpacking all before launch: " + packs.keys)
        for ((n, fs) in packs) ensureExtracted(n, root, fs)
        Log.i(TAG, "asset packs: all packs unpacked")
    }

    @Synchronized
    private fun ensureExtracted(name: String, root: File, splits: List<File>): File? {
        if (splits.isEmpty()) return null
        val dir = File(root, "packs/" + name.substringBefore('-'))
        val marker = File(dir, ".las_extracted")
        if (marker.exists()) return dir
        Log.i(TAG, "asset packs: extracting $name (${splits.size} split(s)) -> ${dir.path}")
        var ok = true
        for (f in splits) {
            try {
                ZipFile(f).use { z ->
                    val en = z.entries()
                    while (en.hasMoreElements()) {
                        val e = en.nextElement()
                        val n = e.name
                        if (!n.startsWith("assets/") || e.isDirectory) continue
                        val out = File(dir, n.removePrefix("assets/"))
                        out.parentFile.mkdirs()
                        z.getInputStream(e).use { i -> out.outputStream().use { o -> i.copyTo(o, 1 shl 16) } }
                    }
                }
            } catch (t: Throwable) {
                Log.w(TAG, "asset packs: extract ${f.name} failed: $t")
                ok = false
            }
        }
        if (ok) {
            marker.writeText(System.currentTimeMillis().toString())
            Log.i(TAG, "asset packs: extracted $name to ${dir.path}")
        } else {
            Log.w(TAG, "asset packs: extraction of $name incomplete")
        }
        return if (ok) dir else null
    }

    // Build a real AssetPackLocation pointing at the unpacked pack folder. Reflection is used so
    // the correct static factory is picked regardless of the Play Core version the game ships.
    // Flags are filled in (isAssetFile, isUnpacked) order: false, true -> an unpacked directory.
    private fun locationFor(ld: ClassLoader, name: String, splits: List<File>, root: File): Any? {
        synchronized(locCache) { locCache[name]?.let { return it } }
        val r = buildLocation(ld, name, splits, root)
        if (r != null) synchronized(locCache) { locCache[name] = r }
        return r
    }

    private fun buildLocation(ld: ClassLoader, name: String, splits: List<File>, root: File): Any? {
        val c = Class.forName(PKG + "AssetPackLocation", false, ld)
        val folder = ensureExtracted(name, root, splits) ?: return null
        val cands = c.declaredMethods.filter {
            Modifier.isStatic(it.modifiers) && c.isAssignableFrom(it.returnType)
        }
        Log.i(TAG, "asset packs: location factories: " + cands.joinToString("; ") { sig(it) })
        val fileFactory = cands.firstOrNull { it.parameterTypes.getOrNull(0) == File::class.java }
        val uriFactory = cands.firstOrNull { it.parameterTypes.getOrNull(0) == Uri::class.java }
        return try {
            when {
                fileFactory != null -> {
                    val flags = booleanArrayOf(false, true)
                    var bi = 0
                    val args = fileFactory.parameterTypes.map { t ->
                        when (t) {
                            File::class.java -> folder
                            Boolean::class.javaPrimitiveType -> flags.getOrElse(bi++) { false }
                            else -> null
                        }
                    }.toTypedArray()
                    Log.i(TAG, "asset packs: location $name -> File(${folder.path}) flags=(false,true)")
                    fileFactory.invoke(null, *args)
                }
                uriFactory != null -> {
                    val flags = booleanArrayOf(false, true)
                    var bi = 0
                    val args = uriFactory.parameterTypes.map { t ->
                        when (t) {
                            Uri::class.java -> Uri.fromFile(folder)
                            Boolean::class.javaPrimitiveType -> flags.getOrElse(bi++) { false }
                            else -> null
                        }
                    }.toTypedArray()
                    Log.i(TAG, "asset packs: location $name -> Uri(${folder.toURI()}) flags=(false,true)")
                    uriFactory.invoke(null, *args)
                }
                else -> {
                    // the concrete subclass in the game's own Play Core: (int storageMethod, String path, String assetsPath)
                    val impl = findStatesImpl(ld, c)
                    val ctor = impl?.declaredConstructors?.firstOrNull { k -> k.parameterTypes.any { it == String::class.java } }
                    if (ctor == null) {
                        Log.w(TAG, "asset packs: no way to build AssetPackLocation for $name; subclass=" + (impl?.name ?: "none") +
                            " ctors=" + (impl?.declaredConstructors?.joinToString("; ") { k -> k.parameterTypes.joinToString(",") { it.simpleName } } ?: "-"))
                        null
                    } else {
                        ctor.isAccessible = true
                        val args = ctor.parameterTypes.map { t ->
                            when (t) {
                                String::class.java -> folder.path
                                Int::class.javaPrimitiveType -> 0
                                Boolean::class.javaPrimitiveType -> false
                                Long::class.javaPrimitiveType -> 0L
                                else -> null
                            }
                        }.toTypedArray()
                        Log.i(TAG, "asset packs: location $name -> ${impl.name}(${folder.path}) storage=0")
                        ctor.newInstance(*args)
                    }
                }
            }
        } catch (t: Throwable) {
            Log.w(TAG, "asset packs: build location for $name failed: $t")
            null
        }
    }
}