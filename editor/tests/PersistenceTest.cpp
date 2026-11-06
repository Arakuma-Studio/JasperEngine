// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Persistent objects (persistence spec 11): DontDestroyOnLoad moves a root GameObject with its
// children, script instances, sounds and bodies into the persistent scene, which survives
// SceneManager loads. The scenes are built in code by SceneManager factories, the way the
// generated load_X of an exported game builds them (the exported path).

#include "ScriptTestHarness.h"

#include "script/ScriptBase.h"
#include "memory/HeapTrim.h"
#include "jobs/JobSystem.h"
#include "registry/EntityCloner.h"
#include "component/AudioSourceComponent.h"
#include "component/CameraComponent.h"
#include "component/ActionComponent.h"
#include "subsystem/AudioSystem.h"
#include "object/GameObject.h"
#include "component/ButtonComponent.h"
#ifdef JASPERENGINE_PHYSICS_3D
#include "object/physics/Rigidbody.h"
#include "component/RigidbodyComponent.h"
#endif
#ifdef JASPERENGINE_PHYSICS_2D
#include "object/physics/Rigidbody2D.h"
#include "component/Rigidbody2DComponent.h"
#include "component/Joint2DComponent.h"
#endif

#include "soloud.h"

#ifndef _WIN32
#include "net/Http.h"
#include "thread/MainThreadQueue.h"
#include "util/TestHttpServer.h"
#include <chrono>
#include <thread>
#endif

#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::scripttest;

namespace {

    class PersistenceTest : public ScriptTest {
    protected:
        void SetUp() override {
            ScriptTest::SetUp();
            // the exported game's: a Scene of SceneManager's own
            SceneManager::setPersistentSceneFactory(nullptr, nullptr);
        }

        void TearDown() override {
            ScriptRuntime::quitAll();
            ScriptTest::TearDown();
            EXPECT_EQ(SceneManager::getPersistentScene(), nullptr) << "clearAll deletes the persistent scene";
        }

        // A manager script: persists itself in init, counts its updates, logs its lifecycle
        void writeManager(const std::string& className = "Manager") {
            writeScript(className,
                "local M = {}\n"
                "function M:init()\n"
                "  log[#log + 1] = 'init' .. self.__name\n"
                "  DontDestroyOnLoad(self)\n"
                "  self.counter = 41\n"
                "  self.updates = 0\n"
                "  _G[self.__name .. 'Ref'] = self\n"
                "  RegisterEngineEvent(self, 'onUpdate')\n"
                "end\n"
                "function M:onUpdate() self.updates = self.updates + 1 end\n"
                "function M:onDisable() log[#log + 1] = 'disable' .. self.__name end\n"
                "function M:onDestroy() log[#log + 1] = 'destroy' .. self.__name end\n"
                "function M:onApplicationQuit() log[#log + 1] = 'quit' .. self.__name end\n"
                "return M\n");
        }

        // A level script that logs its destruction
        void writeLevel() {
            writeScript("Level",
                "local M = {}\n"
                "function M:onDestroy() log[#log + 1] = 'destroy' .. self.scene:getEntityName(self.entity) end\n"
                "return M\n");
        }

        // The Scene a Lua expression gives
        Scene* luaScene(const std::string& expression) {
            lua_State* state = L();
            const int top = lua_gettop(state);
            Scene* scene = nullptr;
            if (luaL_loadstring(state, ("return " + expression).c_str()) == LUA_OK && LuaBinding::pcallWithTraceback(state, 0, 1) == LUA_OK) {
                if (luabridge::isInstance<Scene*>(state, -1)) {
                    auto result = luabridge::get<Scene*>(state, -1);
                    if (result) scene = result.value();
                }
            }
            lua_settop(state, top);
            return scene;
        }

        // Every GameObject of the persistent scene named `name`
        std::vector<Entity> persistentNamed(const std::string& name) {
            std::vector<Entity> found;
            Scene* persistent = SceneManager::getPersistentScene();
            if (!persistent) return found;
            for (Entity e : persistent->getEntityList()) {
                if (persistent->getEntityName(e) == name) found.push_back(e);
            }
            return found;
        }

        // Scene "A": a Manager root with a child and a Level object; scene "B": a Level object
        Scene* buildA(const std::string& managerClass = "Manager") {
            Scene* scene = newScene();
            const Entity manager = entity(scene, "Manager");
            entity(scene, "Child", manager);
            addScript(scene, manager, managerClass);
            const Entity level = entity(scene, "LevelA");
            addScript(scene, level, "Level");
            return scene;
        }

        Scene* buildB() {
            Scene* scene = newScene();
            addScript(scene, entity(scene, "LevelB"), "Level");
            return scene;
        }

        void registerAB(const std::string& managerClass = "Manager") {
            SceneManager::registerScene(1, "A", [this, managerClass]() {
                Scene* scene = buildA(managerClass);
                LuaBinding::initializeLuaScripts(scene);
                Engine::setScene(scene);
            });
            SceneManager::registerScene(2, "B", [this]() {
                Scene* scene = buildB();
                LuaBinding::initializeLuaScripts(scene);
                Engine::setScene(scene);
            });
        }

        // a deferred load (asked while the game runs) applied by the next frame
        void loadAndFrame(const std::string& name) {
            ASSERT_TRUE(SceneManager::loadScene(name));
            frame();
        }
    };

