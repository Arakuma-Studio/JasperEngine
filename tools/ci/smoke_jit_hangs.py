#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Whether every hang the watchdog reported in these logs is llvmpipe's JIT inside a draw.

    python3 tools/ci/smoke_jit_hangs.py <log>...      exit 0: all of them are; 1: not, or none

tools/smoke-test.sh asks this of a project's first editor open: llvmpipe compiles a program's
state variant at its first draw on the render thread, and a software GL has no worker context to
move that to (docs/specs/S32-async-shaders.md, deviation 6). A scene of many Lit variants first
drawn together holds one frame past the watchdog's 3 s until the Mesa disk cache has them. The
smoke then opens the editor again with that cache warm, where any hang fails. Anything else in a
report (another driver, the engine, the JIT outside a draw, a report that cannot be read) is a hang
as ever.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPORT = re.compile(r"JASPERENGINE HANG: main thread not responding .*report=(\S+)")


def stack_of(report: str) -> str:
    """the main thread's frames: what follows is what the job threads were doing"""
    _, found, rest = report.partition("\nstack:\n")
    return rest.partition("\njob system:")[0] if found else ""


def is_software_jit(report: str) -> bool:
    stack = stack_of(report)
    # Mesa before 24 links its gallium drivers into swrast_dri.so (the Ubuntu 22.04 build image)
    gallium = "libgallium" in stack or "swrast_dri" in stack
    return "libLLVM" in stack and gallium and "SokolObject::draw" in stack


def only_software_jit(logs: list[Path]) -> bool:
    reports = [match for log in logs for match in REPORT.findall(log.read_text(errors="replace"))]
    if not reports:
        return False
    for report in reports:
        path = Path(report)
        if not path.is_file() or not is_software_jit(path.read_text(errors="replace")):
            return False
    return True


def main(argv: list[str]) -> int:
    return 0 if only_software_jit([Path(arg) for arg in argv]) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
