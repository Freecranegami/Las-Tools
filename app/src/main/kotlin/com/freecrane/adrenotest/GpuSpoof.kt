package com.freecrane.adrenotest

import android.content.Context
import android.os.Build
import android.system.Os
import java.io.File

// Which GPU and CPU (SoC) the game is told it is running on. The game picks its resolution limits from
// the device it detects, so a phone with a mid-range chip can be presented as a flagship Snapdragon.
// Only what the game reads is changed; the real GPU and CPU still do the work.
// The native hooks and the Vulkan shim read LAS_* from the environment; the choices are stored in
// SharedPreferences by the main app and applied when the game process starts.
object GpuSpoof {
    class Preset(val label: String, val name: String, val device: String, val driver: Boolean)
    class CpuPreset(val label: String, val soc: String, val board: String, val gpuIndex: Int)

    // device ids are the Qualcomm chip ids (vendor 0x5143)
    val presets = listOf(
        Preset("Off (real GPU)", "", "", false),
        Preset("Adreno (TM) 735", "Adreno (TM) 735", "43050B00", false),
        Preset("Adreno (TM) 740", "Adreno (TM) 740", "43050A01", false),
        Preset("Adreno (TM) 750", "Adreno (TM) 750", "43051401", false),
        Preset("Adreno (TM) 735 + Qualcomm driver identity", "Adreno (TM) 735", "43050B00", true),
        Preset("Adreno (TM) 740 + Qualcomm driver identity", "Adreno (TM) 740", "43050A01", true),
        Preset("Adreno (TM) 750 + Qualcomm driver identity", "Adreno (TM) 750", "43051401", true),
        Preset("Adreno (TM) 660", "Adreno (TM) 660", "06060001", false),
        Preset("Adreno (TM) 660 + Qualcomm driver identity", "Adreno (TM) 660", "06060001", true),
    )

    val cpus = listOf(
        CpuPreset("Off (real CPU)", "", "", 0),
        CpuPreset("Snapdragon 8s Gen 3 (SM8635)", "SM8635", "crow", 1),
        CpuPreset("Snapdragon 8 Gen 2 (SM8550)", "SM8550", "kalama", 2),
        CpuPreset("Snapdragon 8 Gen 3 (SM8650)", "SM8650", "pineapple", 3),
        CpuPreset("Snapdragon 888 (SM8350)", "SM8350", "lahaina", 7),
    )

    // A whole phone model: games often pick graphics limits from a device-model list, not from the GPU.
    // Choosing one also sets the matching GPU and CPU presets.
    class DevicePreset(val label: String, val manufacturer: String, val brand: String, val model: String,
                       val device: String, val gpuIndex: Int, val cpuIndex: Int)
    val devices = listOf(
        DevicePreset("Off (real phone model)", "", "", "", "", 0, 0),
        DevicePreset("Samsung Galaxy S24 Ultra (Snapdragon 8 Gen 3, Adreno 750)", "samsung", "samsung", "SM-S928B", "e3q", 3, 3),
        DevicePreset("Samsung Galaxy S23 Ultra (Snapdragon 8 Gen 2, Adreno 740)", "samsung", "samsung", "SM-S918B", "dm3q", 2, 2),
        DevicePreset("Xiaomi Mi 11 (Snapdragon 888, Adreno 660)", "Xiaomi", "Xiaomi", "M2011K2G", "venus", 7, 4),
        DevicePreset("Samsung Galaxy S21 Ultra (Snapdragon 888, Adreno 660)", "samsung", "samsung", "SM-G998U", "p3q", 7, 4),
    )

    private const val PREFS = "las"

    fun deviceIndex(ctx: Context): Int =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt("device_spoof", 1).coerceIn(0, devices.size - 1)

    // sets the phone model and, with it, the matching GPU and CPU
    fun setDevice(ctx: Context, i: Int) {
        val d = devices[i]
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putInt("device_spoof", i).putInt("gpu_spoof", d.gpuIndex).putInt("cpu_spoof", d.cpuIndex).commit()
    }

    fun index(ctx: Context): Int =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt("gpu_spoof", devices[1].gpuIndex).coerceIn(0, presets.size - 1)

    fun set(ctx: Context, i: Int) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putInt("gpu_spoof", i).commit()
    }

    fun cpuIndex(ctx: Context): Int =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt("cpu_spoof", devices[1].cpuIndex).coerceIn(0, cpus.size - 1)

    fun setCpu(ctx: Context, i: Int) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putInt("cpu_spoof", i).commit()
    }

    private fun env(k: String, v: String) = Os.setenv(k, v, true)

    private fun setBuild(name: String, v: String) {
        try {
            val f = Build::class.java.getDeclaredField(name)
            f.isAccessible = true
            f.set(null, v)
        } catch (t: Throwable) {
            android.util.Log.w("GuestHost", "Build.$name not changed: $t")
        }
    }

    // Called in the game process before the game's libraries load.
    fun apply(ctx: Context) {
        val g = presets[index(ctx)]
        val c = cpus[cpuIndex(ctx)]
        val m = devices[deviceIndex(ctx)]
        try {
            env("LAS_MODEL", m.model)
            env("LAS_MANUFACTURER", m.manufacturer)
            env("LAS_BRAND", m.brand)
            env("LAS_DEVICE", m.device)
            if (m.model.isNotEmpty()) {
                setBuild("MODEL", m.model); setBuild("MANUFACTURER", m.manufacturer)
                setBuild("BRAND", m.brand); setBuild("DEVICE", m.device); setBuild("PRODUCT", m.device)
            }
            env("LAS_GPU_NAME", g.name)
            env("LAS_GPU_VENDOR", if (g.name.isEmpty()) "" else "5143")
            env("LAS_GPU_DEVICE", g.device)
            env("LAS_GPU_DRIVER", if (g.driver) "1" else "0")
            env("LAS_GL_RENDERER", g.name)
            env("LAS_GL_VENDOR", if (g.name.isEmpty()) "" else "Qualcomm")
            env("LAS_SOC_MODEL", c.soc)
            env("LAS_BOARD", c.board)
            env("LAS_HARDWARE", if (c.soc.isEmpty()) "" else "qcom")
            env("LAS_SOC_MFR", if (c.soc.isEmpty()) "" else "Qualcomm")

            val fake = File(ctx.filesDir, "fake").apply { mkdirs() }
            env("LAS_FAKE_DIR", fake.path)
            File(fake, "gpu_model").delete()
            File(fake, "cpuinfo").delete()
            if (g.name.isNotEmpty()) File(fake, "gpu_model").writeText(g.name + "\n")
            if (c.soc.isNotEmpty()) {
                val real = try { File("/proc/cpuinfo").readLines() } catch (t: Throwable) { emptyList() }
                val keep = real.filterNot { it.startsWith("Hardware") }
                File(fake, "cpuinfo").writeText((keep + "Hardware\t: Qualcomm Technologies, Inc ${c.soc}" + "").joinToString("\n") + "\n")
                setBuild("HARDWARE", "qcom")
                setBuild("BOARD", c.board)
                if (Build.VERSION.SDK_INT >= 31) {
                    setBuild("SOC_MODEL", c.soc)
                    setBuild("SOC_MANUFACTURER", "Qualcomm")
                }
            }
            android.util.Log.i("GuestHost", "GPU spoof: ${g.label}; CPU spoof: ${c.label}; device spoof: ${m.label} (Build.MODEL now ${Build.MODEL}, HARDWARE ${Build.HARDWARE}, BOARD ${Build.BOARD})")
        } catch (t: Throwable) {
            android.util.Log.w("GuestHost", "device spoof failed: $t")
        }
    }
}
