package dev.espmaps.nav

import android.app.Notification
import android.content.ComponentName
import android.content.Context
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log

/**
 * Reads Google Maps' ongoing navigation notification so the ESP32 shows turn
 * guidance the moment you start a trip - no destination typing.
 *
 * Why a notification listener rather than something cleaner: Google Maps does
 * not expose its navigation state to third-party apps at all. No API, no
 * broadcast, no content provider. The ongoing notification is the only
 * machine-readable surface, which is why every "Maps on a smartwatch/HUD"
 * project works this way.
 *
 * What this yields: maneuver, distance to it, road name, remaining distance
 * and arrival time. What it does NOT yield: the route polyline. That is not
 * present in any form, so the route line has to be computed separately.
 *
 * Parsing lives in GuidanceParser; see docs/MAPS_NOTIFICATION.md for the raw
 * captures it was written against.
 */
class MapsNavListener : NotificationListenerService() {

    companion object {
        private const val TAG = "MapsNav"
        const val MAPS_PKG = "com.google.android.apps.maps"

        /** Logs every field of every Maps notification. Very noisy. */
        var dumpNotifications = false

        fun isEnabled(ctx: Context): Boolean {
            val flat = Settings.Secure.getString(
                ctx.contentResolver, "enabled_notification_listeners"
            ) ?: return false
            val me = ComponentName(ctx, MapsNavListener::class.java)
            return flat.split(":").any { ComponentName.unflattenFromString(it) == me }
        }
    }

    // Maps posts several notifications per second, sometimes 3-4 within the
    // same millisecond. Forwarding each one would swamp the BLE link for no
    // benefit, so only meaningful changes are published.
    private var lastKey: String? = null

    override fun onListenerConnected() {
        Log.i(TAG, "notification access granted, watching $MAPS_PKG")
        MapsNav.listenerConnected = true
    }

    override fun onListenerDisconnected() {
        MapsNav.listenerConnected = false
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        if (sbn.packageName != MAPS_PKG) return
        val n = sbn.notification ?: return
        val x = n.extras ?: return

        if (dumpNotifications) dump(sbn, n, x)

        // Maps also posts transient notifications (traffic alerts, arrival
        // toasts); only the ongoing one carries navigation state.
        if (n.flags and Notification.FLAG_ONGOING_EVENT == 0) return

        val g = GuidanceParser.parse(x) ?: return

        // Round the volatile fields so small jitter doesn't trigger a resend.
        val key = "${g.maneuver}|${g.distanceM / 10}|${g.instruction}|" +
                  "${g.remainingM / 25}|${g.remainingS / 30}"
        if (key == lastKey) return
        lastKey = key

        Log.i(TAG, "guidance: ${g.instruction} in ${g.distanceM} m " +
                   "(maneuver ${g.maneuver}), ${g.remainingM} m / ${g.remainingS} s left")
        MapsNav.publish(g)
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        if (sbn.packageName != MAPS_PKG) return
        Log.i(TAG, "Maps navigation ended")
        lastKey = null
        GuidanceParser.reset()
        MapsNav.clear()
    }

    private fun dump(sbn: StatusBarNotification, n: Notification, x: android.os.Bundle) {
        Log.i(TAG, "--- Maps notification (id=${sbn.id}, ongoing=${
            n.flags and Notification.FLAG_ONGOING_EVENT != 0}) ---")
        for (key in x.keySet()) {
            val v = x.get(key)
            if (v is CharSequence || v is Number || v is Boolean) {
                Log.i(TAG, "  extra[$key] = $v")
            }
        }
    }
}

/** Latest guidance from Google Maps, shared with NavService. */
object MapsNav {

    data class Guidance(
        /** enum from dev.espmaps.ble.Maneuver */
        val maneuver: Int,
        /** metres to the next maneuver */
        val distanceM: Int,
        /** road name, or the raw instruction where there is no road name */
        val instruction: String,
        /** arrival wall-clock time as epoch seconds, 0 if unknown */
        val etaEpochS: Long,
        /** metres left on the route, from progressMax - progress */
        val remainingM: Int,
        /** seconds left, derived from the arrival time */
        val remainingS: Int,
    )

    @Volatile var listenerConnected = false
    @Volatile var latest: Guidance? = null
        private set

    private val subscribers = mutableListOf<(Guidance?) -> Unit>()

    @Synchronized
    fun subscribe(cb: (Guidance?) -> Unit) {
        subscribers.add(cb)
        cb(latest)
    }

    @Synchronized
    fun unsubscribe(cb: (Guidance?) -> Unit) {
        subscribers.remove(cb)
    }

    @Synchronized
    fun publish(g: Guidance) {
        latest = g
        subscribers.forEach { it(g) }
    }

    @Synchronized
    fun clear() {
        latest = null
        subscribers.forEach { it(null) }
    }
}
