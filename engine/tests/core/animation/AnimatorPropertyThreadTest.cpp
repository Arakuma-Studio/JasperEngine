// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator's property rows and threads (docs/specs/S77-clip-quality.md R8, R9, AC3): Lua
// fields, a script's enabled and GameObject active never run on a worker, whatever the wave's
// chunks. TSan sees the race on the Lua stack; the test also records the thread of each write
// itself, so a run without TSan is red too. The record is lock-free on purpose: a mutex taken on
// the workers would give TSan the happens-before edges that hide the race it is looking for.

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"
#include "support/AllocationCounter.h"

#include "animation/ClipBinding.h"
#include "animation/PropertyBindings.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "component/ScriptComponent.h"
#include "component/SpriteComponent.h"
#include "jobs/JobSystem.h"
#include "script/LuaBinding.h"
#include "script/ScriptRuntime.h"

#include "lua.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::anim;
using namespace animatortest;

namespace {

    constexpr float kDt = 1.0f / 60.0f;
    // more than one chunk of the wave (4 Animators per job), with work for the workers to take
    constexpr int kCharacters = 16;

    enum WriteKind : size_t { LuaHeap, ScriptCallback, ActiveRow, PlainRow, WatchedRow, WriteKindCount };

    // main thread or not, per kind of write
    class ThreadLog{
    public:
        void reset(){
            mainThread = std::this_thread::get_id();
            for (size_t k = 0; k < WriteKindCount; k++){
                onMain[k].store(0, std::memory_order_relaxed);
                onWorkers[k].store(0, std::memory_order_relaxed);
            }
        }
        void record(WriteKind kind){
            (std::this_thread::get_id() == mainThread ? onMain : onWorkers)[kind].fetch_add(1, std::memory_order_relaxed);
        }
        size_t main(WriteKind kind) const{ return onMain[kind].load(std::memory_order_relaxed); }
        size_t workers(WriteKind kind) const{ return onWorkers[kind].load(std::memory_order_relaxed); }

    private:
        std::thread::id mainThread;
        std::array<std::atomic<size_t>, WriteKindCount> onMain{};
        std::array<std::atomic<size_t>, WriteKindCount> onWorkers{};
    };

    ThreadLog threadLog;

    // A row with its write recording the thread: the rig's binding points to it after the bind
    template<WriteKind Kind>
    struct RecordedRow{
        static inline AnimatableProperty row{};
        static inline bool (*original)(Scene*, Entity, const PropertyParams&, const float*) = nullptr;
        static bool write(Scene* scene, Entity entity, const PropertyParams& params, const float* in){
            threadLog.record(Kind);
            return original(scene, entity, params, in);
        }
        static void install(AnimatorInstance& instance, const char* component){
            for (RigProperty& property : instance.rig.properties){
                if (!property.binding.entry || std::strcmp(property.binding.entry->component, component) != 0) continue;
                if (property.binding.entry != &row){
                    row = *property.binding.entry;
                    original = row.write;
                    row.write = &write;
                }
                property.binding.entry = &row;
            }
        }
    };

    // Every call into the Lua allocator: a Vector3 field's write makes a fresh userdata, so the
    // write of such a field is seen here (a float's touches no allocator: TSan's)
    class LuaHeapRecorder{
    public:
        explicit LuaHeapRecorder(lua_State* state): L(state){
            base = lua_getallocf(L, &baseData);
            lua_setallocf(L, &LuaHeapRecorder::allocate, nullptr);
        }
        ~LuaHeapRecorder(){ lua_setallocf(L, base, baseData); }

    private:
        static void* allocate(void*, void* ptr, size_t osize, size_t nsize){
            threadLog.record(LuaHeap);
            return base(baseData, ptr, osize, nsize);
        }
        lua_State* L;
        static inline lua_Alloc base = nullptr;
        static inline void* baseData = nullptr;
    };

    int recordScriptCallback(lua_State*){
        threadLog.record(ScriptCallback);
        return 0;
    }

    std::function<void()> removeHook;
    int callRemoveHook(lua_State*){
        if (removeHook) removeHook();
        return 0;
    }

