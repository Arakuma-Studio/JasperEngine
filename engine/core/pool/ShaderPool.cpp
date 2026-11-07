// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderPool.h"
#include "render/SystemRender.h"

#include "Log.h"
#include "Engine.h"
#include "io/FileData.h"
#include "shader/RuntimeShaderCompiler.h"
#include "shader/ShaderDataSerializer.h"
#include "shader/ShaderVariantCompiler.h"
#include "util/Base64.h"
#include "shader/ShaderManifest.h"
#include "texture/MaterialPbr.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <string>
#include <mutex>
#include <cctype>
#include <cerrno>
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
ShaderBuildsPendingFn ShaderPool::shaderBuildsPendingFn = nullptr;

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
    if (typeToken == "probefilter") {
        shaderType = ShaderType::PROBE_FILTER;
        return true;
    }
    if (typeToken == "fluid") {
        shaderType = ShaderType::FLUID;
        return true;
    }
    if (typeToken == "shadowcache") {
        shaderType = ShaderType::SHADOW_CACHE;
        return true;
    }
    if (typeToken == "atmospheredepth") {
        shaderType = ShaderType::ATMOSPHERE_DEPTH;
        return true;
    }
    if (typeToken == "volumetricfog") {
        shaderType = ShaderType::VOLUMETRIC_FOG;
        return true;
    }
    if (typeToken == "atmospheredenoise") {
        shaderType = ShaderType::ATMOSPHERE_DENOISE;
        return true;
    }
    if (typeToken == "volumetricclouds") {
        shaderType = ShaderType::VOLUMETRIC_CLOUDS;
        return true;
    }
    if (typeToken == "skycomposite") {
        shaderType = ShaderType::SKY_COMPOSITE;
        return true;
    }
    if (typeToken == "volumetriccomposite") {
        shaderType = ShaderType::VOLUMETRIC_COMPOSITE;
        return true;
    }
    if (typeToken == "passpreview") {
        shaderType = ShaderType::PASS_PREVIEW;
        return true;
    }
    if (typeToken == "particles") {
        shaderType = ShaderType::PARTICLES;
        return true;
    }
    if (typeToken == "htmluiprim") {
        shaderType = ShaderType::HTMLUI_PRIM;
        return true;
    }
    if (typeToken == "lightingpass") {
        shaderType = ShaderType::LIGHTING_PASS;
        return true;
    }

    return false;
}

namespace {
    // The map and the key sets below it: the frames use them, and so does a model loading off the
    // main thread (the thumbnail strand, a game's loading thread: RenderSystem::loadMesh under
    // Engine::executeSceneOnce), next to what the main thread lets go outside a frame. Taken around
    // the containers' own operations alone: never while a builder runs or a GPU shader is made or
    // destroyed (TexturePool's entriesMutex, and docs/rendering/async-shaders.md, Threads).
    std::mutex& shaderEntriesMutex(){
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }
}

shaders_t& ShaderPool::getMap(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static shaders_t* map = new shaders_t();
    return *map;
};

static std::vector<std::string>& missingShaderNames(){
    static std::vector<std::string>* missingshaders = new std::vector<std::string>();
    return *missingshaders;
};

