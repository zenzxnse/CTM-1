"""Live API contract smoke tests using only Python's standard library.

Set CONTEXT_HMI_BINARY to run these tests against the native service. The model
path defaults to examples/pump-station.json from the project directory.
"""

import json
import os
import socket
import subprocess
import time
import unittest
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.environ.get("CONTEXT_HMI_BINARY", os.path.join(ROOT, "build", "debug", "context-hmi"))
MODEL = os.environ.get("CONTEXT_HMI_MODEL", os.path.join(ROOT, "examples", "pump-station.json"))
MODELS = os.environ.get("CONTEXT_HMI_MODELS", os.path.join(ROOT, "examples"))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class ApiContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.port = free_port()
        cls.base = f"http://127.0.0.1:{cls.port}"
        cls.process = subprocess.Popen(
            [
                BINARY,
                "--host",
                "127.0.0.1",
                "--port",
                str(cls.port),
                "--model",
                MODEL,
                "--models",
                MODELS,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        for _ in range(50):
            try:
                cls.request("/api/v1/health")
                return
            except (OSError, urllib.error.URLError):
                time.sleep(0.05)
        cls.process.kill()
        cls.process.wait(timeout=3)
        raise RuntimeError("native service did not start")

    def setUp(self):
        self.assertEqual(self.request("/api/v1/scenario", "POST", {"id": "pump-station"})[0], 200)

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        cls.process.wait(timeout=3)

    @classmethod
    def request(cls, path, method="GET", value=None, headers=None):
        data = None if value is None else json.dumps(value).encode()
        request = urllib.request.Request(cls.base + path, data=data, method=method)
        if value is not None:
            request.add_header("Content-Type", "application/json")
        for key, item in (headers or {}).items():
            request.add_header(key, item)
        try:
            with urllib.request.urlopen(request, timeout=3) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    def test_health_model_scenarios_and_telemetry(self):
        status, health = self.request("/api/v1/health")
        self.assertEqual(status, 200)
        self.assertEqual(health["mode"], "simulation")
        status, model = self.request("/api/v1/model")
        self.assertEqual(status, 200)
        self.assertIsInstance(model["model_id"], str)
        self.assertIsInstance(model["session_id"], str)
        self.assertIsInstance(model["context_generation"], int)
        status, scenarios = self.request("/api/v1/scenarios")
        self.assertEqual(status, 200)
        self.assertTrue(any(item["id"] == "pump-station" for item in scenarios["scenarios"]))
        status, telemetry = self.request("/api/v1/telemetry")
        self.assertEqual(status, 200)
        self.assertEqual(telemetry["model_id"], model["model_id"])

    def test_mutation_validation_and_origin(self):
        status, body = self.request("/api/v1/simulation", "POST", {"running": "yes"})
        self.assertEqual(status, 400)
        self.assertEqual(set(body), {"error"})
        self.assertEqual(set(body["error"]), {"code", "message"})
        status, body = self.request(
            "/api/v1/simulation", "POST", {"running": True}, {"Origin": "https://evil.invalid"}
        )
        self.assertEqual(status, 403)
        self.assertEqual(body["error"]["code"], "cross_origin")

    def test_interpret_resolve_and_sse(self):
        status, body = self.request("/api/v1/interpret", "POST", {"prompt": "show overview"})
        self.assertEqual(status, 200)
        self.assertEqual(body["status"], "ready")
        self.assertEqual(body["interpreter"], "rules")
        status, resolved = self.request("/api/v1/resolve", "POST", {"task": body["task"]})
        self.assertEqual(status, 200)
        self.assertIn("components", resolved)
        self.assertEqual(
            resolved["session_id"], body.get("view", {}).get("session_id", resolved["session_id"])
        )
        self.assertIsInstance(resolved["context_generation"], int)
        status, reconciled = self.request(
            "/api/v1/reconcile", "POST", {"view_id": resolved["view_id"]}
        )
        self.assertEqual(status, 200)
        self.assertEqual(reconciled["view_id"], resolved["view_id"])
        tampered = dict(resolved)
        tampered["status"] = "needs-review"
        status, error = self.request("/api/v1/reconcile", "POST", {"view": tampered})
        self.assertEqual(status, 422)
        self.assertEqual(error["error"]["code"], "view_tampered")
        tampered = dict(resolved)
        tampered["context_generation"] += 1
        status, error = self.request("/api/v1/reconcile", "POST", {"view": tampered})
        self.assertEqual(status, 422)
        self.assertEqual(error["error"]["code"], "view_tampered")
        with urllib.request.urlopen(self.base + "/api/v1/events", timeout=3) as stream:
            self.assertEqual(stream.readline().decode().strip(), "event: telemetry")
            self.assertTrue(stream.readline().startswith(b"data: "))

    def test_owner_conflict_remains_blocked_and_second_machine_resolves(self):
        status, first = self.request(
            "/api/v1/interpret", "POST", {"prompt": "Show filling for Tank 3"}
        )
        self.assertEqual(status, 200)
        previous = first["view"]
        status, revised = self.request(
            "/api/v1/scenario", "POST", {"id": "model-revision-owner-change"}
        )
        self.assertEqual(status, 200)
        self.assertGreater(revised["context_generation"], previous["context_generation"])
        for _ in range(2):
            status, view = self.request(
                "/api/v1/reconcile", "POST", {"view_id": previous["view_id"]}
            )
            self.assertEqual(status, 200)
            self.assertEqual(view["status"], "conflict")
            self.assertTrue(
                any(
                    item["tag_id"] == "pump-1.flow" and item.get("status") == "conflict"
                    for item in view["components"]
                )
            )
        self.assertEqual(self.request("/api/v1/scenario", "POST", {"id": "mixing-cell"})[0], 200)
        status, second = self.request(
            "/api/v1/interpret", "POST", {"prompt": "Show filling for Buffer A"}
        )
        self.assertEqual(status, 200)
        self.assertEqual(second["view"]["status"], "ready")
        self.assertEqual(second["task"]["anchor_asset_id"], "buffer-a")
        self.assertEqual(second["view"]["model_id"], "mixing-cell")

    def test_invalid_scenario_does_not_change_active_context(self):
        _, before = self.request("/api/v1/model")
        status, body = self.request("/api/v1/scenario", "POST", {"id": "../pump-station"})
        self.assertEqual(status, 400)
        self.assertEqual(body["error"]["code"], "invalid_scenario")
        self.assertEqual(self.request("/api/v1/model")[1], before)


if __name__ == "__main__":
    unittest.main()
