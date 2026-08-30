package dev.espmaps.ble

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

/**
 * Wire protocol constants. Mirrors firmware/main/ble/proto.h and
 * docs/PROTOCOL.md. Any change here must be mirrored there.
 */
object Proto {
    const val VERSION = 1
    const val ADV_NAME = "espmaps"

    val SERVICE: UUID = UUID.fromString("7a2b0001-5f3c-4d8e-9a1b-2c3d4e5f6071")
    val RX: UUID = UUID.fromString("7a2b0002-5f3c-4d8e-9a1b-2c3d4e5f6071")  // we write
    val TX: UUID = UUID.fromString("7a2b0003-5f3c-4d8e-9a1b-2c3d4e5f6071")  // we notify
    val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    const val HDR_LEN = 6

    // phone -> esp32
    const val MSG_HELLO = 0x01
    const val MSG_TIME_SYNC = 0x02
    const val MSG_GPS_FIX = 0x10
    const val MSG_TILE_START = 0x20
    const val MSG_TILE_CHUNK = 0x21
    const val MSG_TILE_END = 0x22
    const val MSG_TILE_EMPTY = 0x23
    const val MSG_ROUTE_START = 0x30
    const val MSG_ROUTE_CHUNK = 0x31
    const val MSG_ROUTE_END = 0x32
    const val MSG_ROUTE_CLEAR = 0x33
    const val MSG_MANEUVER = 0x40
    const val MSG_NAV_STATE = 0x50

    // esp32 -> phone
    const val MSG_CREDITS = 0x80
    const val MSG_TILE_REQUEST = 0x81
    const val MSG_TILE_CANCEL = 0x82
    const val MSG_STATUS = 0x83
    const val MSG_VIEW = 0x84
    const val MSG_DEST = 0x85
    const val MSG_LOG = 0x86

    const val GPS_UNKNOWN_U16 = 0xFFFF

    // ETIL tile format
    const val ETIL_MAGIC0 = 0x45  // 'E'
    const val ETIL_MAGIC1 = 0x54  // 'T'
    const val ETIL_VERSION = 1

    const val ETIL_POLYGON = 0
    const val ETIL_LINE = 1
    const val ETIL_POINT = 2

    const val RING_CONTINUES = 0x1

    /** Wraps a payload in the 6-byte packet header. */
    fun frame(type: Int, seq: Int, payload: ByteArray?): ByteArray {
        val len = payload?.size ?: 0
        val b = ByteBuffer.allocate(HDR_LEN + len).order(ByteOrder.LITTLE_ENDIAN)
        b.put(type.toByte())
        b.put(0)                       // flags
        b.putShort(seq.toShort())
        b.putShort(len.toShort())
        if (payload != null) b.put(payload)
        return b.array()
    }

    fun le(capacity: Int): ByteBuffer =
        ByteBuffer.allocate(capacity).order(ByteOrder.LITTLE_ENDIAN)
}

object StyleClass {
    const val BACKGROUND = 0
    const val LANDUSE_PARK = 1
    const val LANDUSE_RESIDENTIAL = 2
    const val LANDUSE_INDUSTRIAL = 3
    const val WATER = 4
    const val WATERWAY = 5
    const val BUILDING = 6
    const val ROAD_MOTORWAY = 7
    const val ROAD_TRUNK = 8
    const val ROAD_PRIMARY = 9
    const val ROAD_SECONDARY = 10
    const val ROAD_TERTIARY = 11
    const val ROAD_MINOR = 12
    const val ROAD_SERVICE = 13
    const val ROAD_PATH = 14
    const val RAIL = 15
    const val BOUNDARY = 16
}

object Maneuver {
    const val NONE = 0
    const val STRAIGHT = 1
    const val SLIGHT_LEFT = 2
    const val LEFT = 3
    const val SHARP_LEFT = 4
    const val ROUNDABOUT = 5
    const val UTURN = 6
    const val DEPART = 7
    const val MERGE = 8
    const val FORK_LEFT = 9
    const val FORK_RIGHT = 10
    const val ARRIVE = 11
    const val SLIGHT_RIGHT = 12
    const val RIGHT = 13
    const val SHARP_RIGHT = 14
}

/** Unsigned LEB128 + zigzag, the encoding used for all ETIL geometry. */
object Varint {
    fun write(out: java.io.ByteArrayOutputStream, value: Int) {
        var v = value
        while (true) {
            if (v and 0x7F.inv() == 0) {
                out.write(v)
                return
            }
            out.write((v and 0x7F) or 0x80)
            v = v ushr 7
        }
    }

    fun writeZigZag(out: java.io.ByteArrayOutputStream, value: Int) {
        write(out, (value shl 1) xor (value shr 31))
    }

    fun sizeOf(value: Int): Int {
        var v = value
        var n = 1
        while (v and 0x7F.inv() != 0) { v = v ushr 7; n++ }
        return n
    }
}
