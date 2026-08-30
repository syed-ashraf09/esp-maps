package dev.espmaps.map

import dev.espmaps.ble.Proto
import dev.espmaps.ble.StyleClass
import dev.espmaps.ble.Varint
import java.io.ByteArrayOutputStream

/**
 * Turns an OpenMapTiles MVT into the compact ETIL format the ESP32 decodes.
 *
 * This is the single most important piece of the bandwidth budget. A raw z14
 * .pbf is 40-120 KB and is mostly labels, POIs, house numbers and attributes
 * that a 368x448 navigation display will never draw. Three reductions apply,
 * in order of how much they save:
 *
 *   1. drop every layer we do not render      (~60-70%)
 *   2. Douglas-Peucker to display resolution  (~3-5x on what remains)
 *   3. delta + zigzag varint coordinates      (~2x over raw int16)
 *
 * Together: 4-15 KB per tile, which is 2 seconds over BLE instead of 12.
 */
object TileEncoder {

    /** Coordinate space on the wire. Half MVT's 4096 - still ~0.2 px at z17. */
    const val OUT_EXTENT = 2048

    /** Douglas-Peucker tolerance, in OUT_EXTENT units. ~0.7 px on a 368 px display. */
    const val SIMPLIFY_TOLERANCE = 4.0

    private const val COORD_CLAMP = 8000

    private class OutFeature(val pts: IntArray, val ringContinues: Boolean)
    private class OutLayer(val kind: Int, val styleClass: Int) {
        val features = ArrayList<OutFeature>()
    }

    /**
     * @return ETIL bytes, or null if the tile has nothing worth drawing (the
     *         caller should answer MSG_TILE_EMPTY, which caches as a positive
     *         "nothing here" instead of provoking a retry).
     */
    fun encode(mvt: ByteArray, dataZoom: Int): ByteArray? {
        val layers = try {
            MvtParser.parse(mvt)
        } catch (e: Exception) {
            return null
        }

        // Keyed by (kind shl 8) or styleClass so features of the same class
        // from different source layers merge into one output layer.
        val out = LinkedHashMap<Int, OutLayer>()

        for (layer in layers) {
            val scale = OUT_EXTENT.toDouble() / layer.extent

            for (f in layer.features) {
                if (f.geomType == GEOM_POINT || f.geomType == GEOM_UNKNOWN) continue

                val styleClass = classify(layer, f, dataZoom) ?: continue
                val kind = if (f.geomType == GEOM_POLYGON) Proto.ETIL_POLYGON
                           else Proto.ETIL_LINE

                val rings = MvtParser.decodeGeometry(f.geometry)
                if (rings.isEmpty()) continue

                val key = (kind shl 8) or styleClass
                val target = out.getOrPut(key) { OutLayer(kind, styleClass) }

                for ((idx, ring) in rings.withIndex()) {
                    val scaled = scaleAndClamp(ring, scale)
                    val simplified =
                        if (kind == Proto.ETIL_POLYGON && scaled.size <= 8) scaled
                        else Simplify.douglasPeucker(scaled, SIMPLIFY_TOLERANCE)

                    val minPts = if (kind == Proto.ETIL_POLYGON) 3 else 2
                    if (simplified.size / 2 < minPts) continue

                    // Holes follow their exterior ring and are flagged so the
                    // firmware fills them as one even-odd polygon.
                    target.features.add(
                        OutFeature(simplified, idx < rings.size - 1 &&
                                               kind == Proto.ETIL_POLYGON)
                    )
                }
            }
        }

        out.values.removeAll { it.features.isEmpty() }
        if (out.isEmpty()) return null

        return serialise(out.values, dataZoom)
    }

    // ---- layer / class mapping ------------------------------------------

    /**
     * Maps an OpenMapTiles layer + class onto our style enum, or null to drop.
     * Also applies a per-class zoom floor: sending residential streets in a
     * z10 tile wastes bytes on geometry that is never drawn.
     */
    private fun classify(layer: MvtLayer, f: MvtFeature, z: Int): Int? {
        return when (layer.name) {
            "transportation" -> {
                val cls = layer.attr(f, "class") ?: return null
                when (cls) {
                    "motorway" -> StyleClass.ROAD_MOTORWAY
                    "trunk" -> StyleClass.ROAD_TRUNK
                    "primary" -> StyleClass.ROAD_PRIMARY
                    "secondary" -> if (z >= 12) StyleClass.ROAD_SECONDARY else null
                    "tertiary" -> if (z >= 12) StyleClass.ROAD_TERTIARY else null
                    "minor" -> if (z >= 13) StyleClass.ROAD_MINOR else null
                    "service" -> if (z >= 14) StyleClass.ROAD_SERVICE else null
                    "path", "track" -> if (z >= 14) StyleClass.ROAD_PATH else null
                    "rail", "transit" -> if (z >= 12) StyleClass.RAIL else null
                    // Slip roads read better one class thinner than the road
                    // they join, which is also what most map styles do.
                    "motorway_link" -> if (z >= 12) StyleClass.ROAD_TRUNK else null
                    "trunk_link", "primary_link" ->
                        if (z >= 13) StyleClass.ROAD_SECONDARY else null
                    "secondary_link", "tertiary_link" ->
                        if (z >= 13) StyleClass.ROAD_TERTIARY else null
                    else -> null
                }
            }

            "water" -> StyleClass.WATER
            "waterway" -> if (z >= 12) StyleClass.WATERWAY else null

            // Buildings are dropped, and the threshold is set above any data
            // zoom we actually fetch (PROTOCOL section 7 caps it at 14).
            //
            // Measured on a real tile: with buildings included, dense urban
            // z14 encoded to 72-97 KB, which is 5-10x the budget and over the
            // firmware's reassembly buffer. A z14 tile spans ~2 km, so we
            // would be shipping every footprint in 2 km-squared to draw the
            // ~150 m of it that fits on a 368x448 screen. On a navigation
            // display they are decoration; roads are the point.
            "building" -> if (z >= 16) StyleClass.BUILDING else null

            "park" -> if (z >= 11) StyleClass.LANDUSE_PARK else null

            "landuse", "landcover" -> {
                if (z < 11) return null
                when (layer.attr(f, "class")) {
                    "residential", "suburb", "neighbourhood" ->
                        StyleClass.LANDUSE_RESIDENTIAL
                    "industrial", "commercial", "retail" ->
                        StyleClass.LANDUSE_INDUSTRIAL
                    "park", "grass", "wood", "forest", "farmland",
                    "cemetery", "pitch", "garden" -> StyleClass.LANDUSE_PARK
                    else -> null
                }
            }

            "boundary" -> {
                val admin = layer.attr(f, "admin_level")?.toIntOrNull() ?: 99
                if (admin <= 4) StyleClass.BOUNDARY else null
            }

            // aeroway, poi, place, housenumber, transportation_name and the
            // rest are deliberately dropped - they are most of the tile.
            else -> null
        }
    }

