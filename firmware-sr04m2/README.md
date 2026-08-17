# FilterTrack SR04M-2 Firmware

Full FilterTrack ESP32-C3 firmware fork for the SR04M-2 ultrasonic module in
trigger/echo mode. It retains the main firmware's BLE protocol, LEDs,
calibration, sensor storage, wash detection, and OTA command handling. This
board has no accelerometer, so I2C accelerometer probing is disabled and BLE
reports `RAW=ERRO` with each distance measurement.

## SR04M-2 wiring

| SR04M-2 pin | ESP32-C3 |
|---|---|
| VCC | 3.3 V |
| GND | GND |
| Trig/RX | GPIO21 |
| Echo/TX | GPIO20 |

The module is sampled every 100 ms with a 20 us trigger pulse. The echo
timeout is 45 ms. The BLE advertised name and data contract remain
`FilterTrackV3` and the same as `firmware/`.

Sand mode keeps the same wash-detection behavior, but this fork also samples
at 100 ms while disconnected so its raw distance stream remains responsive.

USB-Serial/JTAG is the primary console, so UART0 does not drive GPIO21 or
interfere with the ultrasonic trigger. Serial logs remain available on COM15.

## Build and flash

```powershell
idf.py build
idf.py -p COM15 flash
```
