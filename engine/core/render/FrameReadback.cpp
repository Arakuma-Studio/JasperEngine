// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FrameReadback.h"

#include "System.h"
#include "texture/Framebuffer.h"
#include "texture/TexturePrepare.h"

#include <cstring>

#if defined(SOKOL_VULKAN)
    #include <vulkan/vulkan.h>
#elif defined(SOKOL_D3D11)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
    #include <d3d11.h>
#elif defined(SOKOL_GLCORE)
    #if defined(__APPLE__)
        #ifndef GL_SILENCE_DEPRECATION
        #define GL_SILENCE_DEPRECATION
        #endif
        #include <OpenGL/gl3.h>
    #elif defined(_WIN32)
        #ifndef NOMINMAX
        #define NOMINMAX
        #endif
        #include <windows.h>
        #include <GL/gl.h>
    #else
        #define GL_GLEXT_PROTOTYPES
        #include <GL/gl.h>
    #endif
#elif defined(SOKOL_GLES3)
    #if defined(__APPLE__)
        #ifndef GLES_SILENCE_DEPRECATION
        #define GLES_SILENCE_DEPRECATION
        #endif
        #include <OpenGLES/ES3/gl.h>
    #else
        #include <GLES3/gl3.h>
    #endif
#endif


#if defined(SOKOL_GLCORE)
    // GL_HALF_FLOAT is GL 3.0: older headers (Windows' GL/gl.h) do not name it
    #ifdef GL_HALF_FLOAT
        #define READBACK_GL_HALF_FLOAT GL_HALF_FLOAT
    #else
        #define READBACK_GL_HALF_FLOAT 0x140B
    #endif
#endif

#if defined(SOKOL_GLES3)
namespace {
    // float colour buffers are readable on ES 3.0 / WebGL2 only with EXT_color_buffer_float
    bool hasColorBufferFloat() {
        GLint count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count; i++) {
            const char* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
            if (name && (std::strcmp(name, "GL_EXT_color_buffer_float") == 0 || std::strcmp(name, "EXT_color_buffer_float") == 0)) return true;
        }
        return false;
    }
}
#endif

using namespace jasperengine;

namespace {

    FrameReadback::PlatformReader platformReader = nullptr;

    void setError(std::string* error, const char* message) {
        if (error) *error = message;
    }

#if defined(SOKOL_VULKAN)
    // a staging buffer copy of the image, which sokol keeps in SHADER_READ_ONLY_OPTIMAL between passes
    bool readVulkanImage(TextureRender& texture, int width, int height, int bytesPerPixel, std::vector<uint8_t>& out, std::string* error) {
        sg_environment environment = System::instance().getSokolEnvironment();
        VkPhysicalDevice physicalDevice = reinterpret_cast<VkPhysicalDevice>(const_cast<void*>(environment.vulkan.physical_device));
        VkDevice device = reinterpret_cast<VkDevice>(const_cast<void*>(environment.vulkan.device));
        VkQueue queue = reinterpret_cast<VkQueue>(const_cast<void*>(environment.vulkan.queue));
        VkImage image = reinterpret_cast<VkImage>(const_cast<void*>(texture.getVulkanImageHandler()));
        const VkDeviceSize dataSize = static_cast<VkDeviceSize>(width) * height * bytesPerPixel;

        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        auto cleanup = [&]() {
            if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
            if (stagingBuffer != VK_NULL_HANDLE) vkDestroyBuffer(device, stagingBuffer, nullptr);
            if (stagingMemory != VK_NULL_HANDLE) vkFreeMemory(device, stagingMemory, nullptr);
        };
        auto fail = [&](const char* message) {
            cleanup();
            setError(error, message);
            return false;
        };

        if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE || image == VK_NULL_HANDLE)
            return fail("no Vulkan device or image to read");
        if (vkQueueWaitIdle(queue) != VK_SUCCESS)
            return fail("the Vulkan queue did not go idle");

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = dataSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS)
            return fail("cannot create a Vulkan staging buffer");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, stagingBuffer, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        uint32_t memoryType = UINT32_MAX;
        const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & wanted) == wanted) {
                memoryType = i;
                break;
            }
        }
        if (memoryType == UINT32_MAX)
            return fail("no host-visible Vulkan memory");

        VkMemoryAllocateInfo memoryInfo{};
        memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memoryInfo.allocationSize = requirements.size;
        memoryInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(device, &memoryInfo, nullptr, &stagingMemory) != VK_SUCCESS ||
            vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0) != VK_SUCCESS)
            return fail("cannot allocate Vulkan readback memory");

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = environment.vulkan.queue_family_index;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS)
            return fail("cannot create a Vulkan command pool");

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo commandInfo{};
        commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandInfo.commandPool = commandPool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &commandInfo, &commandBuffer) != VK_SUCCESS)
            return fail("cannot allocate a Vulkan command buffer");

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
            return fail("cannot begin a Vulkan command buffer");

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
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &copy);

        VkImageMemoryBarrier toTexture = toTransfer;
        toTexture.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTexture.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toTexture.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTexture.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toTexture);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS || vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS ||
            vkQueueWaitIdle(queue) != VK_SUCCESS)
            return fail("the Vulkan readback copy failed");

        void* mapped = nullptr;
        if (vkMapMemory(device, stagingMemory, 0, dataSize, 0, &mapped) != VK_SUCCESS)
            return fail("cannot map Vulkan readback memory");
        out.resize(static_cast<size_t>(dataSize));
        std::memcpy(out.data(), mapped, static_cast<size_t>(dataSize));
        vkUnmapMemory(device, stagingMemory);
        cleanup();
        return true;
    }