    ClipTrackData propertyTrack(const std::string& path, const std::string& component, const std::string& name, std::vector<ClipKey> keys){
        ClipTrackData t;
        t.path = path;
        t.channel = ClipChannel::Property;
        t.component = component;
        t.property = name;
        t.keys = std::move(keys);
        return t;
    }

    // on and off, 4 times a second
    std::vector<ClipKey> toggling(){
        return {key(0, {1}), key(0.25f, {0}), key(0.5f, {1}), key(0.75f, {0}), key(1, {1})};
    }

    ControllerDef oneState(const std::string& clip){
        return controller({}, {state("Play", clip)});
    }

    class AnimatorPropertyThread : public ::testing::Test, public AnimatorFixture {
    protected:
        lua_State* L = nullptr;
        std::vector<int> refs;
        std::vector<Entity> withScripts;
        std::vector<Entity> characters;

        void SetUp() override{
            L = LuaBinding::getLuaState();
            ASSERT_TRUE(L != nullptr);
            lua_register(L, "recordScriptCallback", &recordScriptCallback);
            threadLog.reset();
        }

        void TearDown() override{
            ScriptRuntime::forgetScene(&scene);
            for (Entity entity : withScripts){
                if (scene.isEntityCreated(entity)) scene.getComponent<ScriptComponent>(entity).scripts.clear();
            }
            for (int ref : refs) luaL_unref(L, LUA_REGISTRYINDEX, ref);
        }

        // a Lua script of `className` on the entity, with the fields `glow` (float) and `offset` (Vector3)
        int script(Entity entity, const std::string& className){
            EXPECT_EQ(luaL_dostring(L, "threadProbe = {glow = 0, offset = Vector3(0, 0, 0), "
                                       "onEnable = function(self) recordScriptCallback() end, "
                                       "onDisable = function(self) recordScriptCallback() end}"), LUA_OK);
            lua_getglobal(L, "threadProbe");
            refs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
            ScriptEntry entry;
            entry.type = ScriptType::LUA;
            entry.enabled = true;
            entry.className = className;
            entry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(refs.back()));
            ScriptProperty glow;
            glow.name = "glow";
            glow.type = ScriptPropertyType::Float;
            glow.value = 0.0f;
            ScriptProperty offset;
            offset.name = "offset";
            offset.type = ScriptPropertyType::Vector3;
            offset.value = Vector3(0, 0, 0);
            entry.properties = {glow, offset};
            ScriptComponent scripts;
            scripts.scripts.push_back(entry);
            scene.addComponent<ScriptComponent>(entity, scripts);
            withScripts.push_back(entity);
            return refs.back();
        }

        float luaField(int ref, const char* name){
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            lua_pushstring(L, name);
            lua_rawget(L, -2);
            const float value = (float)lua_tonumber(L, -1);
            lua_pop(L, 2);
            return value;
        }

        Entity lamp(Entity parent, const std::string& name){
            const Entity entity = node(parent, name, Vector3(0, 0, 0));
            LightComponent light;
            light.intensity = 1.0f;
            scene.addComponent<LightComponent>(entity, light);
            return entity;
        }

        // Animators keying a Lua field, a script's enabled, GameObject active and a plain row
        void buildCharacters(const std::string& controllerPath){
            for (int i = 0; i < kCharacters; i++){
                const Entity character = node(NULL_ENTITY, "Character", Vector3((float)i, 0, 0));
                node(character, "Hips", Vector3(0, 1, 0));
                lamp(character, "Lamp");
                script(character, "Probe");
                script(node(character, "Door", Vector3(0, 0, 0)), "Door");
                animator(controllerPath, character);
                characters.push_back(character);
            }
        }

