// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderPool.h"

#include "Log.h"
#include "Engine.h"
#include "io/FileData.h"
#include "shader/RuntimeShaderCompiler.h"
#include "shader/ShaderDataSerializer.h"
#include "shader/ShaderVariantCompiler.h"
#include "util/Base64.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <string>
#include <mutex>
#include <cctype>
#include <set>
#include <system_error>

#ifdef SOKOL_GLCORE
#include "glsl410.h"
#endif
#ifdef SOKOL_GLES3
#include "glsl300es.h"
#endif
#ifdef SOKOL_D3D11
#include "hlsl5.h"
#endif
#ifdef SOKOL_VULKAN
#include "spirv10.h"
#endif
// Metal shader cache. SOKOL_METAL covers cmake Metal builds. JASPERENGINE_APPLE is
// also needed: the Xcode engine targets define it without SOKOL_METAL (that
// flag is only on the sokol gfx unit), and Engine::getGraphicBackend() treats
// JASPERENGINE_APPLE as Metal. Exclude SOKOL_GLCORE so an Apple+OpenGL build does
// not pull in both headers and redefine getBase64Shader.
#if defined(SOKOL_METAL) || (defined(JASPERENGINE_APPLE) && !defined(SOKOL_GLCORE))
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include "msl21ios.h"
#elif TARGET_OS_MAC
#include "msl21macos.h"
#endif
#endif

using namespace jasperengine;

ShaderBuilderFn ShaderPool::shaderBuilderFn = nullptr;

bool ShaderPool::parseShaderTypeToken(const std::string& typeToken, ShaderType& shaderType) {
    if (typeToken == "mesh") {
        shaderType = ShaderType::MESH;
        return true;
    }
    if (typeToken == "depth") {
        shaderType = ShaderType::DEPTH;
        return true;
    }
    if (typeToken == "gbuffer") {
        shaderType = ShaderType::GBUFFER;
        return true;
    }
    if (typeToken == "sky") {
        shaderType = ShaderType::SKYBOX;
        return true;
    }
    if (typeToken == "ui") {
        shaderType = ShaderType::UI;
        return true;
    }
    if (typeToken == "points") {
        shaderType = ShaderType::POINTS;
        return true;
    }
    if (typeToken == "lines") {
        shaderType = ShaderType::LINES;
        return true;
    }
    if (typeToken == "ssao") {
        shaderType = ShaderType::SSAO;
        return true;
    }
    if (typeToken == "ssaoblur") {
        shaderType = ShaderType::SSAO_BLUR;
        return true;
    }
    if (typeToken == "ssr") {
        shaderType = ShaderType::SSR;
        return true;
    }
    if (typeToken == "ssrblur") {
        shaderType = ShaderType::SSR_BLUR;
        return true;
    }
    if (typeToken == "composite") {
        shaderType = ShaderType::COMPOSITE;
        return true;
    }
    if (typeToken == "shadow2d") {
        shaderType = ShaderType::SHADOW2D;
        return true;
    }
    if (typeToken == "blit") {
        shaderType = ShaderType::BLIT;
        return true;
    }
    if (typeToken == "postprocess") {
        shaderType = ShaderType::POSTPROCESS;
        return true;
    }
    if (typeToken == "bloom") {
        shaderType = ShaderType::BLOOM;
        return true;
    }
    if (typeToken == "htmlui") {
        shaderType = ShaderType::HTMLUI;
        return true;
    }
    // no underscore: a shader basename splits type and properties at the first one
    if (typeToken == "htmluigradient") {
        shaderType = ShaderType::HTMLUI_GRADIENT;
        return true;
    }
    if (typeToken == "htmluipost") {
        shaderType = ShaderType::HTMLUI_POST;
        return true;
    }
    if (typeToken == "uieffect") {
        shaderType = ShaderType::UI_EFFECT;
        return true;
    }
    if (typeToken == "fluid") {
        shaderType = ShaderType::FLUID;
        return true;
    }

    return false;
}

shaders_t& ShaderPool::getMap(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static shaders_t* map = new shaders_t();
    return *map;
};

std::vector<std::string>& ShaderPool::getMissingShaders(){
    static std::vector<std::string>* missingshaders = new std::vector<std::string>();
    return *missingshaders;
};

// Keys whose last build or backend resource creation failed. The editor pool stops
// re-invoking the builder for these (the compile is expensive and spams errors every
// frame) until the failure is cleared by remove()/destroyCustomShaders().
static std::set<ShaderKey>& failedShaders(){
    static std::set<ShaderKey>* set = new std::set<ShaderKey>();
    return *set;
};

// Keys the build has no precompiled shader for: looked up once, then left to the
// runtime compiler (or reported missing) without searching the build again.
static std::set<ShaderKey>& notPrecompiledShaders(){
    static std::set<ShaderKey>* set = new std::set<ShaderKey>();
    return *set;
};

// Custom shader registry: index = customId-1, value = project-relative base path.
// Bumped whenever the registry is reset (clear/clearUnused): ids held elsewhere are stale.
// read without the lock (UI effects ask for it every draw); changed under customShaderMutex()
static std::atomic<uint32_t> customShaderGeneration{1};
static std::function<std::string(const std::string&)>& customShaderNameResolver(){
    static std::function<std::string(const std::string&)>* resolver = new std::function<std::string(const std::string&)>();
    return *resolver;
}
static std::function<std::string(const std::string&)>& uiEffectSpecResolver(){
    static std::function<std::string(const std::string&)>* resolver = new std::function<std::string(const std::string&)>();
    return *resolver;
}

namespace {
    // UI effects of the engine library (phase 5): custom names "@effects/<name>", sources
    // effects/<name>.frag; their basename is fx_<name> with '-' as '_' (a leading '_' of an
    // internal effect stays)
    const char* const libraryEffectPrefix = "@effects/";

    bool startsWithText(const std::string& text, const char* prefix){
        return text.rfind(prefix, 0) == 0;
    }
}

static std::mutex& customShaderMutex(){
    static std::mutex* m = new std::mutex();
    return *m;
}
static std::vector<std::string>& customShaderNames(){
    static std::vector<std::string>* names = new std::vector<std::string>();
    return *names;
}

uint16_t ShaderPool::registerCustomShader(const std::string& baseName){
    if (baseName.empty())
        return 0;

    std::lock_guard<std::mutex> lock(customShaderMutex());
    auto& names = customShaderNames();
    for (size_t i = 0; i < names.size(); i++){
        if (names[i] == baseName)
            return (uint16_t)(i + 1);
    }
    names.push_back(baseName);
    return (uint16_t)names.size();
}

std::string ShaderPool::getCustomShaderName(uint16_t customId){
    if (customId == 0)
        return "";

    std::lock_guard<std::mutex> lock(customShaderMutex());
    auto& names = customShaderNames();
    if (customId <= names.size())
        return names[customId - 1];
    return "";
}

