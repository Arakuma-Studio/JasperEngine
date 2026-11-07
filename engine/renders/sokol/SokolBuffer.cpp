// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "render/RenderBackendTime.h"
#include "SokolBuffer.h"
#include "memory/MemoryStats.h"

#include <mutex>
#include <unordered_map>
#include "Log.h"
#include "SokolCmdQueue.h"
#include "SokolSystem.h"
#include "Engine.h"

using namespace jasperengine;

namespace {
    // GPU buffer memory (vertices, indices, storage) in the memory stats, per buffer id:
    // buffer copies share one sokol buffer
    std::mutex& trackedBufferMutex(){
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }

    std::unordered_map<uint32_t, uint64_t>& trackedBuffers(){
        static auto* buffers = new std::unordered_map<uint32_t, uint64_t>();
        return *buffers;
    }

    jasperengine::memory::TrackedBytes& bufferBytes(){
        static auto* tracked = new jasperengine::memory::TrackedBytes("Mesh.GPUBuffers", jasperengine::memory::Residency::Gpu);
        return *tracked;
    }

    void trackBuffer(sg_buffer buffer, uint64_t bytes){
        if (buffer.id == SG_INVALID_ID)
            return;
        std::lock_guard<std::mutex> lock(trackedBufferMutex());
        uint64_t& tracked = trackedBuffers()[buffer.id];
        if (tracked)
            bufferBytes().subtract(tracked);
        tracked = bytes;
        bufferBytes().add(bytes);
    }

    void untrackBuffer(sg_buffer buffer){
        std::lock_guard<std::mutex> lock(trackedBufferMutex());
        auto it = trackedBuffers().find(buffer.id);
        if (it == trackedBuffers().end())
            return;
        bufferBytes().subtract(it->second);
        trackedBuffers().erase(it);
    }
}

SokolBuffer::SokolBuffer(){
    buffer.id = SG_INVALID_ID;
    view.id = SG_INVALID_ID;
}

SokolBuffer::SokolBuffer(const SokolBuffer& rhs) = default;

SokolBuffer& SokolBuffer::operator=(const SokolBuffer& rhs) = default;

bool SokolBuffer::createBuffer(unsigned int size, void* data, BufferType type, BufferUsage usage){
    sg_buffer_desc vbuf_desc = {0};

    createdUsage = usage;

    vbuf_desc.size = (size_t)size;
    if (usage == BufferUsage::IMMUTABLE){
        vbuf_desc.data = {data, (size_t)size};
    }

    // TODO: Multi-purpose buffer objects
    // https://floooh.github.io/2025/05/19/sokol-gfx-compute-ms2.html
    if (type == BufferType::VERTEX_BUFFER){
        vbuf_desc.usage.vertex_buffer = true;
    } else if (type == BufferType::INDEX_BUFFER){
        vbuf_desc.usage.index_buffer = true;
    } else if (type == BufferType::STORAGE_BUFFER){
        vbuf_desc.usage.storage_buffer = true;
    }

    if (usage == BufferUsage::IMMUTABLE){
        vbuf_desc.usage.immutable = true;
    }else if (usage == BufferUsage::DYNAMIC){
        vbuf_desc.usage.dynamic_update = true;
    }else if (usage == BufferUsage::STREAM){
        vbuf_desc.usage.stream_update = true;
    }

    if (Engine::isAsyncThread()){
        buffer = SokolCmdQueue::add_command_make_buffer(vbuf_desc);
    }else{
        buffer = sg_make_buffer(vbuf_desc);
    }
    trackBuffer(buffer, vbuf_desc.size);

    if (type == BufferType::STORAGE_BUFFER && buffer.id != SG_INVALID_ID){
        sg_view_desc view_desc = {0};
        view_desc.storage_buffer.buffer = buffer;

        if (Engine::isAsyncThread()){
            view = SokolCmdQueue::add_command_make_view(view_desc);
        }else{
            view = sg_make_view(view_desc);
        }
    }


    return buffer.id != SG_INVALID_ID;
}

// called by draw
bool SokolBuffer::updateBuffer(unsigned int size, void* data){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    if (buffer.id == SG_INVALID_ID || !data || size == 0)
        return false;
    if (Engine::isAsyncThread()){
        // budgeted when the render thread runs it (a refused one is counted and logged there)
        SokolCmdQueue::add_command_update_buffer(buffer, {data, (size_t)size});
        return true;
    }
    return SokolSystem::updateBuffer(buffer, {data, (size_t)size});
}

// main thread: the offset is needed right away
int SokolBuffer::appendBuffer(unsigned int size, const void* data){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    if (buffer.id == SG_INVALID_ID || !data || size == 0 || Engine::isAsyncThread())
        return -1;
    return SokolSystem::appendBuffer(buffer, {data, (size_t)size});
}

void SokolBuffer::destroyBuffer(){
    if (view.id != SG_INVALID_ID && sg_isvalid()){
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_destroy_view(view);
        }else{
            sg_destroy_view(view);
            SokolSystem::forgetAppliedBindings();
        }
    }
    if (buffer.id != SG_INVALID_ID)
        untrackBuffer(buffer);
    if (buffer.id != SG_INVALID_ID && sg_isvalid()){
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_destroy_buffer(buffer);
        }else{
            sg_destroy_buffer(buffer);
            SokolSystem::forgetAppliedBindings();
        }
    }

    buffer.id = SG_INVALID_ID;
    view.id = SG_INVALID_ID;
}

bool SokolBuffer::isCreated() const{
    if (buffer.id != SG_INVALID_ID && sg_isvalid()) {
        return sg_query_buffer_state(buffer) == SG_RESOURCESTATE_VALID;
    }

    return false;
}

bool SokolBuffer::isPending() const{
    if (buffer.id != SG_INVALID_ID && sg_isvalid()) {
        return sg_query_buffer_state(buffer) == SG_RESOURCESTATE_ALLOC;
    }

    return false;
}

BufferUsage SokolBuffer::getCreatedUsage() const{
    return createdUsage;
}

sg_buffer SokolBuffer::get() const{
    return buffer;
}

sg_view SokolBuffer::getView() const{
    return view;
}