    // A mono 16-bit PCM WAV of silence
    void writeWav(const std::filesystem::path& path, int samples) {
        std::ofstream out(path, std::ios::binary);
        auto u32 = [&out](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&out](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        const uint32_t dataSize = static_cast<uint32_t>(samples) * 2;
        out.write("RIFF", 4); u32(36 + dataSize); out.write("WAVE", 4);
        out.write("fmt ", 4); u32(16); u16(1); u16(1); u32(8000); u32(16000); u16(2); u16(16);
        out.write("data", 4); u32(dataSize);
        for (int i = 0; i < samples; i++) u16(0);
    }

    std::vector<std::string> cppLog;

    class CppManager : public ScriptBase {
    public:
        CppManager(Scene* scene, Entity entity) : ScriptBase(scene, entity) {}
        bool duplicate = false;
        void init() override {
            if (persistSingleton<CppManager>() != this) {
                duplicate = true;
                cppLog.push_back("duplicate");
                return;
            }
            cppLog.push_back("first");
        }
        void onDestroy() override { cppLog.push_back(std::string("destroy") + (duplicate ? "Dup" : "First")); }
    };

    // a C++ script entry filled in as the generated factory does
    CppManager* addCppManager(Scene* scene, Entity e) {
        ScriptEntry entry;
        entry.type = ScriptType::CPP;
        entry.className = "CppManager";
        entry.enabled = true;
        CppManager* script = new CppManager(scene, e);
        entry.instance = script;
        entry.cppScript = script;
        entry.cppMessageMask = ScriptMessages::cppMask<CppManager>();
        ScriptComponent component;
        component.scripts.push_back(entry);
        scene->addComponent<ScriptComponent>(e, component);
        ScriptMessages::notifyReceiversChanged(scene, e);
        return script;
    }

    // what the generated cleanupEntityScripts does for the test's class
    void cleanupCppScripts(Scene* scene, const Entity* entities, size_t count) {
        for (size_t i = 0; i < count; i++) {
            ScriptComponent* scripts = scene->isEntityCreated(entities[i]) ? scene->findComponent<ScriptComponent>(entities[i]) : nullptr;
            if (!scripts) continue;
            for (ScriptEntry& entry : scripts->scripts) {
                if (entry.type == ScriptType::CPP && entry.instance) {
                    ScriptRuntime::forgetInstance(entry.instance);
                    delete static_cast<CppManager*>(entry.instance);
                    entry.instance = nullptr;
                    entry.cppScript = nullptr;
                }
            }
        }
    }

    void deleteCppScripts(Scene* scene) {
        if (!scene) return;
        auto scripts = scene->getComponentArray<ScriptComponent>();
        for (size_t i = 0; i < scripts->size(); i++) {
            for (ScriptEntry& entry : scripts->getComponentFromIndex(i).scripts) {
                if (entry.type == ScriptType::CPP && entry.instance) {
                    ScriptRuntime::forgetInstance(entry.instance);
                    delete static_cast<CppManager*>(entry.instance);
                    entry.instance = nullptr;
                    entry.cppScript = nullptr;
                }
            }
        }
    }

}

// A finished scene load asks for a heap trim (mesh-memory spec 6.8): what the replaced scene and the
// decoders freed goes back to the system (glibc Linux, Apple, Android; nothing elsewhere)
TEST_F(PersistenceTest, ASceneLoadRequestsAHeapTrim) {
    writeManager();
    writeLevel();
    registerAB();
    ASSERT_TRUE(SceneManager::loadScene("A"));
    const uint64_t before = memory::heapTrimCount();
    loadAndFrame("B");
#if (defined(__linux__) && defined(__GLIBC__)) || defined(__APPLE__)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (memory::heapTrimCount() == before && std::chrono::steady_clock::now() < deadline) {
        jobs::pump(5.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_GT(memory::heapTrimCount(), before) << "the load's end asked for a trim";
#else
    EXPECT_EQ(memory::heapTrimCount(), before);
#endif
}

// 1. DontDestroyOnLoad(self) in init: the object (with its child) lives on in the persistent
//    scene with the same instance table; the level's objects are destroyed by the load
TEST_F(PersistenceTest, SurvivesLoadWithStateAndChild) {
    writeManager();
    writeLevel();
    registerAB();

    ASSERT_TRUE(SceneManager::loadScene("A"));
    Scene* a = Engine::getMainScene();
    ASSERT_NE(SceneManager::getPersistentScene(), nullptr);
    EXPECT_EQ(persistentNamed("Manager").size(), 1u) << "moved at the end of the scene's start, before its documents";
    EXPECT_EQ(a->findEntity("Manager"), NULL_ENTITY);
    EXPECT_EQ(lua("return GameObject.isPersistent(ManagerRef)"), "true");
    lua("ManagerRef.marker = 'kept'; savedManager = ManagerRef");
    EXPECT_EQ(takeLog(), "initManager");

    loadAndFrame("B");
    EXPECT_EQ(takeLog(), "destroyLevelA") << "the level's objects go, the manager gets no callback";
    ASSERT_EQ(persistentNamed("Manager").size(), 1u);
    ASSERT_EQ(persistentNamed("Child").size(), 1u);
    Scene* persistent = SceneManager::getPersistentScene();
    const Entity manager = persistentNamed("Manager")[0];
    EXPECT_EQ(persistent->getComponent<Transform>(persistentNamed("Child")[0]).parent, manager);
    EXPECT_EQ(luaScene("ManagerRef.scene"), persistent);
    EXPECT_EQ(lua("return ManagerRef.entity"), std::to_string(manager));
    EXPECT_EQ(lua("return tostring(rawequal(savedManager, ManagerRef)) .. ManagerRef.marker .. ManagerRef.counter"), "truekept41");
    EXPECT_EQ(lua("return rawequal(ManagerRef.gameObject:getScript('Manager'), ManagerRef)"), "true");
    EXPECT_EQ(luaScene("SceneManager.persistentScene"), persistent);
    EXPECT_EQ(lua("return GameObject.findObjectOfType('Manager') ~= nil"), "true") << "finds see persistent objects";
}

// 2. the entity id is kept when free; a collision gets a new id and self.entity follows
TEST_F(PersistenceTest, EntityIdsKeptOrRemapped) {
    writeManager("First");
    writeManager("Second");
    Scene* one = newScene();
    const Entity first = entity(one, "First");
    addScript(one, first, "First");
    Scene* two = newScene();
    const Entity second = entity(two, "Second");
    addScript(two, second, "Second");
    ASSERT_EQ(first, second) << "both scenes count from the same first id";

    Engine::setScene(one);
    LuaBinding::initializeLuaScripts(one);
    frame();
    ASSERT_EQ(persistentNamed("First").size(), 1u);
    EXPECT_EQ(persistentNamed("First")[0], first) << "kept: free in the persistent scene";

    Engine::addSceneLayer(two);
    LuaBinding::initializeLuaScripts(two);
    frame();
    ASSERT_EQ(persistentNamed("Second").size(), 1u);
    const Entity moved = persistentNamed("Second")[0];
    EXPECT_NE(moved, second) << "taken: a new id";
    EXPECT_EQ(lua("return SecondRef.entity"), std::to_string(moved));
    EXPECT_EQ(lua("return SecondRef.gameObject.name"), "Second");
    Engine::removeAllScenes();
}

// 3 (timers). Engine events registered before the load keep running across it
TEST_F(PersistenceTest, EngineEventsContinueAcrossLoad) {
    writeManager();
    writeLevel();
    registerAB();
    ASSERT_TRUE(SceneManager::loadScene("A"));
    frame();
    frame();
    const int before = std::stoi(lua("return ManagerRef.updates"));
    EXPECT_GE(before, 1);
    loadAndFrame("B");
    frame();
    EXPECT_GT(std::stoi(lua("return ManagerRef.updates")), before);
}

// 4. a playing AudioSource keeps its voice: the handle goes with it, nothing stops it
TEST_F(PersistenceTest, PlayingSoundKeepsItsVoice) {
    writeWav(dir / "loop.wav", 8000);
    Scene* scene = newScene();
    const Entity music = entity(scene, "Music");
    AudioSourceComponent audio;
    audio.filename = "loop.wav";
    audio.looping = true;
    audio.state = AudioSourceState::Playing;
    scene->addComponent<AudioSourceComponent>(music, audio);
    Engine::setScene(scene);
    LuaBinding::initializeLuaScripts(scene);
    for (int i = 0; i < 30 && scene->getComponent<AudioSourceComponent>(music).handle == 0 && AudioSystem::isOutputAvailable(); i++) {
        frame();
    }
    AudioSourceComponent& playing = scene->getComponent<AudioSourceComponent>(music);
    const bool realVoice = playing.handle != 0;
    if (!realVoice) {
        // no audio output here: the hand-over is checked on a handle the test makes up (and
        // no frame runs with it)
        playing.handle = 0x5A5A;
        playing.loaded = true;
        playing.state = AudioSourceState::Playing;
    }
    const unsigned int handle = playing.handle;

    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(scene, music));
    SceneManager::flushPersistentMoves();
    ASSERT_EQ(persistentNamed("Music").size(), 1u);
    AudioSourceComponent& moved = SceneManager::getPersistentScene()->getComponent<AudioSourceComponent>(persistentNamed("Music")[0]);
    EXPECT_EQ(moved.handle, handle);
    EXPECT_EQ(moved.state, AudioSourceState::Playing);
    if (realVoice) {
        EXPECT_TRUE(AudioSystem::getSoloud().isValidVoiceHandle(handle)) << "the voice plays on";
        EXPECT_NE(moved.sample, nullptr);
    } else {
        moved.handle = 0;
    }
    Engine::removeAllScenes();
}

#ifdef JASPERENGINE_PHYSICS_2D
// 5. a Rigidbody 2D keeps its velocity (its body is made again in the persistent world)
TEST_F(PersistenceTest, Rigidbody2DKeepsVelocity) {
    Scene* scene = newScene();
    scene->setGravity2D(0, 0);
    const Entity ball = entity(scene, "Ball");
    scene->addComponent<Rigidbody2DComponent>(ball, Rigidbody2DComponent());
    Rigidbody2D body(scene, ball);
    body.setType(BodyType::DYNAMIC);
    body.createCircleCollider(Vector2(0, 0), 5.0f);
    Engine::setScene(scene);
    frame();
    body.setLinearVelocity(Vector2(10, 0));
    frame();
    ASSERT_NEAR(body.getLinearVelocity().x, 10.0f, 0.01f);

    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(scene, ball));
    SceneManager::getPersistentScene()->setGravity2D(0, 0);
    frame();
    frame();
    ASSERT_EQ(persistentNamed("Ball").size(), 1u);
    Rigidbody2D moved(SceneManager::getPersistentScene(), persistentNamed("Ball")[0]);
    EXPECT_NEAR(moved.getLinearVelocity().x, 10.0f, 0.01f);
    EXPECT_NEAR(moved.getLinearVelocity().y, 0.0f, 0.01f);
    Engine::removeAllScenes();
}

