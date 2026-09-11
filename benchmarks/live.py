#!/usr/bin/env python3
"""Measure the external-telemetry, interpretation, cache, and reconciliation paths."""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import json
import platform
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/linux-release"))
    parser.add_argument("--samples", type=int, default=40)
    parser.add_argument("--concurrency", type=int, default=12)
    parser.add_argument("--save", type=Path)
    return parser.parse_args()


def executable(build_dir: Path) -> Path:
    candidate = (build_dir if build_dir.is_absolute() else ROOT / build_dir) / "context-hmi"
    if candidate.exists():
        return candidate
    windows = candidate.with_suffix(".exe")
    if windows.exists():
        return windows
    raise RuntimeError(f"missing service executable: {candidate}")


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def request_json(
    base: str, path: str, payload: dict[str, Any] | None = None
) -> tuple[int, dict[str, Any], float]:
    body = None if payload is None else json.dumps(payload).encode()
    request = urllib.request.Request(
        base + path,
        data=body,
        method="GET" if body is None else "POST",
        headers={} if body is None else {"Content-Type": "application/json"},
    )
    started = time.perf_counter_ns()
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            result = json.loads(response.read())
            status = response.status
    except urllib.error.HTTPError as error:
        result = json.loads(error.read())
        status = error.code
    elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
    if not isinstance(result, dict):
        raise RuntimeError(f"{path} did not return an object")
    return status, result, elapsed_ms


def percentile(samples: list[float], fraction: float) -> float:
    ordered = sorted(samples)
    position = fraction * (len(ordered) - 1)
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (position - lower) * (ordered[upper] - ordered[lower])


def summary(samples: list[float]) -> dict[str, float | int]:
    if not samples:
        raise RuntimeError("measurement has no samples")
    return {
        "count": len(samples),
        "minimum": min(samples),
        "p50": percentile(samples, 0.5),
        "p95": percentile(samples, 0.95),
        "maximum": max(samples),
        "mean": sum(samples) / len(samples),
    }


def wait_ready(base: str, timeout: float = 15.0) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        try:
            status, last, _ = request_json(base, "/api/v1/ready")
            if status == 200 and last.get("ready") is True:
                return last
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError(f"service did not become ready: {last}")


def wait_service(base: str, timeout: float = 15.0) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        try:
            status, last, _ = request_json(base, "/api/v1/health")
            if status == 200:
                return last
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError(f"service did not become healthy: {last}")


def stop(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def telemetry_for(model: dict[str, Any]) -> dict[str, Any]:
    generation = model.get("context_generation")
    if not isinstance(generation, int) or generation < 0:
        raise RuntimeError("model response lacks a non-negative context_generation")
    tags = model.get("tags", [])
    values: dict[str, dict[str, Any]] = {}
    for index, tag in enumerate(tags):
        if not isinstance(tag, dict) or not isinstance(tag.get("id"), str):
            continue
        data_type = tag.get("data_type", "number")
        if data_type in {"bool", "boolean"}:
            value: bool | int | float | str = index % 2 == 0
        elif data_type in {"integer", "int", "int32", "int64"}:
            value = index
        elif data_type in {"string", "text"}:
            value = "nominal"
        else:
            value = float(index + 1)
        values[tag["id"]] = {
            "value": value,
            "quality": "good",
            "timestamp_ms": int(time.time() * 1000),
        }
    return {
        "schema_version": "context-hmi-telemetry/1",
        "model_id": model["model_id"],
        "model_revision": model["revision"],
        "context_generation": generation,
        "source_id": "benchmark-input",
        "sequence": 1,
        "values": values,
    }


def rss_bytes(process_id: int) -> int | None:
    status_path = Path(f"/proc/{process_id}/status")
    if not status_path.exists():
        return None
    for line in status_path.read_text(encoding="utf-8").splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1]) * 1024
    return None


