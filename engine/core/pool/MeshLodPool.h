// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MESHLODPOOL_H
#define MESHLODPOOL_H

#include "Export.h"
#include "Engine.h"
#include "render/Render.h"
#include "jobs/JobSystem.h"
#include <cfloat>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace jasperengine{

    struct MeshLodLevel{
        unsigned int indexBase = 0;
        unsigned int indexCount = 0;
        float error = 0; // deviation from the source, in mesh units
    };

    // simplified index ranges of one submesh over its own vertex buffer
    struct MeshLodData{
        std::vector<unsigned char> indices; // packed in the source index type
        AttributeDataType indexType = AttributeDataType::UNSIGNED_SHORT;
        MeshLodLevel levels[MAX_MESH_LODS]; // level 0 is the source and stays empty
        unsigned int numLevels = 1;
    };

    struct MeshLodSource{
        std::vector<float> positions;  // xyz
        std::vector<float> attributes; // normal xyz + uv, when present
        unsigned int attributeCount = 0;
        std::vector<uint32_t> indices;
        AttributeDataType indexType = AttributeDataType::UNSIGNED_SHORT;
    };

    // How the levels of a mesh are cut: the runtime's (S54 owns it) or, in the model cook, the
    // importer's LOD Count and Reduction (S53 R10)
    struct MeshLodRecipe{
        unsigned int levels = MAX_MESH_LODS - 1; // levels after LOD 0, at most MAX_MESH_LODS - 1
        float ratio = 0.5f;                      // share of the triangles of the level before that a level keeps
        float targetError = FLT_MAX;             // the most a level may deviate, as a share of the mesh's size
        bool permissive = false;                 // collapses across attribute seams too (meshopt_SimplifyPermissive)

        // the levels the runtime builds when nothing baked them
        static MeshLodRecipe runtime();
    };

    // The version of the runtime's recipe and level builder. A cooked model's baked levels
    // (JE_mesh_lods.recipe) of another version are not used, and the cook's cache key carries it.
    constexpr int kMeshLodRecipe = 1;

    typedef std::unordered_map< uint64_t, jobs::Task<std::shared_ptr<MeshLodData>> > lods_t;

    // Detail levels keyed by a hash of the geometry, simplified as Background Heavy jobs
    class JASPERENGINE_API MeshLodPool{
    private:
        static lods_t& getMap();
        static std::mutex& getMutex();

    public:
        // The levels of one submesh: the pure function the runtime's builds and the model cook's
        // baked levels share (no pool, no job)
        static std::shared_ptr<MeshLodData> buildLevels(const MeshLodSource& source, const MeshLodRecipe& recipe);

        static uint64_t hash(uint64_t seed, const void* data, size_t size);

        static std::shared_ptr<MeshLodData> get(uint64_t key); // null while a build is pending
        static bool has(uint64_t key);
        static void build(uint64_t key, MeshLodSource&& source);

        // necessary for engine shutdown
        static void clear();
        // removes only entries no one else references (safe while other scenes are alive)
        static void clearUnused();
    };
}

#endif //MESHLODPOOL_H
