package com.example.filtertrack

import android.os.Bundle
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

class DeviceControlActivity : AppCompatActivity(), BLEManager.BLEListener, BLEManager.DataListener {

    private lateinit var tvConnectionStatus: TextView
    private lateinit var tvReceivedData: TextView
    private lateinit var bleManager: BLEManager

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_device_control)

        tvConnectionStatus = findViewById(R.id.tvConnectionStatus)
        tvReceivedData = findViewById(R.id.tvReceivedData)

        bleManager = BLEManager.getInstance(this)
        bleManager.bleListener = this
        bleManager.dataListener = this

        tvConnectionStatus.text = "Connected"
    }

    override fun onDeviceFound(device: android.bluetooth.BluetoothDevice) {
        // Not needed in this activity
    }

    override fun onConnectionStateChanged(status: String) {
        runOnUiThread {
            tvConnectionStatus.text = status
            if (status == "Disconnected") {
                finish()
            }
        }
    }

    override fun onDataReceived(data: String) {
        runOnUiThread {
            tvReceivedData.append(data + "\n")
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        // We don't necessarily want to disconnect here if we want to keep it simple,
        // but usually you should. For this test, let's just clear listeners.
        bleManager.bleListener = null
        bleManager.dataListener = null
    }
}
