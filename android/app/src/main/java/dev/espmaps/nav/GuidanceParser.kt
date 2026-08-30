package dev.espmaps.nav

import android.app.Notification
import android.os.Bundle
import dev.espmaps.ble.Maneuver
import java.util.Calendar

/**
 * Parses Google Maps' ongoing navigation notification.
 *
 * Written against real captured output rather than guesswork - see
 * docs/MAPS_NOTIFICATION.md for the raw dumps this was built from.
 *
 * Current Maps uses Android 16's ProgressStyle ("Live Updates") template,
 * which is a much better source than the old text-only notification:
 *
 *   android.progress      distance travelled along the route, metres
 *   android.progressMax   total route distance, metres
 *   android.title         the maneuver instruction, e.g. "Head toward St 2"
 *   android.subText       arrival clock time, e.g. "Arrive 11:43 am"
 *   ...primaryInfo        ALTERNATES between distance-to-turn ("0 m") and
 *                         the instruction ("towards St 2")
 *   ...secondaryInfo      instruction, or empty
 *
 * The alternation on primaryInfo is why distance is sticky here: it is only
 * updated when the field actually parses as a distance.
 */
object GuidanceParser {

    private const val K_PRIMARY = "android.ongoingActivityNoti.primaryInfo"
    private const val K_SECONDARY = "android.ongoingActivityNoti.secondaryInfo"
    private const val K_CHIP = "android.ongoingActivityNoti.chipExpandedText"
    private const val K_NOWBAR = "android.ongoingActivityNoti.nowbarPrimaryInfo"

    /** Titles that are status, not guidance. Keep the previous banner. */
    private val TRANSIENT = Regex(
        """^\s*(rerouting|starting navigation|searching|finding|loading|gps)""",
        RegexOption.IGNORE_CASE
    )

    /** Last distance seen, because primaryInfo alternates away from it. */
    private var stickyDistanceM = 0

    fun reset() {
        stickyDistanceM = 0
    }

    /**
     * The notification fields this parser cares about, lifted out of the
     * Bundle so the logic below is plain JVM and can be unit-tested without
     * an emulator or Robolectric.
     */
    data class Fields(
        val title: String?,
        val subText: String? = null,
        val primary: String? = null,
        val secondary: String? = null,
        val chip: String? = null,
        val nowbar: String? = null,
        val progress: Int = -1,
        val progressMax: Int = -1,
    )

    fun parse(x: Bundle): MapsNav.Guidance? = parse(
        Fields(
            title = x.getCharSequence(Notification.EXTRA_TITLE)?.toString(),
            subText = x.getCharSequence(Notification.EXTRA_SUB_TEXT)?.toString(),
            primary = x.getCharSequence(K_PRIMARY)?.toString(),
            secondary = x.getCharSequence(K_SECONDARY)?.toString(),
            chip = x.getCharSequence(K_CHIP)?.toString(),
            nowbar = x.getCharSequence(K_NOWBAR)?.toString(),
            progress = x.getInt("android.progress", -1),
            progressMax = x.getInt("android.progressMax", -1),
        )
    )

    fun parse(f: Fields): MapsNav.Guidance? {
        val title = f.title?.trim() ?: return null

        // "Rerouting..." / "Starting navigation" carry no usable guidance.
        // Returning null leaves the previous banner up, which is much better
        // than blanking the display every time Maps recalculates.
        if (TRANSIENT.containsMatchIn(title)) return null

        val sub = f.subText
        val primary = f.primary
        val secondary = f.secondary
        val chip = f.chip
        val nowbar = f.nowbar

        // Distance to the next maneuver. Only one of these fields carries it
        // at any given update, so take the first that parses and remember it.
        val dist = parseDistance(primary)
            ?: parseDistance(chip)
            ?: parseDistance(nowbar)
            ?: parseDistance(title)
        if (dist != null) stickyDistanceM = dist

        // Remaining distance straight from the ProgressStyle fields.
        val progress = f.progress
        val progressMax = f.progressMax
        val remainingM =
            if (progressMax > 0 && progress in 0..progressMax) progressMax - progress
            else 0

        val etaEpoch = parseArrivalClock(sub)
        val nowS = System.currentTimeMillis() / 1000L
        val remainingS = if (etaEpoch > 0) (etaEpoch - nowS).coerceAtLeast(0L).toInt() else 0

        // The title is the authoritative instruction. Strip the leading verb
        // where there is a road name behind it, else keep the title whole so
        // "Head west" still reads sensibly.
        val instruction = extractStreet(title)
            ?: secondary?.takeIf { it.isNotBlank() && parseDistance(it) == null }
            ?: title

        return MapsNav.Guidance(
            maneuver = parseManeuver(title),
            distanceM = stickyDistanceM,
            instruction = instruction.take(47),
            etaEpochS = etaEpoch,
            remainingM = remainingM,
            remainingS = remainingS,
        )
    }

    // ---- distances -------------------------------------------------------

