"""Generated files stay current: every generator of the repository with a --check mode is run
in it, and a stale file fails with the generator's own message (what to run). No editor needed.

tools/gen_rcss_properties.py had no test: the code editor's RCSS property suggestions
(editor/window/widget/RcssProperties.h) went stale from phase 3's first fork registration on
(p3-base, `will-change`) until p5-tooling regenerated them. The HTML/CSS support matrix's check
(it needs the editor) stays in test_html_ui_checks.py.

The per-project AGENTS.md (Generator::writeAgentsFile) is separate from the repository's own
contributor AGENTS.md (S31 R28): a project made by create_project gets the generated one, which
names `jasperengine-editor call`, and jasper://project/AGENTS.md serves it, not the repository's.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import harness
from harness import Editor, TestFailure

CI = True
EDITOR = {"create": True, "name": "Generators"}

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
    ("tools/htmlui/xcode_add_video_libs.py", []),   # p6-video (p6-accept: the only Xcode sync no test ran)
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


async def test_project_agents_file(ed: Editor):
    target = ed.scratch.root / "made-by-create-project"
    await ed.call("create_project", path=str(target), name="AgentsDoc", unsaved="discard")
    status = await ed.call("editor_status")
    ed.check_eq(Path(status["project"]["path"]).resolve(), target.resolve(), "the new project is open")
    # written on the first play/build, or on the first read of the resource
    got = await ed.session.read_resource("jasper://project/AGENTS.md")
    served = "".join(getattr(c, "text", "") or "" for c in got.contents)
    agents = (target / "AGENTS.md").read_text(encoding="utf-8")
    ed.check_eq(agents.splitlines()[0], "# JasperEngine Project Context", "the generated AGENTS.md")
    ed.check("`jasperengine-editor call <tool> [--flags]`" in agents, "it names the call CLI")
    ed.check_eq((target / "CLAUDE.md").read_text(encoding="utf-8"), "@AGENTS.md\n", "CLAUDE.md imports it")
    ed.check_eq(served, agents, "jasper://project/AGENTS.md serves the project's file")
    repo = (harness.REPO / "AGENTS.md").read_text(encoding="utf-8")
    ed.check(served != repo and "For engine contributors" not in served, "not the repository's contributor AGENTS.md")

