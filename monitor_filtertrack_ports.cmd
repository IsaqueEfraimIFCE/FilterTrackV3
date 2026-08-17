@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Pure CMD serial monitor for FilterTrack / ESP devices.
rem Default: opens one window per registered legacy COM port (COM1-COM9) at 115200 baud.
rem Usage:
rem   monitor_filtertrack_ports.cmd
rem   monitor_filtertrack_ports.cmd COM7
rem   monitor_filtertrack_ports.cmd COM7 9600
rem   monitor_filtertrack_ports.cmd --list

if /I "%~1"=="--monitor" goto monitor_worker
if /I "%~1"=="--list" goto list
if not "%~1"=="" (
  set "PORT=%~1"
  set "BAUD=%~2"
  if "%BAUD%"=="" set "BAUD=115200"
  goto stream
)

set "BAUD=115200"
set /a PORT_COUNT=0
echo Looking for registered serial ports...
for /f "skip=2 tokens=1,2,*" %%A in ('reg query "HKLM\HARDWARE\DEVICEMAP\SERIALCOMM" 2^>nul') do (
  set "PORT=%%C"
  if not "!PORT!"=="" (
    set /a PORT_COUNT+=1
    echo   !PORT!
    start "FilterTrack serial !PORT! @ !BAUD!" cmd /k call "%~f0" --monitor !PORT! !BAUD!
  )
)
if %PORT_COUNT% EQU 0 (
  echo No registered COM ports were found.
  echo Connect the ESP board, install its USB serial driver if needed, then run this script again.
)
exit /b

:list
reg query "HKLM\HARDWARE\DEVICEMAP\SERIALCOMM" 2>nul
if errorlevel 1 echo No registered COM ports were found.
exit /b

:monitor_worker
set "PORT=%~2"
set "BAUD=%~3"
if "%BAUD%"=="" set "BAUD=115200"
:stream
title FilterTrack serial %PORT% @ %BAUD%
color 0A
echo.
echo ============================================================
echo  FilterTrack serial monitor: %PORT% at %BAUD% baud
echo  Waiting for data. This window is the port that receives it.
echo  Press Ctrl+C to stop this port.
echo ============================================================
echo.

mode %PORT%: BAUD=%BAUD% PARITY=n DATA=8 STOP=1 >nul 2>&1
if errorlevel 1 (
  echo Could not open %PORT%. It may be disconnected or in use by another program.
  pause
  exit /b 1
)

set "PORT_NUMBER=%PORT:COM=%"
if %PORT_NUMBER% GTR 9 goto extended_port_notice

rem TYPE keeps a legacy COM1-COM9 port open and prints received serial bytes.
type %PORT%
echo.
echo %PORT% was closed or disconnected.
pause
exit /b

:extended_port_notice
echo.
echo CMD's built-in TYPE command cannot read extended serial ports such as %PORT%.
echo The connected ESP32 is on %PORT%, so a truly CMD-only monitor cannot stream it.
echo Use a serial terminal, or allow this .cmd launcher to call PowerShell/Python for COM10+ support.
pause
exit /b 2
