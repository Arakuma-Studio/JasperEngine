// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderBuilder.h"
#include "diag/HangWatchdog.h"
#include "io/AtomicFile.h"
#include "shader/ShaderGlobals.h"

#include "ShaderDataSerializer.h"
#include "App.h"
#include "Engine.h"
#include "Out.h"

#include "thread/ResourceProgress.h"

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "shader/ShaderManifest.h"
#include "util/SHA1.h"

#include <cstring>
#include <cstdint>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcompiler.h>
#endif
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>

using namespace jasperengine;

namespace {

struct CustomSourceSnapshot {
    // entry points, reached project files and the resolved graph (ShaderVariantCompiler)
    CustomShaderSources sources;
    // SHA1 of sources.signature, persisted next to the cached build
    std::string signature;
};

std::mutex customSourceCacheMutex;
std::unordered_map<std::string, std::shared_ptr<const CustomSourceSnapshot>> customSourceCache;

bool readTextFile(const std::filesystem::path& path, std::string& content) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;
    std::ostringstream stream;
    stream << file.rdbuf();
    content = stream.str();
    return true;
}

// a library shader resolves the same in any project; its DEPTH variant is another program
std::string customSourceCacheKey(editor::Project* project, const std::string& customShader, ShaderType shaderType) {
    // (with the assets directory, which a fork path may be relative to: Project::findShaderFile)
    const std::string root = (project && !ShaderReference::isLibrary(customShader))
        ? project->getProjectPath().lexically_normal().generic_string() + '\n' + project->getAssetsPath().lexically_normal().generic_string()
        : std::string();
    return root + '\n' + customShader + '\n' + std::to_string(static_cast<int>(shaderType));
}

std::shared_ptr<const CustomSourceSnapshot> resolveCustomSources(editor::Project* project,
                                                                 const std::string& customShader,
                                                                 ShaderType shaderType) {
    // the library's and the built-in Lit's variants ("builtin:lit#...", S11) read engine sources only
    const bool library = ShaderReference::isLibrary(customShader) || ShaderReference::isBuiltin(customShader);
    if ((!project && !library) || customShader.empty()) {
        auto invalid = std::make_shared<CustomSourceSnapshot>();
        invalid->sources.error = "Custom shader project or path is unavailable.";
        return invalid;
    }

    std::string cacheKey = customSourceCacheKey(project, customShader, shaderType);
    std::lock_guard<std::mutex> lock(customSourceCacheMutex);
    if (auto it = customSourceCache.find(cacheKey); it != customSourceCache.end())
        return it->second;

    const std::filesystem::path projectRoot = project ? project->getProjectPath() : std::filesystem::path();
    const std::filesystem::path assetsRoot = project ? project->getAssetsPath() : std::filesystem::path();
    // A project UI effect (a fragment-only custom shader, phase 5) is named by its asset path,
    // as @effect declares it and the shipped game's runtime compiler reads it: its files are
    // read from the assets directory first (the project root when that is the same)
    const bool fragmentOnly = customShader.find('|') == std::string::npos && customShader.size() > 5 &&
                              customShader.compare(customShader.size() - 5, 5, ".frag") == 0;
    auto snapshot = std::make_shared<CustomSourceSnapshot>();
    ShaderVariantCompiler::FileReader reader;
    if (project) {
        // entry points and includes: project-relative, else assets-relative, as the game reads
        // them (a UI effect's: assets-relative first)
        reader = [projectRoot, assetsRoot, fragmentOnly](const std::string& relativePath, std::string& content) {
            const std::filesystem::path path = fragmentOnly ? editor::Project::findShaderFile(assetsRoot, projectRoot, relativePath)
                                                            : editor::Project::findShaderFile(projectRoot, assetsRoot, relativePath);
            return !path.empty() && readTextFile(path, content);
        };
    }
    snapshot->sources = ShaderVariantCompiler::resolveCustomShaderSources(customShader, reader, shaderType);

    // Failures are not cached: the file may be created or restored at any moment.
    if (!snapshot->sources.valid)
        return snapshot;

    snapshot->signature = editor::SHA1::hash(snapshot->sources.signature);
    customSourceCache[cacheKey] = snapshot;
    return snapshot;
}

