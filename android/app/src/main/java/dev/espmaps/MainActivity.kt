package dev.espmaps

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Context
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.util.Log
import android.widget.Toast
import androidx.lifecycle.lifecycleScope
import dev.espmaps.nav.MapsLinkParser
import dev.espmaps.nav.MapsLinkResolver
import dev.espmaps.nav.MapsNavListener
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import dev.espmaps.databinding.ActivityMainBinding

/**
 * Thin control surface. All the real work lives in NavService, which outlives
 * this Activity - the phone spends the journey in a pocket with the screen off.
 */
class MainActivity : AppCompatActivity() {

    private companion object { const val TAG = "MainActivity" }

    /**
     * A destination shared before the service was running. Sharing from Maps
     * naturally happens before you think to start esp-maps, and nothing
     * re-sends a destination at startup - so it is held here and applied the
     * moment the service comes up.
     */
    private var pendingDest: MapsLinkParser.Dest? = null

    private lateinit var ui: ActivityMainBinding
    private var running = false

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { grants ->
        if (grants.values.all { it }) {
            startNav()
        } else {
            toast("Permissions denied - location and Bluetooth are both required")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ui = ActivityMainBinding.inflate(layoutInflater)
        setContentView(ui.root)

        if (BuildConfig.MAPTILER_KEY.isBlank()) {
            ui.status.text = getString(R.string.no_maptiler_key)
        }

        ui.startStop.setOnClickListener {
            if (running) stopNav() else requestThenStart()
        }

        ui.navigate.setOnClickListener { sendDestination() }

        handleSharedDestination(intent)

        ui.simulate.setOnClickListener {
            if (!running) {
                toast("Start the service first")
            } else {
                startService(
                    Intent(this, NavService::class.java)
                        .setAction(NavService.ACTION_DEMO)
                )
                toast("Simulation toggled — watch the display")
            }
        }

        // Notification access cannot be requested with a runtime permission
        // dialog - it only exists as a Settings screen the user must visit.
        ui.grantNotifications.setOnClickListener {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleSharedDestination(intent)
    }

    /**
     * Accepts a place shared from Google Maps (or a geo: link) and turns it
     * into the destination we draw a route to.
     *
     * Maps will not expose its own route geometry to any app, so this is the
     * closest thing to automatic: share the place once, then start navigation
     * in Maps as usual. The display then has the map, Maps' turn guidance,
     * and a route line to the same place.
     */
    private var launchedForShare = false

    private fun handleSharedDestination(intent: Intent?) {
        if (intent?.action != Intent.ACTION_SEND &&
            intent?.action != Intent.ACTION_VIEW) return
        launchedForShare = true

        // Log everything: what a sharing app actually puts in the intent is
        // undocumented and varies. EXTRA_TEXT is the common case but not the
        // only one - some apps only populate ClipData.
        Log.i(TAG, "share intent: action=${intent.action} type=${intent.type}")
        intent.extras?.keySet()?.forEach { k ->
            Log.i(TAG, "  extra[$k] = ${intent.extras?.get(k).toString().take(300)}")
        }
        intent.dataString?.let { Log.i(TAG, "  data = ${it.take(300)}") }
        intent.clipData?.let { cd ->
            for (i in 0 until cd.itemCount) {
                Log.i(TAG, "  clip[$i] text=${cd.getItemAt(i).text?.toString()?.take(300)} " +
                           "uri=${cd.getItemAt(i).uri}")
            }
        }

        val text = sequenceOf(
            intent.getStringExtra(Intent.EXTRA_TEXT),
            intent.dataString,
            intent.getStringExtra(Intent.EXTRA_SUBJECT),
            intent.clipData?.takeIf { it.itemCount > 0 }
                ?.getItemAt(0)?.let { it.text?.toString() ?: it.uri?.toString() },
        ).firstOrNull { !it.isNullOrBlank() } ?: run {
            Log.w(TAG, "share intent carried no usable text")
            toast("That share had no link in it")
            return
        }

        val sharedTitle = intent.getStringExtra(Intent.EXTRA_SUBJECT)
            ?: intent.getStringExtra("android.intent.extra.TITLE")

        Log.i(TAG, "resolving share text: ${text.take(300)} (title=$sharedTitle)")

        lifecycleScope.launch {
            val dest = withContext(Dispatchers.IO) {
                // Short links carry no coordinates until the redirect is
                // followed, which needs the network - hence the IO hop.
                MapsLinkParser.parse(text)
                    ?: MapsLinkParser.findShortLink(text)
                        ?.let { MapsLinkResolver.resolve(this@MainActivity, it, sharedTitle) }
            }

            if (dest == null) {
                Log.w(TAG, "could not resolve: ${text.take(200)}")
                toast("Could not locate that link - no coordinates, and the address did not geocode")
                return@launch
            }

            Log.i(TAG, "destination ${dest.lat}, ${dest.lon} (${dest.label})")
            ui.destination.setText("${dest.lat}, ${dest.lon}")
            if (!NavService.isRunning) {
                pendingDest = dest
                toast("Destination saved - tap Start and it will route")
            } else {
                startService(
                    Intent(this@MainActivity, NavService::class.java)
                        .setAction(NavService.ACTION_SET_DEST)
                        .putExtra(NavService.EXTRA_LAT, dest.lat)
                        .putExtra(NavService.EXTRA_LON, dest.lon)
                )
                toast("Route to ${dest.label ?: "destination"}")

                // Launched purely to receive a share, and the work is done -
                // get out of the way and return the user to Maps.
                if (launchedForShare && !isFinishing) finish()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        refreshMapsStatus()
        syncRunningState()
    }

    /** The service outlives this Activity, so re-read its state on return. */
    private fun syncRunningState() {
        running = NavService.isRunning
        ui.startStop.setText(if (running) R.string.stop else R.string.start)
        ui.status.setText(if (running) R.string.status_running else R.string.status_idle)
    }

    private fun refreshMapsStatus() {
        val granted = MapsNavListener.isEnabled(this)
        ui.mapsStatus.setText(
            if (granted) R.string.maps_status_on else R.string.maps_status_off
        )
        ui.grantNotifications.isEnabled = !granted
    }

    private fun requiredPermissions(): Array<String> {
        val perms = mutableListOf(Manifest.permission.ACCESS_FINE_LOCATION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            perms += Manifest.permission.BLUETOOTH_SCAN
            perms += Manifest.permission.BLUETOOTH_CONNECT
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            perms += Manifest.permission.POST_NOTIFICATIONS
        }
        return perms.toTypedArray()
    }

    private fun requestThenStart() {
        val missing = requiredPermissions().filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) startNav() else permissionLauncher.launch(missing.toTypedArray())
    }

    private fun startNav() {
        // Check this after permissions, because ACTION_REQUEST_ENABLE itself
        // needs BLUETOOTH_CONNECT on API 31+.
        val bt = (getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter
        if (bt == null) {
            toast("This device has no Bluetooth")
            return
        }
        if (!bt.isEnabled) {
            // The service would otherwise start, fail to get a scanner, and
            // sit there silently doing nothing.
            toast("Turn Bluetooth on to connect to the display")
            runCatching {
                startActivity(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
            }
            return
        }

        ContextCompat.startForegroundService(this, Intent(this, NavService::class.java))

        // Intents queue in order behind onCreate, so this lands once the
        // service exists.
        pendingDest?.let { d ->
            Log.i(TAG, "applying destination shared before start: ${d.lat}, ${d.lon}")
            startService(
                Intent(this, NavService::class.java)
                    .setAction(NavService.ACTION_SET_DEST)
                    .putExtra(NavService.EXTRA_LAT, d.lat)
                    .putExtra(NavService.EXTRA_LON, d.lon)
            )
            pendingDest = null
            toast("Routing to ${d.label ?: "destination"}")
        }

        running = true
        ui.startStop.setText(R.string.stop)
        ui.status.setText(R.string.status_running)
    }

    private fun stopNav() {
        startService(Intent(this, NavService::class.java).setAction(NavService.ACTION_STOP))
        running = false
        ui.startStop.setText(R.string.start)
        ui.status.setText(R.string.status_idle)
    }

    /**
     * Accepts "lat,lon". Once touch input lands on the device this becomes a
     * fallback rather than the only way in.
     */
    private fun sendDestination() {
        val text = ui.destination.text.toString().trim()
        val parts = text.split(",")
        if (parts.size != 2) {
            toast("Enter a destination as: lat,lon")
            return
        }
        val lat = parts[0].trim().toDoubleOrNull()
        val lon = parts[1].trim().toDoubleOrNull()
        if (lat == null || lon == null || lat !in -90.0..90.0 || lon !in -180.0..180.0) {
            toast("Could not parse that as a coordinate")
            return
        }
        if (!running) {
            toast("Start the service first")
            return
        }

        startService(
            Intent(this, NavService::class.java)
                .setAction(NavService.ACTION_SET_DEST)
                .putExtra(NavService.EXTRA_LAT, lat)
                .putExtra(NavService.EXTRA_LON, lon)
        )
        toast("Routing to $lat, $lon")
    }

    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_SHORT).show()
}
