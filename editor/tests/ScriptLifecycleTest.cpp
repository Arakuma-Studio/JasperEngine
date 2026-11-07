// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The script lifecycle (script-lifecycle spec 11): init / onEnable / start / onDisable / onDestroy
// and the application messages for Lua and C++ scripts with exact execution orders, enabling
// scripts, activation, destroy, scene unload, quit; getScript and the GameObject finds; events
// (Unregister*, remove/has, the Event class); reserved members.

#include "ScriptTestHarness.h"

#include "script/ScriptBase.h"
#include "script/ScriptMessages.h"
#include "component/CameraComponent.h"
#include "object/GameObject.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::scripttest;

namespace {

    class ScriptLifecycleTest : public ScriptTest {};

    // ---- C++ scripts, filled in by hand as the generated factory does ----

    std::vector<std::string> cppLog;

    class CppLogger : public ScriptBase {
    public:
        CppLogger(Scene* scene, Entity entity) : ScriptBase(scene, entity) {}
        std::string tag() const { return scene->getEntityName(entity) + suffix; }
        std::string suffix = "1";
        void init() override { cppLog.push_back("init" + tag()); }
        void start() override { cppLog.push_back("start" + tag()); }
        void onEnable() override { cppLog.push_back("enable" + tag()); }
        void onDisable() override { cppLog.push_back("disable" + tag()); }
        void onDestroy() override { cppLog.push_back("destroy" + tag()); }
        void onApplicationQuit() override { cppLog.push_back("quit" + tag()); }
    };

    class CppPlain : public ScriptBase {
    public:
        CppPlain(Scene* scene, Entity entity) : ScriptBase(scene, entity) {}
    };

    class CppPartial : public ScriptBase {
    public:
        CppPartial(Scene* scene, Entity entity) : ScriptBase(scene, entity) {}
        void onEnable() override {}
        void onApplicationPause(bool) override {}
    };

    // the GameObject wrapper kind of C++ script (not a ScriptBase)
    class CppWrapper {
    public:
        CppWrapper(Scene*, Entity) {}
        void init() {}
    };

    // every message is a candidate for a ScriptBase (overrides, possibly private, are learned
    // when sent: CppOverridesLearnedWhenSent)
    static_assert(ScriptMessages::cppMask<CppPlain>() == ScriptMessages::cppMask<CppPartial>(), "no member is named");
    static_assert(ScriptMessages::cppMask<CppPlain>() & scriptMessageBit(ScriptMessage::Init), "a candidate");
    static_assert(ScriptMessages::cppMask<CppWrapper>() == 0, "not a ScriptBase: no messages");

    std::string joined(const std::vector<std::string>& lines) {
        std::string text;
        for (const std::string& line : lines) text += (text.empty() ? "" : " ") + line;
        return text;
    }

    template<class T>
    T* addCppScript(Scene* scene, Entity e, const std::string& className, bool enabled = true) {
        ScriptEntry entry;
        entry.type = ScriptType::CPP;
        entry.className = className;
        entry.enabled = enabled;
        T* script = new T(scene, e);
        entry.instance = script;
        entry.cppScript = ScriptMessages::asScriptBase(script);
        entry.cppMessageMask = ScriptMessages::cppMask<T>();
        if (ScriptComponent* scripts = scene->findComponent<ScriptComponent>(e)) {
            scripts->scripts.push_back(entry);
        } else {
            ScriptComponent component;
            component.scripts.push_back(entry);
            scene->addComponent<ScriptComponent>(e, component);
        }
        return script;
    }

    void deleteCppScripts(Scene* scene) {
        auto scripts = scene->getComponentArray<ScriptComponent>();
        for (size_t i = 0; i < scripts->size(); i++) {
            for (ScriptEntry& entry : scripts->getComponentFromIndex(i).scripts) {
                if (entry.type == ScriptType::CPP && entry.cppScript) {
                    ScriptRuntime::forgetInstance(entry.instance);
                    delete entry.cppScript;
                    entry.cppScript = nullptr;
                    entry.instance = nullptr;
                }
            }
        }
    }

}

// 1. all init, then all onEnable (pre-order, component order); start at the next frame's start
// point, before the first update
TEST_F(ScriptLifecycleTest, LoadOrderInitThenEnableThenStartBeforeUpdate) {
    writeLogger("L1");
    writeLogger("L2");
    Scene* scene = newScene();
    const Entity a = entity(scene, "A");
    const Entity b = entity(scene, "B", a);
    addScript(scene, a, "L1");
    addScript(scene, a, "L2");
    addScript(scene, b, "L1");
    addScript(scene, b, "L2");

    LuaBinding::initializeLuaScripts(scene);
    EXPECT_EQ(takeLog(), "initA1 initA2 initB1 initB2 enableA1 enableA2 enableB1 enableB2");
    EXPECT_TRUE(ScriptRuntime::hasStarted(scene));

    frame();
    EXPECT_EQ(takeLog(), "startA1 startA2 startB1 startB2 updateA1 updateA2 updateB1 updateB2");
    frame();
    EXPECT_EQ(takeLog(), "") << "start runs once";
    (void)b;
}

// 2. a disabled script: init runs (Unity Awake), onEnable / start / onUpdate do not until enabled
TEST_F(ScriptLifecycleTest, DisabledScriptInitsButWaitsForEnabled) {
    writeLogger("L1");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "L1", false);
    setScene("testScene", scene);

    LuaBinding::initializeLuaScripts(scene);
    EXPECT_EQ(takeLog(), "initX1");
    frame();
    EXPECT_EQ(takeLog(), "") << "no start, and the registered onUpdate is skipped while disabled";

    EXPECT_EQ(lua("local s = GameObject(testScene, " + std::to_string(x) + "):getScript('L1'); "
                  "local before = s.enabled; s.enabled = true; return tostring(before) .. ' ' .. tostring(s.enabled)"), "false true");
    EXPECT_EQ(takeLog(), "enableX1");
    frame();
    EXPECT_EQ(takeLog(), "startX1 updateX1");

    lua("GameObject(testScene, " + std::to_string(x) + "):getScript('L1').enabled = false");
    EXPECT_EQ(takeLog(), "disableX1");
    lua("GameObject(testScene, " + std::to_string(x) + "):getScript('L1').enabled = false");
    EXPECT_EQ(takeLog(), "") << "the same value: nothing";
    lua("GameObject(testScene, " + std::to_string(x) + "):getScript('L1').enabled = true");
    frame();
    EXPECT_EQ(takeLog(), "enableX1") << "start ran already";
}

