import argparse
import ast
import hashlib
import json
import os
import re
import signal
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description="Project-wide checks with explicit verification gaps")
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--analyzer", choices=("gcc", "clang"), default="gcc")
    parser.add_argument("--files", nargs="+", help="Run checks only for these repository-relative files")
    parser.add_argument("--cbmc", default="cbmc")
    parser.add_argument("--out-dir", default="out/project-verification")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--vici-include", help="Directory containing the actual libvici.h")
    parser.add_argument("--skip-cbmc", action="store_true", help="Static checks only; CBMC remains unverified")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    root = Path(__file__).resolve().parents[2]
    output = (root / args.out_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    results = []

    def run(label, command, limit=None):
        print("project check:", label, flush=True)
        log = output / (label.replace("/", "_") + ".log")
        try:
            with log.open("w", encoding="utf-8") as stream:
                process = subprocess.Popen(command, cwd=root, stdout=stream,
                                           stderr=subprocess.STDOUT, start_new_session=os.name != "nt")
                try:
                    exit_status = process.wait(timeout=limit or args.timeout)
                    status = "pass" if exit_status == 0 else "fail"
                except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
                    if os.name != "nt":
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    else:
                        process.kill()
                    process.wait()
                    if isinstance(error, KeyboardInterrupt):
                        raise
                    status, exit_status = "incomplete", "timeout"
        except OSError as error:
            log.write_text(str(error) + "\n", encoding="utf-8")
            status, exit_status = "incomplete", "unavailable"
        result = {"check": label, "status": status, "exit_status": exit_status,
                  "log": str(log.relative_to(output))}
        results.append(result)
        return result, log

    inventory = []
    for folder in ("src", "include", "examples", "tests", "scripts", "third_party/yyjson"):
        for path in sorted((root / folder).rglob("*")):
            if not path.is_file() or path.suffix not in (".c", ".h", ".py", ".sh"):
                continue
            relative = path.relative_to(root).as_posix()
            selected = not args.files or relative in args.files
            entry = {"file": relative, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                     "formal_status": "not_proved", "checks": []}
            if path.suffix == ".c":
                names = re.findall(r"(?m)^[A-Za-z_][A-Za-z_0-9 \t*]*\s+([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{",
                                   path.read_text(encoding="utf-8"))
                entry["functions"] = [{"name": name, "formal_status": "not_proved"} for name in names]
            inventory.append(entry)
            if not selected:
                continue
            if path.suffix == ".c" and "formal" not in path.parts:
                target = output / (relative.replace("/", "_") + ".o")
                flags = ["-fanalyzer", "-c"] if args.analyzer == "gcc" else ["--analyze", "-Xanalyzer", "-analyzer-output=text"]
                command = [args.cc, "-std=c17", "-D_POSIX_C_SOURCE=200809L",
                    "-Iinclude", "-Isrc", "-Ithird_party/yyjson", "-Wall", "-Wextra"] + flags + [relative, "-o", str(target)]
                if args.vici_include:
                    command += ["-I", args.vici_include]
                result, log = run(relative, command)
                diagnostics = log.read_text(encoding="utf-8", errors="replace")
                if result["status"] == "pass" and "warning:" in diagnostics:
                    result["status"] = "review"
                entry["checks"].append(result)
            elif path.suffix == ".sh":
                result, _ = run(relative, ["sh", "-n", relative])
                entry["checks"].append(result)
            elif path.suffix == ".py":
                try:
                    ast.parse(path.read_text(encoding="utf-8"), filename=relative)
                    result = {"check": relative, "status": "pass", "scope": "syntax only"}
                except (SyntaxError, UnicodeError) as error:
                    result = {"check": relative, "status": "fail", "error": str(error)}
                entry["checks"].append(result)
                results.append(result)
            elif path.suffix == ".h" and folder == "include":
                probe = output / (relative.replace("/", "_") + ".c")
                probe.write_text('#include "' + path.relative_to(root / "include").as_posix() + '"\n', encoding="utf-8")
                result, _ = run(relative, [args.cc, "-std=c17", "-D_POSIX_C_SOURCE=200809L",
                                          "-Iinclude", "-Ithird_party/yyjson", "-fsyntax-only", str(probe)])
                entry["checks"].append(result)

    if args.files:
        unknown = set(args.files) - {entry["file"] for entry in inventory}
        if unknown:
            results.append({"check": "file_selection", "status": "fail", "unknown_files": sorted(unknown)})
    if not results and args.skip_cbmc:
        results.append({"check": "empty_selection", "status": "incomplete"})
    if not args.skip_cbmc:
        run("cbmc", [sys.executable, "tests/formal/check_cbmc.py", "--cbmc", args.cbmc,
            "--out-dir", str(output / "cbmc"), "--timeout", str(args.timeout)],
            limit=args.timeout * 30)
    report = {"scope": "Static analyzer and syntax checks; CBMC only for named harnesses",
              "analyzer": args.analyzer, "requested_files": args.files,
              "results": results, "inventory": inventory,
              "gaps": ["Headers are checked through including translation units, not independent proof",
                       "External-enabled libvici/VAPI and Windows branches need their own toolchains",
                       "Daemon, kernel, filesystem, scripts and actual network behavior are not formally proved",
                       "CBMC harness assumptions do not cover every function or every input",
                       "Third-party VPP/strongSwan source trees are outside this repository check"]}
    changed = [entry["file"] for entry in inventory if
               hashlib.sha256((root / entry["file"]).read_bytes()).hexdigest() != entry["sha256"]]
    report["changed_during_run"] = changed
    report["cbmc_requested"] = not args.skip_cbmc
    if changed:
        results.append({"check": "source_snapshot", "status": "incomplete", "files": changed})
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("project report:", output / "summary.json", flush=True)
    return 0 if all(result["status"] == "pass" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
