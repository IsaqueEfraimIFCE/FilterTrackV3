"""Create Primavera charts using the live FilterTrack distance post-processing.

All source CSVs are concatenated.  The stored distance is already the parsed
vertical distance (the export has no accelerometer payload), so this reproduces
the remaining live pipeline: 2-second robust median/MAD filtering each second,
10-second regression, 0.1-cm minimum motion, and the 10 m/min validity limit.
Falling sensor distance is positive/upward velocity; flow is v * 3.15 m² * 1000.
"""

from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


ROOT = Path(__file__).resolve().parent
SOURCE_DIR = ROOT / "source-csv"
AREA_M2 = 3.15
WINDOW_SECONDS = 10
POST_PROCESS_WINDOW_SECONDS = 2
ROBUST_MOVING_MEAN_WINDOW_SECONDS = 10
MIN_DISTANCE_CM = 25
MAX_DEVIATION_FROM_MOVING_MEAN_CM = 3
MAX_LOCAL_MAD_CM = 1.5


def regression_velocity_mpm(window: pd.DataFrame) -> float:
    """Return water velocity: negative sensor-distance slope, in m/min."""
    x = window["elapsed_s"].to_numpy(dtype=float)
    y = window["distance_cm"].to_numpy(dtype=float)
    slope_cm_per_second = np.polyfit(x, y, 1)[0]
    return -slope_cm_per_second * 0.6


