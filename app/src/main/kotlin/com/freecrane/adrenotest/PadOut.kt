package com.freecrane.adrenotest

import android.app.Activity
import android.content.Context
import android.os.Handler
import android.os.Parcel
import android.os.SystemClock
import android.graphics.PixelFormat
import android.os.Build
import android.util.Log
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.util.SparseArray
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ViewGroup
import java.util.WeakHashMap

// A made-up Xbox 360 controller that the game sees as plugged in. The data comes from the on-screen controller.
object PadDevice {
    const val ID = 9990
    private const val TAG = "GuestHost"
    private var dev: InputDevice? = null
    private var mgr: Any? = null
    private var devs: SparseArray<InputDevice?>? = null
    private var lock: Any? = null
    private var announced = false

    private fun setField(o: Any, name: String, v: Any?) {
        var c: Class<*>? = o.javaClass
        while (c != null) {
            try { val f = c.getDeclaredField(name); f.isAccessible = true; f.set(o, v); return } catch (_: NoSuchFieldException) { c = c.superclass }
            catch (_: Throwable) { return }
        }
    }

    private fun build(): InputDevice? {
        val ids = InputDevice.getDeviceIds()
        var real: InputDevice? = null
        for (id in ids) { real = InputDevice.getDevice(id); if (real != null) break }
        if (real == null) return null
        val p = Parcel.obtain()
        val d: InputDevice = try {
            real.writeToParcel(p, 0)
            p.setDataPosition(0)
            InputDevice.CREATOR.createFromParcel(p)
        } finally { p.recycle() }
        setField(d, "mId", ID)
        setField(d, "mControllerNumber", 1)
        setField(d, "mName", "Xbox 360 Controller")
        setField(d, "mVendorId", 0x045e)
        setField(d, "mProductId", 0x028e)
        setField(d, "mDescriptor", "las-virtual-xbox360")
        setField(d, "mIsExternal", true)
        setField(d, "mSources", InputDevice.SOURCE_KEYBOARD or InputDevice.SOURCE_GAMEPAD or InputDevice.SOURCE_JOYSTICK)
        setField(d, "mKeyboardType", InputDevice.KEYBOARD_TYPE_NON_ALPHABETIC)
        setField(d, "mHasVibrator", false)
        setField(d, "mHasMicrophone", false)
        setField(d, "mHasButtonUnderPad", false)
        setField(d, "mHasSensor", false)
        setField(d, "mHasBattery", false)
        try {   // the identifier object (vendor / product / descriptor) is a separate object in some versions
            val idf = d.javaClass.getDeclaredField("mIdentifier").apply { isAccessible = true }.get(d)
            if (idf != null) { setField(idf, "mVendorId", 0x045e); setField(idf, "mProductId", 0x028e); setField(idf, "mDescriptor", "las-virtual-xbox360") }
        } catch (_: Throwable) { }
        try {
            val ranges = d.javaClass.getDeclaredField("mMotionRanges").apply { isAccessible = true }.get(d) as MutableList<*>
            ranges.clear()
            val add = d.javaClass.getDeclaredMethod("addMotionRange", Int::class.java, Int::class.java, Float::class.java, Float::class.java,
                Float::class.java, Float::class.java, Float::class.java).apply { isAccessible = true }
            val src = InputDevice.SOURCE_JOYSTICK
            for (ax in intArrayOf(MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ, MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y))
                add.invoke(d, ax, src, -1f, 1f, if (ax == MotionEvent.AXIS_HAT_X || ax == MotionEvent.AXIS_HAT_Y) 0f else 0.08f, 0.01f, 0f)
            for (ax in intArrayOf(MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_GAS))
                add.invoke(d, ax, src, 0f, 1f, 0f, 0.01f, 0f)
        } catch (t: Throwable) { Log.w(TAG, "pad: motion ranges: $t") }
        return d
    }

    private fun manager(ctx: Context): Any? {
        try {
            val g = Class.forName("android.hardware.input.InputManagerGlobal")
            val o = g.getMethod("getInstance").invoke(null)
            if (o != null) return o
        } catch (_: Throwable) { }
        return ctx.getSystemService(Context.INPUT_SERVICE)
    }