// 3. inactive at load: no init; activation calls init and onEnable synchronously, deactivating the
// parent disables the children in pre-order, reactivating enables (init once)
TEST_F(ScriptLifecycleTest, InactiveObjectInitsOnFirstActivation) {
    writeLogger("L1");
    Scene* scene = newScene();
    const Entity p = entity(scene, "P", NULL_ENTITY, false);
    const Entity c = entity(scene, "C", p);
    addScript(scene, p, "L1");
    addScript(scene, c, "L1");
    setScene("testScene", scene);
    const std::string go = "GameObject(testScene, " + std::to_string(p) + ")";

    LuaBinding::initializeLuaScripts(scene);
    EXPECT_EQ(takeLog(), "");
    EXPECT_EQ(lua("return tostring(GameObject(testScene, " + std::to_string(c) + "):getScript('L1') ~= nil)"), "true")
        << "instances exist on inactive objects";

    lua(go + ":setActive(true)");
    EXPECT_EQ(takeLog(), "initP1 initC1 enableP1 enableC1");
    lua(go + ":setActive(false)");
    EXPECT_EQ(takeLog(), "disableP1 disableC1");
    lua(go + ":setActive(true)");
    EXPECT_EQ(takeLog(), "enableP1 enableC1");
    frame();
    EXPECT_EQ(takeLog(), "startP1 startC1 updateP1 updateC1");

    // the child alone; then reparenting under an inactive object deactivates it
    scene->setEntityActive(c, false);
    EXPECT_EQ(takeLog(), "disableC1");
    scene->setEntityActive(c, true);
    EXPECT_EQ(takeLog(), "enableC1");
    const Entity off = entity(scene, "Off", NULL_ENTITY, false);
    scene->addEntityChild(off, c, false);
    EXPECT_EQ(takeLog(), "disableC1");
    scene->addEntityChild(p, c, false);
    EXPECT_EQ(takeLog(), "enableC1");
}

// 4. a script disabling itself in onEnable: onDisable right after; no start
TEST_F(ScriptLifecycleTest, DisablingItselfInOnEnable) {
    writeLogger("L1", "function M:onEnable() log[#log + 1] = 'enable' .. tag(self); self.enabled = false end\n");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "L1");

    LuaBinding::initializeLuaScripts(scene);
    EXPECT_EQ(takeLog(), "initX1 enableX1 disableX1");
    frame();
    EXPECT_EQ(takeLog(), "") << "not active and enabled at the start point";
}

// 5. destroy: onDisable then onDestroy at the end of the frame; a never-active object gets neither
TEST_F(ScriptLifecycleTest, DestroyDisablesThenDestroys) {
    writeLogger("L1");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    const Entity y = entity(scene, "Y", NULL_ENTITY, false);
    addScript(scene, x, "L1");
    addScript(scene, y, "L1");
    setScene("testScene", scene);
    LuaBinding::initializeLuaScripts(scene);
    frame();
    clearLog();

    lua("GameObject.destroy(GameObject(testScene, " + std::to_string(x) + "))");
    EXPECT_EQ(takeLog(), "") << "deferred to the end of the frame";
    frame();
    EXPECT_EQ(takeLog(), "disableX1 destroyX1");
    EXPECT_FALSE(scene->isEntityCreated(x));

    lua("GameObject.destroyImmediate(GameObject(testScene, " + std::to_string(y) + "))");
    EXPECT_EQ(takeLog(), "") << "never active: no init, no onDestroy";
    EXPECT_FALSE(scene->isEntityCreated(y));
}

// 6. a scene load: the old scene's onDisable and onDestroy before the new scene's init
TEST_F(ScriptLifecycleTest, SceneLoadUnloadsOldScenesBeforeNewInit) {
    writeLogger("L1");
    Scene* first = newScene();
    Scene* second = newScene();
    entity(first, "A");
    entity(second, "B");
    addScript(first, first->findEntity("A"), "L1");
    addScript(second, second->findEntity("B"), "L1");
    SceneManager::registerScene(1, "First", [first]() {
        LuaBinding::initializeLuaScripts(first);
        Engine::setScene(first);
    });
    SceneManager::registerScene(2, "Second", [second]() {
        EXPECT_EQ(SceneManager::getUnloadingScenes().size(), 1u);
        LuaBinding::initializeLuaScripts(second);
        Engine::setScene(second);
    });

    ASSERT_TRUE(SceneManager::loadScene("First"));
    EXPECT_EQ(takeLog(), "initA1 enableA1");
    ASSERT_TRUE(SceneManager::loadScene("Second"));
    EXPECT_TRUE(SceneManager::isLoadPending());
    SceneManager::applyPendingLoad();
    EXPECT_EQ(takeLog(), "disableA1 destroyA1 initB1 enableB1");
    EXPECT_FALSE(ScriptRuntime::hasStarted(first));
    EXPECT_TRUE(ScriptRuntime::hasStarted(second));
    EXPECT_TRUE(SceneManager::getUnloadingScenes().empty());
    Engine::removeAllScenes();
}

// 7. quit: onApplicationQuit to all, then onDisable, then onDestroy
TEST_F(ScriptLifecycleTest, QuitCallsQuitThenDisableThenDestroy) {
    writeLogger("L1");
    Scene* scene = newScene();
    addScript(scene, entity(scene, "X"), "L1");
    addScript(scene, entity(scene, "Y"), "L1", false);
    addScript(scene, entity(scene, "Z", NULL_ENTITY, false), "L1");
    LuaBinding::initializeLuaScripts(scene);
    clearLog();

    ScriptRuntime::quitAll();
    EXPECT_EQ(takeLog(), "quitX1 quitY1 disableX1 destroyX1 destroyY1")
        << "every initialized instance (enabled or not) quits; never-initialized ones get nothing";
    EXPECT_FALSE(ScriptRuntime::hasStarted(scene));
    LuaBinding::cleanupLuaScripts(scene);
    EXPECT_EQ(takeLog(), "") << "cleanup after the unload only releases";
}

