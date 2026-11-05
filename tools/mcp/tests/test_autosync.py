"""External file pick-up through real MCP calls, using independently owned project inputs."""

import shutil
import tempfile
from pathlib import Path

from harness import Editor
from pngdiff import encode as encode_png

EDITOR = {"create": True, "name": "Automatic File Pickup"}
CI = True

MOVER = """local Mover = { properties = {
    { name = "speed", type = "float", default = 5.0 },
} }
function Mover:init()
    RegisterEngineEvent(self, "onUpdate")
end
function Mover:onUpdate()
end
return Mover
"""


def with_property(text: str, name: str, default: int) -> str:
    return text.replace('{ name = "speed", type = "float", default = 5.0 },',
                        '{ name = "speed", type = "float", default = 5.0 },\n'
                        f'    {{ name = "{name}", type = "int", default = {default} }},', 1)


def touch_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def external(result) -> dict | None:
    return result.structured.get("external_changes") if isinstance(result.structured, dict) else None


async def status_call(ed: Editor):
    result = await ed.call_raw("editor_status")
    ed.check(not result.is_error, result.text)
    return result


async def make_mover(ed: Editor, name: str):
    entity = (await ed.call("create_entity", name=name + "Host", type="cube"))["entity_id"]
    made = await ed.call("create_script", kind="lua", name=name, content=MOVER, entity=entity)
    return entity, made["path"]


async def test_large_project_incremental():
    """An existing, non-hot asset is eventually scanned; no external game fixture is needed."""
    with tempfile.TemporaryDirectory(prefix="jasper-autosync-scan-") as tmp:
        project = Path(tmp) / "project"
        async with Editor(create=True, name="Asset Scan") as seed:
            await seed.call("save_scene", path="Scan.scene")
            shutil.copytree(seed.project, project,
                            ignore=shutil.ignore_patterns(".jasperengine", "build", ".git"))
        folder = project / "assets" / "scan"
        folder.mkdir(parents=True)
        red = encode_png(1, 1, bytes((255, 0, 0, 255)))
        blue = encode_png(1, 1, bytes((0, 0, 255, 255)))
        for index in range(512):
            (folder / f"image{index:03d}.png").write_bytes(red)
        async with Editor(project=project) as ed:
            await status_call(ed)
            target = "assets/scan/image477.png"
            (ed.project / target).write_bytes(blue)
            for _ in range(128):
                report = external(await status_call(ed))
                if report and target in report["changed"]:
                    break
            else:
                ed.fail("a changed existing image was never picked up by the asset scan")


async def test_external_property_picked_up(ed: Editor):
    entity, rel = await make_mover(ed, "PropertyPickup")
    ed.check(external(await status_call(ed)) is None, "MCP's own script creation is not an external edit")
    path = ed.project / rel
    touch_write(path, with_property(MOVER, "jump", 3))
    report = external(await status_call(ed))
    ed.check(report is not None and rel in report["changed"], "unrelated calls report external script changes")
    listed = (await ed.call("list_scripts", entity=entity))["scripts"][0]
    jump = next((p for p in listed["properties"] if p["name"] == "jump"), None)
    ed.check(jump is not None and jump["value"] == 3, "the added property is parsed before the caller continues")
    ed.check(external(await status_call(ed)) is None, "an external edit is reported once")
    touch_write(path, with_property(path.read_text(), "dash", 7))
    forced = await ed.call("sync_files")
    ed.check_in(rel, forced["changed"]["modified"], "forced rescan reports its own pickup")
    changes = [c for c in forced["script_properties"] if c["script"] == "PropertyPickup"]
    ed.check(changes and "dash" in changes[0].get("added", []), "forced pickup reports its schema change")
    listed = (await ed.call("list_scripts", entity=entity))["scripts"][0]
    dash = next((p for p in listed["properties"] if p["name"] == "dash"), None)
    ed.check(dash is not None and dash["value"] == 7, "forced pickup applies the added property's authored default")
    ed.check(external(await status_call(ed)) is None, "forced pickup is not reported again")


