"""Automatic pick-up of files edited on disk by the agent's own tools (McpAutoSync), and the
agent guide (server instructions) naming only tools and parameters that exist.

Before every tools/call the server scans the project for changed Lua / C++ / shader /
.material / image files and, when something changed, runs the sync_files pick-up and attaches
`external_changes: {changed, problems, ...}` to that call's result (structured + one text line).
"""

import re
import shutil
import statistics
import tempfile
import time
from pathlib import Path

from harness import Editor, default_fixture, find_key

TIMEOUT = 900

MOVER = """local Mover = {
    properties = {
        { name = "speed", displayName = "Speed", type = "float", default = 5.0 },
    }
}

function Mover:init()
    RegisterEngineEvent(self, "onUpdate")
end

function Mover:onUpdate()
end

return Mover
"""

SPEED_LINE = '{ name = "speed", displayName = "Speed", type = "float", default = 5.0 },'


def with_property(text: str, name: str, default: int) -> str:
    return text.replace(
        SPEED_LINE,
        SPEED_LINE + f'\n        {{ name = "{name}", displayName = "{name}", type = "int", default = {default} }},', 1)


def touch_write(path: Path, text: str) -> None:
    """Write so the mtime surely differs from the previous write (coarse-mtime filesystems)."""
    time.sleep(0.02)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


async def status_call(ed: Editor):
    """An unrelated tool call; returns the raw CallResult."""
    r = await ed.call_raw("editor_status")
    ed.check(not r.is_error, f"editor_status failed: {r.text[:300]}")
    return r


def external(r) -> dict | None:
    s = r.structured if isinstance(r.structured, dict) else {}
    return s.get("external_changes")


def prop(script: dict, name: str) -> dict | None:
    for p in script.get("properties", []):
        if p["name"] == name:
            return p
    return None


STATE: dict = {}


async def test_setup_script_no_report(ed: Editor):
    ed.require_tools("create_script", "create_entity", "list_scripts", "editor_status")
    await status_call(ed)  # settle whatever the editor did at startup
    made = await ed.call("create_entity", name="AutoSyncHost", type="cube")
    eid = find_key(made, "entity_id", "id")
    STATE["eid"] = eid
    made = await ed.call("create_script", kind="lua", name="Mover", content=MOVER, entity=eid)
    ed.check_eq(made["path"], "scripts/Mover.lua", "script path")
    # a file written by an MCP tool is not an external change
    r = await status_call(ed)
    ed.check(external(r) is None, f"create_script reported as external: {external(r)!r}")
    ed.check("External changes" not in r.text, f"text mentions external changes: {r.text[-300:]!r}")


async def test_external_property_picked_up(ed: Editor):
    path = ed.project / "scripts/Mover.lua"
    touch_write(path, with_property(MOVER, "jump", 3))
    r = await status_call(ed)  # unrelated call, no sync_files
    ext = external(r)
    ed.check(ext is not None, f"external edit not reported; text: {r.text[-300:]!r}")
    ed.check_in("scripts/Mover.lua", ext["changed"], "changed list")
    ed.check_eq(ext.get("problems", []), [], "problems of a valid edit")
    ed.check("scripts/Mover.lua" in r.text, f"text line lacks the path: {r.text[-300:]!r}")
    listed = await ed.call("list_scripts", entity=STATE["eid"])
    jump = prop(listed["scripts"][0], "jump")
    ed.check(jump is not None, f"jump not parsed: {listed['scripts'][0]!r}")
    ed.check_eq(jump["value"], 3, "new property's default")
    # reported once only
    r = await status_call(ed)
    ed.check(external(r) is None, f"reported twice: {external(r)!r}")


