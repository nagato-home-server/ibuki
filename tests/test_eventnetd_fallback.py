import json
import pathlib
import re
import subprocess
import sys
import tempfile


def main():
    eventnetd, yaml, telemetry = sys.argv[1:]
    with tempfile.TemporaryDirectory() as temporary:
        status_path = pathlib.Path(temporary) / "status.jsonl"
        result = subprocess.run(
            [eventnetd, yaml, "--intent", "intent-a-b", "--telemetry", telemetry,
             "--batch-size", "3", "--count", "2", "--max-age-ms", "0",
             "--status-jsonl", str(status_path)],
            check=False, capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise AssertionError(f"eventnetd failed: {result.stderr}\n{result.stdout}")
        paths = re.findall(r"^selected_path: (.+)$", result.stdout, re.MULTILINE)
        if paths != ["path-direct", "path-via-hub"]:
            raise AssertionError(f"expected direct then configured hub fallback, got {paths}\n{result.stdout}")
        with status_path.open(encoding="utf-8") as stream:
            records = [json.loads(line) for line in stream if line.strip()]
        if len(records) != 2:
            raise AssertionError(f"expected two status records, got {len(records)}")
        if records[0]["traffic_key"] != "site-a->site-b":
            raise AssertionError("status JSONL did not retain the traffic key")
        if records[0]["metrics_ns"]["health_probe_count"] != 2:
            raise AssertionError("initial priority should probe only its first usable candidate plus validation")
        fallback = records[1]
        if fallback["transition_state"] != "completed" or "configured fallback" not in fallback["reason"]:
            raise AssertionError(f"fallback outcome not retained: {fallback}")
        if fallback["metrics_ns"]["health_probe_count"] != 4:
            raise AssertionError("fallback should observe all candidates and revalidate the target")

        state_path = pathlib.Path(temporary) / "active-state.tsv"
        failed_status_path = pathlib.Path(temporary) / "failed-status.jsonl"
        state_path.write_text("site-a->site-b\tpath-direct\n", encoding="utf-8")
        unsupported = subprocess.run(
            [eventnetd, yaml, "--intent", "intent-a-b", "--telemetry", telemetry,
             "--batch-size", "3", "--count", "2", "--max-age-ms", "0", "--backend", "command",
             "--state-file", str(state_path), "--status-jsonl", str(failed_status_path)],
            check=False, capture_output=True, text=True,
        )
        if unsupported.returncode == 0:
            raise AssertionError(
                "command adapter unexpectedly accepted unsupported Graceful drain\n"
                f"stdout:\n{unsupported.stdout}\nstderr:\n{unsupported.stderr}"
            )
        with failed_status_path.open(encoding="utf-8") as stream:
            failed_records = [json.loads(line) for line in stream if line.strip()]
        if len(failed_records) != 2:
            raise AssertionError("failed transition result was not emitted to status JSONL")
        failed = failed_records[-1]
        if failed["selected_path"] != "path-via-hub" or failed["transition_state"] != "failed":
            raise AssertionError(f"unexpected fail-closed transition status: {failed}")
        if failed["metrics_ns"]["decision"] <= 0:
            raise AssertionError("failed transition decision time was not retained")
    print("eventnetd direct-to-configured-fallback replay passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
