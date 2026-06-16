# Firmware Build and Flash Guide

## Environment

Tested configuration (Windows):

| Component | Path |
|---|---|
| ESP-IDF | `C:\Espressif\frameworks\esp-idf-v5.5.1` |
| Python venv | `C:\Espressif\python_env\idf5.5_py3.14_env` |
| riscv32 toolchain | `C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin` |
| CMake | `C:\Espressif\tools\cmake\3.16.4\bin` |
| Ninja | `C:\Espressif\tools\ninja\1.12.1` |
| Git | `C:\Espressif\tools\idf-git\2.44.0\cmd` |
| esptool | system Python (`python -m esptool`) v5.2.0 |
| Target | ESP32-C3 rev v0.4, 4 MB flash (XMC), USB-Serial/JTAG |
| Default port | COM13 |

## Build

Set up the environment and build from `firmware/`:

```powershell
$IDF_PATH    = "C:\Espressif\frameworks\esp-idf-v5.5.1"
$IDF_PYTHON  = "C:\Espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe"
$CMAKE_BIN   = "C:\Espressif\tools\cmake\3.16.4\bin"
$NINJA_BIN   = "C:\Espressif\tools\ninja\1.12.1"
$RISCV_BIN   = "C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin"
$GIT_BIN     = "C:\Espressif\tools\idf-git\2.44.0\cmd"
$PYENV_BIN   = "C:\Espressif\python_env\idf5.5_py3.14_env\Scripts"

$env:IDF_PATH            = $IDF_PATH
$env:IDF_TOOLS_PATH      = "C:\Espressif"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\python_env\idf5.5_py3.14_env"
$env:PATH = "$CMAKE_BIN;$NINJA_BIN;$RISCV_BIN;$GIT_BIN;$PYENV_BIN;$env:PATH"

Set-Location firmware
& $IDF_PYTHON "$IDF_PATH\tools\idf.py" build
```

Output: `firmware/build/FilterTrackv3.bin` (851 KB, 19% of 1 MB partition).

### Stale build cache

If CMake errors mention the wrong IDF version (e.g. `esp-idf-v4.3.1`), delete the
build directory and rebuild:

```powershell
Remove-Item -Recurse -Force firmware\build
```

## Flash

Use system `esptool` (v5.2.0+). Run from `firmware/build/`:

```powershell
Set-Location firmware\build
python -m esptool --chip esp32c3 -p COM13 -b 460800 `
  --before default-reset --after hard-reset write-flash `
  --flash-mode dio --flash-size 2MB --flash-freq 80m `
  0x0   bootloader\bootloader.bin `
  0x8000 partition_table\partition-table.bin `
  0x10000 FilterTrackv3.bin
```

Replace `COM13` with the actual port if it differs.

### Flash address map

| Region | Offset | File |
|---|---|---|
| Bootloader | `0x00000` | `bootloader/bootloader.bin` |
| Partition table | `0x08000` | `partition_table/partition-table.bin` |
| App | `0x10000` | `FilterTrackv3.bin` |

These match `firmware/build/flasher_args.json` and must not be changed without
also updating `sdkconfig`.

## Verify

After flash the chip hard-resets automatically. Expected behaviour:

- Red LED blinks (advertising, disconnected).
- Device advertises `FilterTrackV3` over BLE.
- Serial monitor at 115200 baud shows `FilterTrack BLE pronto!` on startup.

## Troubleshooting

### `Could not open COMx — port is busy`

Another process holds the port (Android Studio serial monitor, VS Code, PlatformIO
terminal, etc.). Close it and retry.

### Device not found / timeout

ESP32-C3 with USB-Serial/JTAG does **not** require manual bootloader mode — it
enters automatically. If it still fails:

1. Try a different USB cable (data cable, not charge-only).
2. Try a different USB port.
3. Check Device Manager → Ports. The chip shows as `USB-Serial/JTAG (COMx)` with
   no extra driver needed on Windows 10/11.

### Baud rate errors

If `460800` fails, drop to `115200`:

```powershell
python -m esptool --chip esp32c3 -p COM13 -b 115200 ...
```

### esptool deprecation warnings

esptool v5+ uses hyphenated flags (`--flash-mode`, `write-flash`, etc.).
The old underscore forms still work but print warnings — use the hyphenated
forms to silence them.
