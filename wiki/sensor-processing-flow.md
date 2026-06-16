# Sensor Processing And Flow

This page captures how the Android app turns BLE distance notifications into
velocity, flow, charts, and session samples.

## Angle Correction — Sensor Mounting

The LSM303DLHC accelerometer is mounted with its **Z axis perpendicular to the
ultrasonic beam**, not aligned with it.

Physical mapping:

| Sensor orientation | gz (normalized) | acos(gz) | sin(acos(gz)) | Effect on distance |
|---|---|---|---|---|
| Straight down (beam ↓) | ≈ 0 | 90° | 1.0 | No correction |
| Tilted θ from vertical | ≈ sin(θ) | 90° − θ | cos(θ) | Reduces by cos(θ) |
| Horizontal (beam →) | ≈ 1 | 0° | 0.0 | Output = 0 |

Formula:

```text
angleFromGroundDeg = acos(|gz_normalized|)    // degrees
verticalDistance   = rawDistance * sin(angleFromGroundDeg)
```

This is implemented as `Math.acos(Math.abs(dot)) * DEG_PER_RAD` in
`calculateAngleFromGroundDeg` (index.html and filtertrack_serial.py).

**Do not change `acos` to `asin`.** The common mistake is to assume gz ≈ 1 when
the sensor is vertical (which would be true if Z were aligned with the beam). In
this mounting, gz ≈ 0 when vertical because Z is orthogonal to the beam direction.

## Parsing And Filtering

The WebView pipeline supports:

- `DIST=...`
- distance plus optional `ACC_RAW`
- numeric strings
- JSON-like payloads

Current rules:

- Distance readings below `25 cm` are ignored.
- The first reading after connect or command reset is discarded.
- A robust 2-second median/MAD filter is applied before flow calculation.
- Movement under `0.1 cm` over the 10-second analysis window is ignored.
- Velocity above `10 m/min` is treated as invalid.

Debug mode can show raw ESP payloads, raw values, processed distance, movement,
velocity, and flow.

## Velocity And Flow

Velocity and flow are computed on a 10-second window.

Formula:

```text
flowLpm = areaM2 * velocityMPerMin * 1000
```

Sign convention:

- Distance decreasing means water level rising.
- Water level rising means positive velocity and positive flow.
- Positive flow is `Subindo` and uses green/success styling.
- Negative flow is `Descendo` and uses red/danger styling.

The BI analytics should match this convention.

## Monitor Display

The Monitor tab shows:

- `velocidade de subida` in `m/min`, 2 decimals.
- `vazao`, default `L/min`, 1 decimal.
- Tapping the flow tile toggles between `L/min` and `m3/h`.

Both velocity and flow refresh every 10 seconds. During the first 10 seconds,
both tiles show a collecting-data countdown and spinner. When velocity is
unavailable, flow is unavailable too and both tiles show the current reason.

The chart plots velocity, not raw distance.

## Chart Accumulation

The velocity and flow history now accumulates all points from session start until
flow direction inverts. The previous behavior limited history to recent points.

Implementation concept:

```text
lastWindowVelocitySignRef tracks Math.sign(velocityMpm)
```

An inversion occurs when both the previous and current signs are non-zero and
the sign changes.

On inversion:

1. History resets to the current point.
2. A 10-second countdown starts.
3. Window timing resets.
4. Accumulation resumes after the countdown.

See [../info/monitor-chart-changes.md](../info/monitor-chart-changes.md) for the
source notes from 2026-05-07.

## Session Continuity Rule

On unexpected connection loss, the app keeps the active local session open for
automatic reconnect only when the latest displayed velocity is above
`0.1 m/min`. Otherwise it finalizes the old session and lets the next connection
start a new one.