std::string ShaderPool::getCustomShaderBasename(uint16_t customId){
    const std::string name = getCustomShaderName(customId);
    // a library UI effect ("@effects/<name>", the HTML UI's) is "fx_<name>"
    if (startsWithText(name, libraryEffectPrefix)){
        std::string effect = name.substr(std::char_traits<char>::length(libraryEffectPrefix));
        for (size_t i = 0; i < effect.size(); i++){
            if (effect[i] == '-' || (effect[i] == '_' && i > 0) || !(std::isalnum((unsigned char)effect[i]) || effect[i] == '_'))
                effect[i] = '_';
        }
        return "fx_" + effect;
    }
    std::string base = name;
    // Sanitize the project-relative base into a filesystem-safe, deterministic token
    // (e.g. "shaders/myMesh" -> "shaders_myMesh") used in .sdat filenames.
    for (char& c : base){
        if (!(std::isalnum((unsigned char)c) || c == '_'))
            c = '_';
    }
    // A library reference or a feature suffix ("library:toon#RAMP,RIM") is not a path:
    // the hash of the whole string keeps it apart from a fork whose path sanitizes alike
    // and from the same shader with other features
    if (name.find(':') != std::string::npos || name.find('#') != std::string::npos){
        base += "_" + ShaderVariantCompiler::toHex(ShaderVariantCompiler::hash(name)).substr(0, 8);
    }
    return base;
}

void ShaderPool::setShaderBuilder(ShaderBuilderFn fn) {
    shaderBuilderFn = fn;
}

std::string ShaderPool::getShaderLangStr(ShaderLang lang, int version, bool es, Platform platform) {
    if (lang == ShaderLang::GLSL) {
        return es ? "glsl" + std::to_string(version) + "es" : "glsl" + std::to_string(version);
    }
    if (lang == ShaderLang::HLSL) {
        return "hlsl" + std::to_string(version / 10);
    }
    if (lang == ShaderLang::MSL) {
        if (platform == Platform::iOS) {
            return "msl" + std::to_string(version) + "ios";
        }
        if (platform == Platform::MacOS) {
            return "msl" + std::to_string(version) + "macos";
        }
    }
    if (lang == ShaderLang::SPIRV) {
        return "spirv" + std::to_string(version);
    }

    return "<unknown>";
}

const std::vector<ShaderBackend>& ShaderPool::getShaderBackends() {
    static const std::vector<ShaderBackend>* backends = new std::vector<ShaderBackend>{
        ShaderBackend::GLCore,
        ShaderBackend::GLES3,
        ShaderBackend::D3D11,
        ShaderBackend::MetalMacOS,
        ShaderBackend::MetalIOS,
        ShaderBackend::Vulkan
    };
    return *backends;
}

ShaderBackend ShaderPool::getShaderBackend() {
    switch (Engine::getGraphicBackend()) {
        case GraphicBackend::GLES3:  return ShaderBackend::GLES3;
        case GraphicBackend::D3D11:  return ShaderBackend::D3D11;
        case GraphicBackend::VULKAN: return ShaderBackend::Vulkan;
        // only place the OS takes part: MSL differs between macOS and iOS
        case GraphicBackend::METAL:  return (Engine::getPlatform() == Platform::iOS) ? ShaderBackend::MetalIOS : ShaderBackend::MetalMacOS;
        default:                     return ShaderBackend::GLCore;
    }
}

std::string ShaderPool::getShaderBackendName(ShaderBackend backend) {
    switch (backend) {
        case ShaderBackend::GLCore:     return "OpenGL";
        case ShaderBackend::GLES3:      return "OpenGL ES 3";
        case ShaderBackend::D3D11:      return "Direct3D 11";
        case ShaderBackend::MetalMacOS: return "Metal (macOS)";
        case ShaderBackend::MetalIOS:   return "Metal (iOS)";
        case ShaderBackend::Vulkan:     return "Vulkan";
        default:                        return "<unknown>";
    }
}

std::string ShaderPool::getShaderBackendCliToken(ShaderBackend backend) {
    switch (backend) {
        case ShaderBackend::GLCore:     return "opengl";
        case ShaderBackend::GLES3:      return "opengles";
        case ShaderBackend::D3D11:      return "d3d11";
        case ShaderBackend::MetalMacOS: return "metal-macos";
        case ShaderBackend::MetalIOS:   return "metal-ios";
        case ShaderBackend::Vulkan:     return "vulkan";
        default:                        return "<target-backend>";
    }
}

// Accepts the CLI token and the GRAPHIC_BACKEND value users see in the build settings
bool ShaderPool::parseShaderBackend(const std::string& value, ShaderBackend& out) {
    std::string token;
    for (char c : value) {
        if (std::isalnum((unsigned char)c))
            token += std::tolower((unsigned char)c);
    }

    if (token == "opengl"   || token == "glcore") { out = ShaderBackend::GLCore;     return true; }
    if (token == "opengles" || token == "gles3")  { out = ShaderBackend::GLES3;      return true; }
    if (token == "d3d11")                         { out = ShaderBackend::D3D11;      return true; }
    if (token == "metal"    || token == "metalmacos") { out = ShaderBackend::MetalMacOS; return true; }
    if (token == "metalios")                      { out = ShaderBackend::MetalIOS;   return true; }
    if (token == "vulkan")                        { out = ShaderBackend::Vulkan;     return true; }

    return false;
}

std::string ShaderPool::getShaderLangStr(ShaderBackend backend) {
    switch (backend) {
        case ShaderBackend::GLCore:     return getShaderLangStr(ShaderLang::GLSL, 410, false);
        case ShaderBackend::GLES3:      return getShaderLangStr(ShaderLang::GLSL, 300, true);
        case ShaderBackend::D3D11:      return getShaderLangStr(ShaderLang::HLSL, 50);
        case ShaderBackend::MetalMacOS: return getShaderLangStr(ShaderLang::MSL, 21, false, Platform::MacOS);
        case ShaderBackend::MetalIOS:   return getShaderLangStr(ShaderLang::MSL, 21, false, Platform::iOS);
        case ShaderBackend::Vulkan:     return getShaderLangStr(ShaderLang::SPIRV, 10);
        default:                        return "<unknown>";
    }
}

std::string ShaderPool::getShaderLangStr(){
    return getShaderLangStr(getShaderBackend());
}

bool ShaderPool::getShaderCliSpec(const std::string& shaderStr, std::string& cliSpec) {
    const size_t underscorePos = shaderStr.find('_');
    const std::string typeToken = underscorePos == std::string::npos ? shaderStr : shaderStr.substr(0, underscorePos);

    ShaderType shaderType;
    if (!parseShaderTypeToken(typeToken, shaderType)) {
        return false;
    }

    // "uieffect_<basename>[_Flt]": the effect's name, not a property list
    if (shaderType == ShaderType::UI_EFFECT) {
        if (underscorePos == std::string::npos)
            return false;
        std::string custom;
        uint32_t properties = 0;
        if (!splitUiEffectShaderStr(shaderStr.substr(underscorePos + 1), custom, properties))
            return false;
        cliSpec = typeToken + "/" + getUiEffectName(custom);
        std::string props;
        for (int bit = 0; bit < getShaderPropertyCount(shaderType); bit++) {
            if (properties & (1u << bit))
                props += (props.empty() ? "" : ",") + getShaderPropertyName(shaderType, bit, true);
        }
        if (!props.empty())
            cliSpec += ":" + props;
        return true;
    }

    cliSpec = typeToken;
    if (underscorePos == std::string::npos) {
        return true;
    }

    const std::string propsToken = shaderStr.substr(underscorePos + 1);
    if (propsToken.empty()) {
        return true;
    }

    std::string propsList;
    size_t parsedLength = 0;
    const int propCount = getShaderPropertyCount(shaderType);
    for (int bit = 0; bit < propCount; bit++) {
        const std::string propName = getShaderPropertyName(shaderType, bit, true);
        if (propName.empty() || propName == "?") {
            continue;
        }

        if (propsToken.compare(parsedLength, propName.size(), propName) == 0) {
            if (!propsList.empty()) {
                propsList += ",";
            }
            propsList += propName;
            parsedLength += propName.size();
        }
    }

    if (parsedLength != propsToken.size()) {
        return false;
    }

    if (!propsList.empty()) {
        cliSpec += ":" + propsList;
    }

    return true;
}

