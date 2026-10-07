// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SystemRender.h"

#include "sokol/SokolSystem.h"
#include "Engine.h"

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

void SystemRender::shutdown(){
    SokolSystem::shutdown();
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