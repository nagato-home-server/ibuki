#!/usr/bin/env python3
import argparse
import csv
import hashlib
import json
import math
import os
import pathlib
import select
import socket
import statistics
import struct
import subprocess
import sys
import time
import threading


ROOT = pathlib.Path(__file__).resolve().parents[1]


def checksum(payload):
    if len(payload) % 2:
        payload += b"\0"
    total = sum(struct.unpack("!%dH" % (len(payload) // 2), payload))
    total = (total >> 16) + (total & 65535)
    total += total >> 16
    return (~total) & 65535


def probe(args):
    identifier = os.getpid() & 65535
    channel = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)
    channel.setblocking(False)
    start = time.monotonic()
    sequence = 0
    next_send = start
    sent = {}
    with pathlib.Path(args.output).open("w") as output:
        while time.monotonic() < start + args.duration + 1:
            now = time.monotonic()
            if now >= next_send and now < start + args.duration:
                sequence += 1
                payload = struct.pack("!d", now) + b"ibuki-paper" * 4
                header = struct.pack("!BBHHH", 8, 0, 0, identifier, sequence)
                header = struct.pack("!BBHHH", 8, 0, checksum(header + payload), identifier, sequence)
                channel.sendto(header + payload, (args.target, 0))
                sent[sequence] = now
                output.write(json.dumps({"event": "send", "sequence": sequence, "time": now}) + "\n")
                output.flush()
                next_send = now + args.interval
            readable, _, _ = select.select([channel], [], [], args.interval / 2)
            if readable:
                packet, _ = channel.recvfrom(65535)
                offset = (packet[0] & 15) * 4
                if len(packet) < offset + 8:
                    continue
                kind, _, _, reply_id, reply_seq = struct.unpack("!BBHHH", packet[offset:offset + 8])
                if kind == 0 and reply_id == identifier and reply_seq in sent:
                    received = time.monotonic()
                    output.write(json.dumps({"event": "reply", "sequence": reply_seq, "time": received,
                                             "rtt_ms": (received - sent[reply_seq]) * 1000}) + "\n")
                    output.flush()
    channel.close()


def ping_metrics(events, start=None, end=None):
    sends = {event["sequence"]: event["time"] for event in events if event["event"] == "send"
             and (start is None or event["time"] >= start) and (end is None or event["time"] < end)}
    replies = [event for event in events if event["event"] == "reply" and event["sequence"] in sends
               and (start is None or event["time"] >= start) and (end is None or event["time"] <= end)]
    unique = {event["sequence"]: event for event in replies}
    times = sorted(event["time"] for event in unique.values())
    reordered = 0
    highest = -1
    seen = set()
    for event in replies:
        if event["sequence"] in seen:
            continue
        seen.add(event["sequence"])
        if event["sequence"] < highest:
            reordered += 1
        highest = max(highest, event["sequence"])
    rtts = [event["rtt_ms"] for event in unique.values()]
    burst = 0
    maximum_burst = 0
    for sequence in sorted(sends):
        burst = 0 if sequence in unique else burst + 1
        maximum_burst = max(maximum_burst, burst)
    gaps = [right - left for left, right in zip(times, times[1:])]
    window_start = start if start is not None else min(sends.values(), default=0)
    window_end = end if end is not None else max(sends.values(), default=window_start)
    gaps += [max(0, times[0] - window_start), max(0, window_end - times[-1])] if times else [window_end - window_start]
    return {"sent": len(sends), "received": len(unique),
            "packet_loss": 100 * (len(sends) - len(unique)) / len(sends) if sends else None,
            "max_reply_gap_ms": max(gaps, default=0) * 1000,
            "max_loss_burst": maximum_burst,
            "rtt_ms": statistics.fmean(rtts) if rtts else None, "packet_reordering": reordered}


class Resources:
    def __init__(self, runtime, filename):
        self.filename = pathlib.Path(filename)
        self.pids = []
        for node in ("site-a", "site-b", "hub-1"):
            result = runtime.run(["ip", "netns", "pids", node])
            for pid in result.stdout.split():
                try:
                    if pathlib.Path(f"/proc/{pid}/comm").read_text().strip() in ("vpp", "vpp_main", "charon"):
                        self.pids.append(pid)
                except FileNotFoundError:
                    pass
        self.samples = []
        self.stop = threading.Event()
        self.worker = threading.Thread(target=self.collect, daemon=True)
        self.worker.start()

    def collect(self):
        ticks = os.sysconf("SC_CLK_TCK")
        page_mb = os.sysconf("SC_PAGE_SIZE") / 1048576
        previous = None
        with self.filename.open("w") as stream:
            while not self.stop.is_set():
                now = time.monotonic()
                cpu = 0
                rss = 0
                for pid in self.pids:
                    try:
                        fields = pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
                        cpu += int(fields[11]) + int(fields[12])
                        rss += int(fields[21])
                    except FileNotFoundError:
                        pass
                host = list(map(int, pathlib.Path("/proc/stat").read_text().splitlines()[0].split()[1:9]))
                total, idle = sum(host), host[3] + host[4]
                if previous:
                    elapsed = now - previous[0]
                    host_delta = total - previous[2]
                    sample = {"time": now, "duration": elapsed,
                              "cpu_percent": 100 * (cpu - previous[1]) / ticks / elapsed,
                              "memory_mb": rss * page_mb,
                              "host_cpu_percent": 100 * (1 - (idle - previous[3]) / host_delta) if host_delta else 0}
                    self.samples.append(sample)
                    stream.write(json.dumps(sample) + "\n")
                    stream.flush()
                previous = now, cpu, total, idle
                self.stop.wait(0.25)

    def finish(self):
        self.stop.set()
        self.worker.join()
        duration = sum(sample["duration"] for sample in self.samples)
        fields = {}
        for name in ("cpu_percent", "memory_mb", "host_cpu_percent"):
            fields[name] = sum(sample[name] * sample["duration"] for sample in self.samples) / duration if duration else None
            fields[name + "_max"] = max((sample[name] for sample in self.samples), default=None)
        return fields


class Runtime:
    def __init__(self, args):
        self.args = args
        self.output = pathlib.Path(args.output).resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.build = pathlib.Path(args.build_dir).resolve()
        self.processes = []
        self.events = []
        self.samplers = []
        self.restore_system_vpp = False
        self.logs = (self.output / "commands.log").open("w")

    def run(self, command, check=True, timeout=180, env=None):
        environment = dict(os.environ)
        environment.pop("OUT_DIR", None)
        environment.update(env or {})
        result = subprocess.run([str(part) for part in command], cwd=ROOT, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=timeout, env=environment)
        self.logs.write("$ " + " ".join(str(part) for part in command) + "\n" + result.stdout + "\n")
        self.logs.flush()
        if check and result.returncode:
            raise RuntimeError(f"command failed ({result.returncode}): {command[0:4]}")
        return result

    def ip(self, node, *command, **kwargs):
        return self.run(["ip", "netns", "exec", node, *command], **kwargs)

    def event(self, name, **fields):
        event = {"event": name, "time": time.monotonic(), **fields}
        self.events.append(event)
        with (self.output / "events.jsonl").open("a") as stream:
            stream.write(json.dumps(event) + "\n")
        return event["time"]

    def setup(self):
        self.restore_system_vpp = self.run(["systemctl", "is-active", "--quiet", "vpp"], check=False).returncode == 0
        if self.restore_system_vpp:
            self.run(["systemctl", "stop", "vpp"])
        self.run(["sh", "scripts/vm-vpp-ns-topology.sh", "clean"], check=False)
        self.run(["sh", "scripts/vm-netns-ipsec.sh", "direct", "stop"], check=False)
        self.run(["sh", "scripts/vm-netns-ipsec.sh", "hub", "stop"], check=False)
        self.run(["sh", "scripts/vm-netns.sh", "clean"])
        self.run(["sh", "scripts/vm-netns.sh", "setup"])
        self.run(["sh", "scripts/vm-netns-ipsec.sh", "hub", "start"])
        self.run(["sh", "scripts/vm-vpp-ns-topology.sh", "setup"], env={
            "SKIP_GRE": "1", "VPP_TOPOLOGY_MODE": "ipsec", "VPP_NS_NODES": "site-a site-b"})
        self.run(["sh", "scripts/vm-generate-netns-runtime.sh", self.args.yaml, "--path", "path-via-hub"],
                 env={"BUILD_DIR": str(self.build), "OUT_DIR": str(self.output / "hub-plan")})
        self.run(["sh", self.output / "hub-plan/vpp-netns-route-plan.sh"], env={"DRY_RUN": "0"})
        direct = pathlib.Path("/etc/swanctl/ibuki-paper-live")
        direct.mkdir(mode=0o755, exist_ok=True)
        self.run(["sh", "scripts/vm-netns-ipsec.sh", "direct", "generate"], env={"OUT_DIR": str(direct)})
        for node, interface in (("site-a", "a-direct"), ("site-b", "b-direct")):
            config = direct / node / "swanctl.conf"
            text = config.read_text().replace("mode = tunnel", "mode = tunnel\n        if_id_in = 103\n        if_id_out = 103")
            hub_config = pathlib.Path("out/netns-ipsec-hub") / node / "swanctl.conf"
            config.write_text(hub_config.read_text() + "\n" + text)
            config.chmod(0o600)
            uri = f"unix:///run/eventnet-netns-ipsec-hub/{node}/charon.vici"
            self.run(["swanctl", "--load-conns", "--uri", uri, "--file", config])
            self.run(["swanctl", "--load-creds", "--uri", uri, "--file", config])
            self.ip(node, "ip", "link", "add", "xfrm-direct", "type", "xfrm", "dev", interface, "if_id", "103")
            self.ip(node, "ip", "link", "set", "xfrm-direct", "up")
        self.run(["swanctl", "--initiate", "--uri", "unix:///run/eventnet-netns-ipsec-hub/site-a/charon.vici", "--child", "tun-a-b"])
        self.routes("direct")
        for source, destination in (("client-a", "10.10.2.2"), ("client-b", "10.10.1.2")):
            self.ip(source, "ping", "-c", "2", "-W", "3", destination, check=False)
            self.ip(source, "ping", "-c", "3", "-W", "3", destination)

    def routes(self, path, fail_second=False):
        self.ip("site-a", "ip", "route", "replace", "10.10.2.0/24", "dev", "xfrm-direct" if path == "direct" else "xfrm-a-hub")
        self.ip("site-b", "ip", "route", "replace", "10.10.1.0/24", "dev",
                "ibuki-missing" if fail_second else ("xfrm-direct" if path == "direct" else "xfrm-hub-b"))

    def observe(self, healthy):
        consecutive = 0
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            observed = self.ip("site-a", self.build / "eventnet_agent", "--path", "path-direct", "--target", "203.0.113.9", "--count", "1")
            records = [json.loads(line) for line in observed.stdout.splitlines() if line.startswith("{")]
            if not records:
                raise RuntimeError("Agent emitted no telemetry")
            with (self.output / "agent.jsonl").open("a") as stream:
                for record in records:
                    stream.write(json.dumps(record) + "\n")
            success = records[-1]["state"] == "healthy"
            consecutive = consecutive + 1 if success == healthy else 0
            if consecutive >= self.args.threshold:
                return self.event("agent_detected", healthy=healthy)
            time.sleep(self.args.probe_interval)
        raise RuntimeError("Agent detection deadline exceeded")

    def choose(self, target, strategy, active, failed=False):
        command = [self.build / "eventnet_scenario", self.args.yaml, "--active-path", active,
                   "--expect", target]
        if failed:
            command += ["--fail-path", "path-direct"]
        start = self.event("decision_begin", target=target, strategy=strategy)
        self.run(command)
        return start, self.event("decision_end", target=target)

    def start_process(self, command, output):
        stream = pathlib.Path(output).open("w")
        process = subprocess.Popen([str(part) for part in command], cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        self.processes.append((process, stream))
        return process

    def trial(self, strategy, repetition):
        self.routes("direct")
        self.ip("site-a", "ip", "link", "set", "a-direct", "up")
        trial_dir = self.output / f"{strategy}-{repetition}"
        trial_dir.mkdir()
        sampler = Resources(self, trial_dir / "resources.jsonl")
        self.samplers.append(sampler)
        duration = self.args.duration
        probes = []
        for node, target in (("client-a", "10.10.2.2"), ("client-b", "10.10.1.2")):
            filename = trial_dir / f"{node}.jsonl"
            probes.append((self.start_process(["ip", "netns", "exec", node, "python3", __file__, "probe",
                                              "--target", target, "--output", filename, "--duration", duration,
                                              "--interval", self.args.ping_interval], filename.with_suffix(".log")), filename))
        server = self.start_process(["ip", "netns", "exec", "client-b", "iperf3", "-s", "-1", "-J"], trial_dir / "tcp-server.json")
        time.sleep(0.5)
        tcp = self.start_process(["ip", "netns", "exec", "client-a", "iperf3", "-c", "10.10.2.2", "-t", duration - 2,
                                  "-b", self.args.tcp_rate, "-i", "1", "-J"], trial_dir / "tcp.json")
        time.sleep(4)
        trial_start = self.event("fault_begin", strategy=strategy, repetition=repetition)
        self.ip("site-a", "ip", "link", "set", "a-direct", "down")
        injected = self.event("link_down")
        detected = self.observe(False)
        decision_begin, decision_end = self.choose("path-via-hub", strategy, "path-direct", failed=True)
        for node, child in (("site-a", "tun-a-hub"), ("hub-1", "tun-hub-b")):
            result = self.run(["swanctl", "--list-sas", "--child", child, "--uri", f"unix:///run/eventnet-netns-ipsec-hub/{node}/charon.vici"])
            if "INSTALLED" not in result.stdout:
                raise RuntimeError("standby CHILD SA is not installed")
        prepare_end = self.event("prepare_end")
        if strategy == "graceful":
            time.sleep(self.args.drain_ms / 1000)
        commit_begin = self.event("commit_begin")
        self.routes("hub")
        commit_end = self.event("commit_end")
        self.ip("client-a", "ping", "-c", "1", "-W", "3", "10.10.2.2")
        validated = self.event("post_validation_end")
        time.sleep(4)
        self.ip("site-a", "ip", "link", "set", "a-direct", "up")
        restored = self.event("link_up")
        recovered = self.observe(True)
        self.choose("path-direct", strategy, "path-via-hub")
        if strategy == "graceful":
            time.sleep(self.args.drain_ms / 1000)
        recovery_commit = self.event("recovery_commit_begin")
        self.routes("direct")
        recovery_end = self.event("recovery_commit_end")
        time.sleep(3)
        rollback_begin = self.event("rollback_injection_begin")
        try:
            self.routes("hub", fail_second=True)
        except RuntimeError:
            rollback_failure = self.event("partial_commit_failed")
            self.routes("direct")
            rollback_end = self.event("rollback_end")
        else:
            raise RuntimeError("rollback failure injection unexpectedly succeeded")
        for process, _ in probes:
            if process.wait(timeout=duration + 5):
                raise RuntimeError("ICMP collector failed")
        tcp.wait(timeout=10)
        server.wait(timeout=10)
        for _, stream in self.processes:
            stream.flush()
        tcp_data = json.loads((trial_dir / "tcp.json").read_text())
        if tcp.returncode or "error" in tcp_data:
            raise RuntimeError("TCP session failed")
        for node, target in (("site-a", "tun-a-b"), ("hub-1", "tun-hub-b")):
            self.run(["swanctl", "--list-sas", "--child", target, "--uri", f"unix:///run/eventnet-netns-ipsec-hub/{node}/charon.vici"])
        probe_events = [json.loads(line) for line in probes[0][1].read_text().splitlines()]
        fields = ping_metrics(probe_events)
        fields["outage_estimate_ms"] = max(0, fields["max_reply_gap_ms"] - self.args.ping_interval * 1000)
        fields.update(sampler.finish())
        reverse = ping_metrics([json.loads(line) for line in probes[1][1].read_text().splitlines()])
        fields.update({"reverse_" + key: value for key, value in reverse.items()})
        fields.update({"scenario": "live-fallback-recovery-rollback", "strategy": strategy, "repetition": repetition,
                       "status": "pass", "detection_ms": (detected - injected) * 1000,
                       "decision_ms": (decision_end - decision_begin) * 1000,
                       "prepare_ms": (prepare_end - decision_end) * 1000,
                       "commit_ms": (commit_end - commit_begin) * 1000,
                       "post_validation_ms": (validated - commit_end) * 1000,
                       "transition_ms": (commit_end - detected) * 1000,
                       "fault_to_validated_ms": (validated - trial_start) * 1000,
                       "recovery_detection_ms": (recovered - restored) * 1000,
                       "recovery_commit_ms": (recovery_end - recovery_commit) * 1000,
                       "rollback_ms": (rollback_end - rollback_failure) * 1000,
                       "tcp_session_completed": 1,
                       "tcp_retransmissions": tcp_data["end"]["sum_sent"]["retransmits"],
                       "throughput_mbps": tcp_data["end"]["sum_received"]["bits_per_second"] / 1e6})
        for label, start, end in (("fallback", trial_start, validated + 1),
                                  ("recovery", restored, recovery_end + 1),
                                  ("rollback", rollback_begin, rollback_end + 1)):
            fields[label + "_gap_ms"] = ping_metrics(probe_events, start, end)["max_reply_gap_ms"]
            fields[label + "_loss_percent"] = ping_metrics(probe_events, start, end)["packet_loss"]
        (trial_dir / "result.json").write_text(json.dumps(fields, indent=2) + "\n")
        return fields

    def performance(self, path, repetition):
        self.routes(path)
        directory = self.output / f"throughput-{path}-{repetition}"
        directory.mkdir()
        sampler = Resources(self, directory / "resources.jsonl")
        self.samplers.append(sampler)
        server = self.start_process(["ip", "netns", "exec", "client-b", "iperf3", "-s", "-1", "-J"], directory / "server.json")
        time.sleep(0.5)
        measured = self.ip("client-a", "iperf3", "-c", "10.10.2.2", "-t", "10", "-J", timeout=25)
        (directory / "client.json").write_text(measured.stdout)
        server.wait(timeout=10)
        data = json.loads(measured.stdout)
        if "error" in data:
            raise RuntimeError(data["error"])
        row = {"scenario": "tcp-throughput", "strategy": path, "repetition": repetition, "status": "pass",
               "throughput_mbps": data["end"]["sum_received"]["bits_per_second"] / 1e6,
               "tcp_retransmissions": data["end"]["sum_sent"]["retransmits"]}
        row.update(sampler.finish())
        return row

    def vpp(self, node, *command):
        result = self.run(["vppctl", "-s", f"/run/ibuki-vpp-ns/{node}/cli.sock", *command])
        lowered = result.stdout.lower()
        if any(message in lowered for message in ("unknown input", "unknown interface", "parse error", "failed", "invalid", "error:")):
            raise RuntimeError("VPP CLI rejected the evaluation command: " + result.stdout)
        return result

    def isolation(self):
        self.run(["sh", "scripts/vm-netns-ipsec.sh", "hub", "stop"])
        self.run(["sh", "scripts/vm-vpp-ns-topology.sh", "clean"])
        self.run(["sh", "scripts/vm-vpp-ns-topology.sh", "setup"], env={
            "VPP_TOPOLOGY_MODE": "direct", "SKIP_GRE": "1", "SKIP_IPSEC_TAP": "1", "VPP_NS_NODES": "site-a site-b"})
        for vlan in (100, 200):
            for suffix, node, client, lan, transit, endpoint, peer in (
                ("a", "site-a", "client-a", "10.77.1", "172.30.1.1", "ib-dir-a", "172.30.1.2"),
                ("b", "site-b", "client-b", "10.77.2", "172.30.1.2", "ib-dir-b", "172.30.1.1")):
                namespace = f"paper-{suffix}-{vlan}"
                self.run(["ip", "netns", "add", namespace])
                self.ip(client, "ip", "link", "add", "link", "eth0", "name", f"pv{vlan}", "type", "vlan", "id", str(vlan))
                self.ip(client, "ip", "link", "set", f"pv{vlan}", "netns", namespace)
                self.ip(namespace, "ip", "link", "set", "lo", "up")
                self.ip(namespace, "ip", "addr", "add", lan + ".2/24", "dev", f"pv{vlan}")
                self.ip(namespace, "ip", "link", "set", f"pv{vlan}", "up")
                self.ip(namespace, "ip", "route", "add", "default", "via", lan + ".1")
                self.vpp(node, "ip", "table", "add", str(vlan))
                for base, address in ((f"host-ib-lan-{suffix}", lan + ".1/24"), ("host-" + endpoint, transit + "/30")):
                    interface = base + "." + str(vlan)
                    self.vpp(node, "create", "sub-interfaces", base, str(vlan))
                    self.vpp(node, "set", "interface", "ip", "table", interface, str(vlan))
                    self.vpp(node, "set", "interface", "ip", "address", interface, address)
                    self.vpp(node, "set", "interface", "state", interface, "up")
                remote = "10.77.2.0/24" if suffix == "a" else "10.77.1.0/24"
                self.vpp(node, "ip", "route", "add", remote, "table", str(vlan), "via", peer, f"host-{endpoint}.{vlan}")
        rows = []
        for repetition in range(1, self.args.repeat + 1):
            for vlan in (100, 200):
                for suffix, target in (("a", "10.77.2.2"), ("b", "10.77.1.2")):
                    self.ip(f"paper-{suffix}-{vlan}", "ping", "-c", "1", "-W", "2", target, check=False)
                    self.ip(f"paper-{suffix}-{vlan}", "ping", "-c", "3", "-W", "2", target)
            self.vpp("site-a", "ip", "route", "del", "10.77.2.0/24", "table", "100", "via", "172.30.1.2", "host-ib-dir-a.100")
            blocked = self.ip("paper-a-100", "ping", "-c", "2", "-W", "1", "10.77.2.2", check=False)
            if blocked.returncode != 1 or "100% packet loss" not in blocked.stdout:
                raise RuntimeError("VRF100 escaped into another forwarding table")
            self.ip("paper-a-200", "ping", "-c", "3", "-W", "2", "10.77.2.2")
            self.vpp("site-a", "ip", "route", "add", "10.77.2.0/24", "table", "100", "via", "172.30.1.2", "host-ib-dir-a.100")
            self.ip("paper-a-100", "ping", "-c", "3", "-W", "2", "10.77.2.2")
            rows.append({"scenario": "overlapping-vlan-vrf-isolation", "strategy": "tables-100-200", "repetition": repetition,
                         "status": "pass", "isolated_tables": 2})
            print(f"isolation: {repetition}/{self.args.repeat} pass", flush=True)
        return rows

    def cleanup(self):
        for sampler in self.samplers:
            sampler.finish()
        for process, stream in self.processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            stream.close()
        for suffix in ("a", "b"):
            for vlan in (100, 200):
                self.run(["ip", "netns", "del", f"paper-{suffix}-{vlan}"], check=False)
        self.ip("site-a", "ip", "link", "set", "a-direct", "up", check=False)
        for node in ("site-a", "site-b"):
            self.ip(node, "ip", "link", "del", "xfrm-direct", check=False)
        self.run(["sh", "scripts/vm-netns-ipsec.sh", "hub", "stop"], check=False)
        self.run(["sh", "scripts/vm-vpp-ns-topology.sh", "clean"], check=False)
        self.logs.close()
        if self.restore_system_vpp:
            subprocess.run(["systemctl", "start", "vpp"], check=False, timeout=60)
        for node in ("site-a", "site-b"):
            config = pathlib.Path("/etc/swanctl/ibuki-paper-live") / node / "swanctl.conf"
            config.unlink(missing_ok=True)


def evaluate(args):
    import fcntl

    if os.geteuid() != 0:
        raise RuntimeError("Run in a dedicated Linux VM as root")
    lock = pathlib.Path("/run/ibuki-paper-live.lock").open("w")
    fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    if (pathlib.Path(args.output) / "manifest.json").exists():
        raise RuntimeError("Choose a new output directory; prior measurement must not be overwritten")
    runtime = Runtime(args)
    manifest = {"revision": runtime.run(["git", "rev-parse", "HEAD"]).stdout.strip(),
                "dirty": runtime.run(["git", "status", "--porcelain"]).stdout,
                "kernel": runtime.run(["uname", "-a"]).stdout.strip(), "cpus": os.cpu_count(),
                "yaml_sha256": hashlib.sha256(pathlib.Path(args.yaml).read_bytes()).hexdigest(),
                "settings": vars(args), "executor": "evaluation-only pre-established route executor; not eventnetd apply",
                "measurement_sha256": hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
                "resource_scope": "sum of VPP and charon RSS/CPU; excludes measurement processes; one core=100%",
                "vpp": runtime.run(["vpp", "-v"], check=False).stdout.strip(),
                "strongswan": runtime.run(["swanctl", "--version"], check=False).stdout.strip()}
    (runtime.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    rows = []
    try:
        runtime.setup()
        for repetition in range(1, args.repeat + 1):
            for strategy in ("immediate", "graceful"):
                print(f"measure: {strategy} {repetition}/{args.repeat}", flush=True)
                row = runtime.trial(strategy, repetition)
                rows.append(row)
                with (runtime.output / "metrics.csv").open("w", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=sorted({key for row in rows for key in row}))
                    writer.writeheader()
                    writer.writerows(rows)
        for repetition in range(1, args.repeat + 1):
            for path in ("direct", "hub"):
                print(f"throughput: {path} {repetition}/{args.repeat}", flush=True)
                rows.append(runtime.performance(path, repetition))
                with (runtime.output / "metrics.csv").open("w", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=sorted({key for row in rows for key in row}))
                    writer.writeheader()
                    writer.writerows(rows)
        if args.isolation:
            rows.extend(runtime.isolation())
            with (runtime.output / "metrics.csv").open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=sorted({key for row in rows for key in row}))
                writer.writeheader()
                writer.writerows(rows)
    except Exception as error:
        (runtime.output / "failure.json").write_text(json.dumps({"status": "fail", "error": str(error), "completed_cases": len(rows)}, indent=2) + "\n")
        raise
    finally:
        runtime.cleanup()
        lock.close()


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    collector = commands.add_parser("probe")
    collector.add_argument("--target", required=True)
    collector.add_argument("--output", required=True)
    collector.add_argument("--duration", type=int, default=28)
    collector.add_argument("--interval", type=float, default=0.05)
    experiment = commands.add_parser("evaluate")
    experiment.add_argument("--yaml", default="samples/linux-vm-netns.yaml")
    experiment.add_argument("--build-dir", default="build-linux-cc")
    experiment.add_argument("--output", default="out/paper-live")
    experiment.add_argument("--repeat", type=int, default=5)
    experiment.add_argument("--duration", type=int, default=28)
    experiment.add_argument("--threshold", type=int, default=2)
    experiment.add_argument("--probe-interval", type=float, default=0.25)
    experiment.add_argument("--ping-interval", type=float, default=0.05)
    experiment.add_argument("--drain-ms", type=int, default=10)
    experiment.add_argument("--tcp-rate", default="10M")
    experiment.add_argument("--isolation", action="store_true")
    args = parser.parse_args()
    if args.duration < 25 or args.duration > 600 or (args.command == "evaluate" and (args.repeat < 1 or args.threshold < 1 or args.drain_ms < 0)):
        parser.error("duration >=25, repeat/threshold >=1, drain >=0 required")
    os.chdir(ROOT)
    if args.command == "probe":
        if not math.isfinite(args.interval) or args.interval < 0.01:
            parser.error("interval must be finite and >=0.01 seconds")
        probe(args)
    else:
        if not all(math.isfinite(value) and value >= 0.01 for value in (args.ping_interval, args.probe_interval)):
            parser.error("intervals must be finite and >=0.01 seconds")
        evaluate(args)


if __name__ == "__main__":
    main()
