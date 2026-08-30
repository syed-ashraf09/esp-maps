package dev.espmaps.nav

import dev.espmaps.ble.Maneuver
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.util.Calendar

/**
 * Runs on the JVM - no emulator, no device, no driving.
 *
 * The "captured" tests use verbatim field values from a real Google Maps trip
 * (docs/MAPS_NOTIFICATION.md). The rest cover wording we have not observed
 * yet and are therefore assertions about intent, not about Maps.
 */
class GuidanceParserTest {

    @Before
    fun setUp() = GuidanceParser.reset()

    private fun f(
        title: String?,
        subText: String? = null,
        primary: String? = null,
        secondary: String? = null,
        progress: Int = -1,
        progressMax: Int = -1,
    ) = GuidanceParser.Fields(
        title = title, subText = subText, primary = primary,
        secondary = secondary, progress = progress, progressMax = progressMax
    )

    // ---- captured from a real trip ---------------------------------------

    @Test
    fun `captured - head west at trip start`() {
        val g = GuidanceParser.parse(
            f(
                title = "Head west",
                subText = "Arrive 11:43 am",
                primary = "0 m",
                secondary = "Head west",
                progress = 0,
                progressMax = 12571,
            )
        )
        assertNotNull(g)
        assertEquals(Maneuver.STRAIGHT, g!!.maneuver)
        assertEquals(0, g.distanceM)
        // No road name to extract, so the title survives whole.
        assertEquals("Head west", g.instruction)
        assertEquals(12571, g.remainingM)
    }

    @Test
    fun `captured - road name is extracted from toward`() {
        val g = GuidanceParser.parse(
            f(
                title = "Head toward Bhagavatguda Rd",
                subText = "Arrive 11:44 am",
                primary = "0 m",
                progress = 0,
                progressMax = 12488,
            )
        )
        assertEquals("Bhagavatguda Rd", g!!.instruction)
        assertEquals(Maneuver.STRAIGHT, g.maneuver)
    }

    @Test
    fun `captured - remaining distance is progressMax minus progress`() {
        val g = GuidanceParser.parse(
            f(title = "Head toward St 2", progress = 33, progressMax = 12571)
        )
        assertEquals(12538, g!!.remainingM)
    }

    @Test
    fun `captured - progress past max does not go negative`() {
        val g = GuidanceParser.parse(
            f(title = "Head toward St 2", progress = 99999, progressMax = 12571)
        )
        assertEquals(0, g!!.remainingM)
    }

    @Test
    fun `captured - transient titles keep the previous banner`() {
        assertNull(GuidanceParser.parse(f("Rerouting...", subText = "Arrive")))
        assertNull(GuidanceParser.parse(f("Starting navigation…")))
    }

    /**
     * primaryInfo alternates between the distance and the instruction on
     * successive updates, so the distance has to persist across the ones that
     * do not carry it. This is the single most fragile behaviour here.
     */
    @Test
    fun `captured - distance survives the primaryInfo alternation`() {
        val a = GuidanceParser.parse(
            f(title = "Head toward St 2", primary = "250 m", progressMax = 13734, progress = 0)
        )
        assertEquals(250, a!!.distanceM)

        val b = GuidanceParser.parse(
            f(title = "Head toward St 2", primary = "towards St 2", progressMax = 13734, progress = 0)
        )
        assertEquals("distance must not reset when primaryInfo shows text", 250, b!!.distanceM)
    }

    @Test
    fun `reset clears sticky distance between trips`() {
        GuidanceParser.parse(f(title = "Head north", primary = "800 m"))
        GuidanceParser.reset()
        val g = GuidanceParser.parse(f(title = "Head north", primary = "towards A"))
        assertEquals(0, g!!.distanceM)
    }

    // ---- distances -------------------------------------------------------

    @Test
    fun `distance units`() {
        assertEquals(0, GuidanceParser.parseDistance("0 m"))
        assertEquals(250, GuidanceParser.parseDistance("250 m"))
        assertEquals(1200, GuidanceParser.parseDistance("1.2 km"))
        assertEquals(1200, GuidanceParser.parseDistance("1,2 km"))
        assertEquals(160, GuidanceParser.parseDistance("528 ft"))
        assertEquals(1609, GuidanceParser.parseDistance("1 mi"))
        assertEquals(91, GuidanceParser.parseDistance("100 yd"))
    }

    @Test
    fun `text without a distance yields null`() {
        assertNull(GuidanceParser.parseDistance("towards St 2"))
        assertNull(GuidanceParser.parseDistance("Rerouting..."))
        assertNull(GuidanceParser.parseDistance(""))
        assertNull(GuidanceParser.parseDistance(null))
    }