std::string ShaderPool::getSuggestedCliBackend() {
    return getShaderBackendCliToken(getShaderBackend());
}

std::string ShaderPool::getMissingShadersCliArgs() {
    std::string cliArgs;

    for (const std::string& shaderName : getMissingShaders()) {
        std::string cliSpec;
        if (!getShaderCliSpec(shaderName, cliSpec)) {
            continue;
        }

        cliArgs += " --shader \"" + cliSpec + "\"";
    }

    return cliArgs;
}

std::string ShaderPool::getMissingShadersDisplayList() {
    std::string shaderList;

    for (const std::string& shaderName : getMissingShaders()) {
        std::string displayName = shaderName;
        std::string cliSpec;
        if (getShaderCliSpec(shaderName, cliSpec)) {
            displayName = cliSpec;
        }

        if (!shaderList.empty()) {
            shaderList += "; ";
        }
        shaderList += displayName;
    }

    return shaderList;
}


std::string ShaderPool::getShaderName(const std::string& shaderStr){
    return shaderStr + "_" + getShaderLangStr();
}

std::string ShaderPool::getShaderTypeName(ShaderType shaderType, bool lowerCase){
    switch (shaderType) {
        case ShaderType::MESH:   return lowerCase ? "mesh"   : "Mesh";
        case ShaderType::DEPTH:  return lowerCase ? "depth"  : "Depth";
        case ShaderType::GBUFFER: return lowerCase ? "gbuffer" : "GBuffer";
        case ShaderType::SKYBOX: return lowerCase ? "sky"    : "Skybox";
        case ShaderType::UI:     return lowerCase ? "ui"     : "UI";
        case ShaderType::POINTS: return lowerCase ? "points" : "Points";
        case ShaderType::LINES:  return lowerCase ? "lines"  : "Lines";
        case ShaderType::SSAO:   return lowerCase ? "ssao"   : "SSAO";
        case ShaderType::SSAO_BLUR: return lowerCase ? "ssaoblur" : "SSAO Blur";
        case ShaderType::SSR:    return lowerCase ? "ssr"    : "SSR";
        case ShaderType::SSR_BLUR: return lowerCase ? "ssrblur" : "SSR Blur";
        case ShaderType::COMPOSITE: return lowerCase ? "composite" : "Composite";
        case ShaderType::SHADOW2D: return lowerCase ? "shadow2d" : "Shadow 2D";
        case ShaderType::BLIT:   return lowerCase ? "blit"   : "Blit";
        case ShaderType::POSTPROCESS: return lowerCase ? "postprocess" : "Post-process";
        case ShaderType::BLOOM:  return lowerCase ? "bloom"  : "Bloom";
        case ShaderType::HTMLUI: return lowerCase ? "htmlui" : "HTML UI";
        case ShaderType::HTMLUI_GRADIENT: return lowerCase ? "htmluigradient" : "HTML UI Gradient";
        case ShaderType::HTMLUI_POST: return lowerCase ? "htmluipost" : "HTML UI Post";
        case ShaderType::UI_EFFECT: return lowerCase ? "uieffect" : "UI Effect";
        case ShaderType::FLUID:  return lowerCase ? "fluid"  : "Fluid";
        default:                 return lowerCase ? "unknown": "Unknown";
    }
}

uint32_t ShaderPool::getPostColorProperty(ShaderType shaderType){
    switch (shaderType) {
        case ShaderType::MESH:   return 1u << 31;
        case ShaderType::POINTS: return 1u << 4;
        case ShaderType::LINES:  return 1u << 2;
        case ShaderType::SKYBOX: return 1u << 0;
        default:                 return 0;
    }
}

int ShaderPool::getShaderPropertyCount(ShaderType shaderType){
    switch (shaderType) {
        case ShaderType::MESH:   return 32;
        case ShaderType::DEPTH:  return 14;
        case ShaderType::GBUFFER: return 10;
        case ShaderType::UI:     return 4;
        case ShaderType::POINTS: return 5;
        case ShaderType::LINES:  return 3;
        case ShaderType::SKYBOX: return 1;
        case ShaderType::SSAO:   return 0;
        case ShaderType::SSAO_BLUR: return 0;
        case ShaderType::SSR:    return 0;
        case ShaderType::SSR_BLUR: return 0;
        case ShaderType::COMPOSITE: return 0;
        case ShaderType::SHADOW2D: return 0;
        case ShaderType::BLIT:   return 13;
        case ShaderType::BLOOM:  return 4;
        case ShaderType::HTMLUI: return 1;
        case ShaderType::HTMLUI_GRADIENT: return 0;
        case ShaderType::HTMLUI_POST: return 7;
        // Flt (filter form), Stp and Sct (phase 6: a stateful effect's step and scatter forms)
        case ShaderType::UI_EFFECT: return 3;
        // Lok (the look program), Mfl (the solver with manual filtering)
        case ShaderType::FLUID:  return 2;
        default:                 return 0;
    }
}