// a joint to a body that stays behind cannot come along (separate worlds)
TEST_F(PersistenceTest, JointToOutsideBodyIsDropped) {
    Scene* scene = newScene();
    const Entity anchor = entity(scene, "Anchor");
    const Entity swing = entity(scene, "Swing");
    scene->addComponent<Rigidbody2DComponent>(anchor, Rigidbody2DComponent());
    scene->addComponent<Rigidbody2DComponent>(swing, Rigidbody2DComponent());
    Rigidbody2D(scene, anchor).setType(BodyType::STATIC);
    Rigidbody2D(scene, swing).setType(BodyType::DYNAMIC);
    Joint2DComponent joint;
    joint.bodyA = anchor;
    joint.bodyB = swing;
    scene->addComponent<Joint2DComponent>(swing, joint);
    Engine::setScene(scene);
    frame();

    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(scene, swing));
    frame();
    ASSERT_EQ(persistentNamed("Swing").size(), 1u);
    EXPECT_EQ(SceneManager::getPersistentScene()->findComponent<Joint2DComponent>(persistentNamed("Swing")[0]), nullptr);
    EXPECT_TRUE(logged("joint 'Swing' to an object outside the moved hierarchy was removed"));
    Engine::removeAllScenes();
}
#endif

// 6. layer order: the loaded stack, then the persistent scene, then the loading scene
TEST_F(PersistenceTest, LayerOrder) {
    writeManager();
    Scene* main = nullptr;
    Scene* layer = nullptr;
    Scene* hud = newScene();
    Scene* loading = newScene();
    SceneManager::registerScene(1, "A", [this, &main, &layer]() {
        main = buildA();
        layer = newScene();
        LuaBinding::initializeLuaScripts(main);
        LuaBinding::initializeLuaScripts(layer);
        Engine::setScene(main);
        Engine::addSceneLayer(layer);
    });
    SceneManager::registerScene(3, "Hud", []() {}, [hud]() { SceneManager::setScenePtr(3, hud); }, {3});
    SceneManager::registerScene(4, "Loading", []() {}, [loading]() { SceneManager::setScenePtr(4, loading); }, {4});

    ASSERT_TRUE(SceneManager::loadScene("A"));
    Scene* persistent = SceneManager::getPersistentScene();
    ASSERT_NE(persistent, nullptr);
    EXPECT_EQ(Engine::getSceneLayers(), (std::vector<Scene*>{main, layer, persistent}));

    ASSERT_TRUE(SceneManager::addChildScene("Hud"));
    EXPECT_EQ(Engine::getSceneLayers(), (std::vector<Scene*>{main, layer, hud, persistent}));

    ASSERT_TRUE(SceneManager::setLoadingScene("Loading"));
    ASSERT_TRUE(SceneManager::loadScene("A"));
    frame();
    std::vector<Scene*> layers = Engine::getSceneLayers();
    ASSERT_GE(layers.size(), 2u);
    EXPECT_EQ(layers[layers.size() - 2], persistent);
    EXPECT_EQ(layers.back(), loading) << "the loading scene covers everything";
    EXPECT_TRUE(SceneManager::isCoveredByLoading(persistent));
    for (int i = 0; i < 10 && Engine::getMainScene() != main; i++) {
        frame();
    }
    layers = Engine::getSceneLayers();
    ASSERT_GE(layers.size(), 3u);
    EXPECT_EQ(layers[0], main) << "the new stack loaded";
    EXPECT_NE(std::find(layers.begin(), layers.end(), persistent), layers.end());
    const auto persistentAt = std::find(layers.begin(), layers.end(), persistent);
    for (auto it = persistentAt + 1; it != layers.end(); ++it) {
        EXPECT_EQ(*it, loading) << "only the loading scene is above the persistent scene";
    }
    Engine::removeAllScenes();
}

