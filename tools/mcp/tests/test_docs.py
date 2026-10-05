"""W4 docs: jasper://guide and jasper://docs/* resources, the project resources, engine API search,
prompts."""

from harness import Editor, ProtocolError

DOCS = ["cpp-scripting", "lua-api", "physics", "prefabs", "shaders", "unity-glossary"]


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
    ed.check(any(getattr(t, "uri_template", getattr(t, "uriTemplate", None)) == "jasper://project/{+path}" for t in getattr(templates, "resource_templates", None) or templates.resourceTemplates), "project file template")


async def test_read_docs(ed: Editor):
    guide = await read_text(ed, "jasper://guide")
    ed.check(guide.startswith(ed.instructions.strip()[:200]), "guide starts with the initialize instructions")
    ed.check("jasper://docs/lua-api" in guide, "guide lists the docs")
    for name in DOCS:
        text = await read_text(ed, f"jasper://docs/{name}")
        ed.check(len(text) > 500 and text.lstrip().startswith("#"), f"{name} looks empty: {text[:100]!r}")
    lua = await read_text(ed, "jasper://docs/lua-api")
    ed.check("RegisterEngineEvent" in lua, "lua-api content")
    try:
        await ed.session.read_resource("jasper://docs/nope")
        ed.fail("reading a missing doc should fail")
    except Exception as e:  # McpError
        ed.check("lua-api" in str(e), f"missing doc error lists the documents: {e}")


async def test_project_resources(ed: Editor):
    agents = await read_text(ed, "jasper://project/AGENTS.md")
    ed.check("JasperEngine" in agents, "AGENTS.md content")
    lua = await read_text(ed, "jasper://project/scripts/GameController.lua")
    ed.check("GameController" in lua, "project file template")
    try:
        await ed.session.read_resource("jasper://project/../etc/passwd")
        ed.fail("escaping the project must fail")
    except Exception:
        pass


async def test_search_engine_api(ed: Editor):
    ed.require_tools("search_engine_api", "search_engine_source", "read_engine_source")
    r = await ed.call("search_engine_api", query="Rigidbody2D applyForce")
    ed.check(r["results"], "API results")
    ed.check(any("applyForce" in x["detail"] for x in r["results"]), f"applyForce found: {r['results'][:3]!r}")
    ed.check(r["docs"] and r["docs"][0]["doc"].startswith("jasper://docs/"), f"doc hits: {r['docs']!r}")
    src = await ed.call("search_engine_source", query="REGISTER_ENGINE_EVENT", max_results=5)
    ed.check(src, "engine source search")


async def test_prompts(ed: Editor):
    listed = await ed.session.list_prompts()
    names = {p.name for p in listed.prompts}
    ed.check_in("new_2d_game", names, "prompt listed")
    got = await ed.session.get_prompt("new_2d_game", {"idea": "a pong clone"})
    text = got.messages[0].content.text
    ed.check("pong" in text and "create_script" in text, text[:300])
