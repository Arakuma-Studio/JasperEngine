// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "render/RenderBackendTime.h"
#include "SokolTexture.h"
#include "Log.h"
#include "SokolCmdQueue.h"
#include "SokolSystem.h"
#include "render/SystemRender.h"
#include "Engine.h"
#include "texture/TextureData.h"

#include "memory/MemoryStats.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "meshoptimizer.h"

// Sokol never sets TRANSFER_SRC, needed to copy render targets out: editor thumbnails
// (GraphicUtils) and a game's --screenshot (FrameReadback, docs/specs/S30-shipping.md R20).
// Wrapping the call keeps sokol itself unpatched (re-check when sokol_gfx is updated)
#if defined(SOKOL_VULKAN)
// (vulkan_win32.h includes <windows.h>)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <vulkan/vulkan.h>

namespace {
VkResult createSokolImage(VkDevice device, const VkImageCreateInfo* createInfo,
                          const VkAllocationCallbacks* allocator, VkImage* image);
}

#define vkCreateImage createSokolImage

#include "../../libs/sokol/sokol.cpp"

#undef vkCreateImage

namespace {
VkResult createSokolImage(VkDevice device, const VkImageCreateInfo* createInfo,
                          const VkAllocationCallbacks* allocator, VkImage* image){
    VkImageCreateInfo info = *createInfo;
    if (info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
#if defined(JASPERENGINE_TEXTURE_READBACK)
    // the tests also read sampled textures back (dynamic textures)
    if (!(info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
#endif
    return vkCreateImage(device, &info, allocator, image);
}
}
#endif

using namespace jasperengine;

namespace {
    // The sampler of every float data texture (createDynamicTexture: bones, lattice records),
    // which only texelFetch reads: one for all of them, not one each (a skinned mesh has a
    // bones texture: 10,000 of them would each hold a sampler). Destroyed with its last holder.
    struct SharedDataSampler {
        sg_sampler sampler = {SG_INVALID_ID};
        uint64_t context = 0;

        ~SharedDataSampler(){
            if (sampler.id == SG_INVALID_ID || context != SystemRender::getContextId() || !sg_isvalid())
                return;
            if (Engine::isAsyncThread()){
                SokolCmdQueue::add_command_destroy_sampler(sampler);
            }else{
                sg_destroy_sampler(sampler);
                SokolSystem::forgetAppliedBindings();
            }
        }
    };

    std::mutex& getDataSamplerMutex(){
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }

    std::shared_ptr<SharedDataSampler> getDataSampler(){
        std::lock_guard<std::mutex> lock(getDataSamplerMutex());
        static std::weak_ptr<SharedDataSampler>* cached = new std::weak_ptr<SharedDataSampler>();
        std::shared_ptr<SharedDataSampler> shared = cached->lock();
        if (shared && shared->context == SystemRender::getContextId()){
            const sg_resource_state state = sg_query_sampler_state(shared->sampler);
            // one the loading thread made is drawn by the main thread once the queue has run
            if (state == SG_RESOURCESTATE_VALID || (state == SG_RESOURCESTATE_ALLOC && Engine::isAsyncThread()))
                return shared;
        }

        sg_sampler_desc samplerDesc = {};
        samplerDesc.min_filter = SG_FILTER_NEAREST;
        samplerDesc.mag_filter = SG_FILTER_NEAREST;
        samplerDesc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
        samplerDesc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
        samplerDesc.label = "data-texture-sampler";

        shared = std::make_shared<SharedDataSampler>();
        shared->context = SystemRender::getContextId();
        if (Engine::isAsyncThread()){
            shared->sampler = SokolCmdQueue::add_command_make_sampler(samplerDesc);
        }else{
            shared->sampler = sg_make_sampler(samplerDesc);
        }
        if (shared->sampler.id == SG_INVALID_ID)
            return nullptr;
        *cached = shared;
        return shared;
    }
}

const void* SokolTexture::getVulkanView(sg_view view) {
#if defined(SOKOL_VULKAN)
    const _sg_view_t* resource = _sg_lookup_view(view.id);
    return resource ? reinterpret_cast<const void*>(resource->vk.img_view) : nullptr;
#else
    (void)view;
    return nullptr;
#endif
}

const void* SokolTexture::getVulkanImage(sg_image image) {
#if defined(SOKOL_VULKAN)
    const _sg_image_t* resource = _sg_lookup_image(image.id);
    return resource ? reinterpret_cast<const void*>(resource->vk.img) : nullptr;
#else
    (void)image;
    return nullptr;
#endif
}

namespace {
    // GPU texture memory in the memory stats (get_memory_stats, Jobs & Memory): estimated
    // from each image's description when it is made, given back when it is destroyed.
    // Texture copies share an image, so the count is kept per image id.
    struct TrackedImage {
        uint64_t bytes = 0;
        jasperengine::memory::TrackedBytes* record = nullptr;
    };

    std::mutex& trackedImageMutex(){
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }

    std::unordered_map<uint32_t, TrackedImage>& trackedImages(){
        static auto* images = new std::unordered_map<uint32_t, TrackedImage>();
        return *images;
    }

    jasperengine::memory::TrackedBytes& textureBytes(){
        static auto* tracked = new jasperengine::memory::TrackedBytes("Texture.GPU", jasperengine::memory::Residency::Gpu);
        return *tracked;
    }

    jasperengine::memory::TrackedBytes& renderTargetBytes(){
        static auto* tracked = new jasperengine::memory::TrackedBytes("Texture.RenderTargets", jasperengine::memory::Residency::Gpu);
        return *tracked;
    }

    uint64_t pixelBytes(sg_pixel_format format){
        switch (format){
            case SG_PIXELFORMAT_R8: return 1;
            case SG_PIXELFORMAT_R16: case SG_PIXELFORMAT_RG8: case SG_PIXELFORMAT_RGB565: case SG_PIXELFORMAT_RGBA4: return 2;
            case SG_PIXELFORMAT_R16F: return 2;
            case SG_PIXELFORMAT_RGBA16F: return 8;
            case SG_PIXELFORMAT_RGBA32F: return 16;
            default: return 4;
        }
    }

    // the exact bytes of an upload of levels (block formats included)
    void trackImageBytes(sg_image image, uint64_t bytes){
        if (image.id == SG_INVALID_ID)
            return;
        jasperengine::memory::TrackedBytes& record = textureBytes();
        std::lock_guard<std::mutex> lock(trackedImageMutex());
        TrackedImage& tracked = trackedImages()[image.id];
        if (tracked.record)
            tracked.record->subtract(tracked.bytes);
        tracked.bytes = bytes;
        tracked.record = &record;
        record.add(bytes);
    }

    // a full mip chain is ~4/3 of the base level
    void trackImage(sg_image image, const sg_image_desc& desc, bool mipChain, bool renderTarget){
        if (image.id == SG_INVALID_ID)
            return;
        const uint64_t slices = desc.type == SG_IMAGETYPE_CUBE ? 6 : (uint64_t)(desc.num_slices > 0 ? desc.num_slices : 1);
        const uint64_t samples = desc.sample_count > 1 ? (uint64_t)desc.sample_count : 1;
        uint64_t bytes = (uint64_t)desc.width * (uint64_t)desc.height * pixelBytes(desc.pixel_format) * slices * samples;
        if (mipChain || desc.num_mipmaps > 1)
            bytes = bytes * 4 / 3;
        jasperengine::memory::TrackedBytes& record = renderTarget ? renderTargetBytes() : textureBytes();
        std::lock_guard<std::mutex> lock(trackedImageMutex());
        TrackedImage& tracked = trackedImages()[image.id];
        if (tracked.record)
            tracked.record->subtract(tracked.bytes);
        tracked.bytes = bytes;
        tracked.record = &record;
        record.add(bytes);
    }

    void untrackImage(sg_image image){
        std::lock_guard<std::mutex> lock(trackedImageMutex());
        auto it = trackedImages().find(image.id);
        if (it == trackedImages().end())
            return;
        if (it->second.record)
            it->second.record->subtract(it->second.bytes);
        trackedImages().erase(it);
    }
}

SokolTexture::SokolTexture(){
    image.id = SG_INVALID_ID;
    sampler.id = SG_INVALID_ID;
    view.id = SG_INVALID_ID;
    streamBytes = 0;
    aliasImage = false;
    dynamicMap = false;
    lastMapUpdateFrame = UINT64_MAX;
}

SokolTexture::SokolTexture(const SokolTexture& rhs) = default;

SokolTexture& SokolTexture::operator=(const SokolTexture& rhs) = default;

sg_image_type SokolTexture::getTextureType(TextureType textureType){
    if (textureType == TextureType::TEXTURE_2D){
        return SG_IMAGETYPE_2D;
    }else if (textureType == TextureType::TEXTURE_CUBE){
        return SG_IMAGETYPE_CUBE;
    }else if (textureType == TextureType::TEXTURE_ARRAY){
        return SG_IMAGETYPE_ARRAY;
    }else if (textureType == TextureType::TEXTURE_3D){
        return SG_IMAGETYPE_3D;
    }

    return _SG_IMAGETYPE_DEFAULT;
}

sg_filter SokolTexture::getFilter(TextureFilter textureFilter){
    if (textureFilter == TextureFilter::LINEAR){
        return SG_FILTER_LINEAR;
    }else if (textureFilter == TextureFilter::NEAREST){
        return SG_FILTER_NEAREST;
    }else if (textureFilter == TextureFilter::LINEAR_MIPMAP_LINEAR){
        return SG_FILTER_LINEAR;
    }else if (textureFilter == TextureFilter::LINEAR_MIPMAP_NEAREST){
        return SG_FILTER_LINEAR;
    }else if (textureFilter == TextureFilter::NEAREST_MIPMAP_NEAREST){
        return SG_FILTER_NEAREST;
    }else if (textureFilter == TextureFilter::NEAREST_MIPMAP_LINEAR){
        return SG_FILTER_NEAREST;
    }

    return _SG_FILTER_DEFAULT;
}

sg_filter SokolTexture::getFilterMipmap(TextureFilter textureFilter){
    if (textureFilter == TextureFilter::LINEAR_MIPMAP_LINEAR){
        return SG_FILTER_LINEAR;
    }else if (textureFilter == TextureFilter::LINEAR_MIPMAP_NEAREST){
        return SG_FILTER_NEAREST;
    }else if (textureFilter == TextureFilter::NEAREST_MIPMAP_NEAREST){
        return SG_FILTER_NEAREST;
    }else if (textureFilter == TextureFilter::NEAREST_MIPMAP_LINEAR){
        return SG_FILTER_LINEAR;
    }

    return _SG_FILTER_DEFAULT;
}

sg_wrap SokolTexture::getWrap(TextureWrap textureWrap){
    if (textureWrap == TextureWrap::REPEAT){
        return SG_WRAP_REPEAT;
    }else if (textureWrap == TextureWrap::MIRRORED_REPEAT){
        return SG_WRAP_MIRRORED_REPEAT;
    }else if (textureWrap == TextureWrap::CLAMP_TO_EDGE){
        return SG_WRAP_CLAMP_TO_EDGE;
    }else if (textureWrap == TextureWrap::CLAMP_TO_BORDER){
        return SG_WRAP_CLAMP_TO_BORDER;
    }

    return _SG_WRAP_DEFAULT;
}

// GLES3 and WebGL2 have no unorm16 textures (sokol reports R16 unsupported there),
// so 16-bit heightmaps go up as R32F where float textures filter, else as R16F,
// which always does. Every mip level is converted; returns the converted pixels (one
// block), freed once the image is made, and the bytes they take.
static void* unorm16ToFloat(sg_image_desc* desc, uint64_t* bytes = nullptr){
    const bool full = sg_query_pixelformat(SG_PIXELFORMAT_R32F).filter;
    const size_t texel = full ? sizeof(float) : sizeof(uint16_t);
    size_t total = 0;
    for (int level = 0; level < SG_MAX_MIPMAPS && desc->data.mip_levels[level].ptr; level++){
        total += desc->data.mip_levels[level].size / sizeof(uint16_t) * texel;
    }
    unsigned char* block = (unsigned char*)malloc(total);
    unsigned char* at = block;
    for (int level = 0; level < SG_MAX_MIPMAPS && desc->data.mip_levels[level].ptr; level++){
        const uint16_t* source = (const uint16_t*)desc->data.mip_levels[level].ptr;
        const size_t count = desc->data.mip_levels[level].size / sizeof(uint16_t);
        for (size_t i = 0; i < count; i++){
            if (full){
                const float value = source[i] / 65535.0f;
                memcpy(at + i * texel, &value, texel);
            }else{
                const uint16_t value = meshopt_quantizeHalf(source[i] / 65535.0f);
                memcpy(at + i * texel, &value, texel);
            }
        }
        desc->data.mip_levels[level].ptr = at;
        desc->data.mip_levels[level].size = count * texel;
        at += count * texel;
    }
    desc->pixel_format = full ? SG_PIXELFORMAT_R32F : SG_PIXELFORMAT_R16F;
    if (bytes){
        *bytes = total;
    }
    return block;
}

bool SokolTexture::getDataPixelFormat(ColorFormat colorFormat, sg_pixel_format& pixelFormat, size_t& bytesPerTexel){
    switch (colorFormat){
        case ColorFormat::RGBA:    pixelFormat = SG_PIXELFORMAT_RGBA8;   bytesPerTexel = 4;  return true;
        case ColorFormat::RED:     pixelFormat = SG_PIXELFORMAT_R8;      bytesPerTexel = 1;  return true;
        case ColorFormat::RED16:   pixelFormat = SG_PIXELFORMAT_R16;     bytesPerTexel = 2;  return true;
        case ColorFormat::RGBA16F: pixelFormat = SG_PIXELFORMAT_RGBA16F; bytesPerTexel = 8;  return true;
        case ColorFormat::RGBA32F: pixelFormat = SG_PIXELFORMAT_RGBA32F; bytesPerTexel = 16; return true;
        case ColorFormat::R32F:    pixelFormat = SG_PIXELFORMAT_R32F;    bytesPerTexel = 4;  return true;
        case ColorFormat::RG16F:   pixelFormat = SG_PIXELFORMAT_RG16F;   bytesPerTexel = 4;  return true;
        case ColorFormat::R16F:    pixelFormat = SG_PIXELFORMAT_R16F;    bytesPerTexel = 2;  return true;
        default: break;
    }
    pixelFormat = _SG_PIXELFORMAT_DEFAULT;
    bytesPerTexel = 0;
    return false;
}

bool SokolTexture::isLinearFilterable(ColorFormat colorFormat, const RenderFeatures& features){
    switch (colorFormat){
        case ColorFormat::RGBA16F: return features.filterRGBA16F;
        case ColorFormat::RGBA32F: return features.filterRGBA32F;
        case ColorFormat::R32F:    return features.filterR32F;
        case ColorFormat::RG16F:   return features.filterRG16F;
        case ColorFormat::R16F:    return features.filterR16F;
        default:                   return true;
    }
}

static bool isFloatPixelFormat(sg_pixel_format pixelFormat){
    return pixelFormat == SG_PIXELFORMAT_RGBA16F || pixelFormat == SG_PIXELFORMAT_RGBA32F ||
           pixelFormat == SG_PIXELFORMAT_R32F || pixelFormat == SG_PIXELFORMAT_RG16F ||
           pixelFormat == SG_PIXELFORMAT_R16F;
}

bool SokolTexture::createTexture(
            const std::string& label, int width, int height,
            ColorFormat colorFormat, TextureType type, int numFaces, void* data[], size_t size[], 
            TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV){

    sg_pixel_format pixelFormat;
    size_t bytesPerTexel;
    if (!getDataPixelFormat(colorFormat, pixelFormat, bytesPerTexel)){
        Log::error("Texture '%s': a texture from data can't have colour format %s (RGBA, RED, RED16, RGBA16F, RGBA32F, R32F, RG16F or R16F)",
                   label.c_str(), TextureData::getColorFormatName(colorFormat));
        return false;
    }

    // float data: checked here (sokol only validates in debug builds, and reads
    // width * height * bytesPerTexel bytes whatever the size says)
    const bool floatFormat = isFloatPixelFormat(pixelFormat);
    if (floatFormat){
        const RenderFeatures& features = SystemRender::getFeatures();
        if (sg_isvalid() && !sg_query_pixelformat(pixelFormat).sample){
            Log::error("Texture '%s': this device can't sample colour format %s", label.c_str(), TextureData::getColorFormatName(colorFormat));
            return false;
        }
        if (width <= 0 || height <= 0 ||
            (features.maxTextureSize2D > 0 && (width > features.maxTextureSize2D || height > features.maxTextureSize2D))){
            Log::error("Texture '%s': size %i x %i is not supported (the device's limit is %i)",
                       label.c_str(), width, height, features.maxTextureSize2D);
            return false;
        }
        const int faces = (numFaces > 1) ? numFaces : 1;
        const size_t expected = (size_t)width * (size_t)height * bytesPerTexel;
        for (int f = 0; f < faces; f++){
            if (!data[f] || size[f] != expected){
                Log::error("Texture '%s': face %i has %zu bytes of data, %i x %i %s takes %zu",
                           label.c_str(), f, data[f] ? size[f] : (size_t)0, width, height, TextureData::getColorFormatName(colorFormat), expected);
                return false;
            }
        }
    }

    sg_image_desc image_desc = {0};
    image_desc.type = getTextureType(type);
    image_desc.width = width;
    image_desc.height = height;
    image_desc.pixel_format = pixelFormat;
    image_desc.num_slices = (type == TextureType::TEXTURE_CUBE)? 6 : ((type == TextureType::TEXTURE_ARRAY)? numFaces : 1);
    image_desc.label = label.c_str();

    sg_sampler_desc sampler_desc = {0};
    sampler_desc.min_filter = getFilter(minFilter);
    sampler_desc.mag_filter = getFilter(magFilter);
    sampler_desc.mipmap_filter = getFilterMipmap(minFilter);
    sampler_desc.wrap_u = getWrap(wrapU);
    sampler_desc.wrap_v = getWrap(wrapV);

    if (floatFormat){
        // one level (mipmap generation averages 8-bit texels only), so no mipmap filter
        sampler_desc.mipmap_filter = _SG_FILTER_DEFAULT;
        // a linear filter on a format the device can't filter is an invalid sampler (GLES3
        // incomplete texture, Vulkan/Metal validation): nearest instead
        if (!isLinearFilterable(colorFormat, SystemRender::getFeatures()) &&
            (sampler_desc.min_filter == SG_FILTER_LINEAR || sampler_desc.mag_filter == SG_FILTER_LINEAR)){
            Log::warn("Texture '%s': this device can't filter colour format %s linearly, sampled with NEAREST",
                      label.c_str(), TextureData::getColorFormatName(colorFormat));
            sampler_desc.min_filter = SG_FILTER_NEAREST;
            sampler_desc.mag_filter = SG_FILTER_NEAREST;
        }
    }

    // all faces of a mip level must be in one contiguous memory block
    unsigned char* combined = NULL;
    if (numFaces > 1){
        size_t total_size = 0;
        for (int f = 0; f < numFaces; f++){
            total_size += size[f];
        }
        combined = (unsigned char*)malloc(total_size);
        size_t offset = 0;
        for (int f = 0; f < numFaces; f++){
            memcpy(combined + offset, data[f], size[f]);
            offset += size[f];
        }
        image_desc.data.mip_levels[0].ptr = combined;
        image_desc.data.mip_levels[0].size = total_size;
    }else{
        image_desc.data.mip_levels[0].ptr = data[0];
        image_desc.data.mip_levels[0].size = size[0];
    }

    void* converted = NULL;
    if (pixelFormat == SG_PIXELFORMAT_R16 && !sg_query_pixelformat(SG_PIXELFORMAT_R16).filter){
        converted = unorm16ToFloat(&image_desc);
    }

    if (Engine::isAsyncThread()){
        image = SokolCmdQueue::add_command_make_image(image_desc);
    }else{
        image = sg_make_image(image_desc);
    }
    trackImage(image, image_desc, false, false);
    // must come after image creation: in the synchronous path scheduleCleanup
    // frees immediately, and the data is only consumed inside sg_make_image
    if (combined){
        SystemRender::scheduleCleanup(std::free, combined);
    }
    if (converted){
        SystemRender::scheduleCleanup(std::free, converted);
    }
    if (Engine::isAsyncThread()){
        sampler = SokolCmdQueue::add_command_make_sampler(sampler_desc);
    }else{
        sampler = sg_make_sampler(sampler_desc);
    }

    createTextureView(label.c_str());

    if (image.id != SG_INVALID_ID && sampler.id != SG_INVALID_ID && view.id != SG_INVALID_ID)
        return true;

    return false;
}

sg_pixel_format SokolTexture::toSokol(GpuTextureFormat format, bool srgb){
    (void)srgb;   // D7: every colour texture uploads UNORM; the sRGB flag rides in the KTX2 DFD
    switch (format){
        case GpuTextureFormat::R8: return SG_PIXELFORMAT_R8;
        case GpuTextureFormat::RG8: return SG_PIXELFORMAT_RG8;
        case GpuTextureFormat::RGBA8: return SG_PIXELFORMAT_RGBA8;
        case GpuTextureFormat::R16: return SG_PIXELFORMAT_R16;
        case GpuTextureFormat::RGB565: return SG_PIXELFORMAT_RGB565;
        case GpuTextureFormat::RGBA4: return SG_PIXELFORMAT_RGBA4;
        case GpuTextureFormat::RGB9E5: return SG_PIXELFORMAT_RGB9E5;
        case GpuTextureFormat::RGBA16F: return SG_PIXELFORMAT_RGBA16F;
        case GpuTextureFormat::BC1: return SG_PIXELFORMAT_BC1_RGBA;
        case GpuTextureFormat::BC3: return SG_PIXELFORMAT_BC3_RGBA;
        case GpuTextureFormat::BC4: return SG_PIXELFORMAT_BC4_R;
        case GpuTextureFormat::BC5: return SG_PIXELFORMAT_BC5_RG;
        case GpuTextureFormat::BC6H: return SG_PIXELFORMAT_BC6H_RGBUF;
        case GpuTextureFormat::BC7: return SG_PIXELFORMAT_BC7_RGBA;
        case GpuTextureFormat::ETC2_RGB8: return SG_PIXELFORMAT_ETC2_RGB8;
        case GpuTextureFormat::ETC2_RGBA8: return SG_PIXELFORMAT_ETC2_RGBA8;
        case GpuTextureFormat::EAC_R11: return SG_PIXELFORMAT_EAC_R11;
        case GpuTextureFormat::EAC_RG11: return SG_PIXELFORMAT_EAC_RG11;
        case GpuTextureFormat::ASTC_4x4: return SG_PIXELFORMAT_ASTC_4x4_RGBA;
        case GpuTextureFormat::ASTC_5x5: return SG_PIXELFORMAT_ASTC_5x5_RGBA;
        case GpuTextureFormat::ASTC_6x6: return SG_PIXELFORMAT_ASTC_6x6_RGBA;
        case GpuTextureFormat::ASTC_8x8: return SG_PIXELFORMAT_ASTC_8x8_RGBA;
        case GpuTextureFormat::ASTC_10x10: return SG_PIXELFORMAT_ASTC_10x10_RGBA;
        case GpuTextureFormat::ASTC_12x12: return SG_PIXELFORMAT_ASTC_12x12_RGBA;
    }
    return SG_PIXELFORMAT_RGBA8;
}

bool SokolTexture::isFormatSupported(GpuTextureFormat format){
    if (!sg_isvalid())
        return false;
    const sg_pixelformat_info info = sg_query_pixelformat(toSokol(format, false));
    return info.sample && info.filter;
}

bool SokolTexture::createTextureLevels(
            const std::string& label, TextureType type, GpuTextureFormat format,
            int width, int height, int numFaces, std::span<const TextureLevel> levels,
            TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV){
    if (levels.empty() || (int)levels.size() > SG_MAX_MIPMAPS || width <= 0 || height <= 0){
        Log::error("Texture '%s': %zu levels of %i x %i cannot be uploaded", label.c_str(), levels.size(), width, height);
        return false;
    }
    const int slices = (type == TextureType::TEXTURE_CUBE) ? 6 : ((type == TextureType::TEXTURE_ARRAY) ? std::max(1, numFaces) : 1);
    uint64_t bytes = 0;
    for (size_t level = 0; level < levels.size(); level++){
        const int levelWidth = mipDimension(width, (int)level);
        const int levelHeight = mipDimension(height, (int)level);
        const size_t expected = gpuFormatLevelBytes(format, levelWidth, levelHeight) * (size_t)slices;
        if (!levels[level].data || levels[level].size != expected){
            Log::error("Texture '%s': level %zu has %zu bytes, %s %i x %i x %i needs %zu",
                       label.c_str(), level, levels[level].size, gpuFormatName(format), levelWidth, levelHeight, slices, expected);
            return false;
        }
        bytes += expected;
    }

    sg_image_desc imageDesc = {};
    imageDesc.type = getTextureType(type);
    imageDesc.width = width;
    imageDesc.height = height;
    imageDesc.pixel_format = toSokol(format, false);
    imageDesc.num_slices = slices;
    imageDesc.num_mipmaps = (int)levels.size();
    imageDesc.label = label.c_str();
    for (size_t level = 0; level < levels.size(); level++){
        imageDesc.data.mip_levels[level].ptr = levels[level].data;
        imageDesc.data.mip_levels[level].size = levels[level].size;
    }

    // a 16-bit height map where unorm16 does not filter (GLES3, WebGL2): as float, full precision
    void* converted = nullptr;
    if (format == GpuTextureFormat::R16 && !sg_query_pixelformat(SG_PIXELFORMAT_R16).filter){
        converted = unorm16ToFloat(&imageDesc, &bytes);
    }

    sg_sampler_desc samplerDesc = {};
    samplerDesc.min_filter = getFilter(minFilter);
    samplerDesc.mag_filter = getFilter(magFilter);
    samplerDesc.mipmap_filter = getFilterMipmap(minFilter);
    samplerDesc.wrap_u = getWrap(wrapU);
    samplerDesc.wrap_v = getWrap(wrapV);

    if (Engine::isAsyncThread()){
        image = SokolCmdQueue::add_command_make_image(imageDesc);
        sampler = SokolCmdQueue::add_command_make_sampler(samplerDesc);
    }else{
        image = sg_make_image(imageDesc);
        sampler = sg_make_sampler(samplerDesc);
    }
    // after the image: the synchronous path frees at once, and sg_make_image consumed the data
    if (converted){
        SystemRender::scheduleCleanup(std::free, converted);
    }
    trackImageBytes(image, bytes);
    createTextureView(label.c_str());
    return image.id != SG_INVALID_ID && sampler.id != SG_INVALID_ID && view.id != SG_INVALID_ID;
}

bool SokolTexture::createTexture3D(
            const std::string& label, int width, int height, int depth, const void* data, size_t size,
            TextureFilter filter, TextureWrap wrap){
    if (width <= 0 || height <= 0 || depth <= 0 || !data || size != (size_t)width * (size_t)height * (size_t)depth * 4){
        Log::error("3D texture '%s': invalid size %i x %i x %i (%zu bytes)", label.c_str(), width, height, depth, size);
        return false;
    }

    sg_image_desc imageDesc = {};
    imageDesc.type = SG_IMAGETYPE_3D;
    imageDesc.width = width;
    imageDesc.height = height;
    imageDesc.num_slices = depth;
    imageDesc.pixel_format = SG_PIXELFORMAT_RGBA8;
    imageDesc.data.mip_levels[0].ptr = data;
    imageDesc.data.mip_levels[0].size = size;
    imageDesc.label = label.c_str();

    sg_sampler_desc samplerDesc = {};
    samplerDesc.min_filter = getFilter(filter);
    samplerDesc.mag_filter = getFilter(filter);
    samplerDesc.wrap_u = getWrap(wrap);
    samplerDesc.wrap_v = getWrap(wrap);
    samplerDesc.wrap_w = getWrap(wrap);

    if (Engine::isAsyncThread()){
        image = SokolCmdQueue::add_command_make_image(imageDesc);
        sampler = SokolCmdQueue::add_command_make_sampler(samplerDesc);
    }else{
        image = sg_make_image(imageDesc);
        sampler = sg_make_sampler(samplerDesc);
    }
    createTextureView(label.c_str());
    return image.id != SG_INVALID_ID && sampler.id != SG_INVALID_ID && view.id != SG_INVALID_ID;
}

bool SokolTexture::createDynamicTexture(const std::string& label, int width, int height){
    sg_image_desc imageDesc = {};
    imageDesc.usage.stream_update = true;
    imageDesc.width = width;
    imageDesc.height = height;
    imageDesc.pixel_format = SG_PIXELFORMAT_RGBA32F;
    imageDesc.label = label.c_str();

    if (Engine::isAsyncThread()){
        image = SokolCmdQueue::add_command_make_image(imageDesc);
    }else{
        image = sg_make_image(imageDesc);
    }
    trackImage(image, imageDesc, false, false);
    // nearest, clamped: the one of every data texture
    std::shared_ptr<SharedDataSampler> shared = getDataSampler();
    sharedSampler = shared;
    sampler = shared ? shared->sampler : sg_sampler{SG_INVALID_ID};
    createTextureView(label.c_str());
    return image.id != SG_INVALID_ID && sampler.id != SG_INVALID_ID && view.id != SG_INVALID_ID;
}

bool SokolTexture::createDynamicTexture(const std::string& label, int width, int height,
            ColorFormat colorFormat, TextureFilter filter, TextureWrap wrap){
    sg_pixel_format pixelFormat = _SG_PIXELFORMAT_DEFAULT;
    size_t bytesPerPixel;
    if (colorFormat == ColorFormat::RGBA){
        pixelFormat = SG_PIXELFORMAT_RGBA8;
        bytesPerPixel = 4;
    }else if (colorFormat == ColorFormat::RED){
        pixelFormat = SG_PIXELFORMAT_R8;
        bytesPerPixel = 1;
    }else{
        Log::error("Dynamic texture '%s': only RGBA8 and R8 are supported", label.c_str());
        return false;
    }
    if (width <= 0 || height <= 0){
        Log::error("Dynamic texture '%s': invalid size %i x %i", label.c_str(), width, height);
        return false;
    }

    sg_image_desc imageDesc = {};
    imageDesc.usage.stream_update = true;
    imageDesc.width = width;
    imageDesc.height = height;
    imageDesc.pixel_format = pixelFormat;
    imageDesc.label = label.c_str();

    // no mipmaps: a mipmap filter samples level 0 with its base filter
    sg_sampler_desc samplerDesc = {};
    samplerDesc.min_filter = getFilter(filter);
    samplerDesc.mag_filter = getFilter(filter);
    samplerDesc.wrap_u = getWrap(wrap);
    samplerDesc.wrap_v = getWrap(wrap);

    if (Engine::isAsyncThread()){
        image = SokolCmdQueue::add_command_make_image(imageDesc);
        sampler = SokolCmdQueue::add_command_make_sampler(samplerDesc);
    }else{
        image = sg_make_image(imageDesc);
        sampler = sg_make_sampler(samplerDesc);
    }
    trackImage(image, imageDesc, false, false);
    createTextureView(label.c_str());
    streamBytes = (size_t)width * (size_t)height * bytesPerPixel;
    return image.id != SG_INVALID_ID && sampler.id != SG_INVALID_ID && view.id != SG_INVALID_ID;
}

bool SokolTexture::createDynamicColorTexture(const std::string& label, int width, int height){
    if (width <= 0 || height <= 0){
        Log::error("Dynamic texture '%s': invalid size %i x %i", label.c_str(), width, height);
        return false;
    }

    sg_image_desc imageDesc = {};
    imageDesc.usage.dynamic_update = true;
    imageDesc.width = width;
    imageDesc.height = height;
    imageDesc.pixel_format = SG_PIXELFORMAT_RGBA8;
    imageDesc.label = label.c_str();

    sg_sampler_desc samplerDesc = {};
    samplerDesc.min_filter = SG_FILTER_LINEAR;
    samplerDesc.mag_filter = SG_FILTER_LINEAR;
    samplerDesc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    samplerDesc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;

    if (Engine::isAsyncThread()){
        image = SokolCmdQueue::add_command_make_image(imageDesc);
        sampler = SokolCmdQueue::add_command_make_sampler(samplerDesc);
    }else{
        image = sg_make_image(imageDesc);
        sampler = sg_make_sampler(samplerDesc);
    }
    trackImage(image, imageDesc, false, false);
    createTextureView(label.c_str());
    // updated whole (the LUT strip), so the size check holds
    streamBytes = (size_t)width * (size_t)height * 4;
    return image.id != SG_INVALID_ID && sampler.id != SG_INVALID_ID && view.id != SG_INVALID_ID;
}

bool SokolTexture::createDynamicMapTexture(const std::string& label, int width, int height, ColorFormat colorFormat){
    destroyTexture();
    if (width <= 0 || height <= 0){
        Log::error("Dynamic map '%s': invalid size %i x %i", label.c_str(), width, height);
        return false;
    }
    sg_pixel_format pixelFormat;
    size_t bytesPerTexel;
    if (colorFormat == ColorFormat::RGBA){
        pixelFormat = SG_PIXELFORMAT_RGBA8;
        bytesPerTexel = 4;
        uploadFormat = ColorFormat::RGBA;
    }else if (colorFormat == ColorFormat::RED){
        pixelFormat = SG_PIXELFORMAT_R8;
        bytesPerTexel = 1;
        uploadFormat = ColorFormat::RED;
    }else if (colorFormat == ColorFormat::RED16){
        // the rule of createTexture's unorm16ToFloat (GLES3, WebGL2: R16 does not filter)
        if (sg_query_pixelformat(SG_PIXELFORMAT_R16).filter){
            pixelFormat = SG_PIXELFORMAT_R16;
            bytesPerTexel = 2;
            uploadFormat = ColorFormat::RED16;
        }else if (sg_query_pixelformat(SG_PIXELFORMAT_R32F).filter){
            pixelFormat = SG_PIXELFORMAT_R32F;
            bytesPerTexel = 4;
            uploadFormat = ColorFormat::R32F;
        }else{
            pixelFormat = SG_PIXELFORMAT_R16F;
            bytesPerTexel = 2;
            uploadFormat = ColorFormat::R16F;
        }
    }else{
        Log::error("Dynamic map '%s': colour format %s is not a terrain map's (RED16, RED, RGBA)", label.c_str(), TextureData::getColorFormatName(colorFormat));
        return false;
    }

    sg_image_desc imageDesc = {};
    imageDesc.usage.stream_update = true;
    imageDesc.width = width;
    imageDesc.height = height;
    imageDesc.pixel_format = pixelFormat;
    imageDesc.label = label.c_str();

    sg_sampler_desc samplerDesc = {};
    samplerDesc.min_filter = SG_FILTER_LINEAR;
    samplerDesc.mag_filter = SG_FILTER_LINEAR;
    samplerDesc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    samplerDesc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;

    if (Engine::isAsyncThread()){
        image = SokolCmdQueue::add_command_make_image(imageDesc);
        sampler = SokolCmdQueue::add_command_make_sampler(samplerDesc);
    }else{
        image = sg_make_image(imageDesc);
        sampler = sg_make_sampler(samplerDesc);
    }
    trackImage(image, imageDesc, false, false);
    createTextureView(label.c_str());
    streamBytes = (size_t)width * (size_t)height * bytesPerTexel;
    dynamicMap = true;
    return image.id != SG_INVALID_ID && sampler.id != SG_INVALID_ID && view.id != SG_INVALID_ID;
}

bool SokolTexture::updateTexture(const void* data, size_t size){
    RenderBackendTime::Scope backendTime;   // (tests: the backend's share of a frame)
    if (image.id == SG_INVALID_ID || !data || size == 0)
        return false;
    if (dynamicMap){
        // a terrain map already updated this frame: the caller tries again next frame
        const uint64_t frame = SystemRender::getFrameCount();
        if (frame == lastMapUpdateFrame)
            return false;
        lastMapUpdateFrame = frame;
    }
    // sokol reads width * height * bpp bytes whatever the size says
    if (streamBytes != 0 && size != streamBytes){
        Log::error("Dynamic texture update of %zu bytes ignored: the texture takes %zu", size, streamBytes);
        return false;
    }

    sg_image_data imageData = {};
    imageData.mip_levels[0] = {data, size};
    if (Engine::isAsyncThread()){
        // budgeted when the render thread runs it (a refused one is counted and logged there)
        SokolCmdQueue::add_command_update_image(image, imageData);
        return true;
    }
    return SokolSystem::updateImage(image, imageData);
}

bool SokolTexture::createTextureCubeWithMips(
            const std::string& label, int width,
            ColorFormat colorFormat, int numMipmaps, void* data[], size_t size[],
            TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV){

    sg_pixel_format pixelFormat = _SG_PIXELFORMAT_DEFAULT;
    if (colorFormat == ColorFormat::RGBA){
        pixelFormat = SG_PIXELFORMAT_RGBA8;
    }else if (colorFormat == ColorFormat::RED){
        pixelFormat = SG_PIXELFORMAT_R8;
    }else{
        Log::error("Renders only support 8bpp and 32bpp textures");
        return false;
    }

    if (numMipmaps > SG_MAX_MIPMAPS){
        Log::error("Cubemap %s has more mipmaps than supported", label.c_str());
        return false;
    }

    sg_image_desc image_desc = {0};
    image_desc.type = SG_IMAGETYPE_CUBE;
    image_desc.width = width;
    image_desc.height = width;
    image_desc.pixel_format = pixelFormat;
    image_desc.num_slices = 6;
    image_desc.num_mipmaps = numMipmaps;
    image_desc.label = label.c_str();

    for (int level = 0; level < numMipmaps; level++){
        image_desc.data.mip_levels[level].ptr = data[level];
        image_desc.data.mip_levels[level].size = size[level];
    }

    sg_sampler_desc sampler_desc = {0};
    sampler_desc.min_filter = getFilter(minFilter);
    sampler_desc.mag_filter = getFilter(magFilter);
    sampler_desc.mipmap_filter = getFilterMipmap(minFilter);
    sampler_desc.wrap_u = getWrap(wrapU);
    sampler_desc.wrap_v = getWrap(wrapV);

    if (Engine::isAsyncThread()){
        image = SokolCmdQueue::add_command_make_image(image_desc);
        sampler = SokolCmdQueue::add_command_make_sampler(sampler_desc);
    }else{
        image = sg_make_image(image_desc);
        sampler = sg_make_sampler(sampler_desc);
    }
    trackImage(image, image_desc, false, false);

    createTextureView(label.c_str());

    if (image.id != SG_INVALID_ID && sampler.id != SG_INVALID_ID && view.id != SG_INVALID_ID)
        return true;

    return false;
}

void SokolTexture::createTextureView(const char* label){
    if (image.id == SG_INVALID_ID){
        view.id = SG_INVALID_ID;
        return;
    }

    sg_view_desc view_desc = {0};
    view_desc.texture.image = image;
    view_desc.label = label;

    if (Engine::isAsyncThread()){
        view = SokolCmdQueue::add_command_make_view(view_desc);
    }else{
        view = sg_make_view(view_desc);
    }
}

bool SokolTexture::createFramebufferTexture(
            TextureType type, bool depth, bool shadowMap, int width, int height,
            TextureFilter minFilter, TextureFilter magFilter, TextureWrap wrapU, TextureWrap wrapV,
            ColorFormat colorFormat, int sampleCount, bool depthStencil, bool resolve, int numMipmaps){
    sg_image_desc img_desc = {0};
    if (depth){
        img_desc.usage.depth_stencil_attachment = true;
    }else if (resolve){
        img_desc.usage.resolve_attachment = true;
    }else{
        img_desc.usage.color_attachment = true;
    }
    img_desc.type = getTextureType(type);
    img_desc.width = width;
    img_desc.height = height;
    img_desc.num_slices = (type == TextureType::TEXTURE_CUBE)? 6 : 1;

    sg_sampler_desc sampler_desc = {0};
    sampler_desc.min_filter = getFilter(minFilter);
    sampler_desc.mag_filter = getFilter(magFilter);
    sampler_desc.mipmap_filter = getFilterMipmap(minFilter);
    sampler_desc.wrap_u = getWrap(wrapU);
    sampler_desc.wrap_v = getWrap(wrapV);
    if (shadowMap){
        // Over sampling - https://learnopengl.com/Advanced-Lighting/Shadows/Shadow-Mapping
        sampler_desc.border_color = SG_BORDERCOLOR_OPAQUE_WHITE;
        if (depth){
            sampler_desc.compare = SG_COMPAREFUNC_LESS_EQUAL;
        }
    }

    //if not set Sokol gets default from sg_desc.context.sample_count
    img_desc.sample_count = (sampleCount > 1 && !resolve) ? sampleCount : 1;
    // levels a pass each renders into (multisampled images have one)
    img_desc.num_mipmaps = (!depth && img_desc.sample_count == 1 && numMipmaps > 1) ? numMipmaps : 1;

    if (depth){
        // DEPTH_STENCIL: GL/GLES3 DEPTH24_STENCIL8, D3D11 R24G8_TYPELESS (D24S8 to draw, depth
        // to sample), Metal Depth32Float_Stencil8, Vulkan D32_SFLOAT_S8_UINT; sokol samples
        // its depth aspect. DEPTH: a 32-bit float depth (shadow maps)
        img_desc.pixel_format = depthStencil ? SG_PIXELFORMAT_DEPTH_STENCIL : SG_PIXELFORMAT_DEPTH;
        img_desc.label = depthStencil ? "framebuffer-depth-stencil-image" : "framebuffer-depth-image";
    } else {
        if (colorFormat == ColorFormat::RED){
            img_desc.pixel_format = SG_PIXELFORMAT_R8;
        }else if (colorFormat == ColorFormat::SRGB8A8){
            img_desc.pixel_format = SG_PIXELFORMAT_SRGB8A8;
        }else if (colorFormat == ColorFormat::RGBA16F){
            img_desc.pixel_format = SG_PIXELFORMAT_RGBA16F;
        }else if (colorFormat == ColorFormat::RGBA32F){
            img_desc.pixel_format = SG_PIXELFORMAT_RGBA32F;
        }else if (colorFormat == ColorFormat::R32F){
            img_desc.pixel_format = SG_PIXELFORMAT_R32F;
        }else if (colorFormat == ColorFormat::RG16F){
            img_desc.pixel_format = SG_PIXELFORMAT_RG16F;
        }else if (colorFormat == ColorFormat::R16F){
            img_desc.pixel_format = SG_PIXELFORMAT_R16F;
        }else{
            img_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
        }
        img_desc.label = (img_desc.sample_count > 1) ? "framebuffer-msaa-color-image" : (resolve ? "framebuffer-resolve-image" : "framebuffer-color-image");
    }

    // multisampled attachments are only rendered to (an MSAA image is sampled through its
    // resolve image); every single-sampled attachment, depth-stencil included, gets a view
    // (sokol samples a depth-stencil image's depth aspect)
    const bool sampled = img_desc.sample_count == 1;

    if (Engine::isAsyncThread()){
        image = SokolCmdQueue::add_command_make_image(img_desc);
        if (sampled)
            sampler = SokolCmdQueue::add_command_make_sampler(sampler_desc);
    }else{
        image = sg_make_image(img_desc);
        if (sampled)
            sampler = sg_make_sampler(sampler_desc);
    }
    trackImage(image, img_desc, false, true);

    if (!sampled)
        return image.id != SG_INVALID_ID;

    createTextureView(img_desc.label);

    if (image.id != SG_INVALID_ID && sampler.id != SG_INVALID_ID && view.id != SG_INVALID_ID)
        return true;

    return false;
}

bool SokolTexture::createAliasView(const SokolTexture& source, const char* label){
    destroyTexture();
    if (source.image.id == SG_INVALID_ID)
        return false;

    sg_sampler_desc samplerDesc = {};
    samplerDesc.min_filter = SG_FILTER_NEAREST;
    samplerDesc.mag_filter = SG_FILTER_NEAREST;
    samplerDesc.mipmap_filter = SG_FILTER_NEAREST;
    samplerDesc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    samplerDesc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    samplerDesc.label = label;

    image = source.image;
    aliasImage = true;
    if (Engine::isAsyncThread()){
        sampler = SokolCmdQueue::add_command_make_sampler(samplerDesc);
    }else{
        sampler = sg_make_sampler(samplerDesc);
    }
    createTextureView(label);
    return sampler.id != SG_INVALID_ID && view.id != SG_INVALID_ID;
}

bool SokolTexture::createImageAliasView(uint32_t imageId, int mipLevel, const char* label){
    destroyTexture();
    const sg_image source = {imageId};
    if (imageId == SG_INVALID_ID || sg_query_image_state(source) != SG_RESOURCESTATE_VALID)
        return false;

    sg_sampler_desc samplerDesc = {};
    samplerDesc.min_filter = SG_FILTER_NEAREST;
    samplerDesc.mag_filter = SG_FILTER_NEAREST;
    samplerDesc.mipmap_filter = SG_FILTER_NEAREST;
    samplerDesc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    samplerDesc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    samplerDesc.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    samplerDesc.label = label;

    image = source;
    aliasImage = true;
    sampler = sg_make_sampler(samplerDesc);
    sg_view_desc viewDesc = {};
    viewDesc.texture.image = image;
    viewDesc.texture.mip_levels.base = mipLevel;
    viewDesc.texture.mip_levels.count = 1;
    viewDesc.label = label;
    view = sg_make_view(viewDesc);
    return sampler.id != SG_INVALID_ID && sg_query_view_state(view) == SG_RESOURCESTATE_VALID;
}

void SokolTexture::destroyTexture(){
    if (view.id != SG_INVALID_ID && sg_isvalid()){
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_destroy_view(view);
        }else{
            sg_destroy_view(view);
            SokolSystem::forgetAppliedBindings();
        }
    }
    if (image.id != SG_INVALID_ID && !aliasImage)
        untrackImage(image);
    if (image.id != SG_INVALID_ID && !aliasImage && sg_isvalid()){
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_destroy_image(image);
        }else{
            sg_destroy_image(image);
            SokolSystem::forgetAppliedBindings();
        }
    }
    // the data textures' sampler is let go (destroyed with its last holder), another destroyed
    const bool samplerShared = sharedSampler && static_cast<SharedDataSampler*>(sharedSampler.get())->sampler.id == sampler.id;
    sharedSampler.reset();
    if (!samplerShared && sampler.id != SG_INVALID_ID && sg_isvalid()){
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_destroy_sampler(sampler);
        }else{
            sg_destroy_sampler(sampler);
            SokolSystem::forgetAppliedBindings();
        }
    }

