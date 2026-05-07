package com.example.filtertrack

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.UUID

class BLEManager private constructor(private val context: Context) {

    companion object {
        private const val TAG = "BLEManager"

        @Volatile private var instance: BLEManager? = null

        fun getInstance(context: Context): BLEManager =
            instance ?: synchronized(this) {
                instance ?: BLEManager(context.applicationContext).also { instance = it }
            }

        private val CLIENT_CHARACTERISTIC_CONFIG_UUID: UUID =
            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        // ESP32 firmware exposes service 0x00FF with characteristic 0xFF01.
        private val FILTERTRACK_SERVICE_UUID: UUID =
            UUID.fromString("000000ff-0000-1000-8000-00805f9b34fb")
        private val FILTERTRACK_CHAR_UUID: UUID =
            UUID.fromString("0000ff01-0000-1000-8000-00805f9b34fb")
    }

    private val bluetoothAdapter: BluetoothAdapter? by lazy {
        val bm = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        bm.adapter
    }

    private var bleScanner: BluetoothLeScanner? = null
    private var bluetoothGatt: BluetoothGatt? = null
    private var writableChar: BluetoothGattCharacteristic? = null
    private val handler = Handler(Looper.getMainLooper())

    // Serialize GATT writes: Android only tolerates one pending write at a time.
    // All access to writeQueue / writeInFlight / currentRetries happens on `handler` (main) thread.
    private val writeQueue: java.util.ArrayDeque<ByteArray> = java.util.ArrayDeque()
    private var writeInFlight = false
    private var waitingForWriteCallback = false
    private var currentBytes: ByteArray? = null
    private var currentRetries = 0
    private val MAX_WRITE_RETRIES = 5
    private val WRITE_TIMEOUT_MS = 3000L
    private val WRITE_NO_RESPONSE_GAP_MS = 40L

    private val writeTimeoutRunnable = Runnable {
        // Callback never fired — assume the write was lost and try to recover.
        val bytes = currentBytes
        writeInFlight = false
        waitingForWriteCallback = false
        currentBytes = null
        if (bytes != null && currentRetries < MAX_WRITE_RETRIES) {
            currentRetries += 1
            Log.w(TAG, "write timed out; retry $currentRetries/$MAX_WRITE_RETRIES")
            writeQueue.addFirst(bytes)
            handler.postDelayed({ drainWriteQueue() }, 80L)
        } else if (bytes != null) {
            Log.w(TAG, "giving up on timed-out write after $currentRetries retries")
            currentRetries = 0
            bleListener?.onError("Falha ao enviar comando")
            handler.post { drainWriteQueue() }
        } else {
            currentRetries = 0
            drainWriteQueue()
        }
    }

    var bleListener: BLEListener? = null
    var dataListener: DataListener? = null

    @Volatile private var isScanning = false

    @SuppressLint("MissingPermission")
    private fun hasResolvableName(result: ScanResult): Boolean {
        val advName = result.scanRecord?.deviceName?.trim().orEmpty()
        if (advName.isNotEmpty()) return true
        val devName = try { result.device?.name?.trim().orEmpty() } catch (_: SecurityException) { "" }
        return devName.isNotEmpty()
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (!hasResolvableName(result)) return
            handler.post {
                bleListener?.onDeviceFound(result.device, result.rssi)
            }
        }

        override fun onBatchScanResults(results: MutableList<ScanResult>) {
            results.forEach { r ->
                if (!hasResolvableName(r)) return@forEach
                handler.post { bleListener?.onDeviceFound(r.device, r.rssi) }
            }
        }