    @Suppress("UNCHECKED_CAST")
    private fun bind(ctx: Context): Boolean {
        if (devs != null) return true
        val m = manager(ctx) ?: return false
        var c: Class<*>? = m.javaClass
        var f: java.lang.reflect.Field? = null
        while (c != null && f == null) { try { f = c.getDeclaredField("mInputDevices") } catch (_: NoSuchFieldException) { c = c.superclass } }
        if (f == null) return false
        f.isAccessible = true
        devs = f.get(m) as? SparseArray<InputDevice?>
        mgr = m
        try {
            var lc: Class<*>? = m.javaClass
            while (lc != null && lock == null) {
                try { lock = lc.getDeclaredField("mInputDevicesLock").apply { isAccessible = true }.get(m) } catch (_: NoSuchFieldException) { lc = lc.superclass }
            }
        } catch (_: Throwable) { }
        return devs != null
    }

    // Makes sure the controller is in the system's list of input devices (the system rebuilds that list when a real device comes and goes).
    fun ensure(ctx: Context): Boolean {
        try {
            if (dev == null) {
                InputDevice.getDeviceIds()                 // lets the system fill its list first
                dev = build() ?: return false
            }
            if (!bind(ctx)) { Log.w(TAG, "pad: can't reach the system's input device list"); return false }
            val arr = devs!!
            val l = lock ?: arr
            var added = false
            synchronized(l) { if (arr.indexOfKey(ID) < 0 || arr.get(ID) == null) { arr.put(ID, dev); added = true } }
            if (added) announce()
            return true
        } catch (t: Throwable) {
            Log.w(TAG, "pad: fake controller failed: $t")
            return false
        }
    }

    // Tells the game's input device listeners that a controller was plugged in.
    private fun announce() {
        try {
            val m = mgr ?: return
            var c: Class<*>? = m.javaClass
            var f: java.lang.reflect.Field? = null
            while (c != null && f == null) { try { f = c.getDeclaredField("mInputDeviceListeners") } catch (_: NoSuchFieldException) { c = c.superclass } }
            if (f == null) return
            f.isAccessible = true
            val list = f.get(m) as? List<*> ?: return
            var n = 0
            for (d in ArrayList(list)) {
                val h = d as? Handler ?: continue
                h.sendMessage(h.obtainMessage(1, ID, 0)); n++        // MSG_DEVICE_ADDED
            }
            Log.i(TAG, "pad: controller plugged in (told $n listener(s))")
        } catch (t: Throwable) { Log.w(TAG, "pad: announce failed: $t") }
    }
}

// Turns the on-screen controller's state into the key and joystick events a real Xbox 360 controller makes, and hands them to the game's activity.
class PadOut(private val act: Activity) {
    private val held = HashSet<Int>()
    private var last = FloatArray(8)
    private var downTime = SystemClock.uptimeMillis()
    private var count = 0

    private val keys = mapOf(
        "a" to KeyEvent.KEYCODE_BUTTON_A, "b" to KeyEvent.KEYCODE_BUTTON_B, "x" to KeyEvent.KEYCODE_BUTTON_X, "y" to KeyEvent.KEYCODE_BUTTON_Y,
        "lb" to KeyEvent.KEYCODE_BUTTON_L1, "rb" to KeyEvent.KEYCODE_BUTTON_R1, "l3" to KeyEvent.KEYCODE_BUTTON_THUMBL, "r3" to KeyEvent.KEYCODE_BUTTON_THUMBR,
        "start" to KeyEvent.KEYCODE_BUTTON_START, "back" to KeyEvent.KEYCODE_BUTTON_SELECT, "guide" to KeyEvent.KEYCODE_BUTTON_MODE)

    fun update(pressed: Set<String>, lx: Float, ly: Float, rx: Float, ry: Float, hx: Float, hy: Float) {
        PadDevice.ensure(HostApp.instance)
        val now = SystemClock.uptimeMillis()
        for ((id, code) in keys) {
            val down = pressed.contains(id)
            if (down && held.add(code)) key(now, KeyEvent.ACTION_DOWN, code)
            else if (!down && held.remove(code)) key(now, KeyEvent.ACTION_UP, code)
        }
        val v = floatArrayOf(lx, ly, rx, ry, if (pressed.contains("lt")) 1f else 0f, if (pressed.contains("rt")) 1f else 0f, hx, hy)
        var changed = false
        for (i in v.indices) if (abs(v[i] - last[i]) > 0.002f) changed = true
        if (!changed) return
        last = v
        motion(now, v)
    }

    private fun abs(f: Float) = if (f < 0) -f else f

    // A NativeActivity game reads its input from the system's input queue in native code, so for those the events go there.
    private val nativeRoute = act is android.app.NativeActivity
    private var statusLogged = false

    private fun checkStatus() {
        if (statusLogged || !nativeRoute) return
        statusLogged = true
        Handler(android.os.Looper.getMainLooper()).postDelayed({
            try {
                val st = Native.padStatus()
                Log.i("GuestHost", "pad: native route status: game uses the hooked input queue=${st and 1 != 0}, game took a made-up event=${st and 2 != 0}")
            } catch (t: Throwable) { Log.w("GuestHost", "pad: status failed: $t") }
        }, 4000)
    }

