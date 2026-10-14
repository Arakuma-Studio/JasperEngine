// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderBuilder.h"

#include "ShaderDataSerializer.h"
#include "App.h"
#include "Engine.h"
#include "Out.h"

#include "thread/ResourceProgress.h"
#include "thread/ThreadPoolManager.h"

#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCompiler.h"
#include "util/SHA1.h"

#include <cstring>
#include <cstdint>
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

std::string customSourceCacheKey(editor::Project* project, const std::string& customShader) {
    return project->getProjectPath().lexically_normal().generic_string() + '\n' + customShader;
}

std::shared_ptr<const CustomSourceSnapshot> resolveCustomSources(editor::Project* project,
                                                                 const std::string& customShader) {
    if (!project || customShader.empty()) {
        auto invalid = std::make_shared<CustomSourceSnapshot>();
        invalid->sources.error = "Custom shader project or path is unavailable.";
        return invalid;
    }

    std::string cacheKey = customSourceCacheKey(project, customShader);
    std::lock_guard<std::mutex> lock(customSourceCacheMutex);
    if (auto it = customSourceCache.find(cacheKey); it != customSourceCache.end())
        return it->second;

    const std::filesystem::path projectRoot = project->getProjectPath();
    // A project UI effect (a fragment-only custom shader, phase 5) is named by its asset path,
    // as @effect declares it and the shipped game's runtime compiler reads it: its files are
    // read from the assets directory first (the project root when that is the same)
    const bool fragmentOnly = customShader.find('|') == std::string::npos && customShader.size() > 5 &&
                              customShader.compare(customShader.size() - 5, 5, ".frag") == 0;
    const std::filesystem::path assetsRoot = fragmentOnly ? project->getAssetsPath() : std::filesystem::path();
    auto snapshot = std::make_shared<CustomSourceSnapshot>();
    snapshot->sources = ShaderVariantCompiler::resolveCustomShaderSources(customShader,
        [&projectRoot, &assetsRoot](const std::string& relativePath, std::string& content) {
            std::error_code ec;
            if (!assetsRoot.empty() && std::filesystem::is_regular_file(assetsRoot / relativePath, ec))
                return readTextFile(assetsRoot / relativePath, content);
            const std::filesystem::path path = projectRoot / relativePath;
            return std::filesystem::is_regular_file(path, ec) && readTextFile(path, content);
        });

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

void clearCustomSourceCache() {
    std::lock_guard<std::mutex> lock(customSourceCacheMutex);
    customSourceCache.clear();
}

} // namespace


