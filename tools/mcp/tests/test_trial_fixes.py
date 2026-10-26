"""Fixes from the end-to-end agent trials (real Claude Code / Codex sessions building games):

- `check --hook` also finds files an agent changed with a shell (Bash sed -i, heredocs): it checks
  the project files changed since its last run (state + log under .jasperengine/);
- generate_placeholder_art takes a project-relative path inside the assets dir, like every tool that
  references the file afterwards;
- a create_entity / create_ui that fails is rolled back, and its error says nothing was created;
- sync_files reports edits made before a session's first sync, exactly once (it or the auto-sync);
- the generated AGENTS.md describes the current workflow, and a CLAUDE.md imports it;
- wait: game_seconds / frames cap an `until` wait in game time;
- send_input: mouse down/up/click without x,y use the last pointer position;
- get_runtime_entity: C++ scripts' DPROPERTY values;
- the editor keeps settings.yaml / its layout ini / editor.log out of a project folder it runs in;
- a moved project's stale CMake cache is an info line, not three warnings.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

import harness
from harness import Editor, Scratch, TestFailure, find_key, launch_spec

TIMEOUT = 900


def check(cond, message: str) -> None:
    if not cond:
        raise TestFailure(message)


def check_eq(actual, expected, message: str = "") -> None:
    if actual != expected:
        raise TestFailure(f"{message + ': ' if message else ''}expected {expected!r}, got {actual!r}")


def touch_write(path: Path, text: str) -> None:
    """Write so the mtime surely differs from the previous write (coarse-mtime filesystems)."""
    time.sleep(0.02)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


@contextlib.contextmanager
def scratch_project():
    """A copy of the fixture (mtimes kept, so its files look old), without .git and .jasperengine/build."""
    tmp = Path(tempfile.mkdtemp(prefix="jasper-trialfix-"))
    try:
        project = tmp / "project"

        def ignore(directory: str, names: list[str]) -> set[str]:
            out = {".git"} & set(names)
            if Path(directory).name == ".jasperengine" and "build" in names:
                out.add("build")
            return out

        shutil.copytree(harness.default_fixture(), project, ignore=ignore, symlinks=True)
        yield project
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run_hook(project: Path, event: dict, *extra: str) -> subprocess.CompletedProcess:
    cmd = harness.editor_command() + ["check", "--project", str(project), "--hook", *extra]
    return subprocess.run(cmd, cwd=str(project), input=json.dumps(event), capture_output=True, text=True, timeout=120)


def bash_event(project: Path, command: str) -> dict:
    return {"hook_event_name": "PostToolUse", "tool_name": "Bash", "cwd": str(project),
            "tool_input": {"command": command, "description": "x"},
            "tool_response": {"stdout": "", "stderr": "", "interrupted": False}}


def break_line(path: Path) -> None:
    """What `sed -i 's/local BEST_SCORE_KEY = /local BEST_SCORE_KEY = = /'` does to GameController.lua."""
    text = path.read_text()
    check("local BEST_SCORE_KEY = " in text, "fixture changed: no BEST_SCORE_KEY line")
    touch_write(path, text.replace("local BEST_SCORE_KEY = ", "local BEST_SCORE_KEY = = ", 1))


def old(path: Path) -> None:
    """Make a file look untouched for a day."""
    t = time.time() - 86400
    os.utime(path, (t, t))


# ---------------------------------------------------------------------------- P0-1 check --hook

async def test_hook_reports_shell_edits_once():
    with scratch_project() as project:
        game = project / "scripts/GameController.lua"
        # first run: nothing changed recently -> baseline, silent
        r = run_hook(project, bash_event(project, "ls scripts"))
        check_eq((r.returncode, r.stdout, r.stderr), (0, "", ""), "first run on an untouched project")
        check((project / ".jasperengine").is_dir(), "state directory")

        # an edit through the shell (no file_path in the hook input)
        break_line(game)
        r = run_hook(project, bash_event(project, "sed -i 's/= /= = /' scripts/GameController.lua"))
        check_eq(r.returncode, 2, f"a shell edit that breaks a Lua file blocks (exit 2): {r.stdout}{r.stderr}")
        check_eq(r.stdout, "", "hook mode writes to stderr only")
        check("scripts/GameController.lua:" in r.stderr and "error:" in r.stderr, r.stderr)

        # reported once: an unrelated next call is silent
        r = run_hook(project, bash_event(project, "cat README.md"))
        check_eq((r.returncode, r.stderr), (0, ""), "already reported")

        # any tool name: an MCP call after another shell edit
        broken = project / "scripts/Other.lua"
        touch_write(broken, "local T = {}\nlocal x = = 1\nreturn T\n")
        event = {"hook_event_name": "PostToolUse", "tool_name": "mcp__jasper__editor_status", "cwd": str(project),
                 "tool_input": {}}
        r = run_hook(project, event)
        check_eq(r.returncode, 2, f"new broken file found whatever the tool: {r.stderr}")
        check("scripts/Other.lua:2:" in r.stderr and "GameController" not in r.stderr, r.stderr)

        # fixed: silent; an explicit file_path (Edit) is checked even when the scan saw it already
        touch_write(broken, "local T = {}\nreturn T\n")
        r = run_hook(project, bash_event(project, "python3 fix.py"))
        check_eq((r.returncode, r.stderr), (0, ""), "fixed file")
        edit = {"hook_event_name": "PostToolUse", "tool_name": "Edit", "cwd": str(project),
                "tool_input": {"file_path": str(game), "old_string": "a", "new_string": "b"}}
        r = run_hook(project, edit)
        check_eq(r.returncode, 2, f"Edit names the file: checked again: {r.stderr}")

        # deleted files don't break it
        broken.unlink()
        r = run_hook(project, bash_event(project, "rm scripts/Other.lua"))
        check_eq(r.returncode, 0, f"deleted file: {r.stderr}")

        # observable: one log line per run
        log = project / ".jasperengine/check-hook.log"
        check(log.is_file(), "check-hook.log written")
        lines = log.read_text().splitlines()
        check_eq(len(lines), 7, f"one line per hook run: {lines!r}")
        check("Bash" in lines[1] and "scripts/GameController.lua" in lines[1] and "exit 2" in lines[1], lines[1])
        check("exit 0" in lines[0] and "exit 0" in lines[2], f"{lines[0]!r} / {lines[2]!r}")


async def test_hook_first_run_checks_recent_files_only():
    with scratch_project() as project:
        stale = project / "scripts/Stale.lua"
        stale.write_text("local T = {}\nlocal x = = 1\nreturn T\n")
        old(stale)
        fresh = project / "scripts/Fresh.lua"
        fresh.write_text("local T = {}\n\nlocal y = = 2\nreturn T\n")
        r = run_hook(project, bash_event(project, "echo hi"))
        check_eq(r.returncode, 2, f"a file changed minutes before the first run is checked: {r.stderr}")
        check("scripts/Fresh.lua:3:" in r.stderr, r.stderr)
        check("Stale.lua" not in r.stderr, f"old problems are not the agent's: {r.stderr}")
        # touched later: reported then
        touch_write(stale, stale.read_text() + "\n")
        r = run_hook(project, bash_event(project, "echo again"))
        check_eq(r.returncode, 2, r.stderr)
        check("scripts/Stale.lua:2:" in r.stderr and "Fresh" not in r.stderr, r.stderr)


async def test_hook_shaders_and_cpp_files_tracked():
    with scratch_project() as project:
        r = run_hook(project, bash_event(project, "ls"))
        check_eq(r.returncode, 0, r.stderr)
        # a C++ file changed through the shell: built only with --cpp, so without it: silent
        touch_write(project / "scripts/Thing.cpp", "int broken( {\n")
        r = run_hook(project, bash_event(project, "cat > scripts/Thing.cpp"))
        check_eq((r.returncode, r.stderr), (0, ""), "C++ without --cpp is not built")
        log = (project / ".jasperengine/check-hook.log").read_text().splitlines()
        check("scripts/Thing.cpp" in log[-1], f"the log names the changed file: {log[-1]!r}")


# ---------------------------------------------------------------------------- editor tools

async def test_placeholder_art_path_inside_assets(ed: Editor):
    r = await ed.call_raw("generate_placeholder_art", {"path": "sprites/shield_a.png", "shape": "circle"})
    ed.check(r.is_error, f"a path outside the assets dir is refused: {r.text[:300]}")
    ed.check("assets/sprites/shield_a.png" in r.text, f"the error names the path to use: {r.text[:300]}")
    ed.check(not (ed.project / "sprites").exists(), "nothing written outside the assets dir")

    made = await ed.call("generate_placeholder_art", path="assets/sprites/shield_b.png", shape="circle")
    check_eq(made["path"], "assets/sprites/shield_b.png", "returned path")
    ed.check((ed.project / "assets/sprites/shield_b.png").is_file(), "file under assets/")
    # the returned path is what the other tools take
    e = await ed.call("create_entity", scene="Main Scene", type="sprite", name="ArtUser", select=False,
                      values={"mesh": {"submeshes[0].material.baseColorTexture": made["path"]}})
    ed.check(find_key(e, "entity_id") is not None, "texture accepted")
    await ed.call("undo")


async def test_create_entity_failure_says_nothing_created(ed: Editor):
    before = await ed.call("find_entities", scene="Main Scene", name="Ghost*")
    check_eq(before["count"], 0, "precondition")
    r = await ed.call_raw("create_entity", {
        "scene": "Main Scene", "type": "sprite", "name": "Ghost Pickup", "select": False,
        "values": {"mesh": {"submeshes[0].material.baseColorTexture": "sprites/ghost.png"}}})
    ed.check(r.is_error, "bad texture path fails")
    text = r.text
    ed.check("Nothing was created" in text, f"the error says the call was rolled back: {text[:400]}")
    ed.check("baseColorTexture" in text and "sprites/ghost.png" in text, f"names the property and value: {text[:400]}")
    ed.check("GameObject 1" not in text and "on GameObject" not in text,
             f"no id of a GameObject that no longer exists: {text[:400]}")
    after = await ed.call("find_entities", scene="Main Scene", name="Ghost*")
    check_eq(after["count"], 0, "really nothing created")


async def test_create_ui_failure_says_nothing_created(ed: Editor):
    await ed.call("open_scene", scene="HUD Scene.scene", close_previous=False)
    r = await ed.call_raw("create_ui", {"scene": "HUD Scene", "root": {
        "type": "container", "name": "Ghost HUD", "children": [
            {"type": "image", "name": "Ghost Icon", "texture": "assets/sprites/does_not_exist.png"}]}})
    ed.check(r.is_error, "missing texture fails")
    ed.check("Nothing was created" in r.text and "does_not_exist.png" in r.text, f"rollback said: {r.text[:400]}")
    after = await ed.call("find_entities", scene="HUD Scene", name="Ghost*")
    check_eq(after["count"], 0, "really nothing created")
    await ed.call("select_scene", scene="Main Scene")


async def test_agents_md_and_claude_md(ed: Editor):
    agents_file = ed.project / "AGENTS.md"
    claude_file = ed.project / "CLAUDE.md"
    for f in (agents_file, claude_file):
        if f.exists():
            f.unlink()
    got = await ed.session.read_resource("jasper://project/AGENTS.md")
    text = "".join(getattr(c, "text", "") or "" for c in got.contents)
    ed.check("followed by the `sync_files`" not in text, "old sync_files workflow")
    for needle in ("automatically", ".jasperengine/engine-api/lua", ".jasperengine/engine-api/lua/scripts",
                   "---@class", ": ScriptInstance", ".luarc.json", ".clangd", "jasperengine-editor check", "--hook"):
        ed.check(needle in text, f"AGENTS.md mentions {needle!r}")
    check_eq(text.count("# JasperEngine Project Context"), 1, "title once")
    ed.check(claude_file.is_file(), "CLAUDE.md generated next to AGENTS.md")
    claude = claude_file.read_text()
    ed.check("@AGENTS.md" in claude, claude)
    ed.check(len(claude) < 600, f"CLAUDE.md stays minimal: {len(claude)} chars")

    # a project's own CLAUDE.md is left alone
    claude_file.write_text("# My rules\nBe nice.\n")
    agents_file.unlink()
    await ed.session.read_resource("jasper://project/AGENTS.md")
    check_eq(claude_file.read_text(), "# My rules\nBe nice.\n", "user CLAUDE.md untouched")
    claude_file.unlink()


async def test_existing_agents_and_claude_md_never_rewritten(ed: Editor):
    """AGENTS.md and CLAUDE.md are written only when missing: once present they belong to the
    project, even when they still carry the old generated-section markers or stale text."""
    agents_file = ed.project / "AGENTS.md"
    claude_file = ed.project / "CLAUDE.md"
    agents_text = ("# Team rules\n\n<!-- BEGIN JASPERENGINE GENERATED CONTEXT -->\nstale engine notes\n"
                   "<!-- END JASPERENGINE GENERATED CONTEXT -->\n\nKeep this.\n")
    claude_text = ("<!-- BEGIN JASPERENGINE GENERATED CONTEXT -->\n@OLD.md\n"
                   "<!-- END JASPERENGINE GENERATED CONTEXT -->\nMine.\n")
    agents_file.write_text(agents_text)
    claude_file.write_text(claude_text)
    # every path that generates project files: the resource, a Play (writes the C++ sources) and the check CLI
    await ed.session.read_resource("jasper://project/AGENTS.md")
    await ed.call("play", action="start", scene="Main Scene", wait=True)
    await ed.call("play", action="stop")
    subprocess.run(harness.editor_command() + ["check", "--project", str(ed.project), "--lua", "--cpp"],
                   capture_output=True, timeout=600)
    check_eq(agents_file.read_text(), agents_text, "existing AGENTS.md untouched")
    check_eq(claude_file.read_text(), claude_text, "existing CLAUDE.md untouched")

    # a missing CLAUDE.md is still created next to an existing AGENTS.md (when the project's files are generated)
    claude_file.unlink()
    await ed.call("play", action="start", scene="Main Scene", wait=True)
    await ed.call("play", action="stop")
    ed.check(claude_file.is_file() and "@AGENTS.md" in claude_file.read_text(), "missing CLAUDE.md created")
    check_eq(agents_file.read_text(), agents_text, "AGENTS.md still untouched")


async def test_wait_until_capped_by_game_seconds(ed: Editor):
    await ed.call("play", action="start", scene="Main Scene", time_scale=1, fixed_fps=0)
    try:
        # condition true before the cap: it wins
        r = await ed.call("wait", until="Engine.gameTime ~= nil", game_seconds=5)
        check_eq(r["reason"], "condition", "condition first")
        # slow game: 2.6 game seconds take ~32 s of wall time; the cap is game time
        await ed.call("play", action="set", time_scale=0.08)
        r = await ed.call_raw("wait", {"until": "false", "game_seconds": 2.6})
        s = r.structured
        check_eq(s["reason"], "game_seconds", f"game time cap, not the wall-clock default: {r.text[:300]}")
        ed.check(s["game_seconds"] >= 2.59, f"ran the game time: {s['game_seconds']}")
        ed.check(r.is_error, "the condition never held: isError like a timeout")
        ed.check("still" in r.text, r.text[:300])
        # frames cap too
        await ed.call("play", action="set", time_scale=1)
        r = await ed.call_raw("wait", {"until": "false", "frames": 5})
        check_eq((r.structured["reason"], r.is_error), ("frames", True), r.text[:300])
        # an explicit wall-clock timeout still applies
        await ed.call("play", action="set", time_scale=0.01)
        r = await ed.call_raw("wait", {"until": "false", "game_seconds": 100, "timeout_seconds": 1})
        check_eq(r.structured["reason"], "timeout", r.text[:300])
    finally:
        await ed.call("play", action="set", time_scale=1)
        await ed.call("play", action="stop")


async def test_send_input_mouse_without_position(ed: Editor):
    await ed.call("play", action="start", scene="Main Scene")
    try:
        r = await ed.call_raw("send_input", {"events": [
            {"type": "mouse", "action": "move", "x": 300, "y": 200},
            {"type": "mouse", "action": "down", "button": "left"}], "frames_after": 1})
        ed.check(not r.is_error, f"mouse down without x,y uses the pointer: {r.text[:300]}")
        held = r.structured.get("held_input", {})
        ed.check("left" in json.dumps(held), f"left button held: {held!r}")
        pos = await ed.call("eval_lua", code="local p = Input.getMousePosition(); return {x = p.x, y = p.y}")
        down_at = find_key(pos, "values") or pos
        r = await ed.call_raw("send_input", {"events": [{"type": "mouse", "action": "up", "button": "left"},
                                                         {"type": "mouse", "action": "click", "button": "left"}]})
        ed.check(not r.is_error, f"up/click without x,y: {r.text[:300]}")
        pos2 = await ed.call("eval_lua", code="local p = Input.getMousePosition(); return {x = p.x, y = p.y}")
        check_eq(find_key(pos2, "values") or pos2, down_at, "the pointer did not jump")
    finally:
        await ed.call("send_input", events=[{"type": "release_all"}])
        await ed.call("play", action="stop")


async def test_hiding_an_already_hidden_parent_hides_children(ed: Editor):
    """A parent saved hidden (Inspector / set_properties write only its own flag) keeps visible children;
    hiding it again from a script must hide them, as hiding a visible parent does."""
    parent = find_key(await ed.call("create_entity", scene="Main Scene", type="sprite", name="VisParent", select=False),
                      "entity_id")
    await ed.call("create_entity", scene="Main Scene", type="sprite", name="VisChild", parent=parent, select=False)
    await ed.call("create_entity", scene="Main Scene", type="sprite", name="VisOther", select=False)
    await ed.call("set_properties", scene="Main Scene", entity=parent, values={"transform": {"visible": False}})
    await ed.call("play", action="start", scene="Main Scene")
    try:
        r = await ed.call("eval_lua", code="return object('VisChild').visible")
        check_eq(find_key(r, "values"), [True], "precondition: the saved child is visible")
        await ed.call("eval_lua", code="object('VisParent').visible = false")
        await ed.call("wait", frames=2)
        r = await ed.call("eval_lua", code="return object('VisChild').visible")
        check_eq(find_key(r, "values"), [False], "hiding the hidden parent hides its child")
        # showing a visible parent again does not re-show a child hidden on its own
        await ed.call("eval_lua", code="object('VisParent').visible = true")
        await ed.call("wait", frames=2)
        await ed.call("eval_lua", code="object('VisChild').visible = false")
        await ed.call("wait", frames=2)
        await ed.call("eval_lua", code="object('VisParent').visible = true")
        await ed.call("wait", frames=2)
        r = await ed.call("eval_lua", code="return object('VisChild').visible")
        check_eq(find_key(r, "values"), [False], "visible = true on a visible parent leaves its children as they are")
    finally:
        await ed.call("play", action="stop")
    await ed.call("undo")
    await ed.call("undo")
    await ed.call("undo")
    await ed.call("undo")


CPP_HEADER = """#pragma once
#include "ScriptBase.h"
#include "ScriptProperty.h"
#include "Sprite.h"
#include <string>