    private fun key(now: Long, action: Int, code: Int) {
        if (nativeRoute) {
            try { Native.padKey(action, code, PadDevice.ID) } catch (t: Throwable) { Log.w("GuestHost", "pad: native key failed: $t") }
            if (++count == 1) { Log.i("GuestHost", "pad: first event sent (native route)"); checkStatus() }
            return
        }
        val e = KeyEvent(now, now, action, code, 0, 0, PadDevice.ID, 0, 0, InputDevice.SOURCE_GAMEPAD or InputDevice.SOURCE_KEYBOARD)
        try { act.dispatchKeyEvent(e) } catch (t: Throwable) { Log.w("GuestHost", "pad: key failed: $t") }
        if (++count == 1) Log.i("GuestHost", "pad: first event sent")
    }

    private fun motion(now: Long, v: FloatArray) {
        if (nativeRoute) {
            try { Native.padAxes(PadDevice.ID, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]) } catch (t: Throwable) { Log.w("GuestHost", "pad: native motion failed: $t") }
            if (++count == 1) { Log.i("GuestHost", "pad: first event sent (native route)"); checkStatus() }
            return
        }
        val pp = MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_UNKNOWN }
        val pc = MotionEvent.PointerCoords()
        pc.setAxisValue(MotionEvent.AXIS_X, v[0]); pc.setAxisValue(MotionEvent.AXIS_Y, v[1])
        pc.setAxisValue(MotionEvent.AXIS_Z, v[2]); pc.setAxisValue(MotionEvent.AXIS_RZ, v[3])
        pc.setAxisValue(MotionEvent.AXIS_LTRIGGER, v[4]); pc.setAxisValue(MotionEvent.AXIS_BRAKE, v[4])
        pc.setAxisValue(MotionEvent.AXIS_RTRIGGER, v[5]); pc.setAxisValue(MotionEvent.AXIS_GAS, v[5])
        pc.setAxisValue(MotionEvent.AXIS_HAT_X, v[6]); pc.setAxisValue(MotionEvent.AXIS_HAT_Y, v[7])
        val e = MotionEvent.obtain(downTime, now, MotionEvent.ACTION_MOVE, 1, arrayOf(pp), arrayOf(pc), 0, 0, 1f, 1f,
            PadDevice.ID, 0, InputDevice.SOURCE_JOYSTICK, 0)
        try { act.dispatchGenericMotionEvent(e) } catch (t: Throwable) { Log.w("GuestHost", "pad: motion failed: $t") }
        e.recycle()
        if (++count == 1) Log.i("GuestHost", "pad: first event sent")
    }
}

// Puts the controller on top of a game's activity. It is a window of its own, not a view inside the game's window: a native game
// owns its window's surface, so nothing added to that window would ever be drawn.
object PadOverlay {
    private val views = WeakHashMap<Activity, PadView>()

    fun attach(a: Activity) {
        try {
            if (!Pad.on(HostApp.instance) || views.containsKey(a)) return
            val ok = PadDevice.ensure(HostApp.instance)
            val v = PadView(a, false, PadOut(a))
            views[a] = v
            val decor = a.window.decorView
            val add = Runnable {
                try {
                    val lp = WindowManager.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT,
                        WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
                            WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS or WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED,
                        PixelFormat.TRANSLUCENT)
                    lp.token = decor.windowToken
                    lp.gravity = Gravity.TOP or Gravity.START
                    lp.title = "LasPad"
                    if (Build.VERSION.SDK_INT >= 28) lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                    a.windowManager.addView(v, lp)
                    Log.i("GuestHost", "pad: on-screen controller added (${a.javaClass.simpleName}), fake Xbox 360 device ${if (ok) "registered" else "NOT registered"}")
                } catch (t: Throwable) {
                    Log.w("GuestHost", "pad: couldn't add the controller window: $t")
                }
            }
            if (decor.windowToken != null) add.run()
            else decor.addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
                override fun onViewAttachedToWindow(x: View) { decor.removeOnAttachStateChangeListener(this); decor.post(add) }
                override fun onViewDetachedFromWindow(x: View) { }
            })
        } catch (t: Throwable) {
            Log.w("GuestHost", "pad: couldn't add the controller: $t")
        }
    }

    fun detach(a: Activity) {
        val v = views.remove(a) ?: return
        try { a.windowManager.removeViewImmediate(v) } catch (_: Throwable) { }
    }
}
