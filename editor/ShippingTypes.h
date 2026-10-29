// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The words of a build for a target (docs/specs/S30-shipping.md D1): which platform, which
// method (a prebuilt player, or the CMake build of a source export), which Android package.

#include "shader/ShaderBuildTypes.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    enum class BuildPlatform { Windows, Linux, MacOS, Android, IOS, Web };
    enum class BuildMethod { Auto, Prebuilt, Compile };
    enum class AndroidFormat { Apk, Aab };

    // "windows" "linux" "macos" "android" "ios" "web"
    const char* buildPlatformName(BuildPlatform platform);
    bool parseBuildPlatform(const std::string& name, BuildPlatform& out);
    BuildPlatform hostBuildPlatform();
    bool isDesktopPlatform(BuildPlatform platform);

    const char* buildMethodName(BuildMethod method);
    bool parseBuildMethod(const std::string& name, BuildMethod& out);

    // the backends a platform's players exist for (R11), the first its default; names are the
    // runtime ids' (glcore gles3 vulkan d3d11 metal)
    std::vector<std::string> platformBackends(BuildPlatform platform);
    // a backend as the CLI and MCP name it ("opengl", "opengles", "vulkan", "d3d11", "metal",
    // "metal-macos", "metal-ios", or the runtime names) -> runtime name; empty when unknown
    std::string normalizeBackend(const std::string& name);
    ShaderBackend shaderBackendOf(BuildPlatform platform, const std::string& backend);
    // the arch of the platform's runtimes (x64, universal, arm64, wasm32)
    std::string platformArch(BuildPlatform platform);

    // a warning of a build with a stable code (W-ICON-NONWINDOWS, W-MAC-UNSIGNED...)
    struct BuildWarning {
        std::string code;
        std::string message;
    };

}