#endif

#if defined(SOKOL_D3D11)
    bool readD3D11Texture(TextureRender& texture, int width, int height, int bytesPerPixel, std::vector<uint8_t>& out, std::string* error) {
        ID3D11Resource* resource = reinterpret_cast<ID3D11Resource*>(const_cast<void*>(texture.getD3D11Handler()));
        ID3D11Device* device = reinterpret_cast<ID3D11Device*>(const_cast<void*>(sg_d3d11_device()));
        ID3D11DeviceContext* context = reinterpret_cast<ID3D11DeviceContext*>(const_cast<void*>(sg_d3d11_device_context()));
        ID3D11Texture2D* source = nullptr;
        if (!resource || !device || !context || FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source))) || !source) {
            setError(error, "no D3D11 device or texture to read");
            return false;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        source->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.SampleDesc.Quality = 0;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)) || !staging) {
            source->Release();
            setError(error, "cannot create a D3D11 staging texture");
            return false;
        }
        context->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, nullptr);
        source->Release();
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            staging->Release();
            setError(error, "cannot map the D3D11 staging texture");
            return false;
        }
        const size_t row = static_cast<size_t>(width) * bytesPerPixel;
        out.resize(row * height);
        for (int y = 0; y < height; ++y)
            std::memcpy(out.data() + row * y, static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(mapped.RowPitch) * y, row);
        context->Unmap(staging, 0);
        staging->Release();
        return true;
    }
#endif

}

void FrameReadback::setPlatformReader(PlatformReader reader) {
    platformReader = reader;
}

bool FrameReadback::finishGpu() {
#if defined(SOKOL_VULKAN)
    sg_environment environment = System::instance().getSokolEnvironment();
    VkQueue queue = reinterpret_cast<VkQueue>(const_cast<void*>(environment.vulkan.queue));
    return queue != VK_NULL_HANDLE && vkQueueWaitIdle(queue) == VK_SUCCESS;
#elif defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    glFinish();
    return true;
#else
    return true;
#endif
}

#if (defined(SOKOL_GLCORE) && !defined(_WIN32)) || defined(SOKOL_GLES3)
namespace {
    GLsync gpuFence = nullptr;
}

void FrameReadback::fenceGpu() {
    if (gpuFence) glDeleteSync(gpuFence);
    gpuFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
}

bool FrameReadback::gpuFencePassed() {
    if (!gpuFence) return true;
    const GLenum state = glClientWaitSync(gpuFence, 0, 0);
    if (state == GL_TIMEOUT_EXPIRED) return false;
    // signalled, or failed (a lost context): no fence to wait for
    glDeleteSync(gpuFence);
    gpuFence = nullptr;
    return true;
}
#else
void FrameReadback::fenceGpu() {
}

bool FrameReadback::gpuFencePassed() {
    return true;
}
#endif

