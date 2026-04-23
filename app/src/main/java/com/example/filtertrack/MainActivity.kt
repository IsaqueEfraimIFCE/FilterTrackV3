package com.example.filtertrack

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
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

    private lateinit var webView: WebView
    private lateinit var bleManager: BLEManager

    @Volatile private var pageReady = false
    private val pendingJs = ArrayDeque<String>()

    private val requestPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        if (permissions.values.all { it }) {
            bleManager.startScan()
        } else {
            Toast.makeText(this, "Permissões BLE necessárias", Toast.LENGTH_SHORT).show()
            push("onError", "Permissões BLE negadas")
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
        setupWebView()
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
        WebView.setWebContentsDebuggingEnabled(true)

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
        webView.addJavascriptInter-face(iface, "Android")

        webView.loadUrl("file:///android_asset/index.html")
    }

    private fun handleAction(action: WebAppInterface.Action) {
        when (action) {
            WebAppInterface.Action.StartScan -> checkPermissionsAndScan()
            WebAppInterface.Action.StopScan -> bleManager.stopScan()
            is WebAppInterface.Action.Connect -> bleManager.connect(action.address)
            WebAppInterface.Action.Disconnect -> bleManager.disconnect()
            is WebAppInterface.Action.SendCommand -> {
                val ok = bleManager.sendCommand(action.cmd)
                if (!ok) push("onError", "Falha ao enviar comando")
            }
        }
    }

    private fun checkPermissionsAndScan() {
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

    // ── Push events from Kotlin → JS ──
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

    // ── BLEManager.BLEListener ──
    @SuppressLint("MissingPermission")
    override fun onDeviceFound(device: BluetoothDevice, rssi: Int) {
        val name = try { device.name } catch (_: SecurityException) { null } ?: "Desconhecido"
        push("onDeviceFound", name, device.address, rssi)
    }

    override fun onConnectionStateChanged(status: String, deviceName: String?, deviceAddress: String?) {
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

    // ── BLEManager.DataListener ──
    override fun onDataReceived(data: String) {
        push("onDataReceived", data)
    }

    override fun onDestroy() {
        super.onDestroy()
        if (bleManager.bleListener === this) bleManager.bleListener = null
        if (bleManager.dataListener === this) bleManager.dataListener = null
        try { webView.destroy() } catch (_: Throwable) {}
    }

    override fun onBackPressed() {
        if (webView.canGoBack()) webView.goBack() else super.onBackPressed()
    }
}
