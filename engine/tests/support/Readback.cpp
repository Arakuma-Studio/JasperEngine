// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Readback.h"

#include "System.h"
#include "render/FrameReadback.h"
#include "texture/TexturePrepare.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(SOKOL_VULKAN)
    #include <vulkan/vulkan.h>
#else
    #define GL_GLEXT_PROTOTYPES
    #include <GL/gl.h>
#endif

using namespace jasperengine;

#if defined(SOKOL_VULKAN)
// as editor/util/GraphicUtils.cpp readFramebufferPixels: a staging buffer copy of the image,
// which sokol keeps in SHADER_READ_ONLY_OPTIMAL between passes
static bool readVulkanImage(TextureRender& texture, int width, int height, int bytesPerPixel, std::vector<uint8_t>& rgba){
    sg_environment environment = System::instance().getSokolEnvironment();
    VkPhysicalDevice physicalDevice = reinterpret_cast<VkPhysicalDevice>(const_cast<void*>(environment.vulkan.physical_device));
    VkDevice device = reinterpret_cast<VkDevice>(const_cast<void*>(environment.vulkan.device));
    VkQueue queue = reinterpret_cast<VkQueue>(const_cast<void*>(environment.vulkan.queue));
    VkImage image = reinterpret_cast<VkImage>(const_cast<void*>(texture.getVulkanImageHandler()));
    const VkDeviceSize dataSize = static_cast<VkDeviceSize>(width) * height * bytesPerPixel;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;

    auto cleanup = [&](){
        if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
        if (stagingBuffer != VK_NULL_HANDLE) vkDestroyBuffer(device, stagingBuffer, nullptr);
        if (stagingMemory != VK_NULL_HANDLE) vkFreeMemory(device, stagingMemory, nullptr);
    };

    if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE || image == VK_NULL_HANDLE){
        std::fprintf(stderr, "readTexture: no Vulkan device or image (JASPERENGINE_TEXTURE_READBACK missing?)\n");
        return false;
    }
    if (vkQueueWaitIdle(queue) != VK_SUCCESS)
        return false;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = dataSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, stagingBuffer, &requirements);
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    uint32_t memoryType = UINT32_MAX;
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i){
        if ((requirements.memoryTypeBits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & wanted) == wanted){
            memoryType = i;
            break;
        }
    }
    if (memoryType == UINT32_MAX){
        cleanup();
        return false;
    }

    VkMemoryAllocateInfo memoryInfo{};
    memoryInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryInfo.allocationSize = requirements.size;
    memoryInfo.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device, &memoryInfo, nullptr, &stagingMemory) != VK_SUCCESS ||
        vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0) != VK_SUCCESS){
        cleanup();
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = environment.vulkan.queue_family_index;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS){
        cleanup();
        return false;
    }

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &commandInfo, &commandBuffer) != VK_SUCCESS){
        cleanup();
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS){
        cleanup();
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
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS ||
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS ||
        vkQueueWaitIdle(queue) != VK_SUCCESS){
        cleanup();
        return false;
    }

    void* mapped = nullptr;
    if (vkMapMemory(device, stagingMemory, 0, dataSize, 0, &mapped) != VK_SUCCESS){
        cleanup();
        return false;
    }
    rgba.resize(static_cast<size_t>(dataSize));
    std::memcpy(rgba.data(), mapped, static_cast<size_t>(dataSize));
    vkUnmapMemory(device, stagingMemory);
    cleanup();
    return true;
}
#endif

#if defined(SOKOL_GLES3)
// OpenGL ES and WebGL2 have no glGetTexImage: the texture as a framebuffer, read with the
// glReadPixels pair ES allows for its kind (RGBA / UNSIGNED_BYTE for 8-bit normalized, RGBA /
// UNSIGNED_SHORT for EXT_texture_norm16, RGBA / FLOAT for float with EXT_color_buffer_float),
// then written in the layout asked for, as glGetTexImage converts on GL: `channels` components
// of `type` a texel
static bool readGlesTexture(GLuint handle, int width, int height, int channels, GLenum type, std::vector<uint8_t>& bytes){
    GLint previousFramebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    GLuint readFramebuffer = 0;
    glGenFramebuffers(1, &readFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, readFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, handle, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    GLint componentType = GL_UNSIGNED_NORMALIZED;
    GLint redBits = 8;
    if (ok){
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &componentType);
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &redBits);
    }
    const size_t texels = static_cast<size_t>(width) * height;
    std::vector<float> rgba(texels * 4);
    if (ok && componentType == GL_FLOAT){
        glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, rgba.data());
    }else if (ok && redBits > 8){
        std::vector<uint16_t> read(texels * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_SHORT, read.data());
        for (size_t i = 0; i < read.size(); i++) rgba[i] = read[i] / 65535.0f;
    }else if (ok){
        std::vector<uint8_t> read(texels * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, read.data());
        for (size_t i = 0; i < read.size(); i++) rgba[i] = read[i] / 255.0f;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
    glDeleteFramebuffers(1, &readFramebuffer);
    ok = ok && glGetError() == GL_NO_ERROR;
    if (!ok)
        return false;

    auto unorm = [](float v, float scale){ return std::lround(std::min(1.0f, std::max(0.0f, v)) * scale); };
    const size_t componentBytes = type == GL_FLOAT ? 4 : (type == GL_UNSIGNED_BYTE ? 1 : 2);
    bytes.resize(texels * channels * componentBytes);
    for (size_t t = 0; t < texels; t++){
        for (int c = 0; c < channels; c++){
            const float v = rgba[t * 4 + c];
            uint8_t* out = bytes.data() + (t * channels + c) * componentBytes;
            if (type == GL_FLOAT){
                std::memcpy(out, &v, 4);
            }else if (type == GL_HALF_FLOAT){
                const uint16_t h = floatToHalf(v);
                std::memcpy(out, &h, 2);
            }else if (type == GL_UNSIGNED_SHORT){
                const uint16_t u = static_cast<uint16_t>(unorm(v, 65535.0f));
                std::memcpy(out, &u, 2);
            }else{
                *out = static_cast<uint8_t>(unorm(v, 255.0f));
            }
        }
    }
    return true;
}
#endif

