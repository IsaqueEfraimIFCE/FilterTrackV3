package com.filtertrack

import android.webkit.JavascriptInterface

class WebAppInterface(
    private val bleManager: BLEManager,
    private val onAction: (Action) -> Unit,
) {
    sealed class Action {
        object StartScan : Action()
        object StopScan : Action()
        data class Connect(val address: String) : Action()
        object Disconnect : Action()
        data class SendCommand(val cmd: String) : Action()
        object OpenBiDashboard : Action()
        object UpdateFirmware : Action()
        object CancelFirmwareUpdate : Action()
    }

    @JavascriptInterface
    fun startScan() { onAction(Action.StartScan) }

    @JavascriptInterface
    fun stopScan() { onAction(Action.StopScan) }

    @JavascriptInterface
    fun connect(address: String) { onAction(Action.Connect(address)) }

    @JavascriptInterface
    fun disconnect() { onAction(Action.Disconnect) }

    @JavascriptInterface
    fun sendCommand(cmd: String) { onAction(Action.SendCommand(cmd)) }

    @JavascriptInterface
    fun openBiDashboard() { onAction(Action.OpenBiDashboard) }

    @JavascriptInterface
    fun updateFirmware() { onAction(Action.UpdateFirmware) }

    @JavascriptInterface
    fun cancelFirmwareUpdate() { onAction(Action.CancelFirmwareUpdate) }

    @JavascriptInterface
    fun isBluetoothEnabled(): Boolean = bleManager.isBluetoothEnabled()
}
