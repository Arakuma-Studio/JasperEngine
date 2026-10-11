// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SystemRender.h"

#include "sokol/SokolSystem.h"
#include "Engine.h"
#include "Log.h"

#include <string>

using namespace jasperengine;

void SystemRender::setup(){
    SokolSystem::setup();
}

void SystemRender::commitQueue(){
    SokolSystem::commitQueue();
}

void SystemRender::executeQueue(){
    SokolSystem::executeQueue();
}

void SystemRender::commit(){
    SokolSystem::commit();
}

void SystemRender::shutdown(){
    SokolSystem::shutdown();
}

namespace{
    void printFrameStats(void* data){
        uint64_t* frame = static_cast<uint64_t*>(data);
        const std::string line = SokolSystem::getFrameStatsLine();
        if (!line.empty())
            Log::print("Frame stats (frame %llu): %s", (unsigned long long)*frame, line.c_str());
        delete frame;
    }
}

void SystemRender::logFrameStats(uint64_t frame){
    addQueueCommand(&printFrameStats, new uint64_t(frame));
}

void SystemRender::scheduleCleanup(void (*cleanupFunc)(void* cleanupData), void* cleanupData, int32_t numFramesToDefer){
    if (Engine::isAsyncThread()){
        SokolSystem::scheduleCleanup(cleanupFunc, cleanupData, numFramesToDefer);
    }else{
        cleanupFunc(cleanupData);
    }
}

void SystemRender::addQueueCommand(void (*custom_cb)(void* custom_data), void* custom_data){
    if (Engine::isAsyncThread()){
        SokolSystem::addQueueCommand(custom_cb, custom_data);
    }else{
        custom_cb(custom_data);
    }
}