std::filesystem::path sourceSignaturePath(const std::filesystem::path& shaderCachePath) {
    return std::filesystem::path(shaderCachePath.string() + ".sources.sha1");
}

bool readSourceSignature(const std::filesystem::path& path, std::string& signature) {
    if (!readTextFile(path, signature))
        return false;
    while (!signature.empty() && (signature.back() == '\n' || signature.back() == '\r'))
        signature.pop_back();
    return !signature.empty();
}

// the customShader of a customSourceCacheKey (its second-to-last line)
std::string customShaderOfCacheKey(const std::string& key) {
    const size_t last = key.find_last_of('\n');
    if (last == std::string::npos || last == 0)
        return std::string();
    const size_t before = key.find_last_of('\n', last - 1);
    return key.substr(before == std::string::npos ? 0 : before + 1, last - (before == std::string::npos ? 0 : before + 1));
}

void clearCustomSourceCache(const std::function<bool(const std::string&)>& which) {
    std::lock_guard<std::mutex> lock(customSourceCacheMutex);
    for (auto it = customSourceCache.begin(); it != customSourceCache.end(); ) {
        if (which(customShaderOfCacheKey(it->first)))
            it = customSourceCache.erase(it);
        else
            ++it;
    }
}

// The id of a variant's build in ResourceProgress (a hash of the key, as other resources hash their name)
uint64_t progressId(ShaderKey key) {
    return static_cast<uint64_t>(std::hash<ShaderKey>{}(key));
}

} // namespace


std::unordered_map<ShaderKey, ShaderData> editor::ShaderBuilder::shaderDataCache;
std::unordered_map<ShaderKey, jobs::Task<ShaderData>> editor::ShaderBuilder::pendingBuilds;
std::mutex editor::ShaderBuilder::cacheMutex;
std::atomic<bool> editor::ShaderBuilder::shutdownRequested{false};

