package com.freecrane.adrenotest

import android.content.Context
import android.system.Os

// Which GPU the game is told it is running on. The game picks its resolution limits from the GPU
// it detects, so a phone with a mid-range GPU can be presented as a flagship Adreno.
// The Vulkan shim reads LAS_GPU_* from the environment; the choice is stored in SharedPreferences
// by the main app and applied when the game process starts.
object GpuSpoof {
    class Preset(val label: String, val name: String, val device: String, val driver: Boolean)

    // device ids are the Qualcomm chip ids (vendor 0x5143)
    val presets = listOf(
        Preset("Off (real GPU)", "", "", false),
        Preset("Adreno 735", "Adreno (TM) 735", "43050B00", false),
        Preset("Adreno 740", "Adreno (TM) 740", "43050A01", false),
        Preset("Adreno 750", "Adreno (TM) 750", "43051401", false),
        Preset("Adreno 735 + Qualcomm driver identity", "Adreno (TM) 735", "43050B00", true),
        Preset("Adreno 740 + Qualcomm driver identity", "Adreno (TM) 740", "43050A01", true),
        Preset("Adreno 750 + Qualcomm driver identity", "Adreno (TM) 750", "43051401", true),
    )

    private const val PREFS = "las"
    private const val KEY = "gpu_spoof"

    fun index(ctx: Context): Int =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt(KEY, 1).coerceIn(0, presets.size - 1)

    fun set(ctx: Context, i: Int) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putInt(KEY, i).commit()
    }

    // Called in the game process before the game's libraries load.
    fun apply(ctx: Context) {
        val p = presets[index(ctx)]
        try {
            Os.setenv("LAS_GPU_NAME", p.name, true)
            Os.setenv("LAS_GPU_VENDOR", if (p.name.isEmpty()) "" else "5143", true)
            Os.setenv("LAS_GPU_DEVICE", p.device, true)
            Os.setenv("LAS_GPU_DRIVER", if (p.driver) "1" else "0", true)
            android.util.Log.i("GuestHost", "GPU spoof: ${p.label}")
        } catch (t: Throwable) {
            android.util.Log.w("GuestHost", "GPU spoof failed: $t")
        }
    }
}
