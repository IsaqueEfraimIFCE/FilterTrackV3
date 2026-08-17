<#!
.SYNOPSIS
    Port-agnostic PowerShell serial monitor for FilterTrack / ESP32 boards.

.DESCRIPTION
    By default, opens every registered COM port and reports the port(s) that
    actually receive bytes. A specified port is optional. COM10+ is supported.

.EXAMPLE
    .\monitor_filtertrack_ports.ps1
    .\monitor_filtertrack_ports.ps1 -Port COM15
    .\monitor_filtertrack_ports.ps1 -Port COM15 -BaudRate 115200
#>
param(
    [string]$Port,
    [int]$BaudRate = 115200,
    [switch]$IncludeBluetooth
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-SortedSerialPorts {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    $serialMap = Get-ItemProperty -Path 'HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM'
    $bluetoothPorts = @(
        $serialMap.PSObject.Properties |
            Where-Object { $_.Name -match 'BthModem' -and $_.Value -in $ports } |
            ForEach-Object Value
    )
    $ports |
        Where-Object { $IncludeBluetooth -or $_ -notin $bluetoothPorts } |
        Sort-Object { [int](($_ -replace '^COM', '')) }
}

if ($Port) {
    $ports = @($Port.ToUpperInvariant())
} else {
    $ports = @(Get-SortedSerialPorts)
    Write-Host "Scanning $($ports.Count) detected COM port(s) at $BaudRate baud..." -ForegroundColor Cyan
}

if ($ports.Count -eq 0) {
    Write-Host 'No COM ports found. Connect the ESP32 and run this script again.' -ForegroundColor Yellow
    exit 1
}

$connections = [System.Collections.Generic.List[System.IO.Ports.SerialPort]]::new()
$receivingPorts = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($portName in $ports) {
    try {
        $serial = [System.IO.Ports.SerialPort]::new($portName, $BaudRate, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
        $serial.Handshake = [System.IO.Ports.Handshake]::None
        $serial.ReadTimeout = 200
        $serial.WriteTimeout = 200
        $serial.DtrEnable = $false
        $serial.RtsEnable = $false
        $serial.Open()
        $connections.Add($serial)
        Write-Host "Connected: $portName at $BaudRate baud" -ForegroundColor Green
    } catch {
        Write-Host "Skipped ${portName}: $($_.Exception.Message)" -ForegroundColor DarkYellow
    }
}

if ($connections.Count -eq 0) {
    Write-Host 'No serial port could be opened. Close any app already using the ESP32 port.' -ForegroundColor Red
    exit 2
}

Write-Host 'Monitoring serial data. Press Ctrl+C to stop.' -ForegroundColor Cyan
try {
    while ($true) {
        foreach ($serial in @($connections)) {
            try {
                if ($serial.IsOpen -and $serial.BytesToRead -gt 0) {
                    $received = $serial.ReadExisting()
                    if ($received.Length -gt 0) {
                        $stamp = Get-Date -Format 'HH:mm:ss.fff'
                        if ($receivingPorts.Add($serial.PortName)) {
                            Write-Host "Receiving data on $($serial.PortName)." -ForegroundColor Green
                        }
                        $received -split "`r?`n" | Where-Object { $_ -ne '' } | ForEach-Object {
                            Write-Host "[$stamp][$($serial.PortName)] $_"
                        }
                    }
                }
            } catch {
                Write-Host "[$($serial.PortName)] Read error: $($_.Exception.Message)" -ForegroundColor Red
                $serial.Close()
            }
        }
        Start-Sleep -Milliseconds 50
    }
} finally {
    foreach ($serial in $connections) {
        if ($serial.IsOpen) { $serial.Close() }
        $serial.Dispose()
    }
}