ShaderBuildResult editor::ShaderBuilder::buildShader(ShaderKey shaderKey, Project* project) {
    std::unique_lock<std::mutex> lock(cacheMutex);

    // Check if already in cache
    if (auto cached = shaderDataCache.find(shaderKey); cached != shaderDataCache.end()){
        return ShaderBuildResult(cached->second, ResourceLoadState::Finished);
    }

    // Try disk cache (<cache>/jasperengine/shaders/<version>/<basename>.sdat). Forked shaders
    // are cached too (the run-build / standalone runtime loads shaders from this same
    // dir and cannot compile). A persisted source signature covers both entry points and
    // their exact resolved include graph. The validation key is the storage key
    // (customId stripped) because that id is session-local and would not match across
    // the editor and a separate run process.
    // a library id (an effect of the engine library) is cached like a built-in
    const uint16_t keyCustomId = ShaderPool::getCustomIdFromKey(shaderKey);
    const bool isCustom = keyCustomId != 0 && !ShaderPool::isLibraryCustomShader(keyCustomId);
    if (project) {
        lock.unlock();
        const std::filesystem::path cachePath = getShaderCachePath(shaderKey);
        const bool fresh = !isCustom || !isCustomCacheStale(shaderKey, project, cachePath);
        if (fresh) {
            ShaderData diskData;
            std::string err;
            if (ShaderDataSerializer::readFromFile(cachePath.string(), ShaderPool::getStorageKey(shaderKey), diskData, &err)) {
                lock.lock();
                shaderDataCache[shaderKey] = diskData;
                printf("Shader loaded from disk cache: %s\n", cachePath.string().c_str());
                return ShaderBuildResult(diskData, ResourceLoadState::Finished);
            }
        }
        lock.lock();
    }

    // Check if already building
    if (auto pending = pendingBuilds.find(shaderKey); pending != pendingBuilds.end()) {
        jobs::Task<ShaderData>& task = pending->second;
        if (task.isReady()) {
            // Build finished, move to cache
            try {
                ShaderData data = task.get();
                shaderDataCache[shaderKey] = data;
                pendingBuilds.erase(shaderKey);

                lock.unlock();
                // Persist cache after build finishes (outside lock).
                (void)saveShaderDataCache(shaderKey, project, data);

                return ShaderBuildResult(data, ResourceLoadState::Finished);
            } catch (const std::exception& e) {
                pendingBuilds.erase(shaderKey);
                return ShaderBuildResult({}, ResourceLoadState::Failed);
            }
        } else {
            // Still building
            return ShaderBuildResult({}, ResourceLoadState::Loading);
        }
    }

    if (!Engine::isAsyncLoading()) {
        try {
            lock.unlock();
            // the synchronous compile blocks the main thread (a hang report names it)
            const diag::BusyScope busy("Compiling shader", 120'000);
            ShaderData data = buildShaderInternal(shaderKey, project, false);
            lock.lock();
            shaderDataCache[shaderKey] = data;
            lock.unlock();

            (void)saveShaderDataCache(shaderKey, project, data);

            return ShaderBuildResult(data, ResourceLoadState::Finished);
        } catch (const std::exception& e) {
            return ShaderBuildResult({}, ResourceLoadState::Failed);
        }
    }

    // Start new async build
    std::string shaderName = getShaderDisplayName(shaderKey);
    ResourceProgress::startBuild(progressId(shaderKey), ResourceType::Shader, shaderName);

    // a compile holds a worker for long: Heavy, so the decodes the frame waits on keep slots
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.gate = jobs::GateClass::Heavy;
    options.name = "shader-build";
    // a resource being built: counted by Engine::getQueuedResourceCount (the editor's loading state)
    options.tags = jobs::Tag::Asset;
    pendingBuilds[shaderKey] = jobs::submit(
        [this, shaderKey, project]() {
            return buildShaderInternal(shaderKey, project, true);
        }, options
    );

    return ShaderBuildResult({}, ResourceLoadState::Loading);
}

void editor::ShaderBuilder::buildMissingShaders(const std::set<ShaderKey>& shaderKeys, Project* project) {
    if (!project) {
        return;
    }

    for (const ShaderKey& shaderKey : shaderKeys) {
        const std::filesystem::path cachePath = getShaderCachePath(shaderKey);
        const uint16_t keyCustomId = ShaderPool::getCustomIdFromKey(shaderKey);
        const bool isCustom = keyCustomId != 0 && !ShaderPool::isLibraryCustomShader(keyCustomId);
        const bool fresh = !isCustom || !isCustomCacheStale(shaderKey, project, cachePath);

        ShaderData diskData;
        if (fresh && ShaderDataSerializer::readFromFile(cachePath.string(), ShaderPool::getStorageKey(shaderKey), diskData)) {
            continue;
        }

        try {
            const ShaderData data = buildShaderInternal(shaderKey, project, false);
            (void)saveShaderDataCache(shaderKey, project, data);
        } catch (const std::exception&) {
            // buildShaderInternal reports it; the remaining keys still build
        }
    }
}

size_t editor::ShaderBuilder::getRunningBuildCount() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    size_t running = 0;
    for (const auto& [key, task] : pendingBuilds) {
        if (!task.isReady())
            running++;
    }
    return running;
}

void editor::ShaderBuilder::requestShutdown() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    shutdownRequested = true;

    // builds not started never run; the running ones are waited for (cancelled first, so
    // no wait runs a build inline while this holds cacheMutex)
    for (auto& [key, task] : pendingBuilds){
        if (task.cancel())
            ResourceProgress::failBuild(progressId(key));
    }
    for (auto& [key, task] : pendingBuilds)
        task.wait();
    pendingBuilds.clear();
}