bool jasperengine::test::finishGpu(){
    return FrameReadback::finishGpu();
}

bool jasperengine::test::readTexture(TextureRender& texture, int width, int height, std::vector<uint8_t>& rgba){
    return readTexturePixels(texture, width, height, 4, rgba);
}

bool jasperengine::test::readTextureRaw(TextureRender& texture, int width, int height, ColorFormat format, std::vector<uint8_t>& bytes){
    bytes.clear();
    sg_pixel_format pixelFormat;
    size_t bytesPerTexel = 0;
    if (width <= 0 || height <= 0 || !SokolTexture::getDataPixelFormat(format, pixelFormat, bytesPerTexel))
        return false;

#if defined(SOKOL_VULKAN)
    // the copy keeps the image's own layout
    return readVulkanImage(texture, width, height, static_cast<int>(bytesPerTexel), bytes);
#else
    GLenum glFormat = GL_RGBA;
    GLenum glType = GL_UNSIGNED_BYTE;
    switch (format){
        case ColorFormat::RGBA:    glFormat = GL_RGBA; glType = GL_UNSIGNED_BYTE; break;
        case ColorFormat::RED:     glFormat = GL_RED;  glType = GL_UNSIGNED_BYTE; break;
        case ColorFormat::RED16:   glFormat = GL_RED;  glType = GL_UNSIGNED_SHORT; break;
        case ColorFormat::RGBA16F: glFormat = GL_RGBA; glType = GL_HALF_FLOAT; break;
        case ColorFormat::RGBA32F: glFormat = GL_RGBA; glType = GL_FLOAT; break;
        case ColorFormat::R32F:    glFormat = GL_RED;  glType = GL_FLOAT; break;
        case ColorFormat::RG16F:   glFormat = GL_RG;   glType = GL_HALF_FLOAT; break;
        case ColorFormat::R16F:    glFormat = GL_RED;  glType = GL_HALF_FLOAT; break;
        default: return false;
    }
    const GLuint handle = texture.getGLHandler();
    if (handle == 0)
        return false;

    while (glGetError() != GL_NO_ERROR) {}
    glFinish();
#if defined(SOKOL_GLES3)
    const int channels = glFormat == GL_RGBA ? 4 : (glFormat == GL_RG ? 2 : 1);
    const bool ok = readGlesTexture(handle, width, height, channels, glType, bytes);
    if (!ok) bytes.clear();
    sg_reset_state_cache();
    return ok;
#else
    bytes.resize(static_cast<size_t>(width) * height * bytesPerTexel);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, handle);
    glGetTexImage(GL_TEXTURE_2D, 0, glFormat, glType, bytes.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    const bool ok = glGetError() == GL_NO_ERROR;
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    sg_reset_state_cache();
    return ok;
#endif
#endif
}

// the engine's readback (the shipped game's --screenshot reads with it too)
bool jasperengine::test::readTexturePixels(TextureRender& texture, int width, int height, int bytesPerPixel, std::vector<uint8_t>& pixels){
    std::string error;
    if (FrameReadback::readTexture(texture, width, height, bytesPerPixel, pixels, &error))
        return true;
    std::fprintf(stderr, "readTexture: %s\n", error.c_str());
    return false;
}

bool jasperengine::test::readTextureFloat(TextureRender& texture, int width, int height, std::vector<float>& rgba){
    rgba.clear();
    if (width <= 0 || height <= 0)
        return false;
    const size_t count = static_cast<size_t>(width) * height * 4;

#if defined(SOKOL_VULKAN)
    // 16 bytes a texel, copied as they are
    std::vector<uint8_t> bytes;
    if (!readVulkanImage(texture, width, height, 16, bytes) || bytes.size() != count * sizeof(float))
        return false;
    rgba.resize(count);
    std::memcpy(rgba.data(), bytes.data(), bytes.size());
    return true;
#else
    const GLuint handle = texture.getGLHandler();
    if (handle == 0)
        return false;

    while (glGetError() != GL_NO_ERROR) {}
    // as readTexturePixels: everything drawn into it has finished
    glFinish();
#if defined(SOKOL_GLES3)
    std::vector<uint8_t> bytes;
    const bool ok = readGlesTexture(handle, width, height, 4, GL_FLOAT, bytes);
    sg_reset_state_cache();
    if (!ok)
        return false;
    rgba.resize(count);
    std::memcpy(rgba.data(), bytes.data(), bytes.size());
    return true;
#else
    rgba.resize(count);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, handle);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    const bool ok = glGetError() == GL_NO_ERROR;
    sg_reset_state_cache();
    return ok;
#endif
#endif
}
