param(
    [string]$CsvPath = "$PSScriptRoot\source-csv\filtertrack-samples.csv",
    [string]$AdbPath = "$env:USERPROFILE\AppData\Local\Android\Sdk\platform-tools\adb.exe",
    [string]$Package = "com.filtertrack",
    [string]$Activity = "com.filtertrack/.MainActivity",
    [int]$DevToolsPort = 9222
)

$ErrorActionPreference = "Stop"
$script:CdpRequestId = 0

function Connect-FilterTrackWebView {
    & $AdbPath shell am force-stop $Package | Out-Null
    & $AdbPath shell am start -n $Activity | Out-Null
    Start-Sleep -Seconds 3

    $appProcessId = (& $AdbPath shell pidof $Package).Trim()
    if (-not $appProcessId) {
        throw "FilterTrack process did not start."
    }

    $forwardList = & $AdbPath forward --list
    if ($forwardList -match "tcp:$DevToolsPort\b") {
        & $AdbPath forward --remove "tcp:$DevToolsPort" | Out-Null
    }
    & $AdbPath forward "tcp:$DevToolsPort" "localabstract:webview_devtools_remote_$appProcessId" | Out-Null

    $deadline = (Get-Date).AddSeconds(10)
    $target = $null
    while ((Get-Date) -lt $deadline -and -not $target) {
        try {
            $targets = @(Invoke-RestMethod "http://127.0.0.1:$DevToolsPort/json")
            $target = $targets | Where-Object { $_.url -eq "file:///android_asset/index.html" } | Select-Object -First 1
        } catch {
            Start-Sleep -Milliseconds 250
        }
    }
    if (-not $target) {
        throw "FilterTrack WebView debugging target was not found."
    }

    $socket = [System.Net.WebSockets.ClientWebSocket]::new()
    $token = [Threading.CancellationToken]::None
    [void]$socket.ConnectAsync([Uri]$target.webSocketDebuggerUrl, $token).GetAwaiter().GetResult()
    return $socket
}

function Invoke-CdpExpression {
    param(
        [Parameter(Mandatory)] [System.Net.WebSockets.ClientWebSocket]$Socket,
        [Parameter(Mandatory)] [string]$Expression
    )

    $script:CdpRequestId++
    $requestId = $script:CdpRequestId
    $message = @{
        id = $requestId
        method = "Runtime.evaluate"
        params = @{
            expression = $Expression
            returnByValue = $true
            awaitPromise = $true
        }
    } | ConvertTo-Json -Depth 8 -Compress

    $bytes = [Text.Encoding]::UTF8.GetBytes($message)
    [void]$Socket.SendAsync(
        [ArraySegment[byte]]::new($bytes),
        [Net.WebSockets.WebSocketMessageType]::Text,
        $true,
        [Threading.CancellationToken]::None
    ).GetAwaiter().GetResult()

    while ($true) {
        $stream = [IO.MemoryStream]::new()
        do {
            $buffer = New-Object byte[] 65536
            $result = $Socket.ReceiveAsync(
                [ArraySegment[byte]]::new($buffer),
                [Threading.CancellationToken]::None
            ).GetAwaiter().GetResult()
            $stream.Write($buffer, 0, $result.Count)
        } while (-not $result.EndOfMessage)

        $responseText = [Text.Encoding]::UTF8.GetString($stream.ToArray())
        $response = $responseText | ConvertFrom-Json
        if ($response.id -eq $requestId) {
            if ($response.result.exceptionDetails) {
                throw "WebView evaluation failed: $($response.result.exceptionDetails.text)"
            }
            return $response.result.result.value
        }
    }
}

if (-not (Test-Path -LiteralPath $CsvPath)) {
    throw "CSV not found: $CsvPath"
}

$rows = @(Import-Csv -LiteralPath $CsvPath)
if ($rows.Count -eq 0) {
    throw "CSV has no sample rows: $CsvPath"
}

$invariant = [Globalization.CultureInfo]::InvariantCulture
$t0 = [long]$rows[0].offsetMs
$offsets = [Collections.Generic.List[long]]::new()
$distances = [Collections.Generic.List[long]]::new()
foreach ($row in $rows) {
    $timestampMs = [long]$row.offsetMs
    $distanceCm = [double]::Parse($row.distanceCm, $invariant)
    $offsets.Add($timestampMs - $t0)
    $distances.Add([Math]::Round($distanceCm * 100))
}

$testId = "flash-test-$([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())"
$session = [ordered]@{
    id = $testId
    startedAt = $rows[0].timestamp
    endedAt = $rows[-1].timestamp
    endReason = "android_flash_persistence_test"
    device = [ordered]@{
        name = "FilterTrackV3"
        address = $rows[0].deviceAddress
    }
    filter = [ordered]@{
        id = $rows[0].filterId
        name = "FIL-01 Asc"
        areaM2 = 3.15
        station = "ETA PRIMAVERA"
        location = "Primavera"
        businessUnit = "test"
    }
    samples = [ordered]@{
        format = "distance_cm_x100_v1"
        t0 = $t0
        dtUnit = "ms"
        distanceUnit = "cm"
        scale = 100
        t = $offsets
        d = $distances
    }
}

$sessionJson = $session | ConvertTo-Json -Depth 12 -Compress
$samplesJson = $session.samples | ConvertTo-Json -Depth 8 -Compress
$testIdJson = $testId | ConvertTo-Json -Compress
$socket = $null
$oldServer = $null
$cleanupNeeded = $false