    private val DIST = Regex(
        """(?<![\w.])(\d+(?:[.,]\d+)?)\s*(km|m|mi|ft|yd)(?![\w])""",
        RegexOption.IGNORE_CASE
    )

    /** Returns metres, or null if the string carries no distance. */
    fun parseDistance(s: String?): Int? {
        if (s.isNullOrBlank()) return null
        val m = DIST.find(s) ?: return null
        val v = m.groupValues[1].replace(',', '.').toDoubleOrNull() ?: return null
        return when (m.groupValues[2].lowercase()) {
            "km" -> (v * 1000).toInt()
            "m" -> v.toInt()
            "mi" -> (v * 1609.34).toInt()
            "ft" -> (v * 0.3048).toInt()
            "yd" -> (v * 0.9144).toInt()
            else -> null
        }
    }

    // ---- arrival time ----------------------------------------------------

    private val CLOCK = Regex("""(\d{1,2}):(\d{2})\s*([ap])\.?m\.?""", RegexOption.IGNORE_CASE)
    private val CLOCK24 = Regex("""(\d{1,2}):(\d{2})""")

    /**
     * "Arrive 11:43 am" -> epoch seconds. Maps gives a wall-clock arrival
     * time rather than a duration, which is actually more useful: it stays
     * correct even if our own clock drifts between updates.
     */
    fun parseArrivalClock(s: String?): Long {
        if (s.isNullOrBlank()) return 0

        var hour: Int
        val minute: Int

        val ampm = CLOCK.find(s)
        if (ampm != null) {
            hour = ampm.groupValues[1].toIntOrNull() ?: return 0
            minute = ampm.groupValues[2].toIntOrNull() ?: return 0
            val pm = ampm.groupValues[3].lowercase() == "p"
            if (pm && hour != 12) hour += 12
            if (!pm && hour == 12) hour = 0
        } else {
            val m24 = CLOCK24.find(s) ?: return 0
            hour = m24.groupValues[1].toIntOrNull() ?: return 0
            minute = m24.groupValues[2].toIntOrNull() ?: return 0
        }
        if (hour !in 0..23 || minute !in 0..59) return 0

        val c = Calendar.getInstance().apply {
            set(Calendar.HOUR_OF_DAY, hour)
            set(Calendar.MINUTE, minute)
            set(Calendar.SECOND, 0)
            set(Calendar.MILLISECOND, 0)
        }
        // An arrival time well in the past means the journey crosses midnight.
        if (c.timeInMillis < System.currentTimeMillis() - 2 * 3600_000L) {
            c.add(Calendar.DAY_OF_YEAR, 1)
        }
        return c.timeInMillis / 1000L
    }

    // ---- maneuver --------------------------------------------------------

    /**
     * Keyword-based, deliberately: the drawables Maps ships change far more
     * often than its wording. Order matters - the specific cases have to be
     * tested before the bare "left"/"right" fallbacks, or a road called
     * "Left Bank Road" turns into a left turn.
     */
    fun parseManeuver(s: String): Int {
        val t = s.lowercase()
        return when {
            t.contains("u-turn") || t.contains("make a u") -> Maneuver.UTURN
            t.contains("roundabout") || t.contains("rotary") -> Maneuver.ROUNDABOUT
            t.contains("arriv") || t.contains("destination") -> Maneuver.ARRIVE
            t.contains("merge") -> Maneuver.MERGE
            t.contains("sharp left") -> Maneuver.SHARP_LEFT
            t.contains("sharp right") -> Maneuver.SHARP_RIGHT
            t.contains("slight left") || t.contains("bear left") -> Maneuver.SLIGHT_LEFT
            t.contains("slight right") || t.contains("bear right") -> Maneuver.SLIGHT_RIGHT
            t.contains("keep left") || t.contains("exit left") -> Maneuver.FORK_LEFT
            t.contains("keep right") || t.contains("exit right") -> Maneuver.FORK_RIGHT
            t.contains("turn left") || t.contains("left onto") -> Maneuver.LEFT
            t.contains("turn right") || t.contains("right onto") -> Maneuver.RIGHT
            // "Head west", "Head toward X", "Continue on X" are all straight.
            t.startsWith("head") || t.contains("continue") ||
                t.contains("straight") || t.contains("stay on") -> Maneuver.STRAIGHT
            t.contains(" left") -> Maneuver.LEFT
            t.contains(" right") -> Maneuver.RIGHT
            else -> Maneuver.STRAIGHT
        }
    }

    private val ONTO = Regex(
        """\b(?:onto|on to|towards?|toward)\s+(.+)$""", RegexOption.IGNORE_CASE
    )

    /**
     * Pulls the road name out of "Head toward St 2" -> "St 2".
     * Returns null for "Head west", where there is no road name to extract
     * and the caller should keep the title as-is.
     */
    fun extractStreet(head: String): String? =
        ONTO.find(head)?.groupValues?.get(1)?.trim()?.trimEnd('.', ',')
            ?.takeIf { it.isNotBlank() }
}