        ClipData everyRowType(){
            ClipData data = slide("Hips", 1.0f);
            data.tracks.push_back(propertyTrack("", "script:Probe", "glow", {key(0, {0}), key(1, {1})}));
            data.tracks.push_back(propertyTrack("", "script:Probe", "offset", {key(0, {0, 0, 0}), key(1, {1, 2, 3})}));
            data.tracks.push_back(propertyTrack("", "script:Probe", "enabled", toggling()));
            data.tracks.push_back(propertyTrack("Door", "gameobject", "active", toggling()));
            data.tracks.push_back(propertyTrack("Lamp", "light", "intensity", {key(0, {1}), key(1, {3})}));
            return data;
        }

        void recordRows(){
            for (Entity character : characters){
                RecordedRow<ActiveRow>::install(instance(character), "gameobject");
                RecordedRow<PlainRow>::install(instance(character), "light");
            }
        }

        // frames until a worker wrote a plain row: proves the wave was parallel (a busy host may
        // leave the first frames' chunks to the main thread)
        void stepUntilAWorkerRan(){
            for (int frame = 0; frame < 6000 && threadLog.workers(PlainRow) == 0; frame++) step(kDt);
            ASSERT_GT(threadLog.workers(PlainRow), 0u) << "no plain row ran on a worker: this run says nothing about threads";
        }
    };

}

// AC3: the red run on main. Lua fields written from the wave race on the one Lua stack; active and
// enabled run user callbacks on a worker
TEST_F(AnimatorPropertyThread, LuaFieldsActiveAndEnabledAreWrittenOnTheMainThread){
    if (jobs::system().getWorkerCount() == 0) GTEST_SKIP() << "no workers on this machine: the wave is serial";
    buildCharacters(controllerFile("c", oneState(clip("rows", everyRowType()))));
    ScriptRuntime::awakeScene(&scene);
    step(0.0);
    recordRows();
    threadLog.reset();
    const size_t usesBefore = LuaBinding::getOffThreadUseCount();
    {
        LuaHeapRecorder recorder(L);
        step(kDt, 240);
        stepUntilAWorkerRan();
    }
    EXPECT_EQ(threadLog.workers(LuaHeap), 0u) << "a Lua field written from a worker";
    EXPECT_EQ(threadLog.workers(ScriptCallback), 0u) << "onEnable / onDisable ran on a worker";
    EXPECT_EQ(threadLog.workers(ActiveRow), 0u) << "GameObject active written from a worker";
    EXPECT_EQ(LuaBinding::getOffThreadUseCount(), usesBefore);
    // the frames really wrote them
    EXPECT_GE(threadLog.main(LuaHeap) + threadLog.workers(LuaHeap), (size_t)(240 * kCharacters));
    EXPECT_GE(threadLog.main(ScriptCallback) + threadLog.workers(ScriptCallback), (size_t)(2 * 12 * kCharacters));
    EXPECT_GE(threadLog.main(ActiveRow) + threadLog.workers(ActiveRow), (size_t)(12 * kCharacters));
}

// A field whose value does not change is only read: nothing allocates, the Lua stack is still used
TEST_F(AnimatorPropertyThread, ReadsOfAnUnchangedLuaFieldStayOnTheMainThread){
    if (jobs::system().getWorkerCount() == 0) GTEST_SKIP() << "no workers on this machine: the wave is serial";
    ClipData data = slide("Hips", 1.0f);
    data.tracks.push_back(propertyTrack("", "script:Probe", "glow", {key(0, {0.5f}), key(1, {0.5f})}));
    data.tracks.push_back(propertyTrack("Lamp", "light", "intensity", {key(0, {1}), key(1, {3})}));
    buildCharacters(controllerFile("c", oneState(clip("rows", data))));
    step(0.0);
    recordRows();
    threadLog.reset();
    const size_t usesBefore = LuaBinding::getOffThreadUseCount();
    step(kDt, 240);
    stepUntilAWorkerRan();
    EXPECT_EQ(LuaBinding::getOffThreadUseCount(), usesBefore) << "a Lua field read from a worker";
    EXPECT_FLOAT_EQ(luaField(refs[0], "glow"), 0.5f);
}

