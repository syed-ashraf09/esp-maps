package dev.espmaps.nav

import android.util.Log
import dev.espmaps.ble.Maneuver
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONObject
import java.util.concurrent.TimeUnit

data class RoutePoint(val lat: Double, val lon: Double)

data class RouteManeuver(
    val kind: Int,
    val exitNo: Int,
    val distM: Int,
    val ptIndex: Int,
    val text: String,
)

data class Route(
    val points: List<RoutePoint>,
    val maneuvers: List<RouteManeuver>,
    val distM: Int,
    val durS: Int,
)

interface RouteService {
    suspend fun route(from: RoutePoint, to: RoutePoint): Route?
}

private val http = OkHttpClient.Builder()
    .connectTimeout(10, TimeUnit.SECONDS)
    .readTimeout(25, TimeUnit.SECONDS)
    .build()

private fun getJson(url: String, tag: String): JSONObject? = try {
    http.newCall(Request.Builder().url(url).build()).execute().use { r ->
        if (!r.isSuccessful) {
            Log.w(tag, "HTTP ${r.code}")
            null
        } else {
            r.body?.string()?.let { JSONObject(it) }
        }
    }
} catch (e: Exception) {
    Log.w(tag, "request failed: ${e.message}")
    null
}

/**
 * GraphHopper. Preferred because it is reliable and its `sign` codes map
 * cleanly onto our maneuver enum, so the turn banner shows the right arrow
 * without guesswork.
 */
class GraphHopperRouteService(private val apiKey: String) : RouteService {

    override suspend fun route(from: RoutePoint, to: RoutePoint): Route? {
        if (apiKey.isBlank()) return null

        val url = buildString {
            append("https://graphhopper.com/api/1/route")
            append("?point=${from.lat},${from.lon}")
            append("&point=${to.lat},${to.lon}")
            append("&profile=car&points_encoded=false&instructions=true")
            append("&key=$apiKey")
        }

        val json = getJson(url, TAG) ?: return null
        val paths = json.optJSONArray("paths") ?: return null
        if (paths.length() == 0) return null
        val path = paths.getJSONObject(0)

        val coords = path.optJSONObject("points")?.optJSONArray("coordinates")
            ?: return null
        val pts = ArrayList<RoutePoint>(coords.length())
        for (i in 0 until coords.length()) {
            val c = coords.getJSONArray(i)
            // GeoJSON is [lon, lat].
            pts.add(RoutePoint(c.getDouble(1), c.getDouble(0)))
        }

        val maneuvers = ArrayList<RouteManeuver>()
        path.optJSONArray("instructions")?.let { arr ->
            for (i in 0 until arr.length()) {
                val ins = arr.getJSONObject(i)
                val interval = ins.optJSONArray("interval")
                maneuvers.add(
                    RouteManeuver(
                        kind = signToManeuver(ins.optInt("sign", 0)),
                        exitNo = ins.optInt("exit_number", 0),
                        distM = ins.optDouble("distance", 0.0).toInt(),
                        ptIndex = interval?.optInt(0, 0) ?: 0,
                        text = ins.optString("street_name").ifBlank {
                            ins.optString("text")
                        },
                    )
                )
            }
        }

        return Route(
            points = pts,
            maneuvers = maneuvers,
            distM = path.optDouble("distance", 0.0).toInt(),
            durS = (path.optLong("time", 0L) / 1000L).toInt(),
        )
    }

    private fun signToManeuver(sign: Int): Int = when (sign) {
        -98, -8, 8 -> Maneuver.UTURN
        -7 -> Maneuver.FORK_LEFT
        -3 -> Maneuver.SHARP_LEFT
        -2 -> Maneuver.LEFT
        -1 -> Maneuver.SLIGHT_LEFT
        0 -> Maneuver.STRAIGHT
        1 -> Maneuver.SLIGHT_RIGHT
        2 -> Maneuver.RIGHT
        3 -> Maneuver.SHARP_RIGHT
        4, 5 -> Maneuver.ARRIVE
        6 -> Maneuver.ROUNDABOUT
        7 -> Maneuver.FORK_RIGHT
        else -> Maneuver.STRAIGHT
    }

