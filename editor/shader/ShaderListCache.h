// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "shader/ShaderBuildTypes.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // What Project::refreshClosedSceneShaderKeys computed of each closed scene (docs/specs/
    // S44-shader-lists.md): valid while the scene's file has the same hash and what its keys also
    // read (prefabs, models, scripts...: the context, Project::shaderListContext) did not change.
    // Kept in the project's .jasperengine folder for the next session; a file that does not read
    // only costs computing them again.
    class ShaderListCache {
    public:
        struct Entry {
            std::string sha1;           // of the scene file's bytes
            std::int64_t writeTime = 0; // its stamp when hashed: the same stamp needs no new hash
            std::uintmax_t size = 0;
            std::string context;
            std::set<ShaderKey> keys;
            std::vector<std::string> prefabs; // what the scene lists (Project::updateScenePrefabs)
        };

        const Entry* find(const std::string& scene) const;
        void store(const std::string& scene, Entry entry);
        // The same bytes under a new stamp
        void updateStamp(const std::string& scene, std::int64_t writeTime, std::uintmax_t size);
        void clear();

        bool isDirty() const { return dirty; }
        // false: no usable file (the cache is then empty)
        bool load(const std::filesystem::path& file);
        // false: not written (the cache stays dirty)
        bool save(const std::filesystem::path& file);

    private:
        std::map<std::string, Entry> entries;
        bool dirty = false;
    };

}