// 8. application pause and focus go to the scripts of running scenes, active objects only
TEST_F(ScriptLifecycleTest, PauseAndFocusMessages) {
    writeLogger("L1");
    Scene* scene = newScene();
    addScript(scene, entity(scene, "X"), "L1", false);
    addScript(scene, entity(scene, "Y", NULL_ENTITY, false), "L1");
    LuaBinding::initializeLuaScripts(scene);
    clearLog();

    Engine::systemPause();
    EXPECT_EQ(takeLog(), "") << "not a running scene";
    Engine::systemResume();
    Engine::systemFocus(true);
    EXPECT_EQ(takeLog(), "");
    Engine::setScene(scene);
    Engine::systemPause();
    // platform-services: Unity's order, the focus is lost before the pause
    EXPECT_EQ(takeLog(), "focusfalseX1 pausetrueX1") << "enabled or not, active objects only";
    Engine::systemResume();
    EXPECT_EQ(takeLog(), "pausefalseX1");
    Engine::systemFocus(true);
    EXPECT_EQ(takeLog(), "focustrueX1");
    Engine::systemFocus(false);
    Engine::systemFocus(false);
    EXPECT_EQ(takeLog(), "focusfalseX1") << "only changes";
    EXPECT_FALSE(Engine::isFocused());
    Engine::systemFocus(true);
    EXPECT_EQ(takeLog(), "focustrueX1");
    Engine::removeAllScenes();
}

// 9. getScript family: disabled scripts are returned; children and parents, includeInactive
TEST_F(ScriptLifecycleTest, GetScriptFamily) {
    writeLogger("L1");
    writeLogger("L2");
    Scene* scene = newScene();
    const Entity r = entity(scene, "R");
    const Entity c = entity(scene, "C", r);
    const Entity i = entity(scene, "I", r, false);
    addScript(scene, r, "L1");
    addScript(scene, r, "L2", false);
    addScript(scene, c, "L1");
    addScript(scene, i, "L2");
    setScene("testScene", scene);
    LuaBinding::initializeLuaScripts(scene);
    const std::string R = "GameObject(testScene, " + std::to_string(r) + ")";
    const std::string C = "GameObject(testScene, " + std::to_string(c) + ")";
    const std::string I = std::to_string(i);

    EXPECT_EQ(lua("local s = " + R + ":getScript('L2'); return s.__name .. ' ' .. tostring(s.enabled)"), "L2 false");
    EXPECT_EQ(lua("return tostring(" + R + ":getScript('Missing'))"), "nil");
    EXPECT_EQ(lua("return #" + R + ":getScripts() .. ' ' .. #" + R + ":getScripts('L1')"), "2 1");
    EXPECT_EQ(lua("return " + R + ":getScriptInChildren('L2').entity == " + std::to_string(r)), "true") << "self first";
    EXPECT_EQ(lua("return " + R + ":getScriptInChildren('L1').entity"), std::to_string(r));
    EXPECT_EQ(lua("return #" + R + ":getScriptsInChildren('L1')"), "2");
    EXPECT_EQ(lua("return #" + R + ":getScriptsInChildren('L2') .. ' ' .. #" + R + ":getScriptsInChildren('L2', true)"), "1 2")
        << "inactive children only with includeInactive";
    EXPECT_EQ(lua("return " + C + ":getScriptInParent('L2').entity"), std::to_string(r));
    EXPECT_EQ(lua("return tostring(GameObject(testScene, " + I + "):getScriptInParent('L2').entity)"), std::to_string(r))
        << "an inactive self is skipped without includeInactive";
    EXPECT_EQ(lua("return GameObject(testScene, " + I + "):getScriptInParent('L2', true).entity"), I);
    // from a script instance (self)
    EXPECT_EQ(lua("local s = " + C + ":getScript('L1'); return s:getScriptInParent('L2').__name"), "L2");
    EXPECT_EQ(lua("local s = " + R + ":getScript('L1'); return s:getScript('L2').__name .. #s:getScripts()"), "L22");
    EXPECT_NE(luaError(R + ":getScript(1)").find("getScript: className must be a string"), std::string::npos);
    // self.gameObject is cached, isActiveAndEnabled follows both flags
    EXPECT_EQ(lua("local s = " + R + ":getScript('L1'); return tostring(s.gameObject == s.gameObject) .. ' ' .. "
                  "s.gameObject.name .. ' ' .. tostring(s.isActiveAndEnabled) .. ' ' .. tostring(" + R + ":getScript('L2').isActiveAndEnabled)"),
              "true R true false");
    EXPECT_NE(luaError(R + ":getScript('L1').gameObject = 1").find("gameObject is read-only"), std::string::npos);
    EXPECT_NE(luaError(R + ":getScript('L1').isActiveAndEnabled = true").find("isActiveAndEnabled is read-only"), std::string::npos);
}

// 10. findObjectOfType over the main scene and a layer, in Engine order; includeInactive; wrappers
TEST_F(ScriptLifecycleTest, FindObjectsOfTypeAcrossScenes) {
    writeLogger("L1");
    writeLogger("L2");
    Scene* level = newScene();
    Scene* hud = newScene();
    const Entity m = entity(level, "M");
    const Entity h = entity(hud, "H");
    const Entity hidden = entity(hud, "Hidden", NULL_ENTITY, false);
    const Entity off = entity(hud, "Off");
    addScript(level, m, "L1");
    addScript(hud, h, "L1");
    addScript(hud, h, "L2");
    addScript(hud, hidden, "L2");
    addScript(hud, off, "L2", false);
    const Entity camera = entity(hud, "Cam");
    hud->addComponent<CameraComponent>(camera, CameraComponent());
    LuaBinding::initializeLuaScripts(level);
    LuaBinding::initializeLuaScripts(hud);
    setScene("hud", hud);
    Engine::setScene(level);
    Engine::addSceneLayer(hud);

    EXPECT_EQ(lua("local s = GameObject.findObjectOfType('L1'); return s.gameObject.name .. s.entity"), "M" + std::to_string(m));
    EXPECT_EQ(lua("local list = GameObject.findObjectsOfType('L1'); return #list .. list[1].gameObject.name .. list[2].gameObject.name"), "2MH");
    EXPECT_EQ(lua("return #GameObject.findObjectsOfType('L2') .. ' ' .. #GameObject.findObjectsOfType('L2', true)"), "1 3")
        << "an inactive object and a disabled script only with includeInactive";
    EXPECT_EQ(lua("return tostring(GameObject.findObjectOfType('Nope'))"), "nil");
    EXPECT_EQ(lua("return #GameObject.findObjectsOfType('Nope')"), "0");
    EXPECT_EQ(lua("for _, c in ipairs(GameObject.findObjectsOfType('Camera')) do if c.entity == " + std::to_string(camera) +
                  " then return getmetatable(c) == getmetatable(Camera(hud, c.entity)) end end return false"), "true")
        << "a wrapper name gives that wrapper for the GameObjects with its component";
    EXPECT_NE(luaError("GameObject.findObjectOfType('GameObject')").find("not a component type"), std::string::npos);
    Engine::removeAllScenes();
    EXPECT_EQ(lua("return tostring(GameObject.findObjectOfType('L1'))"), "nil") << "running scenes only";
}

