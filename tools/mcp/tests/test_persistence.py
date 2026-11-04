"""Persistent objects (persistence spec 11): DontDestroyOnLoad in editor Play. The manager of the
Menu scene survives loads in the "DontDestroyOnLoad" scene with its script state, MCP lists and
inspects that scene, loading the Menu again leaves one manager (GameObject.persistSingleton),
and Play stop destroys it (its onDestroy runs) and leaves nothing behind."""

from harness import Editor

EDITOR = {"create": True, "name": "Persistence"}
CI = True

MANAGER = """---@class Manager : ScriptInstance
local Manager = {}
function Manager:init()
    if GameObject.persistSingleton(self) ~= self then
        print("persist duplicate")
        return
    end
    self.isSingleton = true
    self.counter = 0
    RegisterEngineEvent(self, "onUpdate")
    print("persist init")
end
function Manager:onUpdate() self.counter = self.counter + 1 end
function Manager:onDestroy()
    if self.isSingleton then print("persist destroy") end
end
return Manager
"""


def persist_lines(entries: list[dict]) -> list[str]:
    return [e["message"] for e in entries if e["message"].startswith("persist ")]


async def test_dont_destroy_on_load_in_play(ed: Editor):
    menu = (await ed.call("create_scene", name="Menu", type="2d"))["scene"]["id"]
    manager = (await ed.call("create_entity", scene=menu, name="Manager", type="empty"))["entity_id"]
    await ed.call("create_entity", scene=menu, name="MenuButton", type="empty")
    await ed.call("create_script", kind="lua", name="Manager", content=MANAGER, entity=manager, scene=menu)
    await ed.call("save_scene", scene=menu, path="Menu.scene")
    level = (await ed.call("create_scene", name="Level", type="2d"))["scene"]["id"]
    await ed.call("create_entity", scene=level, name="LevelThing", type="empty")
    await ed.call("save_scene", scene=level, path="Level.scene")

    await ed.console_since(None)
    await ed.call("play", action="start", scene=menu, auto_pause=True, fixed_fps=60)
    try:
        await ed.call("wait", frames=2)
        ed.check_eq(persist_lines(await ed.console_since()), ["persist init"])
        state = await ed.call("get_play_state")
        persistent = [s for s in state["running_scenes"] if s.get("persistent")]
        ed.check_eq([s["name"] for s in persistent], ["DontDestroyOnLoad"], f"running scenes: {state['running_scenes']}")
        listed = await ed.call("list_runtime_entities", scene="DontDestroyOnLoad")
        ed.check_eq([e["name"] for e in listed["entities"]], ["Manager"], "the manager moved at the scene's start")
        menu_now = await ed.call("list_runtime_entities")
        ed.check("Manager" not in [e["name"] for e in menu_now["entities"]], "not in the Menu scene any more")

        await ed.call("eval_lua", code="SceneManager.loadScene('Level')")
        await ed.call("wait", frames=3)
        listed = await ed.call("list_runtime_entities", scene="DontDestroyOnLoad")
        ed.check_eq([e["name"] for e in listed["entities"]], ["Manager"], "survives the load")
        live = await ed.call("get_runtime_entity", scene="DontDestroyOnLoad", entity="Manager")
        ed.check_eq(live.get("persistent"), True)
        counter = live["scripts"][0]["state"]["counter"]
        ed.check(counter >= 4, f"its updates kept counting across the load: {counter}")
        level_now = await ed.call("list_runtime_entities")
        ed.check("LevelThing" in [e["name"] for e in level_now["entities"]], "the Level scene is the main scene now")

        # the Menu loaded twice more: its new copies of the manager remove themselves
        for _ in range(2):
            await ed.call("eval_lua", code="SceneManager.loadScene('Menu')")
            await ed.call("wait", frames=3)
        ed.check_eq(persist_lines(await ed.console_since()), ["persist duplicate", "persist duplicate"])
        listed = await ed.call("list_runtime_entities", scene="DontDestroyOnLoad")
        ed.check_eq(listed["total"], 1, f"one manager: {listed['entities']}")
        found = await ed.call("eval_lua", code="return #GameObject.findObjectsOfType('Manager'), "
                                                "GameObject.isPersistent(GameObject.findObjectOfType('Manager'))")
        ed.check_eq(found["values"], [1, True])
        again = await ed.call("get_runtime_entity", scene="DontDestroyOnLoad", entity="Manager")
        ed.check(again["scripts"][0]["state"]["counter"] > counter, "the same instance, still counting")

        refused = await ed.call("eval_lua", code="local go = GameObject.find('MenuButton'); "
                                                  "local child = GameObject(go.scene); go:addChild(child); "
                                                  "return DontDestroyOnLoad(child)")
        ed.check_eq(refused["values"], [False], "a GameObject with a parent stays")
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)

    ed.check_eq(persist_lines(await ed.console_since()), ["persist destroy"], "Play stop destroys the persistent objects")
    state = await ed.call("get_play_state")
    ed.check(not state.get("running_scenes"), f"nothing persists into edit mode: {state.get('running_scenes')}")
    # edit mode: DontDestroyOnLoad moves nothing
    edit = await ed.call("eval_lua", code="return DontDestroyOnLoad(GameObject.find('Manager'))", allow_edit_mode=True)
    ed.check_eq(edit["values"], [False])
    listed = await ed.call("list_scripts", entity="Manager", scene=menu)
    ed.check_eq([s["class_name"] for s in listed["scripts"]], ["Manager"], "the Menu scene is back as saved")
    await ed.check_alive()


