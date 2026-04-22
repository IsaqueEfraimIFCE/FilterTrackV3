package com.example.filtertrack

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.*

/**
 * BLEManager handles the full lifecycle of BLE communication.
 * It is a singleton to maintain the connection state across Activities.
 */
class BLEManager private constructor(private val context: Context) {

    companion object {
        private const val TAG = "BLEManager"
        
        @SuppressLint("StaticFieldLeak")
        private var instance: BLEManager? = null

        fun getInstance(context: Context): BLEManager {
            if (instance == null) {
                instance = BLEManager(context.applicationContext)
            }
            return instance!!
        }

        private val CLIENT_CHARACTERISTIC_CONFIG_UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }

    private val bluetoothAdapter: BluetoothAdapter? by lazy {
        val bluetoothManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        bluetoothManager.adapter
    }

    private var bleScanner: BluetoothLeScanner? = null
    private var bluetoothGatt: BluetoothGatt? = null
    private val handler = Handler(Looper.getMainLooper())
    
    var bleListener: BLEListener? = null
    var dataListener: DataListener? = null
    
    private var isScanning = false

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            handler.post {
                bleListener?.onDeviceFound(result.device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            Log.e(TAG, "Scan failed with error: $errorCode")
            isScanning = false
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(gatt: BluetoothGatt?, status: Int, newState: Int) {
            val statusString = when (newState) {
                BluetoothProfile.STATE_CONNECTED -> "Connected"
                BluetoothProfile.STATE_DISCONNECTED -> "Disconnected"
                BluetoothProfile.STATE_CONNECTING -> "Connecting"
                BluetoothProfile.STATE_DISCONNECTING -> "Disconnecting"
                else -> "Unknown"
            }
            
            Log.d(TAG, "onConnectionStateChange: $statusString (status: $status, newState: $newState)")

            handler.post {
                bleListener?.onConnectionStateChanged(statusString)
            }
            
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.i(TAG, "Connected to GATT server. Starting service discovery...")
                gatt?.discoverServices()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.i(TAG, "Disconnected from GATT server. Cleaning up.")
                closeGatt()
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt?, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                Log.i(TAG, "Services discovered successfully.")
                enableNotifications(gatt)
            } else {
                Log.w(TAG, "onServicesDiscovered failed: $status")
            }
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(gatt: BluetoothGatt?, characteristic: BluetoothGattCharacteristic?) {
            @Suppress("DEPRECATION")
            val data = characteristic?.value
            if (data != null) {
                val stringData = String(data)
                Log.d(TAG, "Data received: $stringData")
                handler.post {
                    dataListener?.onDataReceived(stringData)
                }
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun closeGatt() {
        bluetoothGatt?.let { gatt ->
            Log.d(TAG, "Closing GATT instance")
            gatt.disconnect()
            gatt.close()
            bluetoothGatt = null
        }
    }

    @SuppressLint("MissingPermission")
    private fun enableNotifications(gatt: BluetoothGatt?) {
        gatt?.services?.forEach { service ->
            service.characteristics.forEach { characteristic ->
                if (characteristic.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0) {
                    val success = gatt.setCharacteristicNotification(characteristic, true)
                    if (success) {
                        val descriptor = characteristic.getDescriptor(CLIENT_CHARACTERISTIC_CONFIG_UUID)
                        if (descriptor != null) {
                            descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                            gatt.writeDescriptor(descriptor)
                            Log.i(TAG, "Notifications enabled for characteristic: ${characteristic.uuid}")
                        }
                    }
                }
            }
        }
    }

    fun isBluetoothEnabled(): Boolean {
        return bluetoothAdapter?.isEnabled == true
    }

    @SuppressLint("MissingPermission")
    fun startScan() {
        if (!isBluetoothEnabled()) {
            Log.e(TAG, "Bluetooth is disabled")
            return
        }
        
        if (isScanning) {
            Log.d(TAG, "Already scanning, stopping first")
            stopScan()
        }

        bleScanner = bluetoothAdapter?.bluetoothLeScanner
        if (bleScanner == null) {
            Log.e(TAG, "BluetoothLeScanner is null")
            return
        }

        Log.d(TAG, "Starting scan")
        isScanning = true
        bleScanner?.startScan(scanCallback)
    }

    @SuppressLint("MissingPermission")
    fun stopScan() {
        if (!isScanning) return
        
        Log.d(TAG, "Stopping scan")
        bleScanner?.stopScan(scanCallback)
        isScanning = false
    }

    @SuppressLint("MissingPermission")
    fun connect(device: BluetoothDevice) {
        Log.d(TAG, "Connecting to device: ${device.address}")
        stopScan()
        
        // Ensure previous connection is closed
        closeGatt()
        
        bluetoothGatt = device.connectGatt(context, false, gattCallback)
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        Log.d(TAG, "Manual disconnect initiated")
        bluetoothGatt?.disconnect()
    }

    interface BLEListener {
        fun onDeviceFound(device: BluetoothDevice)
        fun onConnectionStateChanged(status: String)
    }

    interface DataListener {
        fun onDataReceived(data: String)
    }
}
