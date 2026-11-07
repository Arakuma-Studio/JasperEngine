"""Public CSS completion keywords, independent of generator internals.

The author-facing property names are specified by the HTML UI guide and phase-3
CSS compatibility contract. Their presence in the completion data protects the
historical loss of properties registered by RmlUi loops. Generic generator output
self-comparisons are not regression oracles.

The per-project AGENTS.md (Generator::writeAgentsFile) is separate from the repository's own
contributor AGENTS.md (S31 R28): a project made by create_project gets the generated one, which
names `jasperengine-editor call`, and jasper://project/AGENTS.md serves it, not the repository's.
"""

import json
import re
from pathlib import Path

import harness
from harness import Editor, TestFailure

CI = True
EDITOR = {"create": True, "name": "Generators"}


async def test_rcss_properties_keep_loop_registered_names():
    # CSS authoring keys: docs/mcp/guide/html-ui.md (supported border, transition,
    # stacking properties), docs/html-ui/phase-3-fork-patches.md.
    # Read only literal key bytes: variable renames, formatting, and comments
    # must not affect this configuration-data check.
    header = (harness.REPO / "editor/window/widget/RcssProperties.h").read_text(encoding="utf-8")
    tokens = re.findall(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', header, re.S)
    names = {json.loads(token).casefold() for token in tokens if token.startswith('"')}
    required = {"border-top-width", "border-left-width", "border-top-left-radius", "border-bottom-right-radius",
                "will-change", "transition-property"}
    if not required <= names:
        raise TestFailure(f"missing author-facing CSS completion keys: {sorted(required - names)}")


# The generated AGENTS.md's size limit: the ratchet of what it costs every agent session in a game
AGENTS_MD_BYTES = 5000


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
    # S53 R26: the model cook's tools, where an agent of the game finds them
    ed.check("inspect_models" in agents and "check_render_budgets" in agents, "AGENTS.md names inspect_models and check_render_budgets")
    # every agent working in the game reads it (CLAUDE.md imports it): the MCP guide holds the rest
    ed.check(len(agents.encode()) <= AGENTS_MD_BYTES, f"AGENTS.md is {len(agents.encode())} bytes, over {AGENTS_MD_BYTES}")
    ed.check_eq((target / "CLAUDE.md").read_text(encoding="utf-8"), "@AGENTS.md\n", "CLAUDE.md imports it")
    ed.check_eq(served, agents, "jasper://project/AGENTS.md serves the project's file")
    repo = (harness.REPO / "AGENTS.md").read_text(encoding="utf-8")
    ed.check(served != repo and "For engine contributors" not in served, "not the repository's contributor AGENTS.md")