CPP_HEADER = """#pragma once
#include "ScriptBase.h"
#include "ScriptProperty.h"
#include "GameObject.h"

class CppKeeper : public jasperengine::ScriptBase {
public:
    DPROPERTY("Kid")
    jasperengine::GameObject* kid = nullptr;

    CppKeeper(jasperengine::Scene* scene, jasperengine::Entity entity);
    void init() override;
    void start() override;
    void onDestroy() override;
    void report(const char* when);
};
"""

CPP_SOURCE = """#include "CppKeeper.h"
#include "SceneManager.h"
#include "Log.h"

using namespace jasperengine;

CppKeeper::CppKeeper(Scene* scene, Entity entity): ScriptBase(scene, entity) {}
void CppKeeper::init() {
    if (persistSingleton<CppKeeper>() != this) { Log::print("persist cpp duplicate"); return; }
}
void CppKeeper::report(const char* when) {
    const bool here = kid && kid->getScene() == getScene() && getScene() == SceneManager::getPersistentScene();
    Log::print("persist cpp %s %s %s", when, here ? "persistent" : "elsewhere", kid ? kid->getName().c_str() : "none");
}
void CppKeeper::start() { report("start"); }
void CppKeeper::onDestroy() { if (isPersistent()) report("destroy"); }
"""


async def test_cpp_script_in_the_persistent_scene(ed: Editor):
    """The generated move hook: a C++ script's GameObject* property pointing at a child without a
    script follows the move (else it names the deleted original); Play stop with C++ scripts in
    the persistent scene runs their onDestroy and tears down cleanly."""
    menu = (await ed.call("create_scene", name="CppMenu", type="2d"))["scene"]["id"]
    keeper = (await ed.call("create_entity", scene=menu, name="Keeper", type="empty", select=False))["entity_id"]
    kid = (await ed.call("create_entity", scene=menu, name="PlainKid", type="empty", parent=keeper, select=False))["entity_id"]
    await ed.call("create_script", kind="cpp_script", name="CppKeeper", entity=keeper, scene=menu,
                  header_content=CPP_HEADER, source_content=CPP_SOURCE)
    await ed.call("set_script_properties", entity=keeper, scene=menu, values={"kid": kid})
    await ed.call("save_scene", scene=menu, path="CppMenu.scene")
    level = (await ed.call("create_scene", name="CppLevel", type="2d"))["scene"]["id"]
    await ed.call("save_scene", scene=level, path="CppLevel.scene")

    await ed.console_since(None)
    started = await ed.call_raw("play", {"action": "start", "scene": menu, "timeout_seconds": 900, "auto_pause": True,
                                         "fixed_fps": 60}, timeout=1000)
    ed.check(not started.is_error, f"play with the C++ script: {started.text[:1500]}")
    try:
        await ed.call("wait", frames=2)
        ed.check_eq(persist_lines(await ed.console_since()), ["persist cpp start persistent PlainKid"],
                    "the property's wrapper moved with the child")
        await ed.call("eval_lua", code="SceneManager.loadScene('CppLevel')")
        await ed.call("wait", frames=3)
        await ed.call("eval_lua", code="SceneManager.loadScene('CppMenu')")
        await ed.call("wait", frames=3)
        ed.check_eq(persist_lines(await ed.console_since()), ["persist cpp duplicate"])
        listed = await ed.call("list_runtime_entities", scene="DontDestroyOnLoad")
        ed.check_eq(sorted(e["name"] for e in listed["entities"]), ["Keeper", "PlainKid"])
    finally:
        await ed.call("play", action="stop", auto_pause=False, fixed_fps=0)
    ed.check_eq(persist_lines(await ed.console_since()), ["persist cpp destroy persistent PlainKid"],
                "Play stop: onDestroy with the wrapper still valid")
    await ed.check_alive()