// 11. GameObject.find by name and path, findWithTag / findGameObjectsWithTag (active only)
TEST_F(ScriptLifecycleTest, FindByNamePathAndTag) {
    Scene* scene = newScene();
    const Entity root = entity(scene, "Root");
    const Entity child = entity(scene, "Child", root);
    const Entity other = entity(scene, "Other");
    const Entity otherChild = entity(scene, "Child", other, false);
    scene->setEntityTag(child, "Player");
    scene->setEntityTag(other, "Player");
    scene->setEntityTag(otherChild, "Player");
    Engine::setScene(scene);

    EXPECT_EQ(lua("return GameObject.find('Root/Child').entity"), std::to_string(child));
    EXPECT_EQ(lua("return GameObject.find('/Root/Child').entity"), std::to_string(child));
    EXPECT_EQ(lua("return tostring(GameObject.find('/Child'))"), "nil") << "a leading slash: from a root";
    EXPECT_EQ(lua("return tostring(GameObject.find('Other/Child'))"), "nil") << "inactive";
    EXPECT_EQ(lua("return GameObject.find('Child').entity"), std::to_string(child));
    EXPECT_EQ(lua("return GameObject.findWithTag('Player').entity"), std::to_string(child));
    EXPECT_EQ(lua("return #GameObject.findGameObjectsWithTag('Player')"), "2");
    EXPECT_EQ(lua("return tostring(GameObject.findWithTag('NoSuchTag'))"), "nil");
    EXPECT_TRUE(logged("Tag: NoSuchTag is not defined."));
    Engine::removeAllScenes();
}

// 12. UnregisterEngineEvent / UnregisterEvent and the event objects' remove / has
TEST_F(ScriptLifecycleTest, UnregisterEvents) {
    writeScript("U1",
        "local M = {}\n"
        "function M:init() RegisterEngineEvent(self, 'onUpdate'); self.ev = Event(); RegisterEvent(self, self.ev, 'onHit') end\n"
        "function M:onUpdate() log[#log + 1] = 'u' end\n"
        "function M:onHit(n) log[#log + 1] = 'hit' .. n end\n"
        "function M:stop() log[#log + 1] = tostring(UnregisterEngineEvent(self, 'onUpdate')) end\n"
        "function M:stopHit() log[#log + 1] = tostring(UnregisterEvent(self, self.ev, 'onHit')) end\n"
        "return M\n");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "U1");
    setScene("testScene", scene);
    LuaBinding::initializeLuaScripts(scene);
    const std::string s = "GameObject(testScene, " + std::to_string(x) + "):getScript('U1')";

    frame();
    EXPECT_EQ(takeLog(), "u");
    lua(s + ".ev:invoke(1)");
    EXPECT_EQ(takeLog(), "hit1");
    lua(s + ":stop()");
    EXPECT_EQ(takeLog(), "true");
    frame();
    EXPECT_EQ(takeLog(), "") << "unsubscribed";
    lua(s + ":stop()");
    EXPECT_EQ(takeLog(), "false") << "nothing to remove";
    lua(s + ":stopHit()");
    lua(s + ".ev:invoke(2)");
    EXPECT_EQ(takeLog(), "true");
    EXPECT_NE(luaError("UnregisterEngineEvent(" + s + ", 'onNothing')").find("Engine.onNothing not found"), std::string::npos);

    // remove / has on a FunctionSubscribe event object
    EXPECT_EQ(lua("Engine.onUpdate:add('test-tag', function() end); local had = Engine.onUpdate:has('test-tag'); "
                  "local removed = Engine.onUpdate:remove('test-tag'); "
                  "return tostring(had) .. tostring(removed) .. tostring(Engine.onUpdate:has('test-tag')) .. tostring(Engine.onUpdate:remove('test-tag'))"),
              "truetruefalsefalse");
}

// 13. Event: order, duplicates, errors isolated, owned listeners go with their owner, RegisterEvent
TEST_F(ScriptLifecycleTest, EventClass) {
    writeScript("Owner1",
        "local M = {}\n"
        "function M:init() shared:addListener(self, 'onShared'); RegisterEvent(self, shared, 'onTagged') end\n"
        "function M:onShared(v) log[#log + 1] = 'owner' .. v end\n"
        "function M:onTagged(v) log[#log + 1] = 'tagged' .. v end\n"
        "return M\n");
    lua("shared = Event()\n"
        "f1 = function(v) log[#log + 1] = 'f1:' .. v end\n"
        "f2 = function(v) log[#log + 1] = 'f2:' .. v end\n"
        "bad = function() error('boom') end\n");
    EXPECT_EQ(lua("local e = Event(); e:addListener(f1); e:addListener(bad); e:addListener(f2); e:addListener(f1); "
                  "e:invoke('a'); return e.listenerCount"), "3") << "a second add of the same function is ignored";
    EXPECT_EQ(takeLog(), "f1:a f2:a") << "the failing listener does not stop the others";
    EXPECT_TRUE(logged("boom"));
    EXPECT_EQ(lua("local e = Event(); e:addListener(f1); e:addListener(f2); local removed = e:removeListener(f1); "
                  "e:invoke('b'); return tostring(removed) .. tostring(e:removeListener(f1))"), "truefalse");
    EXPECT_EQ(takeLog(), "f2:b");
    // added during invoke: from the next one; removed during invoke: skipped at once
    lua("local e = Event(); e:addListener(function(v) log[#log + 1] = 'x' .. v; e:addListener(f2); e:removeListener(f1) end); "
        "e:addListener(f1); e:invoke('1'); e:invoke('2')");
    EXPECT_EQ(takeLog(), "x1 x2 f2:2");

    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "Owner1");
    setScene("testScene", scene);
    LuaBinding::initializeLuaScripts(scene);
    lua("shared:addListener(f1); shared:invoke('c')");
    EXPECT_EQ(takeLog(), "owner" "c tagged" "c f1:c");
    EXPECT_EQ(lua("return shared.listenerCount"), "3");
    lua("GameObject.destroyImmediate(GameObject(testScene, " + std::to_string(x) + "))");
    EXPECT_EQ(lua("shared:invoke('d'); return shared.listenerCount"), "1") << "the destroyed script's listeners went";
    EXPECT_EQ(takeLog(), "f1:d");
    lua("shared:removeAllListeners(); shared:invoke('e')");
    EXPECT_EQ(takeLog(), "");
    EXPECT_EQ(lua("return tostring(shared)"), "Event(0 listeners)");
    lua("shared = nil; f1 = nil; f2 = nil; bad = nil");
}