    image.id = SG_INVALID_ID;
    sampler.id = SG_INVALID_ID;
    view.id = SG_INVALID_ID;
    streamBytes = 0;
    aliasImage = false;
}

uint32_t SokolTexture::getGLHandler() const{
    if (image.id != SG_INVALID_ID && sg_isvalid()){
        sg_gl_image_info info = sg_gl_query_image_info(image);

        return info.tex[info.active_slot];
    }

    return 0;
}

const void* SokolTexture::getMetalHandler() const{
    if (image.id != SG_INVALID_ID && sg_isvalid()){
        sg_mtl_image_info info = sg_mtl_query_image_info(image);

        return info.tex[info.active_slot];
    }

    return nullptr;
}

const void* SokolTexture::getD3D11Handler() const{
    if (image.id != SG_INVALID_ID && sg_isvalid()){
        sg_d3d11_image_info info = sg_d3d11_query_image_info(image);

        return info.res;
    }

    return nullptr;
}

const void* SokolTexture::getVulkanHandler() const{
    if (view.id != SG_INVALID_ID && sg_isvalid()){
        return getVulkanView(view);
    }

    return nullptr;
}

const void* SokolTexture::getVulkanImageHandler() const{
    if (image.id != SG_INVALID_ID && sg_isvalid()){
        return getVulkanImage(image);
    }

    return nullptr;
}

