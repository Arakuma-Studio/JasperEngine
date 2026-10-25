"""C++ scripts reloaded during Play (S23 R10-R16, AC8, AC10).

With Preferences > Scripting > Reload C++ Scripts During Play on, an edited Mover.cpp is rebuilt
while the game plays and swapped in: a DPROPERTY set at runtime survives, a counter saved by
saveReloadState survives, and the new constructor sees ScriptBase::isReloading(). A compile
error keeps the old library (kept_old, the badge "failed"). Three reloads then Stop: no crash.

The toolchain fingerprint: a library whose marker says iteratorDebugLevel=2
(JASPER_TEST_FINGERPRINT_OVERRIDE) is refused before any of its code runs, naming the field;
a soft difference (compilerMinor) plays with a warning.
"""

import shutil

from harness import Editor, skip

CI = True
EDITOR = {"create": True, "name": "Cpp Reload CI"}

TIMEOUT = 2400

HEADER = """#pragma once

#include "JasperEngine.h"

class Mover : public jasperengine::ScriptBase {
public:
    DPROPERTY("Speed")
    float speed = 1.0f;

    int counter = 0;

    Mover(jasperengine::Scene* scene, jasperengine::Entity entity);
    ~Mover() override;
    void onUpdate();
    void saveReloadState(jasperengine::ScriptReloadState& state) const override;
    void loadReloadState(const jasperengine::ScriptReloadState& state) override;
};
"""

SOURCE = """#include "Mover.h"

using namespace jasperengine;

Mover::Mover(Scene* scene, Entity entity) : ScriptBase(scene, entity) {
    REGISTER_ENGINE_EVENT(onUpdate);
    Log::info("Mover constructed version=%d reloading=%d", {version}, ScriptBase::isReloading() ? 1 : 0);
}

Mover::~Mover() {
}

void Mover::onUpdate() {
    counter++;
    if (counter % 30 == 0) Log::info("Mover tick version=%d counter=%d speed=%.1f", {version}, counter, speed);
}

void Mover::saveReloadState(ScriptReloadState& state) const {
    state.values["counter"] = counter;
}

void Mover::loadReloadState(const ScriptReloadState& state) {
    auto it = state.values.find("counter");
    if (it != state.values.end() && std::holds_alternative<int>(it->second)) counter = std::get<int>(it->second);
    Log::info("Mover loaded counter=%d speed=%.1f", counter, speed);
}
"""

S: dict = {}


def toolchain():
    if not (shutil.which("cmake") and (shutil.which("c++") or shutil.which("g++") or shutil.which("clang++"))):
        skip("no C++ toolchain (cmake + compiler) on PATH for the editor")


async def make_project(ed: Editor) -> dict:
    made = await ed.call("create_scene", name="Movers", type="3D", path="scenes/Movers")
    scene = made["scene"]["id"]
    ent = await ed.call("create_entity", scene=scene, type="empty", name="Mover")
    await ed.call("create_script", kind="cpp_script", name="Mover", scene=scene, entity=ent["entity_id"])
    await ed.call("write_file", path="scripts/Mover.h", content=HEADER)
    await ed.call("write_file", path="scripts/Mover.cpp", content=SOURCE.replace("{version}", "1"))
    await ed.call("sync_files", paths=["scripts/Mover.h", "scripts/Mover.cpp"])
    await ed.call("save_scene", scene=scene)
    await ed.call("save_project")
    return {"scene": scene, "entity": ent["entity_id"]}


async def setup(ed: Editor) -> dict:
    if S.get("ed") is ed:
        return S
    S.clear()
    S["ed"] = ed
    S.update(await make_project(ed))
    return S


async def logged(ed: Editor, text: str) -> list[str]:
    return [e["message"] for e in await ed.console_since(None, limit=2000) if text in e.get("message", "")]


async def play(ed: Editor, scene) -> dict:
    r = await ed.call_raw("play", {"action": "start", "scene": scene}, timeout=1200)
    if r.is_error and ("ABI" in r.text or "build tools" in r.text):
        skip("the C++ build can't run in this environment: " + r.text[:400])
    return r


