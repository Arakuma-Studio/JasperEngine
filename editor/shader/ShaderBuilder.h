// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"

#include "shader/ShaderBuildTypes.h"
#include "ShaderData.h"
#include "jobs/JobSystem.h"
#include <atomic>
#include <vector>
#include <functional>
#include <set>
#include <unordered_map>
#include <mutex>
#include <filesystem>

namespace jasperengine::editor {

    // The editor's shader builds: the engine's ShaderVariantCompiler (the same compiler
    // a game uses at runtime) driven for the open project, with forked shaders read
    // from the project, a disk cache, and builds as Background Heavy jobs.
    class ShaderBuilder {
    private:
        static std::unordered_map<ShaderKey, ShaderData> shaderDataCache;
        static std::unordered_map<ShaderKey, jobs::Task<ShaderData>> pendingBuilds;
        static std::mutex cacheMutex;

        static std::atomic<bool> shutdownRequested;

        // Compiles the key for a backend. For custom shaders, resolves only the exact
        // transitive dependency graph from the fork's .vert/.frag entrypoints.
        // A cancellable build stops at an editor shutdown. Throws on failure.
        ShaderData compileShader(ShaderKey shaderKey, Project* project, ShaderBackend backend, bool trackProgress, bool cancellable);

        ShaderData buildShaderInternal(ShaderKey shaderKey, Project* project, bool trackProgress);
        std::string getShaderDisplayName(ShaderKey key);

        static std::filesystem::path getShaderCachePath(ShaderKey shaderKey);

        // Compares the cached source signature with the entry points and the exact
        // transitive include graph used by this fork. No directory walk is required.
        static bool isCustomCacheStale(ShaderKey shaderKey, Project* project, const std::filesystem::path& cachePath);

    public:
        ShaderBuildResult buildShader(ShaderKey shaderKey, Project* project);
        ShaderData buildShaderForExport(ShaderKey shaderKey, Project* project, ShaderBackend backend);

        // An export's D3D11 (HLSL 5) variant compiled to bytecode, as sokol_gfx compiles its source at
        // runtime (vs_5_0 / ps_5_0, column-major matrices, optimization level 3): the game skips
        // D3DCompile. Windows only (d3dcompiler_47.dll); false when it is not available (the source
        // stays). Throws with the compiler's messages when a stage does not compile.
        static bool compileHlslBytecode(ShaderData& shaderData);

        // False when `key` names a project fork whose sources do not resolve (its files are gone or
        // one of its includes is): what the export's shader build would fail on. True for the
        // built-in shaders, the library's and the builtin: variants. `error` says why.
        static bool customShaderResolves(Project* project, ShaderKey key, std::string* error = nullptr);

        // Compiles the keys absent from the disk cache, synchronously (buildShader hands
        // async builds to the pool and returns before they land).
        void buildMissingShaders(const std::set<ShaderKey>& shaderKeys, Project* project);

        // Drops cached builds and dependency snapshots for all custom variants so the
        // next get() recompiles. Called after source mutations and project switches.
        static void invalidateCustomShaders();
        // Only the forks `which` names (by custom shader name): a hot reload recompiles the
        // forks whose files changed and leaves the others bound (S05 R11)
        static void invalidateCustomShaders(const std::function<bool(const std::string& fork)>& which);
        // A build made ahead (a hot reload compiles before it swaps) for the next buildShader
        static void primeCache(ShaderKey shaderKey, const ShaderData& data);
        // The next build of `fork` reads its files again
        static void dropSourceSnapshot(const std::string& fork);

        static void requestShutdown();

        // builds started off the main thread and not finished yet (ShaderPool::isCreationIdle)
        static size_t getRunningBuildCount();

        // Serialize ShaderData cache on demand (no disk I/O inside buildShaderInternal).
        static bool saveShaderDataCache(ShaderKey shaderKey, Project* project, const ShaderData& shaderData, std::string* err = nullptr);

    };

}
