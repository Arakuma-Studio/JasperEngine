#!/usr/bin/env python3
"""Tests of tools/crash/symbolicate.py against a real binary.

    python3 -I tools/crash/test_symbolicate.py BINARY

BINARY is an ELF or Mach-O built with symbols (the native jasperengine-crash-test). A report
naming `main` by module+offset must come back
with main named; a Windows report prints the cdb steps; a report with no frames named exits 1.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def symbol_offset(binary: Path, name: str) -> int:
    apple = sys.platform == "darwin"
    out = subprocess.run(["nm", "-U" if apple else "--defined-only", str(binary)], capture_output=True, text=True, check=True).stdout
    base = 0
    if apple:
        commands = subprocess.run(["otool", "-l", str(binary)], capture_output=True, text=True, check=True).stdout
        # Mach-O symbol values include the preferred __TEXT address (usually 4 GiB
        # for an executable); engine reports store offsets from the loaded image.
        match = re.search(r"segname __TEXT\s+vmaddr (0x[0-9a-fA-F]+)", commands)
        if not match:
            raise SystemExit(f"{binary} has no Mach-O __TEXT segment")
        base = int(match[1], 16)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == ("_" + name if apple else name):
            return int(parts[0], 16) - base
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

        # a library loaded after install: an absolute address, placed through the maps copy
        late = root / "late.txt"
        start = 0x55d000000000
        late.write_text("JasperEngine crash report v1\nplatform: linux-x86_64\nbuild_id: none\nbacktrace:\n"
                        f"#0 module?+0x{start + offset + 4:x}\nmodules:\n"
                        f"{start:x}-{start + 0x10000000:x} r-xp 00000000 08:01 42 {binary}\n")
        result = symbolicate(str(late))
        print(result.stdout)
        if result.returncode != 0 or "=>  main" not in result.stdout:
            failures.append(f"a late library's frame was not placed through the maps: {result.stdout} {result.stderr}")

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