def robust_filter_distance(values: pd.Series) -> float:
    """Exact robustFilterDistance behavior from the app for numeric values."""
    values = values.dropna().to_numpy(dtype=float)
    if len(values) == 0:
        return np.nan
    if len(values) < 3:
        return float(values[-1])
    ordered = np.sort(values)
    median = ordered[len(ordered) // 2]
    mad = np.sort(np.abs(ordered - median))[len(ordered) // 2]
    inliers = values[np.abs(values - median) <= max(mad * 3, 0.5)]
    return float(inliers.mean()) if len(inliers) else float(median)


def clean_original_readings(session: pd.DataFrame) -> pd.DataFrame:
    """Apply a centered 10-second robust mean to the original sensor series.

    Every reference mean uses samples from five seconds before through five
    seconds after the reading. The mean itself uses the median/MAD inlier rule,
    so outliers are excluded before they can influence the reference. Any raw
    reading farther than 3 cm from this robust mean is discarded. Accepted rows
    retain their source value in ``rawDistanceCm`` and use the robust mean as
    ``distanceCm`` for downstream velocity calculation.
    """
    original = session.sort_values("timestamp").reset_index(drop=True).copy()
    times_ns = original["timestamp"].astype("int64").to_numpy()
    values = original["distanceCm"].to_numpy(dtype=float)
    half_window_ns = int((ROBUST_MOVING_MEAN_WINDOW_SECONDS / 2) * 1_000_000_000)
    means = np.full(len(original), np.nan)
    left = right = 0
    for index in range(len(original)):
        lower = times_ns[index] - half_window_ns
        upper = times_ns[index] + half_window_ns
        while left < len(original) and times_ns[left] < lower:
            left += 1
        while right < len(original) and times_ns[right] <= upper:
            right += 1
        means[index] = robust_filter_distance(pd.Series(values[left:right], dtype=float))
    original["rawDistanceCm"] = original["distanceCm"]
    original["movingMeanCm"] = means
    original["deviationFromMovingMeanCm"] = (original["rawDistanceCm"] - original["movingMeanCm"]).abs()
    accepted = original[original["deviationFromMovingMeanCm"] <= MAX_DEVIATION_FROM_MOVING_MEAN_CM].copy()
    accepted["distanceCm"] = accepted["movingMeanCm"]
    return accepted


def centered_smoothed_points(session: pd.DataFrame) -> pd.DataFrame:
    """One-second series made from a centered 10-second robust mean."""
    session = session.sort_values("timestamp").copy()
    if session.empty:
        return pd.DataFrame()
    first_tick = session["timestamp"].iloc[0].ceil("s")
    last_tick = session["timestamp"].iloc[-1].floor("s")
    ticks = pd.date_range(first_tick, last_tick, freq="1s", tz="UTC")
    half_window = pd.Timedelta(seconds=ROBUST_MOVING_MEAN_WINDOW_SECONDS / 2)
    rows = []
    for tick in ticks:
        local = session[(session["timestamp"] >= tick - half_window) & (session["timestamp"] <= tick + half_window)]
        values = local["distanceCm"].to_numpy(dtype=float)
        if not len(values):
            continue
        median = np.median(values)
        mad = np.median(np.abs(values - median))
        moving_mean = robust_filter_distance(pd.Series(values, dtype=float))
        if np.isfinite(moving_mean) and mad <= MAX_LOCAL_MAD_CM:
            rows.append({"timestamp": tick, "distance_cm": moving_mean, "local_mad_cm": mad, "session_id": session["sessionId"].iloc[0], "source_file": session["source_file"].iloc[0]})
    return pd.DataFrame(rows)


def process_session(session: pd.DataFrame) -> tuple[pd.DataFrame, pd.DataFrame]:
    if session.empty:
        return pd.DataFrame(), pd.DataFrame()
    points = centered_smoothed_points(session)
    if points.empty:
        return points, pd.DataFrame()
    points["elapsed_s"] = (points["timestamp"] - points["timestamp"].iloc[0]).dt.total_seconds()
    rows = []
    for _, point in points.iterrows():
        end = point["elapsed_s"]
        window = points[(points["elapsed_s"] >= end - WINDOW_SECONDS) & (points["elapsed_s"] <= end)]
        if len(window) < 2 or window["elapsed_s"].iloc[-1] - window["elapsed_s"].iloc[0] < WINDOW_SECONDS * 0.9:
            continue
        motion_cm = window["distance_cm"].iloc[-1] - window["distance_cm"].iloc[0]
        if abs(motion_cm) < 0.1:
            continue
        velocity = regression_velocity_mpm(window)
        if abs(velocity) > 10:
            continue
        rows.append({"timestamp": point["timestamp"], "elapsed_s": end, "velocity_mpm": velocity, "flow_lpm": AREA_M2 * velocity * 1000, "motion_cm": motion_cm})
    result = pd.DataFrame(rows)
    if not result.empty:
        result["session_id"] = session["sessionId"].iloc[0]
        result["source_file"] = session["source_file"].iloc[0]
    return points, result


def save_chart(metrics: pd.DataFrame, value_column: str, ylabel: str, title: str, filename: str) -> None:
    fig, ax = plt.subplots(figsize=(14, 7), constrained_layout=True)
    colors = ["#1f77b4", "#ff7f0e"]
    for number, ((source_file, session_id), group) in enumerate(metrics.groupby(["source_file", "session_id"], sort=False), start=1):
        label = f"Session {number} · {session_id[-6:]}"
        color = colors[(number - 1) % len(colors)]
        group = group.sort_values("timestamp")
        segment_ids = group["timestamp"].diff().dt.total_seconds().gt(2).cumsum()
        for segment_number, (_, segment) in enumerate(group.groupby(segment_ids)):
            ax.plot(segment["timestamp"], segment[value_column], marker="o", markersize=2.2, linewidth=1.2, color=color, label=label if segment_number == 0 else None)
    ax.axhline(0, color="#4b5563", linewidth=0.8)
    ax.set_title(title, weight="bold")
    ax.set_xlabel("Timestamp (UTC)")
    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.25)
    ax.legend(title="Concatenated-session trace", fontsize=8, loc="upper right")
    fig.savefig(ROOT / filename, dpi=200, bbox_inches="tight")
    plt.close(fig)


def main() -> None:
    csv_files = sorted(SOURCE_DIR.glob("filtertrack-samples*.csv"))
    if not csv_files:
        raise FileNotFoundError(f"No FilterTrack sample CSVs found in {SOURCE_DIR}")
    raw = pd.concat([pd.read_csv(path).assign(source_file=path.name) for path in csv_files], ignore_index=True)
    raw["timestamp"] = pd.to_datetime(raw["timestamp"], utc=True, format="mixed")
    raw = raw[raw["distanceCm"] >= MIN_DISTANCE_CM].copy()
    raw.sort_values(["timestamp", "source_file", "index"]).to_csv(ROOT / "filtertrack-samples-concatenated.csv", index=False)
    cleaned_original = [clean_original_readings(group) for _, group in raw.groupby(["source_file", "sessionId"], sort=False)]
    pd.concat(cleaned_original, ignore_index=True).sort_values(["timestamp", "source_file", "index"]).to_csv(ROOT / "filtertrack-samples-cleaned.csv", index=False)
    processed = [process_session(group) for _, group in raw.groupby(["source_file", "sessionId"], sort=False)]
    filtered_points = pd.concat([item[0] for item in processed if not item[0].empty], ignore_index=True)
    metrics = pd.concat([item[1] for item in processed if not item[1].empty], ignore_index=True)
    if metrics.empty:
        raise ValueError("No valid 10-second velocity windows were produced.")
    filtered_points.to_csv(ROOT / "primavera_postprocessed_distance.csv", index=False)
    metrics.to_csv(ROOT / "primavera_velocity_flow.csv", index=False)
    save_chart(metrics, "velocity_mpm", "Velocity (m/min)", "Primavera Sand Filter — Velocity from Sensor Distance", "primavera_velocity.png")
    save_chart(metrics, "flow_lpm", "Flow (L/min)", "Primavera Sand Filter — Flow from Sensor Distance (area = 3.15 m²)", "primavera_flow.png")
    accepted_raw_rows = sum(len(group) for group in cleaned_original)
    print(f"Concatenated source rows: {len(raw):,}")
    print(f"Raw readings retained after the centered {ROBUST_MOVING_MEAN_WINDOW_SECONDS}-second robust mean and {MAX_DEVIATION_FROM_MOVING_MEAN_CM}-cm deviation filter: {accepted_raw_rows:,}")
    print(f"Coherent centered {ROBUST_MOVING_MEAN_WINDOW_SECONDS}-second distance points: {len(filtered_points):,} (local MAD <= {MAX_LOCAL_MAD_CM} cm)")
    print(f"Sessions: {raw['sessionId'].nunique():,}; valid 10-second velocity windows: {len(metrics):,}")
    print(metrics[["velocity_mpm", "flow_lpm"]].describe().round(3).to_string())


if __name__ == "__main__":
    main()