// 14. re-entrancy: a child's onEnable deactivating its parent leaves the latches paired
TEST_F(ScriptLifecycleTest, ReentrantActivationKeepsPairs) {
    writeLogger("L1");
    writeLogger("R1", "function M:onEnable() log[#log + 1] = 'enable' .. tag(self); "
                      "if not self.done then self.done = true; GameObject(self.scene, parentId):setActive(false) end end\n");
    Scene* scene = newScene();
    const Entity p = entity(scene, "P");
    const Entity c = entity(scene, "C", p);
    addScript(scene, p, "L1");
    addScript(scene, c, "R1");
    setScene("testScene", scene);
    lua("parentId = " + std::to_string(p));

    LuaBinding::initializeLuaScripts(scene);
    EXPECT_EQ(takeLog(), "initP1 initC1 enableP1 enableC1 disableP1 disableC1");
    frame();
    EXPECT_EQ(takeLog(), "") << "neither is active and enabled at the start point";
    scene->setEntityActive(p, true);
    EXPECT_EQ(takeLog(), "enableP1 enableC1");
    frame();
    EXPECT_EQ(takeLog(), "startP1 startC1 updateP1 updateC1");
    // destroy during onDisable of the same subtree: each script once
    writeLogger("D1", "function M:onDisable() log[#log + 1] = 'disable' .. tag(self); GameObject.destroyImmediate(self) end\n");
    const Entity d = entity(scene, "D", p);
    addScript(scene, d, "D1");
    ObjectManager::initializeScripts(scene, {d});
    EXPECT_EQ(takeLog(), "initD1 enableD1");
    scene->setEntityActive(p, false);
    EXPECT_EQ(takeLog(), "disableP1 disableC1 disableD1 destroyD1");
    EXPECT_FALSE(scene->isEntityCreated(d));
    (void)c;
}

// 15. C++ ScriptBase scripts: the same passes and order as Lua (test 1)
TEST_F(ScriptLifecycleTest, CppScriptsSameOrder) {
    cppLog.clear();
    Scene* scene = newScene();
    const Entity a = entity(scene, "A");
    const Entity b = entity(scene, "B", a);
    addCppScript<CppLogger>(scene, a, "CppLogger");
    addCppScript<CppLogger>(scene, a, "CppLogger")->suffix = "2";
    addCppScript<CppLogger>(scene, b, "CppLogger");
    addCppScript<CppLogger>(scene, b, "CppLogger")->suffix = "2";
    CppPlain* plain = addCppScript<CppPlain>(scene, b, "CppPlain");

    ScriptRuntime::awakeScene(scene);
    EXPECT_EQ(joined(cppLog), "initA1 initA2 initB1 initB2 enableA1 enableA2 enableB1 enableB2");
    cppLog.clear();
    frame();
    EXPECT_EQ(joined(cppLog), "startA1 startA2 startB1 startB2");
    cppLog.clear();

    EXPECT_TRUE(plain->isEnabled());
    plain->setEnabled(false);
    EXPECT_FALSE(plain->isEnabled());
    EXPECT_FALSE(plain->isActiveAndEnabled());
    EXPECT_EQ(plain->getScript<CppLogger>()->suffix, "1") << "getScript<T>: the first one of that class";
    EXPECT_EQ(plain->getScript<ScriptBase>(), scene->getComponent<ScriptComponent>(b).scripts[0].cppScript) << "a base class finds subclasses";
    EXPECT_EQ(plain->getScript<CppPartial>(), nullptr);

    CppLogger* first = static_cast<CppLogger*>(scene->getComponent<ScriptComponent>(a).scripts[0].cppScript);
    first->setEnabled(false);
    first->setEnabled(true);
    EXPECT_EQ(joined(cppLog), "disableA1 enableA1");
    cppLog.clear();
    ScriptRuntime::quitAll();
    EXPECT_EQ(joined(cppLog), "quitA1 quitA2 quitB1 quitB2 disableA1 disableA2 disableB1 disableB2 destroyA1 destroyA2 destroyB1 destroyB2");
    deleteCppScripts(scene);
}

// 16. reserved members: a property named enabled stops the script; enabled takes booleans only
TEST_F(ScriptLifecycleTest, ReservedMembers) {
    writeLogger("L1");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "L1");
    ScriptProperty reserved;
    reserved.name = "enabled";
    reserved.type = ScriptPropertyType::Bool;
    reserved.value = true;
    scene->getComponent<ScriptComponent>(x).scripts[0].properties.push_back(reserved);
    const Entity y = entity(scene, "Y");
    addScript(scene, y, "L1");
    setScene("testScene", scene);

    LuaBinding::initializeLuaScripts(scene);
    EXPECT_EQ(scene->getComponent<ScriptComponent>(x).scripts[0].instance, nullptr);
    EXPECT_TRUE(logged("property 'enabled' uses a reserved name"));
    EXPECT_EQ(takeLog(), "initY1 enableY1");
    EXPECT_NE(luaError("GameObject(testScene, " + std::to_string(y) + "):getScript('L1').enabled = 1").find("enabled must be a boolean"),
              std::string::npos);
    // a released instance: the assignment is ignored with a warning
    lua("released = GameObject(testScene, " + std::to_string(y) + "):getScript('L1')");
    LuaBinding::cleanupLuaScripts(scene);
    clearLog();
    lua("released.enabled = true; released = nil");
    EXPECT_EQ(takeLog(), "");
    EXPECT_TRUE(logged("released or is not attached"));
}