try {
    $socket = Connect-FilterTrackWebView
    $injectExpression = @"
(() => {
  const id = $testIdJson;
  const session = $sessionJson;
  const archiveKey = 'filtertrack.sessionArchive.v1';
  const pendingKey = 'filtertrack.pendingSessions.v1';
  const serverKey = 'filtertrack.serverUrl.v1';
  const readArray = key => { try { const value = JSON.parse(localStorage.getItem(key) || '[]'); return Array.isArray(value) ? value : []; } catch (_) { return []; } };
  const archive = readArray(archiveKey).filter(item => item && item.id !== id);
  const pending = readArray(pendingKey).filter(item => item && item.id !== id);
  const oldServer = localStorage.getItem(serverKey);
  archive.push(session);
  pending.push(session);
  localStorage.setItem(serverKey, 'http://127.0.0.1:9/filtertrack/sessions');
  localStorage.setItem(archiveKey, JSON.stringify(archive.slice(-300)));
  localStorage.setItem(pendingKey, JSON.stringify(pending.slice(-200)));
  return JSON.stringify({ oldServer, archiveCount: archive.length, pendingCount: pending.length });
})()
"@
    $injected = (Invoke-CdpExpression -Socket $socket -Expression $injectExpression) | ConvertFrom-Json
    $oldServer = $injected.oldServer
    $cleanupNeeded = $true
    # localStorage updates are synchronous to JavaScript, while Chromium's
    # backing store flush is asynchronous. Allow the committed values to reach
    # disk before force-stopping the app process.
    Start-Sleep -Seconds 2
    $socket.Dispose()
    $socket = $null

    # A complete process restart forces Chromium to reload the values from the
    # app's on-device WebView storage rather than retaining JavaScript memory.
    $socket = Connect-FilterTrackWebView
    $verifyExpression = @"
(() => {
  const id = $testIdJson;
  const expectedSamples = $samplesJson;
  const readArray = key => { try { const value = JSON.parse(localStorage.getItem(key) || '[]'); return Array.isArray(value) ? value : []; } catch (_) { return []; } };
  const archiveMatches = readArray('filtertrack.sessionArchive.v1').filter(item => item && item.id === id);
  const pendingMatches = readArray('filtertrack.pendingSessions.v1').filter(item => item && item.id === id);
  const stored = archiveMatches[0];
  return JSON.stringify({
    archiveMatches: archiveMatches.length,
    pendingMatches: pendingMatches.length,
    sampleCount: stored && stored.samples ? Math.min(stored.samples.t.length, stored.samples.d.length) : 0,
    exactSamples: !!stored && JSON.stringify(stored.samples) === JSON.stringify(expectedSamples),
    endReason: stored ? stored.endReason : null,
    serverUrl: localStorage.getItem('filtertrack.serverUrl.v1')
  });
})()
"@
    $verified = (Invoke-CdpExpression -Socket $socket -Expression $verifyExpression) | ConvertFrom-Json
    if ($verified.archiveMatches -ne 1 -or $verified.pendingMatches -ne 1) {
        throw "Persisted test session count mismatch: archive=$($verified.archiveMatches), pending=$($verified.pendingMatches), server=$($verified.serverUrl)."
    }
    if ($verified.sampleCount -ne $rows.Count -or -not $verified.exactSamples) {
        throw "Persisted sample data did not exactly match all $($rows.Count) CSV readings."
    }

    [pscustomobject]@{
        Result = "PASS"
        Device = (& $AdbPath shell getprop ro.product.model).Trim()
        TestSessionId = $testId
        SourceCsv = (Resolve-Path -LiteralPath $CsvPath).Path
        SourceRows = $rows.Count
        StoredRows = $verified.sampleCount
        ExactSampleMatch = $verified.exactSamples
        ArchiveMatches = $verified.archiveMatches
        PendingMatches = $verified.pendingMatches
        ProcessRestartVerified = $true
    } | Format-List
}
finally {
    if ($socket) {
        if ($cleanupNeeded) {
            $oldServerJson = if ($null -eq $oldServer) { "null" } else { $oldServer | ConvertTo-Json -Compress }
            $cleanupExpression = @"
(() => {
  const id = $testIdJson;
  const oldServer = $oldServerJson;
  const archiveKey = 'filtertrack.sessionArchive.v1';
  const pendingKey = 'filtertrack.pendingSessions.v1';
  const serverKey = 'filtertrack.serverUrl.v1';
  const clean = key => {
    let value = [];
    try { value = JSON.parse(localStorage.getItem(key) || '[]'); } catch (_) {}
    if (!Array.isArray(value)) value = [];
    localStorage.setItem(key, JSON.stringify(value.filter(item => item && item.id !== id)));
  };
  clean(archiveKey);
  clean(pendingKey);
  if (oldServer === null) localStorage.removeItem(serverKey); else localStorage.setItem(serverKey, oldServer);
  return 'cleaned';
})()
"@
            [void](Invoke-CdpExpression -Socket $socket -Expression $cleanupExpression)
            # Give Chromium's backing store the same flush interval used by
            # the persistence phase before the app process is stopped.
            Start-Sleep -Seconds 2
        }
        $socket.Dispose()
    }
    & $AdbPath shell am force-stop $Package | Out-Null
    & $AdbPath shell am start -n $Activity | Out-Null
    $forwardList = & $AdbPath forward --list
    if ($forwardList -match "tcp:$DevToolsPort\b") {
        & $AdbPath forward --remove "tcp:$DevToolsPort" | Out-Null
    }
}
