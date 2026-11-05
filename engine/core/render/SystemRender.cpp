// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SystemRender.h"

#include "GpuWorkerContext.h"
#include "sokol/SokolAsyncCreate.h"
#include "sokol/SokolPassPreview.h"
#include "sokol/SokolPipelineCache.h"
#include "sokol/SokolPipelineRecord.h"
#include "ShaderRender.h"
#include "sokol/SokolSystem.h"
#include "sokol/SokolObject.h"
#include "sokol/SokolCmdQueue.h"
#include "Engine.h"
#include "Log.h"


#include <mutex>
#include <string>

using namespace jasperengine;

namespace {
    uint64_t contextId = 0;
    GpuWorkerContextFactory& workerContextFactory(){
        static GpuWorkerContextFactory factory;
        return factory;
    }
}

void jasperengine::setGpuWorkerContextFactory(GpuWorkerContextFactory factory){
    workerContextFactory() = std::move(factory);
}

const GpuWorkerContextFactory& jasperengine::getGpuWorkerContextFactory(){
    return workerContextFactory();
}

void SystemRender::setup(){
    contextId++;
    SokolSystem::setup();
}

uint64_t SystemRender::getContextId(){
    return contextId;
}

int SystemRender::getResourcesInUse(GpuResourceKind kind){
    return SokolSystem::getResourcesInUse((int)kind);
}

int SystemRender::getResourceLimit(GpuResourceKind kind){
    return SokolSystem::getResourceLimit((int)kind);
}

const char* SystemRender::getResourceKindName(GpuResourceKind kind){
    return SokolSystem::getResourceKindName((int)kind);
}

uint64_t SystemRender::getResourcePoolExhaustions(){
    uint64_t total = 0;
    for (int kind = 0; kind < GPU_RESOURCE_KINDS; kind++)
        total += SokolSystem::getResourcePoolExhaustions(kind);
    return total;
}

uint64_t SystemRender::getResourcePoolExhaustions(GpuResourceKind kind){
    return SokolSystem::getResourcePoolExhaustions((int)kind);
}

bool SystemRender::hasResourcePoolRoom(){
    return SokolSystem::hasResourcePoolRoom();
}

uint64_t SystemRender::getUniformBytesLastFrame(){
    return SokolSystem::getBudgetLastFrame(SokolSystem::UNIFORM_BUDGET);
}

uint64_t SystemRender::getUniformBytesPeak(){
    return SokolSystem::getBudgetPeak(SokolSystem::UNIFORM_BUDGET);
}

uint64_t SystemRender::getUniformBytesLimit(){
    return SokolSystem::getBudgetLimit(SokolSystem::UNIFORM_BUDGET);
}

bool SystemRender::hasUniformBufferLimit(){
    return SokolSystem::isBudgetEnforced(SokolSystem::UNIFORM_BUDGET);
}

uint64_t SystemRender::getUniformSkippedDraws(){
    return SokolSystem::getBudgetSkipped(SokolSystem::UNIFORM_BUDGET);
}

uint64_t SystemRender::getStreamBytesLastFrame(){
    return SokolSystem::getBudgetLastFrame(SokolSystem::STREAM_BUDGET);
}

uint64_t SystemRender::getStreamBytesPeak(){
    return SokolSystem::getBudgetPeak(SokolSystem::STREAM_BUDGET);
}

uint64_t SystemRender::getStreamBytesLimit(){
    return SokolSystem::getBudgetLimit(SokolSystem::STREAM_BUDGET);
}

bool SystemRender::hasStreamStagingLimit(){
    return SokolSystem::isBudgetEnforced(SokolSystem::STREAM_BUDGET);
}

uint64_t SystemRender::getStreamSkippedUploads(){
    return SokolSystem::getBudgetSkipped(SokolSystem::STREAM_BUDGET);
}

uint64_t SystemRender::getDescriptorBytesLastFrame(){
    return SokolSystem::getBudgetLastFrame(SokolSystem::DESCRIPTOR_BUDGET);
}

