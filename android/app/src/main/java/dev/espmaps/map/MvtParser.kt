package dev.espmaps.map

import java.io.ByteArrayOutputStream
import java.util.zip.GZIPInputStream

/**
 * Minimal Mapbox Vector Tile reader.
 *
 * Hand-rolled rather than generated: the MVT schema is six messages and we
 * only read four fields from it, so pulling in protobuf codegen would cost
 * more in build complexity than it saves. Reads only what the encoder needs -
 * layer name, extent, the `class`/`subclass` attributes, and geometry.
 */

const val GEOM_UNKNOWN = 0
const val GEOM_POINT = 1
const val GEOM_LINESTRING = 2
const val GEOM_POLYGON = 3

class MvtFeature(
    val geomType: Int,
    val tags: IntArray,
    val geometry: IntArray,
)

class MvtLayer(
    val name: String,
    val extent: Int,
    val keys: List<String>,
    val values: List<String?>,
    val features: List<MvtFeature>,
) {
    /** Attribute lookup by key name; null when the feature lacks it. */
    fun attr(f: MvtFeature, key: String): String? {
        val ki = keys.indexOf(key)
        if (ki < 0) return null
        var i = 0
        while (i + 1 < f.tags.size) {
            if (f.tags[i] == ki) return values.getOrNull(f.tags[i + 1])
            i += 2
        }
        return null
    }
}

private class PbReader(val buf: ByteArray, var pos: Int = 0, val end: Int = buf.size) {
    fun hasMore() = pos < end

    fun varint(): Long {
        var result = 0L
        var shift = 0
        while (pos < end) {
            val b = buf[pos++].toInt()
            result = result or ((b and 0x7F).toLong() shl shift)
            if (b and 0x80 == 0) return result
            shift += 7
            if (shift > 63) break
        }
        return result
    }

    fun varintInt(): Int = varint().toInt()

    fun skipField(wireType: Int) {
        when (wireType) {
            0 -> varint()
            1 -> pos += 8
            2 -> pos += varintInt()
            5 -> pos += 4
            else -> pos = end
        }
    }

    fun bytes(): PbReader {
        val len = varintInt()
        val r = PbReader(buf, pos, minOf(pos + len, end))
        pos = minOf(pos + len, end)
        return r
    }

    fun string(): String {
        val len = varintInt()
        val s = String(buf, pos, minOf(len, end - pos), Charsets.UTF_8)
        pos = minOf(pos + len, end)
        return s
    }

    /** Packed repeated uint32, which is how tags and geometry arrive. */
    fun packedInts(): IntArray {
        val len = varintInt()
        val stop = minOf(pos + len, end)
        val out = ArrayList<Int>(len)
        while (pos < stop) out.add(varint().toInt())
        pos = stop
        return out.toIntArray()
    }
}

object MvtParser {

    /** Strips HTTP-level or embedded gzip. Some tile servers do one, some the other. */
    fun maybeGunzip(data: ByteArray): ByteArray {
        if (data.size < 2 || data[0] != 0x1F.toByte() || data[1] != 0x8B.toByte())
            return data
        return GZIPInputStream(data.inputStream()).use { gz ->
            val out = ByteArrayOutputStream(data.size * 4)
            gz.copyTo(out)
            out.toByteArray()
        }
    }

    fun parse(raw: ByteArray): List<MvtLayer> {
        val data = maybeGunzip(raw)
        val r = PbReader(data)
        val layers = ArrayList<MvtLayer>()

        while (r.hasMore()) {
            val tag = r.varintInt()
            val field = tag ushr 3
            val wire = tag and 0x7
            if (field == 3 && wire == 2) {
                layers.add(parseLayer(r.bytes()))
            } else {
                r.skipField(wire)
            }
        }
        return layers
    }

