// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Vulkan editor enables robustBufferAccess where it is safe with sokol's descriptor buffers
// (engine/core/render/VulkanRobustness.h; Doriax 0078f75ab, 82d13ab0f): sokol writes the plain
// descriptor sizes, so a device whose robust sizes differ keeps it off.

#include "render/VulkanRobustness.h"

#include <gtest/gtest.h>

#include <string>

using namespace jasperengine;

TEST(VulkanRobustness, EnabledWhereTheDescriptorSizesMatch){
    const VulkanRobustness on = decideVulkanRobustBufferAccess(true, 16, 16, 16, 16);
    EXPECT_TRUE(on.enable);
    EXPECT_EQ(std::string(on.reason), "enabled");
}

TEST(VulkanRobustness, OffWithoutTheFeature){
    const VulkanRobustness off = decideVulkanRobustBufferAccess(false, 16, 16, 16, 16);
    EXPECT_FALSE(off.enable);
    EXPECT_EQ(std::string(off.reason), "not supported by the device");
}

TEST(VulkanRobustness, OffWhereARobustDescriptorIsLarger){
    EXPECT_FALSE(decideVulkanRobustBufferAccess(true, 32, 16, 16, 16).enable);
    EXPECT_FALSE(decideVulkanRobustBufferAccess(true, 16, 16, 32, 16).enable);
}
