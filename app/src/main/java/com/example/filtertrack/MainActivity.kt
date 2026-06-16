package com.filtertrack

import android.Manifest
import android.annotation.SuppressLint
import android.app.AlertDialog
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.View
import android.webkit.ConsoleMessage
import android.webkit.WebChromeClient
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat

class MainActivity : AppCompatActivity(),
    BLEManager.BLEListener,
    BLEManager.DataListener {

    companion object {
        const val EXTRA_STARTUP_ERROR = "startup_error"
    }

    private lateinit var webView: WebView
    private lateinit var bleManager: BLEManager
    private lateinit var biButton: View

    @Volatile private var pageReady = false
    private val pendingJs = ArrayDeque<String>()
    private var pendingScanAfterBluetoothEnable = false
    private var bluetoothReceiverRegistered = false

    private val bluetoothStateReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != BluetoothAdapter.ACTION_STATE_CHANGED) return
            when (intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)) {
                BluetoothAdapter.STATE_TURNING_OFF,
                BluetoothAdapter.STATE_OFF -> {
                    pendingScanAfterBluetoothEnable = false
                    setBiButtonVisible(true)
                    bleManager.handleBluetoothUnavailable("Bluetooth desligado")
                }
            }
        }
    }

    private val requestPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        if (permissions.values.all { it }) {
            bleManager.startScan()
        } else {
            Toast.makeText(this, "Permissoes BLE necessarias", Toast.LENGTH_SHORT).show()
            push("onError", "Permissoes BLE negadas")
        }
    }

    private val enableBluetoothLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) {
        val enabled = bleManager.isBluetoothEnabled()
        if (enabled && pendingScanAfterBluetoothEnable) {
            pendingScanAfterBluetoothEnable = false
            checkPermissionsAndScan()
        } else {
            pendingScanAfterBluetoothEnable = false
            push("onError", "Ative o Bluetooth para procurar dispositivos")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        window.decorView.systemUiVisibility =
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN

        bleManager = BLEManager.getInstance(this)
        bleManager.bleListener = this
        bleManager.dataListener = this

        webView = findViewById(R.id.webView)
        biButton = findViewById(R.id.biButton)
        biButton.setOnClickListener { openBiDashboard() }
        setupWebView()
        registerBluetoothStateReceiver()
        showStartupErrorIfAny(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (intent.getBooleanExtra("reset_to_initial_screen", false)) {
            bleManager.stopScan()
            bleManager.disconnect()
            pageReady = false
            pendingJs.clear()
            setBiButtonVisible(true)
            webView.loadUrl("file:///android_asset/index.html")
            intent.removeExtra("reset_to_initial_screen")
        }
        showStartupErrorIfAny(intent)
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun setupWebView() {
        webView.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            loadWithOverviewMode = true
            useWideViewPort = true
            cacheMode = WebSettings.LOAD_DEFAULT
            mixedContentMode = WebSettings.MIXED_CONTENT_COMPATIBILITY_MODE
        }
        WebView.setWebContentsDebuggingEnabled(BuildConfig.DEBUG)

        webView.webChromeClient = object : WebChromeClient() {
            override fun onConsoleMessage(msg: ConsoleMessage): Boolean {
                android.util.Log.d("WebView", "[${msg.messageLevel()}] ${msg.message()} @${msg.lineNumber()}")
                return true
            }
        }

        webView.webViewClient = object : WebViewClient() {
            override fun onPageFinished(view: WebView?, url: String?) {
                pageReady = true
                flushPending()
            }
        }

        val iface = WebAppInterface(bleManager) { action ->
            runOnUiThread { handleAction(action) }
        }
        webView.addJavascriptInterface(iface, "Android")

        webView.loadUrl("file:///android_asset/index.html")
    }

    private fun registerBluetoothStateReceiver() {
        if (bluetoothReceiverRegistered) return
        val filter = IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(bluetoothStateReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            registerReceiver(bluetoothStateReceiver, filter)
        }
        bluetoothReceiverRegistered = true
    }

    private fun handleAction(action: WebAppInterface.Action) {
        when (action) {
            WebAppInterface.Action.StartScan -> checkPermissionsAndScan()
            WebAppInterface.Action.StopScan -> bleManager.stopScan()
            is WebAppInterface.Action.Connect -> bleManager.connect(action.address)
            WebAppInterface.Action.Disconnect -> bleManager.disconnect()
            WebAppInterface.Action.OpenBiDashboard -> openBiDashboard()
            is WebAppInterface.Action.SendCommand -> {
                val ok = bleManager.sendCommand(action.cmd)
                if (!ok) push("onError", "Falha ao enviar comando")
            }
        }
    }

    private fun openBiDashboard() {
        startActivity(Intent(this, BiDashboardActivity::class.java))
    }

    private fun setBiButtonVisible(visible: Boolean) {
        if (::biButton.isInitialized) {
            biButton.visibility = if (visible) View.VISIBLE else View.GONE
        }
    }

    private fun checkPermissionsAndScan() {
        if (!bleManager.isBluetoothEnabled()) {
            promptEnableBluetoothForRetry()
            return
        }

        val needed = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            needed.add(Manifest.permission.BLUETOOTH_SCAN)
            needed.add(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            needed.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        val missing = needed.filter {
            ActivityCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) bleManager.startScan()
        else requestPermissionLauncher.launch(missing.toTypedArray())
    }

    private fun promptEnableBluetoothForRetry() {
        if (isFinishing || isDestroyed) return
        AlertDialog.Builder(this)
            .setTitle("Bluetooth desligado")
            .setMessage("Ative o Bluetooth para procurar dispositivos e tentar novamente.")
            .setNegativeButton("Cancelar") { dialog, _ ->
                pendingScanAfterBluetoothEnable = false
                dialog.dismiss()
                push("onError", "Ative o Bluetooth para procurar dispositivos")
            }
            .setPositiveButton("Ativar") { _, _ ->
                pendingScanAfterBluetoothEnable = true
                enableBluetoothLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
            }
            .show()
    }

    private fun showStartupErrorIfAny(intent: Intent?) {
        val message = intent?.getStringExtra(EXTRA_STARTUP_ERROR)?.trim().orEmpty()
        if (message.isEmpty()) return
        Toast.makeText(this, message, Toast.LENGTH_LONG).show()
        intent?.removeExtra(EXTRA_STARTUP_ERROR)
    }

    private fun push(method: String, vararg args: Any?) {
        val jsArgs = args.joinToString(",") { toJsLiteral(it) }
        val js = "window.FilterTrackBridge && window.FilterTrackBridge.$method($jsArgs);"
        if (pageReady) {
            webView.post { webView.evaluateJavascript(js, null) }
        } else {
            pendingJs.addLast(js)
        }
    }

    private fun flushPending() {
        while (pendingJs.isNotEmpty()) {
            val js = pendingJs.removeFirst()
            webView.post { webView.evaluateJavascript(js, null) }
        }
    }

    private fun toJsLiteral(v: Any?): String = when (v) {
        null -> "null"
        is Number, is Boolean -> v.toString()
        else -> "\"" + v.toString()
            .replace("\\", "\\\\")
            .replace("\"", "\\\"")
            .replace("\n", "\\n")
            .replace("\r", "\\r") + "\""
    }

    @SuppressLint("MissingPermission")
    override fun onDeviceFound(device: BluetoothDevice, rssi: Int) {
        val name = try { device.name } catch (_: SecurityException) { null } ?: "Desconhecido"
        push("onDeviceFound", name, device.address, rssi)
    }

    override fun onConnectionStateChanged(status: String, deviceName: String?, deviceAddress: String?) {
        setBiButtonVisible(status != "Connected")
        push("onConnectionStateChanged", status, deviceName, deviceAddress)
    }

    override fun onScanStateChanged(scanning: Boolean) {
        push("onScanStateChanged", scanning)
    }

    override fun onRssiUpdate(rssi: Int) {
        push("onRssiUpdate", rssi)
    }

    override fun onError(message: String) {
        push("onError", message)
    }

    override fun onDataReceived(data: String) {
        push("onDataReceived", data)
    }

    override fun onDestroy() {
        super.onDestroy()
        if (bluetoothReceiverRegistered) {
            try { unregisterReceiver(bluetoothStateReceiver) } catch (_: Throwable) {}
            bluetoothReceiverRegistered = false
        }
        if (bleManager.bleListener === this) bleManager.bleListener = null
        if (bleManager.dataListener === this) bleManager.dataListener = null
        try {
            webView.destroy()
        } catch (_: Throwable) {
        }
    }

    override fun onBackPressed() {
        if (webView.canGoBack()) webView.goBack() else super.onBackPressed()
    }
}
