"""W4 docs: jasper://guide and jasper://docs/* resources, the project resources, engine API search,
prompts."""

from mcp.shared.exceptions import MCPError

from harness import Editor

EDITOR = {"create": True, "name": "MCP Resources"}

DOCS = ["animation", "camera", "cpp-scripting", "html-ui", "input", "lattice", "lines-trails", "lua-api", "physics", "post-processing", "prefabs", "shaders", "sprite-normals", "unity-glossary", "vertex-animation"]


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


async def test_prompts(ed: Editor):
    listed = await ed.session.list_prompts()
    names = {p.name for p in listed.prompts}
    ed.check_in("new_2d_game", names, "prompt listed")
    idea = "a nocturnal library maze"
    got = await ed.session.get_prompt("new_2d_game", {"idea": idea})
    text = got.messages[0].content.text
    ed.check_in(idea, text, "the prompt preserves the caller's complete game idea")


async def test_tool_reference_has_no_other_section(ed: Editor):
    """docs/mcp/TOOLS.md (tools/mcp/gen_tool_docs.py) files every tool under its module's section: none
    falls into "Other" (a tools/*.cpp missing from the generator's MODULES)."""
    from harness import REPO
    text = (REPO / "docs" / "mcp" / "TOOLS.md").read_text()
    ed.check("\n## Other\n" not in text and "[Other](#other)" not in text, "a tool is listed under Other: add its module to gen_tool_docs.py")
    ed.check("- [Vertex animation](#vertex-animation) (1): `bake_vertex_animation`" in text, "bake_vertex_animation under Vertex animation")
