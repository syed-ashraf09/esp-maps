package dev.espmaps

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.location.Location
import android.os.Build
import android.os.IBinder
import android.util.Log
import com.google.android.gms.location.FusedLocationProviderClient
import com.google.android.gms.location.LocationCallback
import com.google.android.gms.location.LocationRequest
import com.google.android.gms.location.LocationResult
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.Priority
import dev.espmaps.ble.BleLink
import dev.espmaps.ble.Maneuver
import dev.espmaps.ble.Proto
import dev.espmaps.ble.Varint
import dev.espmaps.nav.DemoDriver
import dev.espmaps.nav.MapsNav
import dev.espmaps.nav.MapsNavListener
import dev.espmaps.map.MapTilerSource
import dev.espmaps.map.TilePump
import dev.espmaps.nav.FallbackRouteService
import dev.espmaps.nav.GraphHopperRouteService
import dev.espmaps.nav.OsrmRouteService
import dev.espmaps.nav.Route
import dev.espmaps.nav.RoutePoint
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import java.io.ByteArrayOutputStream
import java.util.zip.CRC32
import kotlin.math.roundToInt

/**
 * Owns the link to the device for as long as navigation is running.
 *
 * A foreground service rather than an Activity because the screen will be off
 * and the phone in a pocket for the entire useful lifetime of this app.
 */
class NavService : Service(), BleLink.Listener {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    private lateinit var link: BleLink
    private lateinit var pump: TilePump
    private lateinit var fused: FusedLocationProviderClient

    private val router by lazy {
        FallbackRouteService(
            GraphHopperRouteService(BuildConfig.GRAPHHOPPER_KEY),
            OsrmRouteService()
        )
    }

    private var lastLocation: Location? = null
    private var route: Route? = null

    // When Google Maps is actively navigating, its guidance wins over any
    // route we computed ourselves - it knows about traffic and reroutes and
    // we do not. Our own route, if any, stays on screen as the drawn line.
    @Volatile private var mapsGuiding = false

    private val mapsObserver: (MapsNav.Guidance?) -> Unit = { g ->
        if (g == null) {
            mapsGuiding = false
            if (link.connected) link.send(Proto.MSG_MANEUVER, clearedManeuver())
        } else {
            mapsGuiding = true
            sendMapsGuidance(g)
        }
    }

    companion object {
        private const val TAG = "NavService"
        private const val CHANNEL = "espmaps"
        private const val NOTIF_ID = 1
        const val ACTION_STOP = "dev.espmaps.STOP"
        const val ACTION_SET_DEST = "dev.espmaps.SET_DEST"
        const val ACTION_DEMO = "dev.espmaps.DEMO"
        const val EXTRA_LAT = "lat"
        const val EXTRA_LON = "lon"

        /**
         * Real service state. MainActivity is destroyed and recreated freely -
         * notably when the share sheet launches it - so a local flag there
         * says nothing about whether the service is alive.
         */
        @Volatile
        var isRunning = false
            private set
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        isRunning = true

        link = BleLink(this, this)
        pump = TilePump(link, MapTilerSource(this, BuildConfig.MAPTILER_KEY), scope)
        fused = LocationServices.getFusedLocationProviderClient(this)

        startForegroundCompat()
        link.start()
        startLocation()

        MapsNav.subscribe(mapsObserver)
        if (!MapsNavListener.isEnabled(this)) {
            Log.w(TAG, "notification access not granted - Google Maps guidance " +
                       "will not appear. Grant it from the app's main screen.")
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_SET_DEST -> {
                val lat = intent.getDoubleExtra(EXTRA_LAT, Double.NaN)
                val lon = intent.getDoubleExtra(EXTRA_LON, Double.NaN)
                if (!lat.isNaN() && !lon.isNaN()) onDestinationPicked(lat, lon)
            }
            ACTION_DEMO -> toggleDemo()
        }
        return START_STICKY
    }

    override fun onDestroy() {
        isRunning = false
        demo?.stop()
        MapsNav.unsubscribe(mapsObserver)
        fused.removeLocationUpdates(locationCallback)
        link.disconnect()
        scope.cancel()
        super.onDestroy()
    }