    private fun parseLayer(r: PbReader): MvtLayer {
        var name = ""
        var extent = 4096
        val keys = ArrayList<String>()
        val values = ArrayList<String?>()
        val features = ArrayList<MvtFeature>()

        while (r.hasMore()) {
            val tag = r.varintInt()
            val wire = tag and 0x7
            when (tag ushr 3) {
                1 -> name = r.string()
                2 -> features.add(parseFeature(r.bytes()))
                3 -> keys.add(r.string())
                4 -> values.add(parseValue(r.bytes()))
                5 -> extent = r.varintInt()
                15 -> r.varint()   // version
                else -> r.skipField(wire)
            }
        }
        return MvtLayer(name, extent, keys, values, features)
    }

    /**
     * Values are a union of seven types. Everything is coerced to String
     * because the only attributes we consult (`class`, `subclass`) are
     * strings, and comparing as text keeps the encoder simple.
     */
    private fun parseValue(r: PbReader): String? {
        var out: String? = null
        while (r.hasMore()) {
            val tag = r.varintInt()
            val wire = tag and 0x7
            when (tag ushr 3) {
                1 -> out = r.string()
                2 -> { out = java.lang.Float.intBitsToFloat(readFixed32(r)).toString() }
                3 -> { out = java.lang.Double.longBitsToDouble(readFixed64(r)).toString() }
                4 -> out = r.varint().toString()
                5 -> out = r.varint().toString()
                6 -> { val v = r.varint(); out = ((v ushr 1) xor -(v and 1)).toString() }
                7 -> out = if (r.varint() != 0L) "true" else "false"
                else -> r.skipField(wire)
            }
        }
        return out
    }

    private fun readFixed32(r: PbReader): Int {
        var v = 0
        for (i in 0 until 4) v = v or ((r.buf[r.pos + i].toInt() and 0xFF) shl (i * 8))
        r.pos += 4
        return v
    }

    private fun readFixed64(r: PbReader): Long {
        var v = 0L
        for (i in 0 until 8) v = v or ((r.buf[r.pos + i].toLong() and 0xFF) shl (i * 8))
        r.pos += 8
        return v
    }

    private fun parseFeature(r: PbReader): MvtFeature {
        var geomType = GEOM_UNKNOWN
        var tags = IntArray(0)
        var geom = IntArray(0)

        while (r.hasMore()) {
            val tag = r.varintInt()
            val wire = tag and 0x7
            when (tag ushr 3) {
                1 -> r.varint()             // id
                2 -> tags = r.packedInts()
                3 -> geomType = r.varintInt()
                4 -> geom = r.packedInts()
                else -> r.skipField(wire)
            }
        }
        return MvtFeature(geomType, tags, geom)
    }

    /**
     * Decodes MVT command geometry into rings of absolute tile coordinates.
     *
     * Command integer packs id in the low 3 bits and a repeat count above:
     * MoveTo(1) starts a ring, LineTo(2) extends it, ClosePath(7) ends it.
     * Parameters are zigzag varints, already decoded into the int array.
     */
    fun decodeGeometry(geom: IntArray): List<IntArray> {
        val rings = ArrayList<IntArray>()
        var current = ArrayList<Int>()
        var x = 0
        var y = 0
        var i = 0

        while (i < geom.size) {
            val cmdInt = geom[i++]
            val cmd = cmdInt and 0x7
            val count = cmdInt ushr 3

            when (cmd) {
                1 -> {  // MoveTo - begins a new ring or linestring
                    for (k in 0 until count) {
                        if (i + 1 >= geom.size) break
                        if (current.size >= 4) rings.add(current.toIntArray())
                        current = ArrayList()
                        x += zz(geom[i++])
                        y += zz(geom[i++])
                        current.add(x); current.add(y)
                    }
                }
                2 -> {  // LineTo
                    for (k in 0 until count) {
                        if (i + 1 >= geom.size) break
                        x += zz(geom[i++])
                        y += zz(geom[i++])
                        current.add(x); current.add(y)
                    }
                }
                7 -> {  // ClosePath - implicit return to the ring's first point
                    if (current.size >= 4) {
                        rings.add(current.toIntArray())
                        current = ArrayList()
                    }
                }
                else -> return rings  // unknown command: bail rather than desync
            }
        }
        if (current.size >= 4) rings.add(current.toIntArray())
        return rings
    }

    private fun zz(v: Int): Int = (v ushr 1) xor -(v and 1)
}
