// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T3: what the render system decides and holds for a terrain, for readers that do not take
// RenderSystem.h, whose weight counts against the component headers' fan-out (tools/check_fanout.py).
// Defined in RenderSystemTerrain.cpp.

#include "Export.h"
#include "ecs/Entity.h"
#include "terrain/TerrainQuality.h"
#include "texture/TextureFormat.h"

#include <cstdint>

namespace jasperengine {

    class Scene;
    struct TerrainComponent;

    // The terrain's inputs to its variant (D2): its own Quality, its layers and the features it turns on
    JASPERENGINE_API terrain::TerrainVariantInputs getTerrainVariantInputs(const TerrainComponent& terrain);
    // R43: what the scene's render system built as the terrain's detail array; false when it has none
    JASPERENGINE_API bool getTerrainDetailArrayInfo(Scene* scene, Entity entity, GpuTextureFormat& format, int& slices, uint64_t& bytes);

}
