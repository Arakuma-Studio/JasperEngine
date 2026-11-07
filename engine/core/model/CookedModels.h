// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Where a model's cooked file is (spec S53 R12). An exported game reads
// assets/je-cooked/models/index.json, which maps each asset-relative source path to the cooked
// .glb of each platform family (identical cooks share a file) and whether the player keeps its
// CPU copy after upload (Read/Write, R11); the editor installs a provider that answers from its
// cook cache instead. Paths not in the index load raw. MeshSystem::getImportedModelPath is the
// one place every reader asks.

#ifndef COOKEDMODELS_H
#define COOKEDMODELS_H

#include "Export.h"

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine {

    struct CookedModelEntry {
        std::string file;               // the cooked .glb: a path FileData opens
        uint64_t bytes = 0;             // its size
        uint64_t sourceBytes = 0;
        // Read/Write: false when nothing at runtime reads the mesh data on the CPU, so a player
        // releases the parsed buffers once they are on the GPU (the editor always keeps them)
        bool keepCpuData = true;
    };

    class JASPERENGINE_API CookedModels {
    public:
        static constexpr const char* kIndexFile = "je-cooked/models/index.json";
        static constexpr const char* kCookedDir = "je-cooked/models";
        // The cooked format a player decodes (player-info.json modelCookFormats): glb files with
        // KHR_meshopt_compression, KHR_mesh_quantization and JE_mesh_lods. A cook that needs
        // more bumps it, and the export refuses players without it (R30).
        static constexpr int kFormatVersion = 1;

        // The cooked file of a model source path (asset-relative, "asset://" or an absolute
        // path under the assets directory), for this build's family
        static bool find(const std::string& sourcePath, CookedModelEntry& out);

        // the editor's cook cache; null goes back to the export's index.json
        using Provider = std::function<bool(const std::string& assetPath, CookedModelEntry& out)>;
        static void setProvider(Provider provider);
        static bool hasProvider();
        static void reset();
    };

}

#endif // COOKEDMODELS_H
