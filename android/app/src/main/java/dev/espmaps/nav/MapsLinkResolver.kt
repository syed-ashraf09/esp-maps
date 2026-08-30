package dev.espmaps.nav

import android.content.Context
import android.location.Geocoder
import android.os.Build
import android.util.Log
import okhttp3.OkHttpClient
import okhttp3.Request
import java.util.Locale
import java.util.concurrent.TimeUnit

/**
 * Turns a shared Google Maps link into coordinates.
 *
 * Two stages, because a short link usually resolves to a URL that still has
 * no coordinates in it:
 *
 *   1. Follow maps.app.goo.gl redirects, checking each hop for lat/lon.
 *   2. If none appear, geocode the address embedded in the place path.
 *
 * Verified against a real shared link, which resolved to:
 *
 *   /maps/place/Crispy+Things,+Whisper+Vly+Rd,...,+Telangana+500104/
 *       data=!4m2!3m1!1s0x3bcb97b47afaf027:0xc4a6a0a0e86d78d0!18m1!1e1
 *
 * No `@lat,lon`, no `!3d/!4d`, and the page body is a JS shell with nothing
 * scrapeable. Only the address and a Google feature id, so geocoding the
 * address is the only route that works.
 */
object MapsLinkResolver {

    private const val TAG = "MapsLink"
    private const val MAX_HOPS = 5

    private const val UA = "Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36"

    private val http = OkHttpClient.Builder()
        .followRedirects(false)
        .followSslRedirects(false)
        .connectTimeout(8, TimeUnit.SECONDS)
        .readTimeout(8, TimeUnit.SECONDS)
        .build()

    /**
     * Fallback client. The manual HEAD chain is faster but Google does not
     * always answer a HEAD with a Location header - observed failing on two
     * of three consecutive shares. Letting OkHttp follow a GET is slower but
     * far more reliable, and the final request URL is what we want.
     */
    private val httpFollow = OkHttpClient.Builder()
        .followRedirects(true)
        .followSslRedirects(true)
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(10, TimeUnit.SECONDS)
        .build()

    /**
     * Blocking; call from a background dispatcher.
     *
     * @param fallbackName the shared title (Maps puts the place name in
     *        EXTRA_SUBJECT), used only if the URL yields nothing at all.
     */
    fun resolve(ctx: Context, shortUrl: String, fallbackName: String? = null): MapsLinkParser.Dest? {
        var finalUrl = followRedirects(shortUrl)

        if (finalUrl == shortUrl) {
            Log.i(TAG, "HEAD chain did not redirect - retrying with a followed GET")
            resolveByFollowing(shortUrl)?.let { finalUrl = it }
        }

        MapsLinkParser.parse(finalUrl)?.let {
            Log.i(TAG, "coordinates found directly in the resolved URL")
            return it
        }

        val fromUrl = MapsLinkParser.findPlaceQuery(finalUrl)
        val query = fromUrl ?: fallbackName?.takeIf { it.isNotBlank() }
        if (query == null) {
            Log.w(TAG, "no coordinates and no address in: ${finalUrl.take(160)}")
            return null
        }
        if (fromUrl == null) {
            // Just a place name with no address - the geocoder may well pick
            // a same-named place in another city.
            Log.w(TAG, "URL gave nothing; falling back to the shared title \"$query\"")
        } else {
            Log.i(TAG, "no coordinates in URL - geocoding \"$query\"")
        }
        return geocode(ctx, query)
    }

    private fun resolveByFollowing(url: String): String? = try {
        val req = Request.Builder().url(url).header("User-Agent", UA).get().build()
        httpFollow.newCall(req).execute().use { it.request.url.toString() }
    } catch (e: Exception) {
        Log.w(TAG, "followed GET failed: ${e.message}")
        null
    }

    private fun followRedirects(start: String): String {
        var url = start
        repeat(MAX_HOPS) {
            if (MapsLinkParser.parse(url) != null) return url

            val req = Request.Builder()
                .url(url)
                // Without a browser UA, Maps serves a consent interstitial.
                .header("User-Agent", UA)
                .head()
                .build()

            val next = try {
                http.newCall(req).execute().use { it.header("Location") }
            } catch (e: Exception) {
                Log.w(TAG, "redirect hop failed for ${url.take(80)}: ${e.message}")
                return url
            } ?: return url

            if (!next.startsWith("http")) return url
            url = next
        }
        return url
    }

    /**
     * Uses the platform geocoder, which is backed by Google on most devices
     * and needs no API key of ours.
     */
    private fun geocode(ctx: Context, query: String): MapsLinkParser.Dest? {
        if (!Geocoder.isPresent()) {
            Log.w(TAG, "no geocoder on this device")
            return null
        }
        val geo = Geocoder(ctx, Locale.getDefault())
        return try {
            @Suppress("DEPRECATION")
            val results = geo.getFromLocationName(query, 1)
            val a = results?.firstOrNull()
            if (a == null) {
                Log.w(TAG, "geocoder found nothing for \"$query\"")
                null
            } else {
                Log.i(TAG, "geocoded to ${a.latitude}, ${a.longitude}")
                MapsLinkParser.Dest(a.latitude, a.longitude, query.take(47))
            }
        } catch (e: Exception) {
            // Geocoder throws IOException when the backend is unreachable.
            Log.w(TAG, "geocode failed: ${e.message}")
            null
        }
    }
}
