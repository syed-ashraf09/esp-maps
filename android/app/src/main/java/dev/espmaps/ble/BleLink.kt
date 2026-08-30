package dev.espmaps.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.content.BroadcastReceiver
import android.content.Intent
import android.content.IntentFilter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.SystemClock
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * GATT client for the ESP32.
 *
 * Two details carry almost all the throughput:
 *
 *   - MTU 517 + CONNECTION_PRIORITY_HIGH + LE 2M PHY. Skipping any of these
 *     costs roughly 5x. Requested in onServicesDiscovered, in that order.
 *   - Credit accounting. WRITE_TYPE_NO_RESPONSE will happily overrun the
 *     peer's reassembly buffer with no backpressure of its own; the ESP32
 *     grants credits and we spend them. Without this, roughly a third of
 *     packets vanish silently on a busy link.
 */
@SuppressLint("MissingPermission")
class BleLink(
    private val context: Context,
    private val listener: Listener,
) {
    interface Listener {
        fun onConnectionChanged(connected: Boolean)
        fun onTilesRequested(tiles: List<TileRequest>)
        fun onViewChanged(lat: Double, lon: Double, zoom: Int, bearingDeg: Float)
        fun onDestinationPicked(lat: Double, lon: Double)
    }

    data class TileRequest(val z: Int, val x: Int, val y: Int, val prio: Int)

    companion object {
        private const val TAG = "BleLink"
        private const val SCAN_TIMEOUT_MS = 20_000L
        /** How long to wait for a write completion before assuming it was lost. */
        private const val WRITE_TIMEOUT_MS = 3_000L

        /**
         * Hard ceiling on one GATT attribute write. BluetoothGatt throws
         * IllegalArgumentException above this regardless of the negotiated
         * MTU, so mtu-3 (514 at MTU 517) is NOT a safe bound.
         */
        private const val MAX_ATT_WRITE = 512
    }

    private val handler = Handler(Looper.getMainLooper())
    private val adapter: BluetoothAdapter? =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var txChar: BluetoothGattCharacteristic? = null

    @Volatile var mtu = 23; private set
    @Volatile var connected = false; private set

    /** Usable payload per write, after ATT (3) and our own header (6). */
    /**
     * Largest payload we can hand to send(), which frames it with HDR_LEN
     * bytes on top. Bounded by BOTH the negotiated MTU and Android's 512-byte
     * attribute cap - at MTU 517 the cap is what binds.
     */
    val chunkSize: Int
        get() = (minOf(mtu - 3, MAX_ATT_WRITE) - Proto.HDR_LEN).coerceAtLeast(20)

    private var seq = 0
    private var credits = 0
    // @Volatile AND only mutated under synchronized(txQueue). The GATT
    // callback runs on a binder thread while pump() is driven from coroutine
    // threads; without a happens-before edge between them, a producer can read
    // a stale "true" and stop writing forever after a single packet.
    @Volatile private var writeInFlight = false
    private var lastWriteAt = 0L
    private val txQueue = ArrayDeque<ByteArray>()

    var bytesSent = 0L; private set

    // ---- scan / connect --------------------------------------------------

    private var scanning = false

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val name = result.device?.name ?: result.scanRecord?.deviceName
            if (name == Proto.ADV_NAME) {
                Log.i(TAG, "found ${result.device.address}, connecting")
                stopScan()
                connectTo(result.device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            Log.e(TAG, "scan failed: $errorCode")
            scanning = false
        }
    }

    val bluetoothOn: Boolean get() = adapter?.isEnabled == true

    /**
     * Bluetooth can be switched on after the service starts, and there is no
     * callback for "a scanner became available". Without this receiver the
     * first failed startScan() was permanent: the user turns Bluetooth on and
     * nothing whatsoever happens.
     */
    private val btStateReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context?, i: Intent?) {
            if (i?.action != BluetoothAdapter.ACTION_STATE_CHANGED) return
            when (i.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1)) {
                BluetoothAdapter.STATE_ON -> {
                    Log.i(TAG, "Bluetooth turned on - starting scan")
                    scanning = false          // stale flag from a failed attempt
                    startScan()
                }
                BluetoothAdapter.STATE_TURNING_OFF, BluetoothAdapter.STATE_OFF -> {
                    Log.w(TAG, "Bluetooth turned off - link down")
                    scanning = false
                    gatt?.close()
                    gatt = null
                    setConnected(false)
                }
            }
        }
    }

    private var receiverRegistered = false

    fun start() {
        if (!receiverRegistered) {
            context.registerReceiver(
                btStateReceiver,
                IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED)
            )
            receiverRegistered = true
        }
        startScan()
    }

    fun startScan() {
        if (adapter == null) {
            Log.e(TAG, "device has no Bluetooth adapter")
            return
        }
        if (!adapter.isEnabled) {
            // Not an error worth retrying in a loop - btStateReceiver will
            // kick the scan off the moment Bluetooth comes on.
            Log.w(TAG, "Bluetooth is OFF - waiting for it to be enabled")
            return
        }
        val scanner = adapter.bluetoothLeScanner ?: run {
            Log.e(TAG, "no BLE scanner despite adapter being enabled")
            return
        }
        if (scanning) return
        scanning = true

        // Filtering on the service UUID rather than the name means we match
        // the scan response the firmware puts it in.
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(Proto.SERVICE))
            .build()

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanner.startScan(listOf(filter), settings, scanCallback)
        Log.i(TAG, "scanning for \"${Proto.ADV_NAME}\"")

        handler.postDelayed({ if (scanning) { stopScan(); startScan() } }, SCAN_TIMEOUT_MS)
    }

    fun stopScan() {
        if (!scanning) return
        scanning = false
        adapter?.bluetoothLeScanner?.stopScan(scanCallback)
    }

    private fun connectTo(device: BluetoothDevice) {
        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    fun disconnect() {
        if (receiverRegistered) {
            runCatching { context.unregisterReceiver(btStateReceiver) }
            receiverRegistered = false
        }
        stopScan()
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        setConnected(false)
    }

    private val pumpTicker = object : Runnable {
        override fun run() {
            if (!connected) return
            pump()
            handler.postDelayed(this, 1_000)
        }
    }

    private fun setConnected(v: Boolean) {
        if (connected == v) return
        connected = v
        if (v) handler.post(pumpTicker) else handler.removeCallbacks(pumpTicker)
        if (!v) {
            credits = 0
            writeInFlight = false
            synchronized(txQueue) { txQueue.clear() }
        }
        listener.onConnectionChanged(v)
    }

    // ---- GATT callbacks --------------------------------------------------

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.i(TAG, "connected, discovering services")
                g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                g.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.w(TAG, "disconnected (status $status)")
                setConnected(false)
                g.close()
                gatt = null
                handler.postDelayed({ startScan() }, 1500)
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val svc = g.getService(Proto.SERVICE) ?: run {
                Log.e(TAG, "service not found")
                g.disconnect()
                return
            }
            rxChar = svc.getCharacteristic(Proto.RX)
            txChar = svc.getCharacteristic(Proto.TX)

            if (rxChar == null || txChar == null) {
                Log.e(TAG, "characteristics missing")
                g.disconnect()
                return
            }

            // Order matters: MTU first, then PHY, then subscribe. Requesting
            // the MTU after enabling notifications loses the larger payload
            // on some stacks.
            g.requestMtu(517)
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            mtu = newMtu
            Log.i(TAG, "MTU $newMtu (payload $chunkSize)")

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                g.setPreferredPhy(
                    BluetoothDevice.PHY_LE_2M_MASK,
                    BluetoothDevice.PHY_LE_2M_MASK,
                    BluetoothDevice.PHY_OPTION_NO_PREFERRED
                )
            }
            enableNotifications(g)
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int
        ) {
            synchronized(txQueue) { writeInFlight = false }
            pump()
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic
        ) {
            @Suppress("DEPRECATION")
            handleNotification(ch.value ?: return)
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray
        ) {
            handleNotification(value)
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int
        ) {
            Log.i(TAG, "notifications enabled, link ready")
            setConnected(true)
            sendHello()
        }
    }

    private fun enableNotifications(g: BluetoothGatt) {
        val ch = txChar ?: return
        g.setCharacteristicNotification(ch, true)
        val cccd = ch.getDescriptor(Proto.CCCD) ?: return

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
        } else {
            @Suppress("DEPRECATION")
            cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            @Suppress("DEPRECATION")
            g.writeDescriptor(cccd)
        }
    }

    // ---- inbound ---------------------------------------------------------

    private fun handleNotification(data: ByteArray) {
        if (data.size < Proto.HDR_LEN) return
        val b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        val type = b.get().toInt() and 0xFF
        b.get()                                   // flags
        b.short                                   // seq
        val len = b.short.toInt() and 0xFFFF
        if (b.remaining() < len) return

        when (type) {
            Proto.MSG_CREDITS -> {
                val n = b.short.toInt() and 0xFFFF
                synchronized(txQueue) { credits += n }
                pump()
            }

            Proto.MSG_TILE_REQUEST -> {
                val count = b.get().toInt() and 0xFF
                val reqs = ArrayList<TileRequest>(count)
                for (i in 0 until count) {
                    if (b.remaining() < 10) break
                    val z = b.get().toInt() and 0xFF
                    val x = b.int
                    val y = b.int
                    val prio = b.get().toInt() and 0xFF
                    reqs.add(TileRequest(z, x, y, prio))
                }
                if (reqs.isNotEmpty()) listener.onTilesRequested(reqs)
            }

            Proto.MSG_VIEW -> {
                if (b.remaining() < 11) return
                val lat = b.int / 1e7
                val lon = b.int / 1e7
                val zoom = b.get().toInt() and 0xFF
                val bearing = (b.short.toInt() and 0xFFFF) / 100f
                listener.onViewChanged(lat, lon, zoom, bearing)
            }

            Proto.MSG_DEST -> {
                if (b.remaining() < 8) return
                listener.onDestinationPicked(b.int / 1e7, b.int / 1e7)
            }

            Proto.MSG_LOG -> {
                val s = ByteArray(len).also { b.get(it) }
                Log.i(TAG, "esp: ${String(s)}")
            }
        }
    }

    // ---- outbound --------------------------------------------------------

    /** Enqueues one packet. Never blocks; the pump drains as credits allow. */
    fun send(type: Int, payload: ByteArray?) {
        val pkt = Proto.frame(type, seq++, payload)
        synchronized(txQueue) { txQueue.addLast(pkt) }
        pump()
    }

    /** Queue depth, so callers can avoid piling up stale tile data. */
    fun pending(): Int = synchronized(txQueue) { txQueue.size }

    /**
     * Sends at most one packet, then returns. Android's GATT stack only
     * tolerates one outstanding write, so onCharacteristicWrite is what
     * resumes us rather than a loop here.
     */
    private fun pump() {
        val g = gatt ?: return
        val ch = rxChar ?: return

        // Some GATT stacks drop a write completion under load. One lost
        // callback would otherwise wedge the queue permanently, which looks
        // identical to the link having died.
        synchronized(txQueue) {
            if (writeInFlight && txQueue.isNotEmpty() &&
                SystemClock.elapsedRealtime() - lastWriteAt > WRITE_TIMEOUT_MS) {
                Log.w(TAG, "write completion lost after ${WRITE_TIMEOUT_MS} ms, " +
                           "${txQueue.size} queued - resuming")
                writeInFlight = false
            }
        }

        val pkt: ByteArray = synchronized(txQueue) {
            if (writeInFlight || credits <= 0 || txQueue.isEmpty()) return
            writeInFlight = true
            credits--
            lastWriteAt = SystemClock.elapsedRealtime()
            txQueue.removeFirst()
        }

        if (pkt.size > MAX_ATT_WRITE) {
            // Dropping one packet corrupts a tile; crashing loses the link and
            // every tile after it. The CRC check on the firmware will reject
            // the tile and it will be re-requested.
            Log.e(TAG, "packet of ${pkt.size} B exceeds the ${MAX_ATT_WRITE} B " +
                       "attribute limit - dropping (this is a framing bug)")
            synchronized(txQueue) { writeInFlight = false }
            return
        }

        // The GATT stack throws on bad arguments and on some transient states.
        // This runs in a background service that must survive a whole drive,
        // so a single failed write must never take the process with it.
        val ok = try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeCharacteristic(
                    ch, pkt, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                ) == BluetoothGatt.GATT_SUCCESS
            } else {
                @Suppress("DEPRECATION")
                run {
                    ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                    ch.value = pkt
                    g.writeCharacteristic(ch)
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "writeCharacteristic threw (${pkt.size} B): ${e.message}")
            false
        }

        if (ok) {
            bytesSent += pkt.size
        } else {
            // Stack was busy. Put it back; the next completion or credit
            // grant will retry it.
            synchronized(txQueue) {
                txQueue.addFirst(pkt)
                credits++
                writeInFlight = false
            }
        }
    }

    private fun sendHello() {
        val b = Proto.le(8)
        b.put(Proto.VERSION.toByte())
        b.put(0)                                     // caps
        b.putShort(dev.espmaps.map.TileEncoder.OUT_EXTENT.toShort())
        b.putInt((System.currentTimeMillis() / 1000).toInt())
        send(Proto.MSG_HELLO, b.array())
    }
}
