"""Current standalone API contracts for ingestion, cache observability and context evidence."""

from __future__ import annotations

import time
import unittest
from typing import Any

from service_harness import ServiceTestCase


def telemetry_batch(
    model: dict[str, Any],
    generation: int,
    sequence: int = 1,
    source_id: str = "external-source",
) -> dict[str, Any]:
    values: dict[str, dict[str, Any]] = {}
    timestamp = time.time_ns() // 1_000_000
    for tag in model["tags"]:
        data_type = str(tag["data_type"]).lower()
        if data_type in {"number", "float", "double"}:
            value: Any = 42.5
        elif data_type == "integer":
            value = 42
        elif data_type in {"boolean", "bool"}:
            value = True
        else:
            value = "running"
        values[str(tag["id"])] = {
            "value": value,
            "quality": "good",
            "timestamp_ms": timestamp,
        }
    return {
        "schema_version": "context-hmi-telemetry/1",
        "model_id": model["model_id"],
        "model_revision": model["revision"],
        "context_generation": generation,
        "source_id": source_id,
        "sequence": sequence,
        "values": values,
    }


class CurrentStandaloneApi(ServiceTestCase):
    def model(self) -> dict[str, Any]:
        status, payload = self.get("/api/v1/model")
        self.assertEqual(status, 200)
        self.assertIsInstance(payload, dict)
        return payload

    def test_ingestion_is_external_typed_and_revision_bound(self) -> None:
        model = self.model()
        health = self.health()
        status, before = self.get("/api/v1/telemetry")
        self.assertEqual(status, 200)
        self.assertEqual(before["quality"], "unknown")
        self.assertTrue(
            all(sample["quality"] == "unknown" for sample in before["values"].values())
        )

        batch = telemetry_batch(model, int(health["context_generation"]))
        status, accepted = self.post("/api/v1/telemetry", batch)
        self.assertEqual(status, 202, accepted)
        self.assertTrue(accepted["accepted"])

        status, current = self.get("/api/v1/telemetry")
        self.assertEqual(status, 200)
        self.assertEqual(current["quality"], "good")
        self.assertTrue(
            all(sample["quality"] == "good" for sample in current["values"].values())
        )
        self.assertTrue(all("source_id" in sample for sample in current["values"].values()))

        duplicate_status, duplicate = self.post("/api/v1/telemetry", batch)
        self.assertEqual(duplicate_status, 422, duplicate)
        self.assertEqual(duplicate["error"]["code"], "telemetry_out_of_order")

        foreign = telemetry_batch(model, int(health["context_generation"]) + 1, sequence=2)
        foreign_status, foreign_result = self.post("/api/v1/telemetry", foreign)
        self.assertEqual(foreign_status, 409, foreign_result)
        self.assertEqual(foreign_result["error"]["code"], "stale_context")

    def test_interpretation_cache_and_context_budget_are_visible(self) -> None:
        model = self.model()
        asset_name = str(model["assets"][0]["name"])
        prompt = f"Show an overview of {asset_name}"
        first_status, first = self.post(
            "/api/v1/interpret", {"prompt": prompt, "interpreter": "rules"}
        )
        self.assertEqual(first_status, 200, first)
        self.assertIn("execution", first)
        self.assertEqual(first["execution"]["cache"]["interpretation"]["state"], "miss")
        self.assertIn("context", first["execution"])
        self.assertIsInstance(first["execution"]["context"]["bytes"], int)
        self.assertIsInstance(first["execution"]["context"]["estimated_tokens"], int)
        self.assertIn("retrieval", first["execution"]["cache"])

        second_status, second = self.post(
            "/api/v1/interpret",
            {
                "prompt": f"  SHOW   AN   OVERVIEW OF   {asset_name.upper()}  ",
                "interpreter": "rules",
            },
        )
        self.assertEqual(second_status, 200, second)
        self.assertEqual(second["execution"]["cache"]["interpretation"]["state"], "hit")
        self.assertEqual(second["execution"]["cache"]["retrieval"]["state"], "hit")
        self.assertEqual(
            {key: value for key, value in first["task"].items() if key != "original_request"},
            {key: value for key, value in second["task"].items() if key != "original_request"},
        )
        self.assertEqual(
            second["task"]["original_request"],
            f"  SHOW   AN   OVERVIEW OF   {asset_name.upper()}  ",
        )

        metrics_status, metrics = self.get("/api/v1/metrics")
        self.assertEqual(metrics_status, 200)
        self.assertGreaterEqual(metrics["cache"]["interpretation"]["hits"], 1)
        self.assertGreaterEqual(metrics["cache"]["retrieval"]["hits"], 1)
        self.assertLessEqual(
            metrics["cache"]["interpretation"]["entries"],
            metrics["cache"]["interpretation"]["capacity"],
        )

    def test_model_generation_invalidates_telemetry_and_context_cache(self) -> None:
        model = self.model()
        health = self.health()
        batch = telemetry_batch(
            model,
            int(health["context_generation"]),
            source_id="model-change-test",
        )
        self.assertEqual(self.post("/api/v1/telemetry", batch)[0], 202)
        self.assertEqual(
            self.post(
                "/api/v1/interpret",
                {"prompt": "Show an overview", "interpreter": "rules"},
            )[0],
            200,
        )
        before = self.health()
        scenarios_status, scenarios = self.get("/api/v1/scenarios")
        self.assertEqual(scenarios_status, 200)
        scenario_list = scenarios.get("scenarios", [])
        self.assertTrue(scenario_list, "at least one declared machine model is available")
        alternate = next(
            (item for item in scenario_list if item.get("revision") != model.get("revision")),
            None,
        )
        if alternate is None:
            self.skipTest("the configured model directory has no revision variant")
        changed_status, changed = self.post("/api/v1/scenario", {"id": alternate["id"]})
        self.assertEqual(changed_status, 200, changed)
        self.assertGreater(changed["context_generation"], before["context_generation"])
        telemetry_status, telemetry = self.get("/api/v1/telemetry")
        self.assertEqual(telemetry_status, 200)
        self.assertEqual(telemetry["quality"], "unknown")
        metrics_status, metrics = self.get("/api/v1/metrics")
        self.assertEqual(metrics_status, 200)
        self.assertEqual(metrics["cache"]["interpretation"]["entries"], 0)
        self.assertEqual(metrics["cache"]["retrieval"]["entries"], 0)


if __name__ == "__main__":
    unittest.main()
