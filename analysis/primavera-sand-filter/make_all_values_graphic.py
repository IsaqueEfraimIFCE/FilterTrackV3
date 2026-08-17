"""Render every Primavera sand-filter time-series value in one graphic.

The figure intentionally retains every source observation (rather than
down-sampling) and layers each processed result in separate panels so the
filtering and flow calculations remain auditable.
"""

import argparse
from pathlib import Path

import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import pandas as pd


ROOT = Path(__file__).resolve().parent
SOURCE_DIR = ROOT / "source-csv"
COLORS = {
    "filtertrack-samples.csv": "#2563eb",
    "filtertrack-samples2.csv": "#ea580c",
    "filtertrack-samples3.csv": "#16a34a",
    "filtertrack-samples4.csv": "#9333ea",
}
LABELS = {
    "filtertrack-samples.csv": "Session 1",
    "filtertrack-samples2.csv": "Session 2",
    "filtertrack-samples3.csv": "Session 3",
    "filtertrack-samples4.csv": "Session 4",
}


def load_csv(path: Path) -> pd.DataFrame:
    frame = pd.read_csv(path)
    frame["timestamp"] = pd.to_datetime(frame["timestamp"], utc=True, format="mixed")
    return frame


def scatter_by_source(ax, frame: pd.DataFrame, y: str, *, size: float, alpha: float, label: bool = True) -> None:
    for source, group in frame.groupby("source_file", sort=False):
        ax.scatter(
            group["timestamp"], group[y], s=size, alpha=alpha,
            color=source_color(source), label=source_label(source) if label else None,
            linewidths=0,
        )


def line_by_source(ax, frame: pd.DataFrame, y: str, *, marker_size: float = 1.5) -> None:
    for source, group in frame.groupby("source_file", sort=False):
        group = group.sort_values("timestamp")
        gaps = group["timestamp"].diff().dt.total_seconds().gt(2).cumsum()
        for number, (_, segment) in enumerate(group.groupby(gaps)):
            ax.plot(
                segment["timestamp"], segment[y], color=source_color(source), linewidth=0.9,
                marker="o", markersize=marker_size,
                label=source_label(source) if number == 0 else None,
            )


def format_axis(ax, ylabel: str) -> None:
    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.2, linewidth=0.6)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S", tz=mdates.UTC))
    ax.legend(loc="best", frameon=True, fontsize=8)


def source_color(source: str) -> str:
    """Return a stable fallback color for newly added source files."""
    fallback = ["#9333ea", "#0891b2", "#be123c", "#a16207"]
    return COLORS.get(source, fallback[sum(source.encode("utf-8")) % len(fallback)])


def source_label(source: str) -> str:
    return LABELS.get(source, source)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", help="Render only this source CSV filename")
    parser.add_argument("--output", default="primavera_all_values.png", help="Output PNG filename")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    raw = pd.concat(
        [load_csv(path).assign(source_file=path.name) for path in sorted(SOURCE_DIR.glob("filtertrack-samples*.csv"))],
        ignore_index=True,
    ).sort_values("timestamp")
    cleaned = load_csv(ROOT / "filtertrack-samples-cleaned.csv")
    postprocessed = load_csv(ROOT / "primavera_postprocessed_distance.csv")
    postprocessed = postprocessed.rename(columns={"distance_cm": "distanceCm"})
    velocity = load_csv(ROOT / "primavera_velocity_flow.csv")
    physical_path = load_csv(ROOT / "physical_selected_level_path.csv")
    physical_flow = load_csv(ROOT / "physical_velocity_flow.csv")

    if args.source:
        frames = [raw, cleaned, postprocessed, velocity, physical_path, physical_flow]
        raw, cleaned, postprocessed, velocity, physical_path, physical_flow = [
            frame[frame["source_file"] == args.source].copy() for frame in frames
        ]
        if raw.empty:
            raise ValueError(f"Source file not found in analysis data: {args.source}")

    fig, axes = plt.subplots(6, 1, figsize=(18, 24), sharex=True)
    fig.subplots_adjust(top=0.94, bottom=0.04, left=0.07, right=0.985, hspace=0.22)
    fig.suptitle(
        "Primavera Sand Filter — Complete Measurement and Flow Record"
        + (f" — {source_label(args.source)}" if args.source else ""),
        fontsize=17, fontweight="bold", y=0.985,
    )
    fig.text(
        0.5, 0.958,
        f"All values from {raw['source_file'].nunique():,} source files: "
        f"{len(raw):,} raw readings, {len(cleaned):,} robust-mean readings, "
        f"{len(postprocessed):,} standard distance points, {len(velocity):,} standard flow windows, "
        f"{len(physical_path):,} physics-selected points, and {len(physical_flow):,} physics flow windows. "
        "Area = 3.15 m².",
        ha="center", fontsize=9, color="#374151",
    )

    scatter_by_source(axes[0], raw, "distanceCm", size=4.5, alpha=0.32)
    axes[0].set_title("Raw ultrasonic distance — every recorded reading")
    format_axis(axes[0], "Sensor distance (cm)")

    scatter_by_source(axes[1], cleaned, "rawDistanceCm", size=5, alpha=0.22, label=False)
    line_by_source(axes[1], cleaned, "movingMeanCm", marker_size=1.0)
    axes[1].set_title("Accepted raw readings (faint) and 10-second robust moving mean (line)")
    format_axis(axes[1], "Distance (cm)")

    line_by_source(axes[2], postprocessed, "distanceCm")
    axes[2].set_title("Coherent post-processed distance — every 1-second point (local MAD ≤ 1.5 cm)")
    format_axis(axes[2], "Distance (cm)")

    line_by_source(axes[3], velocity, "flow_lpm")
    axes[3].axhline(0, color="#4b5563", linewidth=0.8)
    axes[3].set_title("Derived flow — every valid 10-second regression window")
    format_axis(axes[3], "Flow (L/min)")

    line_by_source(axes[4], physical_path, "distance_cm")
    axes[4].set_title("Physics-constrained selected level — every 1-second point")
    format_axis(axes[4], "Distance (cm)")

    line_by_source(axes[5], physical_flow, "flow_lpm")
    axes[5].axhline(0, color="#4b5563", linewidth=0.8)
    axes[5].set_title("Physics-constrained derived flow — every valid 30-second regression window")
    format_axis(axes[5], "Flow (L/min)")
    first_date = raw["timestamp"].min().strftime("%Y-%m-%d")
    last_date = raw["timestamp"].max().strftime("%Y-%m-%d")
    date_range = first_date if first_date == last_date else f"{first_date} to {last_date}"
    axes[5].set_xlabel(f"UTC time, {date_range}")

    output = ROOT / args.output
    fig.savefig(output, dpi=220, facecolor="white")
    print(output)


if __name__ == "__main__":
    main()