// 9.3: deactivating itself in init during the load: no onEnable, init not called again
TEST_F(ScriptLifecycleTest, DeactivatingItselfInInit) {
    writeLogger("L1", "function M:init() log[#log + 1] = 'init' .. tag(self); self.gameObject:setActive(false) end\n");
    writeLogger("L2");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "L1");
    addScript(scene, x, "L2");
    LuaBinding::initializeLuaScripts(scene);
    EXPECT_EQ(takeLog(), "initX1") << "the object is inactive: its other scripts wait for the activation";
    scene->setEntityActive(x, true);
    EXPECT_EQ(takeLog(), "initX2 enableX1 enableX2");
}

// errors in callbacks are logged and the instance goes on; instantiate runs init and onEnable
TEST_F(ScriptLifecycleTest, ErrorsAndInstantiate) {
    writeScript("E1",
        "local M = {}\n"
        "function M:init() log[#log + 1] = 'init'; error('init failed') end\n"
        "function M:onEnable() log[#log + 1] = 'enable' end\n"
        "function M:start() log[#log + 1] = 'start'; self.copy = GameObject.instantiate(self) end\n"
        "return M\n");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "E1");
    LuaBinding::initializeLuaScripts(scene);
    EXPECT_EQ(takeLog(), "init enable");
    EXPECT_TRUE(logged("Lua init() failed for 'E1' on 'X'"));
    const ScriptState* state = ScriptRuntime::getState(scene->getComponent<ScriptComponent>(x).scripts[0].instance);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->initialized);
    frame();
    EXPECT_EQ(takeLog(), "start init enable") << "the copy's init and onEnable run inside instantiate";
    frame();
    EXPECT_EQ(takeLog(), "start init enable") << "the copy starts at the next frame (and copies itself)";
}

// instantiate never returns a dead root (runtime-core spec 6.2): a copy whose init destroys it at
// once gives nil (Unity's fake null); a deferred destroy keeps it for the rest of the frame
TEST_F(ScriptLifecycleTest, InstantiateReturnsNilWhenInitDestroysTheCopy) {
    writeScript("Doomed",
        "local M = {}\n"
        "function M:init() if self.gameObject.name ~= 'X' then log[#log + 1] = 'init'; GameObject.destroyImmediate(self.gameObject) end end\n"
        "return M\n");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    entity(scene, "XChild", x);
    addScript(scene, x, "Doomed");
    LuaBinding::initializeLuaScripts(scene);
    setScene("testScene", scene);

    EXPECT_EQ(lua("local copy = GameObject.instantiate(GameObject(testScene, " + std::to_string(x) + "))\n"
                  "return tostring(copy)"), "nil") << "the copy destroyed itself in init";
    EXPECT_EQ(takeLog(), "init");
    EXPECT_TRUE(scene->isEntityCreated(x)) << "the original stays";
}

TEST_F(ScriptLifecycleTest, InstantiateKeepsARootDestroyedLater) {
    writeScript("Fading",
        "local M = {}\n"
        "function M:init() if self.gameObject.name ~= 'X' then GameObject.destroy(self.gameObject) end end\n"
        "return M\n");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "Fading");
    LuaBinding::initializeLuaScripts(scene);
    setScene("testScene", scene);

    EXPECT_EQ(lua("local copy = GameObject.instantiate(GameObject(testScene, " + std::to_string(x) + "))\n"
                  "return tostring(copy ~= nil and copy.name)"), "X(Clone)")
        << "a deferred destroy keeps the copy until the end of the frame";
}

TEST_F(ScriptLifecycleTest, CppInstantiateOfADestroyedCopyIsEmpty) {
    writeScript("Doomed",
        "local M = {}\n"
        "function M:init() if self.gameObject.name ~= 'X' then GameObject.destroyImmediate(self.gameObject) end end\n"
        "return M\n");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "Doomed");
    LuaBinding::initializeLuaScripts(scene);

    const GameObject copy = GameObject::instantiate(GameObject(scene, x));
    EXPECT_EQ(copy.getEntity(), NULL_ENTITY);
    EXPECT_FALSE(static_cast<bool>(copy)) << "the C++ handle tests false (Unity's null)";
    EXPECT_TRUE(static_cast<bool>(GameObject(scene, x)));
    EXPECT_FALSE(static_cast<bool>(GameObject(scene, NULL_ENTITY)));
}

// animation events (sendNamed) still go to enabled Lua scripts only
TEST_F(ScriptLifecycleTest, NamedMethodsGoToEnabledScripts) {
    writeLogger("L1", "function M:onStep(a) log[#log + 1] = 'step' .. tag(self) .. a end\n");
    writeLogger("L2", "function M:onStep(a) log[#log + 1] = 'step' .. tag(self) .. a end\n");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "L1");
    addScript(scene, x, "L2", false);
    LuaBinding::initializeLuaScripts(scene);
    clearLog();
    EXPECT_TRUE(LuaBinding::callLuaScriptMethod(scene, x, "onStep", {std::string("!")}));
    EXPECT_FALSE(LuaBinding::callLuaScriptMethod(scene, x, "missing", {}));
    EXPECT_EQ(takeLog(), "stepX1!");
    EXPECT_EQ(ScriptMessages::receivers(scene, x) & scriptMessageBit(ScriptMessage::Enable), scriptMessageBit(ScriptMessage::Enable));
    EXPECT_EQ(ScriptMessages::receivers(scene, x) & scriptMessageBit(ScriptMessage::CollisionEnter), 0u);
    EXPECT_EQ(ScriptMessages::send(scene, x, ScriptMessage::ApplicationQuit, nullptr, nullptr), 2) << "send: enabled or not";
    EXPECT_EQ(ScriptRuntime::sendMessage(scene, x, ScriptMessage::ApplicationQuit, nullptr, nullptr, DeliverRequireEnabled), 1);
    EXPECT_EQ(takeLog(), "quitX1 quitX2 quitX1");
}

// the editor's commands write the enabled flag directly: refreshEntity calls the callbacks
TEST_F(ScriptLifecycleTest, RefreshAfterDirectWrites) {
    writeLogger("L1");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "L1");
    scene->getComponent<ScriptComponent>(x).scripts[0].enabled = false;
    ScriptRuntime::refreshEntity(scene, x);
    EXPECT_EQ(takeLog(), "") << "before the scene started: nothing";
    LuaBinding::initializeLuaScripts(scene);
    EXPECT_EQ(takeLog(), "initX1");
    scene->getComponent<ScriptComponent>(x).scripts[0].enabled = true;
    ScriptRuntime::refreshEntity(scene, x);
    EXPECT_EQ(takeLog(), "enableX1");
    scene->getComponent<ScriptComponent>(x).scripts[0].enabled = false;
    ScriptRuntime::refreshEntity(scene, x);
    EXPECT_EQ(takeLog(), "disableX1");
}

