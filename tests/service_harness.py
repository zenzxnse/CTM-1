"""Shared process and request helpers for the Python suites.

This module holds no test case. It is named so the build-time test-source guard, which
globs `tests/test_*.py`, does not treat it as a registered test source.

Nothing here assumes a POSIX host. Process control uses subprocess, ports come from the
operating system, and the native binary is resolved with a Windows executable suffix when
one is present.
"""

from __future__ import annotations

import http.client
import json
import os
import socket
import subprocess
import time
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
MODEL = Path(
    os.environ.get("CONTEXT_HMI_MODEL", ROOT / "examples/machines/assembly-line.json")
)
MODELS = Path(os.environ.get("CONTEXT_HMI_MODELS", ROOT / "examples/machines"))


def resolve_binary() -> Path:
    configured = os.environ.get("CONTEXT_HMI_BINARY")
    candidate = Path(configured) if configured else ROOT / "build/debug/context-hmi"
    if candidate.exists():
        return candidate
    with_suffix = candidate.with_suffix(".exe")
    return with_suffix if with_suffix.exists() else candidate


BINARY = resolve_binary()


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def send(
    port: int,
    method: str,
    path: str,
    body: bytes | None = None,
    headers: dict[str, str] | None = None,
    timeout: float = 5.0,
) -> tuple[int, bytes]:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, response.read()
    finally:
        connection.close()


def send_json(
    port: int,
    method: str,
    path: str,
    value: Any = None,
    headers: dict[str, str] | None = None,
    timeout: float = 5.0,
) -> tuple[int, Any]:
    body = None if value is None else json.dumps(value).encode()
    merged = {} if body is None else {"Content-Type": "application/json"}
    merged.update(headers or {})
    status, payload = send(port, method, path, body, merged, timeout)
    try:
        return status, json.loads(payload)
    except (ValueError, UnicodeDecodeError):
        return status, None


def start_service(port: int, extra: list[str] | None = None) -> subprocess.Popen[bytes]:
    process = subprocess.Popen(
        [
            str(BINARY),
            "--host",
            "127.0.0.1",
            "--port",
            str(port),
            "--model",
            str(MODEL),
            "--models",
            str(MODELS),
            *(extra or []),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"native service exited with code {process.returncode}")
        try:
            if send_json(port, "GET", "/api/v1/health")[0] == 200:
                return process
        except OSError:
            pass
        time.sleep(0.02)
    stop_service(process)
    raise RuntimeError("native service did not become ready")


def stop_service(process: subprocess.Popen[bytes]) -> None:
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def command_body(health: dict[str, Any], key: str, command: str, **overrides: Any) -> dict[str, Any]:
    """Build a well-formed command bound to the reported session and generation."""
    body: dict[str, Any] = {
        "idempotency_key": key,
        "command": command,
        "session_id": health["session_id"],
        "context_generation": health["context_generation"],
        "confirmed": True,
    }
    body.update(overrides)
    for field in [name for name, value in overrides.items() if value is None]:
        body.pop(field, None)
    return body


class ServiceTestCase(unittest.TestCase):
    """One isolated native service per test class, on a port chosen by the host."""

    extra_arguments: list[str] = []
    port: int
    process: subprocess.Popen[bytes]

    @classmethod
    def setUpClass(cls) -> None:
        cls.port = free_port()
        cls.process = start_service(cls.port, cls.extra_arguments)

    @classmethod
    def tearDownClass(cls) -> None:
        stop_service(cls.process)

    def get(self, path: str) -> tuple[int, Any]:
        return send_json(self.port, "GET", path)

    def post(self, path: str, value: Any) -> tuple[int, Any]:
        return send_json(self.port, "POST", path, value)

    def health(self) -> dict[str, Any]:
        status, payload = self.get("/api/v1/health")
        self.assertEqual(status, 200, "health must answer for the harness to be meaningful")
        return dict(payload)

    def assert_error_envelope(self, payload: Any, message: str = "") -> str:
        context = f" ({message})" if message else ""
        self.assertIsInstance(payload, dict, f"error body must be a JSON object{context}")
        self.assertEqual(set(payload), {"error"}, f"error body has exactly one key{context}")
        error = payload["error"]
        self.assertIsInstance(error, dict, f"error member must be an object{context}")
        self.assertEqual(set(error), {"code", "message"}, f"envelope keys are fixed{context}")
        self.assertIsInstance(error["code"], str)
        self.assertIsInstance(error["message"], str)
        self.assertTrue(error["code"], f"error code is not empty{context}")
        self.assertTrue(error["message"], f"error message is not empty{context}")
        return str(error["code"])