std::string ShaderPool::getShaderPropertyName(ShaderType shaderType, int bit, bool shortName){
    if (shaderType == ShaderType::MESH) {
        switch (bit) {
            case 0:  return shortName ? "Ult" : "Unlit";
            case 1:  return shortName ? "Uv1" : "UV1";
            case 2:  return shortName ? "Uv2" : "UV2";
            case 3:  return shortName ? "Puc" : "Punctual";
            case 4:  return shortName ? "Shw" : "Shadow";
            // case 5 reserved (was 'Pcf'; the PCF filter is uniform-driven now)
            case 6:  return shortName ? "Nor" : "Normals";
            case 7:  return shortName ? "Nmp" : "Normal Map";
            case 8:  return shortName ? "Tan" : "Tangents";
            case 9:  return shortName ? "Vc3" : "Vertex Color 3";
            case 10: return shortName ? "Vc4" : "Vertex Color 4";
            case 11: return shortName ? "Txr" : "Texture Rect";
            case 12: return "Fog";
            case 13: return shortName ? "Ski" : "Skinning";
            case 14: return shortName ? "Mta" : "Morph Target";
            case 15: return shortName ? "Mnr" : "Morph Normal";
            case 16: return shortName ? "Mtg" : "Morph Tangent";
            case 17: return shortName ? "Ter" : "Terrain";
            case 18: return shortName ? "Ist" : "Instancing";
            case 19: return shortName ? "Ibl" : "IBL";
            case 20: return shortName ? "Mir" : "Mirror";
            case 21: return shortName ? "Sao" : "SSAO";
            case 22: return shortName ? "L2d" : "Light 2D";
            case 23: return shortName ? "S2d" : "Shadow 2D";
            case 24: return shortName ? "Ams" : "Alpha Mask";
            case 25: return shortName ? "Aop" : "Alpha Opaque";
            case 26: return shortName ? "Ifd" : "Instance Fade";
            case 27: return shortName ? "Tpb" : "Terrain PBR";
            case 28: return shortName ? "Vtl" : "Vertex Lights";
            // 29-30: light capacity tier, neither = 8 lights
            case 29: return shortName ? "Lc4" : "Light Capacity 4";
            case 30: return shortName ? "L16" : "Light Capacity 16";
            // the post-processing colour (phase 4, USE_POST_COLOR): one global bit
            case 31: return shortName ? "Pcl" : "Post Color";
        }
    } else if (shaderType == ShaderType::DEPTH) {
        switch (bit) {
            case 0: return shortName ? "Tex" : "Texture";
            case 1: return shortName ? "Ski" : "Skinning";
            case 2: return shortName ? "Mta" : "Morph Target";
            case 3: return shortName ? "Mnr" : "Morph Normal";
            case 4: return shortName ? "Mtg" : "Morph Tangent";
            case 5: return shortName ? "Ter" : "Terrain";
            case 6: return shortName ? "Ist" : "Instancing";
            case 7: return shortName ? "Ams" : "Alpha Mask";
            case 8: return shortName ? "Ifd" : "Instance Fade";
            // inputs a library depth program asks for (@require), never set for the built-in one
            case 9: return shortName ? "Nor" : "Normals";
            case 10: return shortName ? "Vc3" : "Vertex Color 3";
            case 11: return shortName ? "Vc4" : "Vertex Color 4";
            case 12: return shortName ? "Uv1" : "UV1";
            // the sprite texture rect the alpha-clip UV goes through
            case 13: return shortName ? "Txr" : "Texture Rect";
        }
    } else if (shaderType == ShaderType::GBUFFER) {
        switch (bit) {
            case 0: return shortName ? "Bct" : "Base Color Texture";
            case 1: return shortName ? "Nor" : "Normals";
            case 2: return shortName ? "Ski" : "Skinning";
            case 3: return shortName ? "Mta" : "Morph Target";
            case 4: return shortName ? "Mnr" : "Morph Normal";
            case 5: return shortName ? "Mtg" : "Morph Tangent";
            case 6: return shortName ? "Ter" : "Terrain";
            case 7: return shortName ? "Ist" : "Instancing";
            case 8: return shortName ? "Mrt" : "MetalRough Texture";
            case 9: return shortName ? "Tpb" : "Terrain PBR";
        }
    } else if (shaderType == ShaderType::UI) {
        switch (bit) {
            case 0: return shortName ? "Tex" : "Texture";
            case 1: return shortName ? "Ftx" : "Font Texture";
            case 2: return shortName ? "Vc3" : "Vertex Color 3";
            case 3: return shortName ? "Vc4" : "Vertex Color 4";
        }
    } else if (shaderType == ShaderType::HTMLUI) {
        switch (bit) {
            case 0: return shortName ? "Tex" : "Texture";
        }
    } else if (shaderType == ShaderType::HTMLUI_POST) {
        // exclusive: a variant has exactly one of them
        switch (bit) {
            case 0: return shortName ? "Pas" : "Passthrough";
            case 1: return shortName ? "Cmx" : "Color Matrix";
            case 2: return shortName ? "Msk" : "Blend Mask";
            case 3: return shortName ? "Shd" : "Drop Shadow";
            case 4: return shortName ? "Blr" : "Blur";
            case 5: return shortName ? "Unp" : "Unpremultiply";
            case 6: return shortName ? "Yuv" : "YUV";
        }
    } else if (shaderType == ShaderType::UI_EFFECT) {
        switch (bit) {
            case 0: return shortName ? "Flt" : "Filter form";
            case 1: return shortName ? "Stp" : "Step form";
            case 2: return shortName ? "Sct" : "Scatter form";
        }
    } else if (shaderType == ShaderType::POINTS) {
        switch (bit) {
            case 0: return shortName ? "Tex" : "Texture";
            case 1: return shortName ? "Vc3" : "Vertex Color 3";
            case 2: return shortName ? "Vc4" : "Vertex Color 4";
            case 3: return shortName ? "Txr" : "Texture Rect";
            case 4: return shortName ? "Pcl" : "Post Color";
        }
    } else if (shaderType == ShaderType::LINES) {
        switch (bit) {
            case 0: return shortName ? "Vc3" : "Vertex Color 3";
            case 1: return shortName ? "Vc4" : "Vertex Color 4";
            case 2: return shortName ? "Pcl" : "Post Color";
        }
    } else if (shaderType == ShaderType::SKYBOX) {
        switch (bit) {
            case 0: return shortName ? "Pcl" : "Post Color";
        }
    } else if (shaderType == ShaderType::BLIT) {
        // the uber pass's features (RenderSystem::UberFeature); 0 is the plain blit
        switch (bit) {
            case 0: return shortName ? "Fxa" : "FXAA";
            case 1: return shortName ? "Cab" : "Chromatic Aberration";
            case 2: return shortName ? "Pix" : "Pixelate";
            case 3: return shortName ? "Crt" : "CRT";
            case 4: return shortName ? "Edg" : "Edge Detect";
            case 5: return shortName ? "Grn" : "Film Grain";
            case 6: return shortName ? "Shp" : "Sharpen";
            case 7: return shortName ? "Lut" : "Color Grade LUT";
            case 8: return shortName ? "Vig" : "Vignette";
            case 9: return shortName ? "Dth" : "Dither";
            case 10: return shortName ? "Blm" : "Bloom";
            case 11: return shortName ? "Blr" : "Blur";
            case 12: return shortName ? "Scl" : "Upscale";
        }
    } else if (shaderType == ShaderType::BLOOM) {
        // the pass of the bloom and blur chain (bloom.frag); 0 is the dual-filter downsample
        switch (bit) {
            case 0: return shortName ? "Pre" : "Prefilter";
            case 1: return shortName ? "Thr" : "Threshold";
            case 2: return shortName ? "Ups" : "Upsample";
            case 3: return shortName ? "Mix" : "Mix Level";
        }
    } else if (shaderType == ShaderType::FLUID) {
        // FluidSimulator's programs (fluid.frag): 0 is the solver
        switch (bit) {
            case 0: return shortName ? "Lok" : "Look";
            case 1: return shortName ? "Mfl" : "Manual Filtering";
        }
    }
    return shortName ? "?" : "Unknown";
}

std::string ShaderPool::getShaderStr(ShaderType shaderType, uint32_t properties, uint16_t customId){

    std::string str = getShaderTypeName(shaderType, true);
    std::string propOut;

    // Custom (forked) shaders get a unique basename so their compiled .sdat does not
    // collide with the built-in shader of the same type/variant.
    if (customId != 0) {
        std::string customBase = getCustomShaderBasename(customId);
        if (!customBase.empty())
            str += "_" + customBase;
    }

    int propCount = getShaderPropertyCount(shaderType);
    for (int i = 0; i < propCount; i++) {
        if (properties & (1u << i))
            propOut += getShaderPropertyName(shaderType, i, true);
    }

    if (!propOut.empty())
        str += "_" + propOut;

    return str;
}

ShaderKey ShaderPool::getShaderKey(ShaderType shaderType, uint32_t properties, uint16_t customId) {
    return ((uint64_t)customId << 48) | (((uint64_t)shaderType & 0xFFFF) << 32) | (properties & 0xFFFFFFFF);
}

void ShaderPool::addMissingShader(const std::string& shaderStr) {
    auto& missingShaders = getMissingShaders();
    if (std::find(missingShaders.begin(), missingShaders.end(), shaderStr) == missingShaders.end()) {
        missingShaders.push_back(shaderStr);
    }
}

