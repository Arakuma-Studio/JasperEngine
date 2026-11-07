// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VULKAN_ROBUSTNESS_H
#define VULKAN_ROBUSTNESS_H

#include <cstdint>

namespace jasperengine {

    // Whether a Vulkan device may enable robustBufferAccess, so an out-of-range buffer read (a
    // shader under development indexing past an array) returns zeros instead of hanging the GPU
    // (Doriax 0078f75ab, 82d13ab0f). sokol writes descriptors of the plain sizes into its
    // descriptor buffers, so the feature is safe only where the robust descriptor sizes
    // (VkPhysicalDeviceDescriptorBufferPropertiesEXT) equal the plain ones.
    struct VulkanRobustness {
        bool enable = false;
        const char* reason = "";
    };

    inline VulkanRobustness decideVulkanRobustBufferAccess(bool supported,
                                                           uint64_t robustUniformDescriptorSize, uint64_t uniformDescriptorSize,
                                                           uint64_t robustStorageDescriptorSize, uint64_t storageDescriptorSize){
        if (!supported)
            return {false, "not supported by the device"};
        if (robustUniformDescriptorSize != uniformDescriptorSize || robustStorageDescriptorSize != storageDescriptorSize)
            return {false, "the robust descriptor sizes differ from the ones sokol writes"};
        return {true, "enabled"};
    }

}

#endif
