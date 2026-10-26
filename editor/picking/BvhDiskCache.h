// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor's disk cache of picking BVHs (docs/specs/S26-scene-view.md R10): a mesh of 65,536
// triangles or more keeps its tree in .jasperengine/cache/meshbvh/<hash>.bvh, so opening the
// project again does not rebuild it. Files are written as S03 Cache writes; a bad, truncated
// or stale one (another format version, hash, triangle count or a damaged payload) is rebuilt,
// never trusted. The directory is kept under 512 MB, least recently used first out. Exported
// games have no disk cache (PickCache's hook is unset there).

#include "picking/PickCache.h"

#include <cstdint>
#include <filesystem>
#include <mutex>

namespace jasperengine::editor {

    class BvhDiskCache : public PickDiskCache {
    public:
        static constexpr uint32_t FILE_MAGIC = 0x4342454Au;   // "JEBC"
        static constexpr uint32_t FILE_VERSION = 1u;
        static constexpr uint64_t DEFAULT_CAPACITY = 512ull * 1024ull * 1024ull;

        explicit BvhDiskCache(std::filesystem::path directory, uint64_t capacityBytes = DEFAULT_CAPACITY);

        // .jasperengine/cache/meshbvh of a project
        static std::filesystem::path directoryFor(const std::filesystem::path& projectPath);

        bool load(uint64_t hash, const PickGeometry& geometry, MeshBvh& bvh) override;
        void store(uint64_t hash, const MeshBvh& bvh) override;

        std::filesystem::path pathFor(uint64_t hash) const;
        const std::filesystem::path& getDirectory() const { return directory; }
        // the files' total size, then evicting the least recently used ones beyond the capacity
        uint64_t enforceCapacity();

    private:
        std::filesystem::path directory;
        uint64_t capacity;
        std::mutex mutex;
    };

}
