"""Regression checks for telemetry expiry and bounded Linux stream handling."""
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import time


BIN, YAML = sys.argv[1:]


def record(path="path-direct"):
    return json.dumps(dict(schema="ibuki.telemetry.path_health.v1", path_id=path,
                           source="site-a", sequence=1, rtt_ms=12, packet_loss_percent=0,
                           jitter_ms=0, state="healthy", consecutive_successes=3,
                           consecutive_failures=0, timestamp_ms=int(time.time() * 1000))) + "\n"


def wait_for(predicate, process):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if predicate():
            return
        if process.poll() is not None:
            raise AssertionError(process.communicate())
        time.sleep(0.01)
    raise AssertionError("timed out waiting for eventnetd")


def statuses(path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line.endswith("}")]


def stop(process):
    if process.poll() is None:
        process.kill()
    process.communicate(timeout=5)


with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    status = root / "expiry.jsonl"
    p = subprocess.Popen([BIN, YAML, "--telemetry-stdin", "--count", "2", "--max-age-ms", "200",
                          "--status-jsonl", str(status)], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        p.stdin.write(record())
        p.stdin.flush()
        wait_for(lambda: any(r["selected_path"] == "path-direct" for r in statuses(status)), p)
        # No further input: expiry must independently trigger reconciliation.
        wait_for(lambda: any(r["transition_state"] == "failed" for r in statuses(status)), p)
        p.stdin.write(record("path-via-hub"))
        p.stdin.flush()
        out, err = p.communicate(timeout=5)
        assert p.returncode == 0, (out, err)
        assert statuses(status)[-1]["selected_path"] == "path-via-hub", statuses(status)
    finally:
        stop(p)

    # Malformed and oversized lines from an authorized peer cannot end the daemon.
    endpoint = root / "input.sock"
    p = subprocess.Popen([BIN, YAML, "--telemetry-socket", str(endpoint), "--count", "1",
                          "--socket-timeout-ms", "1000"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        wait_for(endpoint.exists, p)
        with socket.socket(socket.AF_UNIX) as peer:
            peer.connect(str(endpoint))
            peer.sendall(b"invalid\n" + b"x" * 2048 + b"\n" + record().encode())
            peer.shutdown(socket.SHUT_WR)
            out, err = p.communicate(timeout=5)
        assert p.returncode == 0 and b"selected_path: path-direct" in out, (out, err)
    finally:
        stop(p)

    # A peer that leaves a line unfinished is bounded even if it stays connected.
    p = subprocess.Popen([BIN, YAML, "--telemetry-socket", str(endpoint),
                          "--socket-timeout-ms", "200"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        wait_for(endpoint.exists, p)
        with socket.socket(socket.AF_UNIX) as peer:
            peer.connect(str(endpoint))
            peer.sendall(b"{")
            out, err = p.communicate(timeout=5)
        assert p.returncode != 0 and b"timed out" in err, (out, err)
    finally:
        stop(p)

    # Parallel receive timeout must also cover the phase waiting for peers.
    p = subprocess.Popen([BIN, YAML, "--telemetry-socket", str(endpoint), "--socket-parallel",
                          "--socket-accept-count", "2", "--socket-parallel-timeout-ms", "200",
                          "--state-file", str(root / "state.tsv")], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        out, err = p.communicate(timeout=5)
        assert p.returncode != 0 and b"accept timed out" in err, (out, err)
    finally:
        stop(p)

print("telemetry expiry, malformed input and socket deadlines passed")