std::shared_ptr<ShaderRender> ShaderPool::get(ShaderType shaderType, uint32_t properties, uint16_t customId){
    ShaderKey shaderKey = getShaderKey(shaderType, properties, customId);
    auto& shared = getMap()[shaderKey];

    if (shared && shared->isCreated()){
        return shared;
    }

    // The build can succeed and the backend still reject the shader (Metal compiles
    // the native library here). Retire the handle so its pool slot is released.
    if (shared && shared->isFailed()) {
        shared->destroyShader();
        failedShaders().insert(shaderKey);
        Log::error("Shader resource creation failed: %s",
                   getShaderStr(shaderType, properties, customId).c_str());
        return shared;
    }

    if (!shared) {
        shared = std::make_shared<ShaderRender>();
    }

    if (!shared->isCreated()) {
        // A previous attempt failed: don't retry every frame (it recompiles and spams
        // errors). Retried once remove()/destroyCustomShaders() clears the failure.
        if (failedShaders().count(shaderKey)) {
            return shared;
        }
        if (shaderBuilderFn) {
            ShaderBuildResult result = shaderBuilderFn(shaderKey);
            if (result.state == ResourceLoadState::Finished) {
                shared->createShader(result.data);
                failedShaders().erase(shaderKey);
            } else if (result.state == ResourceLoadState::Loading) {
                // Shader is still building, do nothing
            } else if (result.state == ResourceLoadState::Failed) {
                failedShaders().insert(shaderKey);
                Log::error("Shader build failed: %s", getShaderStr(shaderType, properties, customId).c_str());
            }
        } else {
            const std::string shaderStr = getShaderStr(shaderType, properties, customId);

            ShaderData precompiled;
            if (!notPrecompiledShaders().count(shaderKey) && loadPrecompiledShader(shaderKey, shaderStr, precompiled)){
                shared->createShader(precompiled);
            } else {
                // looked for once: what the build ships does not change
                notPrecompiledShaders().insert(shaderKey);

                if (RuntimeShaderCompiler::isEnabled()){
                    // Built in the background: Loading leaves whatever draws with it out
                    // until it is done, and the next get() creates it
                    ShaderBuildResult result = RuntimeShaderCompiler::request(shaderKey);
                    if (result.state == ResourceLoadState::Finished){
                        shared->createShader(result.data);
                    } else if (result.state == ResourceLoadState::Failed){
                        failedShaders().insert(shaderKey);
                        Log::error("Shader %s (%s) failed to compile at runtime: %s",
                                   shaderStr.c_str(), getShaderLangStr().c_str(),
                                   RuntimeShaderCompiler::getError(shaderKey).c_str());
                    }
                } else {
                    // Not in this build: reported once (RenderSystem::draw), and not looked
                    // for again every frame by whatever needs it
                    addMissingShader(shaderStr);
                    failedShaders().insert(shaderKey);
                }
            }
        }
    }

    return shared;
}

bool ShaderPool::loadPrecompiledShader(ShaderKey shaderKey, const std::string& shaderStr, ShaderData& shaderData){
    // Validate against the storage key (customId stripped): the unique shaderStr
    // already selected the forked source; the embedded key only checks type+props.
    const ShaderKey storageKey = getStorageKey(shaderKey);

    const std::string base64Shd = getBase64Shader(getShaderName(shaderStr));
    if (!base64Shd.empty() && ShaderDataSerializer::readFromBytes(Base64::decode(base64Shd), storageKey, shaderData)){
        return true;
    }
    return ShaderDataSerializer::readFromFile("shader://"+getShaderName(shaderStr)+".sdat", storageKey, shaderData);
}

bool ShaderPool::warmUp(ShaderType shaderType, uint32_t properties, uint16_t customId){
    std::shared_ptr<ShaderRender> shader = get(shaderType, properties, customId);
    return shader && shader->isCreated();
}

bool ShaderPool::warmUp(ShaderKey shaderKey){
    return warmUp(getShaderTypeFromKey(shaderKey), getPropertiesFromKey(shaderKey), getCustomIdFromKey(shaderKey));
}

bool ShaderPool::isShaderReady(ShaderType shaderType, uint32_t properties, uint16_t customId){
    auto& map = getMap();
    auto it = map.find(getShaderKey(shaderType, properties, customId));
    return it != map.end() && it->second && it->second->isCreated();
}

ResourceLoadState ShaderPool::getShaderState(ShaderKey shaderKey){
    if (failedShaders().count(shaderKey))
        return ResourceLoadState::Failed;
    auto& map = getMap();
    auto it = map.find(shaderKey);
    if (it == map.end() || !it->second)
        return ResourceLoadState::NotStarted;
    if (it->second->isCreated())
        return ResourceLoadState::Finished;
    if (it->second->isFailed())
        return ResourceLoadState::Failed;
    return ResourceLoadState::Loading;
}

std::vector<ShaderKey> ShaderPool::getPrecompiledShaderKeys(){
    std::set<ShaderKey> keys;
    const std::string suffix = "_" + getShaderLangStr();

    auto addName = [&](std::string name){
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0){
            ShaderKey key;
            // forked shaders' names carry their file, not a key: they warm up through a
            // ShaderVariantCollection that names the fork
            if (parseShaderStr(name.substr(0, name.size() - suffix.size()), key))
                keys.insert(key);
        }
    };

    for (const std::string& name : getBase64ShaderNames()){
        addName(name);
    }

    // a binary export's shader directory (no listing where it is not a directory)
    std::error_code ec;
    const std::filesystem::path shaderDir(FileData::getSystemPath("shader://"));
    for (std::filesystem::directory_iterator it(shaderDir, ec), end; !ec && it != end; it.increment(ec)){
        const std::filesystem::path path = it->path();
        if (path.extension() == ".sdat")
            addName(path.stem().string());
    }

    return std::vector<ShaderKey>(keys.begin(), keys.end());
}

size_t ShaderPool::warmUpAllShaders(){
    std::set<ShaderKey> keys;
    for (ShaderKey key : getPrecompiledShaderKeys())
        keys.insert(key);
    if (RuntimeShaderCompiler::isEnabled()){
        for (ShaderKey key : RuntimeShaderCompiler::getCachedKeys())
            keys.insert(key);
    }

    size_t notReady = 0;
    for (ShaderKey key : keys){
        if (!warmUp(key) && !failedShaders().count(key))
            notReady++;
    }
    return notReady;
}

size_t ShaderPool::getPendingShaderCount(){
    return RuntimeShaderCompiler::getPendingCount();
}

bool ShaderPool::isRuntimeCompilationSupported(){
    return RuntimeShaderCompiler::isSupported();
}

void ShaderPool::setRuntimeCompilationEnabled(bool enabled){
    RuntimeShaderCompiler::setEnabled(enabled);
}

bool ShaderPool::isRuntimeCompilationEnabled(){
    return RuntimeShaderCompiler::isEnabled();
}

void ShaderPool::setAsyncCompilation(bool async){
    RuntimeShaderCompiler::setAsync(async);
}

bool ShaderPool::isAsyncCompilation(){
    return RuntimeShaderCompiler::isAsync();
}