ShaderData editor::ShaderBuilder::compileShader(ShaderKey shaderKey, Project* project, ShaderBackend backend, bool trackProgress, bool cancellable){
    if (cancellable && shutdownRequested) {
        throw std::runtime_error("Shutdown requested");
    }
    if (trackProgress) {
        ResourceProgress::updateProgress(progressId(shaderKey), 0.1f); // Starting
    }

    // A fork's entry points and every project include they reach are overlaid on the
    // engine library (the variant system is identical for forked shaders)
    std::shared_ptr<const CustomSourceSnapshot> custom;
    const uint16_t customId = ShaderPool::getCustomIdFromKey(shaderKey);
    if (customId != 0 && !ShaderPool::isLibraryCustomShader(customId)) {
        std::string setupError;
        const std::string customShader = ShaderPool::getCustomShaderName(customId);
        if (!project && !ShaderReference::isLibrary(customShader) && !ShaderReference::isBuiltin(customShader)) {
            setupError = "Custom shader requested without a project";
        } else {
            if (customShader.empty()) {
                setupError = "Unknown custom shader id";
            } else {
                custom = resolveCustomSources(project, customShader, ShaderPool::getShaderTypeFromKey(shaderKey));
                if (!custom->sources.valid) {
                    setupError = custom->sources.error.empty() ? "Could not resolve custom shader sources" : custom->sources.error;
                }
            }
        }
        if (!setupError.empty()) {
            if (trackProgress) {
                ResourceProgress::failBuild(progressId(shaderKey));
            }
            throw std::runtime_error(setupError);
        }
    }

    ShaderVariantCompiler::ProgressFn progress;
    std::function<bool()> cancelled;
    if (trackProgress) {
        progress = [shaderKey](float value) {
            // the last step is reported below, once the build is in the cache
            if (value < 1.0f)
                ResourceProgress::updateProgress(progressId(shaderKey), value);
        };
    }
    if (cancellable) {
        cancelled = []() { return shutdownRequested.load(); };
    }

    ShaderData shaderData;
    std::string error;
    std::string compileLog;
    if (!ShaderVariantCompiler::compile(shaderKey, backend, custom ? &custom->sources : nullptr,
                                        shaderData, &error, &compileLog, progress, cancelled)) {
        if (cancellable && shutdownRequested) {
            throw std::runtime_error("Shutdown requested");
        }
        if (trackProgress) {
            ResourceProgress::failBuild(progressId(shaderKey));
        }
        // the GLSL compiler log goes to the output window, where the details otherwise
        // only reach stderr/app output
        const char* failure = (error == "Error cross-compiling") ? "cross-compile" : "compile";
        Out::error("Shader %s (%s) failed to %s:\n%s", getShaderDisplayName(shaderKey).c_str(),
                   ShaderPool::getShaderLangStr(backend).c_str(), failure,
                   compileLog.empty() ? error.c_str() : compileLog.c_str());
        throw std::runtime_error(error);
    }

    if (trackProgress) {
        ResourceProgress::updateProgress(progressId(shaderKey), 1.0f); // Complete
        ResourceProgress::completeBuild(progressId(shaderKey));
    }

    Out::build(
        "Shader %s (%s) generated successfully",
        getShaderDisplayName(shaderKey).c_str(),
        ShaderPool::getShaderLangStr(backend).c_str());
    // the variants of a GLES3 build that render Ultra shadows without PCSS (docs/rendering/shadows.md)
    if (compileLog.find(ShaderVariantCompiler::PCSS_LEFT_OUT) != std::string::npos){
        Out::build("Shader %s (%s): no PCSS, its textures leave no texture unit for it; Ultra soft shadows use the High tier's filter",
                   getShaderDisplayName(shaderKey).c_str(), ShaderPool::getShaderLangStr(backend).c_str());
    }
    // the painted terrain variants whose lights' records leave no unit for the spot lights' masks
    if (compileLog.find(ShaderVariantCompiler::SPOT_MASKS_LEFT_OUT) != std::string::npos){
        Out::build("Shader %s (%s): no spot light masks, its textures leave no texture unit for them; masked spot lights light it with their cones",
                   getShaderDisplayName(shaderKey).c_str(), ShaderPool::getShaderLangStr(backend).c_str());
    }
    if (compileLog.find(ShaderVariantCompiler::SHADOWS_2D_LEFT_OUT) != std::string::npos){
        Out::build("Shader %s (%s): no 2D light shadows, its textures leave no texture unit for them; 2D lights light it unshadowed",
                   getShaderDisplayName(shaderKey).c_str(), ShaderPool::getShaderLangStr(backend).c_str());
    }
    // what a fork reads of the global shader values that Project Settings does not declare as
    // such (once: for the editor's own backend)
    if (ShaderPool::getCustomIdFromKey(shaderKey) != 0 && backend == ShaderPool::getShaderBackend()) {
        for (const ShaderGlobalDiagnostic& d : ShaderGlobals::checkShader(shaderData, ShaderGlobals::getDeclarationsSnapshot())) {
            Out::warning("Shader %s: %s: %s", getShaderDisplayName(shaderKey).c_str(), d.code.c_str(), d.message.c_str());
        }
    }
    // a variant over the per-stage budget may not bind on some devices (GLES3: 16 texture units)
    const std::string over = ShaderVariantCompiler::checkResourceBudget(shaderData);
    if (!over.empty()){
        Out::warning("Shader %s (%s) is over the resource budget: %s", getShaderDisplayName(shaderKey).c_str(),
                     ShaderPool::getShaderLangStr(backend).c_str(), over.c_str());
    }

    return shaderData;
}

