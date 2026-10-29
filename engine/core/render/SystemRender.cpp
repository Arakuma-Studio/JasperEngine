// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SystemRender.h"

#include "sokol/SokolPassPreview.h"
#include "sokol/SokolSystem.h"
#include "Engine.h"
#include "Log.h"


#include <mutex>
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

namespace {
    std::mutex& textureSupportMutex(){
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }
    bool textureSupportOverridden = false;
    TextureFormatSupport textureSupportOverride;
}

bool SystemRender::isTextureFormatSupported(GpuTextureFormat format){
    return getTextureFormatSupport()(format);
}

TextureFormatSupport SystemRender::getTextureFormatSupport(){
    {
        std::lock_guard<std::mutex> lock(textureSupportMutex());
        if (textureSupportOverridden)
            return textureSupportOverride;
    }
    return SokolSystem::getFeatures().textureFormats;
}

void SystemRender::setTextureFormatSupportOverride(const TextureFormatSupport* support){
    std::lock_guard<std::mutex> lock(textureSupportMutex());
    textureSupportOverridden = support != nullptr;
    if (support)
        textureSupportOverride = *support;
}

bool SystemRender::hasTextureFormatSupportOverride(){
    std::lock_guard<std::mutex> lock(textureSupportMutex());
    return textureSupportOverridden;
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
void SystemRender::setViewTag(uint32_t tag){
    SokolSystem::setViewTag(tag);
}

uint32_t SystemRender::getViewTag(){
    return SokolSystem::getViewTag();
}

RenderStats SystemRender::getRenderStats(uint32_t viewTag){
    RenderStats stats = SokolSystem::getRenderStats(viewTag);
    if (viewTag == FRAME_STATS_FRAME){
        const Engine::FrameStats& frame = Engine::getFrameStats();
        stats.drawCalls = frame.drawCalls;
        stats.instances = frame.instances;
        stats.triangles = frame.triangles;
    }else{
        const PassList list = SokolSystem::getPassList(viewTag, false);
        for (const PassRecord& pass : list.passes){
            stats.drawCalls += pass.draws;
            stats.instances += pass.instances;
            stats.triangles += pass.triangles;
        }
    }
    return stats;
}

PassList SystemRender::getPassList(uint32_t viewTag, bool resolvedOnly){
    return SokolSystem::getPassList(viewTag, resolvedOnly);
}

std::vector<uint32_t> SystemRender::getRecordedViewTags(){
    return SokolSystem::getRecordedViewTags();
}

void SystemRender::setDrawListEnabled(bool enabled){
    SokolSystem::setDrawListEnabled(enabled);
}

void SystemRender::acquireGpuTiming(){
    SokolSystem::acquireGpuTiming();
}

void SystemRender::releaseGpuTiming(){
    SokolSystem::releaseGpuTiming();
}

void SystemRender::leaseGpuTiming(double seconds){
    SokolSystem::leaseGpuTiming(seconds);
}

GpuTimingState SystemRender::getGpuTimingState(){
    return SokolSystem::getGpuTimingState();
}

BackendFrameCounters SystemRender::getBackendFrameCounters(){
    return SokolSystem::getBackendFrameCounters();
}

std::string SystemRender::getApiVersion(){
    return SokolSystem::getApiVersion();
}

const char* SystemRender::getBackendName(){
    return SokolSystem::getBackendName();
}

std::string SystemRender::getDeviceName(){
    return SokolSystem::getDeviceName();
}

void SystemRender::requestPassPreview(const PassPreviewRequest& request){
    SokolPassPreview::request(request);
}

void SystemRender::cancelPassPreview(){
    SokolPassPreview::cancel();
}

PassPreviewResult SystemRender::getPassPreview(){
    return SokolPassPreview::result();
}

TextureRender* SystemRender::getPassPreviewTexture(){
    return SokolPassPreview::texture();
}

void SystemRender::setVulkanHostQueryReset(bool enabled){
    SokolSystem::setVulkanHostQueryReset(enabled);
}

FramebufferRender* SystemRender::getPassPreviewTarget(){
    return SokolPassPreview::target();
}
