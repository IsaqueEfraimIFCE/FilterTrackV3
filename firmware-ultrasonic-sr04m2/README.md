# SR04M-2 Ultrasonic Firmware

Standalone ESP-IDF firmware for an ESP32-C3 and one JSN-SR04M-2 ultrasonic
module in standard trigger/echo mode. It prints distance readings to the
ESP32-C3 USB serial console at 115200 baud.

## Wiring

| SR04M-2 pin | ESP32-C3 connection |
|---|---|
| VCC | regulated supply compatible with the module |
| GND | GND |
| Trig/RX | GPIO21 (output) |
| Echo/TX | GPIO20 (input) |

This uses the module's normal pulse-width mode and does not require UART-mode
configuration. At 5 V supply, level-shift the `Echo/TX` output to 3.3 V before
connecting it to GPIO21.

## Measurement

Every 100 ms, GPIO21 is held high for 20 us to start a measurement. The
firmware measures the high pulse on GPIO20 and calculates distance from the
datasheet conversion `distance cm = echo width us / 57.5`. Distance is reported
in millimetres.

## Build and flash

From this directory, with ESP-IDF 5.5.1 configured:

```powershell
idf.py set-target esp32c3
idf.py build
idf.py -p COM15 flash monitor
```

Use the actual USB serial port in place of `COM15`.
