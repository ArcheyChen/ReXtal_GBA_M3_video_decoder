#!/usr/bin/env python3
"""Summarize MGBA_M3_TRACE CSV files emitted by the M3 trace build."""

from __future__ import annotations

import argparse
import csv
import statistics
from pathlib import Path

ONE_VBLANK_CYCLES = 280_896
VIDEO_FRAME_CYCLES = ONE_VBLANK_CYCLES * 6


def percentile(values: list[int], pct: float) -> int:
    if not values:
        return 0
    if len(values) == 1:
        return values[0]
    values = sorted(values)
    index = round((len(values) - 1) * pct)
    return values[index]


def summarize(path: Path, top: int) -> None:
    zones: dict[str, list[int]] = {}
    frame_zones: dict[int, dict[str, int]] = {}
    frame_number: dict[int, int] = {}
    frame_bytes: dict[int, int] = {}
    sync_lag: dict[int, int] = {}
    occurrence = -1

    with path.open(newline="") as fp:
        rows = (line for line in fp if not line.startswith("#"))
        for row in csv.DictReader(rows):
            frame = int(row["frame"])
            zone = row["zone_name"]
            if row["type"] == "frame":
                occurrence += 1
                frame_number[occurrence] = frame
            elif occurrence >= 0 and row["type"] == "end":
                duration = int(row["duration_cycles"])
                zones.setdefault(zone, []).append(duration)
                frame_zones.setdefault(occurrence, {})[zone] = duration
            elif occurrence >= 0 and row["type"] == "value" and zone == "frame_bytes":
                frame_bytes[occurrence] = int(row["value"])
            elif occurrence >= 0 and row["type"] == "value" and zone == "sync_lag":
                sync_lag[occurrence] = int(row["value"])

    print(f"\n{path}")
    for zone, values in sorted(zones.items()):
        avg = round(statistics.mean(values))
        print(
            f"{zone:14s} n={len(values):5d} "
            f"avg={avg:9d} p95={percentile(values, 0.95):9d} "
            f"max={max(values):9d}"
        )

    scored: list[tuple[int, int, int, int, int, int, int]] = []
    for occurrence_id, per_zone in frame_zones.items():
        decode = per_zone.get("video_decode", 0)
        copy = per_zone.get("vram_copy", 0)
        total = decode + copy
        if total:
            scored.append((
                total,
                occurrence_id,
                frame_number.get(occurrence_id, -1),
                decode,
                copy,
                frame_bytes.get(occurrence_id, 0),
                sync_lag.get(occurrence_id, 0),
            ))

    if not scored:
        return

    over_1vb = sum(total > ONE_VBLANK_CYCLES for total, *_ in scored)
    over_6vb = sum(total > VIDEO_FRAME_CYCLES for total, *_ in scored)
    print(
        f"video frames={len(scored)} over_1vb={over_1vb} "
        f"over_6vb={over_6vb} budget_6vb={VIDEO_FRAME_CYCLES}"
    )
    print("worst frames:")
    for total, occurrence_id, frame, decode, copy, size, lag in sorted(scored, reverse=True)[:top]:
        print(
            f"  occurrence={occurrence_id:5d} frame={frame:5d} total={total:8d} decode={decode:8d} "
            f"copy={copy:7d} bytes={size:6d} lag={lag}"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", nargs="+", type=Path)
    parser.add_argument("--top", type=int, default=12)
    args = parser.parse_args()
    for path in args.csv:
        summarize(path, args.top)


if __name__ == "__main__":
    main()