class StateProbe : public jasperengine::ScriptBase {
public:
    DPROPERTY("Speed")
    float speed = 3.5f;
    DPROPERTY("Count")
    int count = 7;
    DPROPERTY("Label")
    std::string label = "hi";
    DPROPERTY("Alive")
    bool alive = true;
    DPROPERTY("Offset")
    jasperengine::Vector3 offset = jasperengine::Vector3(1, 2, 3);
    DPROPERTY("Target")
    jasperengine::Sprite* target = nullptr;

    StateProbe(jasperengine::Scene* scene, jasperengine::Entity entity);
    void onUpdate();
};
"""

CPP_SOURCE = """#include "StateProbe.h"
#include "Engine.h"

using namespace jasperengine;

StateProbe::StateProbe(Scene* scene, Entity entity): ScriptBase(scene, entity) {
    REGISTER_ENGINE_EVENT(onUpdate);
}

void StateProbe::onUpdate() {
    count += 1;
}
"""


async def test_runtime_cpp_script_state(ed: Editor):
    ed.require_tools("create_script", "compile_scripts")
    made = await ed.call("create_entity", scene="Main Scene", name="Probe", type="empty", select=False)
    eid = find_key(made, "entity_id")
    await ed.call("create_script", kind="cpp_script", name="StateProbe", entity=eid, scene="Main Scene",
                  header_content=CPP_HEADER, source_content=CPP_SOURCE)
    await ed.call("save_scene", scene="Main Scene")
    started = await ed.call_raw("play", {"action": "start", "scene": "Main Scene", "timeout_seconds": 600})
    ed.check(not started.is_error, f"play with the C++ script: {started.text[:1500]}")
    try:
        await ed.call("wait", frames=3)
        live = await ed.call("get_runtime_entity", entity="Probe")
        scripts = live["scripts"]
        check_eq(len(scripts), 1, f"one script: {scripts!r}")
        state = scripts[0].get("state")
        ed.check(isinstance(state, dict) and state, f"C++ DPROPERTY values exposed: {scripts!r}")
        check_eq(state["speed"], 3.5, "float")
        ed.check(isinstance(state["count"], int) and state["count"] > 7, f"live int (updated each frame): {state['count']!r}")
        check_eq(state["label"], "hi", "string")
        check_eq(state["alive"], True, "bool")
        check_eq(state["offset"], [1, 2, 3], "vector3")
        ed.check("target" in state and state["target"] is None, f"null pointer property: {state.get('target')!r}")
    finally:
        await ed.call("play", action="stop")


async def test_stale_cmake_cache_is_info(ed: Editor):
    """The build of the previous test now runs with a CMake cache from another folder."""
    cache = ed.project / ".jasperengine/build/CMakeCache.txt"
    ed.check(cache.is_file(), "the C++ build made a CMake cache")
    text = cache.read_text()
    lines = [("CMAKE_HOME_DIRECTORY:INTERNAL=/tmp/some/old/place" if l.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=") else l)
             for l in text.splitlines()]
    cache.write_text("\n".join(lines) + "\n")
    cursor = (await ed.call("read_console", limit=1))["next_cursor"]
    r = await ed.call_raw("compile_scripts", {"timeout_seconds": 600})
    ed.check(not r.is_error, f"compile after the move: {r.text[:1500]}")
    console = await ed.call("read_console", since=cursor, limit=500)
    entries = console["entries"]
    moved = [e for e in entries if "Project path changed" in e.get("message", "")]
    check_eq(len(moved), 1, f"one line about the moved project: {moved!r}")
    check_eq(moved[0].get("type"), "info", f"info, not a warning: {moved[0]!r}")
    ed.check("/tmp/some/old/place" in moved[0]["message"], moved[0]["message"])
    ed.check(not [e for e in entries if e.get("type") == "warning" and ("Previous:" in e.get("message", "") or
                                                                         "Current:" in e.get("message", ""))],
             "no leftover warning lines")


async def test_gitignore_covers_editor_files(ed: Editor):
    gitignore = ed.project / ".gitignore"
    gitignore.unlink(missing_ok=True)
    await ed.call("set_project_settings", values={"editor.version_control_files": True})
    ed.check(gitignore.is_file(), "the editor writes its .gitignore with the project")
    text = gitignore.read_text()
    for pattern in ("/settings.yaml", "/editor_layout_v2.ini", "*.log", ".jasperengine/"):
        ed.check(pattern in text.splitlines(), f".gitignore ignores {pattern}: {text}")


# ---------------------------------------------------------------------------- sync_files (own editor)

async def test_sync_files_first_sync_reports_edits():
    async with Editor() as ed:
        await ed.call("editor_status")
        game = ed.project / "scripts/GameController.lua"
        # A: edit, then a session's first sync_files (with paths) is the first to see it
        touch_write(game, game.read_text() + "\n-- edit A\n")
        r = await ed.call("sync_files", paths=["scripts/GameController.lua"])
        check_eq(r["changed"]["modified"], ["scripts/GameController.lua"], "first sync lists the edit")
        st = await ed.call_raw("editor_status")
        ed.check(not (st.structured or {}).get("external_changes"), f"not reported twice: {st.structured.get('external_changes')!r}")
        # B: the auto-sync sees it first; sync_files then has nothing new
        touch_write(game, game.read_text() + "-- edit B\n")
        st = await ed.call_raw("editor_status")
        ext = (st.structured or {}).get("external_changes")
        ed.check(ext and ext["changed"] == ["scripts/GameController.lua"], f"auto-sync: {ext!r}")
        r = await ed.call("sync_files")
        check_eq(r["changed"], {"added": [], "modified": [], "removed": []}, "already picked up")
        # C: full sync without paths
        touch_write(game, game.read_text() + "-- edit C\n")
        r = await ed.call("sync_files")
        check_eq(r["changed"]["modified"], ["scripts/GameController.lua"], "full sync")
        # D: a new file, sync_files with its path and full: false
        touch_write(ed.project / "scripts/Foo.lua", "local Foo = { properties = {} }\nreturn Foo\n")
        r = await ed.call("sync_files", paths=["scripts/Foo.lua"], full=False)
        check_eq(r["changed"]["added"], ["scripts/Foo.lua"], "listed path, partial sync")
        st = await ed.call_raw("editor_status")
        ed.check(not (st.structured or {}).get("external_changes"), f"once: {st.structured.get('external_changes')!r}")


async def test_sync_files_first_call_after_edit_before_any_call():
    """The very first tools/call of the session is sync_files, the edit made before it."""
    async with Editor() as ed:
        game = ed.project / "scripts/GameController.lua"
        touch_write(game, game.read_text() + "\n-- before the first call\n")
        r = await ed.call("sync_files")
        check_eq(r["changed"]["modified"], ["scripts/GameController.lua"], "edit before the first call")


async def test_opening_a_project_writes_no_generated_file():
    """Opening a project writes nothing, so a sync never lists the editor's own CMakeLists.txt: one an
    older editor generated stays as it is until a setting the build bakes in changes."""
    with scratch_project() as source:
        stale = (source / "CMakeLists.txt").read_text() + "# generated by an older editor\n"
        (source / "CMakeLists.txt").write_text(stale)
        async with Editor(project=source) as ed:
            cmake = ed.project / "CMakeLists.txt"
            await ed.call("editor_status")
            await ed.call("wait", frames=10)
            check_eq(cmake.read_text(), stale, "CMakeLists.txt after opening the project")
            r = await ed.call("sync_files")
            ed.check("CMakeLists.txt" not in r["changed"]["modified"], f"the editor's own write: {r['changed']!r}")
            # a threading setting is baked into the standalone build: its sources are generated again
            await ed.call("set_project_settings", values={"player.threading.workers": 3})
            await ed.call("wait", frames=10)
            ed.check(cmake.read_text() != stale, "CMakeLists.txt is generated again after a threading change")


# ---------------------------------------------------------------------------- editor working files

async def test_editor_files_stay_out_of_project_cwd():
    """`jasperengine-editor mcp` started in the project folder (an MCP client's cwd) with no settings.yaml
    there: settings.yaml, the layout ini and editor.log go to the user config dir."""
    scratch = Scratch.make()
    ok = False
    try:
        (scratch.root / "settings.yaml").unlink()
        argv, env, _ = launch_spec(scratch)
        with open(scratch.stderr_log, "w") as err:
            proc = await asyncio.create_subprocess_exec(
                *argv, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=err,
                env=env, cwd=str(scratch.project), start_new_session=True, limit=64 * 1024 * 1024)

            async def send(msg):
                proc.stdin.write((json.dumps(msg) + "\n").encode())
                await proc.stdin.drain()

            async def response(id_, timeout=180):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    line = await asyncio.wait_for(proc.stdout.readline(), timeout=deadline - time.monotonic())
                    if not line:
                        break
                    with contextlib.suppress(ValueError):
                        msg = json.loads(line)
                        if msg.get("id") == id_:
                            return msg
                raise TestFailure("no response from the editor: " + scratch.stderr_log.read_text()[-2000:])

            await send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}}})
            await response(1)
            await send({"jsonrpc": "2.0", "method": "notifications/initialized"})
            await send({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                        "params": {"name": "editor_status", "arguments": {}}})
            r = await response(2)
            check("result" in r, f"editor_status: {r!r}")
            await asyncio.sleep(6)  # ImGui saves its ini every 5 s
            proc.stdin.close()
            try:
                await asyncio.wait_for(proc.wait(), 60)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.wait()
        leaked = [n for n in ("settings.yaml", "editor_layout_v2.ini", "editor.log") if (scratch.project / n).exists()]
        check_eq(leaked, [], "editor working files written into the project folder")
        config = scratch.home / ".config"
        found = sorted(p.name for p in config.rglob("*") if p.name in ("settings.yaml", "editor.log"))
        check_eq(found, ["editor.log", "settings.yaml"], "they are in the user config dir")
        ok = True
    finally:
        if ok:
            scratch.remove()
        else:
            print(f"kept {scratch.root}")
