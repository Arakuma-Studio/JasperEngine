"""W4 docs: jasper://guide and jasper://docs/* resources, the project resources, engine API search,
prompts; and the docs gate of docs/mcp/TOOLS.md (S31 R22): `gen_tool_docs.py --check` passes on
the committed file and fails after a description edit that was not regenerated (red control), the
header names the editor version, each tool shows its group and the sections follow _meta modules."""

import asyncio
import os
import shutil
import sys
import tempfile
from pathlib import Path

from mcp.shared.exceptions import MCPError

import harness
from harness import Editor

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import gen_tool_docs  # noqa: E402

TIMEOUT = 900
TOOLS_MD = harness.REPO / "docs" / "mcp" / "TOOLS.md"
EDITOR = {"create": True, "name": "MCP Resources"}
CI = True

DOCS = ["animation", "assets", "camera", "cpp-scripting", "data-safety", "debugging", "html-ui", "input",
        "lattice", "lines-trails", "lua-api", "lua-scripting", "palettes", "particles", "physics",
        "platform-services", "post-processing", "prefabs", "safe-area", "scene-view", "shaders",
        "sprite-normals", "tilemaps", "ui", "undo", "unity-glossary", "vertex-animation"]


async def read_text(ed: Editor, uri: str) -> str:
    got = await ed.session.read_resource(uri)
    return "".join(getattr(c, "text", "") or "" for c in got.contents)


async def test_resources_listed(ed: Editor):
    listed = await ed.session.list_resources()
    uris = {str(r.uri) for r in listed.resources}
    for name in DOCS:
        ed.check_in(f"jasper://docs/{name}", uris, "doc resource listed")
    for uri in ("jasper://guide", "jasper://docs", "jasper://project/AGENTS.md"):
        ed.check_in(uri, uris, "resource listed")
    templates = await ed.session.list_resource_templates()
    ed.check(any(t.uri_template == "jasper://project/{+path}" for t in templates.resource_templates), "project file template")


async def test_read_docs(ed: Editor):
    guide = await read_text(ed, "jasper://guide")
    ed.check(guide.startswith(ed.instructions.strip()), "guide starts with the initialize instructions")
    ed.check("jasper://docs/lua-api" in guide, "guide lists the docs")
    for name in DOCS:
        text = await read_text(ed, f"jasper://docs/{name}")
        ed.check(bool(text.strip()), f"{name} returned an empty resource")
    # the big Lua reference is an index of section resources
    lua = await read_text(ed, "jasper://docs/lua-api")
    sections = [line.split("`")[1] for line in lua.splitlines() if line.startswith("- `jasper://docs/lua-api/")]
    ed.check(bool(sections), "lua-api index has no readable section resources")
    events = next(u for u in sections if "event-registration" in u)
    ed.check("RegisterEngineEvent" in await read_text(ed, events), "lua-api section content")
    listed = {str(r.uri) for r in (await ed.session.list_resources()).resources}
    ed.check(all(u in listed for u in sections), "sections are listed resources")
    hits = await ed.call("search_engine_api", query="RegisterEngineEvent", max_results=1)
    ed.check(any(h["doc"].startswith("jasper://docs/lua-api/") for h in hits["docs"]), f"doc hits point at sections: {hits['docs']!r}")
    try:
        await ed.session.read_resource("jasper://docs/nope")
    except MCPError as e:
        ed.check("lua-api" in str(e), f"missing doc error lists the documents: {e}")
    else:
        ed.fail("reading a missing doc should fail")


async def test_project_resources(ed: Editor):
    (ed.project / "AGENTS.md").unlink(missing_ok=True)
    claude_file = ed.project / "CLAUDE.md"
    claude_file.unlink(missing_ok=True)
    agents = await read_text(ed, "jasper://project/AGENTS.md")
    ed.check((ed.project / "AGENTS.md").is_file(), "reading the resource generates AGENTS.md")
    ed.check(claude_file.is_file(), "generating context also creates its client import")
    ed.check(any(line.strip() in ("@AGENTS.md", "@./AGENTS.md") for line in claude_file.read_text().splitlines()),
             "CLAUDE.md imports the generated project context")
    # Public onboarding targets required by phase-4-agent-tooling.md §3.8.4.
    for target in ("check --ui", "jasper://docs/html-ui", "jasper://docs/html-ui-support",
                   ".jasperengine/engine-api/html-ui-examples/"):
        ed.check_in(target, agents, "generated AGENTS.md exposes the HTML UI checks and references")
    contents = "Resource probe: caf\u00e9\nSecond line.\n"
    (ed.project / "resource-probe.txt").write_text(contents, encoding="utf-8")
    ed.check_eq(await read_text(ed, "jasper://project/resource-probe.txt"), contents,
                "project resources preserve UTF-8 file contents")
    try:
        await ed.session.read_resource("jasper://project/../etc/passwd")
    except MCPError as error:
        ed.check("inside the project" in str(error), f"path traversal failed for the wrong reason: {error}")
    else:
        ed.fail("escaping the project must fail")


