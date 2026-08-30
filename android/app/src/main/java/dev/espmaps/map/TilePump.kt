package dev.espmaps.map

import android.util.Log
import dev.espmaps.ble.BleLink
import dev.espmaps.ble.Proto
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.sync.withPermit
import java.util.concurrent.ConcurrentHashMap
import java.util.zip.CRC32

/**
 * Turns tile requests from the ESP32 into ETIL byte streams on the wire.
 *
 * Fetch and encode run concurrently (network-bound), but sending is naturally
 * serialised by BleLink's credit pump. In-flight requests are deduplicated:
 * the firmware re-asks for tiles whose request timed out, and answering the
 * same tile twice would waste the scarce resource in the system.
 */
class TilePump(
    private val link: BleLink,
    private val source: TileSource,
    private val scope: CoroutineScope,
) {
    private val inFlight = ConcurrentHashMap.newKeySet<Long>()

    // A handful of parallel fetches keeps the radio fed without stampeding
    // the tile server when the view jumps.
    private val gate = Semaphore(3)

    // Fetching and encoding run in parallel, but transmission must not: the
    // firmware reassembles one tile at a time and rejects a chunk whose index
    // is not the one it expects. Interleaving two tiles' chunks would abort
    // both. This mutex is what keeps the parallel fetches from doing that.
    private val sendLock = Mutex()

    var tilesSent = 0; private set
    var tilesEmpty = 0; private set
    var bytesEncoded = 0L; private set

    private fun key(z: Int, x: Int, y: Int): Long =
        (z.toLong() shl 58) or (x.toLong() shl 29) or y.toLong()

    fun request(reqs: List<BleLink.TileRequest>) {
        for (r in reqs.sortedBy { it.prio }) {
            val k = key(r.z, r.x, r.y)
            if (!inFlight.add(k)) continue

            scope.launch(Dispatchers.IO) {
                try {
                    gate.withPermit { serve(r.z, r.x, r.y) }
                } catch (e: Exception) {
                    Log.w(TAG, "tile ${r.z}/${r.x}/${r.y}: ${e.message}")
                } finally {
                    inFlight.remove(k)
                }
            }
        }
    }

    private suspend fun serve(z: Int, x: Int, y: Int) {
        // Network and CPU work first, outside the send lock.
        val raw = source.fetch(z, x, y)
        val etil = if (raw == null) null else TileEncoder.encode(raw, z)

        if (etil == null) {
            sendLock.withLock { sendEmpty(z, x, y) }
            tilesEmpty++
            return
        }

        bytesEncoded += etil.size
        tilesSent++

        Log.i(TAG, "tile $z/$x/$y: ${raw!!.size} B pbf -> ${etil.size} B etil " +
                   "(${100 * etil.size / raw.size}%)")

        // The firmware reassembles into a fixed buffer and drops anything
        // larger, which shows up as a blank area rather than an error.
        if (etil.size > 48 * 1024) {
            Log.w(TAG, "tile $z/$x/$y encoded to ${etil.size / 1024} KB - well over " +
                       "the 10-20 KB budget. Check classify() is dropping the " +
                       "heavy layers; at this size it will also be slow over BLE.")
        }

        sendLock.withLock { transmit(z, x, y, etil) }
    }

    private fun transmit(z: Int, x: Int, y: Int, etil: ByteArray) {
        val chunkSize = link.chunkSize - 2   // 2 bytes of chunk index
        val chunks = (etil.size + chunkSize - 1) / chunkSize

        val start = Proto.le(15)
        start.put(z.toByte())
        start.putInt(x)
        start.putInt(y)
        start.putInt(etil.size)
        start.putShort(chunks.toShort())
        link.send(Proto.MSG_TILE_START, start.array())

        var off = 0
        var index = 0
        while (off < etil.size) {
            val n = minOf(chunkSize, etil.size - off)
            val body = Proto.le(2 + n)
            body.putShort(index.toShort())
            body.put(etil, off, n)
            link.send(Proto.MSG_TILE_CHUNK, body.array())
            off += n
            index++
        }

        val crc = CRC32().apply { update(etil) }.value
        val end = Proto.le(4)
        end.putInt(crc.toInt())
        link.send(Proto.MSG_TILE_END, end.array())
    }

    private fun sendEmpty(z: Int, x: Int, y: Int) {
        val b = Proto.le(9)
        b.put(z.toByte())
        b.putInt(x)
        b.putInt(y)
        link.send(Proto.MSG_TILE_EMPTY, b.array())
    }

    companion object { private const val TAG = "TilePump" }
}
