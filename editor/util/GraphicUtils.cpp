// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GraphicUtils.h"
#include "util/PngWriter.h"

#include "Out.h"
#include "System.h"
#include "stb_image_write.h"
#include "stb_image_resize2.h"
#include "util/STBText.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <cstring>
#include <future>
#include <vector>
#include <thread>
#include <utility>

#if defined(SOKOL_METAL)
    #include <TargetConditionals.h>
    #import <Metal/Metal.h>
#endif

#if defined(SOKOL_D3D11)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <d3d11.h>
    #include <wrl/client.h>
    using Microsoft::WRL::ComPtr;
#endif

#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    #ifdef __APPLE__
        #include <OpenGL/gl.h>
    #elif defined(_WIN32)
        #include <windows.h>
        #include <GL/gl.h>
    #else
        #define GL_GLEXT_PROTOTYPES
        #include <GL/gl.h>
    #endif
#endif

#if defined(SOKOL_VULKAN)
    #include <vulkan/vulkan.h>
#endif

using namespace jasperengine;

Vector2 editor::GraphicUtils::getRectTransformCenter(Scene* scene, Entity entity, const RectTransformComponent& layout) {
    Vector2 center = Vector2(0, 0);
    Signature signature = scene->getSignature(entity);
    if (signature.test(scene->getComponentId<TextComponent>())) {
        TextComponent& text = scene->getComponent<TextComponent>(entity);
        if (text.pivotBaseline) {
            center.y = text.stbtext->getAscent();
        }
        if (text.pivotCentered) {
            center.x = layout.width / 2.0f;
        }
    }
    return center;
}