    /** A road called "St 2" must not read as 2 metres. */
    @Test
    fun `road names are not mistaken for distances`() {
        assertNull(GuidanceParser.parseDistance("Head toward St 2"))
        assertNull(GuidanceParser.parseDistance("Highway 401"))
    }

    // ---- arrival time ----------------------------------------------------

    @Test
    fun `arrival clock 12 hour`() {
        val eta = GuidanceParser.parseArrivalClock("Arrive 11:43 am")
        assertTrue(eta > 0)
        val c = Calendar.getInstance().apply { timeInMillis = eta * 1000 }
        assertEquals(11, c.get(Calendar.HOUR_OF_DAY))
        assertEquals(43, c.get(Calendar.MINUTE))
    }

    @Test
    fun `arrival clock pm converts correctly`() {
        val c = Calendar.getInstance().apply {
            timeInMillis = GuidanceParser.parseArrivalClock("Arrive 1:05 pm") * 1000
        }
        assertEquals(13, c.get(Calendar.HOUR_OF_DAY))
    }

    @Test
    fun `arrival clock midnight noon edge cases`() {
        var c = Calendar.getInstance().apply {
            timeInMillis = GuidanceParser.parseArrivalClock("Arrive 12:30 am") * 1000
        }
        assertEquals(0, c.get(Calendar.HOUR_OF_DAY))

        c = Calendar.getInstance().apply {
            timeInMillis = GuidanceParser.parseArrivalClock("Arrive 12:30 pm") * 1000
        }
        assertEquals(12, c.get(Calendar.HOUR_OF_DAY))
    }

    /** During a reroute subText degrades to a bare "Arrive". */
    @Test
    fun `arrival without a time yields zero`() {
        assertEquals(0, GuidanceParser.parseArrivalClock("Arrive"))
        assertEquals(0, GuidanceParser.parseArrivalClock(null))
    }

    // ---- maneuvers -------------------------------------------------------
    // Not yet observed from real Maps output - these encode intent.

    @Test
    fun `maneuver keywords`() {
        val cases = mapOf(
            "In 200 m, turn left onto High Street" to Maneuver.LEFT,
            "Turn right onto Main St" to Maneuver.RIGHT,
            "Sharp left onto Mill Lane" to Maneuver.SHARP_LEFT,
            "Sharp right" to Maneuver.SHARP_RIGHT,
            "Slight left onto A40" to Maneuver.SLIGHT_LEFT,
            "Bear right" to Maneuver.SLIGHT_RIGHT,
            "Keep left at the fork" to Maneuver.FORK_LEFT,
            "Keep right to stay on M4" to Maneuver.FORK_RIGHT,
            "At the roundabout, take the 2nd exit" to Maneuver.ROUNDABOUT,
            "Make a U-turn" to Maneuver.UTURN,
            "Merge onto M25" to Maneuver.MERGE,
            "Continue on Church Road" to Maneuver.STRAIGHT,
            "Head west" to Maneuver.STRAIGHT,
            "Head toward St 2" to Maneuver.STRAIGHT,
            "Arrive at destination" to Maneuver.ARRIVE,
        )
        for ((text, expected) in cases) {
            assertEquals(text, expected, GuidanceParser.parseManeuver(text))
        }
    }

    /**
     * Order matters in parseManeuver: a road with a direction in its name
     * must not be read as a turn. This is why the bare fallbacks require a
     * leading space and sit last.
     */
    @Test
    fun `direction words inside road names do not become turns`() {
        assertEquals(Maneuver.STRAIGHT, GuidanceParser.parseManeuver("Head toward Leftwich Road"))
        assertEquals(Maneuver.STRAIGHT, GuidanceParser.parseManeuver("Continue on Rightwell Ave"))
    }

    // ---- street extraction ----------------------------------------------

    @Test
    fun `street extraction`() {
        assertEquals("St 2", GuidanceParser.extractStreet("Head toward St 2"))
        assertEquals("High Street", GuidanceParser.extractStreet("Turn left onto High Street"))
        assertEquals("Bhagavatguda Rd", GuidanceParser.extractStreet("Head towards Bhagavatguda Rd"))
        assertNull(GuidanceParser.extractStreet("Head west"))
        assertNull(GuidanceParser.extractStreet("Rerouting..."))
    }

    @Test
    fun `instruction is truncated to the wire limit`() {
        val long = "Head toward " + "A".repeat(200)
        val g = GuidanceParser.parse(f(title = long))
        assertTrue(g!!.instruction.length <= 47)
    }
}