    // ---- Google Maps guidance -------------------------------------------

    private fun clearedManeuver(): ByteArray {
        val b = Proto.le(9)
        b.put(Maneuver.NONE.toByte())
        b.put(0)
        b.putInt(0)
        b.putShort(0)
        b.put(0)
        return b.array()
    }

    private fun sendMapsGuidance(g: MapsNav.Guidance) {
        if (!link.connected) return

        val text = g.instruction.take(47).toByteArray(Charsets.UTF_8)
        val b = Proto.le(9 + text.size)
        b.put(g.maneuver.toByte())
        b.put(0)                       // exit number, not available from Maps
        b.putInt(g.distanceM)
        // pt_index is meaningless here: there is no polyline from Maps, so the
        // firmware must use dist_m as sent rather than measuring along a route.
        b.putShort(0)
        b.put(text.size.toByte())
        b.put(text)
        link.send(Proto.MSG_MANEUVER, b.array())

        if (g.remainingM > 0 || g.remainingS > 0) {
            // Maps gives a wall-clock arrival time rather than a duration, so
            // pass that straight through instead of re-deriving it from a
            // countdown that would drift between updates.
            val eta = if (g.etaEpochS > 0) g.etaEpochS
                      else System.currentTimeMillis() / 1000L + g.remainingS
            val n = Proto.le(13)
            n.put(1)                   // state: navigating
            n.putInt(eta.toInt())
            n.putInt(g.remainingM)
            n.putInt(g.remainingS)
            link.send(Proto.MSG_NAV_STATE, n.array())
        }
    }

    // ---- notification ----------------------------------------------------