async def test_broken_lua_problem_with_line(ed: Editor):
    path = ed.project / "scripts/Mover.lua"
    good = with_property(MOVER, "jump", 3)
    touch_write(path, good.replace("function Mover:onUpdate()", "function Mover:onUpdate(", 1))
    r = await status_call(ed)
    ext = external(r)
    ed.check(ext is not None, "broken edit not reported")
    problems = [p for p in ext.get("problems", []) if p.get("path") == "scripts/Mover.lua"]
    ed.check(problems, f"no problem for the broken script: {ext!r}")
    ed.check(problems[0].get("line", 0) > 0, f"problem without a line: {problems[0]!r}")
    ed.check(problems[0].get("message"), "problem without a message")
    ed.check("scripts/Mover.lua:" in r.text, f"problem not in the text line: {r.text[-400:]!r}")
    touch_write(path, good)
    r = await status_call(ed)
    ext = external(r)
    ed.check(ext is not None and "scripts/Mover.lua" in ext["changed"], "fix not reported")
    ed.check_eq(ext.get("problems", []), [], "problems after the fix")


async def test_mcp_writes_not_reported(ed: Editor):
    ed.require_tools("write_file", "create_material", "fork_shader", "screenshot")
    r = await ed.call_raw("write_file", {"path": "scripts/Mover.lua", "content": with_property(MOVER, "jump", 4)})
    ed.check(not r.is_error, r.text)
    ed.check(external(r) is None, f"write_file's own write reported: {external(r)!r}")
    r = await status_call(ed)
    ed.check(external(r) is None, f"write_file reported as external: {external(r)!r}")

    await ed.call("create_material", path="materials/AutoSync.material", values={"baseColorFactor": [1, 0, 0, 1]})
    forked = await ed.call("fork_shader", entity=STATE["eid"], name="AutoSyncFork")
    STATE["frag"] = forked["frag_path"]
    await ed.screenshot(view="game", save_path="assets/autosync_shot.png")
    r = await status_call(ed)
    ed.check(external(r) is None, f"MCP-written files reported as external: {external(r)!r}")
    ed.check((ed.project / STATE["frag"]).exists(), "fork not written")


async def test_shader_fork_error_reported(ed: Editor):
    frag = ed.project / STATE["frag"]
    good = frag.read_text()
    idx = good.rfind("}")
    broken = good[:idx] + "    this_is_not_glsl += 1;\n" + good[idx:]
    touch_write(frag, broken)
    r = await status_call(ed)
    ext = external(r)
    ed.check(ext is not None, "shader edit not reported")
    ed.check_in(STATE["frag"], ext["changed"], "changed shader")
    problems = [p for p in ext.get("problems", []) if p.get("path", "").startswith(STATE["frag"].rsplit(".", 1)[0])]
    ed.check(problems, f"shader error not reported: {ext!r}")
    ed.check(any(p.get("line", 0) > 0 for p in problems), f"shader problem without a line: {problems!r}")
    touch_write(frag, good)
    r = await status_call(ed)
    ext = external(r)
    ed.check(ext is not None and STATE["frag"] in ext["changed"], "shader fix not reported")
    ed.check_eq([p for p in ext.get("problems", []) if "shader" in p.get("path", "") or p.get("path", "").endswith(".frag")],
                [], "shader problems after the fix")


async def test_added_removed_and_ignored_files(ed: Editor):
    extra = ed.project / "scripts/Extra.lua"
    touch_write(extra, "return {}\n")
    touch_write(ed.project / "assets/notes.txt", "not tracked\n")
    touch_write(ed.project / "Unlisted.scene", "not tracked\n")
    r = await status_call(ed)
    ext = external(r)
    ed.check(ext is not None, "added file not reported")
    ed.check_eq(ext["changed"], ["scripts/Extra.lua"], "only the tracked file")
    ed.check_eq(ext.get("added"), ["scripts/Extra.lua"], "added list")
    time.sleep(0.02)
    extra.unlink()
    r = await status_call(ed)
    ext = external(r)
    ed.check(ext is not None, "removed file not reported")
    ed.check_eq(ext["changed"], ["scripts/Extra.lua"], "changed on removal")
    ed.check_eq(ext.get("removed"), ["scripts/Extra.lua"], "removed list")
    # a new folder with a file, then the folder removed
    touch_write(ed.project / "scripts/sub/deep/Deep.lua", "return {}\n")
    r = await status_call(ed)
    ext = external(r)
    ed.check(ext is not None and ext.get("added") == ["scripts/sub/deep/Deep.lua"], f"file in new folder: {ext!r}")
    time.sleep(0.02)
    shutil.rmtree(ed.project / "scripts/sub")
    r = await status_call(ed)
    ext = external(r)
    ed.check(ext is not None and ext.get("removed") == ["scripts/sub/deep/Deep.lua"], f"removed folder: {ext!r}")