// 7. a GameObject with a parent cannot persist; the singleton pattern keeps one manager
TEST_F(PersistenceTest, NonRootRefusedAndSingleton) {
    Scene* scene = newScene();
    const Entity parent = entity(scene, "Parent");
    const Entity child = entity(scene, "Kid", parent);
    EXPECT_FALSE(SceneManager::dontDestroyOnLoad(scene, child));
    EXPECT_TRUE(logged("DontDestroyOnLoad only works for root GameObjects ('Kid' has a parent)"));
    EXPECT_EQ(SceneManager::getPersistentScene(), nullptr) << "nothing happened";

    writeScript("Singleton",
        "local M = {}\n"
        "function M:init()\n"
        "  if GameObject.persistSingleton(self) ~= self then log[#log + 1] = 'duplicate'; return end\n"
        "  log[#log + 1] = 'first'\n"
        "end\n"
        "function M:onDestroy() log[#log + 1] = 'destroyed' end\n"
        "return M\n");
    writeLevel();
    registerAB("Singleton");
    ASSERT_TRUE(SceneManager::loadScene("A"));
    loadAndFrame("A");
    loadAndFrame("A");
    frame();
    EXPECT_EQ(takeLog(), "first destroyLevelA duplicate destroyed destroyLevelA duplicate destroyed")
        << "each copy loaded again finds the first and goes";
    EXPECT_EQ(persistentNamed("Manager").size(), 1u);
    EXPECT_EQ(lua("return #GameObject.findObjectsOfType('Singleton')"), "1");
}

// two duplicates starting in one pass: the first (pass order) wins, the second sees it queued
TEST_F(PersistenceTest, SingletonDuplicatesInOnePass) {
    writeScript("Singleton",
        "local M = {}\n"
        "function M:init() if GameObject.persistSingleton(self) ~= self then log[#log + 1] = 'dup' .. self.gameObject.name; return end end\n"
        "return M\n");
    Scene* scene = newScene();
    addScript(scene, entity(scene, "One"), "Singleton");
    addScript(scene, entity(scene, "Two"), "Singleton");
    Engine::setScene(scene);
    LuaBinding::initializeLuaScripts(scene);
    frame();
    EXPECT_EQ(takeLog(), "dupTwo");
    EXPECT_EQ(persistentNamed("One").size(), 1u);
    EXPECT_EQ(scene->findEntity("Two"), NULL_ENTITY) << "the duplicate was destroyed";
    Engine::removeAllScenes();
}

// 8. moveGameObjectToScene back into the loaded scene keeps the instance
TEST_F(PersistenceTest, MoveGameObjectToScene) {
    writeManager();
    writeLevel();
    registerAB();
    ASSERT_TRUE(SceneManager::loadScene("A"));
    loadAndFrame("B");
    Scene* b = Engine::getMainScene();
    lua("savedManager = ManagerRef");
    EXPECT_EQ(lua("return SceneManager.moveGameObjectToScene(ManagerRef, Engine.scene)"), "true");
    frame();
    EXPECT_TRUE(persistentNamed("Manager").empty());
    ASSERT_NE(b->findEntity("Manager"), NULL_ENTITY);
    EXPECT_EQ(luaScene("ManagerRef.scene"), b);
    EXPECT_EQ(lua("return rawequal(savedManager, ManagerRef)"), "true");
    EXPECT_EQ(lua("return GameObject.isPersistent(ManagerRef)"), "false");
    EXPECT_NE(luaError("SceneManager.moveGameObjectToScene(ManagerRef, 42)").find("argument 2 must be a Scene"), std::string::npos);
    Scene* stopped = newScene();
    setScene("stoppedScene", stopped);
    EXPECT_EQ(lua("return SceneManager.moveGameObjectToScene(ManagerRef, stoppedScene)"), "false");
    EXPECT_TRUE(logged("the target scene is not loaded"));

    // now it is a level object again: the next load destroys it
    clearLog();
    loadAndFrame("A");
    EXPECT_NE(takeLog().find("destroyManager"), std::string::npos);
}

// 9. quit: onApplicationQuit, onDisable, onDestroy for persistent scripts; then the scene goes
TEST_F(PersistenceTest, QuitRunsCallbacksAndDeletesScene) {
    writeManager();
    writeLevel();
    registerAB();
    ASSERT_TRUE(SceneManager::loadScene("A"));
    loadAndFrame("B");
    clearLog();
    ScriptRuntime::quitAll();
    EXPECT_EQ(takeLog(), "quitManager disableManager destroyLevelB destroyManager")
        << "the persistent scene is the last running scene";
    SceneManager::destroyPersistentScene();
    EXPECT_EQ(SceneManager::getPersistentScene(), nullptr);
    EXPECT_EQ(SceneManager::getScenePtr(SceneManager::PERSISTENT_SCENE_ID), nullptr);
    EXPECT_EQ(takeLog(), "") << "released only";
    Engine::removeAllScenes();
}

// 10. destroyed before its move: no move; destroyed with a delay: it goes later, persistent
TEST_F(PersistenceTest, DestroyAndMoveInOneFrame) {
    Scene* scene = newScene();
    const Entity gone = entity(scene, "Gone");
    const Entity later = entity(scene, "Later");
    Engine::setScene(scene);
    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(scene, gone));
    EXPECT_TRUE(SceneManager::isPersistent(scene, gone)) << "from the call on";
    ObjectManager::destroyImmediate(scene, gone);
    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(scene, later));
    ObjectManager::destroy(scene, later, 0.5f);
    frame();
    EXPECT_TRUE(persistentNamed("Gone").empty());
    ASSERT_EQ(persistentNamed("Later").size(), 1u) << "moved, its destroy follows it";
    for (int i = 0; i < 40; i++) {
        frame();
    }
    EXPECT_TRUE(persistentNamed("Later").empty()) << "destroyed half a second later in the persistent scene";
    Engine::removeAllScenes();
}

