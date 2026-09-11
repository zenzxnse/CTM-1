"""OpenAI-compatible provider contract tests for bounded, local-first inference."""

from __future__ import annotations

import json
import socket
import subprocess
import threading
import time
import unittest
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from service_harness import BINARY, MODEL, MODELS, free_port


class ProviderFixture:
    def __init__(self, model: dict[str, object]) -> None:
        self.model = model
        self.requests: list[dict[str, object]] = []
        self.paths: list[str] = []
        self._server = ThreadingHTTPServer(("127.0.0.1", 0), self._handler())
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    def _handler(self) -> type[BaseHTTPRequestHandler]:
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                fixture.paths.append(self.path)
                body = json.dumps({"data": [{"id": "local-model"}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self) -> None:
                length = int(self.headers.get("Content-Length", "0"))
                fixture.paths.append(self.path)
                fixture.requests.append(json.loads(self.rfile.read(length)))
                asset = str(fixture.model["assets"][0]["id"])
                result = {
                    "status": "ready",
                    "task": {
                        "kind": "overview",
                        "model_id": fixture.model["model_id"],
                        "model_revision": fixture.model["revision"],
                        "anchor_asset_id": asset,
                    },
                }
                body = json.dumps(
                    {
                        "choices": [{"message": {"content": json.dumps(result)}}],
                        "usage": {
                            "prompt_tokens": 120,
                            "completion_tokens": 18,
                            "total_tokens": 138,
                            "prompt_tokens_details": {"cached_tokens": 80},
                        },
                    }
                ).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, format: str, *args: object) -> None:
                del format, args

        return Handler

    def __enter__(self) -> ProviderFixture:
        self._thread.start()
        return self

    def __exit__(self, *_: object) -> None:
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=2)

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self._server.server_port}"


def request(base: str, path: str, body: object | None = None) -> tuple[int, dict[str, object]]:
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + path, data=data)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=5) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


class ProviderContract(unittest.TestCase):
    def test_static_prefix_and_usage_are_preserved(self) -> None:
        model = json.loads(MODEL.read_text(encoding="utf-8"))
        with ProviderFixture(model) as fixture:
            port = free_port()
            process = subprocess.Popen(
                [
                    str(BINARY),
                    "--port",
                    str(port),
                    "--model",
                    str(MODEL),
                    "--models",
                    str(MODELS),
                    "--provider-url",
                    fixture.url,
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            base = f"http://127.0.0.1:{port}"
            try:
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    try:
                        if request(base, "/api/v1/health")[0] == 200:
                            break
                    except (OSError, urllib.error.URLError):
                        time.sleep(0.02)
                else:
                    self.fail("native service did not start")

                prompt = f"Show an overview of {model['assets'][0]['name']}"
                status, result = request(
                    base,
                    "/api/v1/interpret",
                    {"prompt": prompt, "interpreter": "llama"},
                )
                self.assertEqual(status, 200, result)
                self.assertEqual(result["interpreter"], "llama.cpp")
                usage = result["provider_usage"]
                self.assertEqual(usage["prompt_tokens"], 120)
                self.assertEqual(usage["cached_prompt_tokens"], 80)
                self.assertEqual(fixture.paths[-1], "/v1/chat/completions")
                self.assertEqual(len(fixture.requests), 1)
                messages = fixture.requests[0]["messages"]
                self.assertEqual(messages[0]["role"], "system")
                self.assertEqual(messages[1]["role"], "system")
                self.assertEqual(messages[2]["role"], "user")
                self.assertIn("Interpret the operator request", messages[0]["content"])
                self.assertIn("Declared machine context", messages[1]["content"])
                self.assertEqual(messages[2]["content"], prompt)
                self.assertTrue(fixture.requests[0]["cache_prompt"])
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
