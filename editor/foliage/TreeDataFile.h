// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A Terrain's trees on disk (T5 R22, R61, R62): terrain_edit_trees_<scene>_<entity>_<serial>.jetrees in
// the maps folder, so the orphan collector handles them. Written with writeFileAtomic and reported
// through S05's self-write suppression, flushed when the maps flush. A copy that shares a file gets
// its own at its first write, as painted maps do.

#include "ecs/Entity.h"
#include "terrain/TreeStore.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class TreeDataFile {
    public:
        // The store has changes; the Terrain gets a path it owns if it has none
        static void markDirty(Project* project, uint32_t sceneId, Entity terrain);
        // Every dirty store of every scene. False when a write failed; a refused file stays unwritten.
        static bool flushAll(Project* project, std::string* error = nullptr);
        static bool flush(Project* project, uint32_t sceneId, Entity terrain, std::string* error = nullptr);
        // Whether this path is the kind this class writes for this terrain (a duplicate's is not)
        static bool isOwnedPath(const std::string& path, uint32_t sceneId, Entity terrain);
        // The path of a new file in the maps folder, relative to the assets as the component stores it
        static std::string makePath(Project* project, uint32_t sceneId, Entity terrain);
        // The bytes, compressed above the store's limit
        static void encode(const terrain::TreeStore& store, std::vector<uint8_t>& bytes);
    };

}