// R8: two Animators keying one entity's plain row: never both on the workers, the later one wins
TEST_F(AnimatorPropertyThread, TwoAnimatorsOnOnePlainRowDefer){
    if (jobs::system().getWorkerCount() == 0) GTEST_SKIP() << "no workers on this machine: the wave is serial";
    // the first and the last Animator: different chunks of the wave, so really concurrent
    const Entity outer = node(NULL_ENTITY, "Outer", Vector3(0, 0, 0));
    const Entity inner = node(outer, "Inner", Vector3(0, 0, 0));
    const Entity shared = lamp(inner, "Shared");
    node(inner, "Bone", Vector3(0, 0, 0));
    ClipData fromOuter = slide("Inner/Bone", 1.0f);
    fromOuter.tracks.push_back(propertyTrack("Inner/Shared", "light", "intensity", {key(0, {10}), key(1, {20})}));
    ClipData fromInner;
    fromInner.wrap = ClipWrapMode::Loop;
    fromInner.tracks.push_back(propertyTrack("Shared", "light", "intensity", {key(0, {100}), key(1, {200})}));
    animator(controllerFile("outer", oneState(clip("outer", fromOuter))), outer);
    ClipData filler = slide("Hips", 1.0f);
    filler.tracks.push_back(propertyTrack("Lamp", "light", "intensity", {key(0, {1}), key(1, {3})}));
    buildCharacters(controllerFile("filler", oneState(clip("filler", filler))));
    animator(controllerFile("inner", oneState(clip("inner", fromInner))), inner);

    step(0.0);
    recordRows();
    threadLog.reset();
    RecordedRow<WatchedRow>::install(instance(outer), "light");
    RecordedRow<WatchedRow>::install(instance(inner), "light");
    step(kDt, 240);
    stepUntilAWorkerRan();
    EXPECT_GT(threadLog.main(WatchedRow), 0u);
    EXPECT_EQ(threadLog.workers(WatchedRow), 0u) << "a row two Animators key was written from a worker";
    EXPECT_GE(scene.getComponent<LightComponent>(shared).intensity, 100.0f) << "the later Animator's value";
}

// R8: rows whose write reaches past the entity (a sprite's frame can load its texture) never run
// on a worker, even with one Animator on the entity
TEST_F(AnimatorPropertyThread, SpriteFramesAreWrittenOnTheMainThread){
    if (jobs::system().getWorkerCount() == 0) GTEST_SKIP() << "no workers on this machine: the wave is serial";
    ClipData data = slide("Hips", 1.0f);
    data.tracks.push_back(propertyTrack("Lamp", "light", "intensity", {key(0, {1}), key(1, {3})}));
    data.tracks.push_back(propertyTrack("Lamp", "sprite", "frame", {key(0, {0}), key(0.5f, {1}), key(1, {0})}));
    buildCharacters(controllerFile("c", oneState(clip("rows", data))));
    for (Entity character : characters){
        for (Entity child : getChildren(&scene, character)){
            if (scene.getEntityName(child) != "Lamp") continue;
            MeshComponent mesh;
            mesh.numSubmeshes = 1;
            mesh.submeshes[0].textureRect = Rect(0, 0, 0.5f, 1);
            scene.addComponent<MeshComponent>(child, mesh);
            SpriteComponent sprite;
            sprite.numFramesRect = 2;
            sprite.framesRect[0].rect = Rect(0, 0, 0.5f, 1);
            sprite.framesRect[1].rect = Rect(0.5f, 0, 0.5f, 1);
            scene.addComponent<SpriteComponent>(child, sprite);
        }
    }
    step(0.0);
    recordRows();
    for (Entity character : characters) RecordedRow<WatchedRow>::install(instance(character), "sprite");
    threadLog.reset();
    step(kDt, 240);
    stepUntilAWorkerRan();
    EXPECT_GT(threadLog.main(WatchedRow), 0u);
    EXPECT_EQ(threadLog.workers(WatchedRow), 0u) << "a sprite frame written from a worker";
}

