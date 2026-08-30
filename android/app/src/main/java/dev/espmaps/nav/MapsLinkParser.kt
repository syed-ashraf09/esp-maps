package dev.espmaps.nav

/**
 * Pulls a destination out of a shared Google Maps link.
 *
 * Google Maps will not tell us its route, but it will happily *share* the
 * destination - and a shared link carries coordinates. So the flow is: in
 * Maps, share the place to esp-maps once, then start navigation as normal.
 * The display then has the real map, Maps' own turn guidance, and a route
 * line to the same destination.
 *
 * Deliberately free of Android types so it is unit-testable on the JVM.
 * Short links (maps.app.goo.gl) need an HTTP redirect resolved first; see
 * MapsLinkResolver.
 */
object MapsLinkParser {

    data class Dest(val lat: Double, val lon: Double, val label: String? = null)

    /** Short links carry no coordinates until the redirect is followed. */
    private val SHORT = Regex(
        """https?://(?:maps\.app\.goo\.gl|goo\.gl/maps)/\S+""", RegexOption.IGNORE_CASE
    )

    fun findShortLink(text: String): String? = SHORT.find(text)?.value

    // Place URLs embed the true place coordinates as !3d<lat>!4d<lon>. The
    // @lat,lon earlier in the path is the map viewport centre, which can be
    // some way off, so this is checked first.
    private val BANG = Regex("""!3d(-?\d+\.\d+)!4d(-?\d+\.\d+)""")

    // ?q=, &destination=, &daddr=, &ll=
    private val PARAM = Regex(
        """[?&](?:q|destination|daddr|ll)=(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)""",
        RegexOption.IGNORE_CASE
    )

    private val AT = Regex("""@(-?\d+\.\d+),(-?\d+\.\d+)""")

    private val GEO = Regex(
        """geo:(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)""", RegexOption.IGNORE_CASE
    )

    /** Bare "17.385, 78.486" typed or pasted by hand. */
    private val BARE = Regex(
        """^\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*$"""
    )

    /**
     * Returns the destination, or null if the text carries no coordinates -
     * which includes an unresolved short link, so check findShortLink first.
     */
    fun parse(text: String): Dest? {
        for (re in listOf(BANG, PARAM, GEO, AT, BARE)) {
            val m = re.find(text) ?: continue
            val lat = m.groupValues[1].toDoubleOrNull() ?: continue
            val lon = m.groupValues[2].toDoubleOrNull() ?: continue
            if (!valid(lat, lon)) continue
            return Dest(lat, lon, label(text))
        }
        return null
    }

    private fun valid(lat: Double, lon: Double) =
        lat in -90.0..90.0 && lon in -180.0..180.0 && !(lat == 0.0 && lon == 0.0)

    // /maps/place/<address>/data=... or /maps/search/<query>
    private val PLACE_PATH = Regex(
        """/maps/(?:place|search)/([^/?#]+)""", RegexOption.IGNORE_CASE
    )

    /**
     * Extracts the human-readable address from a Maps place URL, for
     * geocoding when the URL carries no coordinates.
     *
     * This is the common case for shared short links: resolving
     * maps.app.goo.gl gives a URL with a full address and a Google feature id
     * (`!1s0x...:0x...`) but no lat/lon anywhere - not in the URL, and not in
     * the page body either, which is a JS shell. The address is the only
     * usable signal, so it gets geocoded.
     */
    fun findPlaceQuery(url: String): String? {
        val raw = PLACE_PATH.find(url)?.groupValues?.get(1) ?: return null
        val decoded = try {
            java.net.URLDecoder.decode(raw, "UTF-8")
        } catch (e: Exception) {
            raw.replace('+', ' ')
        }
        val cleaned = decoded.trim().trim(',').trim()
        // A bare coordinate pair in the path is handled by parse(), and a
        // one-word fragment is rarely enough for a geocoder to place.
        return cleaned.takeIf { it.length >= 3 && parse(it) == null }
    }

    /**
     * Maps' share text is typically "Place Name\nhttps://...", so the first
     * non-URL line is a usable label.
     */
    private fun label(text: String): String? =
        text.lineSequence()
            .map { it.trim() }
            .firstOrNull { it.isNotEmpty() && !it.startsWith("http") && !it.startsWith("geo:") }
            ?.take(47)
}
