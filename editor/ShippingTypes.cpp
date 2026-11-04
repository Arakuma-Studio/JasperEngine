// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShippingTypes.h"

using namespace jasperengine;
using namespace jasperengine::editor;

const char* editor::buildPlatformName(BuildPlatform platform) {
    switch (platform) {
        case BuildPlatform::Windows: return "windows";
        case BuildPlatform::Linux: return "linux";
        case BuildPlatform::MacOS: return "macos";
        case BuildPlatform::Android: return "android";
        case BuildPlatform::IOS: return "ios";
        case BuildPlatform::Web: return "web";
    }
    return "linux";
}

bool editor::parseBuildPlatform(const std::string& name, BuildPlatform& out) {
    for (BuildPlatform platform : {BuildPlatform::Windows, BuildPlatform::Linux, BuildPlatform::MacOS, BuildPlatform::Android,
                                   BuildPlatform::IOS, BuildPlatform::Web}) {
        if (name == buildPlatformName(platform)) {
            out = platform;
            return true;
        }
    }
    if (name == "osx" || name == "mac") {
        out = BuildPlatform::MacOS;
        return true;
    }
    return false;
}

BuildPlatform editor::hostBuildPlatform() {
#if defined(_WIN32)
    return BuildPlatform::Windows;
#elif defined(__APPLE__)
    return BuildPlatform::MacOS;
#else
    return BuildPlatform::Linux;
#endif
}

bool editor::isDesktopPlatform(BuildPlatform platform) {
    return platform == BuildPlatform::Windows || platform == BuildPlatform::Linux || platform == BuildPlatform::MacOS;
}

const char* editor::buildMethodName(BuildMethod method) {
    switch (method) {
        case BuildMethod::Auto: return "auto";
        case BuildMethod::Prebuilt: return "prebuilt";
        case BuildMethod::Compile: return "compile";
    }
    return "auto";
}

bool editor::parseBuildMethod(const std::string& name, BuildMethod& out) {
    for (BuildMethod method : {BuildMethod::Auto, BuildMethod::Prebuilt, BuildMethod::Compile}) {
        if (name == buildMethodName(method)) {
            out = method;
            return true;
        }
    }
    return false;
}

const char* editor::androidFormatName(AndroidFormat format) {
    switch (format) {
        case AndroidFormat::Aab: return "aab";
        case AndroidFormat::ApkPerAbi: return "apk-per-abi";
        case AndroidFormat::Apk: return "apk";
    }
    return "aab";
}

bool editor::parseAndroidFormat(const std::string& name, AndroidFormat& out) {
    for (AndroidFormat format : {AndroidFormat::Aab, AndroidFormat::ApkPerAbi, AndroidFormat::Apk}) {
        if (name == androidFormatName(format)) {
            out = format;
            return true;
        }
    }
    return false;
}

std::vector<std::string> editor::platformBackends(BuildPlatform platform) {
    switch (platform) {
        case BuildPlatform::Windows: return {"d3d11", "glcore", "vulkan"};
        case BuildPlatform::Linux: return {"glcore", "vulkan", "gles3"};
        case BuildPlatform::MacOS: return {"metal", "glcore"};
        case BuildPlatform::Android: return {"gles3"};
        case BuildPlatform::IOS: return {"metal"};
        case BuildPlatform::Web: return {"gles3"};
    }
    return {"glcore"};
}

std::string editor::normalizeBackend(const std::string& name) {
    if (name == "opengl" || name == "glcore" || name == "gl") return "glcore";
    if (name == "opengles" || name == "gles3" || name == "gles" || name == "webgl2") return "gles3";
    if (name == "vulkan") return "vulkan";
    if (name == "d3d11" || name == "directx" || name == "dx11") return "d3d11";
    if (name == "metal" || name == "metal-macos" || name == "metal-ios") return "metal";
    return "";
}

ShaderBackend editor::shaderBackendOf(BuildPlatform platform, const std::string& backend) {
    if (backend == "gles3") return ShaderBackend::GLES3;
    if (backend == "vulkan") return ShaderBackend::Vulkan;
    if (backend == "d3d11") return ShaderBackend::D3D11;
    if (backend == "metal") return platform == BuildPlatform::IOS ? ShaderBackend::MetalIOS : ShaderBackend::MetalMacOS;
    return ShaderBackend::GLCore;
}

std::string editor::platformArch(BuildPlatform platform) {
    switch (platform) {
        case BuildPlatform::Android:
        case BuildPlatform::MacOS: return "universal";
        case BuildPlatform::IOS: return "arm64";
        case BuildPlatform::Web: return "wasm32";
        default: return "x64";
    }
}