async def test_open_clean_buffer_reloads(ed: Editor):
    ed.require_tools("code_editor")
    await ed.call("code_editor", action="open", path="scripts/Mover.lua")
    path = ed.project / "scripts/Mover.lua"
    text = path.read_text() + "-- autosync marker\n"
    touch_write(path, text)
    r = await status_call(ed)
    ext = external(r)
    ed.check(ext is not None and "scripts/Mover.lua" in ext["changed"], f"edit of an open file not reported: {ext!r}")
    # save writes the buffer: it must hold the disk version by now
    await ed.call("code_editor", action="save", path="scripts/Mover.lua")
    ed.check("-- autosync marker" in path.read_text(), "the open buffer was not reloaded before the save")
    r = await status_call(ed)
    ed.check(external(r) is None, f"code_editor save reported as external: {external(r)!r}")
    await ed.call("code_editor", action="close", path="scripts/Mover.lua")


def p90(values: list) -> float:
    values = sorted(values)
    return values[int(len(values) * 0.9) - 1]


async def measure_no_change(ed: Editor, calls: int) -> dict:
    """The auto-sync cost per call when nothing changed, in CPU and wall ms: a read-only call
    pays the scan before it; a call that may write files also the settle scan after it."""
    out = {"read_cpu": [], "write_cpu": [], "read_wall": [], "write_wall": []}
    auto: dict = {}
    for _ in range(calls):
        r = await ed.call_raw("focus_window", {"window": "hierarchy"})   # may write: settles after
        ed.check(not r.is_error and external(r) is None, f"focus_window: {r.text[:300]}")
        r = await status_call(ed)  # read-only: its scan + focus_window's settle
        ed.check(external(r) is None, f"no change but reported: {external(r)!r}")
        auto = r.structured.get("auto_sync")
        ed.check(isinstance(auto, dict), f"editor_status lacks auto_sync: {list(r.structured)!r}")
        out["read_cpu"].append(auto["scan_cpu_ms"])
        out["write_cpu"].append(auto["scan_cpu_ms"] + auto["settle_cpu_ms"])
        out["read_wall"].append(auto["scan_ms"])
        out["write_wall"].append(auto["scan_ms"] + auto["settle_ms"])
    out["auto"] = auto
    return out


def describe(m: dict) -> str:
    return ", ".join(f"{k} median {statistics.median(m[k]):.3f} p90 {p90(m[k]):.3f} ms"
                     for k in ("read_cpu", "write_cpu", "read_wall", "write_wall"))


async def test_no_change_timing(ed: Editor):
    await status_call(ed)
    m = await measure_no_change(ed, 25)
    auto = m["auto"]
    print(f"no-change auto-sync, {auto['files']} files: {describe(m)}")
    ed.check(auto["complete"], f"the fixture is scanned whole every call: {auto!r}")
    ed.check(p90(m["write_cpu"]) < 5.0, f"no-change auto-sync too slow (CPU ms): {sorted(m['write_cpu'])!r}")
    ed.check(statistics.median(m["write_wall"]) < 5.0, f"no-change auto-sync too slow (wall ms): {sorted(m['write_wall'])!r}")


