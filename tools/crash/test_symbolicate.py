#!/usr/bin/env python3
"""Tests of tools/crash/symbolicate.py against a real binary.

    python3 -I tools/crash/test_symbolicate.py BINARY

BINARY is an ELF built with symbols (build/linux/core-tests/.../jasperengine-crash-test). A report
naming `main` by module+offset (nm's value: the module-relative address of a PIE) must come back
with main named; a Windows report prints the cdb steps; a report with no frames named exits 1.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def symbol_offset(binary: Path, name: str) -> int:
    out = subprocess.run(["nm", "--defined-only", str(binary)], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise SystemExit(f"{binary} defines no {name}")


def symbolicate(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(HERE / "symbolicate.py"), *args], capture_output=True, text=True)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = Path(sys.argv[1]).resolve()
    offset = symbol_offset(binary, "main")
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        report = root / "crash.txt"
        report.write_text("JasperEngine crash report v1\nproduct: test\nplatform: linux-x86_64\nbuild_id: none\n"
                          "reason: SIGSEGV (invalid memory access)\nframes:\n"
                          f"#0 {binary.name}+0x{offset + 4:x}\n#1 libnothing.so+0x10\n")
        result = symbolicate(str(report), "--binary-dir", str(binary.parent))
        print(result.stdout)
        if result.returncode != 0 or "=>  main" not in result.stdout:
            failures.append(f"main was not named (exit {result.returncode}): {result.stdout} {result.stderr}")
        if "1 of 2 frames named" not in result.stdout:
            failures.append("an unknown module must stay unnamed")

        windows = root / "win" / "crash.txt"
        windows.parent.mkdir()
        windows.write_text("JasperEngine crash report v1\nplatform: windows-x86_64\nbuild_id: ABC1\nframes:\n#0 game.exe+0x10\n")
        result = symbolicate(str(windows.parent))
        if result.returncode != 0 or "cdb -z" not in result.stdout:
            failures.append(f"a Windows report gives the cdb steps: {result.stdout}")

        empty = root / "none.txt"
        empty.write_text("JasperEngine crash report v1\nplatform: linux-x86_64\nframes:\n#0 missing.so+0x10\n")
        if symbolicate(str(empty)).returncode != 1:
            failures.append("a report with nothing named must exit 1")

    for failure in failures:
        print("FAIL:", failure)
    print("PASS" if not failures else f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