    private fun startForegroundCompat() {
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL, "Navigation",
                    NotificationManager.IMPORTANCE_LOW)
            )
        }

        val stopIntent = PendingIntent.getService(
            this, 0,
            Intent(this, NavService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE
        )

        val n: Notification = Notification.Builder(this, CHANNEL)
            .setContentTitle("esp-maps")
            .setContentText("Streaming map to display")
            .setSmallIcon(android.R.drawable.ic_menu_mapmode)
            .addAction(Notification.Action.Builder(null, "Stop", stopIntent).build())
            .setOngoing(true)
            .build()

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(NOTIF_ID, n)
            return
        }

        // The primary job of this service is talking to a BLE device, so
        // connectedDevice is the type that always applies. Location is added
        // only when we can legally claim it.
        //
        // A "location" foreground service throws SecurityException unless the
        // location permission is granted AND currently in an eligible state.
        // A one-time ("Only this time") grant lapses the moment the app leaves
        // the foreground, so this crashed the whole process mid-transfer and
        // took the BLE link down with it.
        val canUseLocation =
            checkSelfPermission(android.Manifest.permission.ACCESS_FINE_LOCATION) ==
                PackageManager.PERMISSION_GRANTED ||
            checkSelfPermission(android.Manifest.permission.ACCESS_COARSE_LOCATION) ==
                PackageManager.PERMISSION_GRANTED

        val types = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE or
            if (canUseLocation) ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION else 0

        try {
            startForeground(NOTIF_ID, n, types)
        } catch (e: SecurityException) {
            // The permission was granted but not in an eligible state - almost
            // always a one-time grant that has since lapsed. Degrade to a
            // device-only service so the map keeps streaming; position simply
            // stops updating until the user grants location properly.
            Log.w(TAG, "location FGS refused (${e.message?.take(120)}), " +
                       "falling back to connectedDevice only")
            startForeground(NOTIF_ID, n,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        }
    }

    // ---- simulated drive -------------------------------------------------

    private var demo: DemoDriver? = null

    /**
     * Replays the current route as if you were driving it. Everything
     * downstream - tile prefetch, heading-up rotation, the puck, the route
     * line, the turn banner - behaves exactly as on a real trip.
     */
    private fun toggleDemo() {
        if (demo?.running == true) {
            demo?.stop()
            demo = null
            Log.i(TAG, "simulated drive stopped")
            return
        }
        val r = route
        if (r == null) {
            Log.w(TAG, "no route to simulate - set a destination first")
            return
        }
        demo = DemoDriver(r, onFix = { loc ->
            lastLocation = loc
            sendFix(loc)
            updateProgress(loc)
        }).also { it.start(scope) }
    }

    // ---- location --------------------------------------------------------

    private val locationCallback = object : LocationCallback() {
        override fun onLocationResult(result: LocationResult) {
            // A simulated drive owns the position; ignore the real one so the
            // two cannot fight over the map centre.
            if (demo?.running == true) return
            val loc = result.lastLocation ?: return
            val firstFix = lastLocation == null
            lastLocation = loc

            if (firstFix) {
                pendingDestination?.let { (dlat, dlon) ->
                    pendingDestination = null
                    Log.i(TAG, "first fix arrived - routing to held destination")
                    onDestinationPicked(dlat, dlon)
                }
            }
            sendFix(loc)
            updateProgress(loc)
        }
    }

    @SuppressLint("MissingPermission")
    private fun startLocation() {
        val req = LocationRequest.Builder(Priority.PRIORITY_HIGH_ACCURACY, 1000L)
            .setMinUpdateIntervalMillis(1000L)
            .build()
        try {
            fused.requestLocationUpdates(req, locationCallback, mainLooper)
        } catch (e: SecurityException) {
            Log.e(TAG, "location permission missing")
        }
    }

    private fun sendFix(loc: Location) {
        if (!link.connected) return

        val b = Proto.le(20)
        b.putInt((loc.latitude * 1e7).roundToInt())
        b.putInt((loc.longitude * 1e7).roundToInt())
        b.putShort(
            if (loc.hasSpeed()) (loc.speed * 100f).toInt().coerceIn(0, 65534).toShort()
            else Proto.GPS_UNKNOWN_U16.toShort()
        )
        // Only trust bearing when actually moving; below ~1 m/s the fused
        // provider's course is noise, and the device has no compass to
        // fall back on, so it holds the last good value instead.
        b.putShort(
            if (loc.hasBearing() && loc.hasSpeed() && loc.speed > 1.0f)
                ((loc.bearing * 100f).toInt().coerceIn(0, 35999)).toShort()
            else Proto.GPS_UNKNOWN_U16.toShort()
        )
        b.putShort(
            if (loc.hasAccuracy()) (loc.accuracy * 10f).toInt().coerceIn(0, 65534).toShort()
            else Proto.GPS_UNKNOWN_U16.toShort()
        )
        b.putShort(loc.altitude.toInt().toShort())
        b.putInt((loc.time / 1000L).toInt())

        link.send(Proto.MSG_GPS_FIX, b.array())
    }

    // ---- BleLink.Listener ------------------------------------------------

    override fun onConnectionChanged(connected: Boolean) {
        Log.i(TAG, if (connected) "device connected" else "device gone")
        if (connected) {
            lastLocation?.let { sendFix(it) }
            route?.let { scope.launch { sendRoute(it) } }
        }
    }

    override fun onTilesRequested(tiles: List<BleLink.TileRequest>) {
        pump.request(tiles)
    }

    override fun onViewChanged(lat: Double, lon: Double, zoom: Int, bearingDeg: Float) {
        // Room for predictive prefetch along the bearing; the firmware's own
        // 3x3 request window already covers the common case.
    }

    /**
     * A destination can easily arrive before the first GPS fix - sharing from
     * Maps starts the service, and a fix takes a few seconds longer. Holding
     * it means the route is computed as soon as we know where we are, instead
     * of the destination being silently discarded.
     */
    @Volatile private var pendingDestination: Pair<Double, Double>? = null

    override fun onDestinationPicked(lat: Double, lon: Double) {
        val from = lastLocation ?: run {
            Log.w(TAG, "destination picked before first fix - holding it")
            pendingDestination = lat to lon
            return
        }
        scope.launch {
            val r = router.route(
                RoutePoint(from.latitude, from.longitude),
                RoutePoint(lat, lon)
            )
            if (r == null) {
                Log.w(TAG, "routing failed")
                return@launch
            }
            route = r
            Log.i(TAG, "route: ${r.points.size} pts, ${r.distM} m, ${r.durS} s")
            sendRoute(r)
        }
    }

    // ---- route transfer --------------------------------------------------

    private fun sendRoute(r: Route) {
        if (!link.connected) return

        // Delta + zigzag varint at 1e-6 degrees. Consecutive route points are
        // metres apart, so most deltas land in one or two bytes - a 40 km
        // route is around 5 KB, sent once.
        val body = ByteArrayOutputStream(r.points.size * 4)
        var pLat = 0
        var pLon = 0
        for (p in r.points) {
            val la = (p.lat * 1e6).roundToInt()
            val lo = (p.lon * 1e6).roundToInt()
            Varint.writeZigZag(body, la - pLat)
            Varint.writeZigZag(body, lo - pLon)
            pLat = la
            pLon = lo
        }
        val bytes = body.toByteArray()

        val chunkSize = link.chunkSize - 2
        val chunks = (bytes.size + chunkSize - 1) / chunkSize

        val start = Proto.le(14)
        start.putInt(r.points.size)
        start.putInt(r.distM)
        start.putInt(r.durS)
        start.putShort(chunks.toShort())
        link.send(Proto.MSG_ROUTE_START, start.array())

        var off = 0
        var index = 0
        while (off < bytes.size) {
            val n = minOf(chunkSize, bytes.size - off)
            val c = Proto.le(2 + n)
            c.putShort(index.toShort())
            c.put(bytes, off, n)
            link.send(Proto.MSG_ROUTE_CHUNK, c.array())
            off += n
            index++
        }

        val crc = CRC32().apply { update(bytes) }.value
        link.send(Proto.MSG_ROUTE_END, Proto.le(4).putInt(crc.toInt()).array())

        sendNextManeuver(r, 0)
    }

    private fun sendNextManeuver(r: Route, fromPtIndex: Int) {
        val m = r.maneuvers.firstOrNull { it.ptIndex >= fromPtIndex } ?: return
        val text = m.text.take(47).toByteArray(Charsets.UTF_8)

        val b = Proto.le(9 + text.size)
        b.put(m.kind.toByte())
        b.put(m.exitNo.toByte())
        b.putInt(m.distM)
        b.putShort(m.ptIndex.toShort())
        b.put(text.size.toByte())
        b.put(text)
        link.send(Proto.MSG_MANEUVER, b.array())
    }

    // ---- progress --------------------------------------------------------

    private fun updateProgress(loc: Location) {
        val r = route ?: return
        if (!link.connected || r.points.isEmpty()) return

        // Nearest route vertex, then remaining polyline length from there.
        var nearest = 0
        var bestD = Double.MAX_VALUE
        val results = FloatArray(1)
        for (i in r.points.indices) {
            Location.distanceBetween(
                loc.latitude, loc.longitude,
                r.points[i].lat, r.points[i].lon, results
            )
            val d = results[0].toDouble()
            if (d < bestD) { bestD = d; nearest = i }
        }

        var remain = 0.0
        for (i in nearest until r.points.size - 1) {
            Location.distanceBetween(
                r.points[i].lat, r.points[i].lon,
                r.points[i + 1].lat, r.points[i + 1].lon, results
            )
            remain += results[0]
        }

        // Scale the original duration by how much distance is left. Crude,
        // but it does not drift the way a live-speed estimate does when you
        // stop at lights.
        val fraction = if (r.distM > 0) remain / r.distM else 0.0
        val remainS = (r.durS * fraction).toInt()
        val eta = (System.currentTimeMillis() / 1000L).toInt() + remainS

        val b = Proto.le(13)
        b.put(1)                       // state: navigating
        b.putInt(eta)
        b.putInt(remain.toInt())
        b.putInt(remainS)
        link.send(Proto.MSG_NAV_STATE, b.array())

        // Don't fight Google Maps for the turn banner. When it is guiding, its
        // maneuvers account for traffic and reroutes; ours are from a route we
        // computed once and never revisited.
        if (!mapsGuiding) sendNextManeuver(r, nearest)
    }
}