// 11. the engine event guard reads where the script is now: deactivating the moved object
//     stops its onUpdate
TEST_F(PersistenceTest, EngineEventGuardFollowsTheMove) {
    writeManager();
    writeLevel();
    registerAB();
    ASSERT_TRUE(SceneManager::loadScene("A"));
    loadAndFrame("B");
    frame();
    const int running = std::stoi(lua("return ManagerRef.updates"));
    frame();
    EXPECT_EQ(std::stoi(lua("return ManagerRef.updates")), running + 1);
    lua("ManagerRef.gameObject:setActive(false)");
    frame();
    frame();
    EXPECT_EQ(std::stoi(lua("return ManagerRef.updates")), running + 1) << "inactive: no onUpdate";
    lua("ManagerRef.gameObject:setActive(true)");
    frame();
    EXPECT_EQ(std::stoi(lua("return ManagerRef.updates")), running + 2);
}

// running actions, child GameObjects and wrappers held by the script follow the move
TEST_F(PersistenceTest, ScriptWrappersAndActionsFollow) {
    writeScript("Holder",
        "local M = {}\n"
        "function M:init()\n"
        "  self.kid = GameObject(self.scene, self.scene:findEntity('Kid'))\n"
        "  HolderRef = self\n"
        "end\n"
        "return M\n");
    Scene* scene = newScene();
    const Entity holder = entity(scene, "Holder");
    const Entity kid = entity(scene, "Kid", holder);
    addScript(scene, holder, "Holder");
    ActionComponent action;
    action.state = ActionState::Running;
    action.timecount = 1.5f;
    scene->addComponent<ActionComponent>(kid, action);
    Engine::setScene(scene);
    LuaBinding::initializeLuaScripts(scene);
    // the editor would draw this frame: the action has nothing to do but keep running
    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(scene, holder));
    SceneManager::flushPersistentMoves();
    ASSERT_EQ(persistentNamed("Kid").size(), 1u);
    Scene* persistent = SceneManager::getPersistentScene();
    const Entity movedKid = persistentNamed("Kid")[0];
    EXPECT_EQ(persistent->getComponent<ActionComponent>(movedKid).state, ActionState::Running);
    EXPECT_FLOAT_EQ(persistent->getComponent<ActionComponent>(movedKid).timecount, 1.5f);
    EXPECT_EQ(luaScene("HolderRef.kid.scene"), persistent) << "the wrapper in its field follows";
    EXPECT_EQ(lua("return HolderRef.kid.entity"), std::to_string(movedKid));
    Engine::removeAllScenes();
}

// C++ scripts: ScriptBase::dontDestroyOnLoad / persistSingleton, rebound by the move
TEST_F(PersistenceTest, CppScriptsPersistAndRebind) {
    cppLog.clear();
    ObjectManager::setCppScriptHooks(nullptr, cleanupCppScripts);
    Scene* one = newScene();
    const Entity first = entity(one, "CppFirst");
    CppManager* firstScript = addCppManager(one, first);
    Engine::setScene(one);
    ScriptRuntime::awakeScene(one);
    frame();
    ASSERT_EQ(persistentNamed("CppFirst").size(), 1u);
    EXPECT_EQ(firstScript->getScene(), SceneManager::getPersistentScene());
    EXPECT_EQ(firstScript->getEntity(), persistentNamed("CppFirst")[0]);
    EXPECT_TRUE(firstScript->isPersistent());

    Scene* two = newScene();
    addCppManager(two, entity(two, "CppSecond"));
    Engine::addSceneLayer(two);
    ScriptRuntime::awakeScene(two);
    frame();
    EXPECT_EQ(cppLog, (std::vector<std::string>{"first", "duplicate", "destroyDup"}));
    EXPECT_EQ(two->findEntity("CppSecond"), NULL_ENTITY);

    ScriptRuntime::quitAll();
    EXPECT_EQ(cppLog.back(), "destroyFirst");
    deleteCppScripts(SceneManager::getPersistentScene());
    deleteCppScripts(one);
    deleteCppScripts(two);
    ObjectManager::setCppScriptHooks(nullptr, nullptr);
    Engine::removeAllScenes();
}

// the persistent scene's camera: its default UI camera until a moved GameObject brings one
TEST_F(PersistenceTest, PersistentSceneAdoptsCamera) {
    Scene* scene = newScene();
    const Entity holder = entity(scene, "WithCamera");
    scene->addComponent<CameraComponent>(holder, CameraComponent());
    Engine::setScene(scene);
    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(scene, holder));
    Scene* persistent = SceneManager::getPersistentScene();
    ASSERT_NE(persistent->getCamera(), NULL_ENTITY);
    EXPECT_LE(persistent->getCamera(), EntityManager::lastSystemEntity()) << "its default camera";
    frame();
    ASSERT_EQ(persistentNamed("WithCamera").size(), 1u);
    EXPECT_EQ(persistent->getCamera(), persistentNamed("WithCamera")[0]);

    // moved out again: the persistent scene draws with a default camera again
    ASSERT_TRUE(SceneManager::moveGameObjectToScene(persistent, persistentNamed("WithCamera")[0], scene));
    frame();
    EXPECT_NE(persistent->getCamera(), NULL_ENTITY);
    EXPECT_TRUE(persistent->isEntityCreated(persistent->getCamera()));
    EXPECT_NE(persistent->findComponent<CameraComponent>(persistent->getCamera()), nullptr);
    Engine::removeAllScenes();
}

// ---- review regressions -------------------------------------------------------------------

namespace {

    // a C++ script holding a GameObject wrapper its property owns (bindCppScriptProperties makes
    // one for a GameObject without a script), as the generated code fills it in
    class CppHolder : public ScriptBase {
    public:
        CppHolder(Scene* scene, Entity entity) : ScriptBase(scene, entity) {}
        GameObject* kid = nullptr;
    };

    std::vector<Entity> hookOld;
    std::vector<Entity> hookNew;

