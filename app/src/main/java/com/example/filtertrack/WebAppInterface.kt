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
        object OpenCsvGuide : Action()
        data class SetInitialGuideActive(val active: Boolean) : Action()
        data class SetGuideDataButtonsVisible(val visible: Boolean) : Action()
        data class CompleteInitialGuide(val showNextTime: Boolean) : Action()
        data class SetGuideStartupPreference(val showOnStartup: Boolean) : Action()
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
    fun openCsvGuide() { onAction(Action.OpenCsvGuide) }

    @JavascriptInterface
    fun setInitialGuideActive(active: Boolean) { onAction(Action.SetInitialGuideActive(active)) }

    @JavascriptInterface
    fun setGuideDataButtonsVisible(visible: Boolean) {
        onAction(Action.SetGuideDataButtonsVisible(visible))
    }

    @JavascriptInterface
    fun completeInitialGuide(showNextTime: Boolean) {
        onAction(Action.CompleteInitialGuide(showNextTime))
    }

    @JavascriptInterface
    fun setGuideStartupPreference(showOnStartup: Boolean) {
        onAction(Action.SetGuideStartupPreference(showOnStartup))
    }

    @JavascriptInterface
    fun updateFirmware() { onAction(Action.UpdateFirmware) }

    @JavascriptInterface
    fun cancelFirmwareUpdate() { onAction(Action.CancelFirmwareUpdate) }

    @JavascriptInterface
    fun isBluetoothEnabled(): Boolean = bleManager.isBluetoothEnabled()
}
