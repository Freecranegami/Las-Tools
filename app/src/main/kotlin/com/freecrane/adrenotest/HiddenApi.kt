package com.freecrane.adrenotest

import android.util.Log
import java.lang.reflect.Method

// Lets the game process use the few framework internals the compatibility layer needs
// (swapping an activity's base context and resources). Best effort: on most devices these
// fields are reachable anyway because the app targets an old SDK level.
object HiddenApi {
    fun exemptAll() {
        try {
            val forName = Class::class.java.getDeclaredMethod("forName", String::class.java)
            val getDeclaredMethod = Class::class.java.getDeclaredMethod(
                "getDeclaredMethod", String::class.java, arrayOf<Class<*>>()::class.java)
            val vmRuntime = forName.invoke(null, "dalvik.system.VMRuntime") as Class<*>
            val getRuntime = getDeclaredMethod.invoke(vmRuntime, "getRuntime", null) as Method
            val setExemptions = getDeclaredMethod.invoke(
                vmRuntime, "setHiddenApiExemptions",
                arrayOf<Class<*>>(Array<String>::class.java)) as Method
            setExemptions.invoke(getRuntime.invoke(null), arrayOf("L"))
            Log.i("GuestHost", "hidden API exemptions set")
        } catch (t: Throwable) {
            Log.w("GuestHost", "hidden API exemptions not available: $t")
        }
    }
}
