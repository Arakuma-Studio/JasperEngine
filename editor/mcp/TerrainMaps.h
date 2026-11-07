// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ecs/Entity.h"
#include "texture/Texture.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace jasperengine::editor {
class Project;
}

namespace jasperengine::editor::mcp {

struct TerrainHeightmapRequest {
    int resolution = 512;
    std::string mode = "middle";
    uint32_t seed = 0;
    float baseHeight = 0.5f;
    float amplitude = 0.35f;
    float frequency = 4.0f;
    int octaves = 4;
};

struct TerrainHeightmapResult {
    bool success = false;
    std::string error;
    std::string relativePath;
    Texture texture;
};

struct TerrainBlendmapRequest {
    int resolution = 512;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
};

struct TerrainBlendmapResult {
    bool success = false;
    std::string error;
    std::string relativePath;
    std::filesystem::path fullPath;
    std::string encoded;    // the PNG bytes (written to fullPath unless the caller writes them)
};

struct TerrainDensityMapRequest {
    int resolution = 512;
    float fill = 1.0f;      // 0-1: the share of the layer's density everywhere
};

// Generated terrain maps (set_terrain_maps, edit_terrain_details): 16-bit heightmaps, constant RGB
// blend maps and constant detail density maps written as PNGs into the project's terrain maps directory
class TerrainMaps {
public:
    static TerrainHeightmapResult createHeightmap(Project* project,
                                                  uint32_t sceneId,
                                                  Entity entity,
                                                  const TerrainHeightmapRequest& request);

    static TerrainBlendmapResult createBlendmap(Project* project,
                                                uint32_t sceneId,
                                                Entity entity,
                                                const TerrainBlendmapRequest& request);

    // A detail layer's density map, the same grey everywhere: encoded, not written (edit_terrain_details
    // writes it in its undoable step); named after the layer so maps of one call never share a path
    static TerrainBlendmapResult encodeDensityMap(Project* project,
                                                  uint32_t sceneId,
                                                  Entity entity,
                                                  int layer,
                                                  const TerrainDensityMapRequest& request);
};

} // namespace jasperengine::editor::mcp