// The framebuffer's color texture, width * height RGBA8 rows as the backend stores them
// (pixels from new[], owned by the caller when needDelete). Synchronous GPU readback,
// on the main thread.
static bool readRenderPixels(FramebufferRender& render, unsigned int width, unsigned int height, uint8_t*& pixels, bool& needDelete) {
    pixels = nullptr;
    needDelete = false;

    #if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
        // OpenGL
        pixels = new uint8_t[width * height * 4];
        needDelete = true;

        // every command that renders into the texture finished first: GL makes glGetTexImage
        // wait for them, Mesa's llvmpipe (the headless editor's driver) did not always (p4-gaps)
        glFinish();
        glBindTexture(GL_TEXTURE_2D, render.getColorTexture().getGLHandler());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glBindTexture(GL_TEXTURE_2D, 0);
    #endif

    #if defined(SOKOL_VULKAN)
        sg_environment environment = System::instance().getSokolEnvironment();
        VkPhysicalDevice physicalDevice = reinterpret_cast<VkPhysicalDevice>(
            const_cast<void*>(environment.vulkan.physical_device));
        VkDevice device = reinterpret_cast<VkDevice>(
            const_cast<void*>(environment.vulkan.device));
        VkQueue queue = reinterpret_cast<VkQueue>(
            const_cast<void*>(environment.vulkan.queue));
        VkImage image = reinterpret_cast<VkImage>(const_cast<void*>(
            render.getColorTexture().getVulkanImageHandler()));
        const VkDeviceSize dataSize = static_cast<VkDeviceSize>(width) * height * 4;
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        VkCommandPool commandPool = VK_NULL_HANDLE;

        auto cleanupVulkan = [&]() {
            if (commandPool != VK_NULL_HANDLE)
                vkDestroyCommandPool(device, commandPool, nullptr);
            if (stagingBuffer != VK_NULL_HANDLE)
                vkDestroyBuffer(device, stagingBuffer, nullptr);
            if (stagingMemory != VK_NULL_HANDLE)
                vkFreeMemory(device, stagingMemory, nullptr);
        };

        if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
            queue == VK_NULL_HANDLE || image == VK_NULL_HANDLE ||
            vkQueueWaitIdle(queue) != VK_SUCCESS) {
            editor::Out::error("Engine failure: Vulkan framebuffer readback is unavailable");
            return false;
        }

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = dataSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS) {
            editor::Out::error("Engine failure: Could not create Vulkan readback buffer");
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, stagingBuffer, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        uint32_t memoryType = UINT32_MAX;
        const VkMemoryPropertyFlags wanted =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memoryProperties.memoryTypes[i].propertyFlags & wanted) == wanted) {
                memoryType = i;
                break;
            }
        }
        if (memoryType == UINT32_MAX) {
            cleanupVulkan();
            editor::Out::error("Engine failure: No host-visible Vulkan readback memory");
            return false;
        }

        VkMemoryAllocateInfo memoryInfo{};
        memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memoryInfo.allocationSize = requirements.size;
        memoryInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(device, &memoryInfo, nullptr, &stagingMemory) != VK_SUCCESS ||
            vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0) != VK_SUCCESS) {
            cleanupVulkan();
            editor::Out::error("Engine failure: Could not allocate Vulkan readback memory");
            return false;
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = environment.vulkan.queue_family_index;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            cleanupVulkan();
            editor::Out::error("Engine failure: Could not create Vulkan readback command pool");
            return false;
        }

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo commandInfo{};
        commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandInfo.commandPool = commandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &commandInfo, &commandBuffer) != VK_SUCCESS) {
            cleanupVulkan();
            editor::Out::error("Engine failure: Could not allocate Vulkan readback command buffer");
            return false;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            cleanupVulkan();
            editor::Out::error("Engine failure: Could not begin Vulkan readback command buffer");
            return false;
        }

        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               stagingBuffer, 1, &copy);

        VkImageMemoryBarrier toTexture = toTransfer;
        toTexture.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTexture.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toTexture.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTexture.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTexture);
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            cleanupVulkan();
            editor::Out::error("Engine failure: Could not end Vulkan readback command buffer");
            return false;
        }

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        if (vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS ||
            vkQueueWaitIdle(queue) != VK_SUCCESS) {
            cleanupVulkan();
            editor::Out::error("Engine failure: Vulkan framebuffer readback failed");
            return false;
        }

        void* mapped = nullptr;
        if (vkMapMemory(device, stagingMemory, 0, dataSize, 0, &mapped) != VK_SUCCESS) {
            cleanupVulkan();
            editor::Out::error("Engine failure: Could not map Vulkan readback memory");
            return false;
        }
        pixels = new uint8_t[static_cast<size_t>(dataSize)];
        std::memcpy(pixels, mapped, static_cast<size_t>(dataSize));
        vkUnmapMemory(device, stagingMemory);
        cleanupVulkan();
        needDelete = true;
    #endif

    #if defined(SOKOL_METAL)
        // Metal
        auto metalTexPtr = render.getColorTexture().getMetalHandler();
        id<MTLTexture> tex = (__bridge id<MTLTexture>)metalTexPtr;
        id<MTLDevice> device = [tex device];
        NSUInteger bytesPerRow = width * 4;
        NSUInteger dataSize = bytesPerRow * height;
        id<MTLBuffer> buffer = [device newBufferWithLength:dataSize options:MTLResourceStorageModeShared];

        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];

        [blit copyFromTexture:tex
                 sourceSlice:0
                 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(width, height, 1)
                   toBuffer:buffer
           destinationOffset:0
      destinationBytesPerRow:bytesPerRow
    destinationBytesPerImage:dataSize];
        [blit endEncoding];
        [cmdBuf commit];
        [cmdBuf waitUntilCompleted];

        // The Metal buffer is released when this function returns, while PNG
        // encoding happens on a detached thread. Give that thread owned bytes.
        pixels = new uint8_t[dataSize];
        memcpy(pixels, [buffer contents], dataSize);
        needDelete = true;
    #endif

    #if defined(SOKOL_D3D11)
        // D3D11
        auto rtvPtr = render.getD3D11HandlerColorRTV();
        ID3D11RenderTargetView* rtv = reinterpret_cast<ID3D11RenderTargetView*>(const_cast<void*>(rtvPtr));

        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        ID3D11Resource* renderTargetResource = nullptr;
        D3D11_TEXTURE2D_DESC desc = {};
        ID3D11Texture2D* staging = nullptr;
        pixels = new uint8_t[width * height * 4];
        needDelete = true;

        rtv->GetDevice(&device);
        device->GetImmediateContext(&context);
        rtv->GetResource(&renderTargetResource);
        ID3D11Texture2D* srcTex = nullptr;
        renderTargetResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&srcTex);
        srcTex->GetDesc(&desc);

        // Create staging texture
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        device->CreateTexture2D(&stagingDesc, nullptr, &staging);

        // Copy to staging
        context->CopyResource(staging, srcTex);

        // Map and read
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);

        for (int y = 0; y < height; ++y) {
            memcpy(&pixels[y * width * 4],
                (uint8_t*)mapped.pData + y * mapped.RowPitch,
                width * 4);
        }

        // Cleanup
        context->Unmap(staging, 0);
        staging->Release();
        srcTex->Release();
        renderTargetResource->Release();
        context->Release();
        device->Release();
    #endif

    return true;
}

