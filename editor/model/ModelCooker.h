// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The model cook (spec S53 R9, R10, R12): a glTF (or an FBX's import) to a valid, node-preserving
// glb, with meshoptimizer's passes in Unity's Model Importer names.
//   - Nodes, meshes, primitives, materials, skins and animations keep their order and count,
//     and every accessor keeps its index (only its data moves): scenes, sub-clip paths and
//     submesh overrides need no migration. The cook checks this on its own output and refuses
//     to write a file that breaks it.
//   - Optimize Mesh: exact duplicates welded (Weld Vertices), the vertex cache and overdraw
//     orders (threshold 1.05) and the fetch order, every stream remapped (morph targets too). A
//     primitive drawn blended keeps its triangle order; a primitive whose vertex data is shared
//     with other users keeps its vertex order (both are reported).
//   - Normals Calculate: area-weighted, with the Smoothing Angle as the crease.
//   - Index Format, Generate LODs (JE_mesh_lods, built with MeshLodPool::buildLevels: contract
//     C1/C2), Mesh Compression (KHR_meshopt_compression, KHR_mesh_quantization for the lossy
//     levels: positions as EXPONENTIAL floats, normals and tangents OCTAHEDRAL).
// The source is never written. A file the cook cannot rewrite safely (an extension of its own
// pointing at bufferViews) ships as it is, with the reason in its record.

#pragma once

#include "model/ModelCookCache.h"
#include "model/ModelImportSettings.h"

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    struct ModelCookRequest {
        std::filesystem::path source;       // the file the cook reads (an FBX's import .glb)
        std::string assetPath;              // as messages name it ("models/hero.glb")
        ResolvedModelSettings settings;
        ModelFamily family = ModelFamily::Desktop;
        // (mesh, primitive) that a scene draws blended: their triangle order is kept. A mesh of
        // -1 is the primitive of that index in every mesh (a scene override names submeshes).
        std::set<std::pair<int, int>> blended;
    };

    struct ModelCookResult {
        bool ok = false;
        bool cacheHit = false;
        bool cancelled = false;
        std::string error;
        std::string key;
        ModelCookRecord record;
    };

    class ModelCooker {
    public:
        // .gltf, .glb and .fbx (an FBX cooks from its import); OBJ loads through the loader's
        // dedupe (R7) and is not cooked
        static bool isCookableModel(const std::string& path);
        // the files whose bytes are the source: the main file's external buffers (relative
        // .bin files), read from its JSON only
        static bool sourceFiles(const std::filesystem::path& source, std::vector<std::filesystem::path>& extra, std::string& error);

        // The cache key of a request (hashes the source through the cache's sources table)
        static bool planKey(ModelCookCache& cache, const ModelCookRequest& request, std::string& key, std::string& error);
        // Cooks into the cache (a hit returns its record); `cancelled` is polled between passes
        static ModelCookResult cook(ModelCookCache& cache, const ModelCookRequest& request, const std::function<bool()>& cancelled);
        // The cook without a cache: the glb and its record (the key left empty)
        static bool cookToMemory(const ModelCookRequest& request, std::vector<uint8_t>& glb, ModelCookRecord& record, std::string& error,
                                 const std::function<bool()>& cancelled = nullptr);

        // The bits each lossy level keeps: position, texture coordinate, normal/tangent, colour
        struct QuantizationBits { int position = 0, uv = 0, normal = 0, color = 0; };
        static QuantizationBits quantizationBits(MeshCompression compression);
    };

}
