#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Checks that the missing-translation capture compiles out of release engines
(docs/specs/game-ui-localization.md D8, AC12): the capture code defines the symbol
jasperengine_l10n_capture, so `nm` of a release engine (JASPERENGINE_L10N_CAPTURE=0) must not
list it and `nm` of the editor's engine (or a Development one) must.

    python3 tools/l10n/check_capture_symbols.py --absent build/linux/engine/libjasperengine.a \
        --present build/linux/editor/libjasperengine.so

Exit 1 naming each library that breaks the rule; 2 when a library or nm is missing.
"""

import argparse
import shutil
import subprocess
import sys

SYMBOL = "jasperengine_l10n_capture"


def defines(library: str) -> bool:
    nm = shutil.which("nm")
    if not nm:
        print("check_capture_symbols: no nm on PATH", file=sys.stderr)
        sys.exit(2)
    run = subprocess.run([nm, "-A", "--defined-only", library], capture_output=True, text=True)
    if run.returncode != 0 and "no symbols" not in run.stderr:
        dynamic = subprocess.run([nm, "-D", "--defined-only", library], capture_output=True, text=True)
        if dynamic.returncode != 0:
            print(f"check_capture_symbols: nm cannot read {library}: {run.stderr.strip()}", file=sys.stderr)
            sys.exit(2)
        run = dynamic
    return any(line.split()[-1] == SYMBOL for line in run.stdout.splitlines() if line.split())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--absent", action="append", default=[], help="a release engine library")
    parser.add_argument("--present", action="append", default=[], help="an editor or Development engine library")
    args = parser.parse_args()
    if not args.absent and not args.present:
        parser.error("name at least one library")
    failed = 0
    for library in args.absent:
        found = defines(library)
        print(f"{'FAIL' if found else 'ok'}   {library}: {SYMBOL} {'defined' if found else 'absent'} (release: must be absent)")
        failed += found
    for library in args.present:
        found = defines(library)
        print(f"{'ok' if found else 'FAIL'}   {library}: {SYMBOL} {'defined' if found else 'absent'} (capture build: must be defined)")
        failed += not found
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