ShaderData editor::ShaderBuilder::buildShaderInternal(ShaderKey shaderKey, Project* project, bool trackProgress){
    return compileShader(shaderKey, project, ShaderPool::getShaderBackend(), trackProgress, true);
}

std::filesystem::path editor::ShaderBuilder::getShaderCachePath(ShaderKey shaderKey) {
    ShaderType shaderType = ShaderPool::getShaderTypeFromKey(shaderKey);
    uint64_t properties = ShaderPool::getPropertiesFromKey(shaderKey);
    uint16_t customId = ShaderPool::getCustomIdFromKey(shaderKey);
    std::string basename = ShaderPool::getShaderStr(shaderType, properties, customId) +
                           "_" + ShaderPool::getShaderLangStr();

    return App::getUserShaderCacheDir() / (basename + ".sdat");
}

bool editor::ShaderBuilder::saveShaderDataCache(ShaderKey shaderKey, Project* project, const ShaderData& shaderData, std::string* err) {
    if (!project) {
        return false;
    }

    const std::filesystem::path cachePath = getShaderCachePath(shaderKey);

    std::error_code ec;
    std::filesystem::create_directories(cachePath.parent_path(), ec);
    if (ec) {
        if (err) {
            *err = "Failed to create shader cache directory: " + cachePath.parent_path().string();
        }
        return false;
    }

    // Validate/serialize with the storage key (customId stripped): a separate run
    // process assigns its own session-local customId, so the on-disk key must not
    // depend on it (the unique basename in the filename identifies the forked source).
    if (!ShaderDataSerializer::writeToFile(cachePath.string(), ShaderPool::getStorageKey(shaderKey), shaderData, err))
        return false;

    const uint16_t customId = ShaderPool::getCustomIdFromKey(shaderKey);
    if (customId == 0 || ShaderPool::isLibraryCustomShader(customId))
        return true;

    const std::string customShader = ShaderPool::getCustomShaderName(customId);
    const std::shared_ptr<const CustomSourceSnapshot> sources = resolveCustomSources(project, customShader, ShaderPool::getShaderTypeFromKey(shaderKey));
    if (!sources->sources.valid) {
        if (err)
            *err = sources->sources.error;
        return false;
    }

    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    if (!io::writeFileAtomic(sourceSignaturePath(cachePath), sources->signature + "\n", options).ok()) {
        if (err)
            *err = "Failed to write custom shader source signature.";
        return false;
    }
    return true;
}

bool editor::ShaderBuilder::isCustomCacheStale(ShaderKey shaderKey, Project* project, const std::filesystem::path& cachePath) {
    std::error_code ec;
    if (!std::filesystem::exists(cachePath, ec))
        return true;

    const std::string customShader = ShaderPool::getCustomShaderName(ShaderPool::getCustomIdFromKey(shaderKey));
    if (customShader.empty())
        return true;

    const std::shared_ptr<const CustomSourceSnapshot> sources = resolveCustomSources(project, customShader, ShaderPool::getShaderTypeFromKey(shaderKey));
    if (!sources->sources.valid)
        return true;

    std::string cachedSignature;
    return !readSourceSignature(sourceSignaturePath(cachePath), cachedSignature) ||
           cachedSignature != sources->signature;
}

std::string editor::ShaderBuilder::getShaderDisplayName(ShaderKey key) {
    ShaderType type = ShaderPool::getShaderTypeFromKey(key);
    uint64_t properties = ShaderPool::getPropertiesFromKey(key);
    uint16_t customId = ShaderPool::getCustomIdFromKey(key);
    std::string shaderStr = ShaderPool::getShaderStr(type, properties, customId);

    std::string cliSpec;
    if (ShaderPool::getShaderCliSpec(shaderStr, cliSpec)) {
        return cliSpec;
    }

    return shaderStr;
}

void editor::ShaderBuilder::invalidateCustomShaders() {
    invalidateCustomShaders([](const std::string&) { return true; });
}