    // what the generated moveEntityScripts does for CppHolder (Generator::buildInitSceneScriptsSource)
    void moveCppScripts(Scene* from, Scene* to, const Entity* oldEntities, const Entity* newEntities, size_t count) {
        hookOld.assign(oldEntities, oldEntities + count);
        hookNew.assign(newEntities, newEntities + count);
        for (size_t i = 0; i < count; i++) {
            ScriptComponent* scriptComp = to->isEntityCreated(newEntities[i]) ? to->findComponent<ScriptComponent>(newEntities[i]) : nullptr;
            if (!scriptComp) continue;
            for (auto& scriptEntry : scriptComp->scripts) {
                if (scriptEntry.type == ScriptType::LUA || !scriptEntry.instance) continue;
                if (scriptEntry.className == "CppHolder") {
                    rebindScriptObject(static_cast<CppHolder*>(scriptEntry.instance), to, newEntities[i]);
                    for (auto& prop : scriptEntry.properties) {
                        if (!prop.ownedInstance) continue;
                        if (prop.name == "kid") rebindScriptReference(static_cast<GameObject*>(prop.ownedInstance), from, to, oldEntities, newEntities, count);
                    }
                }
            }
        }
    }

    class OtherBase {
    public:
        virtual ~OtherBase() = default;
        int padding = 7;
    };

    // ScriptBase not the first base: the entry's instance and its ScriptBase are different addresses
    class CppSecondBase : public OtherBase, public ScriptBase {
    public:
        CppSecondBase(Scene* scene, Entity entity) : ScriptBase(scene, entity) {}
        bool duplicate = false;
        void init() override { duplicate = persistSingleton<CppSecondBase>() != this; }
    };

    CppSecondBase* addSecondBase(Scene* scene, Entity e) {
        CppSecondBase* script = new CppSecondBase(scene, e);
        ScriptEntry entry;
        entry.type = ScriptType::CPP;
        entry.className = "CppSecondBase";
        entry.enabled = true;
        entry.instance = static_cast<void*>(script);
        entry.cppScript = ScriptMessages::cppScript<CppSecondBase>(script);
        entry.cppMessageMask = ScriptMessages::cppMask<CppSecondBase>();
        ScriptComponent component;
        component.scripts.push_back(entry);
        scene->addComponent<ScriptComponent>(e, component);
        return script;
    }

}

// the generated C++ move hook gets every moved GameObject, so a wrapper pointing at a child
// without a script follows it (else it names the destroyed original: use after free)
TEST_F(PersistenceTest, CppMoveHookRebindsWrappersOfUnscriptedChildren) {
    Scene* scene = newScene();
    const Entity holder = entity(scene, "Holder");
    const Entity kid = entity(scene, "PlainKid", holder);
    CppHolder* script = new CppHolder(scene, holder);
    GameObject* wrapper = new GameObject(scene, kid);
    script->kid = wrapper;
    ScriptEntry entry;
    entry.type = ScriptType::CPP;
    entry.className = "CppHolder";
    entry.enabled = true;
    entry.instance = script;
    entry.cppScript = script;
    entry.cppMessageMask = ScriptMessages::cppMask<CppHolder>();
    ScriptProperty property;
    property.name = "kid";
    property.type = ScriptPropertyType::EntityReference;
    property.value = EntityReference{kid, 0};
    property.ownedInstance = wrapper;
    entry.properties.push_back(property);
    ScriptComponent component;
    component.scripts.push_back(entry);
    scene->addComponent<ScriptComponent>(holder, component);
    Engine::setScene(scene);
    ObjectManager::setCppScriptMoveHook(moveCppScripts);

    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(scene, holder));
    SceneManager::flushPersistentMoves();
    ObjectManager::setCppScriptMoveHook(nullptr);
    Scene* persistent = SceneManager::getPersistentScene();
    ASSERT_EQ(persistentNamed("PlainKid").size(), 1u);
    EXPECT_EQ(hookOld, (std::vector<Entity>{holder, kid})) << "every moved GameObject, scripted or not";
    EXPECT_EQ(hookNew.size(), 2u);
    EXPECT_EQ(wrapper->getScene(), persistent);
    EXPECT_EQ(wrapper->getEntity(), persistentNamed("PlainKid")[0]);
    EXPECT_EQ(wrapper->getName(), "PlainKid");
    EXPECT_EQ(script->getScene(), persistent);

    ScriptEntry& moved = persistent->getComponent<ScriptComponent>(persistentNamed("Holder")[0]).scripts[0];
    ScriptRuntime::forgetInstance(moved.instance);
    delete static_cast<GameObject*>(moved.properties[0].ownedInstance);
    delete static_cast<CppHolder*>(moved.instance);
    moved.properties[0].ownedInstance = nullptr;
    moved.instance = nullptr;
    moved.cppScript = nullptr;
    Engine::removeAllScenes();
}

// a released script's subscriptions on persistent GameObjects go with it (Lua and the
// generated C++ cleanup share ScriptRuntime::removeSubscriptionsByTag)
TEST_F(PersistenceTest, ReleasedScriptsLeavePersistentEvents) {
    Scene* level = newScene();
    const Entity hud = entity(level, "HudButton");
    level->addComponent<ButtonComponent>(hud, ButtonComponent());
    Engine::setScene(level);
    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(level, hud));
    SceneManager::flushPersistentMoves();
    Scene* persistent = SceneManager::getPersistentScene();
    ButtonComponent& button = persistent->getComponent<ButtonComponent>(persistentNamed("HudButton")[0]);
    int calls = 0;
    // a C++ script's tag ("_<address>_"), and a Lua one's (class_address_method)
    button.onPress.add("_4242_onPress", std::function<void()>([&calls]() { calls++; }));
    button.onRelease.add("Level_4242_onRelease", std::function<void()>([&calls]() { calls++; }));
    ScriptRuntime::removeSubscriptionsByTag(level, "_4242_");
    button.onPress.call();
    button.onRelease.call();
    EXPECT_EQ(calls, 0) << "the level's script is gone: nothing of it is called";

    // the Lua path: a level script listening to the persistent button, its scene unloaded
    writeScript("Listener",
        "local M = {}\n"
        "function M:init() RegisterEvent(self, Button(SceneManager.persistentScene, hudEntity):getButtonComponent().onPress, 'pressed') end\n"
        "function M:pressed() log[#log + 1] = 'pressed' end\n"
        "return M\n");
    lua("hudEntity = " + std::to_string(persistentNamed("HudButton")[0]));
    Scene* listenerScene = nullptr;
    SceneManager::registerScene(1, "L", [this, &listenerScene]() {
        Scene* scene = newScene();
        listenerScene = scene;
        addScript(scene, entity(scene, "Listener"), "Listener");
        LuaBinding::initializeLuaScripts(scene);
        Engine::setScene(scene);
    });
    SceneManager::registerScene(2, "Empty", [this, &listenerScene]() {
        // as the exported load_X: the replaced scene's scripts are released, then it goes
        LuaBinding::cleanupLuaScripts(listenerScene);
        Scene* scene = newScene();
        LuaBinding::initializeLuaScripts(scene);
        Engine::setScene(scene);
    });
    loadAndFrame("L");
    persistent->getComponent<ButtonComponent>(persistentNamed("HudButton")[0]).onPress.call();
    EXPECT_EQ(takeLog(), "pressed");
    loadAndFrame("Empty");
    persistent->getComponent<ButtonComponent>(persistentNamed("HudButton")[0]).onPress.call();
    EXPECT_EQ(takeLog(), "") << "released with its scene";
}