bool FrameReadback::readTexture(TextureRender& texture, int width, int height, int bytesPerPixel,
                                std::vector<uint8_t>& pixels, std::string* error) {
    pixels.clear();
    if (width <= 0 || height <= 0 || (bytesPerPixel != 4 && bytesPerPixel != 1 && bytesPerPixel != 8 && bytesPerPixel != 16)) {
        setError(error, "bad readback size");
        return false;
    }
    if (platformReader)
        return platformReader(texture, width, height, bytesPerPixel, pixels, error);

#if defined(SOKOL_VULKAN)
    return readVulkanImage(texture, width, height, bytesPerPixel, pixels, error);
#elif defined(SOKOL_D3D11)
    return readD3D11Texture(texture, width, height, bytesPerPixel, pixels, error);
#elif defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    const GLuint handle = texture.getGLHandler();
    if (handle == 0) {
        setError(error, "no GL texture to read");
        return false;
    }
    // errors of earlier GL calls are not this readback's
    while (glGetError() != GL_NO_ERROR) {}
    // Everything that renders into the texture finished first: GL makes the read wait for it,
    // Mesa's llvmpipe did not always (16x16 blocks not yet rasterized)
    glFinish();
    pixels.resize(static_cast<size_t>(width) * height * bytesPerPixel);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
#if defined(SOKOL_GLCORE)
    // 8 bytes: RGBA16F as IEEE halves; 16 bytes: RGBA32F
    const GLenum format = bytesPerPixel == 1 ? GL_RED : GL_RGBA;
    const GLenum type = bytesPerPixel == 8 ? READBACK_GL_HALF_FLOAT : bytesPerPixel == 16 ? GL_FLOAT : GL_UNSIGNED_BYTE;
    glBindTexture(GL_TEXTURE_2D, handle);
    glGetTexImage(GL_TEXTURE_2D, 0, format, type, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
#else
    // OpenGL ES and WebGL2 have no glGetTexImage: a framebuffer of the texture, read with
    // glReadPixels (RGBA8 for both sizes, R8 has no guaranteed read format)
    GLint previousFramebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    GLuint readFramebuffer = 0;
    glGenFramebuffers(1, &readFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, readFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, handle, 0);
    const bool floatRead = bytesPerPixel == 8 || bytesPerPixel == 16;
    if (floatRead && !hasColorBufferFloat()) {
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
        glDeleteFramebuffers(1, &readFramebuffer);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        sg_reset_state_cache();
        pixels.clear();
        setError(error, "float readback needs EXT_color_buffer_float");
        return false;
    }
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        if (floatRead) {
            // ES reads float colour buffers as RGBA / FLOAT only: 16 bytes, halved on the CPU
            std::vector<float> rgba(static_cast<size_t>(width) * height * 4);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, rgba.data());
            if (bytesPerPixel == 16) {
                std::memcpy(pixels.data(), rgba.data(), pixels.size());
            } else {
                uint16_t* halves = reinterpret_cast<uint16_t*>(pixels.data());
                for (size_t i = 0; i < rgba.size(); i++) halves[i] = floatToHalf(rgba[i]);
            }
        } else if (bytesPerPixel == 4) {
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        } else {
            std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            for (size_t i = 0; i < pixels.size(); i++) pixels[i] = rgba[i * 4];
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
    glDeleteFramebuffers(1, &readFramebuffer);
    if (!complete) {
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        sg_reset_state_cache();
        pixels.clear();
        setError(error, "the texture cannot be attached to a framebuffer for reading");
        return false;
    }
#endif
    const bool ok = glGetError() == GL_NO_ERROR;
    // sokol caches GL bindings and pixel store state; it must not trust them after this
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    sg_reset_state_cache();
    if (!ok) {
        pixels.clear();
        setError(error, "the GL readback failed");
    }
    return ok;
#else
    setError(error, "no readback for this graphics backend");
    return false;
#endif
}

bool FrameReadback::readFramebuffer(Framebuffer& framebuffer, std::vector<uint8_t>& rgba, std::string* error) {
    if (!framebuffer.isCreated()) {
        setError(error, "the framebuffer was not created");
        return false;
    }
    return readTexture(framebuffer.getRender().getColorTexture(), static_cast<int>(framebuffer.getWidth()),
                       static_cast<int>(framebuffer.getHeight()), 4, rgba, error);
}
