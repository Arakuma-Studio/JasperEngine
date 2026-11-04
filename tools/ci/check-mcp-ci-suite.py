#!/usr/bin/env python3
"""Every MCP test file is classified for the CI suite: CI = True, CI_EXCLUDED = "why", or it needs
the default fixture project (tools/mcp/run_tests.py, ci_files).

`--suite ci` refuses to run when one is not, but only after check-changed's editor build, and for
everyone once the file is on main. This says the same in a second, with the standard library alone:
check-changed runs it when a diff touches an MCP test file.

    python3 tools/ci/check-mcp-ci-suite.py
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

RUN_TESTS = Path(__file__).resolve().parents[1] / "mcp" / "run_tests.py"


def main() -> int:
    spec = importlib.util.spec_from_file_location("run_tests", RUN_TESTS)
    run_tests = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(run_tests)
    files, problems, excluded = run_tests.ci_files()
    for problem in problems:
        print(f"check-mcp-ci-suite: {problem}", file=sys.stderr)
    if problems:
        print(f"check-mcp-ci-suite: {len(problems)} problem(s): docs/mcp/README.md, \"The CI suite\"", file=sys.stderr)
        return 1
    print(f"check-mcp-ci-suite: {len(files)} file(s) in the CI suite, {len(excluded)} left out, each with its reason")
    return 0


if __name__ == "__main__":
    sys.exit(main())
