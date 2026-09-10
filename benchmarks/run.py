#!/usr/bin/env python3
"""Run the native context-hmi smoke benchmark and preserve its raw JSON."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "build" / "debug" / "context-hmi-benchmark"


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--batch-size", type=int, default=1)
    parser.add_argument("--base-model", type=Path, default=Path("examples/pump-station.json"))
    parser.add_argument(
        "--revision-model",
        type=Path,
        default=Path("examples/model-revision-added-feed.json"),
    )
    parser.add_argument(
        "--save",
        type=Path,
        help="write the native stdout JSON byte-for-byte to this path",
    )
    parser.add_argument(
        "--human",
        action="store_true",
        help="print a RESULTS summary after the native run",
    )
    return parser.parse_args()


def resolved(path: Path) -> Path:
    return path if path.is_absolute() else ROOT / path


def run(
    binary: Path,
    trials: int,
    iterations: int,
    batch_size: int,
    base_model: Path,
    revision_model: Path,
) -> tuple[str, dict[str, object]]:
    command: list[str] = [
        str(resolved(binary)),
        "--trials",
        str(trials),
        "--iterations",
        str(iterations),
        "--batch-size",
        str(batch_size),
        "--base-model",
        str(base_model),
        "--revision-model",
        str(revision_model),
    ]
    completed = subprocess.run(
        command,
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        sys.stderr.write(completed.stderr)
        raise SystemExit(completed.returncode)
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise SystemExit(f"native benchmark did not emit JSON: {error}") from error
    if not isinstance(result, dict) or not isinstance(result.get("date_utc"), str):
        raise SystemExit("native benchmark JSON lacks a dated date_utc field")
    return completed.stdout, result


def print_human(result: dict[str, object]) -> None:
    print("RESULTS")
    print(f"date_utc: {result['date_utc']}")
    metrics = result.get("metrics", [])
    if isinstance(metrics, list):
        for metric in metrics:
            if not isinstance(metric, dict):
                continue
            times = metric.get("times_ms", {})
            if not isinstance(times, dict):
                continue
            print(
                f"{metric.get('name', 'metric')}: "
                f"p50={times.get('p50_ms')} ms p95={times.get('p95_ms')} ms"
            )


def main() -> int:
    options = arguments()
    raw_json, result = run(
        options.binary,
        options.trials,
        options.iterations,
        options.batch_size,
        options.base_model,
        options.revision_model,
    )
    if options.save is not None:
        destination = options.save if options.save.is_absolute() else ROOT / options.save
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(raw_json, encoding="utf-8")
    sys.stdout.write(raw_json)
    if options.human:
        print_human(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