static bool readFramebufferPixels(Framebuffer* framebuffer, uint8_t*& pixels, bool& needDelete) {
    return readRenderPixels(framebuffer->getRender(), framebuffer->getWidth(), framebuffer->getHeight(), pixels, needDelete);
}

bool editor::GraphicUtils::readRenderPixels(FramebufferRender* render, int width, int height, std::vector<uint8_t>& rgba) {
    rgba.clear();
    if (!render || !render->isCreated() || width <= 0 || height <= 0)
        return false;
    uint8_t* pixels = nullptr;
    bool needDelete = false;
    const bool read = ::readRenderPixels(*render, (unsigned int)width, (unsigned int)height, pixels, needDelete) && pixels;
    if (read)
        rgba.assign(pixels, pixels + (size_t)width * height * 4);
    if (needDelete)
        delete[] pixels;
    return read;
}

bool editor::GraphicUtils::saveFramebufferImage(Framebuffer* framebuffer, fs::path path, bool flipY, std::function<void()> onComplete) {
    uint8_t* pixels = nullptr;
    bool needDelete = false;

    unsigned int width = framebuffer->getWidth();
    unsigned int height = framebuffer->getHeight();

    if (!readFramebufferPixels(framebuffer, pixels, needDelete)) {
        return false;
    }

    try {
        // must-run (the pixels are freed and onComplete called there): a detached Background job
        jobs::enqueueDetached([pixels, needDelete, width, height, path, flipY, onComplete]() {
            // Do not use stb's process-global flip flag from concurrent writers.
            // The readback buffer is owned here, so flip its rows directly.
            if (flipY && pixels) {
                const size_t rowBytes = static_cast<size_t>(width) * 4;
                for (unsigned int y = 0; y < height / 2; ++y) {
                    uint8_t* top = pixels + static_cast<size_t>(y) * rowBytes;
                    uint8_t* bottom = pixels + static_cast<size_t>(height - 1 - y) * rowBytes;
                    for (size_t x = 0; x < rowBytes; ++x) {
                        std::swap(top[x], bottom[x]);
                    }
                }
            }
            writePng(path, width, height, 4, pixels, width * 4, io::Durability::Cache);

            if (needDelete && pixels) {
                delete[] pixels;
            }

            if (onComplete) {
                onComplete();
            }
        }, jobs::Lane::Background, "save-framebuffer-png");
    } catch (...) {
        if (needDelete && pixels) {
            delete[] pixels;
        }
        return false;
    }
    return true;
}

bool editor::GraphicUtils::readFramebufferRGBA(Framebuffer* framebuffer, std::vector<uint8_t>& out) {
    out.clear();
    if (!framebuffer || !framebuffer->isCreated() || framebuffer->getWidth() == 0 || framebuffer->getHeight() == 0) {
        return false;
    }

    uint8_t* pixels = nullptr;
    bool needDelete = false;
    if (!readFramebufferPixels(framebuffer, pixels, needDelete) || !pixels) {
        if (needDelete) delete[] pixels;
        return false;
    }

    const size_t size = static_cast<size_t>(framebuffer->getWidth()) * framebuffer->getHeight() * 4;
    out.assign(pixels, pixels + size);
    if (needDelete) delete[] pixels;
    return true;
}