async def test_play_deferred_then_picked_up(ed: Editor):
    ed.require_tools("play", "get_play_state", "save_scene")
    await ed.call("save_scene")
    path = ed.project / "scripts/Mover.lua"
    base = path.read_text()
    r = await ed.call_raw("play", {"action": "start", "timeout_seconds": 300})
    ed.check(not r.is_error, f"play failed: {r.text[:500]}")
    try:
        touch_write(path, with_property(base, "dash", 7))
        r = await ed.call_raw("get_play_state")
        ext = external(r)
        ed.check(ext is not None, "edit during play not reported")
        ed.check_in("scripts/Mover.lua", ext["changed"], "changed during play")
        ed.check(ext.get("deferred"), f"no deferred note during play: {ext!r}")
        ed.check("play stops" in ext["deferred"], ext["deferred"])
    finally:
        await ed.call("play", action="stop")
    r = await status_call(ed)
    ext = external(r)
    ed.check(ext is not None and "scripts/Mover.lua" in ext["changed"], f"deferred change not picked up after stop: {ext!r}")
    ed.check("deferred" not in ext, f"still deferred after stop: {ext!r}")
    listed = await ed.call("list_scripts", entity=STATE["eid"])
    ed.check(prop(listed["scripts"][0], "dash") is not None, f"dash not parsed after stop: {listed['scripts'][0]!r}")


async def test_sync_files_still_reports(ed: Editor):
    """sync_files (forced rescan) keeps reporting what it picked up itself."""
    await ed.call("sync_files")  # everything up to here picked up
    path = ed.project / "scripts/Mover.lua"
    touch_write(path, path.read_text() + "-- sync\n")
    r = await ed.call("sync_files")
    ed.check_in("scripts/Mover.lua", r["changed"]["modified"], "sync_files lists the file")
    r = await status_call(ed)
    ed.check(external(r) is None, f"sync_files' pick-up reported again: {external(r)!r}")


