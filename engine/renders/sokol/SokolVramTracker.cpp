// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SokolVramTracker.h"

#if JASPERENGINE_PROFILER

#include "profiler/GpuMemory.h"
#include "sokol_gfx.h"
#include "sokol_gpu_timer.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;

namespace {

    void copyLabel(gpumem::Entry& entry, const char* label){
        std::strncpy(entry.label, label ? label : "", sizeof(entry.label) - 1);
        entry.label[sizeof(entry.label) - 1] = '\0';
    }

    // the scope's category and owner, else what the usage says
    void classify(gpumem::Entry& entry, gpumem::Category fallback){
        const gpumem::ScopeState scope = gpumem::currentScope();
        if (scope.active()){
            entry.category = (gpumem::Category)scope.category;
            entry.owner = scope.owner;
            entry.tagged = true;
        }else{
            entry.category = fallback;
        }
    }

}

uint64_t SokolVramTracker::imageBytes(const sg_image_desc& desc){
    const int width = std::max(desc.width, 1);
    const int height = std::max(desc.height, 1);
    const int slices = std::max(desc.num_slices, 1);
    const int mips = std::max(desc.num_mipmaps, 1);
    uint64_t bytes = 0;
    for (int mip = 0; mip < mips; mip++){
        const int w = std::max(width >> mip, 1);
        const int h = std::max(height >> mip, 1);
        const uint64_t surface = (uint64_t)sg_query_surface_pitch(desc.pixel_format, w, h, 1);
        uint64_t layers = 1;
        switch (desc.type){
            case SG_IMAGETYPE_CUBE: layers = 6; break;
            case SG_IMAGETYPE_ARRAY: layers = (uint64_t)slices; break;
            case SG_IMAGETYPE_3D: layers = (uint64_t)std::max(slices >> mip, 1); break;
            default: break;
        }
        bytes += surface * layers;
    }
    bytes *= (uint64_t)std::max(desc.sample_count, 1);
    // updatable images have a copy per frame in flight
    if (!desc.usage.immutable && (desc.usage.dynamic_update || desc.usage.stream_update))
        bytes *= SG_NUM_INFLIGHT_FRAMES;
    return bytes;
}

uint64_t SokolVramTracker::bufferBytes(const sg_buffer_desc& desc){
    uint64_t bytes = (uint64_t)(desc.size > 0 ? desc.size : desc.data.size);
    if (!desc.usage.immutable && (desc.usage.dynamic_update || desc.usage.stream_update))
        bytes *= SG_NUM_INFLIGHT_FRAMES;
    return bytes;
}

void SokolVramTracker::onImageCreated(uint32_t image, const sg_image_desc* desc){
    if (!desc || image == SG_INVALID_ID || sg_query_image_state(sg_image{image}) != SG_RESOURCESTATE_VALID)
        return;
    gpumem::Entry entry;
    entry.id = image;
    entry.width = desc->width;
    entry.height = desc->height;
    entry.mips = (uint8_t)std::clamp(desc->num_mipmaps, 1, 255);
    entry.samples = (uint8_t)std::clamp(desc->sample_count, 1, 255);
    entry.slices = (uint16_t)std::clamp(desc->type == SG_IMAGETYPE_CUBE ? 6 : desc->num_slices, 1, 65535);
    entry.format = sgj_pixel_format_name((int)desc->pixel_format);
    entry.bytes = imageBytes(*desc);
    const sg_image_usage& usage = desc->usage;
    if (usage.immutable) entry.usage |= gpumem::USAGE_IMMUTABLE;
    if (usage.dynamic_update) entry.usage |= gpumem::USAGE_DYNAMIC;
    if (usage.stream_update) entry.usage |= gpumem::USAGE_STREAM;
    if (usage.color_attachment || usage.resolve_attachment || usage.depth_stencil_attachment) entry.usage |= gpumem::USAGE_ATTACHMENT;
    if (usage.storage_image) entry.usage |= gpumem::USAGE_STORAGE;
    if (usage.depth_stencil_attachment)
        entry.kind = gpumem::Kind::DepthTarget;
    else if (usage.color_attachment || usage.resolve_attachment || usage.storage_image)
        entry.kind = gpumem::Kind::RenderTarget;
    else
        entry.kind = gpumem::Kind::Texture;
    copyLabel(entry, desc->label);
    classify(entry, entry.kind == gpumem::Kind::Texture ? gpumem::Category::Textures : gpumem::Category::RenderTargets);
    gpumem::track(entry, true);
}

void SokolVramTracker::onImageDestroyed(uint32_t image){
    gpumem::untrack(image, true);
}

void SokolVramTracker::onBufferCreated(uint32_t buffer, const sg_buffer_desc* desc){
    if (!desc || buffer == SG_INVALID_ID || sg_query_buffer_state(sg_buffer{buffer}) != SG_RESOURCESTATE_VALID)
        return;
    gpumem::Entry entry;
    entry.id = buffer;
    entry.kind = gpumem::Kind::Buffer;
    entry.width = (int32_t)std::min<uint64_t>((uint64_t)(desc->size > 0 ? desc->size : desc->data.size), 0x7FFFFFFF);
    entry.height = 1;
    entry.format = "";
    entry.bytes = bufferBytes(*desc);
    const sg_buffer_usage& usage = desc->usage;
    if (usage.immutable) entry.usage |= gpumem::USAGE_IMMUTABLE;
    if (usage.dynamic_update) entry.usage |= gpumem::USAGE_DYNAMIC;
    if (usage.stream_update) entry.usage |= gpumem::USAGE_STREAM;
    if (usage.storage_buffer) entry.usage |= gpumem::USAGE_STORAGE;
    if (usage.vertex_buffer) entry.usage |= gpumem::USAGE_VERTEX;
    if (usage.index_buffer) entry.usage |= gpumem::USAGE_INDEX;
    copyLabel(entry, desc->label);
    gpumem::Category fallback = gpumem::Category::Other;
    if (usage.stream_update)
        fallback = gpumem::Category::Uniforms;
    else if (usage.vertex_buffer || usage.index_buffer)
        fallback = gpumem::Category::Meshes;
    classify(entry, fallback);
    gpumem::track(entry, false);
}

void SokolVramTracker::onBufferDestroyed(uint32_t buffer){
    gpumem::untrack(buffer, false);
}

const char* SokolVramTracker::imageLabel(uint32_t image){
    return gpumem::label(image, true);
}

#endif
