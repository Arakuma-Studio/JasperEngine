"""W4 docs: jasper://guide and jasper://docs/* resources, the project resources, engine API search,
prompts."""

from mcp.shared.exceptions import MCPError

from harness import Editor

DOCS = ["cpp-scripting", "data-safety", "html-ui", "lattice", "lua-api", "physics", "post-processing", "prefabs", "shaders", "undo", "unity-glossary"]


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
    ed.check(guide.startswith(ed.instructions.strip()[:200]), "guide starts with the initialize instructions")
    ed.check("jasper://docs/lua-api" in guide, "guide lists the docs")
    for name in DOCS:
        text = await read_text(ed, f"jasper://docs/{name}")
        ed.check(len(text) > 500 and text.lstrip().startswith("#"), f"{name} looks empty: {text[:100]!r}")
    # the big Lua reference is an index of section resources
    lua = await read_text(ed, "jasper://docs/lua-api")
    sections = [line.split("`")[1] for line in lua.splitlines() if line.startswith("- `jasper://docs/lua-api/")]
    ed.check(len(sections) > 10 and len(lua) < 12000, f"lua-api index: {len(sections)} sections, {len(lua)} chars")
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
    agents = await read_text(ed, "jasper://project/AGENTS.md")
    ed.check("JasperEngine" in agents, "AGENTS.md content")
    (ed.project / "AGENTS.md").unlink()  # a new project has none until play/build: written on read
    agents = await read_text(ed, "jasper://project/AGENTS.md")
    ed.check("JasperEngine Project Context" in agents and (ed.project / "AGENTS.md").exists(), "AGENTS.md generated")
    lua = await read_text(ed, "jasper://project/scripts/GameController.lua")
    ed.check("GameController" in lua, "project file template")
    try:
        await ed.session.read_resource("jasper://project/../etc/passwd")
    except MCPError:
        pass
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
    got = await ed.session.get_prompt("new_2d_game", {"idea": "a pong clone"})
    text = got.messages[0].content.text
    ed.check("pong" in text and "create_script" in text, text[:300])
