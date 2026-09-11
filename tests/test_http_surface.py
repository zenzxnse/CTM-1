"""HTTP surface contracts that remain in the standalone ingestion service."""

from __future__ import annotations

import json
import unittest

from service_harness import ServiceTestCase, send


class HttpSurface(ServiceTestCase):
    def test_health_ready_and_session_separate_service_source_and_provider(self) -> None:
        health_status, health = self.get("/api/v1/health")
        self.assertEqual(health_status, 200)
        self.assertEqual(health["mode"], "telemetry-ingestion")
        self.assertEqual(health["command_gateway"], "disabled")
        self.assertNotIn("simulation_running", health)

        ready_status, ready = self.get("/api/v1/ready")
        self.assertEqual(ready_status, 503)
        self.assertFalse(ready["ready"])
        self.assertTrue(ready["service"]["ready"])
        self.assertEqual(ready["source"]["kind"], "telemetry-ingestion")
        self.assertIn("inference", ready)

        session_status, session = self.get("/api/v1/session")
        self.assertEqual(session_status, 200)
        self.assertEqual(session["session_id"], health["session_id"])
        self.assertEqual(session["context_generation"], health["context_generation"])
        self.assertNotIn("simulation.command", session["capabilities"])
        self.assertNotIn("command", " ".join(session["capabilities"]))

    def test_unknown_and_unversioned_routes_have_deterministic_envelopes(self) -> None:
        for path in ("/api/v1/does-not-exist", "/api/health"):
            with self.subTest(path=path):
                status, body = self.get(path)
                self.assertEqual(status, 404)
                self.assertEqual(set(body), {"error"})
                self.assertEqual(set(body["error"]), {"code", "message"})
                self.assertTrue(body["error"]["code"])
                self.assertTrue(body["error"]["message"])

    def test_mutations_require_json_and_same_origin(self) -> None:
        status, body = send(self.port, "POST", "/api/v1/interpret", b"{}", {})
        self.assertEqual(status, 415)
        self.assertEqual(json.loads(body)["error"]["code"], "content_type")

        status, body = send(
            self.port,
            "POST",
            "/api/v1/interpret",
            json.dumps({"prompt": "show overview"}).encode(),
            {"Content-Type": "application/json", "Origin": "https://foreign.invalid"},
        )
        self.assertEqual(status, 403)
        self.assertEqual(json.loads(body)["error"]["code"], "cross_origin")

    def test_sse_close_response_is_a_current_telemetry_snapshot(self) -> None:
        status, body = send(
            self.port,
            "GET",
            "/api/v1/events",
            headers={"Connection": "close"},
        )
        self.assertEqual(status, 200)
        text = body.decode("utf-8")
        self.assertTrue(text.startswith("event: telemetry\ndata: "))
        payload = json.loads(text.split("\n", 1)[1][len("data: ") :].strip())
        self.assertEqual(payload["schema_version"], "context-hmi-telemetry/1")
        self.assertEqual(payload["context_generation"], self.health()["context_generation"])

    def test_metrics_expose_bounded_execution_cache_and_source_state(self) -> None:
        status, metrics = self.get("/api/v1/metrics")
        self.assertEqual(status, 200)
        self.assertIn("execution", metrics)
        self.assertIn("provider_execution", metrics)
        self.assertIn("cache", metrics)
        self.assertIn("retrieval", metrics["cache"])
        self.assertIn("interpretation", metrics["cache"])
        self.assertIn("source", metrics)
        self.assertLessEqual(
            metrics["cache"]["retrieval"]["entries"],
            metrics["cache"]["retrieval"]["capacity"],
        )


if __name__ == "__main__":
    unittest.main()
