package dev.espmaps.nav

import android.location.Location
import android.os.SystemClock
import android.util.Log
import dev.espmaps.ble.Maneuver
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Drives a route without a car.
 *
 * Plays a synthetic position along a real computed route at a fixed speed,
 * emitting Location fixes and matching turn guidance. Because the path
 * follows an actual routed road, the display exercises everything the real
 * thing does: tile fetching across boundaries, heading-up rotation, the route
 * line, the puck, and the turn banner countdown.
 *
 * This is not a mock-location provider - it feeds NavService directly, so it
 * needs no Developer Options setup and cannot be confused with a real fix.
 */
class DemoDriver(
    private val route: Route,
    private val speedMps: Double = 13.9,       // 50 km/h
    private val tickMs: Long = 1000,
    private val onFix: (Location) -> Unit,
) {
    companion object {
        private const val TAG = "DemoDriver"
        private const val R = 6_371_000.0
    }

    private var job: Job? = null

    /** Cumulative distance in metres to each route point. */
    private val cumulative: DoubleArray = DoubleArray(route.points.size)

    init {
        for (i in 1 until route.points.size) {
            cumulative[i] = cumulative[i - 1] +
                haversine(route.points[i - 1], route.points[i])
        }
    }

    val totalM: Double get() = cumulative.lastOrNull() ?: 0.0

    fun start(scope: CoroutineScope) {
        stop()
        if (route.points.size < 2) {
            Log.w(TAG, "route too short to simulate")
            return
        }
        Log.i(TAG, "simulating ${"%.1f".format(totalM / 1000)} km at " +
                   "${"%.0f".format(speedMps * 3.6)} km/h")

        job = scope.launch {
            var travelled = 0.0
            while (isActive && travelled <= totalM) {
                emit(travelled)
                delay(tickMs)
                travelled += speedMps * (tickMs / 1000.0)
            }
            if (isActive) {
                Log.i(TAG, "simulated trip complete")
                MapsNav.publish(
                    MapsNav.Guidance(
                        maneuver = Maneuver.ARRIVE,
                        distanceM = 0,
                        instruction = "Arrived",
                        etaEpochS = System.currentTimeMillis() / 1000,
                        remainingM = 0,
                        remainingS = 0,
                    )
                )
            }
        }
    }

    fun stop() {
        job?.cancel()
        job = null
    }

    val running: Boolean get() = job?.isActive == true

    // ---- playback --------------------------------------------------------

    private fun emit(travelled: Double) {
        val idx = segmentAt(travelled)
        val a = route.points[idx]
        val b = route.points[(idx + 1).coerceAtMost(route.points.size - 1)]

        val segLen = (cumulative[idx + 1] - cumulative[idx]).coerceAtLeast(0.0001)
        val t = ((travelled - cumulative[idx]) / segLen).coerceIn(0.0, 1.0)

        val lat = a.lat + (b.lat - a.lat) * t
        val lon = a.lon + (b.lon - a.lon) * t

        val loc = Location("espmaps-demo").apply {
            latitude = lat
            longitude = lon
            bearing = bearingBetween(a, b).toFloat()
            speed = speedMps.toFloat()
            accuracy = 4f
            time = System.currentTimeMillis()
            elapsedRealtimeNanos = SystemClock.elapsedRealtimeNanos()
        }
        onFix(loc)

        publishGuidance(travelled, idx)
    }

    /** Binary search for the segment containing this distance. */
    private fun segmentAt(travelled: Double): Int {
        var lo = 0
        var hi = cumulative.size - 2
        while (lo < hi) {
            val mid = (lo + hi + 1) / 2
            if (cumulative[mid] <= travelled) lo = mid else hi = mid - 1
        }
        return lo.coerceIn(0, route.points.size - 2)
    }

    /**
     * Synthesises the same shape of guidance Google Maps would publish, so
     * the banner and the whole MapsNav path get exercised identically.
     */
    private fun publishGuidance(travelled: Double, idx: Int) {
        val next = route.maneuvers.firstOrNull { it.ptIndex > idx }
        val remainingM = (totalM - travelled).coerceAtLeast(0.0).toInt()
        val remainingS = (remainingM / speedMps).toInt()

        val (kind, text, distToTurn) = if (next != null) {
            val at = cumulative.getOrElse(next.ptIndex) { totalM }
            Triple(next.kind, next.text, (at - travelled).coerceAtLeast(0.0).toInt())
        } else {
            Triple(Maneuver.ARRIVE, "Destination", remainingM)
        }

        MapsNav.publish(
            MapsNav.Guidance(
                maneuver = kind,
                distanceM = distToTurn,
                instruction = text.ifBlank { "Continue" }.take(47),
                etaEpochS = System.currentTimeMillis() / 1000 + remainingS,
                remainingM = remainingM,
                remainingS = remainingS,
            )
        )
    }

    // ---- geo -------------------------------------------------------------

    private fun haversine(a: RoutePoint, b: RoutePoint): Double {
        val dLat = Math.toRadians(b.lat - a.lat)
        val dLon = Math.toRadians(b.lon - a.lon)
        val la1 = Math.toRadians(a.lat)
        val la2 = Math.toRadians(b.lat)
        val h = sin(dLat / 2) * sin(dLat / 2) +
                cos(la1) * cos(la2) * sin(dLon / 2) * sin(dLon / 2)
        return 2 * R * atan2(sqrt(h), sqrt(1 - h))
    }

    private fun bearingBetween(a: RoutePoint, b: RoutePoint): Double {
        val la1 = Math.toRadians(a.lat)
        val la2 = Math.toRadians(b.lat)
        val dLon = Math.toRadians(b.lon - a.lon)
        val y = sin(dLon) * cos(la2)
        val x = cos(la1) * sin(la2) - sin(la1) * cos(la2) * cos(dLon)
        return (Math.toDegrees(atan2(y, x)) + 360.0) % 360.0
    }
}