async def test_cpp_reload_keeps_state(ed: Editor):
    toolchain()
    s = await setup(ed)
    await ed.call("script_status", reload_cpp_during_play=True, changes_while_playing="reload")
    await ed.console_since(None, limit=1)
    r = await play(ed, s["scene"])
    ed.check(not r.is_error, f"play starts: {r.text[:800]}")
    await ed.call("wait", frames=40)
    ed.check(await logged(ed, "Mover constructed version=1 reloading=0"), "the first library runs")
    await ed.call("set_script_properties", scene=s["scene"], entity=s["entity"], script="Mover", values={"speed": 2.5})
    await ed.call("wait", frames=40)

    await ed.call("write_file", path="scripts/Mover.cpp", content=SOURCE.replace("{version}", "2"))
    r = await ed.call("reload_scripts", language="cpp", timeout_seconds=1200, timeout=1300)
    reloaded = [x for x in r["reloaded"] if x["language"] == "cpp"]
    ed.check(reloaded, f"the C++ scripts reloaded: {r}")
    ed.check("Mover" in reloaded[0].get("classes", []), f"the class is listed: {reloaded[0]}")
    ed.check(await logged(ed, "Mover constructed version=2 reloading=1"), "the new constructor saw isReloading()")
    loaded = await logged(ed, "Mover loaded counter=")
    ed.check(loaded, "loadReloadState ran")
    counter = int(loaded[-1].split("counter=")[1].split()[0])
    ed.check(counter >= 40, f"the saved counter survived ({loaded[-1]})")
    ed.check("speed=2.5" in loaded[-1], f"the runtime DPROPERTY value survived ({loaded[-1]})")
    await ed.call("wait", frames=40)
    ed.check(await logged(ed, "Mover tick version=2"), "the new code runs")

    # a compile error keeps the last good library
    await ed.call("write_file", path="scripts/Mover.cpp", content=SOURCE.replace("{version}", "3").replace("counter++;", "counter++ undefinedThing;"))
    r = await ed.call("reload_scripts", language="cpp", timeout_seconds=1200, timeout=1300)
    kept = [x for x in r["kept_old"] if x["language"] == "cpp"]
    ed.check(kept, f"kept the old library: {r}")
    status = await ed.call("script_status")
    ed.check_eq(status["cpp"]["state"], "failed", f"the badge: {status['cpp']}")
    ed.check_eq(status["cpp"]["using_last_good_build"], True, "using the last good build")
    ed.check(any(f.get("file", "").endswith("Mover.cpp") and f.get("line") for f in status["cpp"]["findings"]),
             f"errors carry file and line: {status['cpp']['findings']}")
    await ed.console_since(None, limit=1)
    await ed.call("wait", frames=40)
    ed.check(await logged(ed, "Mover tick version=2"), "the old behaviour continues")

    # three more reloads, then Stop: every generation unloads
    for version in (4, 5, 6):
        await ed.call("write_file", path="scripts/Mover.cpp", content=SOURCE.replace("{version}", str(version)))
        r = await ed.call("reload_scripts", language="cpp", timeout_seconds=1200, timeout=1300)
        ed.check([x for x in r["reloaded"] if x["language"] == "cpp"], f"reload {version}: {r}")
    await ed.call("wait", frames=10)
    stopped = await ed.call("play", action="stop")
    ed.check_eq(stopped["state"], "stopped", "Stop")
    await ed.check_alive()
    await ed.call("script_status", reload_cpp_during_play=False)


async def test_fingerprint_refused(ed: Editor):
    """A hard mismatch: refused before any script code runs (no constructor log), the message
    names iteratorDebugLevel"""
    toolchain()
    async with Editor(create=True, name="Fingerprint Hard", env={"JASPER_TEST_FINGERPRINT_OVERRIDE": "iteratorDebugLevel=2"}) as ed2:
        s = await make_project(ed2)
        await ed2.console_since(None, limit=1)
        r = await play(ed2, s["scene"])
        ed2.check(r.is_error or (r.structured or {}).get("state") != "playing", f"play must fail: {r.text[:500]}")
        messages = [e["message"] for e in await ed2.console_since(None, limit=2000)]
        refused = [m for m in messages if "Refused script library" in m]
        ed2.check(refused and "iteratorDebugLevel" in refused[0], f"the refusal names the field: {refused or messages[-20:]}")
        ed2.check(not any("Mover constructed" in m for m in messages), "no script code ran")
        status = await ed2.call("script_status")
        ed2.check_eq(status["fingerprint"]["ok"], False, "script_status reports the refusal")
        ed2.check(any(m["field"] == "iteratorDebugLevel" and m["severity"] == "hard" for m in status["fingerprint"]["mismatches"]),
                  f"mismatches: {status['fingerprint']}")


async def test_fingerprint_soft_difference_plays(ed: Editor):
    toolchain()
    async with Editor(create=True, name="Fingerprint Soft", env={"JASPER_TEST_FINGERPRINT_OVERRIDE": "compilerMinor=999"}) as ed2:
        s = await make_project(ed2)
        await ed2.console_since(None, limit=1)
        r = await play(ed2, s["scene"])
        ed2.check(not r.is_error, f"play starts: {r.text[:500]}")
        await ed2.call("wait", frames=10)
        messages = [e["message"] for e in await ed2.console_since(None, limit=2000)]
        ed2.check(any("compilerMinor" in m and "another toolchain version" in m for m in messages), "a warning names compilerMinor")
        ed2.check(any("Mover constructed" in m for m in messages), "the library runs")
        await ed2.call("play", action="stop")
