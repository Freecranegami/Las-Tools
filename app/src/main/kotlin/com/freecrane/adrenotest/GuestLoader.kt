package com.freecrane.adrenotest

import dalvik.system.DexClassLoader

// A class loader for a game. NativeActivity asks it for the library "main"; games that
// name their library differently get the right file through mainLib.
class GuestLoader(
    dexPath: String, optimizedDir: String, libraryPath: String, parent: ClassLoader?,
    private val mainLib: String?
) : DexClassLoader(dexPath, optimizedDir, libraryPath, parent) {

    override fun findLibrary(name: String?): String? {
        if (name == "main" && mainLib != null) return mainLib
        return super.findLibrary(name)
    }
}