async def test_broken_lua_problem_with_line(ed: Editor):
    _, rel = await make_mover(ed, "SyntaxPickup")
    path = ed.project / rel
    touch_write(path, "local broken = = 1\n" + MOVER)
    try:
        report = external(await status_call(ed))
        problems = [p for p in (report or {}).get("problems", []) if p.get("path") == rel]
        ed.check(problems, "external syntax errors reach the unrelated call")
        ed.check_eq(problems[0].get("line"), 1, "the error names the authored broken line")
    finally:
        touch_write(path, MOVER)
        repaired = external(await status_call(ed))
    ed.check(repaired is not None and rel in repaired["changed"], "the repaired source is noticed")
    ed.check(not [p for p in repaired.get("problems", []) if p.get("path") == rel], "the repaired source has no syntax error")


async def test_mcp_writes_not_reported(ed: Editor):
    entity = (await ed.call("create_entity", name="ToolOwnedFiles", type="cube"))["entity_id"]
    await ed.call("create_material", path="materials/Owned.material", values={"baseColorFactor": [1, 0, 0, 1]})
    fork = await ed.call("fork_shader", entity=entity, name="OwnedFork")
    await ed.screenshot(view="game", save_path="assets/owned-shot.png")
    ed.check((ed.project / fork["frag_path"]).is_file(), "shader fixture was actually written")
    ed.check((ed.project / "assets/owned-shot.png").is_file(), "screenshot fixture was actually written")
    ed.check(external(await status_call(ed)) is None, "tool-owned writes are not attributed to another actor")


async def test_shader_fork_error_reported(ed: Editor):
    entity = (await ed.call("create_entity", name="ShaderPickup", type="cube"))["entity_id"]
    fork = await ed.call("fork_shader", entity=entity, name="ExternalFork")
    valid = await ed.call("check_shader", shader=fork["custom_shader"])
    ed.check_eq(valid["failed"], 0, "the unmodified shader is valid")
    rel = fork["frag_path"]
    path = ed.project / rel
    good = path.read_text()
    broken = good.replace("void main()", "void main()\n{ undefined_autosync_symbol = 1.0; }\nvoid unused_autosync()", 1)
    ed.check(broken != good, "fixture has a GLSL entry point")
    touch_write(path, broken)
    try:
        report = external(await status_call(ed))
        problems = [p for p in (report or {}).get("problems", []) if p.get("path") == rel]
        ed.check(problems and any("undefined_autosync_symbol" in p.get("message", "") for p in problems),
                 "the unrelated call reports the compiler's actual symbol error")
        ed.check(any(p.get("line", 0) > 0 for p in problems), "the compiler error has a source location")
    finally:
        touch_write(path, good)
        repaired = external(await status_call(ed))
    ed.check(repaired is not None and rel in repaired["changed"], "fixed shader is picked up")
    ed.check(not [p for p in repaired.get("problems", []) if p.get("path") == rel], "the compiler error clears after repair")


async def test_added_removed_and_ignored_files(ed: Editor):
    extra = ed.project / "scripts/new-folder/Added.lua"
    touch_write(extra, "return {}\n")
    touch_write(ed.project / "assets/notes.txt", "not tracked\n")
    touch_write(ed.project / "Unlisted.scene", "not tracked\n")
    report = external(await status_call(ed))
    ed.check(report is not None, "a new nested script is discovered")
    ed.check_eq(report["changed"], ["scripts/new-folder/Added.lua"], "untracked/editor-owned extensions are excluded")
    ed.check_eq(report.get("added"), ["scripts/new-folder/Added.lua"])
    shutil.rmtree(extra.parent)
    removed = external(await status_call(ed))
    ed.check(removed is not None, "deleting a watched folder is discovered")
    ed.check_eq(removed.get("removed"), ["scripts/new-folder/Added.lua"])