std::vector<std::string> ShaderPool::getMissingShaders(){
    std::lock_guard<std::mutex> lock(shaderEntriesMutex());
    return missingShaderNames();
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

static bool isFailedShader(ShaderKey key){
    std::lock_guard<std::mutex> lock(shaderEntriesMutex());
    return failedShaders().count(key) > 0;
}

static void markFailedShader(ShaderKey key){
    std::lock_guard<std::mutex> lock(shaderEntriesMutex());
    failedShaders().insert(key);
}

static void clearFailedShader(ShaderKey key){
    std::lock_guard<std::mutex> lock(shaderEntriesMutex());
    failedShaders().erase(key);
}

static bool isNotPrecompiledShader(ShaderKey key){
    std::lock_guard<std::mutex> lock(shaderEntriesMutex());
    return notPrecompiledShaders().count(key) > 0;
}

static void markNotPrecompiledShader(ShaderKey key){
    std::lock_guard<std::mutex> lock(shaderEntriesMutex());
    notPrecompiledShaders().insert(key);
}

// the entry of the key, if it has one, held: remove() leaves what someone holds
static std::shared_ptr<ShaderRender> findEntry(shaders_t& map, ShaderKey key){
    std::lock_guard<std::mutex> lock(shaderEntriesMutex());
    auto it = map.find(key);
    return it != map.end() ? it->second : nullptr;
}

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
    // a variant of the built-in Lit ("builtin:lit#PBR_CLEARCOAT,...") is "lit_" and two
    // letters per define, short enough for every platform's paths (MaterialPbr)
    if (ShaderReference::getBase(name) == ShaderReference::BUILTIN_LIT)
        return MaterialPbr::getVariantBasename(ShaderReference::getFeatures(name));
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

void ShaderPool::setShaderBuilder(ShaderBuilderFn fn, ShaderBuildsPendingFn pending) {
    shaderBuilderFn = fn;
    shaderBuildsPendingFn = fn ? pending : nullptr;
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
        uint64_t properties = 0;
        if (!splitUiEffectShaderStr(shaderStr.substr(underscorePos + 1), custom, properties))
            return false;
        cliSpec = typeToken + "/" + getUiEffectName(custom);
        std::string props;
        for (int bit = 0; bit < getShaderPropertyCount(shaderType); bit++) {
            if (properties & shaderPropertyBit(bit))
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
        case ShaderType::PROBE_FILTER: return lowerCase ? "probefilter" : "Probe Filter";
        case ShaderType::FLUID:  return lowerCase ? "fluid"  : "Fluid";
        case ShaderType::SHADOW_CACHE: return lowerCase ? "shadowcache" : "Shadow Cache";
        case ShaderType::ATMOSPHERE_DEPTH: return lowerCase ? "atmospheredepth" : "Atmosphere Depth";
        case ShaderType::VOLUMETRIC_FOG: return lowerCase ? "volumetricfog" : "Volumetric Fog";
        case ShaderType::ATMOSPHERE_DENOISE: return lowerCase ? "atmospheredenoise" : "Atmosphere Denoise";
        case ShaderType::VOLUMETRIC_CLOUDS: return lowerCase ? "volumetricclouds" : "Volumetric Clouds";
        case ShaderType::SKY_COMPOSITE: return lowerCase ? "skycomposite" : "Sky Composite";
        case ShaderType::VOLUMETRIC_COMPOSITE: return lowerCase ? "volumetriccomposite" : "Volumetric Composite";
        case ShaderType::PASS_PREVIEW: return lowerCase ? "passpreview" : "Pass Preview";
        case ShaderType::PARTICLES: return lowerCase ? "particles" : "Particles";
        case ShaderType::HTMLUI_PRIM: return lowerCase ? "htmluiprim" : "HTML UI Primitives";
        case ShaderType::LIGHTING_PASS: return lowerCase ? "lightingpass" : "Lighting Pass";
        default:                 return lowerCase ? "unknown": "Unknown";
    }
}

// MESH bit 33 ('Shq'): S10 took bit 5, which upstream gave Stochastic Tiling the same month;
// the upstream keys keep bit 5 and the high filters moved past the Lattice bit
uint64_t ShaderPool::getShadowsHQProperty(){
    return shaderPropertyBit(33);
}

// The Vertex Animation's bits: MESH 34/35 (upstream gave it 33/34, S10's 'Shq' had 33 first),
// DEPTH 16/17, GBUFFER 14/15
uint64_t ShaderPool::getVatProperty(ShaderType shaderType, bool bones){
    switch (shaderType) {
        case ShaderType::MESH:    return shaderPropertyBit(bones ? 35 : 34);
        case ShaderType::DEPTH:   return shaderPropertyBit(bones ? 17 : 16);
        case ShaderType::GBUFFER: return shaderPropertyBit(bones ? 15 : 14);
        default:                  return 0;
    }
}

uint64_t ShaderPool::getAutoInstancingProperty(ShaderType shaderType){
    switch (shaderType) {
        case ShaderType::MESH:  return shaderPropertyBit(41);
        case ShaderType::DEPTH: return shaderPropertyBit(18);
        default:                return 0;
    }
}

uint64_t ShaderPool::getLightingProperty(ShaderPool::LightingFeature feature){
    switch (feature){
        case LightingFeature::TILED_LIGHTS:    return shaderPropertyBit(36);
        case LightingFeature::CAPSULE_SHADOWS: return shaderPropertyBit(37);
        case LightingFeature::CONTACT_SHADOWS: return shaderPropertyBit(38);
        case LightingFeature::OIT:             return shaderPropertyBit(39);
    }
    return 0;
}

// MESH's 'Lfd': the LOD cross-fade's dither (includes/lod_fade.glsl, spec S54 R6), only in the variant
// fading draws use
uint64_t ShaderPool::getLodFadeProperty(){
    return shaderPropertyBit(40);
}

uint64_t ShaderPool::getVatProperties(ShaderType shaderType){
    return getVatProperty(shaderType, false) | getVatProperty(shaderType, true);
}

uint64_t ShaderPool::getPostColorProperty(ShaderType shaderType){
    switch (shaderType) {
        case ShaderType::MESH:   return shaderPropertyBit(31);
        case ShaderType::POINTS: return shaderPropertyBit(4);
        case ShaderType::LINES:  return shaderPropertyBit(2);
        case ShaderType::SKYBOX: return shaderPropertyBit(0);
        case ShaderType::PARTICLES: return shaderPropertyBit(10);
        default:                 return 0;
    }
}

// At most SHADER_PROPERTY_BITS (64): a property past bit 31 is added like any other (a
// case in getShaderPropertyName, the count raised to cover it)
int ShaderPool::getShaderPropertyCount(ShaderType shaderType){
    switch (shaderType) {
        case ShaderType::MESH:   return 45;
        case ShaderType::DEPTH:  return 22;
        case ShaderType::GBUFFER: return 22;
        case ShaderType::UI:     return 5;
        case ShaderType::POINTS: return 5;
        // Vc3 Vc4 Pcl, and the ribbon of thick lines and trails: Rib, Tex (its base texture), Pxw
        case ShaderType::LINES:  return 6;
        case ShaderType::SKYBOX: return 1;
        case ShaderType::SSAO:   return 0;
        case ShaderType::SSAO_BLUR: return 0;
        case ShaderType::SSR:    return 0;
        case ShaderType::SSR_BLUR: return 0;
        case ShaderType::COMPOSITE: return 0;
        case ShaderType::SHADOW2D: return 0;
        case ShaderType::BLIT:   return 18;
        case ShaderType::BLOOM:  return 4;
        case ShaderType::HTMLUI: return 1;
        case ShaderType::HTMLUI_GRADIENT: return 0;
        case ShaderType::HTMLUI_POST: return 7;
        // Flt (filter form), Stp and Sct (phase 6: a stateful effect's step and scatter forms)
        case ShaderType::UI_EFFECT: return 3;
        case ShaderType::PROBE_FILTER: return 0;
        // Lok (the look program), Mfl (the solver with manual filtering)
        case ShaderType::FLUID:  return 2;
        case ShaderType::SHADOW_CACHE: return 1;
        // the source's texture type: 2D (none), array, cube or 3D
        case ShaderType::PASS_PREVIEW: return 3;
        // ShaderPool::getParticlesProperties
        case ShaderType::PARTICLES: return 11;
        case ShaderType::HTMLUI_PRIM: return 0;
        // 'Oir': the OIT resolve, else the contact shadows' mask
        case ShaderType::LIGHTING_PASS: return 1;
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
            // Stochastic Tiling of the material's maps (USE_STOCHASTIC_TILING). Until October
            // 2026 bit 5 was reserved, the retired 'Pcf' (July 2026): keys files hold from
            // before have it stripped as they are read (editor::Stream::decodeShaderKey)
            case 5:  return shortName ? "Sto" : "Stochastic Tiling";
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
            // the lattice deformer (HAS_LATTICE, includes/lattice.glsl): the first property past
            // the 32 of older keys, so keys without a lattice read as they always did
            case 32: return shortName ? "Ltc" : "Lattice";
            // the High and Ultra shadow filters (USE_SHADOWS_HQ, S10): set by the shadow tier on
            // variants that receive shadows (getShadowsHQProperty)
            case 33: return shortName ? "Shq" : "Shadows HQ";
            // a Vertex Animator's baked animation (includes/vat.glsl): Vertices mode
            // (HAS_VAT) or Bones mode (HAS_VAT and HAS_VAT_BONES), never both; either one
            // replaces the skinning and the morph targets (getVatProperty: upstream's bits 33
            // and 34, after S10's 'Shq')
            case 34: return shortName ? "Vat" : "Vertex Animation";
            case 35: return shortName ? "Vab" : "Vertex Animation Bones";
            // S57's lighting (getLightingProperty): the pixel lights from screen tiles
            // (USE_TILED_LIGHTS), capsule and contact shadows of the shadowed lights
            // (USE_CAPSULE_SHADOWS, USE_CONTACT_SHADOWS) and the OIT accumulation (USE_OIT)
            case 36: return shortName ? "Til" : "Tiled Lights";
            case 37: return shortName ? "Cap" : "Capsule Shadows";
            case 38: return shortName ? "Cts" : "Contact Shadows";
            case 39: return shortName ? "Oit" : "Order Independent Transparency";
            // the LOD cross-fade's dither, for fading draws only (getLodFadeProperty)
            case 40: return shortName ? "Lfd" : "Lod Fade";
            // automatic GPU instancing (HAS_AUTO_INSTANCING, includes/object_params.glsl, S55):
            // the object block per instance (getAutoInstancingProperty)
            case 41: return shortName ? "Aui" : "Auto Instancing";
            // foliage (T5 F3, F6, F8): the 32-byte instance, the wind block, the fade by dither
            case 42: return shortName ? "Cin" : "Compact Instance";
            case 43: return shortName ? "Wnd" : "Foliage Wind";
            case 44: return shortName ? "Dth" : "Foliage Dither";
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
            // the alpha clip reads the base colour as the colour pass's Stochastic Tiling does
            case 14: return shortName ? "Sto" : "Stochastic Tiling";
            // the lattice deformer (HAS_LATTICE, includes/lattice.glsl)
            case 15: return shortName ? "Ltc" : "Lattice";
            // a Vertex Animator's baked animation (includes/vat.glsl), as MESH 33/34
            case 16: return shortName ? "Vat" : "Vertex Animation";
            case 17: return shortName ? "Vab" : "Vertex Animation Bones";
            // automatic GPU instancing (includes/object_depth_params.glsl, S55)
            case 18: return shortName ? "Aui" : "Auto Instancing";
            case 19: return shortName ? "Cin" : "Compact Instance";
            case 20: return shortName ? "Wnd" : "Foliage Wind";
            case 21: return shortName ? "Dth" : "Foliage Dither";
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
            // base colour and metallic-roughness read as the colour pass's Stochastic Tiling does
            case 10: return shortName ? "Sto" : "Stochastic Tiling";
            // the lattice deformer (HAS_LATTICE, includes/lattice.glsl)
            case 11: return shortName ? "Ltc" : "Lattice";
            // the vertex colour a lattice's mask reads, never set without 'Ltc'
            case 12: return shortName ? "Vc3" : "Vertex Color 3";
            case 13: return shortName ? "Vc4" : "Vertex Color 4";
            // a Vertex Animator's baked animation (includes/vat.glsl), as MESH 33/34
            case 14: return shortName ? "Vat" : "Vertex Animation";
            case 15: return shortName ? "Vab" : "Vertex Animation Bones";
            // the material's occlusion map, which the SSR composite weighs its swap by (color[3])
            case 16: return shortName ? "Occ" : "Occlusion Texture";
            // a map read on the second UV set, as the material names it (u_fs_gbufferMaterial.texCoordSets)
            case 17: return shortName ? "Uv2" : "UV Set 2";
            // the Instanced Mesh distance fade (includes/instance_fade.glsl), as MESH 26 and DEPTH 8
            case 18: return shortName ? "Ifd" : "Instance Fade";
            case 19: return shortName ? "Cin" : "Compact Instance";
            case 20: return shortName ? "Wnd" : "Foliage Wind";
            case 21: return shortName ? "Dth" : "Foliage Dither";
        }
    } else if (shaderType == ShaderType::UI) {
        switch (bit) {
            case 0: return shortName ? "Tex" : "Texture";
            case 1: return shortName ? "Ftx" : "Font Texture";
            case 2: return shortName ? "Vc3" : "Vertex Color 3";
            case 3: return shortName ? "Vc4" : "Vertex Color 4";
            // a Text with outline or shadow: signed-distance glyphs
            case 4: return shortName ? "Sdf" : "Font SDF";
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
    } else if (shaderType == ShaderType::SHADOW_CACHE) {
        switch (bit) {
            case 0: return shortName ? "Vis" : "Atlas View";
        }
    } else if (shaderType == ShaderType::LIGHTING_PASS) {
        switch (bit) {
            case 0: return shortName ? "Oir" : "OIT Resolve";
        }
    } else if (shaderType == ShaderType::PASS_PREVIEW) {
        switch (bit) {
            case 0: return shortName ? "Arr" : "Array Source";
            case 1: return shortName ? "Cub" : "Cube Source";
            case 2: return shortName ? "Tx3" : "3D Source";
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
    } else if (shaderType == ShaderType::PARTICLES) {
        // what the effect's modules draw (getParticlesProperties)
        switch (bit) {
            case 0: return shortName ? "Tex" : "Texture";
            case 1: return shortName ? "Msh" : "Mesh";
            case 2: return shortName ? "Trl" : "Trail";
            case 3: return shortName ? "Str" : "Stretch";
            case 4: return shortName ? "Fbl" : "Frame Blend";
            case 5: return shortName ? "Lit" : "Lit";
            case 6: return shortName ? "Sxw" : "Six-Way";
            case 7: return shortName ? "Cus" : "Custom Data";
            case 8: return shortName ? "Rsh" : "Receive Shadows";
            case 9: return "Fog";
            case 10: return shortName ? "Pcl" : "Post Color";
        }
    } else if (shaderType == ShaderType::LINES) {
        switch (bit) {
            case 0: return shortName ? "Vc3" : "Vertex Color 3";
            case 1: return shortName ? "Vc4" : "Vertex Color 4";
            case 2: return shortName ? "Pcl" : "Post Color";
            case 3: return shortName ? "Rib" : "Ribbon";
            case 4: return shortName ? "Tex" : "Texture";
            case 5: return shortName ? "Pxw" : "Pixel Width";
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
            case 13: return shortName ? "Vhs" : "VHS";
            case 14: return shortName ? "Hal" : "Halation";
            case 15: return shortName ? "Fdm" : "Film Damage";
            case 16: return shortName ? "Gln" : "Film Grain Lens";
            case 17: return shortName ? "Gfm" : "Film Grain Filmic";
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

std::string ShaderPool::getShaderStr(ShaderType shaderType, uint64_t properties, uint16_t customId){

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
        if (properties & shaderPropertyBit(i))
            propOut += getShaderPropertyName(shaderType, i, true);
    }

    if (!propOut.empty())
        str += "_" + propOut;

    return str;
}

ShaderKey ShaderPool::getShaderKey(ShaderType shaderType, uint64_t properties, uint16_t customId) {
    return ShaderKey(static_cast<uint16_t>(shaderType), properties, customId);
}

ShaderKey ShaderPool::unpackLegacyKey(uint64_t packed) {
    return ShaderKey(static_cast<uint16_t>((packed >> 32) & 0xFFFF), packed & 0xFFFFFFFFULL, static_cast<uint16_t>((packed >> 48) & 0xFFFF));
}

std::string ShaderPool::getShaderKeyText(ShaderKey key) {
    static const char digits[] = "0123456789abcdef";
    auto hex = [](uint64_t value, int width){
        std::string text(width, '0');
        for (int i = width - 1; i >= 0; i--, value >>= 4)
            text[i] = digits[value & 0xF];
        return text;
    };
    if (key.properties <= 0xFFFFFFFFULL) {
        const uint64_t packed = ((uint64_t)key.customId << 48) | ((uint64_t)key.type << 32) | key.properties;
        std::string text = hex(packed, 16);
        const size_t first = text.find_first_not_of('0');
        return "0x" + (first == std::string::npos ? std::string("0") : text.substr(first));
    }
    return "0x" + hex(key.customId, 4) + hex(key.type, 4) + hex(key.properties, 16);
}

bool ShaderPool::parseShaderKeyText(const std::string& text, ShaderKey& key) {
    if (text.empty() || !std::isdigit(static_cast<unsigned char>(text[0])))
        return false;
    const bool hexText = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    if (hexText && text.size() - 2 > 16) {
        // the wide form: customId (4), type (4), properties (16)
        if (text.size() - 2 != 24)
            return false;
        uint64_t parts[3] = {0, 0, 0};
        const int widths[3] = {4, 4, 16};
        size_t at = 2;
        for (int part = 0; part < 3; part++) {
            for (int i = 0; i < widths[part]; i++, at++) {
                const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[at])));
                int digit;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else return false;
                parts[part] = (parts[part] << 4) | (uint64_t)digit;
            }
        }
        key = ShaderKey(static_cast<uint16_t>(parts[1]), parts[2], static_cast<uint16_t>(parts[0]));
        return true;
    }
    // the packed form (hex, or decimal as numbers were stored)
    errno = 0;
    char* end = nullptr;
    const unsigned long long packed = std::strtoull(text.c_str(), &end, 0);
    if (errno != 0 || !end || *end != '\0')
        return false;
    key = unpackLegacyKey(packed);
    return true;
}

void ShaderPool::addMissingShader(const std::string& shaderStr) {
    std::lock_guard<std::mutex> lock(shaderEntriesMutex());
    auto& missingShaders = missingShaderNames();
    if (std::find(missingShaders.begin(), missingShaders.end(), shaderStr) == missingShaders.end()) {
        missingShaders.push_back(shaderStr);
    }
}

std::shared_ptr<ShaderRender> ShaderPool::get(ShaderType shaderType, uint64_t properties, uint16_t customId){
    ShaderKey shaderKey = getShaderKey(shaderType, properties, customId);
    // its entry, held: the lock is not kept while the variant is looked at or made, and remove()
    // leaves what someone holds
    std::shared_ptr<ShaderRender> shared;
    {
        std::lock_guard<std::mutex> lock(shaderEntriesMutex());
        std::shared_ptr<ShaderRender>& entry = getMap()[shaderKey];
        if (!entry) {
            entry = std::make_shared<ShaderRender>();
        }
        shared = entry;
    }

    if (shared->isCreated()){
        return shared;
    }
    // its GPU object is being created off the render thread (spec S32): nothing to do until then
    if (shared->isPending()){
        return shared;
    }

    // The build can succeed and the backend still reject the shader (Metal compiles
    // the native library here). Retire the handle so its pool slot is released.
    if (shared->isFailed()) {
        shared->destroyShader();
        markFailedShader(shaderKey);
        Log::error("Shader resource creation failed: %s",
                   getShaderStr(shaderType, properties, customId).c_str());
        return shared;
    }

    if (!shared->isCreated()) {
        // A previous attempt failed: don't retry every frame (it recompiles and spams
        // errors). Retried once remove()/destroyCustomShaders() clears the failure.
        if (isFailedShader(shaderKey)) {
            return shared;
        }
        if (shaderBuilderFn) {
            static const bool timing = std::getenv("JASPER_SHADER_TIMING") != nullptr;
            const auto t0 = std::chrono::steady_clock::now();
            ShaderBuildResult result = shaderBuilderFn(shaderKey);
            const auto t1 = std::chrono::steady_clock::now();
            if (timing && std::chrono::duration<double, std::milli>(t1 - t0).count() > 5.0)
                Log::print("[shader-timing] builder %s: %.1f ms (state %d)", getShaderStr(shaderType, properties, customId).c_str(),
                          std::chrono::duration<double, std::milli>(t1 - t0).count(), (int)result.state);
            if (result.state == ResourceLoadState::Finished) {
                const auto c0 = std::chrono::steady_clock::now();
                shared->createShader(result.data);
                if (timing)
                    Log::print("[shader-timing] create %s: %.1f ms", getShaderStr(shaderType, properties, customId).c_str(),
                              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count());
                clearFailedShader(shaderKey);
            } else if (result.state == ResourceLoadState::Loading) {
                // Shader is still building, do nothing
            } else if (result.state == ResourceLoadState::Failed) {
                markFailedShader(shaderKey);
                Log::error("Shader build failed: %s", getShaderStr(shaderType, properties, customId).c_str());
            }
        } else {
            const std::string shaderStr = getShaderStr(shaderType, properties, customId);

            ShaderData precompiled;
            if (!isNotPrecompiledShader(shaderKey) && loadPrecompiledShader(shaderKey, shaderStr, precompiled)){
                shared->createShader(precompiled);
            } else {
                // looked for once: what the build ships does not change
                markNotPrecompiledShader(shaderKey);

                if (RuntimeShaderCompiler::isEnabled()){
                    // Built in the background: Loading leaves whatever draws with it out
                    // until it is done, and the next get() creates it
                    ShaderBuildResult result = RuntimeShaderCompiler::request(shaderKey);
                    if (result.state == ResourceLoadState::Finished){
                        shared->createShader(result.data);
                    } else if (result.state == ResourceLoadState::Failed){
                        markFailedShader(shaderKey);
                        Log::error("Shader %s (%s) failed to compile at runtime: %s",
                                   shaderStr.c_str(), getShaderLangStr().c_str(),
                                   RuntimeShaderCompiler::getError(shaderKey).c_str());
                    }
                } else {
                    // A build whose variant set came from stale scene keys can lack this
                    // one: the nearest variant it ships draws instead, with the optional
                    // features it lacks left out (getFallbackProperties)
                    bool fallback = false;
                    for (uint64_t candidate : getFallbackProperties(shaderType, properties)){
                        const ShaderKey candidateKey = getShaderKey(shaderType, candidate, customId);
                        const std::string candidateStr = getShaderStr(shaderType, candidate, customId);
                        ShaderData candidateData;
                        if (loadPrecompiledShader(candidateKey, candidateStr, candidateData)){
                            Log::warn("Shader %s (%s) is not in this build, using %s",
                                      shaderStr.c_str(), getShaderLangStr().c_str(), candidateStr.c_str());
                            shared->createShader(candidateData);
                            fallback = true;
                            break;
                        }
                    }
                    if (!fallback){
                        // Not in this build: reported once (RenderSystem::draw), and not looked
                        // for again every frame by whatever needs it
                        addMissingShader(shaderStr);
                        markFailedShader(shaderKey);
                    }
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

std::vector<uint64_t> ShaderPool::getFallbackProperties(ShaderType shaderType, uint64_t properties){
    // The optional features, in the order they are given up. Only features whose loss keeps
    // every uniform block the draw uploads by the key's own bits: their resources are blocks,
    // textures and attributes of their own, which the renderer binds by the variant's
    // reflection (absent: skipped), and u_fs_litFrame is uploaded at its reflected size, a
    // prefix (includes/lit_frame.glsl). Never the lighting model or the per-object lights
    // block (Puc, Vtl, Lc4, L16), the u_fs_pbrParams layout (Ult, Ams, Sto), the output
    // encoding and litFrame prefix of the post colour (Pcl), nor the geometry (Ski, Mta,
    // Ter, Ist, Txr...).
    static const int meshBits[] = {21, 12, 23, 4, 20, 19, 7, 8, 2, 1}; // Sao Fog S2d Shw Mir Ibl Nmp Tan Uv2 Uv1
    static const int depthBits[] = {7, 0};                               // Ams Tex
    static const int gbufferBits[] = {16, 17, 8, 0};                     // Occ Uv2 Mrt Bct

    const int* bits = nullptr;
    size_t bitCount = 0;
    if (shaderType == ShaderType::MESH){
        bits = meshBits;
        bitCount = sizeof(meshBits) / sizeof(meshBits[0]);
    }else if (shaderType == ShaderType::DEPTH){
        bits = depthBits;
        bitCount = sizeof(depthBits) / sizeof(depthBits[0]);
    }else if (shaderType == ShaderType::GBUFFER){
        bits = gbufferBits;
        bitCount = sizeof(gbufferBits) / sizeof(gbufferBits[0]);
    }

    std::vector<uint64_t> present;
    for (size_t i = 0; i < bitCount; i++){
        if (properties & shaderPropertyBit(bits[i]))
            present.push_back(shaderPropertyBit(bits[i]));
    }

    // fewest features dropped first; among as many, the earlier ones in the order above
    std::vector<uint64_t> candidates;
    const uint32_t combinations = 1u << present.size();
    for (size_t dropped = 1; dropped <= present.size(); dropped++){
        for (uint32_t combination = 1; combination < combinations; combination++){
            size_t count = 0;
            uint64_t candidate = properties;
            for (size_t i = 0; i < present.size(); i++){
                if (combination & (1u << i)){
                    candidate &= ~present[i];
                    count++;
                }
            }
            if (count == dropped)
                candidates.push_back(candidate);
        }
    }
    return candidates;
}

bool ShaderPool::warmUp(ShaderType shaderType, uint64_t properties, uint16_t customId){
    std::shared_ptr<ShaderRender> shader = get(shaderType, properties, customId);
    return shader && shader->isCreated();
}

bool ShaderPool::warmUp(ShaderKey shaderKey){
    return warmUp(getShaderTypeFromKey(shaderKey), getPropertiesFromKey(shaderKey), getCustomIdFromKey(shaderKey));
}

bool ShaderPool::isShaderReady(ShaderType shaderType, uint64_t properties, uint16_t customId){
    const std::shared_ptr<ShaderRender> entry = findEntry(getMap(), getShaderKey(shaderType, properties, customId));
    return entry && entry->isCreated();
}

ResourceLoadState ShaderPool::getShaderState(ShaderKey shaderKey){
    if (isFailedShader(shaderKey))
        return ResourceLoadState::Failed;
    const std::shared_ptr<ShaderRender> entry = findEntry(getMap(), shaderKey);
    if (!entry)
        return ResourceLoadState::NotStarted;
    if (entry->isCreated())
        return ResourceLoadState::Finished;
    if (entry->isFailed())
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
        if (!warmUp(key) && !isFailedShader(key))
            notReady++;
    }
    return notReady;
}

bool ShaderPool::findKeyOfHandle(uint32_t handleId, ShaderKey& key){
    if (handleId == 0)
        return false;
    std::lock_guard<std::mutex> lock(shaderEntriesMutex());
    for (const auto& entry : getMap()){
        if (entry.second && entry.second->backend.get().id == handleId){
            key = entry.first;
            return true;
        }
    }
    return false;
}

std::string ShaderPool::getEmbeddedData(const std::string& name){
    const std::string base64 = getBase64Shader(name);
    if (base64.empty())
        return std::string();
    const std::vector<unsigned char> bytes = Base64::decode(base64);
    return std::string(bytes.begin(), bytes.end());
}

size_t ShaderPool::getPendingShaderCount(){
    // compiling at runtime, and GPU objects being created (spec S32)
    return RuntimeShaderCompiler::getPendingCount() + SystemRender::getGpuCreationStats().pendingShaders;
}

size_t ShaderPool::getPendingPipelineCount(){
    return SystemRender::getGpuCreationStats().pendingPipelines;
}

bool ShaderPool::isGpuCreationIdle(){
    return SystemRender::isGpuCreationIdle();
}

bool ShaderPool::isCreationIdle(){
    return getPendingShaderCount() == 0 && (!shaderBuildsPendingFn || shaderBuildsPendingFn() == 0) && SystemRender::isGpuCreationIdle();
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

bool parseSpecUInt64(const std::string& value, uint64_t& out) {
    if (value.empty() || value[0] == '+' || value[0] == '-') {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value.c_str(), &end, 0);
    if (!end || *end != '\0' || errno == ERANGE) {
        return false;
    }

    out = static_cast<uint64_t>(parsed);
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

    uint64_t properties = 0;
    if (!propsPart.empty()) {
        const bool numericMask = std::isdigit(static_cast<unsigned char>(propsPart[0]));

        if (numericMask) {
            if (!parseSpecUInt64(propsPart, properties)) {
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
                        properties |= shaderPropertyBit(bit);
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
        auto addForms = [&](uint64_t drawForm){
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

bool ShaderPool::splitUiEffectShaderStr(const std::string& basenameAndProperties, std::string& customShader, uint64_t& properties){
    // "<basename>[_<Flt|Stp|Sct, concatenated>]": the property names are capitalised, effect
    // basenames are not, and the longest suffix that leaves a known effect wins
    static const struct { const char* suffix; uint64_t properties; } suffixes[] = {
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
        uint64_t properties = 0;
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
    // the built-in Lit's variants map back without a resolver too
    std::vector<std::string> litDefines;
    if (MaterialPbr::parseVariantBasename(basename, litDefines))
        return ShaderReference::withFeatures(ShaderReference::BUILTIN_LIT, litDefines);
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

bool ShaderPool::isShaderBuildFailed(ShaderType shaderType, uint64_t properties, uint16_t customId){
    return isFailedShader(getShaderKey(shaderType, properties, customId));
}

void ShaderPool::remove(ShaderType shaderType, uint64_t properties, uint16_t customId){
    ShaderKey shaderKey = getShaderKey(shaderType, properties, customId);
    bool found = false;
    std::shared_ptr<ShaderRender> unused;
    {
        std::lock_guard<std::mutex> lock(shaderEntriesMutex());
        failedShaders().erase(shaderKey);
        auto& map = getMap();
        auto it = map.find(shaderKey);
        if (it != map.end()){
            found = true;
            if (!it->second || it->second.use_count() <= 1){
                unused = std::move(it->second);
                map.erase(it);
            }
        }
    }
    if (unused){
        unused->destroyShader();
    }
    if (!found && Engine::isViewLoaded()){
        Log::debug("Trying to destroy a non existent shader");
    }
}

void ShaderPool::destroyCustomShaders(){
    destroyCustomShaders([](const std::string&){ return true; });
}

void ShaderPool::destroyCustomShaders(const std::function<bool(const std::string& name)>& which){
    // library shaders ("library:<name>") and the library UI effects ("@effects/<name>") are
    // embedded in the engine: their sources never change with the project
    auto isFork = [&which](ShaderKey key){
        const uint16_t customId = getCustomIdFromKey(key);
        if (customId == 0)
            return false;
        const std::string name = getCustomShaderName(customId);
        return name.compare(0, 8, "library:") != 0 && !isLibraryCustomShader(customId) && which(name);
    };
    // (what `which` asks and the GPU shaders' end are outside the lock: copies of the entries)
    std::vector<std::pair<ShaderKey, std::shared_ptr<ShaderRender>>> entries;
    std::vector<ShaderKey> failed;
    {
        std::lock_guard<std::mutex> lock(shaderEntriesMutex());
        entries.assign(getMap().begin(), getMap().end());
        failed.assign(failedShaders().begin(), failedShaders().end());
    }
    for (auto& entry : entries){
        // being made as well (queued off the main thread, or created off the render thread): what
        // would come of it is the old source. A queued make finds its handle gone and makes nothing
        if (isFork(entry.first) && entry.second &&
                (entry.second->isCreated() || entry.second->isPending() || entry.second->loading.load())){
            entry.second->destroyShader();
        }
    }
    // Forget custom-shader build failures so an edited source is retried on the next get().
    for (ShaderKey key : failed){
        if (isFork(key)){
            clearFailedShader(key);
        }
    }
}

ShaderType ShaderPool::getShaderTypeFromKey(ShaderKey key) {
    return (ShaderType)key.type;
}

uint64_t ShaderPool::getPropertiesFromKey(ShaderKey key) {
    return key.properties;
}

uint16_t ShaderPool::getCustomIdFromKey(ShaderKey key) {
    return key.customId;
}

ShaderKey ShaderPool::getStorageKey(ShaderKey key) {
    // Strip the session-local customShaderId; keep type + properties.
    key.customId = 0;
    return key;
}

uint64_t ShaderPool::getValidPropertyMask(ShaderType shaderType) {
    // Derived from the property-name table so a retired bit is excluded automatically: it
    // has no short name ("?"). (MESH bit 5, the old 'Pcf', is Stochastic Tiling again: the
    // stale ones of old files are stripped where those are read, editor::Stream.)
    uint64_t mask = 0;
    const int propCount = std::min(getShaderPropertyCount(shaderType), SHADER_PROPERTY_BITS);
    for (int i = 0; i < propCount; i++) {
        if (getShaderPropertyName(shaderType, i, true) != "?")
            mask |= shaderPropertyBit(i);
    }
    return mask;
}

ShaderKey ShaderPool::normalizeKey(ShaderKey key) {
    ShaderType type = getShaderTypeFromKey(key);
    key.properties &= getValidPropertyMask(type);
    // the high-quality shadow filters only exist in a variant that receives shadows
    if (type == ShaderType::MESH && !(key.properties & shaderPropertyBit(4)))
        key.properties &= ~getShadowsHQProperty();
    // the built-in Lit's unlit 2D light variant took 'Tan' without 'Nmp' until November 2026;
    // the 2D lights read tangents only for a normal map. A shader with a custom id keeps it: a
    // library shader may @require tangents on that path
    if (type == ShaderType::MESH && key.customId == 0 && (key.properties & shaderPropertyBit(0)) &&
            (key.properties & shaderPropertyBit(22)) && !(key.properties & shaderPropertyBit(7)))
        key.properties &= ~shaderPropertyBit(8);
    return key;
}

uint64_t ShaderPool::getMeshProperties(
    bool unlit, bool uv1, bool uv2,
    bool punctual, bool shadows, bool normals, bool normalMap,
    bool tangents, bool vertexColorVec3, bool vertexColorVec4, bool textureRect,
    bool fog, bool skinning, bool morphTarget, bool morphNormal, bool morphTangent,
    bool terrain, bool instanced, bool ibl, bool mirror, bool ssao, bool light2d, bool shadows2d,
    bool alphaMask, bool alphaOpaque, bool instanceFade, bool terrainPBR,
    bool vertexLights, int lightCapacity, bool stochasticTiling, bool lattice, bool vat, bool vatBones){
    uint64_t prop = 0;

    prop |= unlit            ? shaderPropertyBit(0) : 0;
    prop |= uv1              ? shaderPropertyBit(1) : 0;
    prop |= uv2              ? shaderPropertyBit(2) : 0;
    prop |= punctual         ? shaderPropertyBit(3) : 0;
    prop |= shadows          ? shaderPropertyBit(4) : 0;
    // bit 5 was USE_SHADOWS_PCF until July 2026 (the PCF filter is uniform-driven now); since
    // October 2026 it is Stochastic Tiling (see getShaderPropertyName). Bit 33 ('Shq',
    // USE_SHADOWS_HQ) is set after, by the render system's shadow tier (getShadowsHQProperty)
    prop |= stochasticTiling ? shaderPropertyBit(5) : 0;
    prop |= normals          ? shaderPropertyBit(6) : 0;
    prop |= normalMap        ? shaderPropertyBit(7) : 0;
    prop |= tangents         ? shaderPropertyBit(8) : 0;
    prop |= vertexColorVec3  ? shaderPropertyBit(9) : 0;
    prop |= vertexColorVec4  ? shaderPropertyBit(10) : 0;
    prop |= textureRect      ? shaderPropertyBit(11) : 0;
    prop |= fog              ? shaderPropertyBit(12) : 0;
    prop |= skinning         ? shaderPropertyBit(13) : 0;
    prop |= morphTarget      ? shaderPropertyBit(14) : 0;
    prop |= morphNormal      ? shaderPropertyBit(15) : 0;
    prop |= morphTangent     ? shaderPropertyBit(16) : 0;
    prop |= terrain          ? shaderPropertyBit(17) : 0;
    prop |= instanced        ? shaderPropertyBit(18) : 0;
    prop |= ibl              ? shaderPropertyBit(19) : 0;
    prop |= mirror           ? shaderPropertyBit(20) : 0;
    prop |= ssao             ? shaderPropertyBit(21) : 0;
    prop |= light2d          ? shaderPropertyBit(22) : 0;
    prop |= shadows2d        ? shaderPropertyBit(23) : 0;
    prop |= alphaMask        ? shaderPropertyBit(24) : 0;
    prop |= alphaOpaque      ? shaderPropertyBit(25) : 0;
    prop |= instanceFade     ? shaderPropertyBit(26) : 0;
    prop |= terrainPBR       ? shaderPropertyBit(27) : 0;
    prop |= vertexLights     ? shaderPropertyBit(28) : 0;
    // the default capacity (8) sets neither bit, so it keeps the keys it had before tiers
    prop |= (lightCapacity == 4)  ? shaderPropertyBit(29) : 0;
    prop |= (lightCapacity == 16) ? shaderPropertyBit(30) : 0;
    prop |= lattice          ? shaderPropertyBit(32) : 0;
    // one of the two: Bones mode is 'Vab' alone
    prop |= (vat && !vatBones) ? getVatProperty(ShaderType::MESH, false) : 0;
    prop |= vatBones         ? getVatProperty(ShaderType::MESH, true) : 0;

    return prop;
}

uint64_t ShaderPool::getFoliageProperties(ShaderType shaderType, bool compact, bool wind, bool dither){
    // appended after each type's last bit: MESH 42-44, DEPTH and GBUFFER 19-21
    int first = 42;
    if (shaderType == ShaderType::DEPTH || shaderType == ShaderType::GBUFFER) first = 19;
    uint64_t prop = 0;
    prop |= compact ? shaderPropertyBit(first) : 0;
    prop |= wind    ? shaderPropertyBit(first + 1) : 0;
    prop |= dither  ? shaderPropertyBit(first + 2) : 0;
    return prop;
}

uint64_t ShaderPool::getDepthMeshProperties(bool texture, bool skinning, bool morphTarget, bool morphNormal, bool morphTangent, bool terrain, bool instanced, bool alphaMask, bool instanceFade,
                                            bool normals, bool vertexColorVec3, bool vertexColorVec4, bool uv1, bool textureRect,
                                            bool stochasticTiling, bool lattice, bool vat, bool vatBones){
    uint64_t prop = 0;

    prop |= texture          ? shaderPropertyBit(0) : 0;
    prop |= skinning         ? shaderPropertyBit(1) : 0;
    prop |= morphTarget      ? shaderPropertyBit(2) : 0;
    prop |= morphNormal      ? shaderPropertyBit(3) : 0;
    prop |= morphTangent     ? shaderPropertyBit(4) : 0;
    prop |= terrain          ? shaderPropertyBit(5) : 0;
    prop |= instanced        ? shaderPropertyBit(6) : 0;
    prop |= alphaMask        ? shaderPropertyBit(7) : 0;
    prop |= instanceFade     ? shaderPropertyBit(8) : 0;
    prop |= normals          ? shaderPropertyBit(9) : 0;
    prop |= vertexColorVec3  ? shaderPropertyBit(10) : 0;
    prop |= vertexColorVec4  ? shaderPropertyBit(11) : 0;
    prop |= uv1              ? shaderPropertyBit(12) : 0;
    prop |= textureRect      ? shaderPropertyBit(13) : 0;
    prop |= stochasticTiling ? shaderPropertyBit(14) : 0;
    prop |= lattice          ? shaderPropertyBit(15) : 0;
    prop |= (vat && !vatBones) ? shaderPropertyBit(16) : 0;
    prop |= vatBones         ? shaderPropertyBit(17) : 0;

    return prop;
}

uint64_t ShaderPool::getGBufferMeshProperties(bool baseColorTexture, bool normals, bool skinning, bool morphTarget, bool morphNormal, bool morphTangent, bool terrain, bool instanced, bool metallicRoughnessTexture, bool terrainPBR,
                                              bool stochasticTiling, bool lattice, bool vertexColorVec3, bool vertexColorVec4,
                                              bool vat, bool vatBones, bool occlusionTexture, bool uvSet2, bool instanceFade){
    uint64_t prop = 0;

    prop |= baseColorTexture        ? shaderPropertyBit(0) : 0;
    prop |= normals                 ? shaderPropertyBit(1) : 0;
    prop |= skinning                ? shaderPropertyBit(2) : 0;
    prop |= morphTarget             ? shaderPropertyBit(3) : 0;
    prop |= morphNormal             ? shaderPropertyBit(4) : 0;
    prop |= morphTangent            ? shaderPropertyBit(5) : 0;
    prop |= terrain                 ? shaderPropertyBit(6) : 0;
    prop |= instanced               ? shaderPropertyBit(7) : 0;
    prop |= metallicRoughnessTexture ? shaderPropertyBit(8) : 0;
    prop |= terrainPBR              ? shaderPropertyBit(9) : 0;
    prop |= stochasticTiling        ? shaderPropertyBit(10) : 0;
    prop |= lattice                 ? shaderPropertyBit(11) : 0;
    prop |= (lattice && vertexColorVec3) ? shaderPropertyBit(12) : 0;
    prop |= (lattice && vertexColorVec4) ? shaderPropertyBit(13) : 0;
    prop |= (vat && !vatBones)      ? shaderPropertyBit(14) : 0;
    prop |= vatBones                ? shaderPropertyBit(15) : 0;
    prop |= occlusionTexture        ? shaderPropertyBit(16) : 0;
    prop |= uvSet2                  ? shaderPropertyBit(17) : 0;
    prop |= instanceFade            ? shaderPropertyBit(18) : 0;

    return prop;
}

uint64_t ShaderPool::getUIProperties(bool texture, bool fontAtlasTexture, bool vertexColorVec3, bool vertexColorVec4, bool fontAtlasSdf){
    uint64_t prop = 0;

    prop |= texture          ? shaderPropertyBit(0) : 0;
    prop |= fontAtlasTexture ? shaderPropertyBit(1) : 0;
    prop |= vertexColorVec3  ? shaderPropertyBit(2) : 0;
    prop |= vertexColorVec4  ? shaderPropertyBit(3) : 0;
    prop |= (fontAtlasTexture && fontAtlasSdf) ? shaderPropertyBit(4) : 0;

    return prop;
}

uint64_t ShaderPool::getHtmlUIProperties(bool texture){
    uint64_t prop = 0;

    prop |= texture          ? shaderPropertyBit(0) : 0;

    return prop;
}

uint64_t ShaderPool::getHtmlUIPostProperties(HtmlUIPostVariant variant){
    return shaderPropertyBit(static_cast<int>(variant));
}

std::vector<std::pair<ShaderType, uint64_t>> ShaderPool::getHtmlUIVariants(){
    std::vector<std::pair<ShaderType, uint64_t>> variants;
    variants.push_back({ShaderType::HTMLUI, getHtmlUIProperties(false)});
    variants.push_back({ShaderType::HTMLUI, getHtmlUIProperties(true)});
    variants.push_back({ShaderType::HTMLUI_GRADIENT, 0});
    variants.push_back({ShaderType::HTMLUI_PRIM, 0});
    for (int variant = 0; variant < HTMLUI_POST_VARIANT_COUNT; variant++){
        variants.push_back({ShaderType::HTMLUI_POST, getHtmlUIPostProperties((HtmlUIPostVariant)variant)});
    }
    return variants;
}

std::vector<std::pair<ShaderType, uint64_t>> ShaderPool::getVideoPlayerVariants(){
    return {{ShaderType::HTMLUI_POST, getHtmlUIPostProperties(HtmlUIPostVariant::YUV)},
            {ShaderType::HTMLUI_POST, getHtmlUIPostProperties(HtmlUIPostVariant::PASSTHROUGH)}};
}

uint64_t ShaderPool::getPointsProperties(bool texture, bool vertexColorVec3, bool vertexColorVec4, bool textureRect){
    uint64_t prop = 0;

    prop |= texture          ? shaderPropertyBit(0) : 0;
    prop |= vertexColorVec3	 ? shaderPropertyBit(1) : 0;
    prop |= vertexColorVec4  ? shaderPropertyBit(2) : 0;
    prop |= textureRect      ? shaderPropertyBit(3) : 0;

    return prop;
}

uint64_t ShaderPool::getParticlesProperties(const ParticlesShaderFlags& flags){
    uint64_t prop = 0;

    prop |= flags.texture        ? (1 <<  0) : 0;
    prop |= flags.mesh           ? (1 <<  1) : 0;
    prop |= flags.trail          ? (1 <<  2) : 0;
    prop |= flags.stretch        ? (1 <<  3) : 0;
    prop |= flags.frameBlend     ? (1 <<  4) : 0;
    prop |= flags.lit            ? (1 <<  5) : 0;
    prop |= flags.sixWay         ? (1 <<  6) : 0;
    prop |= flags.customData     ? (1 <<  7) : 0;
    prop |= flags.receiveShadows ? (1 <<  8) : 0;
    prop |= flags.fog            ? (1 <<  9) : 0;

    return prop;
}

uint64_t ShaderPool::getLinesProperties(bool vertexColorVec3, bool vertexColorVec4, bool ribbon, bool texture, bool pixelWidth){
    uint64_t prop = 0;

    prop |= vertexColorVec3	 ? shaderPropertyBit(0) : 0;
    prop |= vertexColorVec4  ? shaderPropertyBit(1) : 0;
    // a hairline has no texture coordinates nor widths
    prop |= ribbon                  ? shaderPropertyBit(3) : 0;
    prop |= (ribbon && texture)     ? shaderPropertyBit(4) : 0;
    prop |= (ribbon && pixelWidth)  ? shaderPropertyBit(5) : 0;

    return prop;
}

void ShaderPool::clear(){
    shaders_t entries;
    {
        std::lock_guard<std::mutex> lock(shaderEntriesMutex());
        entries.swap(getMap());
        missingShaderNames().clear();
        failedShaders().clear();
        notPrecompiledShaders().clear();
    }
    for (auto& it: entries) {
        if (it.second)
            it.second->destroyShader();
    }
    // stops a compile in flight; what it compiled is in the disk cache for next time
    RuntimeShaderCompiler::shutdown();

    std::lock_guard<std::mutex> lock(customShaderMutex());
    customShaderNames().clear();
    customShaderGeneration++;
}

void ShaderPool::clearUnused(){
    std::vector<ShaderKey> keys;
    {
        std::lock_guard<std::mutex> lock(shaderEntriesMutex());
        keys.reserve(getMap().size());
        for (const auto& entry : getMap()) {
            keys.push_back(entry.first);
        }
    }
    for (ShaderKey key : keys) {
        remove(getShaderTypeFromKey(key), getPropertiesFromKey(key), getCustomIdFromKey(key));
    }
    // Per-project bookkeeping: always reset so the next project does not inherit
    // missing/failed entries or custom-shader name registrations.
    {
        std::lock_guard<std::mutex> lock(shaderEntriesMutex());
        missingShaderNames().clear();
        failedShaders().clear();
        notPrecompiledShaders().clear();
    }
    // its variants are keyed by the custom shader ids reset below
    RuntimeShaderCompiler::shutdown();
    std::lock_guard<std::mutex> lock(customShaderMutex());
    customShaderNames().clear();
    customShaderGeneration++;
}
