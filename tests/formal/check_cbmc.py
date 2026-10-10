import argparse
import json
from pathlib import Path
import subprocess


CASES = ("count_predicate", "reject_invalid_counts", "candidates", "comparisons", "copy")


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
        command = [args.cbmc, "tests/formal/cbmc_core_harness.c", "src/path_selection.c",
                   "src/state.c", "-I", "include", "-I", "src", "--function", "harness_" + case,
                   "--unwind", "66" if case == "copy" else "18",
                   "--unwindset", "vsnprintf.0:154,vsnprintf.1:154",
                   "--unwinding-assertions", "--bounds-check", "--pointer-check",
                   "--signed-overflow-check", "--slice-formula"]
        print("CBMC check:", case, flush=True)
        log = output / (case + ".log")
        with log.open("w", encoding="utf-8") as stream:
            try:
                completed = subprocess.run(command, cwd=root, stdout=stream,
                                           stderr=subprocess.STDOUT, timeout=args.timeout)
                status = completed.returncode
            except subprocess.TimeoutExpired:
                status = "timeout"
        text = log.read_text(encoding="utf-8", errors="replace")
        passed = status == 0 and "VERIFICATION SUCCESSFUL" in text
        results.append({"case": case, "status": "pass" if passed else "fail",
                        "exit_status": status, "log": log.name})
        print("CBMC result:", case, results[-1]["status"], "exit:", status, flush=True)
    summary = {"cbmc_version": version, "cases": results,
               "scope": "Individual harnesses with stated input assumptions; not whole-program proof"}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return 0 if all(item["status"] == "pass" for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