async def test_open_clean_buffer_reloads(ed: Editor):
    _, rel = await make_mover(ed, "BufferPickup")
    path = ed.project / rel
    await ed.call("code_editor", action="open", path=rel)
    authored = MOVER + "-- external marker\n"
    touch_write(path, authored)
    report = external(await status_call(ed))
    ed.check(report is not None and rel in report["changed"], "the open file's edit is discovered")
    await ed.call("code_editor", action="save", path=rel)
    ed.check_eq(path.read_text(), authored, "saving a clean buffer cannot overwrite the newer disk edit")
    ed.check(external(await status_call(ed)) is None, "saving the refreshed buffer is not another external edit")
    await ed.call("code_editor", action="close", path=rel)


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
    try:
        ext = external(await status_call(ed))
        ed.check(ext is not None and frag_rel in ext["changed"], f"the .frag edit is reported: {ext!r}")
        problems = [p for p in ext.get("problems", []) if p.get("path") == frag_rel]
        ed.check(problems and all(p.get("code") == "HTMLUI-FX-0015" for p in problems), f"effect-shader-compile: {ext!r}")
        ed.check_eq([p.get("line") for p in problems], [line], f"once (both forms fail alike), at the .frag line: {problems!r}")
        # reloaded by this pick-up, or already by the editor's poller (ui_current)
        ed.check(any(r.get("html") == "assets/ui/auto.html" for r in ext.get("ui_reloaded", [])) or ext.get("ui_current", 0) >= 1,
                 f"its document reloaded: {ext!r}")

    finally:
        touch_write(ed.project / frag_rel, EFFECT_FRAG)
        ext = external(await status_call(ed))
    ed.check(ext is not None and not [p for p in ext.get("problems", []) if p.get("path") == frag_rel], f"fixed: {ext!r}")

    # while playing: the documents reload now (a fork shader would be deferred)
    await ed.call("save_scene", path="Autosync.scene")
    try:
        r = await ed.call_raw("play", {"action": "start", "auto_pause": True, "fixed_fps": 60, "timeout_seconds": 300})
        ed.check(not r.is_error, f"play failed: {r.text[:500]}")
        touch_write(ed.project / frag_rel, EFFECT_FRAG.replace("#ff8800", "#00ff88"))
        ext = external(await ed.call_raw("get_play_state"))
        ed.check(ext is not None and frag_rel in ext["changed"], f"reported during play: {ext!r}")
        ed.check("deferred" not in ext, f"not deferred: {ext!r}")
        ed.check(any(r.get("html") == "assets/ui/auto.html" for r in ext.get("ui_reloaded", [])) or ext.get("ui_current", 0) >= 1,
                 f"reloaded in play: {ext!r}")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
        touch_write(ed.project / frag_rel, EFFECT_FRAG)
        await status_call(ed)


async def test_play_deferred_then_picked_up(ed: Editor):
    """With Reload After Stopping Play (S23: the default reloads a Lua script in place, see
    test_script_reload.py) a script edited during Play is deferred"""
    entity, rel = await make_mover(ed, "DeferredPickup")
    await ed.call("save_scene", path="Autosync.scene")
    await ed.call("set_script_settings", changes_while_playing="after_stop")
    path = ed.project / rel
    try:
        await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
        touch_write(path, with_property(MOVER, "dash", 7))
        report = external(await ed.call_raw("get_play_state"))
        ed.check(report is not None and rel in report["changed"] and report.get("deferred"),
                 "script edits are reported but deferred while playing")
        during = (await ed.call("list_scripts", entity=entity))["scripts"][0]
        ed.check(not any(p["name"] == "dash" for p in during["properties"]), "the edit-time schema is not reparsed during Play")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    try:
        report = external(await status_call(ed))
        ed.check(report is not None and rel in report["changed"] and "deferred" not in report,
                 "the deferred edit is picked up after Play stops")
        after = (await ed.call("list_scripts", entity=entity))["scripts"][0]
        dash = next((p for p in after["properties"] if p["name"] == "dash"), None)
        ed.check(dash is not None and dash["value"] == 7, "the deferred property's authored default reaches the edit-time schema")
    finally:
        await ed.call("set_script_settings", changes_while_playing="reload")
