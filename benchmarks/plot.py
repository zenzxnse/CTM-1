#!/usr/bin/env python3
"""Render measured Context HMI benchmark JSON with matplotlib."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live", type=Path, required=True)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def load(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise RuntimeError(f"benchmark is not an object: {path}")
    return value


def main() -> int:
    options = arguments()
    report = load(options.live)
    measurements = report.get("measurements")
    if not isinstance(measurements, dict):
        raise RuntimeError("benchmark has no measurements object")
    warm = measurements.get("warm_interpretation_ms")
    concurrent = measurements.get("concurrent_telemetry")
    per_request = concurrent.get("per_request_ms") if isinstance(concurrent, dict) else None
    if not isinstance(warm, dict) or not isinstance(per_request, dict):
        raise RuntimeError("benchmark lacks warm interpretation or telemetry statistics")
    labels = ["Cold interpretation", "Warm interpretation p50", "Warm interpretation p95"]
    values = [
        float(measurements["cold_interpretation_ms"]),
        float(warm["p50"]),
        float(warm["p95"]),
    ]
    column_count = 3 if options.native is not None else 2
    figure, axes = plt.subplots(
        1, column_count, figsize=(6 * column_count, 4.8), constrained_layout=True
    )
    axes[0].bar(labels, values, color=["#bf8700", "#2f81f7", "#238636"])
    axes[0].set_ylabel("Milliseconds")
    axes[0].set_title("Measured interpretation latency")
    axes[0].tick_params(axis="x", rotation=22)
    axes[1].bar(
        ["Telemetry p50", "Telemetry p95"],
        [float(per_request["p50"]), float(per_request["p95"])],
        color=["#2f81f7", "#238636"],
    )
    axes[1].set_ylabel("Milliseconds")
    axes[1].set_title("Measured telemetry request latency")
    if options.native is not None:
        native = load(options.native)
        native_metrics = native.get("metrics")
        if not isinstance(native_metrics, list):
            raise RuntimeError("native benchmark has no metrics array")
        native_labels: list[str] = []
        native_p50: list[float] = []
        native_p95: list[float] = []
        for metric in native_metrics:
            if not isinstance(metric, dict) or not isinstance(metric.get("times_ms"), dict):
                continue
            timing = metric["times_ms"]
            native_labels.append(str(metric.get("name", "operation")).replace("_", " "))
            native_p50.append(float(timing["p50_ms"]))
            native_p95.append(float(timing["p95_ms"]))
        if not native_labels:
            raise RuntimeError("native benchmark has no timing metrics")
        positions = list(range(len(native_labels)))
        axes[2].bar(
            [position - 0.18 for position in positions],
            native_p50,
            width=0.36,
            label="p50",
            color="#2f81f7",
        )
        axes[2].bar(
            [position + 0.18 for position in positions],
            native_p95,
            width=0.36,
            label="p95",
            color="#238636",
        )
        axes[2].set_xticks(positions, native_labels, rotation=18)
        axes[2].set_ylabel("Milliseconds")
        axes[2].set_title("Measured native engine latency")
        axes[2].legend()
    figure.suptitle("Context HMI measured local workload")
    options.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(options.output, dpi=180)
    plt.close(figure)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
