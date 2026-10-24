// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Log.h"
#include "jobs/JobSystem.h"
#include "thread/ResourceProgress.h"
#include "thread/ThreadPoolManagerLua.h"

#include <algorithm>
#include <atomic>

using namespace jasperengine;
// unqualified in the JobLane values: the Lua stub generator resolves `Lane::High`
using jobs::Lane;

namespace{
    void warnDeprecatedPool(){
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            Log::warn("ThreadPoolManager is deprecated: the engine's job system replaces it (Jobs.getWorkerCount, Engine.setMaxResourceLoadingThreads)");
    }

    // a number from Lua outside JobLane reads as Background
    size_t laneIndex(jobs::Lane lane){
        return std::min((size_t)lane, jobs::kLaneCount - 1);
    }
}

ThreadPoolManagerLua* ThreadPoolManagerLua::getInstance(){
    warnDeprecatedPool();
    static ThreadPoolManagerLua instance;
    return &instance;
}

void ThreadPoolManagerLua::initialize(size_t){
    warnDeprecatedPool();
}

void ThreadPoolManagerLua::shutdown(){
    warnDeprecatedPool();
}

size_t ThreadPoolManagerLua::getQueueSize() const{
    return jobs::system().stats(1).lanes[(size_t)jobs::Lane::Background].queued;
}

void LuaBinding::registerThreadClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ResourceType")
        .addVariable("Shader", ResourceType::Shader)
        .addVariable("Texture", ResourceType::Texture)
        .addVariable("Model", ResourceType::Model)
        .addVariable("Sound", ResourceType::Sound)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        // Progress reporting, read-only on purpose
        .beginClass<ResourceBuildInfo>("ResourceBuildInfo")
        .addProperty("type", &ResourceBuildInfo::type)
        .addProperty("name", &ResourceBuildInfo::name)
        .addProperty("progress", &ResourceBuildInfo::progress)
        .addProperty("isActive", &ResourceBuildInfo::isActive)
        .addProperty("startTime", &ResourceBuildInfo::startTime)
        .endClass();

    luabridge::getGlobalNamespace(L)
        // Progress snapshot, read-only on purpose
        .beginClass<OverallBuildProgress>("OverallBuildProgress")
        .addProperty("totalProgress", &OverallBuildProgress::totalProgress)
        .addProperty("totalBuilds", &OverallBuildProgress::totalBuilds)
        .addProperty("completedBuilds", &OverallBuildProgress::completedBuilds)
        .addProperty("currentBuildName", &OverallBuildProgress::currentBuildName)
        .addProperty("currentBuildType", &OverallBuildProgress::currentBuildType)
        .addProperty("hasActiveBuilds", &OverallBuildProgress::hasActiveBuilds)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<ResourceProgress>("ResourceProgress")
        .addStaticFunction("startBuild", &ResourceProgress::startBuild)
        .addStaticFunction("updateProgress", &ResourceProgress::updateProgress)
        .addStaticFunction("completeBuild", &ResourceProgress::completeBuild)
        .addStaticFunction("failBuild", &ResourceProgress::failBuild)
        .addStaticFunction("hasActiveBuilds", &ResourceProgress::hasActiveBuilds)
        .addStaticFunction("getOverallProgress", &ResourceProgress::getOverallProgress)
        .addStaticFunction("getActiveBuildCount", &ResourceProgress::getActiveBuildCount)
        .addStaticFunction("getAllActiveBuilds", &ResourceProgress::getAllActiveBuilds)
        .addStaticFunction("getCurrentBuild", &ResourceProgress::getCurrentBuild)
        .addStaticFunction("getResourceTypeName", &ResourceProgress::getResourceTypeName)
        .endClass();

    // read-only: Lua states are single-threaded, so scripts never submit jobs
    luabridge::getGlobalNamespace(L)
        .beginNamespace("JobLane")
        .addVariable("High", Lane::High)
        .addVariable("Normal", Lane::Normal)
        .addVariable("Background", Lane::Background)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("Jobs")
        .addFunction("getWorkerCount", []() -> int { return jobs::system().getWorkerCount(); })
        .addFunction("isSingleThreaded", []() -> bool { return jobs::system().isSingleThreaded(); })
        .addFunction("getQueuedCount", [](Lane lane) -> int { return (int)jobs::system().stats(1).lanes[laneIndex(lane)].queued; })
        .addFunction("getRunningCount", [](Lane lane) -> int { return (int)jobs::system().stats(1).lanes[laneIndex(lane)].running; })
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<ThreadPoolManagerLua>("ThreadPoolManager")
        .addStaticFunction("getInstance", &ThreadPoolManagerLua::getInstance)
        .addStaticFunction("initialize", &ThreadPoolManagerLua::initialize)
        .addStaticFunction("shutdown", &ThreadPoolManagerLua::shutdown)
        .addFunction("getQueueSize", &ThreadPoolManagerLua::getQueueSize)
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}