async def test_large_project_incremental():
    """Thousands of files: the no-change scan stays cheap; edits are still found."""
    src = default_fixture()
    tmp = Path(tempfile.mkdtemp(prefix="jasper-autosync-big-"))
    try:
        proj = tmp / "project"
        shutil.copytree(src, proj, symlinks=True, ignore=shutil.ignore_patterns(".git"))
        for d in range(250):
            folder = proj / "assets" / "bulk" / f"d{d:03d}"
            folder.mkdir(parents=True)
            for f in range(100):
                (folder / f"f{f:03d}.png").write_bytes(b"")
        (proj / "assets/bulk/code").mkdir()
        for f in range(300):
            (proj / f"assets/bulk/code/S{f:03d}.lua").write_text("return {}\n")
        async with Editor(project=proj) as ed:
            await status_call(ed)
            m = await measure_no_change(ed, 20)
            auto = m["auto"]
            files = auto["files"]
            print(f"{files} files, {auto['checked']} checked per scan: {describe(m)}")
            ed.check(files >= 25000, f"files tracked: {files}")
            ed.check(not auto["complete"] and auto["checked"] < files, f"large projects are scanned incrementally: {auto!r}")
            ed.check(p90(m["read_cpu"]) < 5.0, f"large project scan too slow (CPU ms): {sorted(m['read_cpu'])!r}")
            ed.check(p90(m["write_cpu"]) < 8.0, f"large project scan + settle too slow (CPU ms): {sorted(m['write_cpu'])!r}")

            # a code edit is found on the next call
            touch_write(ed.project / "assets/bulk/code/S123.lua", "return { x = 1 }\n")
            r = await status_call(ed)
            ext = external(r)
            ed.check(ext is not None and ext["changed"] == ["assets/bulk/code/S123.lua"], f"code edit: {ext!r}")
            # a new file in a new folder too
            touch_write(ed.project / "assets/bulk/d100/new/N.lua", "return {}\n")
            r = await status_call(ed)
            ext = external(r)
            ed.check(ext is not None and ext.get("added") == ["assets/bulk/d100/new/N.lua"], f"new file: {ext!r}")
            # an image deep in the bulk: found once the round-robin reaches it (bounded)
            time.sleep(0.02)
            (ed.project / "assets/bulk/d200/f050.png").write_bytes(b"12345")
            found = None
            for i in range(600):
                r = await status_call(ed)
                if external(r):
                    found = (i, external(r))
                    break
            ed.check(found is not None, "image change never found")
            ed.check_eq(found[1]["changed"], ["assets/bulk/d200/f050.png"], "image change")
            print(f"image change found after {found[0] + 1} call(s)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------- the agent guide

# snake_case words in the guide that are neither tools nor tool parameters
NON_TOOL_WORDS = {
    "on_start",                 # "Lua has no ..." list (names from other engines)
    "external_changes",         # the field results carry after files changed on disk
}


def schema_words(schema, out: set) -> None:
    if isinstance(schema, dict):
        for key, value in schema.items():
            if key == "properties" and isinstance(value, dict):
                out.update(value.keys())
            if key == "enum" and isinstance(value, list):
                out.update(v for v in value if isinstance(v, str))
            schema_words(value, out)
    elif isinstance(schema, list):
        for item in schema:
            schema_words(item, out)


def tool_params(ed: Editor, name: str) -> set:
    words: set = set()
    schema_words(ed.tool(name).input_schema, words)
    return words


async def test_instructions_name_real_tools(ed: Editor):
    text = ed.instructions
    ed.check(len(text) > 1000, "no instructions")
    tools = ed.tool_names
    params: set = set()
    for t in ed.tool_list:
        schema_words(t.input_schema, params)
    # scene setting names (set_scene_settings values) count as parameters
    settings = await ed.call("get_scene_settings")

    def keys(value):
        if isinstance(value, dict):
            for k, v in value.items():
                params.add(k)
                keys(v)
        elif isinstance(value, list):
            for v in value:
                keys(v)
    keys(settings)

    # every snake_case identifier is a tool, a parameter / enum value, or a known non-tool word
    unknown = sorted({w for w in re.findall(r"(?<![\w./-])[a-z][a-z0-9]*(?:_[a-z0-9]+)+(?![\w(]|\.\w)", text)
                      if w not in tools and w not in params and w not in NON_TOOL_WORDS})
    ed.check(not unknown, f"guide names unknown tools/params: {unknown}")

    # tool + parameter pairs the guide teaches
    pairs = {
        "play": ["time_scale", "fixed_fps", "auto_pause", "action"],
        "wait": ["frames", "game_seconds", "until", "input"],
        "run_build": ["input", "screenshots_at"],
        "create_script": ["kind", "entity"],
        "check_script": ["path"],
        "check_shader": ["shader"],
        "sync_files": ["full"],
        "create_ui": ["root"],
        "create_ui_document": ["html", "template"],
        "inspect_ui": ["selector", "at"],
        "assert_ui": ["checks", "viewports"],
        "import_files": ["fonts", "icons", "from_document"],
    }
    for tool, names in pairs.items():
        ed.check(tool in tools, f"guide tool {tool} missing")
        ed.check(tool in text, f"guide does not mention {tool}")
        missing = [n for n in names if n not in tool_params(ed, tool)]
        ed.check(not missing, f"{tool} lacks params {missing}")
    ed.check("set" in tool_params(ed, "play"), "play action set")

    # what the Code section promises
    for phrase in (".jasperengine/engine-api", "editor_status", "check_script", "check_shader", "compile_scripts",
                   "jasperengine-editor check", "external_changes", "Engine.timeScale", "Engine.fixedDeltatime",
                   "Engine.gameTime", "Engine.frameCount"):
        ed.check(phrase in text, f"guide lacks {phrase!r}")
    status = await ed.call("editor_status")
    ed.check(Path(status["project"]["path"]).is_absolute(), f"editor_status project path: {status['project']!r}")
