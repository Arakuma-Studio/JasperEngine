"""Script lifecycle (script-lifecycle spec 11): init / onEnable / start / onDisable / onDestroy in
editor Play, the runtime script fields of get_runtime_entity, enabling a script while playing,
check_script's lifecycle warnings, and a scene loaded again in Play being built anew."""

from harness import Editor

EDITOR = {"create": True, "name": "Lifecycle"}
CI_EXCLUDED = ("long-running (about 100 s on a loaded server: a C++ script compiled for Play); "
               "in the full suite (MCP_SUITE=all) on the build servers")

LOGGER = """---@class Logger : ScriptInstance
local Logger = {}
function Logger:init()
    lifecycleInits = (lifecycleInits or 0) + 1
    print("lifecycle init " .. self.gameObject.name .. " " .. lifecycleInits)
end
function Logger:onEnable() print("lifecycle enable") end
function Logger:start() print("lifecycle start") end
function Logger:onDisable() print("lifecycle disable") end
function Logger:onDestroy() print("lifecycle destroy") end
function Logger:onApplicationQuit() print("lifecycle quit") end
return Logger
"""


def lifecycle_lines(entries: list[dict], counts: bool = True) -> list[str]:
    lines = [e["message"] for e in entries if e["message"].startswith("lifecycle ")]
    # "lifecycle init <name> <n>": n counts across Play sessions (a Lua global)
    return lines if counts else [" ".join(line.split(" ")[:3]) for line in lines]


async def test_check_script_lifecycle_warnings(ed: Editor):
    code = ("local M = { properties = {{ name = 'enabled', type = 'bool', default = true }} }\n"
            "function M:Update() end\n"
            "function M:onUpdate() end\n"
            "-- function M:Start() end\n"
            "function M:init() end\n"
            "return M\n")
    result = await ed.call("check_script", code=code, language="lua")
    warnings = result.get("warnings", [])
    text = " | ".join(w["message"] for w in warnings)
    ed.check(any(w["line"] == 2 and "Update is never called" in w["message"] and "onUpdate" in w["message"] for w in warnings), text)
    ed.check(any(w["line"] == 3 and "never registered" in w["message"] for w in warnings), text)
    ed.check(any("reserved name" in w["message"] for w in warnings), text)
    ed.check(not any("Start" in w["message"] for w in warnings), "a commented-out method is not reported")
    clean = await ed.call("check_script", code="local M = {}\nfunction M:init() RegisterEngineEvent(self, 'onUpdate') end\n"
                                                "function M:onUpdate() end\nfunction M:start() end\nreturn M\n", language="lua")
    ed.check("warnings" not in clean, f"no warnings for a correct script: {clean.get('warnings')}")


async def test_lifecycle_in_play(ed: Editor):
    await ed.call("eval_lua", code="lifecycleInits = nil", allow_edit_mode=True)
    entity = (await ed.call("create_entity", name="LifeHost", type="empty"))["entity_id"]
    await ed.call("create_script", kind="lua", name="Logger", content=LOGGER, entity=entity)
    await ed.console_since(None)
    await ed.call("play", action="start", auto_pause=True, fixed_fps=60)
    try:
        await ed.call("wait", frames=2)
        lines = lifecycle_lines(await ed.console_since())
        ed.check_eq(lines, ["lifecycle init LifeHost 1", "lifecycle enable", "lifecycle start"], "load order")

        live = await ed.call("get_runtime_entity", entity="LifeHost")
        script = live["scripts"][0]
        ed.check_eq((script["enabled"], script["initialized"], script["started"], script["active_and_enabled"]),
                    (True, True, True, True), f"runtime script fields: {script}")

        # the Inspector / MCP path: onDisable, onEnable at once
        await ed.call("set_runtime_property", entity="LifeHost", component="script", property="scripts[0].enabled", value=False)
        ed.check_eq(lifecycle_lines(await ed.console_since()), ["lifecycle disable"])
        live = await ed.call("get_runtime_entity", entity="LifeHost")
        ed.check_eq(live["scripts"][0]["active_and_enabled"], False)
        await ed.call("set_runtime_property", entity="LifeHost", component="script", property="scripts[0].enabled", value=True)
        await ed.call("wait", frames=1)
        ed.check_eq(lifecycle_lines(await ed.console_since()), ["lifecycle enable"], "start ran already")

        # from Lua: self.enabled, deactivating the GameObject
        value = await ed.call("eval_lua", code="local s = object('LifeHost'):getScript('Logger'); s.enabled = false; "
                                                "local was = s.isActiveAndEnabled; s.enabled = true; "
                                                "object('LifeHost'):setActive(false); return was, s.isActiveAndEnabled")
        ed.check_eq(value["values"], [False, False])
        ed.check_eq(lifecycle_lines(await ed.console_since()), ["lifecycle disable", "lifecycle enable", "lifecycle disable"])
        await ed.call("eval_lua", code="object('LifeHost'):setActive(true)")
        ed.check_eq(lifecycle_lines(await ed.console_since()), ["lifecycle enable"], "no second init")

        # the scene loaded again is built anew, as in an exported build (old onDestroy first)
        await ed.call("eval_lua", code="SceneManager.loadScene(SceneManager.currentSceneName)")
        await ed.call("wait", frames=3)
        ed.check_eq(lifecycle_lines(await ed.console_since()),
                    ["lifecycle disable", "lifecycle destroy", "lifecycle init LifeHost 2", "lifecycle enable", "lifecycle start"],
                    "reload: the old instance goes, a new one starts")
        count = await ed.call("eval_lua", code="return lifecycleInits, object('LifeHost'):getScript('Logger').isActiveAndEnabled")
        ed.check_eq(count["values"], [2, True])
        live = await ed.call("get_runtime_entity", entity="LifeHost")
        ed.check_eq(live["scripts"][0]["started"], True, "the default scene is the reloaded copy")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)

    lines = lifecycle_lines(await ed.console_since())
    ed.check_eq(lines, ["lifecycle quit", "lifecycle disable", "lifecycle destroy"], "Play stop quits")
    # the played scene is back to its saved state, with its script
    listed = await ed.call("list_scripts", entity="LifeHost")
    ed.check_eq([s["class_name"] for s in listed["scripts"]], ["Logger"])
    await ed.check_alive()


