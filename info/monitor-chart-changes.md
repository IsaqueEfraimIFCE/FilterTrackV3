# Monitor Tab Chart Changes (2026-05-07)

## Overview
Modified the velocidade de subida (velocity) and vazão (flow) display behavior to accumulate all data from session start, resetting only when flow direction inverts.

## Previous Behavior
- Chart displayed only recent data: limited to `MAX_HISTORY_POINTS = 3600` points
- Every 10-second window, new data point was added: `.slice(-MAX_HISTORY_POINTS + 1)`
- Old data was discarded as new data arrived
- This meant only the most recent ~1 hour of continuous same-direction flow was visible

## New Behavior
- Chart accumulates **all data from 0 to current point** (no history limiting)
- Data persists as long as flow direction remains unchanged
- When flow inverts, entire history resets and re-accumulates

## Flow Inversion Detection

### Definition
Flow inversion occurs when the **direction of water movement reverses** between consecutive 10-second windows.

### Implementation
- Tracks velocity sign from each 10-second window: `Math.sign(velocityMpm)`
- Signs: `1` (positive/ascending), `-1` (negative/descending), `0` (no motion)
- Compares current window sign to previous window sign via `lastWindowVelocitySignRef`
- Inversion triggered when: `lastSign !== 0 && currentSign !== 0 && lastSign !== currentSign`

### Example
```
Window 1: distance 30cm → 40cm
  Change: +10cm (distance increased)
  Velocity: negative (water descending)
  Sign: -1

Window 2: distance 40cm → 35cm
  Change: -5cm (distance decreased)  
  Velocity: positive (water ascending)
  Sign: +1

Result: Sign changed from -1 to +1 → INVERSION DETECTED
```

## Data Reset Behavior

When flow inverts:
1. `setFlowHistory([{ ts, velocityMpm, flow10 }])` — Keep only current point
2. `setFlowWaitSeconds(10)` — Start 10-second countdown
3. `flowWindowStartMsRef.current = now` — Reset window timer
4. After 10 seconds: Resume normal accumulation with new data points

## Code Changes

### File: `app/src/main/assets/index.html`

#### New Ref
```javascript
const lastWindowVelocitySignRef = useRef(null);
```

#### Updated Functions
- `resetFlowMetrics()` — Reset `lastWindowVelocitySignRef` to null
- `finalizeSession()` — Reset `lastWindowVelocitySignRef` to null

#### Main Logic (tick function, ~line 1901-1920)
```javascript
} else if (now - lastInstantUpdateRef.current >= 10000) {
  const currentVelocitySign = Math.sign(velocityMpm);
  let flowInverted = false;
  if (lastWindowVelocitySignRef.current !== null && 
      lastWindowVelocitySignRef.current !== 0 && 
      currentVelocitySign !== 0) {
    flowInverted = lastWindowVelocitySignRef.current !== currentVelocitySign;
  }
  if (flowInverted) {
    setFlowHistory([{ ts: now, velocityMpm: round(velocityMpm, 4), flow10 }]);
    setFlowWaitSeconds(10);
    flowWindowStartMsRef.current = now;
  } else {
    setFlowHistory(prev => [...prev, { ts: now, velocityMpm: round(velocityMpm, 4), flow10 }]);
    setFlowWaitSeconds(0);
  }
  lastWindowVelocitySignRef.current = currentVelocitySign;
  // ... rest of update
}
```

## Testing Notes

### Expected Behavior
1. **Normal flow**: Chart grows continuously with each 10-second window
2. **Flow inversion**: Chart resets, shows "Coletando dados (10s)" countdown, then resumes accumulating
3. **Zero motion**: If velocity ≈ 0 (no motion detected), sign is 0 and inversion is NOT triggered

### Edge Cases
- First window ever: `lastWindowVelocitySignRef.current` is null, no inversion
- Velocity exactly 0: `Math.sign(0)` returns 0, skipped in comparison
- Sign remains 0 briefly: Chart doesn't accumulate until valid velocity detected

## Build Status
- Built and deployed: 2026-05-07, 30s build time
- Device: SM-A035M (Android 13)
- Status: APK installed, waiting for device reconnection
