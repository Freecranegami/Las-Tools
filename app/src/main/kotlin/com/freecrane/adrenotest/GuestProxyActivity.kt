package com.freecrane.adrenotest

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.widget.TextView

// The activity the system is asked to start for a game. GuestFactory swaps in the game's
// real activity class, so this body only runs if that did not happen.
class GuestProxyActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.e("GuestHost", "proxy activity ran: the component factory did not take over")
        setContentView(TextView(this).apply {
            text = "The game's own activity could not be started on this Android version."
        })
    }
}
