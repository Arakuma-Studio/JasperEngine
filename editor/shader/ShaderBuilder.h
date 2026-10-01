// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"

#include "shader/ShaderBuildTypes.h"
#include "ShaderData.h"
#include <atomic>
#include <vector>
#include <set>
#include <unordered_map>
#include <mutex>
#include <future>
#include <filesystem>

namespace jasperengine::editor {

    // The editor's shader builds: the engine's ShaderVariantCompiler (the same compiler
    // a game uses at runtime) driven for the open project, with forked shaders read
    // from the project, a disk cache, and builds on the thread pool.
    class ShaderBuilder {
    private:
        static std::unordered_map<ShaderKey, ShaderData> shaderDataCache;
        static std::unordered_map<ShaderKey, std::future<ShaderData>> pendingBuilds;
        static std::mutex cacheMutex;

        static std::atomic<bool> shutdownRequested;

        // Compiles the key for a backend. For custom shaders, resolves only the exact
        // transitive dependency graph from the fork's .vert/.frag entrypoints.
        // A cancellable build stops at an editor shutdown. Throws on failure.
        ShaderData compileShader(ShaderKey shaderKey, Project* project, ShaderBackend backend, bool trackProgress, bool cancellable);

        ShaderData buildShaderInternal(ShaderKey shaderKey, Project* project, bool trackProgress);
        std::string getShaderDisplayName(ShaderKey key);

        static std::filesystem::path getShaderCachePath(ShaderKey shaderKey, Project* project);

        // Compares the cached source signature with the entry points and the exact
        // transitive include graph used by this fork. No directory walk is required.
        static bool isCustomCacheStale(ShaderKey shaderKey, Project* project, const std::filesystem::path& cachePath);

    public:
        ShaderBuilder();
        virtual ~ShaderBuilder();

        ShaderBuildResult buildShader(ShaderKey shaderKey, Project* project);
        ShaderData buildShaderForExport(ShaderKey shaderKey, Project* project, ShaderBackend backend);

        // Compiles the keys absent from the disk cache, synchronously (buildShader hands
        // async builds to the pool and returns before they land).
        void buildMissingShaders(const std::set<ShaderKey>& shaderKeys, Project* project);

        // Drops cached builds and dependency snapshots for all custom variants so the
        // next get() recompiles. Called after source mutations and project switches.
        static void invalidateCustomShaders();

        static void requestShutdown();

        // Serialize ShaderData cache on demand (no disk I/O inside buildShaderInternal).
        static bool saveShaderDataCache(ShaderKey shaderKey, Project* project, const ShaderData& shaderData, std::string* err = nullptr);

        ShaderData& getShaderData(ShaderKey shaderKey);
    };

}
