param(
    [string]$AdbPath = "$env:USERPROFILE\AppData\Local\Android\Sdk\platform-tools\adb.exe",
    [string]$Package = "com.filtertrack",
    [int]$DevToolsPort = 9222
)

$ErrorActionPreference = "Stop"
$script:CdpId = 0

function Connect-WebView {
    $appProcessId = (& $AdbPath shell pidof $Package).Trim()
    if (-not $appProcessId) {
        & $AdbPath shell am start -n "$Package/.MainActivity" | Out-Null
        Start-Sleep -Seconds 3
        $appProcessId = (& $AdbPath shell pidof $Package).Trim()
    }
    if (-not $appProcessId) { throw "FilterTrack is not running on Android." }
    $forwards = & $AdbPath forward --list
    if ($forwards -match "tcp:$DevToolsPort\b") {
        & $AdbPath forward --remove "tcp:$DevToolsPort" | Out-Null
    }
    & $AdbPath forward "tcp:$DevToolsPort" "localabstract:webview_devtools_remote_$appProcessId" | Out-Null
    $deadline = (Get-Date).AddSeconds(10)
    $target = $null
    while ((Get-Date) -lt $deadline -and -not $target) {
        try {
            $target = @(Invoke-RestMethod "http://127.0.0.1:$DevToolsPort/json") |
                Where-Object { $_.url -like "file:///android_asset/index.html*" } |
                Select-Object -First 1
        } catch {}
        if (-not $target) { Start-Sleep -Milliseconds 250 }
    }
    if (-not $target) { throw "FilterTrack WebView target not found." }
    $socket = [Net.WebSockets.ClientWebSocket]::new()
    [void]$socket.ConnectAsync([Uri]$target.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
    return $socket
}

function Invoke-JavaScript {
    param([Net.WebSockets.ClientWebSocket]$Socket, [string]$Expression)
    $script:CdpId++
    $requestId = $script:CdpId
    $request = @{
        id = $requestId
        method = "Runtime.evaluate"
        params = @{ expression = $Expression; returnByValue = $true; awaitPromise = $true }
    } | ConvertTo-Json -Depth 8 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($request)
    [void]$Socket.SendAsync([ArraySegment[byte]]::new($bytes), [Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
    while ($true) {
        $memory = [IO.MemoryStream]::new()
        do {
            $buffer = New-Object byte[] 65536
            $received = $Socket.ReceiveAsync([ArraySegment[byte]]::new($buffer), [Threading.CancellationToken]::None).GetAwaiter().GetResult()
            $memory.Write($buffer, 0, $received.Count)
        } while (-not $received.EndOfMessage)
        $response = ([Text.Encoding]::UTF8.GetString($memory.ToArray()) | ConvertFrom-Json)
        if ($response.id -eq $requestId) {
            if ($response.result.exceptionDetails) { throw "WebView evaluation failed: $($response.result.exceptionDetails.text)" }
            return $response.result.result.value
        }
    }
}

function Get-EspState {
    param([Net.WebSockets.ClientWebSocket]$Socket)
    $json = Invoke-JavaScript $Socket "JSON.stringify(window.__espFlashTestState || {})"
    return $json | ConvertFrom-Json
}

function Send-EspCommand {
    param([Net.WebSockets.ClientWebSocket]$Socket, [string]$Command)
    $commandJson = $Command | ConvertTo-Json -Compress
    $result = Invoke-JavaScript $Socket "bridge.sendCommand($commandJson) !== false"
    if ($result -ne $true) { throw "Failed to send ESP command: $Command" }
}

function Wait-ForMessage {
    param(
        [Net.WebSockets.ClientWebSocket]$Socket,
        [long]$AfterSequence,
        [string]$Pattern,
        [int]$TimeoutSeconds = 20
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $state = Get-EspState $Socket
        $match = @($state.controlLog | Where-Object { [long]$_.seq -gt $AfterSequence -and $_.msg -match $Pattern }) | Select-Object -First 1
        if ($match) { return $match }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for ESP notification matching: $Pattern"
}

function Last-ControlSequence {
    param([Net.WebSockets.ClientWebSocket]$Socket)
    $state = Get-EspState $Socket
    if (-not $state.controlLog -or @($state.controlLog).Count -eq 0) { return 0L }
    return [long](@($state.controlLog | Measure-Object seq -Maximum)[0].Maximum)
}

$socket = $null
$testRecordsAdded = $false
$testStartedAt = [DateTimeOffset]::UtcNow.ToString("o")

try {
    $socket = Connect-WebView
    [void](Invoke-JavaScript $socket "(() => { if (!window.__espFlashTestUnsub) window.__espFlashTestUnsub = bridge.subscribe(s => window.__espFlashTestState = { connected:s.connected, status:s.connectionStatus, deviceName:s.deviceName, deviceAddress:s.deviceAddress, controlLog:s.controlLog }); return true; })()")
    $state = Get-EspState $socket
    if (-not $state.connected) { throw "FilterTrackV3 is not connected to the Android app." }

    # Do not mix synthetic records with real pending wash records.
    $sequence = Last-ControlSequence $socket
    Send-EspCommand $socket "LOG:COUNT"
    $emptyCount = Wait-ForMessage $socket $sequence '^WL=CNT,'
    $countParts = $emptyCount.msg.Substring(7).Split(',')
    if ([int]$countParts[0] -ne 0) {
        throw "ESP already has $($countParts[0]) real/pending flash record(s); test stopped without modifying them."
    }
    $bootBefore = [int]$countParts[1]

    Start-Sleep -Seconds 1
    $sequence = Last-ControlSequence $socket
    Send-EspCommand $socket "LOG:TEST"
    $added = Wait-ForMessage $socket $sequence '^(WL=ADD,|WL=ERR,)'
    if ($added.msg -ne "WL=ADD,2") {
        throw "ESP rejected the synthetic flash write: $($added.msg)"
    }
    $addedCount = [int]$added.msg.Substring(7)
    if ($addedCount -ne 2) { throw "LOG:TEST produced $addedCount records instead of 2." }
    $testRecordsAdded = $true

    $sequence = Last-ControlSequence $socket
    Send-EspCommand $socket "LOG:COUNT"
    $beforeRestart = Wait-ForMessage $socket $sequence '^WL=CNT,2,'

    # Firmware command 0 calls esp_restart(). The Android process remains alive,
    # reconnects, then automatically performs COUNT -> READ -> ACK.
    $sequence = Last-ControlSequence $socket
    Send-EspCommand $socket "0"
    $afterRestartCount = Wait-ForMessage $socket $sequence '^WL=CNT,2,' 45
    $restartParts = $afterRestartCount.msg.Substring(7).Split(',')
    $bootAfter = [int]$restartParts[1]
    if ($bootAfter -le $bootBefore) {
        throw "ESP boot counter did not increase ($bootBefore -> $bootAfter)."
    }

    $endMessage = Wait-ForMessage $socket $sequence '^WL=END$' 20
    $clearMessage = Wait-ForMessage $socket $sequence '^WL=CLR$' 20
    $finalState = Get-EspState $socket
    $records = @($finalState.controlLog | Where-Object { [long]$_.seq -gt $sequence -and $_.msg -match '^WL=R,' })
    if ($records.Count -ne 2) { throw "Expected 2 restored records after reboot; received $($records.Count)." }

    $parsed = @($records | ForEach-Object {
        $parts = $_.msg.Substring(5).Split(',')
        [pscustomobject]@{
            Boot = [int]$parts[0]
            Uptime = [int]$parts[1]
            Type = [int]$parts[2]
            A = [int]$parts[3]
            B = [int]$parts[4]
            Count = [int]$parts[5]
            Aux = [int]$parts[6]
        }
    })
    $event = @($parsed | Where-Object Type -eq 1)
    $wash = @($parsed | Where-Object Type -eq 0)
    if ($event.Count -ne 1 -or $wash.Count -ne 1) { throw "Restored record types were not one event plus one wash summary." }
    if ($event[0].Count -ne 3 -or $event[0].Aux -ne 90 -or ($event[0].B - $event[0].A) -ne 120) {
        throw "Synthetic event payload did not match firmware LOG:TEST values."
    }
    if ($wash[0].Count -ne 1 -or $wash[0].Aux -ne 600 -or ($wash[0].B - $wash[0].A) -ne 20) {
        throw "Synthetic wash payload did not match firmware LOG:TEST values."
    }

    $sequenceAfterClear = Last-ControlSequence $socket
    Send-EspCommand $socket "LOG:COUNT"
    $zeroAfterAck = Wait-ForMessage $socket $sequenceAfterClear '^WL=CNT,0,'

    $startedJson = $testStartedAt | ConvertTo-Json -Compress
    $androidCopyJson = Invoke-JavaScript $socket "JSON.stringify((() => { const start=Date.parse($startedJson); let a=[]; try { a=JSON.parse(localStorage.getItem('filtertrack.washLog.v1')||'[]'); } catch (_) {} return (Array.isArray(a)?a:[]).filter(r => Date.parse(r.receivedAt||'') >= start && ((r.type===1 && r.count===3 && r.aux===90) || (r.type===0 && r.count===1 && r.aux===600))); })())"
    $androidCopies = @($androidCopyJson | ConvertFrom-Json)
    if ($androidCopies.Count -ne 2) { throw "Android did not retain exactly 2 copied test records." }

    [pscustomobject]@{
        Result = "PASS"
        Device = "$($state.deviceName) $($state.deviceAddress)"
        BootCounter = "$bootBefore -> $bootAfter"
        FlashRecordsBeforeRestart = 2
        FlashRecordsRestored = $records.Count
        RestoredTypes = (($parsed | Sort-Object Type -Descending | ForEach-Object { $_.Type }) -join ",")
        EndMarkerReceived = ($endMessage.msg -eq "WL=END")
        AckClearReceived = ($clearMessage.msg -eq "WL=CLR")
        FlashCountAfterAck = 0
        AndroidCopiesVerified = $androidCopies.Count
    } | Format-List
    $testRecordsAdded = $false
}
finally {
    if ($socket) {
        if ($testRecordsAdded) {
            # The precondition guaranteed an empty log, so any two records left
            # after a failed run are the synthetic test pair.
            try { Send-EspCommand $socket "LOG:ACK:2" } catch {}
        }
        # Remove only the synthetic Android copies created during this run.
        $startedJson = $testStartedAt | ConvertTo-Json -Compress
        $cleanup = "(() => { const start=Date.parse($startedJson); let a=[]; try { a=JSON.parse(localStorage.getItem('filtertrack.washLog.v1')||'[]'); } catch (_) {} if(!Array.isArray(a))a=[]; a=a.filter(r => !(Date.parse(r.receivedAt||'') >= start && ((r.type===1 && r.count===3 && r.aux===90) || (r.type===0 && r.count===1 && r.aux===600)))); localStorage.setItem('filtertrack.washLog.v1',JSON.stringify(a)); return true; })()"
        try { [void](Invoke-JavaScript $socket $cleanup); Start-Sleep -Seconds 2 } catch {}
        $socket.Dispose()
    }
    $forwards = & $AdbPath forward --list
    if ($forwards -match "tcp:$DevToolsPort\b") {
        & $AdbPath forward --remove "tcp:$DevToolsPort" | Out-Null
    }
}
