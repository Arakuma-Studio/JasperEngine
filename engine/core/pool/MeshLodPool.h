// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MESHLODPOOL_H
#define MESHLODPOOL_H

#include "Export.h"
#include "Engine.h"
#include "render/Render.h"
#include "jobs/JobSystem.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tinygltf{
    class Model;
    struct Primitive;
}

namespace jasperengine{

    struct MeshLodLevel{
        unsigned int indexBase = 0;
        unsigned int indexCount = 0;
        // the most the level's surface lies from the source's and back, in mesh units: a geometric
        // deviation sampled at the vertices, edge midpoints and centroids of both (spec S54 R2)
        float error = 0;
    };

    // simplified index ranges of one submesh over its own vertex buffer
    struct MeshLodData{
        std::vector<unsigned char> indices; // packed in the source index type
        AttributeDataType indexType = AttributeDataType::UNSIGNED_SHORT;
        MeshLodLevel levels[MAX_MESH_LODS]; // level 0 is the source and stays empty
        unsigned int numLevels = 1;
        // read from a cooked model (JE_mesh_lods), not simplified here
        bool baked = false;
    };

    // What the levels are cut from: the positions and the attributes that guide the collapses,
    // each empty when the mesh has none
    struct MeshLodSource{
        std::vector<float> positions;   // xyz
        std::vector<float> normals;     // xyz
        std::vector<float> uvs;         // uv (the first set)
        std::vector<float> skinWeights; // the four bone weights of a skinned mesh
        std::vector<uint32_t> indices;
        AttributeDataType indexType = AttributeDataType::UNSIGNED_SHORT;
    };

    // the Model Importer's Reduction (spec S53 R10): how much each level keeps and may deviate
    enum class MeshLodReduction{
        PRESERVE_DETAIL,
        BALANCED,
        SMALLEST
    };

    // How the levels of a mesh are cut: the runtime's (when nothing baked them) or, in the model
    // cook, the importer's LOD Count and Reduction
    struct MeshLodRecipe{
        unsigned int levels = MAX_MESH_LODS - 1; // levels after LOD 0, at most MAX_MESH_LODS - 1
        float ratio = 0.5f;                      // share of the triangles of the level before that a level keeps
        float targetError = 0.02f;               // the most a level may deviate, as a share of the mesh's size
        bool permissive = false;                 // collapses across attribute seams too (meshopt_SimplifyPermissive)

        // the levels the runtime builds when nothing baked them (Balanced, 3 levels)
        static MeshLodRecipe runtime();
        // levels after LOD 0 (1-3) at a Reduction: Preserve Detail keeps 0.6 a level within 1 % of
        // the size, Balanced 0.5 within 2 %, Smallest 0.35 within 5 % and across seams
        static MeshLodRecipe forReduction(MeshLodReduction reduction, unsigned int levels);
    };

    // The version of the level builder (buildLevels and its recipes). A cooked model's baked
    // levels (JE_mesh_lods.recipe) of another version are not used: the runtime builds its own.
    // The cook's cache key carries it. 2: geometric errors, weights by attribute, error budgets,
    // permissive retry, skinned meshes (spec S54 R2)
    constexpr int kMeshLodRecipe = 2;

    // the glTF extension of a cooked primitive's baked levels (spec S54 contract C1)
    constexpr const char* MESH_LODS_GLTF_EXTENSION = "JE_mesh_lods";

    // a level of the cache, or the job building it
    struct MeshLodEntry{
        jobs::Task<std::shared_ptr<MeshLodData>> task;
        std::shared_ptr<MeshLodData> data; // set at once for baked, refused or failed builds
    };
    typedef std::unordered_map< uint64_t, MeshLodEntry > lods_t;

    // Detail levels keyed by a hash of the geometry, simplified as Background Heavy jobs
    class JASPERENGINE_API MeshLodPool{
    private:
        static lods_t& getMap();
        static std::mutex& getMutex();

    public:
        // The levels of one submesh: the pure function the runtime's builds and the model cook's
        // baked levels share (no pool, no job). Levels are cut from the source while they keep
        // shrinking; a result without any (numLevels 1) means the source could not be reduced
        static std::shared_ptr<MeshLodData> buildLevels(const MeshLodSource& source, const MeshLodRecipe& recipe);
        // the deviation buildLevels gives a level (the source's indices and a level's over the
        // same positions, in mesh units)
        // the simplifier's weight of each attribute float buildLevels passes, in its order:
        // normal xyz, uv, the four bone weights, each only when the source has it
        static std::vector<float> attributeWeights(bool normals, bool uvs, bool skinned);
        static float measureDeviation(const std::vector<float>& positions, const std::vector<uint32_t>& source, const std::vector<uint32_t>& level);

        static uint64_t hash(uint64_t seed, const void* data, size_t size);

        // null while a build is pending or when the key is not (or no longer) in the pool
        static std::shared_ptr<MeshLodData> get(uint64_t key);
        static bool has(uint64_t key);
        // blocks until the key's build ends, running it here when no worker has started it
        // (capture_model_lods, tests); false when the key is not in the pool
        static bool wait(uint64_t key);
        static void build(uint64_t key, MeshLodSource&& source);
        // levels known without a build (baked in a cooked model, or none for a source that cannot
        // have any): get(key) returns them at once
        static void prefill(uint64_t key, std::shared_ptr<MeshLodData> data);
        // the simplify jobs started since the process began (tests, diagnostics)
        static uint64_t getBuildCount();

        // The levels of a cooked glTF primitive (JE_mesh_lods) in the primitive's index type, null
        // when it has none or they are unusable; recipe is the version they were built with
        static std::shared_ptr<MeshLodData> readGltfLevels(const tinygltf::Model& model, const tinygltf::Primitive& primitive,
                                                           AttributeDataType indexType, size_t vertexCount, int& recipe);

        // necessary for engine shutdown
        static void clear();
        // removes only entries no one else references (safe while other scenes are alive); a
        // submesh that had not taken its finished levels yet builds them again
        static void clearUnused();
    };
}

#endif //MESHLODPOOL_H