void editor::ShaderBuilder::primeCache(ShaderKey shaderKey, const ShaderData& data) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    pendingBuilds.erase(shaderKey);
    shaderDataCache[shaderKey] = data;
}

void editor::ShaderBuilder::dropSourceSnapshot(const std::string& fork) {
    clearCustomSourceCache([&fork](const std::string& name) { return name == fork; });
}

void editor::ShaderBuilder::invalidateCustomShaders(const std::function<bool(const std::string& fork)>& which) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    // the engine library's effects (library ids) do not change with the project
    auto projectCustom = [&which](ShaderKey key) {
        const uint16_t customId = ShaderPool::getCustomIdFromKey(key);
        return customId != 0 && !ShaderPool::isLibraryCustomShader(customId) && which(ShaderPool::getCustomShaderName(customId));
    };
    for (auto it = shaderDataCache.begin(); it != shaderDataCache.end(); ) {
        if (projectCustom(it->first))
            it = shaderDataCache.erase(it);
        else
            ++it;
    }
    // Drop pending custom builds too, otherwise buildShader would reap a stale (pre-edit)
    // result into the cache. One not started is cancelled; a running one finishes and its
    // result is discarded (dropping a handle never blocks); the next get() restarts the build.
    for (auto it = pendingBuilds.begin(); it != pendingBuilds.end(); ) {
        if (projectCustom(it->first)){
            if (it->second.cancel())
                ResourceProgress::failBuild(progressId(it->first));
            it = pendingBuilds.erase(it);
        }else{
            ++it;
        }
    }
    clearCustomSourceCache(which);
}

ShaderData editor::ShaderBuilder::buildShaderForExport(ShaderKey shaderKey, Project* project, ShaderBackend backend) {
    return compileShader(shaderKey, project, backend, false, false);
}

bool editor::ShaderBuilder::compileHlslBytecode(ShaderData& shaderData) {
#ifdef _WIN32
    // loaded once; a Windows without it (none since 8.1) keeps shipping the source
    static const pD3DCompile d3dCompile = []() -> pD3DCompile {
        HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
        return dll ? reinterpret_cast<pD3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    }();
    if (!d3dCompile) {
        return false;
    }

    for (ShaderStage& stage : shaderData.stages) {
        if (stage.bytecode.data || stage.source.empty()) continue;
        // the target and flags of sokol_gfx compiling the source (_sg_d3d11_compile_shader)
        const char* target = (stage.type == ShaderStageType::VERTEX) ? "vs_5_0" : "ps_5_0";
        ID3DBlob* code = nullptr;
        ID3DBlob* messages = nullptr;
        const HRESULT hr = d3dCompile(stage.source.c_str(), stage.source.size(), nullptr, nullptr, nullptr, "main", target,
                                      D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
        const std::string error = (FAILED(hr) && messages) ? static_cast<const char*>(messages->GetBufferPointer()) : "";
        if (messages) messages->Release();
        if (FAILED(hr)) {
            if (code) code->Release();
            throw std::runtime_error("Error compiling HLSL: " + error);
        }

        stage.bytecode.size = static_cast<uint32_t>(code->GetBufferSize());
        stage.bytecode.data = new unsigned char[stage.bytecode.size];
        std::memcpy(stage.bytecode.data, code->GetBufferPointer(), stage.bytecode.size);
        stage.source.clear();
        code->Release();
    }
    return true;
#else
    (void)shaderData;
    return false;
#endif
}

bool editor::ShaderBuilder::customShaderResolves(Project* project, ShaderKey key, std::string* error) {
    const uint16_t customId = ShaderPool::getCustomIdFromKey(key);
    if (customId == 0 || ShaderPool::isLibraryCustomShader(customId)) return true;
    const std::string customShader = ShaderPool::getCustomShaderName(customId);
    if (customShader.empty()) {
        if (error) *error = "unknown custom shader";
        return false;
    }
    if (ShaderReference::isLibrary(customShader) || ShaderReference::isBuiltin(customShader)) return true;
    const std::shared_ptr<const CustomSourceSnapshot> sources = resolveCustomSources(project, customShader, ShaderPool::getShaderTypeFromKey(key));
    if (sources->sources.valid) return true;
    if (error) *error = sources->sources.error.empty() ? "its sources do not resolve" : sources->sources.error;
    return false;
}
