"""Hooks, atomic failed edits, user-owned context files, runtime C++ state and project relocation.

Every workflow authors only its required files/entities. Input/clock semantics live in
public play/playctl owners; file synchronization and editor config placement remain here.
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
from project_fixture import write_project

EDITOR = {"create": True, "name": "Editor Workflow"}

TIMEOUT = 900


def check(cond, message: str) -> None:
    if not cond:
        raise TestFailure(message)


def check_eq(actual, expected, message: str = "") -> None:
    if actual != expected:
        raise TestFailure(f"{message + ': ' if message else ''}expected {expected!r}, got {actual!r}")


def touch_write(path: Path, text: str) -> None:
    """Write with a changed timestamp even on coarse timestamp filesystems."""
    previous = path.stat().st_mtime_ns if path.exists() else None
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    stamp = path.stat()
    if previous is not None and stamp.st_mtime_ns == previous:
        os.utime(path, ns=(stamp.st_atime_ns, previous + 2_000_000_000))
        check(path.stat().st_mtime_ns != previous, f"filesystem did not update timestamp: {path}")


@contextlib.contextmanager
def scratch_project():
    """An authored CLI-only project with an old, initially clean script."""
    tmp = Path(tempfile.mkdtemp(prefix="jasper-trialfix-"))
    try:
        project = tmp / "project"

        write_project(project)
        old(project / "scripts/Probe.lua")
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
    """An external edit introduces a known syntax error."""
    touch_write(path, "local Probe = {}\nlocal probe_value = = 1\nreturn Probe\n")


def old(path: Path) -> None:
    """Make a file look untouched for a day."""
    t = time.time() - 86400
    os.utime(path, (t, t))


# ---------------------------------------------------------------------------- P0-1 check --hook

async def test_hook_reports_shell_edits_once():
    with scratch_project() as project:
        game = project / "scripts/Probe.lua"
        # first run: nothing changed recently -> baseline, silent
        r = run_hook(project, bash_event(project, "ls scripts"))
        check_eq((r.returncode, r.stdout, r.stderr), (0, "", ""), "first run on an untouched project")
        check((project / ".jasperengine").is_dir(), "state directory")

        # an edit through the shell (no file_path in the hook input)
        break_line(game)
        r = run_hook(project, bash_event(project, "sed -i 's/= /= = /' scripts/Probe.lua"))
        check_eq(r.returncode, 2, f"a shell edit that breaks a Lua file blocks (exit 2): {r.stdout}{r.stderr}")
        check_eq(r.stdout, "", "hook mode writes to stderr only")
        check("scripts/Probe.lua:" in r.stderr and "error:" in r.stderr, r.stderr)

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
        check("scripts/Other.lua:2:" in r.stderr and "Probe" not in r.stderr, r.stderr)

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
        text = log.read_text()
        check("Bash" in text and "scripts/Probe.lua" in text and "exit 2" in text,
              "hook history exposes the changed file and failing outcome")


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


async def owned_scene(ed: Editor, name: str):
    await ed.call("set_project_settings", values={"editor.assets_dir": "assets", "scripting.lua_dir": "scripts"})
    return (await ed.call("create_scene", name=name, type="2D"))["scene"]["id"]


# ---------------------------------------------------------------------------- editor tools

async def test_placeholder_art_path_inside_assets(ed: Editor):
    scene = await owned_scene(ed, "Art Paths")
    r = await ed.call_raw("generate_placeholder_art", {"path": "sprites/shield_a.png", "shape": "circle"})
    ed.check(r.is_error, f"a path outside the assets dir is refused: {r.text[:300]}")
    ed.check("assets/sprites/shield_a.png" in r.text, f"the error names the path to use: {r.text[:300]}")
    ed.check(not (ed.project / "sprites").exists(), "nothing written outside the assets dir")

    made = await ed.call("generate_placeholder_art", path="assets/sprites/shield_b.png", shape="circle")
    check_eq(made["path"], "assets/sprites/shield_b.png", "returned path")
    ed.check((ed.project / "assets/sprites/shield_b.png").is_file(), "file under assets/")
    # the returned path is what the other tools take
    e = await ed.call("create_entity", scene=scene, type="sprite", name="ArtUser", select=False,
                      values={"mesh": {"submeshes[0].material.baseColorTexture": made["path"]}})
    ed.check(find_key(e, "entity_id") is not None, "texture accepted")
    await ed.call("undo")


async def test_create_entity_failure_says_nothing_created(ed: Editor):
    scene = await owned_scene(ed, "Atomic Entity")
    before = await ed.call("find_entities", scene=scene, name="Ghost*")
    check_eq(before["count"], 0, "precondition")
    r = await ed.call_raw("create_entity", {
        "scene": scene, "type": "sprite", "name": "Ghost Pickup", "select": False,
        "values": {"mesh": {"submeshes[0].material.baseColorTexture": "sprites/ghost.png"}}})
    ed.check(r.is_error, "bad texture path fails")
    text = r.text
    ed.check("baseColorTexture" in text and "sprites/ghost.png" in text, f"names the property and value: {text[:400]}")
    after = await ed.call("find_entities", scene=scene, name="Ghost*")
    check_eq(after["count"], 0, "really nothing created")


async def test_create_ui_failure_says_nothing_created(ed: Editor):
    ui_scene = (await ed.call("create_scene", name="Rollback UI", type="UI"))["scene"]["id"]
    r = await ed.call_raw("create_ui", {"scene": ui_scene, "root": {
        "type": "container", "name": "Ghost HUD", "children": [
            {"type": "image", "name": "Ghost Icon", "texture": "assets/sprites/does_not_exist.png"}]}})
    ed.check(r.is_error, "missing texture fails")
    ed.check("does_not_exist.png" in r.text, f"failure names the invalid texture: {r.text[:400]}")
    after = await ed.call("find_entities", scene=ui_scene, name="Ghost*")
    check_eq(after["count"], 0, "really nothing created")
    await ed.call("close_scene", scene=ui_scene, unsaved="discard")

    # AGENTS.md there and CLAUDE.md not (what a Play's generation can leave when it writes
    # AGENTS.md between the two deletes above): a read writes the missing CLAUDE.md, as a Play does
    ed.check(agents_file.is_file() and not claude_file.exists(), "AGENTS.md alone")
    await ed.session.read_resource("jasper://project/AGENTS.md")
    ed.check(claude_file.is_file() and "@AGENTS.md" in claude_file.read_text(), "the read writes the missing CLAUDE.md")


async def test_existing_agents_and_claude_md_never_rewritten(ed: Editor):
    """AGENTS.md and CLAUDE.md are written only when missing: once present they belong to the
    project, even when they still carry the old generated-section markers or stale text."""
    scene = await owned_scene(ed, "User Context")
    await ed.call("get_project_summary")
    agents_file = ed.project / "AGENTS.md"
    claude_file = ed.project / "CLAUDE.md"
    agents_text = ("# Team rules\n\n<!-- BEGIN JASPERENGINE GENERATED CONTEXT -->\nstale engine notes\n"
                   "<!-- END JASPERENGINE GENERATED CONTEXT -->\n\nKeep this.\n")
    claude_text = ("<!-- BEGIN JASPERENGINE GENERATED CONTEXT -->\n@OLD.md\n"
                   "<!-- END JASPERENGINE GENERATED CONTEXT -->\nMine.\n")
    agents_file.write_text(agents_text)
    claude_file.write_text(claude_text)
    # Resource generation and Play must both leave the user's context intact.
    resource = await ed.session.read_resource("jasper://project/AGENTS.md")
    check_eq("".join(getattr(c, "text", "") or "" for c in resource.contents), agents_text,
             "project context resource returns the user's rules")
    await ed.call("play", action="start", scene=scene, wait=True)
    await ed.call("play", action="stop")
    check_eq(agents_file.read_text(), agents_text, "existing AGENTS.md untouched")
    check_eq(claude_file.read_text(), claude_text, "existing CLAUDE.md untouched")

    # a missing CLAUDE.md is still created next to an existing AGENTS.md (when the project's files are generated)
    claude_file.unlink()
    await ed.call("play", action="start", scene=scene)
    await ed.call("play", action="stop")
    ed.check(claude_file.is_file() and "@AGENTS.md" in claude_file.read_text(), "missing CLAUDE.md created")
    check_eq(agents_file.read_text(), agents_text, "AGENTS.md still untouched")


async def test_hiding_an_already_hidden_parent_hides_children(ed: Editor):
    """A parent saved hidden (Inspector / set_properties write only its own flag) keeps visible children;
    hiding it again from a script must hide them, as hiding a visible parent does."""
    scene = await owned_scene(ed, "Visibility")
    parent = find_key(await ed.call("create_entity", scene=scene, type="sprite", name="VisParent", select=False),
                      "entity_id")
    await ed.call("create_entity", scene=scene, type="sprite", name="VisChild", parent=parent, select=False)
    await ed.call("create_entity", scene=scene, type="sprite", name="VisOther", select=False)
    await ed.call("set_properties", scene=scene, entity=parent, values={"transform": {"visible": False}})
    await ed.call("play", action="start", scene=scene)
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
    await ed.call("close_scene", scene=scene, unsaved="discard")


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
    scene = await owned_scene(ed, "CPP State")
    ed.require_tools("create_script", "compile_scripts")
    made = await ed.call("create_entity", scene=scene, name="Probe", type="empty", select=False)
    eid = find_key(made, "entity_id")
    await ed.call("create_script", kind="cpp_script", name="StateProbe", entity=eid, scene=scene,
                  header_content=CPP_HEADER, source_content=CPP_SOURCE)
    await ed.call("save_scene", scene=scene, path="CPPState.scene")
    started = await ed.call_raw("play", {"action": "start", "scene": scene, "timeout_seconds": 600})
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



    # A moved project's stale CMake cache must not prevent compiling new source.
    cache = ed.project / ".jasperengine/build/CMakeCache.txt"
    ed.check(cache.is_file(), "the successful script build creates a CMake cache")
    cache.write_text("\n".join("CMAKE_HOME_DIRECTORY:INTERNAL=/tmp/old-project-location"
                               if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=") else line
                               for line in cache.read_text().splitlines()) + "\n")
    header_path = ed.project / "scripts/StateProbe.h"
    header_path.write_text(CPP_HEADER.replace("speed = 3.5f", "speed = 4.5f"))
    rebuilt = await ed.call("compile_scripts", timeout_seconds=600, timeout=700)
    ed.check(rebuilt["success"], f"script compilation recovers after the cache's project path changes: {rebuilt}")
    await ed.call("play", action="start", scene=scene)
    try:
        current = await ed.call("get_runtime_entity", entity="Probe")
        check_eq(current["scripts"][0]["state"]["speed"], 4.5,
                 "Play loads the newly compiled module after relocating the CMake cache")
    finally:
        await ed.call("play", action="stop")


async def test_gitignore_covers_editor_files(ed: Editor):
    gitignore = ed.project / ".gitignore"
    gitignore.unlink(missing_ok=True)
    await ed.call("set_project_settings", values={"editor.version_control_files": True})
    ed.check(gitignore.is_file(), "the editor writes its .gitignore with the project")
    subprocess.run(["git", "init", "-q", str(ed.project)], check=True, capture_output=True)
    for path in ("settings.yaml", "editor_layout_v2.ini", "editor.log", ".jasperengine/private.dat"):
        result = subprocess.run(["git", "-C", str(ed.project), "check-ignore", "--no-index", "-q", path], capture_output=True)
        ed.check_eq(result.returncode, 0, f"Git ignores generated editor data: {path}")


# ---------------------------------------------------------------------------- sync_files (own editor)

async def test_sync_files_first_sync_reports_edits():
    with tempfile.TemporaryDirectory(prefix="jasper-sync-input-") as tmp:
        source = write_project(Path(tmp), scenes=True)
        async with Editor(project=source) as ed:
            await ed.call("editor_status")
            game = ed.project / "scripts/Probe.lua"
            # A: edit, then a session's first sync_files (with paths) is the first to see it
            touch_write(game, game.read_text() + "\n-- edit A\n")
            r = await ed.call("sync_files", paths=["scripts/Probe.lua"])
            check_eq(r["changed"]["modified"], ["scripts/Probe.lua"], "first sync lists the edit")
            st = await ed.call_raw("editor_status")
            ed.check(not (st.structured or {}).get("external_changes"), f"not reported twice: {st.structured.get('external_changes')!r}")
            # B: the auto-sync sees it first; sync_files then has nothing new
            touch_write(game, game.read_text() + "-- edit B\n")
            st = await ed.call_raw("editor_status")
            ext = (st.structured or {}).get("external_changes")
            ed.check(ext and ext["changed"] == ["scripts/Probe.lua"], f"auto-sync: {ext!r}")
            r = await ed.call("sync_files")
            check_eq(r["changed"], {"added": [], "modified": [], "removed": []}, "already picked up")
            # C: full sync without paths
            touch_write(game, game.read_text() + "-- edit C\n")
            r = await ed.call("sync_files")
            check_eq(r["changed"]["modified"], ["scripts/Probe.lua"], "full sync")
            # D: a new file, sync_files with its path and full: false
            touch_write(ed.project / "scripts/Foo.lua", "local Foo = { properties = {} }\nreturn Foo\n")
            r = await ed.call("sync_files", paths=["scripts/Foo.lua"], full=False)
            check_eq(r["changed"]["added"], ["scripts/Foo.lua"], "listed path, partial sync")
            st = await ed.call_raw("editor_status")
            ed.check(not (st.structured or {}).get("external_changes"), f"once: {st.structured.get('external_changes')!r}")


async def test_sync_files_first_call_after_edit_before_any_call():
    """The very first tools/call of the session is sync_files, the edit made before it."""
    with tempfile.TemporaryDirectory(prefix="jasper-sync-input-") as tmp:
        source = write_project(Path(tmp), scenes=True)
        async with Editor(project=source) as ed:
            game = ed.project / "scripts/Probe.lua"
            touch_write(game, game.read_text() + "\n-- before the first call\n")
            r = await ed.call("sync_files")
            check_eq(r["changed"]["modified"], ["scripts/Probe.lua"], "edit before the first call")


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
    source_dir = tempfile.TemporaryDirectory(prefix="jasper-cwd-input-")
    source = write_project(Path(source_dir.name), scenes=True)
    scratch = Scratch.make(project=source)
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
        source_dir.cleanup()
        if ok:
            scratch.remove()
        else:
            print(f"kept {scratch.root}")