// persistent 3D sounds are heard by the loaded main scene's camera: the persistent scene's
// system places the listener every frame
TEST_F(PersistenceTest, Persistent3DSoundsUseTheMainCameraAsListener) {
    Scene* level = newScene();
    const Entity cameraEntity = entity(level, "LevelCamera");
    // perspective: it hears at its position (an orthographic or UI camera at the centre of its
    // view, AudioSourceTest.A2DCamerasListenerStandsAtTheCentreOfItsView)
    CameraComponent perspective;
    perspective.type = CameraType::CAMERA_PERSPECTIVE;
    level->addComponent<CameraComponent>(cameraEntity, perspective);
    level->getComponent<Transform>(cameraEntity).position = Vector3(5, 6, 7);
    level->getComponent<Transform>(cameraEntity).worldPosition = Vector3(5, 6, 7);
    level->setCamera(cameraEntity);
    const Entity source = entity(level, "Sound3D");
    AudioSourceComponent audio;
    audio.filename = "nothing.wav";
    level->addComponent<AudioSourceComponent>(source, audio);
    Engine::setScene(level);
    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(level, source));
    SceneManager::flushPersistentMoves();
    // asked to play (its file does not exist: no voice, here or without audio output)
    AudioSourceComponent& moved = SceneManager::getPersistentScene()->getComponent<AudioSourceComponent>(persistentNamed("Sound3D")[0]);
    moved.state = AudioSourceState::Playing;
    AudioSystem::getSoloud().set3dListenerParameters(0, 0, 0, 0, 0, -1, 0, 1, 0);
    frame();
    const float* listener = AudioSystem::getSoloud().m3dPosition;
    EXPECT_FLOAT_EQ(listener[0], 5.0f);
    EXPECT_FLOAT_EQ(listener[1], 6.0f);
    EXPECT_FLOAT_EQ(listener[2], 7.0f);
    Engine::removeAllScenes();
}

// a scene's camera made persistent: that scene falls back to a default camera, with a warning
TEST_F(PersistenceTest, PersistingTheLevelCameraWarns) {
    Scene* level = newScene();
    const Entity cameraEntity = entity(level, "MainCamera");
    level->addComponent<CameraComponent>(cameraEntity, CameraComponent());
    level->setCamera(cameraEntity);
    Engine::setScene(level);
    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(level, cameraEntity));
    SceneManager::flushPersistentMoves();
    EXPECT_TRUE(logged("'MainCamera' was the camera of the scene it left"));
    EXPECT_NE(level->getCamera(), NULL_ENTITY);
    EXPECT_TRUE(level->isEntityCreated(level->getCamera()));
    EXPECT_EQ(SceneManager::getPersistentScene()->getCamera(), persistentNamed("MainCamera")[0]);
    Engine::removeAllScenes();
}

// a new level's init sees its own stack as Engine.scene, never the persistent scene
TEST_F(PersistenceTest, EngineSceneDuringALoadIsNotThePersistentScene) {
    writeManager();
    writeLevel();
    writeScript("Peek",
        "local M = {}\n"
        "function M:init() peekedScene = Engine.scene; peekedNil = Engine.scene == nil end\n"
        "return M\n");
    registerAB();
    SceneManager::registerScene(3, "C", [this]() {
        Scene* scene = newScene();
        addScript(scene, entity(scene, "Peeker"), "Peek");
        LuaBinding::initializeLuaScripts(scene);
        Engine::setScene(scene);
    });
    ASSERT_TRUE(SceneManager::loadScene("A"));
    ASSERT_NE(SceneManager::getPersistentScene(), nullptr);
    loadAndFrame("C");
    EXPECT_NE(luaScene("peekedScene"), SceneManager::getPersistentScene());
    EXPECT_EQ(Engine::getScene(), Engine::getMainScene());
}

// moved out and made persistent again in the same frame: it stays persistent
TEST_F(PersistenceTest, MoveOutCalledOffByDontDestroyOnLoad) {
    Scene* level = newScene();
    const Entity keeper = entity(level, "Keeper");
    Engine::setScene(level);
    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(level, keeper));
    frame();
    Scene* persistent = SceneManager::getPersistentScene();
    const Entity moved = persistentNamed("Keeper")[0];
    ASSERT_TRUE(SceneManager::moveGameObjectToScene(persistent, moved, level));
    EXPECT_FALSE(SceneManager::isPersistent(persistent, moved)) << "about to leave";
    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(persistent, moved));
    EXPECT_TRUE(SceneManager::isPersistent(persistent, moved));
    frame();
    EXPECT_EQ(persistentNamed("Keeper").size(), 1u);
    EXPECT_EQ(level->findEntity("Keeper"), NULL_ENTITY);
    Engine::removeAllScenes();
}

// persistSingleton<T> reads the lifecycle state by the entry's instance (not its ScriptBase,
// a different address when ScriptBase is not the first base): a manager being destroyed is no
// longer the survivor
TEST_F(PersistenceTest, CppSingletonIgnoresADestroyedManager) {
    Scene* one = newScene();
    const Entity first = entity(one, "FirstManager");
    CppSecondBase* firstScript = addSecondBase(one, first);
    ASSERT_NE(static_cast<void*>(firstScript), static_cast<void*>(static_cast<ScriptBase*>(firstScript)));
    Engine::setScene(one);
    ScriptRuntime::awakeScene(one);
    frame();
    ASSERT_EQ(persistentNamed("FirstManager").size(), 1u);
    EXPECT_FALSE(firstScript->duplicate);
    Scene* persistent = SceneManager::getPersistentScene();
    // its onDestroy ran (destroyed), the GameObject is still there
    ScriptRuntime::destroyEntities(persistent, {persistentNamed("FirstManager")[0]});

    Scene* two = newScene();
    CppSecondBase* secondScript = addSecondBase(two, entity(two, "SecondManager"));
    Engine::addSceneLayer(two);
    ScriptRuntime::awakeScene(two);
    EXPECT_FALSE(secondScript->duplicate) << "the destroyed one does not count";
    frame();
    EXPECT_EQ(persistentNamed("SecondManager").size(), 1u);

    for (Scene* scene : {persistent, one, two}) {
        auto scripts = scene->getComponentArray<ScriptComponent>();
        for (size_t i = 0; i < scripts->size(); i++) {
            for (ScriptEntry& entry : scripts->getComponentFromIndex(i).scripts) {
                if (entry.instance) {
                    ScriptRuntime::forgetInstance(entry.instance);
                    delete static_cast<CppSecondBase*>(entry.instance);
                    entry.instance = nullptr;
                    entry.cppScript = nullptr;
                }
            }
        }
    }
    Engine::removeAllScenes();
}