    // ---- geometry --------------------------------------------------------

    private fun scaleAndClamp(ring: IntArray, scale: Double): IntArray {
        val out = IntArray(ring.size)
        for (i in ring.indices) {
            val v = Math.round(ring[i] * scale).toInt()
            out[i] = v.coerceIn(-COORD_CLAMP, COORD_CLAMP)
        }
        return out
    }

    // ---- serialisation ---------------------------------------------------

    private fun serialise(layers: Collection<OutLayer>, zoom: Int): ByteArray {
        val payload = ByteArrayOutputStream(8192)

        for (layer in layers) {
            payload.write(layer.kind)
            payload.write(layer.styleClass)
            payload.write(layer.features.size and 0xFF)
            payload.write((layer.features.size ushr 8) and 0xFF)

            for (feat in layer.features) {
                val n = feat.pts.size / 2
                // Point count carries the ring-continues flag in bit 0.
                val packed = (n shl 1) or (if (feat.ringContinues) Proto.RING_CONTINUES else 0)
                Varint.write(payload, packed)

                var px = 0
                var py = 0
                for (i in 0 until n) {
                    val x = feat.pts[i * 2]
                    val y = feat.pts[i * 2 + 1]
                    Varint.writeZigZag(payload, x - px)
                    Varint.writeZigZag(payload, y - py)
                    px = x
                    py = y
                }
            }
        }

        val body = payload.toByteArray()
        val buf = Proto.le(12 + body.size)
        buf.put(Proto.ETIL_MAGIC0.toByte())
        buf.put(Proto.ETIL_MAGIC1.toByte())
        buf.put(Proto.ETIL_VERSION.toByte())
        buf.put(0)                          // flags
        buf.put(zoom.toByte())
        buf.put(layers.size.toByte())
        buf.putShort(OUT_EXTENT.toShort())
        buf.putInt(body.size)
        buf.put(body)
        return buf.array()
    }
}

/** Douglas-Peucker, iterative so a pathological ring cannot blow the stack. */
object Simplify {

    fun douglasPeucker(pts: IntArray, tolerance: Double): IntArray {
        val n = pts.size / 2
        if (n < 3) return pts

        val keep = BooleanArray(n)
        keep[0] = true
        keep[n - 1] = true

        val tol2 = tolerance * tolerance
        val stack = ArrayDeque<Pair<Int, Int>>()
        stack.addLast(0 to n - 1)

        while (stack.isNotEmpty()) {
            val (first, last) = stack.removeLast()
            if (last <= first + 1) continue

            var maxDist = 0.0
            var index = first

            val x1 = pts[first * 2].toDouble()
            val y1 = pts[first * 2 + 1].toDouble()
            val x2 = pts[last * 2].toDouble()
            val y2 = pts[last * 2 + 1].toDouble()

            for (i in first + 1 until last) {
                val d = perpDist2(pts[i * 2].toDouble(), pts[i * 2 + 1].toDouble(),
                                  x1, y1, x2, y2)
                if (d > maxDist) { maxDist = d; index = i }
            }

            if (maxDist > tol2) {
                keep[index] = true
                stack.addLast(first to index)
                stack.addLast(index to last)
            }
        }

        var kept = 0
        for (k in keep) if (k) kept++
        val out = IntArray(kept * 2)
        var j = 0
        for (i in 0 until n) {
            if (!keep[i]) continue
            out[j * 2] = pts[i * 2]
            out[j * 2 + 1] = pts[i * 2 + 1]
            j++
        }
        return out
    }

    /** Squared perpendicular distance from (px,py) to segment (x1,y1)-(x2,y2). */
    private fun perpDist2(px: Double, py: Double,
                          x1: Double, y1: Double, x2: Double, y2: Double): Double {
        var dx = x2 - x1
        var dy = y2 - y1

        if (dx == 0.0 && dy == 0.0) {
            dx = px - x1; dy = py - y1
            return dx * dx + dy * dy
        }

        val t = ((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy)
        val cx: Double
        val cy: Double
        when {
            t < 0 -> { cx = x1; cy = y1 }
            t > 1 -> { cx = x2; cy = y2 }
            else -> { cx = x1 + t * dx; cy = y1 + t * dy }
        }
        val ex = px - cx
        val ey = py - cy
        return ex * ex + ey * ey
    }
}
