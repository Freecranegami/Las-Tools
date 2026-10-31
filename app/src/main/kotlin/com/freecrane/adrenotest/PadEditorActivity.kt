package com.freecrane.adrenotest

import android.app.Activity
import android.os.Bundle
import android.view.View
import android.view.WindowManager

// Full-screen editor for the on-screen controller's layout (for the defaults or for one game).
class PadEditorActivity : Activity() {
    private var view: PadView? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Prefs.scope = intent.getStringExtra("pkg")
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        val v = PadView(this, true, null)
        v.onDone = { finish() }
        view = v
        setContentView(v)
    }

    override fun onPause() {
        view?.save()
        super.onPause()
    }
}
