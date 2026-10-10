import argparse
import json
import hashlib
from pathlib import Path
import subprocess


CORE_CASES = ("count_predicate", "reject_invalid_counts", "candidates", "comparisons", "copy")
MODULE_CASES = {
    "audit_capacity": ["src/audit.c", "src/state.c"],
    "error_capacity": ["src/audit.c", "src/state.c"],
    "swan_mock": ["src/strongswan_adapter_mock.c"],
    "vpp_failure": ["src/vpp_adapter_mock.c"],
    "command_rejection": ["src/render_commands.c"],
    "transport_arguments": ["src/vpp_api_transport.c"],
    "observer_empty": ["src/strongswan_observer.c", "src/vpp_observer.c"],
    "path_count_predicate": ["src/state.c"],
    "plan_counts": ["src/apply_plan.c", "src/state.c", "src/render_commands.c"],
}
YAML_CASES = ("yaml_context", "yaml_dedent", "yaml_boolean", "yaml_counts")
CASES = CORE_CASES + tuple(MODULE_CASES) + YAML_CASES


def main():
    parser = argparse.ArgumentParser(description="Bounded CBMC checks of actual Controller core")
    parser.add_argument("--cbmc", default="cbmc")
    parser.add_argument("--out-dir", default="out/cbmc-core")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    root = Path(__file__).resolve().parents[2]
    output = (root / args.out_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    version = subprocess.check_output([args.cbmc, "--version"], text=True, timeout=10).strip()
    print("CBMC:", version, flush=True)
    results = []
    for case in args.cases:
        sources = ["tests/formal/cbmc_core_harness.c", "src/path_selection.c", "src/state.c"]
        if case in MODULE_CASES:
            sources = ["tests/formal/cbmc_modules_harness.c"] + MODULE_CASES[case]
        if case in YAML_CASES:
            sources = ["tests/formal/cbmc_yaml_harness.c", "src/state.c"]
        command = [args.cbmc] + sources + ["-D", "__NO_CTYPE=1", "-I", "include", "-I", "src", "-I", "third_party/yyjson", "--function", "harness_" + case,
                   "--unwind", "66" if case == "copy" or case in YAML_CASES else "18",
                   "--unwindset", "vsnprintf.0:154,vsnprintf.1:154",
                   "--unwinding-assertions", "--bounds-check", "--pointer-check",
                   "--signed-overflow-check", "--slice-formula"]
        print("CBMC check:", case, flush=True)
        log = output / (case + ".log")
        dependencies = set(sources)
        dependencies.update(path.relative_to(root).as_posix() for path in (root / "include").rglob("*.h"))
        dependencies.add("src/internal.h")
        if case in YAML_CASES:
            dependencies.add("src/yaml_config.c")
        hashes = {source: hashlib.sha256((root / source).read_bytes()).hexdigest() for source in sorted(dependencies)}
        with log.open("w", encoding="utf-8") as stream:
            try:
                completed = subprocess.run(command, cwd=root, stdout=stream,
                                           stderr=subprocess.STDOUT, timeout=args.timeout)
                status = completed.returncode
            except subprocess.TimeoutExpired:
                status = "timeout"
        text = log.read_text(encoding="utf-8", errors="replace")
        unchanged = all(hashlib.sha256((root / source).read_bytes()).hexdigest() == digest for source, digest in hashes.items())
        passed = status == 0 and "VERIFICATION SUCCESSFUL" in text and unchanged
        results.append({"case": case, "status": "pass" if passed else "fail",
                        "exit_status": status, "log": log.name, "command": command,
                        "sources_sha256": hashes, "unchanged_during_check": unchanged})
        print("CBMC result:", case, results[-1]["status"], "exit:", status, flush=True)
    summary = {"cbmc_version": version, "cases": results,
               "scope": "Individual harnesses with stated input assumptions; not whole-program proof"}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return 0 if all(item["status"] == "pass" for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
