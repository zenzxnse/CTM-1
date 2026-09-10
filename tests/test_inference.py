"""Exercise the compiled C++ inference provider against a local HTTP fixture."""

from __future__ import annotations

import json
import os
import socket
import subprocess
import threading
import time
import unittest
import urllib.error
import urllib.request
from collections.abc import Iterator
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("CONTEXT_HMI_BINARY", ROOT / "build/debug/context-hmi"))
MODEL = Path(os.environ.get("CONTEXT_HMI_MODEL", ROOT / "examples/pump-station.json"))
MODELS = Path(os.environ.get("CONTEXT_HMI_MODELS", ROOT / "examples"))


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def request(base: str, path: str, value: object | None = None) -> tuple[int, dict[str, Any]]:
    data = None if value is None else json.dumps(value).encode()
    req = urllib.request.Request(base + path, data=data)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=5) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


class MockLlama:
    """A bounded local fixture that captures the real native client's request."""

    def __init__(self, body: object, status: int = 200, delay: float = 0) -> None:
        self.body = body
        self.status = status
        self.delay = delay
        self.request_body: dict[str, Any] | None = None
        self.request_path = ""
        self.received = threading.Event()
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self) -> None:
                length = int(self.headers.get("Content-Length", 0))
                fixture.request_body = json.loads(self.rfile.read(length))
                fixture.request_path = self.path
                fixture.received.set()
                if fixture.delay:
                    time.sleep(fixture.delay)
                data = json.dumps(fixture.body).encode()
                try:
                    self.send_response(fixture.status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, format: str, *args: object) -> None:
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(
            target=lambda: self.server.serve_forever(poll_interval=0.02), daemon=True
        )

    def __enter__(self) -> MockLlama:
        self.thread.start()
        return self

    def __exit__(self, *_: object) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


@contextmanager
def native_service(mock: MockLlama, timeout_ms: int = 1500) -> Iterator[str]:
    port = free_port()
    base = f"http://127.0.0.1:{port}"
    process = subprocess.Popen(
        [
            str(BINARY),
            "--port",
            str(port),
            "--model",
            str(MODEL),
            "--models",
            str(MODELS),
            "--llama-url",
            f"http://127.0.0.1:{mock.server.server_port}",
            "--llama-timeout-ms",
            str(timeout_ms),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    try:
        for _ in range(100):
            if process.poll() is not None:
                details = process.stderr.read().decode() if process.stderr else "no stderr"
                raise RuntimeError(f"native service exited: {details}")
            try:
                if request(base, "/api/v1/health")[0] == 200:
                    break
            except (OSError, urllib.error.URLError):
                time.sleep(0.02)
        else:
            raise RuntimeError("native service did not start")
        yield base
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
        if process.stderr:
            process.stderr.close()


def ready(**changes: object) -> dict[str, Any]:
    task: dict[str, Any] = {
        "kind": "overview",
        "anchor_asset_id": "tank-3",
        "model_revision": 1,
        "original_request": "provider transcription",
    }
    task.update(changes)
    return {"status": "ready", "task": task}


def envelope(value: object) -> dict[str, Any]:
    return {"choices": [{"message": {"content": json.dumps(value)}}]}


class LlamaProviderContract(unittest.TestCase):
    def run_native(self, mock: MockLlama) -> tuple[int, dict[str, Any]]:
        with native_service(mock) as base:
            return request(
                base,
                "/api/v1/interpret",
                {"prompt": "Show overview for Tank 3", "interpreter": "llama"},
            )

    def test_ready_response_uses_real_client_and_a_valid_output_schema(self) -> None:
        with MockLlama(envelope(ready())) as mock:
            status, result = self.run_native(mock)
            self.assertEqual(status, 200)
            self.assertEqual(result["interpreter"], "llama.cpp")
            self.assertEqual(result["task"]["original_request"], "Show overview for Tank 3")
            self.assertIn("view", result)
            self.assertEqual(mock.request_path, "/v1/chat/completions")
            self.assertIsNotNone(mock.request_body)
            assert mock.request_body is not None
            schema = mock.request_body["response_format"]["json_schema"]["schema"]
            self.assertIsInstance(schema["properties"]["status"], dict)
            self.assertEqual(schema["properties"]["task"]["properties"]["kind"]["type"], "string")
            context = mock.request_body["messages"][-1]["content"]
            self.assertNotIn("namespace_uri", context)
            self.assertNotIn("identifier", context)

    def test_unknown_ids_and_stale_revisions_are_rejected(self) -> None:
        for task in [ready(anchor_asset_id="invented-tank"), ready(model_revision=99)]:
            with self.subTest(task=task), MockLlama(envelope(task)) as mock:
                status, result = self.run_native(mock)
                self.assertEqual(status, 502)
                self.assertEqual(result["error"]["code"], "provider_schema")
                self.assertNotIn("view", result)

    def test_unexpected_properties_and_malformed_content_are_rejected(self) -> None:
        wrong = ready()
        wrong["unexpected"] = True
        for body in [envelope(wrong), {"choices": [{"message": {"content": "not JSON"}}]}]:
            with self.subTest(body=body), MockLlama(body) as mock:
                status, result = self.run_native(mock)
                self.assertEqual(status, 502)
                self.assertIn(result["error"]["code"], {"provider_schema", "provider_response"})

    def test_clarification_does_not_create_a_view(self) -> None:
        with MockLlama(envelope({"status": "clarification", "message": "Which tank?"})) as mock:
            status, result = self.run_native(mock)
            self.assertEqual(status, 200)
            self.assertEqual(result["status"], "clarification")
            self.assertNotIn("view", result)

    def test_http_errors_are_not_silent_fallbacks(self) -> None:
        with MockLlama({"error": "down"}, status=503) as mock:
            status, result = self.run_native(mock)
            self.assertEqual(status, 502)
            self.assertEqual(result["error"]["code"], "provider_http")

    def test_response_limit_is_enforced_during_receive(self) -> None:
        with MockLlama({"padding": "x" * 100_000}) as mock:
            status, result = self.run_native(mock)
            self.assertEqual(status, 502)
            self.assertEqual(result["error"]["code"], "provider_response_too_large")

    def test_timeout_releases_request_without_fallback(self) -> None:
        with MockLlama(envelope(ready()), delay=0.5) as mock, native_service(mock, 100) as base:
            status, result = request(
                base, "/api/v1/interpret", {"prompt": "Show overview for Tank 3"}
            )
            self.assertEqual(status, 502)
            self.assertEqual(result["error"]["code"], "provider_timeout")
            self.assertEqual(request(base, "/api/v1/telemetry")[0], 200)

    def test_configuration_change_during_inference_rejects_old_plan(self) -> None:
        with (
            MockLlama(envelope(ready()), delay=0.35) as mock,
            native_service(mock) as base,
            ThreadPoolExecutor(max_workers=1) as executor,
        ):
            future = executor.submit(
                request, base, "/api/v1/interpret", {"prompt": "Show overview for Tank 3"}
            )
            self.assertTrue(mock.received.wait(timeout=2))
            self.assertEqual(request(base, "/api/v1/telemetry")[0], 200)
            self.assertEqual(
                request(base, "/api/v1/scenario", {"id": "model-revision-added-feed"})[0], 200
            )
            status, result = future.result(timeout=3)
            self.assertEqual(status, 409, result)
            self.assertEqual(result["error"]["code"], "stale_context")


if __name__ == "__main__":
    unittest.main()