// The values do not depend on the workers, the shared row included
TEST(AnimatorPropertyThreadDeterminism, ParallelEqualsSerial){
    std::vector<std::vector<float>> results(2);
    for (int r = 0; r < 2; r++){
        AnimatorFixture f;
        f.system().setParallel(r == 0);
        lua_State* L = LuaBinding::getLuaState();
        ClipData data = slide("Hips", 1.0f);
        data.tracks.push_back(propertyTrack("", "script:Probe", "glow", {key(0, {0}), key(1, {1})}));
        data.tracks.push_back(propertyTrack("Lamp", "light", "intensity", {key(0, {1}), key(1, {3})}));
        const std::string path = f.controllerFile("c", oneState(f.clip("rows", data)));
        std::vector<Entity> characters;
        std::vector<int> refs;
        for (int i = 0; i < kCharacters; i++){
            const Entity character = f.node(NULL_ENTITY, "Character", Vector3((float)i, 0, 0));
            f.node(character, "Hips", Vector3(0, 1, 0));
            const Entity lampEntity = f.node(character, "Lamp", Vector3(0, 0, 0));
            f.scene.addComponent<LightComponent>(lampEntity, LightComponent{});
            ASSERT_EQ(luaL_dostring(L, "threadProbe = {glow = 0}"), LUA_OK);
            lua_getglobal(L, "threadProbe");
            refs.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
            ScriptEntry entry;
            entry.type = ScriptType::LUA;
            entry.enabled = true;
            entry.className = "Probe";
            entry.instance = reinterpret_cast<void*>(static_cast<intptr_t>(refs.back()));
            ScriptProperty glow;
            glow.name = "glow";
            glow.type = ScriptPropertyType::Float;
            glow.value = 0.0f;
            entry.properties = {glow};
            ScriptComponent scripts;
            scripts.scripts.push_back(entry);
            f.scene.addComponent<ScriptComponent>(character, scripts);
            f.animator(path, character);
            characters.push_back(character);
        }
        // one lamp two Animators key: the later one's value, frame after frame
        const Entity outer = f.node(NULL_ENTITY, "Outer", Vector3(0, 0, 0));
        const Entity inner = f.node(outer, "Inner", Vector3(0, 0, 0));
        const Entity shared = f.node(inner, "Shared", Vector3(0, 0, 0));
        f.scene.addComponent<LightComponent>(shared, LightComponent{});
        ClipData fromOuter;
        fromOuter.wrap = ClipWrapMode::Loop;
        fromOuter.tracks.push_back(propertyTrack("Inner/Shared", "light", "intensity", {key(0, {10}), key(1, {20})}));
        ClipData fromInner;
        fromInner.wrap = ClipWrapMode::Loop;
        fromInner.tracks.push_back(propertyTrack("Shared", "light", "intensity", {key(0, {100}), key(1, {200})}));
        f.animator(f.controllerFile("outer", oneState(f.clip("outer", fromOuter))), outer);
        f.animator(f.controllerFile("inner", oneState(f.clip("inner", fromInner))), inner);
        f.step(0.0);
        for (int frame = 0; frame < 240; frame++){
            f.step(kDt);
            results[(size_t)r].push_back(f.scene.getComponent<LightComponent>(shared).intensity);
            for (size_t i = 0; i < characters.size(); i++){
                lua_rawgeti(L, LUA_REGISTRYINDEX, refs[i]);
                lua_pushstring(L, "glow");
                lua_rawget(L, -2);
                results[(size_t)r].push_back((float)lua_tonumber(L, -1));
                lua_pop(L, 2);
            }
        }
        for (Entity character : characters) f.scene.getComponent<ScriptComponent>(character).scripts.clear();
        for (int ref : refs) luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
    ASSERT_EQ(results[0].size(), results[1].size());
    EXPECT_EQ(std::memcmp(results[0].data(), results[1].data(), results[0].size() * sizeof(float)), 0) << "bit-identical";
}

// S77 R8 and docs/animator.md "Order of a frame": every Animator's values, then the callback rows,
// then root motion and events Animator by Animator; each once
TEST_F(AnimatorPropertyThread, EveryValueIsWrittenBeforeAnyCallback){
    ClipData data = slide("Hips", 1.0f);
    data.events = {event(0.5f, "mark")};
    data.tracks.push_back(propertyTrack("", "script:Probe", "glow", {key(0, {0}), key(1, {1})}));
    data.tracks.push_back(propertyTrack("", "script:Probe", "enabled", {key(0, {1}), key(0.5f, {0}), key(1, {1})}));
    const std::string path = controllerFile("c", oneState(clip("rows", data)));
    ASSERT_EQ(luaL_dostring(L, "orderLog = {}"), LUA_OK);
    std::vector<std::string> log;
    int glows[2];
    const char* names[2] = {"A", "B"};
    for (int i = 0; i < 2; i++){
        const Entity entity = node(NULL_ENTITY, names[i], Vector3(0, 0, 0));
        node(entity, "Hips", Vector3(0, 1, 0));
        glows[i] = script(entity, "Probe");
        AnimatorComponent& component = animator(path, entity);
        component.applyRootMotion = true;
        characters.push_back(entity);
    }
    for (int i = 0; i < 2; i++){
        const std::string name = names[i];
        lua_rawgeti(L, LUA_REGISTRYINDEX, glows[i]);
        lua_setglobal(L, ("probe" + name).c_str());
        const std::string code = "probe" + name + ".onDisable = function(self) orderLog[#orderLog + 1] = string.format('" + name +
                                 " disable %.2f %.2f', probeA.glow, probeB.glow) end; probe" + name + ".onEnable = nil";
        ASSERT_EQ(luaL_dostring(L, code.c_str()), LUA_OK) << lua_tostring(L, -1);
    }
    auto drain = [&](){
        ASSERT_EQ(luaL_dostring(L, "orderResult = table.concat(orderLog, ','); orderLog = {}"), LUA_OK);
        lua_getglobal(L, "orderResult");
        const std::string text = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (!text.empty()) log.push_back(text);
    };
    for (int i = 0; i < 2; i++){
        const std::string name = names[i];
        AnimatorComponent& component = scene.getComponent<AnimatorComponent>(characters[(size_t)i]);
        component.onAnimatorMove.add("test", [&, name](Vector3, Quaternion){
            drain();
            log.push_back(name + " move");
        });
        component.onEvent.add("test", [&, name](const AnimationEvent&){
            char text[64];
            std::snprintf(text, sizeof(text), "%s event %.2f %.2f", name.c_str(), luaField(glows[0], "glow"), luaField(glows[1], "glow"));
            log.push_back(text);
        });
    }
    ScriptRuntime::awakeScene(&scene);
    step(0.0);
    log.clear();
    drain();
    log.clear();
    step(0.6);
    const std::vector<std::string> expected = {"A disable 0.60 0.60,B disable 0.60 0.60", "A move", "A event 0.60 0.60", "B move", "B event 0.60 0.60"};
    EXPECT_EQ(log, expected);
}

// A callback that removes an Animator: nothing it held is used, and the one that moved into its
// slot of the component array still plays its events
TEST_F(AnimatorPropertyThread, ACallbackMayRemoveAnAnimator){
    ClipData data = slide("Hips", 1.0f);
    data.events = {event(0.5f, "mark")};
    data.tracks.push_back(propertyTrack("", "script:Probe", "enabled", {key(0, {1}), key(0.5f, {0}), key(1, {1})}));
    data.tracks.push_back(propertyTrack("", "script:Probe", "glow", {key(0, {0}), key(1, {1})}));
    const std::string path = controllerFile("c", oneState(clip("rows", data)));
    std::vector<int> fired(3, 0);
    std::vector<int> glows;
    for (int i = 0; i < 3; i++){
        const Entity entity = node(NULL_ENTITY, "Character", Vector3(0, 0, 0));
        node(entity, "Hips", Vector3(0, 1, 0));
        glows.push_back(script(entity, "Probe"));
        animator(path, entity).onEvent.add("test", [&fired, i](const AnimationEvent&){ fired[(size_t)i]++; });
        characters.push_back(entity);
    }
    removeHook = [this](){
        if (!scene.findComponent<AnimatorComponent>(characters[1])) return;
        scene.removeComponent<AnimatorComponent>(characters[1]);
        // what any index read in a script's call would do: the hole closes, the third one moves
        scene.getComponentArray<AnimatorComponent>()->compact();
    };
    lua_register(L, "removeTheSecondAnimator", &callRemoveHook);
    lua_rawgeti(L, LUA_REGISTRYINDEX, glows[0]);
    lua_setglobal(L, "firstProbe");
    ASSERT_EQ(luaL_dostring(L, "firstProbe.onDisable = function(self) removeTheSecondAnimator() end"), LUA_OK);
    ScriptRuntime::awakeScene(&scene);
    step(0.0);
    step(0.6);
    removeHook = nullptr;
    EXPECT_EQ(scene.findComponent<AnimatorComponent>(characters[1]), nullptr);
    EXPECT_EQ(fired[0], 1);
    EXPECT_EQ(fired[1], 0) << "removed before its events";
    EXPECT_EQ(fired[2], 1) << "moved in the array, not gone";
    EXPECT_NEAR(luaField(glows[1], "glow"), 0.6f, 1e-5f) << "the values came before any callback";
    EXPECT_NEAR(luaField(glows[2], "glow"), 0.6f, 1e-5f);
}

// A row on an entity that only one Animator writes stays in the wave; a second writer sends it to
// the main thread, and it comes back when that one goes
TEST_F(AnimatorPropertyThread, PlainRowsLeaveTheWaveWhileTheEntityIsShared){
    const Entity outer = node(NULL_ENTITY, "Outer", Vector3(0, 0, 0));
    const Entity inner = node(outer, "Inner", Vector3(0, 0, 0));
    const Entity shared = lamp(inner, "Shared");
    const Entity other = lamp(inner, "Other");
    ClipData fromOuter;
    fromOuter.wrap = ClipWrapMode::Loop;
    fromOuter.tracks.push_back(propertyTrack("Inner/Shared", "light", "intensity", {key(0, {10}), key(1, {20})}));
    fromOuter.tracks.push_back(propertyTrack("Inner/Other", "light", "intensity", {key(0, {10}), key(1, {20})}));
    ClipData fromInner;
    fromInner.wrap = ClipWrapMode::Loop;
    fromInner.tracks.push_back(propertyTrack("Shared", "light", "intensity", {key(0, {100}), key(1, {200})}));
    animator(controllerFile("outer", oneState(clip("outer", fromOuter))), outer);
    step(0.0);
    auto inWave = [&](Entity animatorEntity, Entity target){
        for (const RigProperty& property : instance(animatorEntity).rig.properties){
            if (property.entity == target) return property.inWave;
        }
        ADD_FAILURE() << "no row on the entity";
        return false;
    };
    EXPECT_TRUE(inWave(outer, shared)) << "one writer, a row that touches only its entity";
    EXPECT_TRUE(inWave(outer, other));

    animator(controllerFile("inner", oneState(clip("inner", fromInner))), inner);
    step(kDt);
    EXPECT_FALSE(inWave(outer, shared)) << "the first Animator's row follows the second's bind";
    EXPECT_FALSE(inWave(inner, shared));
    EXPECT_TRUE(inWave(outer, other)) << "another entity is not concerned";
    EXPECT_GE(scene.getComponent<LightComponent>(shared).intensity, 100.0f) << "the later Animator's value";

    scene.removeComponent<AnimatorComponent>(inner);
    step(kDt);
    EXPECT_TRUE(inWave(outer, shared)) << "one writer again";
    EXPECT_LE(scene.getComponent<LightComponent>(shared).intensity, 20.0f);

    // a row on another Animator's bone
    ClipData boneOfInner = slide("Shared", 1.0f);
    animator(controllerFile("bones", oneState(clip("bones", boneOfInner))), inner);
    step(kDt);
    EXPECT_FALSE(inWave(outer, shared)) << "the other Animator writes that entity's Transform";
}

// The registry's side of R8: only rows that touch nothing but their entity may run on a worker
TEST(AnimatorPropertyThreadRows, RowsThatReachSharedStateAreNotWorkerSafe){
    auto workerSafe = [](const char* component, const char* property){
        const AnimatableProperty* row = findAnimatableProperty(component, property);
        EXPECT_NE(row, nullptr) << component << "." << property;
        return row && row->workerSafe;
    };
    EXPECT_FALSE(workerSafe("gameobject", "active")) << "user callbacks";
    EXPECT_FALSE(workerSafe("sprite", "frame")) << "Texture::load";
    EXPECT_FALSE(workerSafe("rigidbody2d", "enabled")) << "the Box2D world";
    EXPECT_FALSE(workerSafe("mesh", "submeshes[0].material.shaderParams.tint")) << "the manifest cache, its lock";
    EXPECT_FALSE(workerSafe("ui", "shaderUniforms.tint"));
    EXPECT_TRUE(workerSafe("light", "intensity"));
    EXPECT_TRUE(workerSafe("mesh", "submeshes[0].material.baseColorFactor"));
    EXPECT_TRUE(workerSafe("canvasgroup", "alpha"));
    EXPECT_TRUE(workerSafe("rigidbody", "enabled")) << "the flag only: physics applies it on its step";
    PropertyRef ref;
    ASSERT_TRUE(parsePropertyRef("script:Torch", "glow", ref));
    EXPECT_FALSE(ref.deferred) << "a field runs no callback, and still has no row to run on a worker";
    PropertyBinding binding;
    EXPECT_FALSE(propertyWorkerSafe(binding));
}

// The edit-mode preview has no finalize: its rows are written before it returns
TEST_F(AnimatorPropertyThread, PreviewWritesEveryRowAtOnce){
    ClipData data = slide("Hips", 1.0f);
    data.tracks.push_back(propertyTrack("", "script:Probe", "glow", {key(0, {0}), key(1, {1})}));
    data.tracks.push_back(propertyTrack("Hips/Spine/Head", "gameobject", "active", {key(0, {1}), key(0.5f, {0}), key(1, {1})}));
    const int ref = script(root, "Probe");
    animator(controllerFile("c", oneState(clip("rows", data))));
    std::string error;
    ASSERT_TRUE(system().preview(root, 0.6f, &error)) << error;
    EXPECT_NEAR(luaField(ref, "glow"), 0.6f, 1e-5f);
    EXPECT_FALSE(scene.isEntityActive(head));
    system().restorePreview(root);
    EXPECT_FLOAT_EQ(luaField(ref, "glow"), 0.0f);
    EXPECT_TRUE(scene.isEntityActive(head));
}

// R9: rows that wait for the main thread allocate nothing once playing
TEST_F(AnimatorPropertyThread, WaitingRowsAllocateNothing){
    if (!testsupport::countsAllocations()) GTEST_SKIP() << "this build cannot count allocations";
    ClipData data = slide("Hips", 1.0f);
    data.tracks.push_back(propertyTrack("", "script:Probe", "glow", {key(0, {0}), key(1, {1})}));
    data.tracks.push_back(propertyTrack("Door", "gameobject", "active", {key(0, {1}), key(1, {1})}));
    data.tracks.push_back(propertyTrack("Lamp", "light", "intensity", {key(0, {1}), key(1, {3})}));
    buildCharacters(controllerFile("c", oneState(clip("rows", data))));
    system().setParallel(false);
    step(0.0);
    step(kDt, 120);
    ASSERT_TRUE(system().getStats().sampled > 0) << "the characters play";
    // Serially: the rows wait for the main thread on either path (writePose fills the waiting
    // buffers whether or not the wave is parallel), and the parallel wave's own blocks come from the
    // job system's slab pool, whose worker caches fill over a varying number of frames (traced:
    // jobs::detail::acquireSlab in runParallelFor, 1 or 20 allocations in 6 of 100 runs). R9's parallel
    // zero is Crowd200's (je-bench-anim --count-allocs)
    testsupport::AllocationCountScope scope;
    step(kDt, 600);
    EXPECT_EQ(scope.count(), 0u);
}
