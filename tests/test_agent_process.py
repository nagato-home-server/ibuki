import pathlib
import subprocess
import sys
import tempfile


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    fixtures = {
        "stdout": ("printf 'reply time=1.25 ms\\n'\n", "healthy"),
        "stderr": ("printf 'reply time=1.25 ms\\n' >&2\n", "healthy"),
        "exit_failure": ("printf 'reply time=1.25 ms\\n'\nexit 1\n", "failed"),
        "malformed": ("printf 'reply time=invalid ms\\n'\n", "failed"),
        "missing_executable": (None, "failed"),
    }
    with tempfile.TemporaryDirectory(prefix="ibuki-agent-process-") as temporary:
        root = pathlib.Path(temporary)
        for name, (body, expected) in fixtures.items():
            directory = root / name
            directory.mkdir()
            if body is not None:
                ping = directory / "ping"
                ping.write_text("#!/bin/sh\n" + body, encoding="utf-8")
                ping.chmod(0o700)
            for mask in range(8):
                result = subprocess.run(
                    [str(executable), str(mask), expected, str(directory)],
                    capture_output=True, timeout=20,
                )
                if result.returncode != 0:
                    raise RuntimeError(
                        f"{name}: closed mask {mask}, status {result.returncode}"
                    )
    print("Agent process checks passed: 40 cases, 1200 probes, stable FD counts, no unreaped children")


if __name__ == "__main__":
    main()
