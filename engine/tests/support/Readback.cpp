// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Readback.h"

#include "System.h"
#include "render/FrameReadback.h"

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

bool jasperengine::test::finishGpu(){
    return FrameReadback::finishGpu();
}

bool jasperengine::test::readTexture(TextureRender& texture, int width, int height, std::vector<uint8_t>& rgba){
    return readTexturePixels(texture, width, height, 4, rgba);
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
    rgba.resize(count);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, handle);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    const bool ok = glGetError() == GL_NO_ERROR;
    sg_reset_state_cache();
    return ok;
#endif
}