namespace {

std::string normalizeSpecToken(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            result.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return result;
}

std::vector<std::string> splitSpecList(const std::string& value) {
    std::string normalized = value;
    for (char& ch : normalized) {
        if (ch == '+' || ch == '|' || ch == ';') {
            ch = ',';
        }
    }

    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= normalized.size()) {
        size_t end = normalized.find(',', start);
        if (end == std::string::npos)
            end = normalized.size();
        const std::string part = normalized.substr(start, end - start);
        const size_t first = part.find_first_not_of(" \t\n\r");
        const size_t last = part.find_last_not_of(" \t\n\r");
        if (first != std::string::npos)
            parts.push_back(part.substr(first, last - first + 1));
        start = end + 1;
    }
    return parts;
}

bool parseSpecUInt32(const std::string& value, uint32_t& out) {
    if (value.empty() || value[0] == '+' || value[0] == '-') {
        return false;
    }

    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value.c_str(), &end, 0);
    if (!end || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }

    out = static_cast<uint32_t>(parsed);
    return true;
}

bool parseSpecTypeName(const std::string& value, ShaderType& out) {
    const std::string token = normalizeSpecToken(value);
    if (token == "point")  { out = ShaderType::POINTS; return true; }
    if (token == "line")   { out = ShaderType::LINES; return true; }
    if (token == "skybox") { out = ShaderType::SKYBOX; return true; }
    if (token == "post")   { out = ShaderType::POSTPROCESS; return true; }
    return ShaderPool::parseShaderTypeToken(token, out);
}

} // namespace

bool ShaderPool::parseShaderSpec(const std::string& spec, ShaderKey& shaderKey, std::string* error, const std::string& customShader){
    auto fail = [error](const std::string& message){
        if (error)
            *error = message;
        return false;
    };

    const size_t separator = spec.find(':');
    std::string typePart  = separator == std::string::npos ? spec : spec.substr(0, separator);
    const std::string propsPart = separator == std::string::npos ? ""   : spec.substr(separator + 1);

    // "uieffect/<name>": a UI effect names its effect after the type
    std::string effectName;
    const size_t slash = typePart.find('/');
    if (slash != std::string::npos) {
        effectName = typePart.substr(slash + 1);
        typePart = typePart.substr(0, slash);
    }

    ShaderType type;
    if (!parseSpecTypeName(typePart, type)) {
        return fail("Unknown shader type: " + typePart);
    }

    std::string custom = customShader;
    if (type == ShaderType::UI_EFFECT) {
        if (!effectName.empty()) {
            if (effectName == "all")
                return fail("uieffect/all names every built-in effect: expand it with expandShaderSpec");
            custom = resolveUiEffect(effectName);
            if (custom.empty())
                return fail("Unknown UI effect: " + effectName);
        } else if (custom.empty()) {
            return fail("A UI effect spec names its effect: uieffect/<name>[:Flt]");
        }
    } else if (!effectName.empty()) {
        return fail("Only UI effects take a name after the type: " + spec);
    }

    uint32_t properties = 0;
    if (!propsPart.empty()) {
        const bool numericMask = std::isdigit(static_cast<unsigned char>(propsPart[0]));

        if (numericMask) {
            if (!parseSpecUInt32(propsPart, properties)) {
                return fail("Invalid shader property mask: " + propsPart);
            }
        } else {
            const int propCount = getShaderPropertyCount(type);
            for (const std::string& prop : splitSpecList(propsPart)) {
                const std::string propToken = normalizeSpecToken(prop);
                bool found = false;
                for (int bit = 0; bit < propCount; bit++) {
                    const std::string shortName = normalizeSpecToken(getShaderPropertyName(type, bit, true));
                    const std::string longName  = normalizeSpecToken(getShaderPropertyName(type, bit, false));
                    if (propToken == shortName || propToken == longName) {
                        properties |= (1u << bit);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return fail("Unknown shader property '" + prop + "' for shader type " + typePart);
                }
            }
        }
    }

    shaderKey = getShaderKey(type, properties, registerCustomShader(custom));
    return true;
}

bool ShaderPool::expandShaderSpec(const std::string& spec, std::vector<ShaderKey>& shaderKeys, std::string* error){
    std::string lower;
    for (char c : spec)
        lower += (char)std::tolower((unsigned char)c);
    if (lower != "uieffect/all") {
        ShaderKey key;
        if (!parseShaderSpec(spec, key, error))
            return false;
        shaderKeys.push_back(key);
        return true;
    }

    // every effect of the library: a paint effect in both forms, a filter effect (and an
    // internal one) in the filter form
    const auto& library = ShaderVariantCompiler::getLibrarySources();
    std::vector<std::string> names;
    for (const auto& file : library) {
        const std::string& path = file.first;
        if (path.rfind("effects/", 0) != 0 || path.size() <= 13 || path.compare(path.size() - 5, 5, ".frag") != 0)
            continue;
        const std::string name = path.substr(8, path.size() - 13);
        if (name.find('/') != std::string::npos)
            continue;
        names.push_back(name);
    }
    if (names.empty()) {
        if (error)
            *error = "This build has no UI effect library to expand uieffect/all from";
        return false;
    }
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        const uint16_t customId = registerCustomShader(libraryEffectPrefix + name);
        // the manifest's "// @kind paint|filter" line (internal effects have none: filter form)
        const std::string& source = library.at("effects/" + name + ".frag");
        const size_t kind = source.find("// @kind");
        const size_t end = kind == std::string::npos ? kind : source.find('\n', kind);
        const bool paint = kind != std::string::npos && source.substr(kind, end - kind).find("paint") != std::string::npos;
        // phase 6: a stateful effect (// @state lines) has a step form per draw form, and a
        // scatter form when a step (or frame) pass scatters
        const bool stateful = source.find("\n// @state") != std::string::npos;
        bool scatter = false;
        for (const char* directive : {"\n// @step", "\n// @frame"}) {
            for (size_t at = source.find(directive); stateful && at != std::string::npos; at = source.find(directive, at + 1)) {
                const size_t lineEnd = source.find('\n', at + 1);
                if (source.substr(at, lineEnd == std::string::npos ? std::string::npos : lineEnd - at).find(" scatter ") != std::string::npos)
                    scatter = true;
            }
        }
        auto addForms = [&](uint32_t drawForm){
            shaderKeys.push_back(getShaderKey(ShaderType::UI_EFFECT, drawForm, customId));
            if (stateful)
                shaderKeys.push_back(getShaderKey(ShaderType::UI_EFFECT, drawForm | 2u, customId));
            if (scatter)
                shaderKeys.push_back(getShaderKey(ShaderType::UI_EFFECT, drawForm | 4u, customId));
        };
        if (paint)
            addForms(0);
        addForms(1);
    }
    return true;
}

bool ShaderPool::splitUiEffectShaderStr(const std::string& basenameAndProperties, std::string& customShader, uint32_t& properties){
    // "<basename>[_<Flt|Stp|Sct, concatenated>]": the property names are capitalised, effect
    // basenames are not, and the longest suffix that leaves a known effect wins
    static const struct { const char* suffix; uint32_t properties; } suffixes[] = {
        {"_FltStp", 1u | 2u}, {"_FltSct", 1u | 4u}, {"_Flt", 1u}, {"_Stp", 2u}, {"_Sct", 4u}};
    for (const auto& candidate : suffixes) {
        const size_t length = std::char_traits<char>::length(candidate.suffix);
        if (basenameAndProperties.size() <= length ||
            basenameAndProperties.compare(basenameAndProperties.size() - length, length, candidate.suffix) != 0)
            continue;
        const std::string custom = resolveCustomShaderBasename(basenameAndProperties.substr(0, basenameAndProperties.size() - length));
        if (!custom.empty()) {
            customShader = custom;
            properties = candidate.properties;
            return true;
        }
    }
    customShader = resolveCustomShaderBasename(basenameAndProperties);
    properties = 0;
    return !customShader.empty();
}