std::unordered_map<ShaderKey, ShaderData> editor::ShaderBuilder::shaderDataCache;
std::unordered_map<ShaderKey, std::future<ShaderData>> editor::ShaderBuilder::pendingBuilds;
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
        auto& future = pending->second;
        if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            // Build finished, move to cache
            try {
                ShaderData data = future.get();
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
    ResourceProgress::startBuild(shaderKey, ResourceType::Shader, shaderName);

    // Use thread pool instead of std::async
    pendingBuilds[shaderKey] = ThreadPoolManager::getInstance().enqueue(
        [this, shaderKey, project]() {
            return buildShaderInternal(shaderKey, project, true);
        }
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

void editor::ShaderBuilder::requestShutdown() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    shutdownRequested = true;

    // Wait for all pending builds to complete
    for (auto& [key, future] : pendingBuilds) {
        if (future.valid()) {
            future.wait(); // Wait for completion
        }
    }
    pendingBuilds.clear();
}


ShaderData editor::ShaderBuilder::compileShader(ShaderKey shaderKey, Project* project, ShaderBackend backend, bool trackProgress, bool cancellable){
    if (cancellable && shutdownRequested) {
        throw std::runtime_error("Shutdown requested");
    }
    if (trackProgress) {
        ResourceProgress::updateProgress(shaderKey, 0.1f); // Starting
    }

    // A fork's entry points and every project include they reach are overlaid on the
    // engine library (the variant system is identical for forked shaders)
    std::shared_ptr<const CustomSourceSnapshot> custom;
    const uint16_t customId = ShaderPool::getCustomIdFromKey(shaderKey);
    if (customId != 0 && !ShaderPool::isLibraryCustomShader(customId)) {
        std::string setupError;
        if (!project) {
            setupError = "Custom shader requested without a project";
        } else {
            const std::string customShader = ShaderPool::getCustomShaderName(customId);
            if (customShader.empty()) {
                setupError = "Unknown custom shader id";
            } else {
                custom = resolveCustomSources(project, customShader);
                if (!custom->sources.valid) {
                    setupError = custom->sources.error.empty() ? "Could not resolve custom shader sources" : custom->sources.error;
                }
            }
        }
        if (!setupError.empty()) {
            if (trackProgress) {
                ResourceProgress::failBuild(shaderKey);
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
                ResourceProgress::updateProgress(shaderKey, value);
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
            ResourceProgress::failBuild(shaderKey);
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
        ResourceProgress::updateProgress(shaderKey, 1.0f); // Complete
        ResourceProgress::completeBuild(shaderKey);
    }

    Out::build(
        "Shader %s (%s) generated successfully",
        getShaderDisplayName(shaderKey).c_str(),
        ShaderPool::getShaderLangStr(backend).c_str());

    return shaderData;
}

ShaderData editor::ShaderBuilder::buildShaderInternal(ShaderKey shaderKey, Project* project, bool trackProgress){
    return compileShader(shaderKey, project, ShaderPool::getShaderBackend(), trackProgress, true);
}

std::filesystem::path editor::ShaderBuilder::getShaderCachePath(ShaderKey shaderKey) {
    ShaderType shaderType = ShaderPool::getShaderTypeFromKey(shaderKey);
    uint32_t properties = ShaderPool::getPropertiesFromKey(shaderKey);
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
    const std::shared_ptr<const CustomSourceSnapshot> sources = resolveCustomSources(project, customShader);
    if (!sources->sources.valid) {
        if (err)
            *err = sources->sources.error;
        return false;
    }

    std::ofstream signatureFile(sourceSignaturePath(cachePath), std::ios::binary | std::ios::trunc);
    if (!signatureFile) {
        if (err)
            *err = "Failed to write custom shader source signature.";
        return false;
    }
    signatureFile << sources->signature << '\n';
    if (!signatureFile) {
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

    const std::shared_ptr<const CustomSourceSnapshot> sources = resolveCustomSources(project, customShader);
    if (!sources->sources.valid)
        return true;

    std::string cachedSignature;
    return !readSourceSignature(sourceSignaturePath(cachePath), cachedSignature) ||
           cachedSignature != sources->signature;
}

std::string editor::ShaderBuilder::getShaderDisplayName(ShaderKey key) {
    ShaderType type = ShaderPool::getShaderTypeFromKey(key);
    uint32_t properties = ShaderPool::getPropertiesFromKey(key);
    uint16_t customId = ShaderPool::getCustomIdFromKey(key);
    std::string shaderStr = ShaderPool::getShaderStr(type, properties, customId);

    std::string cliSpec;
    if (ShaderPool::getShaderCliSpec(shaderStr, cliSpec)) {
        return cliSpec;
    }

    return shaderStr;
}

void editor::ShaderBuilder::invalidateCustomShaders() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    // the engine library's effects (library ids) do not change with the project
    auto projectCustom = [](ShaderKey key) {
        const uint16_t customId = ShaderPool::getCustomIdFromKey(key);
        return customId != 0 && !ShaderPool::isLibraryCustomShader(customId);
    };
    for (auto it = shaderDataCache.begin(); it != shaderDataCache.end(); ) {
        if (projectCustom(it->first))
            it = shaderDataCache.erase(it);
        else
            ++it;
    }
    // Drop pending custom builds too, otherwise buildShader would reap a stale (pre-edit)
    // future into the cache. ThreadPoolManager futures don't block on destruction; an
    // in-flight task simply discards its result and the next get() restarts the build.
    for (auto it = pendingBuilds.begin(); it != pendingBuilds.end(); ) {
        if (projectCustom(it->first))
            it = pendingBuilds.erase(it);
        else
            ++it;
    }
    clearCustomSourceCache();
}

ShaderData editor::ShaderBuilder::buildShaderForExport(ShaderKey shaderKey, Project* project, ShaderBackend backend) {
    return compileShader(shaderKey, project, backend, false, false);
}
