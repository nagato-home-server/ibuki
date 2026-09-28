import csv
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "extract-transition-metrics.py"
METRICS = {
    "decision": 12,
    "prepare": 34,
    "validate": 56,
    "commit": 78,
    "post_validation": 90,
    "rollback": 0,
    "health_probe_count": 3,
    "health_probe": 11,
}


class ExtractTransitionMetricsTest(unittest.TestCase):
    def test_extracts_status_records_in_order(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / "status.jsonl"
            output = pathlib.Path(temporary) / "metrics.csv"
            first = {"schema": "ibuki.status.v1", "timestamp_ms": 1000, "intent_id": "intent-a", "traffic_key": "site-a->site-b", "selected_path": "path-a",
                     "transition_state": "completed", "metrics_ns": METRICS}
            second = {**first, "timestamp_ms": 2000, "intent_id": "intent-b"}
            source.write_text(json.dumps(first) + "\n" + json.dumps(second) + "\n", encoding="utf-8")

            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--input", str(source), "--output", str(output), "--scenario", "direct"],
                check=False, capture_output=True, text=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with output.open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual([row["repetition"] for row in rows], ["1", "2"])
            self.assertEqual(rows[0]["scenario"], "direct")
            self.assertEqual(rows[0]["traffic_key"], "site-a->site-b")
            self.assertEqual(rows[0]["decision_ns"], "12")
            self.assertEqual(rows[0]["health_probe_count"], "3")

    def test_rejects_missing_or_invalid_metric_field(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / "status.jsonl"
            output = pathlib.Path(temporary) / "metrics.csv"
            invalid = dict(METRICS, commit=-1)
            record = {"schema": "ibuki.status.v1", "intent_id": "i", "traffic_key": "t", "selected_path": "p", "transition_state": "failed", "metrics_ns": invalid}
            source.write_text(json.dumps(record) + "\n", encoding="utf-8")

            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--input", str(source), "--output", str(output), "--scenario", "test"],
                check=False, capture_output=True, text=True,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("metrics_ns.commit", result.stderr)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