    companion object { private const val TAG = "GraphHopper" }
}

/**
 * OSRM's public demo server. No key, so it works before you have set anything
 * up - but it is explicitly demo-only and rate-limited, so treat it as a
 * fallback rather than the thing you rely on mid-journey.
 */
class OsrmRouteService : RouteService {

    override suspend fun route(from: RoutePoint, to: RoutePoint): Route? {
        val url = buildString {
            append("https://router.project-osrm.org/route/v1/driving/")
            append("${from.lon},${from.lat};${to.lon},${to.lat}")
            append("?overview=full&geometries=geojson&steps=true")
        }

        val json = getJson(url, TAG) ?: return null
        val routes = json.optJSONArray("routes") ?: return null
        if (routes.length() == 0) return null
        val r = routes.getJSONObject(0)

        val coords = r.optJSONObject("geometry")?.optJSONArray("coordinates")
            ?: return null
        val pts = ArrayList<RoutePoint>(coords.length())
        for (i in 0 until coords.length()) {
            val c = coords.getJSONArray(i)
            pts.add(RoutePoint(c.getDouble(1), c.getDouble(0)))
        }

        // OSRM indexes maneuvers per-leg rather than into the whole polyline,
        // so accumulate an offset as we walk the legs.
        val maneuvers = ArrayList<RouteManeuver>()
        var ptOffset = 0
        r.optJSONArray("legs")?.let { legs ->
            for (li in 0 until legs.length()) {
                val steps = legs.getJSONObject(li).optJSONArray("steps") ?: continue
                for (si in 0 until steps.length()) {
                    val step = steps.getJSONObject(si)
                    val man = step.optJSONObject("maneuver")
                    maneuvers.add(
                        RouteManeuver(
                            kind = osrmToManeuver(
                                man?.optString("type") ?: "",
                                man?.optString("modifier") ?: ""
                            ),
                            exitNo = man?.optInt("exit", 0) ?: 0,
                            distM = step.optDouble("distance", 0.0).toInt(),
                            ptIndex = ptOffset,
                            text = step.optString("name"),
                        )
                    )
                    ptOffset += step.optJSONArray("intersections")?.length() ?: 1
                }
            }
        }

        return Route(
            points = pts,
            maneuvers = maneuvers,
            distM = r.optDouble("distance", 0.0).toInt(),
            durS = r.optDouble("duration", 0.0).toInt(),
        )
    }

    private fun osrmToManeuver(type: String, modifier: String): Int = when (type) {
        "depart" -> Maneuver.DEPART
        "arrive" -> Maneuver.ARRIVE
        "roundabout", "rotary", "roundabout turn" -> Maneuver.ROUNDABOUT
        "merge" -> Maneuver.MERGE
        "fork" -> if (modifier.contains("left")) Maneuver.FORK_LEFT
                  else Maneuver.FORK_RIGHT
        else -> when (modifier) {
            "uturn" -> Maneuver.UTURN
            "sharp left" -> Maneuver.SHARP_LEFT
            "left" -> Maneuver.LEFT
            "slight left" -> Maneuver.SLIGHT_LEFT
            "slight right" -> Maneuver.SLIGHT_RIGHT
            "right" -> Maneuver.RIGHT
            "sharp right" -> Maneuver.SHARP_RIGHT
            else -> Maneuver.STRAIGHT
        }
    }

    companion object { private const val TAG = "OSRM" }
}

/** Tries GraphHopper, falls back to OSRM so the app works without a key. */
class FallbackRouteService(
    private val primary: RouteService,
    private val fallback: RouteService,
) : RouteService {
    override suspend fun route(from: RoutePoint, to: RoutePoint): Route? =
        primary.route(from, to) ?: fallback.route(from, to)
}