bool editor::GraphicUtils::encodeFramebufferPng(Framebuffer* framebuffer, std::vector<uint8_t>& out, bool flipY, int maxWidth, int* outWidth, int* outHeight) {
    out.clear();
    if (!framebuffer || !framebuffer->isCreated() || framebuffer->getWidth() == 0 || framebuffer->getHeight() == 0) {
        return false;
    }

    uint8_t* pixels = nullptr;
    bool needDelete = false;
    if (!readFramebufferPixels(framebuffer, pixels, needDelete) || !pixels) {
        if (needDelete) delete[] pixels;
        return false;
    }

    const int width = static_cast<int>(framebuffer->getWidth());
    const int height = static_cast<int>(framebuffer->getHeight());

    // RGB, rows top first: alpha is dropped (the views show the color as opaque) and the
    // rows are flipped when the backend stored them bottom first
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = pixels + static_cast<size_t>(flipY ? height - 1 - y : y) * width * 4;
        uint8_t* dst = rgb.data() + static_cast<size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
    if (needDelete) delete[] pixels;

    int finalWidth = width;
    int finalHeight = height;
    if (maxWidth > 0 && width > maxWidth) {
        finalWidth = maxWidth;
        finalHeight = std::max(1, static_cast<int>(static_cast<long long>(height) * maxWidth / width));
        std::vector<uint8_t> scaled(static_cast<size_t>(finalWidth) * finalHeight * 3);
        if (!stbir_resize_uint8_srgb(rgb.data(), width, height, 0, scaled.data(), finalWidth, finalHeight, 0, STBIR_RGB)) {
            return false;
        }
        rgb.swap(scaled);
    }

    auto append = [](void* context, void* data, int size) {
        auto* bytes = static_cast<std::vector<uint8_t>*>(context);
        const uint8_t* begin = static_cast<const uint8_t*>(data);
        bytes->insert(bytes->end(), begin, begin + size);
    };
    if (!stbi_write_png_to_func(append, &out, finalWidth, finalHeight, 3, rgb.data(), finalWidth * 3)) {
        out.clear();
        return false;
    }

    if (outWidth) *outWidth = finalWidth;
    if (outHeight) *outHeight = finalHeight;
    return true;
}

bool editor::GraphicUtils::encodeFramebufferThumbnailPng(Framebuffer* framebuffer, int width, int height, std::vector<uint8_t>& out) {
    out.clear();
    if (width <= 0 || height <= 0 || !framebuffer || !framebuffer->isCreated() || framebuffer->getWidth() == 0 ||
            framebuffer->getHeight() == 0) {
        return false;
    }
    uint8_t* pixels = nullptr;
    bool needDelete = false;
    if (!readFramebufferPixels(framebuffer, pixels, needDelete) || !pixels) {
        if (needDelete) delete[] pixels;
        return false;
    }
    const int sourceWidth = static_cast<int>(framebuffer->getWidth());
    const int sourceHeight = static_cast<int>(framebuffer->getHeight());
    // the largest centred rectangle of the thumbnail's aspect
    int cropWidth = sourceWidth;
    int cropHeight = static_cast<int>(static_cast<long long>(sourceWidth) * height / width);
    if (cropHeight > sourceHeight) {
        cropHeight = sourceHeight;
        cropWidth = std::max(1, static_cast<int>(static_cast<long long>(sourceHeight) * width / height));
    }
    cropHeight = std::max(1, cropHeight);
    const int x0 = (sourceWidth - cropWidth) / 2;
    const int y0 = (sourceHeight - cropHeight) / 2;
    std::vector<uint8_t> rgb(static_cast<size_t>(cropWidth) * cropHeight * 3);
    for (int y = 0; y < cropHeight; ++y) {
        const uint8_t* src = pixels + (static_cast<size_t>(y0 + y) * sourceWidth + x0) * 4;
        uint8_t* dst = rgb.data() + static_cast<size_t>(y) * cropWidth * 3;
        for (int x = 0; x < cropWidth; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
    if (needDelete) delete[] pixels;

    std::vector<uint8_t> scaled(static_cast<size_t>(width) * height * 3);
    if (!stbir_resize_uint8_srgb(rgb.data(), cropWidth, cropHeight, 0, scaled.data(), width, height, 0, STBIR_RGB)) {
        return false;
    }
    auto append = [](void* context, void* data, int size) {
        auto* bytes = static_cast<std::vector<uint8_t>*>(context);
        const uint8_t* begin = static_cast<const uint8_t*>(data);
        bytes->insert(bytes->end(), begin, begin + size);
    };
    if (!stbi_write_png_to_func(append, &out, width, height, 3, scaled.data(), width * 3)) {
        out.clear();
        return false;
    }
    return true;
}
