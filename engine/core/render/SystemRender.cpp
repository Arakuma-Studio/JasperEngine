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

namespace {
    uint64_t committedFrames = 0;
}

void SystemRender::commit(){
    SokolSystem::commit();
    committedFrames++;
}

uint64_t SystemRender::getFrameCount(){
    return committedFrames;
}

const RenderFeatures& SystemRender::getFeatures(){
    return SokolSystem::getFeatures();
}

uint64_t SystemRender::getPipelineCacheMisses(){
    return SokolSystem::getPipelineCacheMisses();
}

uint64_t SystemRender::getPipelineApplies(){
    return SokolSystem::getPipelineApplies();
}

size_t SystemRender::getPipelineCacheSize(){
    return SokolSystem::getPipelineCacheSize();
}

uint64_t SystemRender::getNestedPassCount(){
    return SokolSystem::getNestedPassCount();
}

void SystemRender::shutdown(){
    SokolSystem::shutdown();
}

namespace{
    struct FrameStatsRequest{
        uint64_t frame;
        uint32_t view;
    };

    void printFrameStats(void* data){
        FrameStatsRequest* request = static_cast<FrameStatsRequest*>(data);
        const std::string line = SokolSystem::getFrameStatsLine();
        if (!line.empty()){
            if (request->view == SystemRender::FRAME_STATS_FRAME)
                Log::print("Frame stats (frame %llu): %s", (unsigned long long)request->frame, line.c_str());
            else
                Log::print("Frame stats (frame %llu, view %u): %s", (unsigned long long)request->frame, request->view, line.c_str());
        }
        delete request;
    }
}

void SystemRender::logFrameStats(uint64_t frame, uint32_t view){
    addQueueCommand(&printFrameStats, new FrameStatsRequest{frame, view});
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