uint32_t SokolTexture::getSamplingKey() const{
    if (image.id == SG_INVALID_ID || !sg_isvalid())
        return 0;
    const int width = sg_query_image_width(image), height = sg_query_image_height(image);
    const auto power = [](int side){ return side > 0 && (side & (side - 1)) == 0; };
    uint32_t key = (uint32_t)sg_query_image_pixelformat(image) | ((uint32_t)sg_query_image_type(image) << 8);
    key |= (power(width) ? 1u : 0u) << 12 | (power(height) ? 1u : 0u) << 13 | (sg_query_image_num_mipmaps(image) > 1 ? 1u : 0u) << 14;
    if (sampler.id != SG_INVALID_ID){
        const sg_sampler_desc s = sg_query_sampler_desc(sampler);
        key |= (uint32_t)s.min_filter << 16 | (uint32_t)s.mag_filter << 19 | (uint32_t)s.wrap_u << 22 | (uint32_t)s.wrap_v << 26;
    }
    return key;
}

int SokolTexture::getWidth() const{
    if (image.id == SG_INVALID_ID || !sg_isvalid())
        return 0;
    return sg_query_image_width(image);
}

int SokolTexture::getHeight() const{
    if (image.id == SG_INVALID_ID || !sg_isvalid())
        return 0;
    return sg_query_image_height(image);
}

uint32_t SokolTexture::getViewId() const{
    return view.id;
}

bool SokolTexture::isViewValid(uint32_t viewId){
    if (viewId == SG_INVALID_ID || !sg_isvalid()) return false;
    return sg_query_view_state({viewId}) == SG_RESOURCESTATE_VALID;
}

bool SokolTexture::isCreated(){
    if (image.id != SG_INVALID_ID && sg_isvalid()) {
        return sg_query_image_state(image) == SG_RESOURCESTATE_VALID;
    }

    return false;
}

sg_image SokolTexture::get(){
    return image;
}

sg_sampler SokolTexture::getSampler(){
    return sampler;
}

sg_view SokolTexture::getView(){
    return view;
}