#ifdef JASPERENGINE_PHYSICS_3D
// a Rigidbody keeps its velocity (its Jolt body is made again in the persistent world)
TEST_F(PersistenceTest, RigidbodyKeepsVelocity) {
    Scene* scene = newScene();
    scene->setGravity3D(0, 0, 0);
    const Entity ball = entity(scene, "Ball3D");
    scene->addComponent<RigidbodyComponent>(ball, RigidbodyComponent());
    Rigidbody body(scene, ball);
    body.setType(BodyType::DYNAMIC);
    body.createSphereCollider(0.5f);
    Engine::setScene(scene);
    frame();
    body.setLinearVelocity(Vector3(3, 0, -2));
    frame();
    ASSERT_NEAR(body.getLinearVelocity().x, 3.0f, 0.01f);

    ASSERT_TRUE(SceneManager::dontDestroyOnLoad(scene, ball));
    SceneManager::getPersistentScene()->setGravity3D(0, 0, 0);
    frame();
    frame();
    ASSERT_EQ(persistentNamed("Ball3D").size(), 1u);
    Rigidbody moved(SceneManager::getPersistentScene(), persistentNamed("Ball3D")[0]);
    EXPECT_NEAR(moved.getLinearVelocity().x, 3.0f, 0.01f);
    EXPECT_NEAR(moved.getLinearVelocity().y, 0.0f, 0.01f);
    EXPECT_NEAR(moved.getLinearVelocity().z, -2.0f, 0.01f);
    Engine::removeAllScenes();
}
#endif

// a singleton's duplicate (destroyed in its init) gets no start; its onDestroy follows
TEST_F(PersistenceTest, DuplicateDestroyedInInitGetsNoStart) {
    writeScript("Single",
        "local M = {}\n"
        "function M:init() if GameObject.persistSingleton(self) ~= self then log[#log + 1] = 'dup'; return end; log[#log + 1] = 'first' end\n"
        "function M:start() log[#log + 1] = 'start' .. self.gameObject.name end\n"
        "function M:onDestroy() log[#log + 1] = 'destroy' .. self.gameObject.name end\n"
        "return M\n");
    Scene* scene = newScene();
    addScript(scene, entity(scene, "One"), "Single");
    addScript(scene, entity(scene, "Two"), "Single");
    Engine::setScene(scene);
    LuaBinding::initializeLuaScripts(scene);
    frame();
    frame();
    EXPECT_EQ(takeLog(), "first dup startOne destroyTwo");
    Engine::removeAllScenes();
}

#ifndef _WIN32
// HTTP requests (feature http) owned by a GameObject that DontDestroyOnLoad moves, or by its
// script `self`, survive the move and the next scene load and call back; the requests owned by
// the level's objects end with them, the ones owned by the old scene when it is deleted
TEST_F(PersistenceTest, HttpRequestsFollowPersistentObjects) {
    jaspertest::TestHttpServer server;
    ASSERT_TRUE(server.started());
    Http::shutdown();
    lua("got = {}; SLOW = '" + server.url("/slow/400") + "'; LONG = '" + server.url("/slow/3000") + "'");
    writeScript("Fetcher",
        "local M = {}\n"
        "local function note(tag) return function(r) got[#got + 1] = tag .. ':' .. r.errorCode .. r.body end end\n"
        "function M:init()\n"
        "  DontDestroyOnLoad(self)\n"
        "  Http.get(SLOW, {owner = self.gameObject}, note('object'))\n"
        "  Http.get(SLOW, {owner = self}, note('self'))\n"
        "  Http.get(LONG, {owner = self.scene}, note('scene'))\n"
        "end\n"
        "return M\n");
    writeScript("LevelFetcher",
        "local M = {}\n"
        "function M:init() Http.get(LONG, {owner = self}, function(r) got[#got + 1] = 'level' end) end\n"
        "return M\n");
    Scene* levelA = nullptr;
    SceneManager::registerScene(1, "A", [this, &levelA]() {
        levelA = newScene();
        addScript(levelA, entity(levelA, "Fetcher"), "Fetcher");
        addScript(levelA, entity(levelA, "LevelFetcher"), "LevelFetcher");
        LuaBinding::initializeLuaScripts(levelA);
        Engine::setScene(levelA);
    });
    SceneManager::registerScene(2, "B", [this]() {
        Scene* scene = newScene();
        entity(scene, "Floor");
        LuaBinding::initializeLuaScripts(scene);
        Engine::setScene(scene);
    });

    ASSERT_TRUE(SceneManager::loadScene("A"));
    ASSERT_EQ(persistentNamed("Fetcher").size(), 1u);
    loadAndFrame("B");
    EXPECT_EQ(Engine::getMainScene()->findEntity("LevelFetcher"), NULL_ENTITY);

    // the old scene is deleted (what the exported game's loads do): its own request ends
    ASSERT_NE(levelA, nullptr);
    for (auto it = scenes.begin(); it != scenes.end(); ++it) {
        if (it->get() == levelA) {
            LuaBinding::cleanupLuaScripts(levelA);
            ObjectManager::forgetScene(levelA);
            scenes.erase(it);
            break;
        }
    }

    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < end && lua("return #got") != "2") {
        frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(lua("table.sort(got); return table.concat(got, ' ')"), "object:slow self:slow")
        << "the moved object's and its script's requests call back, through the frame";
    EXPECT_EQ(Http::getStats().active, 0u) << "the level's and the deleted scene's requests ended";
    for (int i = 0; i < 20; i++) {
        frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(lua("return #got"), "2") << "no callback for the cancelled ones";
    Http::shutdown();
    MainThreadQueue::clear();
    Engine::removeAllScenes();
}
#endif
