// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "DebugStats.h"

#include "Engine.h"
#include "profiler/FrameTimes.h"
#include "Scene.h"
#include "System.h"
#include "script/LuaBinding.h"
#include "script/ScriptScheduler.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/UIDocumentSystem.h"

#include "lua.hpp"

#include <algorithm>
#include <functional>

using namespace jasperengine;

namespace {

    // a stall (a breakpoint, the app in the background) is one long frame, not the scale of the graph
    constexpr float maxShownMs = 1000.0f;
    // the window of the frame-time figures
    constexpr double windowSeconds = 2.0;

    uint32_t toCount(size_t value){
        return value > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(value);
    }
}

std::vector<float> DebugStats::frameHistory(){
    const std::vector<FrameTimeSample> samples = FrameTimes::snapshot(historySize);
    std::vector<float> out;
    out.reserve(samples.size());
    for (const FrameTimeSample& sample : samples)
        out.push_back(std::clamp(sample.frameMs, 0.0f, maxShownMs));
    return out;
}

size_t DebugStats::recordedFrames(){
    return static_cast<size_t>(FrameTimes::currentFrame());
}

DebugStatsSnapshot DebugStats::sample(){
    DebugStatsSnapshot s;

    // the frames of the last 2 s, newest first
    const std::vector<float> frames = frameHistory();
    std::vector<float> window;
    double windowMs = 0.0;
    for (auto it = frames.rbegin(); it != frames.rend() && windowMs < windowSeconds * 1000.0; ++it){
        window.push_back(*it);
        windowMs += *it;
    }
    if (!window.empty()){
        s.frameMs = window.front();
        s.frameMsMin = *std::min_element(window.begin(), window.end());
        s.frameMsMax = *std::max_element(window.begin(), window.end());
        s.frameMsAvg = static_cast<float>(windowMs / window.size());
        s.fps = windowMs > 0.0 ? static_cast<float>(window.size() * 1000.0 / windowMs) : 0.0f;
        // 1% low: the slowest 1% of the frames (at least one), as a frame time
        std::vector<float> sorted = window;
        std::sort(sorted.begin(), sorted.end(), std::greater<float>());
        const size_t slowest = std::max<size_t>(1, sorted.size() / 100);
        double sum = 0.0;
        for (size_t i = 0; i < slowest; i++)
            sum += sorted[i];
        s.frameMs1PercentLow = static_cast<float>(sum / slowest);
    }

    const Engine::FrameStats& frame = Engine::getFrameStats();
    s.drawCalls = frame.drawCalls;
    s.setPassCalls = frame.setPassCalls;
    s.instances = frame.instances;
    s.triangles = frame.triangles;

    if (System::hasInstance())
        s.processMemoryBytes = System::instance().getProcessMemoryBytes();

    if (lua_State* L = LuaBinding::getLuaState()){
        if (LuaBinding::isLuaThread())
            s.luaMemoryBytes = static_cast<uint64_t>(lua_gc(L, LUA_GCCOUNT, 0)) * 1024u + static_cast<uint64_t>(lua_gc(L, LUA_GCCOUNTB, 0));
    }

    AudioSystem::getVoiceCounts(s.audioVoices, s.audioVoicesMax);

    const std::vector<Scene*> scenes = Engine::getSceneLayers();
    s.scenes = toCount(scenes.size());
    size_t entities = 0;
    uint32_t htmlUiDrawCalls = 0;
    for (Scene* scene : scenes){
        entities += scene->getEntityCount();
        if (auto documents = scene->findSystem<UIDocumentSystem>())
            htmlUiDrawCalls += documents->getRenderStats().drawCalls;
        if (auto physics = scene->findSystem<PhysicsSystem>()){
#ifdef JASPERENGINE_PHYSICS_2D
            const b2WorldId world = physics->getWorld2D();
            if (b2World_IsValid(world))
                s.bodies2D += toCount(static_cast<size_t>(std::max(0, b2World_GetCounters(world).bodyCount)));
#endif
#ifdef JASPERENGINE_PHYSICS_3D
            s.bodies3D += toCount(physics->getWorld3D()->GetNumBodies());
#endif
        }
    }
    s.entities = toCount(entities);
    s.htmlUiDrawCalls = htmlUiDrawCalls;

    s.coroutines = toCount(ScriptScheduler::coroutineCount());
    s.timers = toCount(ScriptScheduler::timerCount());

    return s;
}
