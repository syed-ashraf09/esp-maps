package dev.espmaps.map

import android.content.Context
import android.util.Log
import okhttp3.Cache
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.File
import java.util.concurrent.TimeUnit

/**
 * Fetches OpenMapTiles-schema vector tiles.
 *
 * Pluggable so swapping providers is one class. The default is MapTiler:
 * free tier, no card, and the plainest OpenMapTiles schema of the hosted
 * options - which matters because TileEncoder keys off `class` attribute
 * names that are schema-specific.
 */
interface TileSource {
    /** Raw .pbf bytes, or null if the tile does not exist (a 404 is normal). */
    suspend fun fetch(z: Int, x: Int, y: Int): ByteArray?
}

class MapTilerSource(
    context: Context,
    private val apiKey: String,
) : TileSource {

    private val client = OkHttpClient.Builder()
        // A disk cache matters more here than usual: revisiting an area
        // should never re-download, because the phone is the ESP32's only
        // source and cold fetches show up as blank map on the device.
        .cache(Cache(File(context.cacheDir, "tiles"), 64L * 1024 * 1024))
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(20, TimeUnit.SECONDS)
        .build()

    override suspend fun fetch(z: Int, x: Int, y: Int): ByteArray? {
        if (apiKey.isBlank()) {
            Log.e(TAG, "no MapTiler key - set MAPTILER_KEY in gradle.properties")
            return null
        }

        val url = "https://api.maptiler.com/tiles/v3/$z/$x/$y.pbf?key=$apiKey"
        val req = Request.Builder().url(url).build()

        return try {
            client.newCall(req).execute().use { resp ->
                when {
                    resp.code == 404 -> null              // legitimately empty
                    !resp.isSuccessful -> {
                        Log.w(TAG, "tile $z/$x/$y HTTP ${resp.code}")
                        null
                    }
                    else -> resp.body?.bytes()
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "tile $z/$x/$y failed: ${e.message}")
            null
        }
    }

    companion object { private const val TAG = "MapTilerSource" }
}