async def test_search_engine_api(ed: Editor):
    ed.require_tools("search_engine_api", "search_engine_source", "read_engine_source")
    r = await ed.call("search_engine_api", query="Rigidbody2D applyForce")
    ed.check(r["results"], "API results")
    ed.check(any("applyForce" in x["detail"] for x in r["results"]), f"applyForce found: {r['results'][:3]!r}")
    ed.check(r["docs"] and r["docs"][0]["doc"].startswith("jasper://docs/"), f"doc hits: {r['docs']!r}")
    src = await ed.call("search_engine_source", query="REGISTER_ENGINE_EVENT", max_results=5)
    ed.check(0 < len(src["matches"]) <= 5, f"engine source search: {src!r}")
    hit = src["matches"][0]
    got = await ed.call_raw("read_engine_source", {"path": hit["path"], "start_line": hit["line"], "max_lines": 3})
    ed.check(not got.is_error and "REGISTER_ENGINE_EVENT" in got.text, f"read the hit: {got.text[:300]!r}")
    ed.check_eq(got.structured["start_line"], hit["line"])
    await ed.check_error("read_engine_source", path="../etc/passwd.h", contains="relative")
    await ed.check_error("read_engine_source", path="core/NoSuchFile.h", contains="search_engine_source")


async def test_search_engine_api_marks_lua_only(ed: Editor):
    """A Lua property C++ reaches through an accessor (Engine.deltatime: Engine::getDeltatime()) is
    marked cpp_callable false with the accessor (scripting catch-up, Doriax 755972f92)"""
    r = await ed.call("search_engine_api", query="deltatime", parent="Engine")
    hit = next((x for x in r["results"] if x.get("detail") == "Engine.deltatime"), None)
    ed.check(hit, f"Engine.deltatime found: {r['results'][:4]!r}")
    ed.check_eq(hit.get("cpp_callable"), False, f"{hit!r}")
    ed.check("Engine::getDeltatime()" in hit.get("cpp_note", ""), f"{hit!r}")
    zero = await ed.call("search_engine_api", query="ZERO", parent="Vector3")
    hit = next((x for x in zero["results"] if x.get("detail", "").startswith("Vector3.ZERO")), None)
    ed.check(hit and "cpp_callable" not in hit, f"Vector3::ZERO exists in C++: {hit!r}")


async def test_search_names_unlisted_tools(ed: Editor):
    """A search for what a hidden group's tools do names them and the tool_groups call: agents look for
    frame times in the API search and the Game view size in the settings search, and a client offers
    the model only the tools it listed (eval cases bad_profiling_group, bad_game_view_resolution)."""
    r = await ed.call("search_engine_api", query="profiler frame time draw calls stats")
    names = [hit["tool"] for hit in r.get("editor_tools", [])]
    ed.check("get_frame_times" in names, f"the profiling tools are named: {r.get('editor_tools')!r}")
    ed.check('tool_groups {"enable": ["profiling"]}' in r.get("next", ""), f"next names the call: {r.get('next')!r}")
    src = await ed.call("search_engine_source", query="drawCalls", max_results=5)
    ed.check("get_frame_times" in [hit["tool"] for hit in src.get("editor_tools", [])], f"source search too: {src!r}")
    settings = await ed.call("get_project_settings", search="game view resolution")
    ed.check("configure_game_view" in [hit["tool"] for hit in settings.get("editor_tools", [])],
             f"the settings search names the Game view tool (eval case bad_game_view_resolution): {settings!r}"[:400])
    plain = await ed.call("search_engine_api", query="Sprite setTexture")
    ed.check("editor_tools" not in plain, f"an API query names no tools: {plain.get('editor_tools')!r}")
    await ed.call("tool_groups", enable=["profiling"])
    listed = await ed.call("search_engine_api", query="profiler frame time draw calls stats")
    ed.check("profiling" not in [hit["group"] for hit in listed.get("editor_tools", [])],
             "a listed group's tools are found by the client's own tool search")
    await ed.call("tool_groups", disable=["profiling"])


async def test_prompts(ed: Editor):
    listed = await ed.session.list_prompts()
    names = {p.name for p in listed.prompts}
    ed.check_in("new_2d_game", names, "prompt listed")
    idea = "a nocturnal library maze"
    got = await ed.session.get_prompt("new_2d_game", {"idea": idea})
    text = got.messages[0].content.text
    ed.check_in(idea, text, "the prompt preserves the caller's complete game idea")