        override fun onScanFailed(errorCode: Int) {
            Log.e(TAG, "Scan failed: $errorCode")
            isScanning = false
            handler.post {
                bleListener?.onScanStateChanged(false)
                bleListener?.onError("Falha no scan ($errorCode)")
            }
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt?, status: Int, newState: Int) {
            val stateStr = when (newState) {
                BluetoothProfile.STATE_CONNECTED -> "Connected"
                BluetoothProfile.STATE_DISCONNECTED -> "Disconnected"
                BluetoothProfile.STATE_CONNECTING -> "Connecting"
                BluetoothProfile.STATE_DISCONNECTING -> "Disconnecting"
                else -> "Unknown"
            }
            Log.d(TAG, "onConnectionStateChange: $stateStr status=$status")

            if (newState == BluetoothProfile.STATE_CONNECTED) {
                gatt?.discoverServices()
                gatt?.readRemoteRssi()
                val dev = gatt?.device
                handler.post {
                    bleListener?.onConnectionStateChanged(stateStr, dev?.name, dev?.address)
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                closeGatt()
                handler.post {
                    bleListener?.onConnectionStateChanged(stateStr, null, null)
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(gatt: BluetoothGatt?, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                enableNotifications(gatt)
            } else {
                Log.w(TAG, "onServicesDiscovered failed: $status")
            }
        }

        @Deprecated("Kept for API < 33 compat")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt?,
            characteristic: BluetoothGattCharacteristic?
        ) {
            @Suppress("DEPRECATION")
            val data = characteristic?.value ?: return
            val s = String(data)
            handler.post { dataListener?.onDataReceived(s) }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            val s = String(value)
            handler.post { dataListener?.onDataReceived(s) }
        }

        override fun onReadRemoteRssi(gatt: BluetoothGatt?, rssi: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                handler.post { bleListener?.onRssiUpdate(rssi) }
            }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt?,
            characteristic: BluetoothGattCharacteristic?,
            status: Int
        ) {
            Log.d(TAG, "onCharacteristicWrite status=$status uuid=${characteristic?.uuid}")
            handler.post {
                if (!waitingForWriteCallback) {
                    Log.d(TAG, "onCharacteristicWrite with no pending callback; ignoring")
                    return@post
                }
                handler.removeCallbacks(writeTimeoutRunnable)
                waitingForWriteCallback = false
                if (status != BluetoothGatt.GATT_SUCCESS && currentBytes != null && currentRetries < MAX_WRITE_RETRIES) {
                    // Requeue the failed byte-string at head and retry with a small backoff.
                    val b = currentBytes!!
                    currentBytes = null
                    writeInFlight = false
                    currentRetries += 1
                    writeQueue.addFirst(b)
                    handler.postDelayed({ drainWriteQueue() }, 60L)
                } else {
                    currentBytes = null
                    currentRetries = 0
                    writeInFlight = false
                    drainWriteQueue()
                }
            }
        }

        override fun onDescriptorWrite(
            gatt: BluetoothGatt?,
            descriptor: BluetoothGattDescriptor?,
            status: Int
        ) {
            Log.d(TAG, "onDescriptorWrite status=$status")
            // CCCD write counted against the same queue slot as characteristic writes.
            handler.post {
                handler.removeCallbacks(writeTimeoutRunnable)
                writeInFlight = false
                drainWriteQueue()
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun closeGatt() {
        bluetoothGatt?.let {
            try { it.disconnect() } catch (_: Throwable) {}
            try { it.close() } catch (_: Throwable) {}
        }
        bluetoothGatt = null
        writableChar = null
        handler.post {
            handler.removeCallbacks(writeTimeoutRunnable)
            writeQueue.clear()
            writeInFlight = false
            waitingForWriteCallback = false
            currentBytes = null
            currentRetries = 0
        }
    }

    @SuppressLint("MissingPermission")
    private fun enableNotifications(gatt: BluetoothGatt?) {
        gatt ?: return
        val target = gatt.getService(FILTERTRACK_SERVICE_UUID)
            ?.getCharacteristic(FILTERTRACK_CHAR_UUID)

        if (target != null) {
            val props = target.properties
            if (props and BluetoothGattCharacteristic.PROPERTY_WRITE != 0 ||
                props and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0) {
                writableChar = target
            }
            if (props and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0) {
                gatt.setCharacteristicNotification(target, true)
                target.getDescriptor(CLIENT_CHARACTERISTIC_CONFIG_UUID)?.let { desc ->
                    writeInFlight = true
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        gatt.writeDescriptor(desc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                    } else {
                        @Suppress("DEPRECATION")
                        run {
                            desc.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                            gatt.writeDescriptor(desc)
                        }
                    }
                }
            }
            return
        }

        // Fallback: first char with NOTIFY wins; first char with WRITE wins.
        Log.w(TAG, "FilterTrack custom service not found, falling back to scan")
        var foundWritable = false
        var foundNotify = false
        gatt.services.forEach { service ->
            service.characteristics.forEach { ch ->
                val props = ch.properties
                if (!foundWritable && (
                    props and BluetoothGattCharacteristic.PROPERTY_WRITE != 0 ||
                    props and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0)) {
                    writableChar = ch
                    foundWritable = true
                }
                if (!foundNotify && props and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0) {
                    gatt.setCharacteristicNotification(ch, true)
                    ch.getDescriptor(CLIENT_CHARACTERISTIC_CONFIG_UUID)?.let { desc ->
                        writeInFlight = true
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                            gatt.writeDescriptor(desc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                        } else {
                            @Suppress("DEPRECATION")
                            run {
                                desc.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                                gatt.writeDescriptor(desc)
                            }
                        }
                    }
                    foundNotify = true
                }
            }
        }
    }

    fun isBluetoothEnabled(): Boolean = bluetoothAdapter?.isEnabled == true

    @SuppressLint("MissingPermission")
    fun handleBluetoothUnavailable(reason: String = "Bluetooth desligado") {
        stopScan()
        closeGatt()
        handler.post {
            bleListener?.onScanStateChanged(false)
            bleListener?.onConnectionStateChanged("Disconnected", null, null)
            bleListener?.onError(reason)
        }
    }

    @SuppressLint("MissingPermission")
    fun startScan() {
        if (!isBluetoothEnabled()) {
            handleBluetoothUnavailable("Bluetooth desligado")
            return
        }
        if (isScanning) stopScan()

        bleScanner = bluetoothAdapter?.bluetoothLeScanner
        if (bleScanner == null) {
            handler.post { bleListener?.onError("Scanner BLE indisponível") }
            return
        }

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        isScanning = true
        handler.post { bleListener?.onScanStateChanged(true) }
        bleScanner?.startScan(null, settings, scanCallback)
    }

    @SuppressLint("MissingPermission")
    fun stopScan() {
        if (!isScanning) return
        try { bleScanner?.stopScan(scanCallback) } catch (_: Throwable) {}
        isScanning = false
        handler.post { bleListener?.onScanStateChanged(false) }
    }

    @SuppressLint("MissingPermission")
    fun connect(device: BluetoothDevice) {
        if (!isBluetoothEnabled()) {
            handleBluetoothUnavailable("Bluetooth desligado")
            return
        }
        stopScan()
        closeGatt()
        bluetoothGatt = device.connectGatt(context, false, gattCallback)
    }

    @SuppressLint("MissingPermission")
    fun connect(address: String) {
        val adapter = bluetoothAdapter ?: return
        val device = try { adapter.getRemoteDevice(address) } catch (_: IllegalArgumentException) { null }
        if (device == null) {
            handler.post { bleListener?.onError("Endereço inválido: $address") }
            return
        }
        connect(device)
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        if (!isBluetoothEnabled()) {
            handleBluetoothUnavailable("Bluetooth desligado")
            return
        }
        val gatt = bluetoothGatt
        if (gatt == null) {
            handler.post { bleListener?.onConnectionStateChanged("Disconnected", null, null) }
            return
        }
        try {
            gatt.disconnect()
        } catch (_: Throwable) {
            closeGatt()
            handler.post { bleListener?.onConnectionStateChanged("Disconnected", null, null) }
            return
        }
        handler.postDelayed({
            if (bluetoothGatt === gatt) {
                closeGatt()
                bleListener?.onConnectionStateChanged("Disconnected", null, null)
            }
        }, 1500L)
    }

    fun sendCommand(command: String): Boolean {
        if (bluetoothGatt == null || writableChar == null) {
            Log.w(TAG, "sendCommand dropped, no gatt/char")
            return false
        }
        val bytes = command.toByteArray()
        handler.post {
            writeQueue.addLast(bytes)
            drainWriteQueue()
        }
        return true
    }

    @SuppressLint("MissingPermission")
    private fun drainWriteQueue() {
        // Runs on handler (main) thread.
        if (writeInFlight || writeQueue.isEmpty()) return
        val gatt = bluetoothGatt ?: return
        val ch = writableChar ?: return

        val bytes = writeQueue.removeFirst()
        writeInFlight = true
        currentBytes = bytes

        val hasWrite = ch.properties and BluetoothGattCharacteristic.PROPERTY_WRITE != 0
        val hasWriteNoResponse =
            ch.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0
        // Prefer WRITE_TYPE_DEFAULT when available so Android emits onCharacteristicWrite reliably.
        val writeType = when {
            hasWrite -> BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            hasWriteNoResponse -> BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            else -> BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        }
        val expectsCallback = writeType == BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT

        val ok: Boolean = try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                val rc = gatt.writeCharacteristic(ch, bytes, writeType)
                Log.d(TAG, "writeCharacteristic(API33+) rc=$rc bytes=${String(bytes)}")
                rc == BluetoothGatt.GATT_SUCCESS
            } else {
                @Suppress("DEPRECATION")
                run {
                    ch.writeType = writeType
                    ch.value = bytes
                    val r = gatt.writeCharacteristic(ch)
                    Log.d(TAG, "writeCharacteristic(legacy) returned=$r bytes=${String(bytes)}")
                    r
                }
            }
        } catch (t: Throwable) {
            Log.e(TAG, "writeCharacteristic threw", t)
            false
        }

        if (ok) {
            if (expectsCallback) {
                // Wait for onCharacteristicWrite or timeout.
                waitingForWriteCallback = true
                handler.postDelayed(writeTimeoutRunnable, WRITE_TIMEOUT_MS)
            } else {
                // WRITE_TYPE_NO_RESPONSE may not trigger onCharacteristicWrite on many stacks.
                handler.postDelayed({
                    writeInFlight = false
                    currentBytes = null
                    currentRetries = 0
                    drainWriteQueue()
                }, WRITE_NO_RESPONSE_GAP_MS)
            }
        } else {
            // Transient failure (often GATT busy processing notifications).
            // Re-queue at head and retry silently with small backoff.
            writeInFlight = false
            waitingForWriteCallback = false
            if (currentRetries < MAX_WRITE_RETRIES) {
                currentRetries += 1
                writeQueue.addFirst(bytes)
                currentBytes = null
                handler.postDelayed({ drainWriteQueue() }, 80L)
            } else {
                Log.w(TAG, "giving up on write after $currentRetries retries")
                currentBytes = null
                currentRetries = 0
                bleListener?.onError("Falha ao enviar comando")
                // Continue with any remaining commands.
                handler.post { drainWriteQueue() }
            }
        }
    }

    interface BLEListener {
        fun onDeviceFound(device: BluetoothDevice, rssi: Int)
        fun onConnectionStateChanged(status: String, deviceName: String?, deviceAddress: String?)
        fun onScanStateChanged(scanning: Boolean)
        fun onRssiUpdate(rssi: Int)
        fun onError(message: String)
    }

    interface DataListener {
        fun onDataReceived(data: String)
    }
}