bool ShaderPool::parseShaderStr(const std::string& shaderStr, ShaderKey& shaderKey){
    // a UI effect: its basename back into the custom shader it names
    if (shaderStr.rfind("uieffect_", 0) == 0) {
        std::string custom;
        uint32_t properties = 0;
        if (!splitUiEffectShaderStr(shaderStr.substr(9), custom, properties))
            return false;
        shaderKey = getShaderKey(ShaderType::UI_EFFECT, properties, registerCustomShader(custom));
        return true;
    }
    std::string spec;
    return getShaderCliSpec(shaderStr, spec) && parseShaderSpec(spec, shaderKey);
}

bool ShaderPool::isLibraryCustomShader(uint16_t customId){
    return customId != 0 && startsWithText(getCustomShaderName(customId), "@");
}

void ShaderPool::setCustomShaderNameResolver(std::function<std::string(const std::string& basename)> resolver){
    customShaderNameResolver() = std::move(resolver);
}

void ShaderPool::setUiEffectResolver(std::function<std::string(const std::string& effectName)> resolver){
    uiEffectSpecResolver() = std::move(resolver);
}

uint32_t ShaderPool::getCustomShaderGeneration(){
    return customShaderGeneration.load(std::memory_order_acquire);
}

std::string ShaderPool::resolveCustomShaderBasename(const std::string& basename){
    // the library's effects map back without a resolver ("fx_cursor_trail", "fx__bright")
    if (basename.rfind("fx_", 0) == 0 && basename.size() > 3) {
        std::string name = basename.substr(3);
        for (size_t i = 1; i < name.size(); i++) {
            if (name[i] == '_')
                name[i] = '-';
        }
        return libraryEffectPrefix + name;
    }
    const auto& resolver = customShaderNameResolver();
    return resolver ? resolver(basename) : std::string();
}

std::string ShaderPool::resolveUiEffect(const std::string& effectName){
    const auto& resolver = uiEffectSpecResolver();
    if (resolver) {
        const std::string custom = resolver(effectName);
        if (!custom.empty())
            return custom;
    }
    // without the effect registry: the library's sources (a build with the shader compiler)
    if (ShaderVariantCompiler::getLibrarySources().count("effects/" + effectName + ".frag"))
        return libraryEffectPrefix + effectName;
    return std::string();
}

std::string ShaderPool::getUiEffectName(const std::string& customShader){
    if (startsWithText(customShader, libraryEffectPrefix))
        return customShader.substr(std::char_traits<char>::length(libraryEffectPrefix));
    // a project effect: its .frag file's stem
    const size_t slash = customShader.find_last_of('/');
    std::string stem = slash == std::string::npos ? customShader : customShader.substr(slash + 1);
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos)
        stem.resize(dot);
    return stem;
}

bool ShaderPool::isShaderBuildFailed(ShaderType shaderType, uint32_t properties, uint16_t customId){
    return failedShaders().count(getShaderKey(shaderType, properties, customId)) > 0;
}

void ShaderPool::remove(ShaderType shaderType, uint32_t properties, uint16_t customId){
    ShaderKey shaderKey = getShaderKey(shaderType, properties, customId);
    failedShaders().erase(shaderKey);
    auto& map = getMap();
    auto it = map.find(shaderKey);
    if (it != map.end()){
        if (!it->second || it->second.use_count() <= 1){
            if (it->second){
                it->second->destroyShader();
            }
            map.erase(it);
        }
    }else{
        if (Engine::isViewLoaded()){
            Log::debug("Trying to destroy a non existent shader");
        }
    }
}

void ShaderPool::destroyCustomShaders(){
    // library shaders ("library:<name>") and the library UI effects ("@effects/<name>") are
    // embedded in the engine: their sources never change with the project
    auto isFork = [](ShaderKey key){
        const uint16_t customId = getCustomIdFromKey(key);
        return customId != 0 && getCustomShaderName(customId).compare(0, 8, "library:") != 0 &&
               !isLibraryCustomShader(customId);
    };
    for (auto& entry : getMap()){
        if (isFork(entry.first) && entry.second && entry.second->isCreated()){
            entry.second->destroyShader();
        }
    }
    // Forget custom-shader build failures so an edited source is retried on the next get().
    for (auto it = failedShaders().begin(); it != failedShaders().end();){
        if (isFork(*it)){
            it = failedShaders().erase(it);
        } else {
            ++it;
        }
    }
}

ShaderType ShaderPool::getShaderTypeFromKey(ShaderKey key) {
    return (ShaderType)((key >> 32) & 0xFFFF);
}

uint32_t ShaderPool::getPropertiesFromKey(ShaderKey key) {
    return (uint32_t)(key & 0xFFFFFFFF);
}

uint16_t ShaderPool::getCustomIdFromKey(ShaderKey key) {
    return (uint16_t)((key >> 48) & 0xFFFF);
}

ShaderKey ShaderPool::getStorageKey(ShaderKey key) {
    // Strip the session-local customShaderId (bits 48..63); keep type + properties.
    return key & 0x0000FFFFFFFFFFFFULL;
}

uint32_t ShaderPool::getValidPropertyMask(ShaderType shaderType) {
    // Derived from the property-name table so a retired bit (e.g. MESH bit 5, the
    // old 'Pcf') is excluded automatically: it has no short name ("?").
    uint32_t mask = 0;
    int propCount = getShaderPropertyCount(shaderType);
    for (int i = 0; i < propCount; i++) {
        if (getShaderPropertyName(shaderType, i, true) != "?")
            mask |= (1u << i);
    }
    return mask;
}

ShaderKey ShaderPool::normalizeKey(ShaderKey key) {
    ShaderType type = getShaderTypeFromKey(key);
    uint16_t customId = getCustomIdFromKey(key);
    uint32_t properties = getPropertiesFromKey(key) & getValidPropertyMask(type);
    return getShaderKey(type, properties, customId);
}

