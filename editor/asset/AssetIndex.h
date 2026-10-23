// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The typed dependency index (S04 R12): every reference from a scene, prefab, material,
// project.yaml, HTML, CSS, Lua, shader or glTF file to an asset, as DepEdges with field
// locators. Closed files are read from disk (AssetRefSchema's YAML extractor, the text
// scanners); a loaded document with unsaved edits contributes its in-memory edges instead,
// through the overlay the Project provides. It is a cache (.jasperengine/assetdb/deps.json,
// per-referrer fingerprints): deleting it changes no query result. The first full build
// runs on a background thread; queries before it ends see isReady() false ("Indexing...").

#pragma once

#include "asset/AssetTypes.h"
#include "asset/DepEdge.h"
#include "util/AssetDatabase.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace jasperengine::editor {

    class AssetIndex {
    public:
        // The roots that relative hints are resolved against, relative to the project ("" = root)
        struct Roots {
            std::string assetsDir;
            std::string luaDir;
        };

        // In-memory edges by referrer, replacing the file's edges while it has unsaved edits
        using OverlayProvider = std::function<void(std::unordered_map<Guid, std::vector<DepEdge>>& edges)>;

        ~AssetIndex();

        void open(AssetDatabase* database, const std::filesystem::path& projectPath, const Roots& roots);
        void close();
        bool isOpen() const;
        // The first full build finished
        bool isReady() const;
        void waitUntilReady();
        void setOverlayProvider(OverlayProvider provider);

        // Re-reads the referrers the changes touched (synchronously: one file is cheap)
        void update(const std::vector<AssetDatabase::Change>& changes);
        void updateReferrer(const std::string& projectRelativePath);

        // Edges into an asset (by GUID, its sub-assets included; or by path for path edges),
        // following referrers transitively when asked (who uses the material that uses this
        // texture). The edges of a transitive hop have the intermediate asset as target.
        std::vector<DepEdge> referencesTo(const Guid& target, const std::string& targetPath, bool transitive, size_t limit) const;
        // Edges out of a referrer, depth levels deep
        std::vector<DepEdge> dependenciesOf(const Guid& referrer, int depth) const;
        // Every edge, the overlay applied (the Missing Assets tracker walks them)
        void forEachEdge(const std::function<void(const DepEdge&)>& visitor) const;
        // The edges a referrer's file holds, without the overlay (safe off the main thread:
        // the exporter checks the saved build)
        std::vector<DepEdge> fileEdgesOf(const Guid& referrer) const;

        // The path of a referrer (project.yaml for projectReferrer())
        std::string referrerPath(const Guid& referrer) const;
        // project.yaml has no GUID: its edges use this one
        static Guid projectReferrer();

        uint64_t getVersion() const;

        // The edges of one file, as the index reads them (the CLI and tests use it too)
        static std::vector<DepEdge> extractFile(const AssetDatabase& database, const std::filesystem::path& projectPath,
                                                const Roots& roots, const std::string& relativePath, AssetType type,
                                                const Guid& referrer);
        // A hint relative to a base root -> project-relative (and back)
        static std::string toProjectRelative(const Roots& roots, AssetPathBase base, const std::string& hint);
        static std::string fromProjectRelative(const Roots& roots, AssetPathBase base, const std::string& projectRelative);
        // The asset types whose files hold references
        static bool isReferrerType(AssetType type, const std::string& relativePath);

    private:
        struct Referrer {
            std::string path;
            uint64_t size = 0;
            int64_t writeTime = 0;
            std::vector<DepEdge> edges;
        };

        mutable std::mutex mutex;
        AssetDatabase* database = nullptr;
        std::filesystem::path projectPath;
        Roots roots;
        std::unordered_map<Guid, Referrer> referrers;
        // target GUID -> referrers that point at it; target path -> the same for path edges
        std::unordered_map<Guid, std::vector<Guid>> byTarget;
        std::unordered_map<std::string, std::vector<Guid>> byTargetPath;
        OverlayProvider overlay;
        std::atomic<bool> ready{false};
        std::atomic<uint64_t> version{0};

        std::thread builder;
        std::atomic<bool> stopBuild{false};
        std::condition_variable readyChanged;

        std::filesystem::path cacheFile() const;
        void loadCache();
        void saveCache() const;
        void build();
        void rebuildReverse();
        bool readReferrer(const AssetDatabase::AssetInfo& info, Referrer& referrer) const;
        std::unordered_map<Guid, std::vector<DepEdge>> overlayEdges() const;
        void collectReferences(const Guid& target, const std::string& targetPath,
                               const std::unordered_map<Guid, std::vector<DepEdge>>& memory,
                               std::vector<DepEdge>& out, size_t limit) const;
    };

}
