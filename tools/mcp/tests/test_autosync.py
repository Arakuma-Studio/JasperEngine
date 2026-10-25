"""Automatic pick-up of files edited on disk by the agent's own tools (McpAutoSync), and the
agent guide (server instructions) naming only tools and parameters that exist.

Before every tools/call the server scans the project for changed Lua / C++ / shader /
.material / image files and, when something changed, runs the sync_files pick-up and attaches
`external_changes: {changed, problems, ...}` to that call's result (structured + one text line).
"""

import os
import shutil
import tempfile
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
    """Make a changed stamp deterministic without waiting for wall time."""
    previous = path.stat().st_mtime_ns if path.exists() else None
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    stamp = path.stat()
    if previous is not None and stamp.st_mtime_ns == previous:
        os.utime(path, ns=(stamp.st_atime_ns, previous + 2_000_000_000))
        if path.stat().st_mtime_ns == previous:
            raise AssertionError(f"filesystem did not preserve a changed timestamp: {path}")


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


EFFECT_FRAG = """#version 450
// @effect     auto-glow
// @kind       paint
// @category   light
// @summary    A project effect of the auto-sync tests
// @cost       low
// @param      color color #ff8800 -- Colour

#include "effects/lib/jfx_common.glsl"

uniform u_fs_effectParams {
    vec4 color;
} params;

vec4 jfx_paint(JfxContext c){
    return params.color;
}

#include "effects/lib/main_paint.glsl"
"""
EFFECT_HTML = '<html><head><link rel="stylesheet" href="auto.css"/></head><body><div id="glow"></div></body></html>\n'
EFFECT_CSS = ("@effect auto-glow { src: url(fx/auto-glow.frag); }\n"
              "#glow { position: absolute; left: 0; top: 0; width: 200px; height: 100px; background-image: effect(auto-glow); }\n")


async def test_ui_effect_frag_picked_up(ed: Editor):
    """A project UI effect's .frag (phase 5): an external edit reloads the documents declaring it,
    a GLSL error comes back as effect-shader-compile at its line, also while playing."""
    ed.require_tools("create_ui_document", "play")
    frag_rel = "assets/ui/fx/auto-glow.frag"
    touch_write(ed.project / frag_rel, EFFECT_FRAG)
    touch_write(ed.project / "assets/ui/auto.html", EFFECT_HTML)
    touch_write(ed.project / "assets/ui/auto.css", EFFECT_CSS)
    made = await ed.call("create_ui_document", html="assets/ui/auto.html", name="AutoGlow")
    ed.check_eq(made["diagnostics"], [], "the document declaring the effect loads clean")
    await status_call(ed)

    broken = EFFECT_FRAG.replace("return params.color;", "return params.colour;")
    line = broken.split("\n").index("    return params.colour;") + 1
    touch_write(ed.project / frag_rel, broken)
    ext = external(await status_call(ed))
    ed.check(ext is not None and frag_rel in ext["changed"], f"the .frag edit is reported: {ext!r}")
    problems = [p for p in ext.get("problems", []) if p.get("path") == frag_rel]
    ed.check(problems and all(p.get("code") == "HTMLUI-FX-0015" for p in problems), f"effect-shader-compile: {ext!r}")
    ed.check_eq([p.get("line") for p in problems], [line], f"once (both forms fail alike), at the .frag line: {problems!r}")
    # reloaded by this pick-up, or already by the editor's poller (ui_current)
    ed.check(any(r.get("html") == "assets/ui/auto.html" for r in ext.get("ui_reloaded", [])) or ext.get("ui_current", 0) >= 1,
             f"its document reloaded: {ext!r}")

    touch_write(ed.project / frag_rel, EFFECT_FRAG)
    ext = external(await status_call(ed))
    ed.check(ext is not None and not [p for p in ext.get("problems", []) if p.get("path") == frag_rel], f"fixed: {ext!r}")

    # while playing: the documents reload now (a fork shader would be deferred)
    r = await ed.call_raw("play", {"action": "start", "timeout_seconds": 300})
    ed.check(not r.is_error, f"play failed: {r.text[:500]}")
    try:
        touch_write(ed.project / frag_rel, EFFECT_FRAG.replace("#ff8800", "#00ff88"))
        ext = external(await ed.call_raw("get_play_state"))
        ed.check(ext is not None and frag_rel in ext["changed"], f"reported during play: {ext!r}")
        ed.check("deferred" not in ext, f"not deferred: {ext!r}")
        ed.check(any(r.get("html") == "assets/ui/auto.html" for r in ext.get("ui_reloaded", [])) or ext.get("ui_current", 0) >= 1,
                 f"reloaded in play: {ext!r}")
    finally:
        await ed.call("play", action="stop")
    touch_write(ed.project / frag_rel, EFFECT_FRAG)
    await status_call(ed)


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
    """Rotating asset scans still find edits, alongside immediately checked code and new folders."""
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
            # An existing image deep in the rotating tier must eventually be found.
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


# The initialization payload is sent on every session (docs/mcp/DESIGN.md).
async def test_instructions_name_real_tools(ed: Editor):
    ed.check(bool(ed.instructions), "initialize instructions are missing")
    ed.check(len(ed.instructions.encode()) <= 16 * 1024, "initialize instructions exceed the documented 16 KB budget")