uint64_t SystemRender::getDescriptorBytesPeak(){
    return SokolSystem::getBudgetPeak(SokolSystem::DESCRIPTOR_BUDGET);
}

uint64_t SystemRender::getDescriptorBytesLimit(){
    return SokolSystem::getBudgetLimit(SokolSystem::DESCRIPTOR_BUDGET);
}

bool SystemRender::hasDescriptorBufferLimit(){
    return SokolSystem::isBudgetEnforced(SokolSystem::DESCRIPTOR_BUDGET);
}

uint64_t SystemRender::getDescriptorSkippedDraws(){
    return SokolSystem::getBudgetSkipped(SokolSystem::DESCRIPTOR_BUDGET);
}

void SystemRender::updateGpuCreation(){
    SokolAsyncCreate::update();
}

GpuCreationStats SystemRender::getGpuCreationStats(){
    return SokolAsyncCreate::getStats();
}

bool SystemRender::isGpuCreationIdle(){
    return SokolAsyncCreate::isIdle();
}

bool SystemRender::waitGpuCreationIdle(double timeoutMs){
    return SokolAsyncCreate::waitIdle(timeoutMs);
}

void SystemRender::resetGpuCreationMax(){
    SokolAsyncCreate::resetMaxMainMs();
}

void SystemRender::setShaderCreationSettings(const ShaderCreationSettings& settings){
    SokolAsyncCreate::setSettings(settings);
}

const ShaderCreationSettings& SystemRender::getShaderCreationSettings(){
    return SokolAsyncCreate::getSettings();
}

void SystemRender::setPipelineRecorder(std::function<void(uint32_t shaderId, const std::string& record)> recorder){
    if (!recorder){
        SokolPipelineCache::setRecorder(nullptr);
        return;
    }
    SokolPipelineCache::setRecorder([recorder](const sg_pipeline_desc& desc){
        recorder(desc.shader.id, SokolPipelineRecord::toJson(desc));
    });
}

bool SystemRender::warmPipeline(ShaderRender* shader, const std::string& record, bool otherFamily, std::vector<uint32_t>& held,
                                std::string* error){
    if (!shader || !shader->isCreated()){
        if (error) *error = "the shader is not ready";
        return false;
    }
    sg_pipeline_desc desc = {};
    if (!SokolPipelineRecord::fromJson(record, desc, error))
        return false;
    desc.shader = shader->backend.get();
    const sg_pipeline pipeline = SokolPipelineCache::acquire(desc);
    if (pipeline.id != SG_INVALID_ID)
        held.push_back(pipeline.id);
    // an offscreen target's pipeline is flipped on GL: the other family's record warms both
    if (otherFamily && desc.colors[0].pixel_format != _SG_PIXELFORMAT_DEFAULT){
        desc.face_winding = desc.face_winding == SG_FACEWINDING_CW ? SG_FACEWINDING_CCW : SG_FACEWINDING_CW;
        const sg_pipeline flipped = SokolPipelineCache::acquire(desc);
        if (flipped.id != SG_INVALID_ID)
            held.push_back(flipped.id);
    }
    return true;
}

bool SystemRender::isPipelineSettled(uint32_t pipeline){
    const sg_resource_state state = sg_query_pipeline_state({pipeline});
    return state == SG_RESOURCESTATE_VALID || state == SG_RESOURCESTATE_FAILED || state == SG_RESOURCESTATE_INVALID;
}

void SystemRender::releaseWarmPipeline(uint32_t pipeline){
    SokolPipelineCache::release({pipeline});
}

uint32_t SystemRender::getShaderHandleId(ShaderRender* shader){
    return shader ? shader->backend.get().id : 0;
}

void SystemRender::commitQueue(){
    SokolSystem::commitQueue();
}

void SystemRender::executeQueue(){
    SokolSystem::executeQueue();
}

void SystemRender::setUploadBudget(uint64_t bytes){
    SokolCmdQueue::set_upload_budget(bytes);
}

uint64_t SystemRender::getUploadBudget(){
    return SokolCmdQueue::get_upload_budget();
}

