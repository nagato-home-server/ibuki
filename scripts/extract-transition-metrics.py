#!/usr/bin/env python3
"""Convert eventnetd status JSONL phase metrics into a paper-ready CSV."""

import argparse
import csv
import json
import pathlib
import sys


METRIC_FIELDS = (
    "decision",
    "prepare",
    "validate",
    "commit",
    "post_validation",
    "rollback",
    "health_probe_count",
    "health_probe",
)
CSV_FIELDS = (
    "scenario",
    "repetition",
    "timestamp_ms",
    "intent_id",
    "traffic_key",
    "selected_path",
    "transition_state",
    "decision_ns",
    "prepare_ns",
    "validate_ns",
    "commit_ns",
    "post_validation_ns",
    "rollback_ns",
    "health_probe_count",
    "health_probe_ns",
)


def extract(input_path, output_path, scenario):
    rows = []
    with pathlib.Path(input_path).open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
                if record.get("schema") != "ibuki.status.v1":
                    raise ValueError("schema must be ibuki.status.v1")
                metrics = record["metrics_ns"]
                values = {field: metrics[field] for field in METRIC_FIELDS}
                for field, value in values.items():
                    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                        raise ValueError(f"metrics_ns.{field} must be a non-negative integer")
                for field in ("intent_id", "traffic_key", "selected_path", "transition_state"):
                    if not isinstance(record.get(field), str):
                        raise ValueError(f"{field} must be a string")
                timestamp = record.get("timestamp_ms", "")
                if timestamp != "" and (isinstance(timestamp, bool) or not isinstance(timestamp, int)):
                    raise ValueError("timestamp_ms must be an integer")
            except (json.JSONDecodeError, KeyError, TypeError, ValueError) as error:
                raise ValueError(f"{input_path}:{line_number}: invalid status record: {error}") from error

            rows.append({
                "scenario": scenario,
                "repetition": len(rows) + 1,
                "timestamp_ms": timestamp,
                "intent_id": record["intent_id"],
                "traffic_key": record["traffic_key"],
                "selected_path": record["selected_path"],
                "transition_state": record["transition_state"],
                **{f"{field}_ns": values[field] for field in METRIC_FIELDS if field != "health_probe_count"},
                "health_probe_count": values["health_probe_count"],
            })

    if not rows:
        raise ValueError(f"{input_path}: no status records found")
    with pathlib.Path(output_path).open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    return len(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="eventnetd status JSONL containing metrics_ns")
    parser.add_argument("--output", required=True, help="CSV path to create or replace")
    parser.add_argument("--scenario", required=True, help="scenario label for aggregation")
    args = parser.parse_args()
    try:
        count = extract(args.input, args.output, args.scenario)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    print(f"extracted {count} records: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