// ---- review regressions ----------------------------------------------------------------------

// a script enabled during a frame (by another script's onUpdate) gets its start at the next frame
// start point, and its onUpdate only after it: Unity's "Start before the first Update"
TEST_F(ScriptLifecycleTest, EnabledMidFrameStartsBeforeItsFirstUpdate) {
    writeScript("Mgr",
        "local M = {}\n"
        "function M:init() RegisterEngineEvent(self, 'onUpdate') end\n"
        "function M:onUpdate() if not self.done then self.done = true; log[#log + 1] = 'mgr'; self:getScript('L2').enabled = true end end\n"
        "return M\n");
    writeLogger("L2");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "Mgr");
    addScript(scene, x, "L2", false);
    LuaBinding::initializeLuaScripts(scene);
    clearLog();
    frame();
    EXPECT_EQ(takeLog(), "mgr enableX2") << "no onUpdate before its start";
    frame();
    EXPECT_EQ(takeLog(), "startX2 updateX2");
}

namespace {
    class CppUpdater : public ScriptBase {
    public:
        CppUpdater(Scene* scene, Entity entity) : ScriptBase(scene, entity) { REGISTER_ENGINE_EVENT(onUpdate); }
        ~CppUpdater() override { UNREGISTER_ENGINE_EVENT(onUpdate); }
        void onEnable() override { cppLog.push_back("enable"); }
        void start() override { cppLog.push_back("start"); }
        void onUpdate() { if (!updated) { updated = true; cppLog.push_back("update"); } }
        bool updated = false;
    };
}

// the same for C++ engine events (REGISTER_ENGINE_EVENT)
TEST_F(ScriptLifecycleTest, CppEnabledMidFrameStartsBeforeItsFirstUpdate) {
    cppLog.clear();
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    CppUpdater* updater = nullptr;
    Engine::onUpdate.add("lifecycle-test-enabler", std::function<void()>([&updater]() {
        if (updater && !updater->isEnabled()) updater->setEnabled(true);
    }));
    updater = addCppScript<CppUpdater>(scene, x, "CppUpdater", false);
    ScriptRuntime::awakeScene(scene);
    frame();
    EXPECT_EQ(joined(cppLog), "enable");
    frame();
    EXPECT_EQ(joined(cppLog), "enable start update");
    EXPECT_TRUE(updater->isActiveAndEnabled());
    Engine::onUpdate.remove("lifecycle-test-enabler");
    ScriptRuntime::unloadScenes({scene});
    deleteCppScripts(scene);
}

// onDestroy (and onDisable) still find the GameObject's other scripts: they go after it
TEST_F(ScriptLifecycleTest, GetScriptFromOnDestroy) {
    writeScript("A1", "local M = {}\nfunction M:onDestroy() end\nreturn M\n");
    writeScript("B2",
        "local M = {}\n"
        "function M:onDisable() log[#log + 1] = 'disable' .. tostring(self:getScript('A1') ~= nil) end\n"
        "function M:onDestroy() log[#log + 1] = 'destroy' .. tostring(self:getScript('A1') ~= nil) .. "
        "tostring(GameObject.findObjectOfType('A1', true) ~= nil) end\n"
        "return M\n");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "A1");
    addScript(scene, x, "B2");
    setScene("testScene", scene);
    Engine::setScene(scene);
    LuaBinding::initializeLuaScripts(scene);
    lua("GameObject.destroyImmediate(GameObject(testScene, " + std::to_string(x) + "))");
    EXPECT_EQ(takeLog(), "disabletrue destroytruetrue");
    // a scene unload: the other GameObjects' scripts are still reachable in onDestroy
    const Entity y = entity(scene, "Y");
    const Entity z = entity(scene, "Z");
    addScript(scene, y, "B2");
    addScript(scene, z, "A1");
    ObjectManager::initializeScripts(scene, {y, z});
    lua("other = GameObject(testScene, " + std::to_string(z) + ")");
    writeScript("C3", "local M = {}\nfunction M:onDestroy() log[#log + 1] = 'cross' .. tostring(other:getScript('A1') ~= nil) end\nreturn M\n");
    const Entity w = entity(scene, "W");
    addScript(scene, w, "C3");
    ObjectManager::initializeScripts(scene, {w});
    clearLog();
    ScriptRuntime::unloadScenes({scene});
    EXPECT_NE(log().find("crosstrue"), std::string::npos) << log();
    lua("other = nil");
    Engine::removeAllScenes();
}

// a released script is not kept alive by the Event listeners it owned
TEST_F(ScriptLifecycleTest, EventListenersDoNotKeepReleasedScriptsAlive) {
    writeScript("V1",
        "local M = {}\n"
        "function M:init() shared:addListener(self, 'm'); RegisterEvent(self, shared, 'm'); weakRefs[self] = true end\n"
        "function M:m() end\n"
        "return M\n");
    lua("shared = Event(); weakRefs = setmetatable({}, {__mode = 'k'})");
    Scene* scene = newScene();
    addScript(scene, entity(scene, "X"), "V1");
    LuaBinding::initializeLuaScripts(scene);
    EXPECT_EQ(lua("return shared.listenerCount"), "2");
    LuaBinding::cleanupLuaScripts(scene);
    EXPECT_EQ(lua("return shared.listenerCount"), "0") << "dropped at release, not lazily";
    EXPECT_EQ(lua("collectgarbage(); collectgarbage(); local n = 0; for _ in pairs(weakRefs) do n = n + 1 end; return n"), "0")
        << "the released instance was collected";
    lua("shared = nil; weakRefs = nil");
}