async def test_disabled_and_inactive_scripts(ed: Editor):
    scene = (await ed.call("create_scene", name="Disabled", type="2d"))["scene"]["id"]
    host = (await ed.call("create_entity", name="OffHost", type="empty"))["entity_id"]
    asleep = (await ed.call("create_entity", name="Asleep", type="empty"))["entity_id"]
    await ed.call("create_script", kind="lua", name="Logger2", content=LOGGER.replace("Logger", "Logger2"), entity=host)
    await ed.call("attach_script", entity=asleep, path="Logger2.lua")
    await ed.call("update_script_entry", entity=host, enabled=False)
    await ed.call("set_entity_state", entity=asleep, active=False)
    await ed.console_since(None)
    await ed.call("play", action="start", scene=scene, auto_pause=True, fixed_fps=60)
    try:
        await ed.call("wait", frames=2)
        ed.check_eq(lifecycle_lines(await ed.console_since(), False), ["lifecycle init OffHost"],
                    "a disabled script inits (Awake); an inactive object waits")
        live = await ed.call("get_runtime_entity", entity="Asleep")
        ed.check_eq(live["scripts"][0]["initialized"], False)
        await ed.call("set_entity_state", entity="Asleep", active=True)
        ed.check_eq(lifecycle_lines(await ed.console_since(), False), ["lifecycle init Asleep", "lifecycle enable"])
        # update_script_entry in Play: enabled only, on the running GameObject
        await ed.check_error("update_script_entry", entity="OffHost", move_to=0, contains="only enabled")
        updated = await ed.call("update_script_entry", entity="OffHost", enabled=True)
        ed.check_eq(updated["scripts"][0]["enabled"], True)
        await ed.call("wait", frames=1)
        ed.check_eq(lifecycle_lines(await ed.console_since()), ["lifecycle enable", "lifecycle start", "lifecycle start"])
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)


CPP_HEADER = """#pragma once
#include "ScriptBase.h"
#include "ScriptProperty.h"
#include <string>

class CppLife : public jasperengine::ScriptBase {
public:
    DPROPERTY("Label")
    std::string label = "constructor";

    CppLife(jasperengine::Scene* scene, jasperengine::Entity entity);
    void init() override;
    void start() override;
    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;
};
"""

CPP_SOURCE = """#include "CppLife.h"
#include "Log.h"

using namespace jasperengine;

CppLife::CppLife(Scene* scene, Entity entity): ScriptBase(scene, entity) {}
void CppLife::init() { Log::print("lifecycle cpp init %s %s", label.c_str(), getScript<CppLife>() == this ? "self" : "other"); }
void CppLife::start() { Log::print("lifecycle cpp start"); }
void CppLife::onEnable() { Log::print("lifecycle cpp enable %d", isEnabled() ? 1 : 0); }
void CppLife::onDisable() { Log::print("lifecycle cpp disable"); }
void CppLife::onDestroy() { Log::print("lifecycle cpp destroy"); }
"""


async def test_cpp_script_lifecycle(ed: Editor):
    scene = (await ed.call("create_scene", name="Cpp", type="2d"))["scene"]["id"]
    entity = (await ed.call("create_entity", scene=scene, name="CppHost", type="empty", select=False))["entity_id"]
    await ed.call("create_script", kind="cpp_script", name="CppLife", entity=entity, scene=scene,
                  header_content=CPP_HEADER, source_content=CPP_SOURCE)
    await ed.call("set_script_properties", entity=entity, scene=scene, values={"label": "bound"})
    await ed.call("save_scene", scene=scene, path="CppLife.scene")
    await ed.console_since(None)
    started = await ed.call_raw("play", {"action": "start", "scene": scene, "timeout_seconds": 900, "auto_pause": True,
                                         "fixed_fps": 60}, timeout=1000)
    ed.check(not started.is_error, f"play with the C++ script: {started.text[:1500]}")
    try:
        await ed.call("wait", frames=2)
        ed.check_eq(lifecycle_lines(await ed.console_since()),
                    ["lifecycle cpp init bound self", "lifecycle cpp enable 1", "lifecycle cpp start"],
                    "init after the properties are bound, then onEnable, then start")
        await ed.call("set_runtime_property", entity="CppHost", component="script", property="scripts[0].enabled", value=False)
        ed.check_eq(lifecycle_lines(await ed.console_since()), ["lifecycle cpp disable"])
        live = await ed.call("get_runtime_entity", entity="CppHost")
        ed.check_eq((live["scripts"][0]["initialized"], live["scripts"][0]["started"], live["scripts"][0]["active_and_enabled"]),
                    (True, True, False))
        await ed.call("set_runtime_property", entity="CppHost", component="script", property="scripts[0].enabled", value=True)
        ed.check_eq(lifecycle_lines(await ed.console_since()), ["lifecycle cpp enable 1"])
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    ed.check_eq(lifecycle_lines(await ed.console_since()), ["lifecycle cpp disable", "lifecycle cpp destroy"], "Play stop")
