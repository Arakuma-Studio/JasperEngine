"""Generated files stay current: every generator of the repository with a --check mode is run
in it, and a stale file fails with the generator's own message (what to run). No editor needed.

tools/gen_rcss_properties.py had no test: the code editor's RCSS property suggestions
(editor/window/widget/RcssProperties.h) went stale from phase 3's first fork registration on
(p3-base, `will-change`) until p5-tooling regenerated them. The HTML/CSS support matrix's check
(it needs the editor) stays in test_html_ui_checks.py.
"""

from __future__ import annotations

import subprocess
import sys

import harness
from harness import TestFailure

# (script, extra arguments), each run as `python3 <script> [args] --check` from the repository root
GENERATORS = [
    ("tools/gen_rcss_properties.py", []),
    ("tools/htmlui/gen_effect_manifests.py", []),
    ("tools/htmlui/gen_effects_docs.py", []),
    ("tools/htmlui/gen_testcard.py", []),
    ("tools/htmlui/kit_reference.py", []),
    ("tools/htmlui/xcode_sync_compat.py", []),
    ("tools/htmlui/xcode_sync_effects.py", []),
    ("tools/htmlui/xcode_sync_rmlui.py", []),
    ("tools/htmlui/xcode_add_lexbor.py", []),
]


def run(script: str, args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(harness.REPO / script), *args, "--check"], cwd=str(harness.REPO),
                          capture_output=True, text=True, timeout=600)


async def test_generated_files_are_current():
    stale = []
    for script, args in GENERATORS:
        r = run(script, args)
        if r.returncode != 0:
            stale.append(f"{script} --check (exit {r.returncode}):\n{(r.stdout + r.stderr).strip()[-1500:]}")
    if stale:
        raise TestFailure("generated files are stale:\n" + "\n".join(stale))


async def test_rcss_properties_keep_loop_registered_names():
    """The fork registers some longhands in loops (border widths) or builds them at run time (corner radii):
    the generator finds them through the shorthands' item lists and the names[] tables."""
    header = (harness.REPO / "editor/window/widget/RcssProperties.h").read_text(encoding="utf-8")
    for name in ("border-top-width", "border-left-width", "border-top-left-radius", "border-bottom-right-radius",
                 "will-change", "-jfx-text-fill", "transition-property"):
        if f'"{name}",' not in header:
            raise TestFailure(f"RcssProperties.h lacks {name}")
    if '"-rmlui-border-top-left-radius-x"' in header:
        raise TestFailure("RcssProperties.h lists an internal longhand")