// writing the enabled flag as the Inspector's command does (then refreshEntity): callbacks that
// instantiate many GameObjects (the component arrays move) or destroy one at once are safe
TEST_F(ScriptLifecycleTest, EnabledWriteRunsCallbacksThatMoveComponents) {
    writeScript("S1",
        "local M = {}\n"
        "function M:onEnable() if self.gameObject.name == 'X' then for i = 1, 64 do GameObject.instantiate(self.gameObject) end end end\n"
        "function M:onDisable() if self.gameObject.name == 'X' then local v = GameObject.find('V'); if v then GameObject.destroyImmediate(v) end end end\n"
        "return M\n");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    const Entity v = entity(scene, "V");
    addScript(scene, x, "S1", false);
    Engine::setScene(scene);
    LuaBinding::initializeLuaScripts(scene);
    const size_t before = scene->getEntityList().size();
    scene->getComponent<ScriptComponent>(x).scripts[0].enabled = true;
    ScriptRuntime::refreshEntity(scene, x);
    EXPECT_GE(scene->getEntityList().size(), before + 64);
    // read again after the callbacks (the arrays moved)
    scene->getComponent<ScriptComponent>(x).scripts[0].enabled = false;
    ScriptRuntime::refreshEntity(scene, x);
    EXPECT_FALSE(scene->isEntityCreated(v));
    Engine::removeAllScenes();
}

// a class with an __index of its own: a function that raises for what it lacks ("strict"
// classes) and a base table (inheritance); engine lookups never raise through it
TEST_F(ScriptLifecycleTest, ClassesWithTheirOwnIndex) {
    writeScript("Z1",
        "local M = {}\n"
        "function M:init() log[#log + 1] = 'init' .. tostring(self.enabled) .. tostring(self.gameObject.name) end\n"
        "setmetatable(M, {__index = function(_, k) error('strict: no member ' .. tostring(k)) end})\n"
        "return M\n");
    writeScript("Base", "local Base = {}\nfunction Base:onEnable() log[#log + 1] = 'base enable ' .. self.__name end\nreturn Base\n");
    writeScript("Derived",
        "local Base = require('Base')\n"
        "local M = setmetatable({}, {__index = Base})\n"
        "function M:start() log[#log + 1] = 'start ' .. tostring(self:getScript('Z1') ~= nil) .. tostring(self.isActiveAndEnabled) end\n"
        "return M\n");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "Z1");
    addScript(scene, x, "Derived");
    setScene("testScene", scene);
    LuaBinding::initializeLuaScripts(scene);
    EXPECT_EQ(takeLog(), "inittrueX base enable Derived");
    frame();
    EXPECT_EQ(takeLog(), "start truetrue");
    EXPECT_EQ(lua("local s = GameObject(testScene, " + std::to_string(x) + "):getScript('Z1'); s.enabled = false; "
                  "return tostring(s.enabled) .. tostring(s.isActiveAndEnabled)"), "falsefalse");
    EXPECT_NE(luaError("local s = GameObject(testScene, " + std::to_string(x) + "):getScript('Z1'); return s.missing").find("strict"),
              std::string::npos) << "the class's own rule still applies to its fields";
    ScriptRuntime::quitAll();
    EXPECT_EQ(takeLog(), "") << "messages the strict class lacks are skipped";
}

// a class defining one of the engine's members does not run
TEST_F(ScriptLifecycleTest, ClassFieldsWithReservedNames) {
    writeScript("R1", "local M = { enabled = true }\nfunction M:init() log[#log + 1] = 'init' end\nreturn M\n");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "R1");
    LuaBinding::initializeLuaScripts(scene);
    EXPECT_EQ(takeLog(), "");
    EXPECT_TRUE(logged("its table defines 'enabled'"));
}

// method calls and missing-field reads on self cost what they cost on a plain table
TEST_F(ScriptLifecycleTest, InstanceLookupsStayTableLookups) {
    writeScript("P1",
        "local M = {}\n"
        "function M:noop() end\n"
        "local function best(f) local b = math.huge; for _ = 1, 3 do local t = os.clock(); f(); b = math.min(b, os.clock() - t) end; return b end\n"
        "function M:bench(n)\n"
        "  local plain = setmetatable({}, {__index = {noop = M.noop}})\n"
        "  local a = best(function() for i = 1, n do self:noop() end end)\n"
        "  local b = best(function() for i = 1, n do plain:noop() end end)\n"
        "  local c = best(function() for i = 1, n do local _ = self.missingField end end)\n"
        "  local d = best(function() for i = 1, n do local _ = plain.missingField end end)\n"
        "  return string.format('%.3f %.3f', a / b, c / d)\n"
        "end\n"
        "return M\n");
    Scene* scene = newScene();
    const Entity x = entity(scene, "X");
    addScript(scene, x, "P1");
    setScene("testScene", scene);
    LuaBinding::initializeLuaScripts(scene);
    const std::string ratios = lua("return GameObject(testScene, " + std::to_string(x) + "):getScript('P1'):bench(1000000)");
    double method = 0, missing = 0;
    ASSERT_EQ(std::sscanf(ratios.c_str(), "%lf %lf", &method, &missing), 2) << ratios;
    std::printf("[lookup cost vs a plain table] method call %.2fx, missing field %.2fx\n", method, missing);
    EXPECT_LT(method, 1.5) << ratios;
    EXPECT_LT(missing, 1.5) << ratios;
}

// a C++ script that does not override a message is learned to the first time it is sent (its base
// method ran): no longer among the receivers, never sent again
TEST_F(ScriptLifecycleTest, CppOverridesLearnedWhenSent) {
    Scene* scene = newScene();
    const Entity p = entity(scene, "P");
    CppPartial* partial = addCppScript<CppPartial>(scene, p, "CppPartial");
    EXPECT_TRUE(ScriptMessages::receivers(scene, p) & scriptMessageBit(ScriptMessage::Init)) << "not known yet";
    ScriptRuntime::awakeScene(scene);
    const ScriptMessageMask mask = ScriptMessages::receivers(scene, p);
    EXPECT_FALSE(mask & scriptMessageBit(ScriptMessage::Init)) << "its base init ran";
    EXPECT_TRUE(mask & scriptMessageBit(ScriptMessage::Enable)) << "an override";
    EXPECT_TRUE(partial->inheritedMessages & scriptMessageBit(ScriptMessage::Init));
    EXPECT_EQ(ScriptMessages::send(scene, p, ScriptMessage::ApplicationPause, nullptr,
                                   [](ScriptBase& script) { script.onApplicationPause(true); }), 1);
    EXPECT_EQ(ScriptMessages::send(scene, p, ScriptMessage::ApplicationQuit, nullptr,
                                   [](ScriptBase& script) { script.onApplicationQuit(); }), 0) << "not overridden: not counted";
    EXPECT_FALSE(ScriptMessages::receivers(scene, p) & scriptMessageBit(ScriptMessage::ApplicationQuit));
    ScriptRuntime::unloadScenes({scene});
    deleteCppScripts(scene);
}
