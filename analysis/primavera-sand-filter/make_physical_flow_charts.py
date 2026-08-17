"""Physics-constrained alternative to the median/MAD chart pipeline.

For each second, raw samples are grouped into distance clusters.  A continuous
water-level path is then selected: a candidate must be within 1 cm/s of the
recent 10-second median level, and the closest/highest-supported candidate wins.
Velocity is a 30-second regression of that path.  No line bridges data gaps.
"""

from pathlib import Path
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
SOURCE_DIR = ROOT / "source-csv"
AREA_M2 = 3.15
MIN_DISTANCE_CM = 25
CLUSTER_GAP_CM = 3.0
MAX_LEVEL_CHANGE_CM_PER_SECOND = 1.0
REFERENCE_SECONDS = 10
VELOCITY_WINDOW_SECONDS = 30


def clusters(values: np.ndarray) -> list[tuple[float, int]]:
    """Cluster same-second readings; a cluster center is a candidate level."""
    values = np.sort(values)
    if not len(values):
        return []
    groups = [[values[0]]]
    for value in values[1:]:
        if value - groups[-1][-1] <= CLUSTER_GAP_CM:
            groups[-1].append(value)
        else:
            groups.append([value])
    return [(float(np.median(group)), len(group)) for group in groups]


def select_level_path(session: pd.DataFrame) -> pd.DataFrame:
    session = session.sort_values("timestamp").copy()
    session["second"] = session["timestamp"].dt.floor("s")
    seconds = pd.date_range(session["second"].min(), session["second"].max(), freq="1s", tz="UTC")
    selected = []
    for second in seconds:
        candidates = clusters(session.loc[session["second"] == second, "distanceCm"].to_numpy(dtype=float))
        if not candidates:
            continue
        recent = [row for row in selected if row["timestamp"] >= second - pd.Timedelta(seconds=REFERENCE_SECONDS)]
        if not recent:
            # At the beginning of a continuous trace, prefer the strongest cluster.
            level, support = max(candidates, key=lambda item: (item[1], -item[0]))
        else:
            reference = float(np.median([row["distance_cm"] for row in recent]))
            previous = selected[-1]
            elapsed = (second - previous["timestamp"]).total_seconds()
            allowed_step = MAX_LEVEL_CHANGE_CM_PER_SECOND * max(elapsed, 1)
            viable = [(level, support) for level, support in candidates if abs(level - previous["distance_cm"]) <= allowed_step]
            if not viable:
                # The measurement is physically discontinuous: leave a gap.
                continue
            level, support = min(viable, key=lambda item: (abs(item[0] - reference), -item[1]))
        selected.append({"timestamp": second, "distance_cm": level, "cluster_support": support,
                         "session_id": session["sessionId"].iloc[0], "source_file": session["source_file"].iloc[0]})
    return pd.DataFrame(selected)


def derive_velocity(path: pd.DataFrame) -> pd.DataFrame:
    rows = []
    for _, point in path.iterrows():
        window = path[(path["timestamp"] >= point["timestamp"] - pd.Timedelta(seconds=VELOCITY_WINDOW_SECONDS)) & (path["timestamp"] <= point["timestamp"])]
        if len(window) < 15:
            continue
        elapsed = (window["timestamp"] - window["timestamp"].iloc[0]).dt.total_seconds().to_numpy()
        if elapsed[-1] - elapsed[0] < VELOCITY_WINDOW_SECONDS * 0.8:
            continue
        velocity_mpm = -np.polyfit(elapsed, window["distance_cm"].to_numpy(), 1)[0] * 0.6
        if abs(velocity_mpm) < 0.01:
            velocity_mpm = 0.0
        rows.append({"timestamp": point["timestamp"], "velocity_mpm": velocity_mpm,
                     "flow_lpm": AREA_M2 * velocity_mpm * 1000,
                     "session_id": point["session_id"], "source_file": point["source_file"]})
    return pd.DataFrame(rows)


def plot(metrics: pd.DataFrame, value: str, ylabel: str, title: str, output: str) -> None:
    fig, ax = plt.subplots(figsize=(14, 7), constrained_layout=True)
    for number, ((_, session_id), group) in enumerate(metrics.groupby(["source_file", "session_id"], sort=False), start=1):
        group = group.sort_values("timestamp")
        segment_ids = group["timestamp"].diff().dt.total_seconds().gt(2).cumsum()
        for segment_number, (_, segment) in enumerate(group.groupby(segment_ids)):
            colors = ["#1f77b4", "#ff7f0e", "#16a34a", "#9333ea"]
            ax.plot(segment["timestamp"], segment[value], color=colors[(number - 1) % len(colors)], marker="o", markersize=2.2, linewidth=1.5,
                    label=f"Session {number} · {session_id[-6:]}" if segment_number == 0 else None)
    ax.axhline(0, color="#4b5563", linewidth=0.8)
    ax.set(title=title, xlabel="Timestamp (UTC)", ylabel=ylabel)
    ax.grid(True, alpha=0.25); ax.legend(loc="upper right")
    fig.savefig(ROOT / output, dpi=200, bbox_inches="tight")
    plt.close(fig)


def main() -> None:
    raw = pd.concat([pd.read_csv(file).assign(source_file=file.name) for file in sorted(SOURCE_DIR.glob("filtertrack-samples*.csv"))], ignore_index=True)
    raw["timestamp"] = pd.to_datetime(raw["timestamp"], utc=True, format="mixed")
    raw = raw[raw["distanceCm"] >= MIN_DISTANCE_CM].copy()
    paths = [select_level_path(group) for _, group in raw.groupby(["source_file", "sessionId"], sort=False)]
    paths = [path for path in paths if not path.empty]
    metrics = [derive_velocity(path) for path in paths]
    metrics = [item for item in metrics if not item.empty]
    selected = pd.concat(paths, ignore_index=True)
    flow = pd.concat(metrics, ignore_index=True)
    selected.to_csv(ROOT / "physical_selected_level_path.csv", index=False)
    flow.to_csv(ROOT / "physical_velocity_flow.csv", index=False)
    plot(flow, "velocity_mpm", "Velocity (m/min)", "Primavera Sand Filter — Physics-Constrained Velocity", "physical_velocity.png")
    plot(flow, "flow_lpm", "Flow (L/min)", "Primavera Sand Filter — Physics-Constrained Flow (area = 3.15 m²)", "physical_flow.png")
    print(f"Selected path points: {len(selected):,}; velocity points: {len(flow):,}")
    print(flow[["velocity_mpm", "flow_lpm"]].describe().round(3).to_string())


if __name__ == "__main__":
    main()
