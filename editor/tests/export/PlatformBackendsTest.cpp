// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// One table of the graphics backends per target platform (ShippingTypes.h), which the Build
// Settings window and MCP build_project share: Android builds have OpenGL ES 3 only

#include <gtest/gtest.h>

#include "ShippingTypes.h"

#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(PlatformBackends, SourceExportPresets) {
    using Set = std::set<ShaderBackend>;
    EXPECT_EQ(sourceExportBackends(BuildPlatform::Android), (Set{ShaderBackend::GLES3}));
    EXPECT_EQ(sourceExportBackends(BuildPlatform::Web), (Set{ShaderBackend::GLES3}));
    EXPECT_EQ(sourceExportBackends(BuildPlatform::IOS), (Set{ShaderBackend::MetalIOS}));
    EXPECT_EQ(sourceExportBackends(BuildPlatform::MacOS), (Set{ShaderBackend::MetalMacOS, ShaderBackend::GLCore}));
    EXPECT_EQ(sourceExportBackends(BuildPlatform::Linux), (Set{ShaderBackend::GLCore, ShaderBackend::Vulkan}));
    EXPECT_EQ(sourceExportBackends(BuildPlatform::Windows), (Set{ShaderBackend::GLCore, ShaderBackend::Vulkan, ShaderBackend::D3D11}));
}

TEST(PlatformBackends, AndroidPlayersAreGles3Only) {
    EXPECT_EQ(platformBackends(BuildPlatform::Android), (std::vector<std::string>{"gles3"}));
}

TEST(PlatformBackends, HostDesktopBackendsAreTheHostPlatformsPreset) {
    const std::vector<ShaderBackend>& host = hostDesktopBackends();
    ASSERT_FALSE(host.empty());
    const std::set<ShaderBackend> preset = sourceExportBackends(hostBuildPlatform());
    for (ShaderBackend backend : host) EXPECT_TRUE(preset.count(backend)) << static_cast<int>(backend);
}
