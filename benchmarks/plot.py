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
    figure, axes = plt.subplots(1, 2, figsize=(12, 4.8), constrained_layout=True)
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
    figure.suptitle("Context HMI measured local workload")
    options.output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(options.output, dpi=180)
    plt.close(figure)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
