package com.lagradost.cloudstream3

import android.app.Activity
import com.lagradost.api.Log
import java.lang.ref.WeakReference

// SCANCORE: CloudStream's activity helpers, only what plugins call. There
// is no CloudStream activity: toasts go to the log.
object CommonActivity {
    private var _activity: WeakReference<Activity>? = null
    var activity
        get() = _activity?.get()
        set(value) {
            _activity = WeakReference(value)
        }

    fun showToast(message: Int, duration: Int? = null) {
        Log.i("CloudStream", "toast $message")
    }

    fun showToast(message: String?, duration: Int? = null) {
        if (message != null) Log.i("CloudStream", message)
    }

    fun showToast(act: Activity?, message: Int, duration: Int? = null) = showToast(message, duration)

    fun showToast(act: Activity?, message: String?, duration: Int? = null) = showToast(message, duration)
}