# ---------------------------------------------------------------- docs/mcp/TOOLS.md (S31 R22)

async def run_gen_check(out: Path) -> tuple[int, str]:
    proc = await asyncio.create_subprocess_exec(
        sys.executable, str(Path(gen_tool_docs.__file__)), "--check", "--out", str(out),
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT, env=dict(os.environ))
    text, _ = await asyncio.wait_for(proc.communicate(), 600)
    return proc.returncode, text.decode(errors="replace")


async def test_tool_docs_current(ed: Editor):
    code, out = await run_gen_check(TOOLS_MD)
    ed.check(code == 0, f"docs/mcp/TOOLS.md is stale: regenerate it with tools/mcp/gen_tool_docs.py\n{out[-2000:]}")
    text = gen_tool_docs.render(await ed.tools(groups="all"), ed.init_result.server_info, ed.init_result.protocol_version)
    version = ed.init_result.server_info.version
    ed.check(f"jasperengine-editor {version} (protocol" in text.splitlines()[2], f"the header names the version {version}")
    ed.check("Group: `core`" in text and "## Core" in text and "## HTML/CSS UI" in text, "groups and _meta sections")


async def test_tool_docs_red_control(ed: Editor):
    tmp = Path(tempfile.mkdtemp(prefix="jasper-tooldocs-"))
    try:
        copy = tmp / "TOOLS.md"
        shutil.copy(TOOLS_MD, copy)
        # a description edited in the editor without regenerating = the file no longer matches
        text = copy.read_text(encoding="utf-8")
        description = ed.tool("editor_status").description
        ed.check(description in text, "the description is in the file")
        copy.write_text(text.replace(description, description + " Changed."), encoding="utf-8")
        code, out = await run_gen_check(copy)
        ed.check(code == 1 and "is stale" in out and "Changed." in out, f"a stale file fails --check: {code}\n{out[-1500:]}")
        # the version in the header alone is not staleness
        lines = text.splitlines(keepends=True)
        lines[2] = lines[2].replace(" (protocol", "-other-version (protocol", 1)
        copy.write_text("".join(lines), encoding="utf-8")
        ed.check(gen_tool_docs.stale_diff(copy, text) == [], "a different version in the header is not stale")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


async def test_tool_reference_has_no_other_section(ed: Editor):
    """docs/mcp/TOOLS.md (tools/mcp/gen_tool_docs.py) files every tool under its module's section: none
    falls into "Other" (a tools/*.cpp missing from the generator's MODULES)."""
    from harness import REPO
    text = (REPO / "docs" / "mcp" / "TOOLS.md").read_text()
    ed.check("\n## Other\n" not in text and "[Other](#other)" not in text, "a tool is listed under Other: add its module to gen_tool_docs.py")
    ed.check("- [Vertex animation](#vertex-animation) (1): `bake_vertex_animation`" in text, "bake_vertex_animation under Vertex animation")


async def test_title_golden_ledger(ed: Editor):
    """S80 §7: every Title Art golden added or changed since main has its row in docs/titleart/goldens.md
    (tools/titleart/check_golden_ledger.py; its red controls are tools/ci/test_check_title_golden_ledger.py)."""
    proc = await asyncio.create_subprocess_exec(
        sys.executable, "-I", str(harness.REPO / "tools" / "titleart" / "check_golden_ledger.py"),
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
    text, _ = await asyncio.wait_for(proc.communicate(), 120)
    ed.check(proc.returncode == 0, f"the title golden ledger is incomplete:\n{text.decode(errors='replace')[-2000:]}")


async def test_title_library_docs_current(ed: Editor):
    """S84 R47, AC27: docs/mcp/guide/title-library.md is what `title-library gen-docs` makes from the
    built-in library (re-run it after a library change), and it is served per family."""
    proc = await asyncio.create_subprocess_exec(
        *harness.editor_command(), "title-library", "gen-docs", "--check", "--out", str(harness.REPO / "docs" / "mcp" / "guide" / "title-library.md"),
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT, cwd=str(harness.REPO))
    text, _ = await asyncio.wait_for(proc.communicate(), 300)
    ed.check(proc.returncode == 0, f"title-library.md is stale:\n{text.decode(errors='replace')[-1500:]}")
    listed = await ed.session.list_resources()
    uris = {str(r.uri) for r in listed.resources}
    ed.check(any(u.startswith("jasper://docs/title-library") for u in uris), "jasper://docs/title-library is served")
