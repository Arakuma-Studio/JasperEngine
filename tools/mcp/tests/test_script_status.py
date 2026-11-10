"""The script compile status (S23 R17, R40; AC11): get_script_status (the toolbar badge's model).

A project without C++ scripts is "none"; adding / touching a .cpp makes it "stale"; a rebuild
goes "building" (fraction a number or null) then "up_to_date"; a bad edit ends "failed" with
findings that carry file and line. editor_status carries the same block as "scripts".
"""

import asyncio
import shutil
import time

from harness import Editor, skip

CI = True
EDITOR = {"create": True, "name": "Script Status CI"}

TIMEOUT = 2400

HEADER = """#pragma once

#include "ScriptBase.h"
#include "ScriptProperty.h"

class Spinner : public jasperengine::ScriptBase {
public:
    DPROPERTY("Speed")
    float speed = 1.0f;

    Spinner(jasperengine::Scene* scene, jasperengine::Entity entity);
    void onUpdate();
};
"""

SOURCE = """#include "Spinner.h"

using namespace jasperengine;

Spinner::Spinner(Scene* scene, Entity entity) : ScriptBase(scene, entity) {
    REGISTER_ENGINE_EVENT(onUpdate);
}

void Spinner::onUpdate() {
    speed += 0.0f;
}
"""


async def status_until(ed: Editor, states: set[str], seconds: float, seen: list[str]) -> dict:
    deadline = time.monotonic() + seconds
    while True:
        status = await ed.call("get_script_status")
        state = status["cpp"]["state"]
        if not seen or seen[-1] != state:
            seen.append(state)
        if state == "building":
            fraction = status["cpp"].get("fraction", None)
            ed.check(fraction is None or 0.0 <= fraction <= 1.0, f"building fraction is a number in [0, 1] or null: {status['cpp']}")
        if state in states or time.monotonic() > deadline:
            return status
        await asyncio.sleep(0.1)


async def test_cpp_status_states(ed: Editor):
    if not (shutil.which("cmake") and (shutil.which("c++") or shutil.which("g++") or shutil.which("clang++"))):
        skip("no C++ toolchain (cmake + compiler) on PATH")
    status = await ed.call("get_script_status")
    ed.check_eq(status["cpp"]["state"], "none", f"no C++ scripts: {status['cpp']}")
    ed.check_eq(status["lua"]["state"], "ok", "Lua ok")
    ed.check(status["summary"], "a one-line summary (the badge tooltip)")
    editor = await ed.call("editor_status")
    ed.check("scripts" in editor and editor["scripts"]["cpp"]["state"] == "none", f"editor_status.scripts: {editor.get('scripts')}")

    made = await ed.call("create_scene", name="Status", type="3D", path="scenes/Status")
    scene = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=scene, type="empty", name="Spinner")
    await ed.call("create_script", kind="cpp_script", name="Spinner", scene=scene, entity=ent["entity_id"],
                  header_content=HEADER, source_content=SOURCE)
    await ed.call("save_scene", scene=scene, path="scenes/Status.scene")
    seen: list[str] = []
    status = await status_until(ed, {"stale"}, 20, seen)
    ed.check_eq(status["cpp"]["state"], "stale", f"a new .cpp: stale ({seen})")

    r = await ed.call("reload_scripts", language="cpp", wait=False)
    ed.check(not r.get("notes") or not any("C++:" in n and "not" in n for n in r["notes"]), f"the rebuild started: {r}")
    status = await status_until(ed, {"up_to_date", "failed", "refused"}, 1500, seen)
    ed.check_eq(status["cpp"]["state"], "up_to_date", f"built: {status['cpp']} (states {seen})")
    ed.check("building" in seen, f"the build was seen building: {seen}")
    ed.check_eq(seen[seen.index("building"):][-1], "up_to_date", f"building -> up_to_date: {seen}")

    # a touch makes it stale again
    path = ed.project / "scripts/Spinner.cpp"
    path.write_text(path.read_text() + "\n// touched\n")
    seen = []
    status = await status_until(ed, {"stale"}, 20, seen)
    ed.check_eq(status["cpp"]["state"], "stale", f"touched: stale ({seen})")

    # a bad edit: failed, findings with file and line
    path.write_text(SOURCE.replace("speed += 0.0f;", "speed += notDeclaredAnywhere;"))
    r = await ed.call("reload_scripts", language="cpp", timeout_seconds=1500, timeout=1600)
    status = await ed.call("get_script_status")
    ed.check_eq(status["cpp"]["state"], "failed", f"a compile error: {status['cpp']}")
    located = [f for f in status["cpp"]["findings"] if f.get("file", "").endswith("Spinner.cpp") and f.get("line") == 10]
    ed.check(located, f"the error carries Spinner.cpp:10: {status['cpp']['findings']}")
    ed.check(status["cpp"]["using_last_good_build"], "the last good build is kept")
    console = await ed.call("read_console", limit=1000)
    linked = [e for e in console["entries"]
              if any(l["file"].endswith("Spinner.cpp") and l["line"] == 10 and l["in_project"] for l in e.get("locations", []))]
    ed.check(linked, "the Console's compile error links to the project's Spinner.cpp:10: " +
             str([(e.get("type"), e.get("message", "")[:160], e.get("locations")) for e in console["entries"]
                  if "Spinner" in e.get("message", "")][:8]))

    # fixed: up to date again
    path.write_text(SOURCE)
    await ed.call("reload_scripts", language="cpp", timeout_seconds=1500, timeout=1600)
    status = await ed.call("get_script_status")
    ed.check_eq(status["cpp"]["state"], "up_to_date", f"fixed: {status['cpp']}")
    ed.check_eq(status["cpp"]["findings"], [], "no findings left")


async def test_settings_round_trip(ed: Editor):
    status = await ed.call("set_script_settings", changes_while_playing="after_stop", reload_cpp_during_play=True)
    ed.check_eq(status["settings"]["changes_while_playing"], "after_stop", "policy set")
    ed.check_eq(status["settings"]["reload_cpp_during_play"], True, "C++ reload set")
    status = await ed.call("set_script_settings", changes_while_playing="reload", reload_cpp_during_play=False)
    ed.check_eq(status["settings"]["changes_while_playing"], "reload", "policy back")
    ed.check_eq(status["settings"]["reload_cpp_during_play"], False, "C++ reload back")
    bad = await ed.call("set_script_settings", expect_error=True, changes_while_playing="sometimes")
    ed.check_in("stop_and_reload", bad, "lists the policies")


async def test_scripting_tool_group(ed: Editor):
    """The S23 tools are the "scripting" group (S31): hidden from the default listing, listed once
    enabled, callable by name either way. The graph tools moved to "graphs" (S90 R32)."""
    names = ("reload_scripts", "get_script_status", "set_script_settings", "list_editor_menus", "run_editor_menu", "eval_expression")
    default = {t.name for t in await ed.tools(groups=None)}
    for name in names:
        ed.check(name not in default, f"{name} is hidden until scripting is enabled")
    value = await ed.call("eval_expression", expression="2^3^2")
    ed.check_eq(value["value"], 512, "a hidden group's tool is still callable")
    await ed.call("tool_groups", enable=["scripting"])
    try:
        listed = {t.name for t in await ed.tools(groups=None)}
        for name in names:
            ed.check(name in listed, f"{name} is listed once scripting is enabled")
    finally:
        await ed.call("tool_groups", disable=["scripting"])
