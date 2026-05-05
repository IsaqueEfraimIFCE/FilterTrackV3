param(
  [string]$BindHost = "0.0.0.0",
  [int]$Port = 8080,
  [string]$DataDir = ""
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

$env:HOST = $BindHost
$env:PORT = [string]$Port

if ($DataDir -and $DataDir.Trim()) {
  $resolvedDataDir = Resolve-Path -LiteralPath $DataDir -ErrorAction SilentlyContinue
  if ($resolvedDataDir) {
    $env:DATA_DIR = $resolvedDataDir.Path
  } else {
    $env:DATA_DIR = $DataDir
  }
} else {
  Remove-Item Env:DATA_DIR -ErrorAction SilentlyContinue
}

$lanIps = @()
try {
  $lanIps = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
    Where-Object {
      $_.IPAddress -and
      $_.IPAddress -ne "127.0.0.1" -and
      $_.IPAddress -notlike "169.254.*" -and
      $_.IPAddress -notlike "0.*"
    } |
    Select-Object -ExpandProperty IPAddress -Unique
} catch {
  $lanIps = @()
}

Write-Host ""
Write-Host "[FilterTrack] backend local server"
Write-Host "  host: $BindHost"
Write-Host "  port: $Port"
Write-Host ""
Write-Host "Use this endpoint in the app:"
Write-Host "  Emulator (Android Studio): http://10.0.2.2:$Port/filtertrack/sessions"
foreach ($ip in $lanIps) {
  Write-Host "  Physical device (same Wi-Fi): http://${ip}:$Port/filtertrack/sessions"
}
Write-Host "  This PC (curl/browser): http://127.0.0.1:$Port/health"
Write-Host ""

node src/server.js
