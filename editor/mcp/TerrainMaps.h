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
};

// Generated terrain maps (set_terrain_maps): 16-bit heightmaps and constant RGB blend maps
// written as PNGs into the project's terrain maps directory
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
};

} // namespace jasperengine::editor::mcp