uint32_t ShaderPool::getMeshProperties(
    bool unlit, bool uv1, bool uv2,
    bool punctual, bool shadows, bool normals, bool normalMap,
    bool tangents, bool vertexColorVec3, bool vertexColorVec4, bool textureRect,
    bool fog, bool skinning, bool morphTarget, bool morphNormal, bool morphTangent,
    bool terrain, bool instanced, bool ibl, bool mirror, bool ssao, bool light2d, bool shadows2d,
    bool alphaMask, bool alphaOpaque, bool instanceFade, bool terrainPBR,
    bool vertexLights, int lightCapacity){
    uint32_t prop = 0;

    prop |= unlit            ? (1 <<  0) : 0;
    prop |= uv1              ? (1 <<  1) : 0;
    prop |= uv2              ? (1 <<  2) : 0;
    prop |= punctual         ? (1 <<  3) : 0;
    prop |= shadows          ? (1 <<  4) : 0;
    // bit 5 was USE_SHADOWS_PCF; the PCF filter is uniform-driven now (Scene
    // shadowQuality). Kept reserved so the other bit positions stay stable.
    prop |= normals          ? (1 <<  6) : 0;
    prop |= normalMap        ? (1 <<  7) : 0;
    prop |= tangents         ? (1 <<  8) : 0;
    prop |= vertexColorVec3  ? (1 <<  9) : 0;
    prop |= vertexColorVec4  ? (1 << 10) : 0;
    prop |= textureRect      ? (1 << 11) : 0;
    prop |= fog              ? (1 << 12) : 0;
    prop |= skinning         ? (1 << 13) : 0;
    prop |= morphTarget      ? (1 << 14) : 0;
    prop |= morphNormal      ? (1 << 15) : 0;
    prop |= morphTangent     ? (1 << 16) : 0;
    prop |= terrain          ? (1 << 17) : 0;
    prop |= instanced        ? (1 << 18) : 0;
    prop |= ibl              ? (1 << 19) : 0;
    prop |= mirror           ? (1 << 20) : 0;
    prop |= ssao             ? (1 << 21) : 0;
    prop |= light2d          ? (1 << 22) : 0;
    prop |= shadows2d        ? (1 << 23) : 0;
    prop |= alphaMask        ? (1 << 24) : 0;
    prop |= alphaOpaque      ? (1 << 25) : 0;
    prop |= instanceFade     ? (1 << 26) : 0;
    prop |= terrainPBR       ? (1 << 27) : 0;
    prop |= vertexLights     ? (1 << 28) : 0;
    // the default capacity (8) sets neither bit, so it keeps the keys it had before tiers
    prop |= (lightCapacity == 4)  ? (1 << 29) : 0;
    prop |= (lightCapacity == 16) ? (1 << 30) : 0;

    return prop;
}

uint32_t ShaderPool::getDepthMeshProperties(bool texture, bool skinning, bool morphTarget, bool morphNormal, bool morphTangent, bool terrain, bool instanced, bool alphaMask, bool instanceFade,
                                            bool normals, bool vertexColorVec3, bool vertexColorVec4, bool uv1, bool textureRect){
    uint32_t prop = 0;

    prop |= texture          ? (1 <<  0) : 0;
    prop |= skinning         ? (1 <<  1) : 0;
    prop |= morphTarget      ? (1 <<  2) : 0;
    prop |= morphNormal      ? (1 <<  3) : 0;
    prop |= morphTangent     ? (1 <<  4) : 0;
    prop |= terrain          ? (1 <<  5) : 0;
    prop |= instanced        ? (1 <<  6) : 0;
    prop |= alphaMask        ? (1 <<  7) : 0;
    prop |= instanceFade     ? (1 <<  8) : 0;
    prop |= normals          ? (1 <<  9) : 0;
    prop |= vertexColorVec3  ? (1 << 10) : 0;
    prop |= vertexColorVec4  ? (1 << 11) : 0;
    prop |= uv1              ? (1 << 12) : 0;
    prop |= textureRect      ? (1 << 13) : 0;

    return prop;
}

uint32_t ShaderPool::getGBufferMeshProperties(bool baseColorTexture, bool normals, bool skinning, bool morphTarget, bool morphNormal, bool morphTangent, bool terrain, bool instanced, bool metallicRoughnessTexture, bool terrainPBR){
    uint32_t prop = 0;

    prop |= baseColorTexture        ? (1 <<  0) : 0;
    prop |= normals                 ? (1 <<  1) : 0;
    prop |= skinning                ? (1 <<  2) : 0;
    prop |= morphTarget             ? (1 <<  3) : 0;
    prop |= morphNormal             ? (1 <<  4) : 0;
    prop |= morphTangent            ? (1 <<  5) : 0;
    prop |= terrain                 ? (1 <<  6) : 0;
    prop |= instanced               ? (1 <<  7) : 0;
    prop |= metallicRoughnessTexture ? (1 << 8) : 0;
    prop |= terrainPBR              ? (1 <<  9) : 0;

    return prop;
}

uint32_t ShaderPool::getUIProperties(bool texture, bool fontAtlasTexture, bool vertexColorVec3, bool vertexColorVec4){
    uint32_t prop = 0;

    prop |= texture          ? (1 <<  0) : 0;
    prop |= fontAtlasTexture ? (1 <<  1) : 0;
    prop |= vertexColorVec3  ? (1 <<  2) : 0;
    prop |= vertexColorVec4  ? (1 <<  3) : 0;

    return prop;
}

uint32_t ShaderPool::getHtmlUIProperties(bool texture){
    uint32_t prop = 0;

    prop |= texture          ? (1 <<  0) : 0;

    return prop;
}

uint32_t ShaderPool::getHtmlUIPostProperties(HtmlUIPostVariant variant){
    return 1u << (uint32_t)variant;
}

std::vector<std::pair<ShaderType, uint32_t>> ShaderPool::getHtmlUIVariants(){
    std::vector<std::pair<ShaderType, uint32_t>> variants;
    variants.push_back({ShaderType::HTMLUI, getHtmlUIProperties(false)});
    variants.push_back({ShaderType::HTMLUI, getHtmlUIProperties(true)});
    variants.push_back({ShaderType::HTMLUI_GRADIENT, 0});
    for (int variant = 0; variant < HTMLUI_POST_VARIANT_COUNT; variant++){
        variants.push_back({ShaderType::HTMLUI_POST, getHtmlUIPostProperties((HtmlUIPostVariant)variant)});
    }
    return variants;
}

uint32_t ShaderPool::getPointsProperties(bool texture, bool vertexColorVec3, bool vertexColorVec4, bool textureRect){
    uint32_t prop = 0;

    prop |= texture          ? (1 <<  0) : 0;
    prop |= vertexColorVec3	 ? (1 <<  1) : 0;
    prop |= vertexColorVec4  ? (1 <<  2) : 0;
    prop |= textureRect      ? (1 <<  3) : 0;

    return prop;
}

uint32_t ShaderPool::getLinesProperties(bool vertexColorVec3, bool vertexColorVec4){
    uint32_t prop = 0;

    prop |= vertexColorVec3	 ? (1 <<  0) : 0;
    prop |= vertexColorVec4  ? (1 <<  1) : 0;

    return prop;
}

void ShaderPool::clear(){
    for (auto& it: getMap()) {
        if (it.second)
            it.second->destroyShader();
    }
    getMap().clear();
    getMissingShaders().clear();
    failedShaders().clear();
    notPrecompiledShaders().clear();
    // stops a compile in flight; what it compiled is in the disk cache for next time
    RuntimeShaderCompiler::shutdown();

    std::lock_guard<std::mutex> lock(customShaderMutex());
    customShaderNames().clear();
    customShaderGeneration++;
}

void ShaderPool::clearUnused(){
    std::vector<ShaderKey> keys;
    keys.reserve(getMap().size());
    for (const auto& entry : getMap()) {
        keys.push_back(entry.first);
    }
    for (ShaderKey key : keys) {
        remove(getShaderTypeFromKey(key), getPropertiesFromKey(key), getCustomIdFromKey(key));
    }
    // Per-project bookkeeping: always reset so the next project does not inherit
    // missing/failed entries or custom-shader name registrations.
    getMissingShaders().clear();
    failedShaders().clear();
    notPrecompiledShaders().clear();
    // its variants are keyed by the custom shader ids reset below
    RuntimeShaderCompiler::shutdown();
    std::lock_guard<std::mutex> lock(customShaderMutex());
    customShaderNames().clear();
    customShaderGeneration++;
}