def measure(options: argparse.Namespace) -> dict[str, Any]:
    if options.samples < 5 or options.samples > 500:
        raise RuntimeError("samples must be from 5 through 500")
    if options.concurrency < 1 or options.concurrency > 64:
        raise RuntimeError("concurrency must be from 1 through 64")
    server = executable(options.build_dir)
    service_port = free_port()
    base = f"http://127.0.0.1:{service_port}"
    with tempfile.TemporaryDirectory(prefix="context-hmi-benchmark-") as temporary:
        temporary_path = Path(temporary)
        process = subprocess.Popen(
            [
                str(server),
                "--host",
                "127.0.0.1",
                "--port",
                str(service_port),
                "--model",
                "examples/machines/assembly-line.json",
                "--models",
                "examples/machines",
                "--no-model-watch",
                "--store-file",
                str(temporary_path / "operations.chj"),
                "--log-file",
                str(temporary_path / "diagnostics.jsonl"),
            ],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            wait_service(base)
            model_status, model_response, _ = request_json(base, "/api/v1/model")
            if model_status != 200:
                raise RuntimeError(f"model request failed: {model_response}")
            model = model_response.get("model", model_response)
            if not isinstance(model, dict):
                raise RuntimeError("model response lacks an object")
            telemetry = telemetry_for(model)
            telemetry_status, _, _ = request_json(base, "/api/v1/telemetry", telemetry)
            if telemetry_status not in {200, 202}:
                raise RuntimeError("telemetry ingestion failed")
            ready = wait_ready(base)
            prompt = "Show the current status of the production line"
            cold_status, cold_body, cold_ms = request_json(
                base, "/api/v1/interpret", {"prompt": prompt, "interpreter": "rules"}
            )
            if cold_status != 200 or cold_body.get("status") != "ready":
                raise RuntimeError(f"cold interpretation failed: {cold_body}")
            warm: list[float] = []
            latest_body = cold_body
            for _ in range(options.samples):
                status, body, elapsed = request_json(
                    base, "/api/v1/interpret", {"prompt": prompt, "interpreter": "rules"}
                )
                if status != 200 or body.get("status") != "ready":
                    raise RuntimeError(f"warm interpretation failed: {body}")
                warm.append(elapsed)
                latest_body = body
            view = latest_body.get("view")
            if not isinstance(view, dict) or not isinstance(view.get("view_id"), str):
                raise RuntimeError("interpretation did not return a canonical view")
            view_id = view["view_id"]
            reconcile_status, reconcile_body, reconcile_ms = request_json(
                base, "/api/v1/reconcile", {"view_id": view_id}
            )
            if reconcile_status != 200:
                raise RuntimeError(
                    f"reconciliation failed with HTTP {reconcile_status}: {reconcile_body}"
                )

            def telemetry_request(_: int) -> tuple[int, float]:
                status, _, elapsed = request_json(base, "/api/v1/telemetry")
                return status, elapsed

            started = time.perf_counter_ns()
            with concurrent.futures.ThreadPoolExecutor(
                max_workers=options.concurrency
            ) as executor:
                concurrent_results = list(
                    executor.map(telemetry_request, range(options.concurrency * 4))
                )
            concurrent_wall_ms = (time.perf_counter_ns() - started) / 1_000_000
            metrics_status, metrics, _ = request_json(base, "/api/v1/metrics")
            if metrics_status != 200:
                raise RuntimeError("metrics endpoint failed")
            return {
                "schema_version": "context-hmi-live-benchmark/2",
                "recorded_utc": datetime.now(UTC).isoformat().replace("+00:00", "Z"),
                "evidence_class": "measured-local-run",
                "environment": {
                    "os": platform.platform(),
                    "machine": platform.machine(),
                    "python": platform.python_version(),
                    "pid": process.pid,
                },
                "workload": {
                    "samples": options.samples,
                    "concurrency": options.concurrency,
                    "model_id": model.get("model_id"),
                    "model_revision": model.get("revision"),
                    "telemetry_source": "external JSON ingestion",
                    "provider": "rules",
                },
                "readiness": ready,
                "measurements": {
                    "cold_interpretation_ms": cold_ms,
                    "warm_interpretation_ms": summary(warm),
                    "reconciliation_ms": reconcile_ms,
                    "concurrent_telemetry": {
                        "requests": len(concurrent_results),
                        "clients": options.concurrency,
                        "failures": sum(
                            status not in {200, 202} for status, _ in concurrent_results
                        ),
                        "wall_ms": concurrent_wall_ms,
                        "per_request_ms": summary([elapsed for _, elapsed in concurrent_results]),
                    },
                    "service_resident_bytes": rss_bytes(process.pid),
                },
                "interpretation": {
                    "status": cold_body.get("status"),
                    "cold_execution": cold_body.get("execution"),
                    "warm_execution": latest_body.get("execution"),
                },
                "runtime_metrics": metrics,
                "limitations": [
                    "shared workstation, not controlled target hardware",
                    "rules provider, not a measured model inference run",
                    "authored request, not a held-out interpretation set",
                    "container and cross-platform measurements require their named environments",
                ],
            }
        finally:
            stop(process)


def main() -> int:
    options = arguments()
    serialized = json.dumps(measure(options), indent=2) + "\n"
    if options.save is not None:
        destination = options.save if options.save.is_absolute() else ROOT / options.save
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(serialized, encoding="utf-8")
    print(serialized, end="")
    return 0


if __name__ == "__main__":
    with contextlib.suppress(KeyboardInterrupt):
        raise SystemExit(main())