uint64_t SystemRender::getUploadToken(){
    return SokolCmdQueue::get_upload_token();
}

bool SystemRender::isUploadTokenComplete(uint64_t token){
    return SokolCmdQueue::is_token_complete(token);
}

size_t SystemRender::getDeferredUploadCount(){
    return SokolCmdQueue::get_deferred_count();
}

uint64_t SystemRender::getUploadBytesLastFrame(){
    return SokolCmdQueue::get_last_frame_upload_bytes();
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
void SystemRender::setViewTag(uint32_t tag){
    SokolSystem::setViewTag(tag);
}

uint32_t SystemRender::getViewTag(){
    return SokolSystem::getViewTag();
}

namespace {
    enum class PassKind { MAIN, SHADOW, DEPTH, OTHER };

    PassKind passKind(const std::string& label){
        if (label == "camera" || label == "camera layer")
            return PassKind::MAIN;
        if (label == "shadow" || label == "shadow static" || label == "point shadow" || label == "point shadow static")
            return PassKind::SHADOW;
        if (label == "depth pre-pass" || label == "gbuffer")
            return PassKind::DEPTH;
        return PassKind::OTHER;
    }
}

RenderStats SystemRender::getRenderStats(uint32_t viewTag){
    RenderStats stats = SokolSystem::getRenderStats(viewTag);
    const PassList list = SokolSystem::getPassList(viewTag, false);
    const bool frameView = viewTag == FRAME_STATS_FRAME;
    for (const PassRecord& pass : list.passes){
        if (!frameView){
            stats.drawCalls += pass.draws;
            stats.instances += pass.instances;
            stats.triangles += pass.triangles;
            stats.vertices += pass.elements;
        }
        if (!pass.executed)
            continue;
        switch (passKind(pass.label)){
        case PassKind::MAIN:
            stats.mainDraws += pass.draws;
            stats.mainTriangles += pass.triangles;
            break;
        case PassKind::SHADOW:
            stats.shadowDraws += pass.draws;
            stats.shadowTriangles += pass.triangles;
            break;
        case PassKind::DEPTH:
            stats.depthDraws += pass.draws;
            break;
        case PassKind::OTHER:
            stats.otherDraws += pass.draws;
            break;
        }
    }
    if (frameView){
        const Engine::FrameStats& frame = Engine::getFrameStats();
        stats.drawCalls = frame.drawCalls;
        stats.instances = frame.instances;
        stats.triangles = frame.triangles;
        stats.vertices = frame.vertices;
        stats.visible = frame.visible;
        stats.culled = frame.culled;
    }
    const DrawBatchStats batches = getDrawBatchStats();
    stats.instancedDraws = batches.instancedDraws;
    stats.batchedInstances = batches.batchedInstances;
    stats.uniformBytesKept = batches.uniformBytesKept;
    return stats;
}

void SystemRender::setDrawBatching(bool enabled){
    SokolDrawBatch::setEnabled(enabled);
}

bool SystemRender::isDrawBatching(){
    return SokolDrawBatch::isEnabled();
}

void SystemRender::beginDrawBatch(){
    SokolDrawBatch::begin();
}

void SystemRender::endDrawBatch(){
    SokolDrawBatch::end();
}

DrawBatchStats SystemRender::getDrawBatchStats(){
    const SokolDrawBatch::FrameStats& frame = SokolDrawBatch::getLastFrameStats();
    DrawBatchStats stats;
    stats.instancedDraws = frame.instancedDraws;
    stats.batchedInstances = frame.batchedInstances;
    stats.uniformBytesKept = frame.uniformBytesKept;
    stats.pipelinesKept = frame.pipelinesKept;
    stats.instanceBytes = frame.instanceBytes;
    stats.offeredDraws = frame.offeredDraws;
    stats.splitPipeline = frame.splitPipeline;
    stats.splitElements = frame.splitElements;
    stats.splitBindings = frame.splitBindings;
    stats.splitUniforms = frame.splitUniforms;
    stats.splitUniformSlots = frame.splitUniformSlots;
    stats.splitBindingParts = frame.splitBindingParts;
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
