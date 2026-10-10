import argparse
from pathlib import Path
import re
import subprocess


def solve(z3, body, values=()):
    query = body + "\n(check-sat)\n"
    if values:
        query += "(get-value (" + " ".join(values) + "))\n"
    result = subprocess.run([z3, "-in", "-smt2"], input=query, text=True,
                            capture_output=True, timeout=30, check=True)
    lines = result.stdout.splitlines()
    if not lines or lines[0] not in ("sat", "unsat"):
        raise RuntimeError("Solver did not establish sat/unsat: " + result.stdout)
    assignments = {name: int(value) for name, value in
                   re.findall(r"\((\w+)\s+(\d+)\)", result.stdout)}
    return lines[0], assignments


def main():
    parser = argparse.ArgumentParser(description="Z3 policy model plus actual C selection replay")
    parser.add_argument("--z3", default="z3")
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--out-dir", default="out/formal-path-selection")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = (root / args.out_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    probe = output / "path-selection-probe"
    subprocess.run([args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror", "-g",
                    "-I" + str(root / "include"), "-I" + str(root / "src"),
                    str(root / "tests/formal/path_selection_probe.c"),
                    str(root / "src/path_selection.c"), str(root / "src/state.c"),
                    "-lm", "-o", str(probe)], check=True, timeout=60)
    declarations = """
(declare-const active Int)
(declare-const alternative Int)
(declare-const limit Int)
(declare-const hysteresis Int)
(assert (and (>= active 1) (<= active 100)))
(assert (and (>= alternative 0) (< alternative active)))
(assert (and (>= limit 0) (<= limit 100)))
(assert (and (>= hysteresis 1) (<= hysteresis 100)))
(assert (< (* 100 (- active alternative)) (* hysteresis active)))
"""
    status, model = solve(args.z3, declarations +
                          "(assert (> active limit))\n(assert (<= alternative limit))",
                          ("active", "alternative", "limit", "hysteresis"))
    if status != "sat" or len(model) != 4:
        raise RuntimeError("Expected a complete counterexample model")
    print("Z3 threshold counterexample:", model, flush=True)
    boolean_model = """
(declare-const active_eligible Bool)
(declare-const small_improvement Bool)
(define-fun legacy_retain () Bool small_improvement)
(define-fun repaired_retain () Bool (and active_eligible small_improvement))
"""
    for name, expression, expected in (
        ("legacy can retain an ineligible path", "legacy_retain", "sat"),
        ("eligibility guard prevents invalid retention", "repaired_retain", "unsat"),
    ):
        status, _ = solve(args.z3, boolean_model +
                          "(assert (and (not active_eligible) " + expression + "))")
        print("Z3:", name, status, flush=True)
        if status != expected:
            raise RuntimeError("Unexpected proof result")
    cases = [(name, model["active"], model["alternative"], model["hysteresis"],
              model["limit"], "alternative")
             for name in ("rtt", "loss", "disabled", "absent", "waypoint")]
    cases += [("healthy", 100, value, 20, 100, expected)
              for value, expected in ((99, "active"), (81, "active"),
                                      (80, "alternative"), (79, "alternative"),
                                      (0, "alternative"))]
    failed = 0
    for scenario, active, alternative, hysteresis, limit, expected in cases:
        replay = subprocess.run([str(probe), scenario, str(active), str(alternative),
                                 str(hysteresis), str(limit)], text=True,
                                capture_output=True, timeout=10)
        passed = replay.returncode == 0 and replay.stdout.strip() == "0 " + expected
        print("C replay:", scenario, active, alternative, hysteresis, limit,
              replay.stdout.strip(), "pass" if passed else "FAIL", flush=True)
        failed += not passed
    print("C replay cases:", len(cases), "failures:", failed, flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
