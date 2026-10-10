import csv
import importlib.util
import pathlib
import tempfile
import unittest
from unittest import mock
import io
import types
import os


ROOT = pathlib.Path(__file__).resolve().parents[1]


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


runtime = module("paper_runtime", "paper-runtime.py")
graphs = module("paper_graphs", "generate-paper-graphs.py")


def send(sequence, timestamp):
    return {"event": "send", "sequence": sequence, "time": timestamp}


def reply(sequence, timestamp):
    return {"event": "reply", "sequence": sequence, "time": timestamp, "rtt_ms": 10}


class PacketMetrics(unittest.TestCase):
    def test_checksum_empty(self):
        self.assertEqual(runtime.checksum(b""), 65535)

    def test_checksum_known(self):
        self.assertEqual(runtime.checksum(bytes.fromhex("0001f203f4f5f6f7")), 8717)

    def test_checksum_odd(self):
        self.assertEqual(runtime.checksum(b"abc"), runtime.checksum(b"abc\0"))

    def test_empty(self):
        self.assertIsNone(runtime.ping_metrics([])["packet_loss"])

    def test_all_received(self):
        result = runtime.ping_metrics([send(1, 1), reply(1, 1.01)])
        self.assertEqual(result["packet_loss"], 0)

    def test_half_lost(self):
        self.assertEqual(runtime.ping_metrics([send(1, 1), send(2, 2), reply(1, 1.01)])["packet_loss"], 50)

    def test_all_lost(self):
        result = runtime.ping_metrics([send(1, 1), send(2, 2)], 0, 3)
        self.assertEqual(result["max_reply_gap_ms"], 3000)
        self.assertEqual(result["packet_loss"], 100)

    def test_duplicate_not_extra_packet(self):
        result = runtime.ping_metrics([send(1, 1), reply(1, 1.01), reply(1, 1.02)])
        self.assertEqual(result["received"], 1)

    def test_duplicate_not_reordering(self):
        result = runtime.ping_metrics([send(1, 1), send(2, 2), reply(1, 1.01), reply(2, 2.01), reply(1, 2.02)])
        self.assertEqual(result["packet_reordering"], 0)

    def test_reordering(self):
        result = runtime.ping_metrics([send(1, 1), send(2, 1), reply(2, 2), reply(1, 3)])
        self.assertEqual(result["packet_reordering"], 1)

    def test_unmatched_reply(self):
        self.assertEqual(runtime.ping_metrics([send(1, 1), reply(9, 2)])["received"], 0)

    def test_window_filters_sends(self):
        result = runtime.ping_metrics([send(1, 1), send(2, 2), reply(1, 1.1), reply(2, 2.1)], 2, 3)
        self.assertEqual(result["sent"], 1)

    def test_window_filters_late_reply(self):
        result = runtime.ping_metrics([send(1, 1), reply(1, 4)], 0, 3)
        self.assertEqual(result["received"], 0)

    def test_window_end_exclusive_send(self):
        self.assertEqual(runtime.ping_metrics([send(1, 3)], 0, 3)["sent"], 0)

    def test_trailing_outage(self):
        result = runtime.ping_metrics([send(1, 1), reply(1, 1.1), send(2, 2), send(3, 3)])
        self.assertAlmostEqual(result["max_reply_gap_ms"], 1900)

    def test_reply_gap_not_only_loss(self):
        result = runtime.ping_metrics([send(1, 1), reply(1, 1.01), send(2, 2), reply(2, 2.01)])
        self.assertAlmostEqual(result["max_reply_gap_ms"], 1000)

    def test_loss_burst(self):
        result = runtime.ping_metrics([send(1, 1), send(2, 2), send(3, 3), reply(3, 3.01), send(4, 4)])
        self.assertEqual(result["max_loss_burst"], 2)


class MetricGraphs(unittest.TestCase):
    def summarize(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory)
            graphs.generate_metric_graphs(rows, output)
            filename = output / "metrics-summary.csv"
            if not filename.exists():
                return []
            with filename.open() as stream:
                result = list(csv.DictReader(stream))
            for image in output.glob("*.svg"):
                self.assertNotIn("nan", image.read_text())
            return result

    def test_failed_not_averaged(self):
        rows = self.summarize([{"scenario": "fault", "status": "fail", "commit_ms": "10000"}])
        self.assertEqual(rows, [])

    def test_nan_ignored(self):
        self.assertEqual(self.summarize([{"scenario": "fault", "commit_ms": "nan"}]), [])

    def test_infinity_ignored(self):
        self.assertEqual(self.summarize([{"scenario": "fault", "commit_ms": "inf"}]), [])

    def test_missing_not_zero(self):
        rows = self.summarize([{"scenario": "fault", "commit_ms": ""}, {"scenario": "fault", "commit_ms": "10"}])
        self.assertEqual(rows[0]["samples"], "1")
        self.assertEqual(float(rows[0]["mean"]), 10)

    def test_strategies_separate(self):
        rows = self.summarize([{"scenario": "fault", "strategy": "immediate", "commit_ms": "10"},
                               {"scenario": "fault", "strategy": "graceful", "commit_ms": "20"}])
        self.assertEqual(len(rows), 2)

    def test_sample_sd(self):
        rows = self.summarize([{"scenario": "fault", "commit_ms": "10"}, {"scenario": "fault", "commit_ms": "20"}])
        self.assertAlmostEqual(float(rows[0]["stdev"]), 7.0710678118654755)

    def test_p95_nearest_rank(self):
        rows = self.summarize([{"scenario": "fault", "commit_ms": str(value)} for value in range(1, 21)])
        self.assertEqual(float(rows[0]["p95"]), 19)


class CommandEnvironment(unittest.TestCase):
    def test_outer_output_not_inherited(self):
        executor = runtime.Runtime.__new__(runtime.Runtime)
        executor.logs = io.StringIO()
        with mock.patch.dict(os.environ, {"OUT_DIR": "outer-report"}), mock.patch.object(runtime.subprocess, "run") as command:
            command.return_value = types.SimpleNamespace(returncode=0, stdout="")
            executor.run(["true"])
            self.assertNotIn("OUT_DIR", command.call_args.kwargs["env"])

    def test_explicit_child_output_preserved(self):
        executor = runtime.Runtime.__new__(runtime.Runtime)
        executor.logs = io.StringIO()
        with mock.patch.dict(os.environ, {"OUT_DIR": "outer-report"}), mock.patch.object(runtime.subprocess, "run") as command:
            command.return_value = types.SimpleNamespace(returncode=0, stdout="")
            executor.run(["true"], env={"OUT_DIR": "child-config"})
            self.assertEqual(command.call_args.kwargs["env"]["OUT_DIR"], "child-config")


if __name__ == "__main__":
    unittest.main()
