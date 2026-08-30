package dev.espmaps.nav

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class MapsLinkParserTest {

    private fun assertCoords(expLat: Double, expLon: Double, d: MapsLinkParser.Dest?) {
        assertNotNull(d)
        assertEquals(expLat, d!!.lat, 1e-6)
        assertEquals(expLon, d.lon, 1e-6)
    }

    /**
     * !3d/!4d are the true place coordinates; the @lat,lon earlier in the same
     * URL is the viewport centre and can be some way off. The parser must
     * prefer the former, so this URL deliberately contains both.
     */
    @Test
    fun `place url prefers 3d4d over the viewport centre`() {
        val url = "https://www.google.com/maps/place/Charminar/" +
                  "@17.3616,78.4700,17z/data=!3m1!4b1!4m6!3m5!1s0x0:0x0" +
                  "!8m2!3d17.3616159!4d78.4746save"
        assertCoords(17.3616159, 78.4746, MapsLinkParser.parse(url))
    }

    @Test
    fun `at coordinates when there is nothing better`() {
        assertCoords(
            51.5074, -0.1278,
            MapsLinkParser.parse("https://www.google.com/maps/@51.5074,-0.1278,15z")
        )
    }

    @Test
    fun `query and directions parameters`() {
        assertCoords(17.385, 78.4867,
            MapsLinkParser.parse("https://maps.google.com/?q=17.385,78.4867"))
        assertCoords(17.385, 78.4867,
            MapsLinkParser.parse(
                "https://www.google.com/maps/dir/?api=1&destination=17.385,78.4867"))
        assertCoords(17.385, 78.4867,
            MapsLinkParser.parse("https://maps.google.com/maps?daddr=17.385,78.4867"))
    }

    @Test
    fun `geo uri`() {
        assertCoords(17.385, 78.4867, MapsLinkParser.parse("geo:17.385,78.4867"))
        assertCoords(17.385, 78.4867,
            MapsLinkParser.parse("geo:17.385,78.4867?q=Somewhere"))
    }

    @Test
    fun `bare coordinates typed by hand`() {
        assertCoords(17.385, 78.4867, MapsLinkParser.parse("17.385, 78.4867"))
        assertCoords(-33.8688, 151.2093, MapsLinkParser.parse("-33.8688,151.2093"))
    }

    @Test
    fun `negative coordinates survive`() {
        assertCoords(-33.8688, -70.6693,
            MapsLinkParser.parse("https://www.google.com/maps/@-33.8688,-70.6693,12z"))
    }

    // ---- short links -----------------------------------------------------

    @Test
    fun `short links are detected but carry no coordinates`() {
        val share = "Charminar\nhttps://maps.app.goo.gl/AbCdEfGh123"
        assertNull("must not invent coordinates", MapsLinkParser.parse(share))
        assertEquals("https://maps.app.goo.gl/AbCdEfGh123",
            MapsLinkParser.findShortLink(share))
    }

    @Test
    fun `legacy goo dot gl maps short link`() {
        assertEquals("https://goo.gl/maps/XyZ123",
            MapsLinkParser.findShortLink("see https://goo.gl/maps/XyZ123 ok"))
    }

    @Test
    fun `no short link in a full url`() {
        assertNull(MapsLinkParser.findShortLink(
            "https://www.google.com/maps/@51.5074,-0.1278,15z"))
    }

    // ---- labels and rejection -------------------------------------------

    @Test
    fun `label comes from the first non-url line`() {
        val d = MapsLinkParser.parse("Charminar\nhttps://maps.google.com/?q=17.385,78.4867")
        assertEquals("Charminar", d!!.label)
    }

    @Test
    fun `rubbish yields null`() {
        assertNull(MapsLinkParser.parse("have a look at this place"))
        assertNull(MapsLinkParser.parse(""))
        assertNull(MapsLinkParser.parse("https://example.com/nothing/here"))
    }

    // ---- address extraction ---------------------------------------------
    // The common real-world case: a short link resolves to a URL with a full
    // address and a Google feature id, but no coordinates anywhere.

    @Test
    fun `address extracted from a resolved place url with no coordinates`() {
        val url = "https://www.google.com/maps/place/" +
                  "Crispy+Things,+Whisper+Vly+Rd,+Ambedkar+Nagar,+Shaikpet," +
                  "+Hyderabad,+Telangana+500104/" +
                  "data=!4m2!3m1!1s0x3bcb97b47afaf027:0xc4a6a0a0e86d78d0!18m1!1e1"

        assertNull("this URL genuinely has no coordinates", MapsLinkParser.parse(url))
        assertEquals(
            "Crispy Things, Whisper Vly Rd, Ambedkar Nagar, Shaikpet, " +
                "Hyderabad, Telangana 500104",
            MapsLinkParser.findPlaceQuery(url)
        )
    }

    @Test
    fun `percent escapes are decoded in the address`() {
        assertEquals("Café Rouge, London",
            MapsLinkParser.findPlaceQuery(
                "https://www.google.com/maps/place/Caf%C3%A9+Rouge,+London/data=!4m2"))
    }

    @Test
    fun `search paths also yield a query`() {
        assertEquals("pizza near me",
            MapsLinkParser.findPlaceQuery(
                "https://www.google.com/maps/search/pizza+near+me"))
    }

    /** If the path is already coordinates, parse() handles it - not geocoding. */
    @Test
    fun `coordinate path is not returned as an address`() {
        assertNull(MapsLinkParser.findPlaceQuery(
            "https://www.google.com/maps/place/17.385,78.4867"))
    }

    @Test
    fun `no place segment yields no query`() {
        assertNull(MapsLinkParser.findPlaceQuery("https://example.com/foo/bar"))
        assertNull(MapsLinkParser.findPlaceQuery(
            "https://www.google.com/maps/@51.5074,-0.1278,15z"))
    }

    /** 0,0 is Null Island and is almost always a parse artefact. */
    @Test
    fun `out of range and null island rejected`() {
        assertNull(MapsLinkParser.parse("geo:0,0"))
        assertNull(MapsLinkParser.parse("999.0, 999.0"))
        assertNull(MapsLinkParser.parse("91.5, 0.5"))
    }
}
