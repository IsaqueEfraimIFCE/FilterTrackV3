package com.example.filtertrack

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView

class MainActivity : AppCompatActivity(), BLEManager.BLEListener {

    private lateinit var bleManager: BLEManager
    private lateinit var btnStartScan: Button
    private lateinit var tvStatus: TextView
    private lateinit var rvDevices: RecyclerView
    private val deviceList = mutableListOf<BluetoothDevice>()
    private val deviceAdapter by lazy { DeviceAdapter(deviceList) { device ->
        bleManager.connect(device)
    } }

    private val requestPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val granted = permissions.entries.all { it.value }
        if (granted) {
            startBleScan()
        } else {
            Toast.makeText(this, "Permissions required for BLE", Toast.LENGTH_SHORT).show()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        bleManager = BLEManager.getInstance(this)
        bleManager.bleListener = this

        btnStartScan = findViewById(R.id.btnStartScan)
        tvStatus = findViewById(R.id.tvStatus)
        rvDevices = findViewById(R.id.rvDevices)

        rvDevices.layoutManager = LinearLayoutManager(this)
        rvDevices.adapter = deviceAdapter

        btnStartScan.setOnClickListener {
            checkPermissionsAndScan()
        }
    }

    private fun checkPermissionsAndScan() {
        val permissions = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            permissions.add(Manifest.permission.BLUETOOTH_SCAN)
            permissions.add(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            permissions.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }

        val missingPermissions = permissions.filter {
            ActivityCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }

        if (missingPermissions.isEmpty()) {
            startBleScan()
        } else {
            requestPermissionLauncher.launch(missingPermissions.toTypedArray())
        }
    }

    private fun startBleScan() {
        deviceList.clear()
        deviceAdapter.notifyDataSetChanged()
        tvStatus.text = "Status: Scanning..."
        bleManager.startScan()
    }

    override fun onDeviceFound(device: BluetoothDevice) {
        if (!deviceList.contains(device)) {
            deviceList.add(device)
            deviceAdapter.notifyItemInserted(deviceList.size - 1)
        }
    }

    override fun onConnectionStateChanged(status: String) {
        runOnUiThread {
            tvStatus.text = "Status: $status"
            if (status == "Connected") {
                val intent = Intent(this, DeviceControlActivity::class.java)
                startActivity(intent)
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        // Don't disconnect here because we might be moving to DeviceControlActivity
        if (bleManager.bleListener == this) {
            bleManager.bleListener = null
        }
    }

    class DeviceAdapter(
        private val devices: List<BluetoothDevice>,
        private val onClick: (BluetoothDevice) -> Unit
    ) : RecyclerView.Adapter<DeviceAdapter.ViewHolder>() {

        class ViewHolder(view: View) : RecyclerView.ViewHolder(view) {
            val name: TextView = view.findViewById(R.id.tvDeviceName)
            val address: TextView = view.findViewById(R.id.tvDeviceAddress)
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
            val view = LayoutInflater.from(parent.context).inflate(R.layout.item_device, parent, false)
            return ViewHolder(view)
        }

        @SuppressLint("MissingPermission")
        override fun onBindViewHolder(holder: ViewHolder, position: Int) {
            val device = devices[position]
            holder.name.text = device.name ?: "Unknown Device"
            holder.address.text = device.address
            holder.itemView.setOnClickListener { onClick(device) }
        }

        override fun getItemCount() = devices.size
    }
}
