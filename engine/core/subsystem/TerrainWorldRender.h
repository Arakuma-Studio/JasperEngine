// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_WORLD_RENDER_H
#define TERRAIN_WORLD_RENDER_H

// T4 R7-R13, D4-D6: what a Terrain World's runtime holds for drawing: a page per resident tile
// (height in RG8, blend and hole maps), the overview's two textures, and each view's selection with
// its draw items. RenderSystemTerrainWorld.cpp fills and draws it.

#include "Export.h"

#include <cstddef>
#include <memory>

namespace jasperengine {

    struct TerrainComponent;
    class TerrainWorldRuntime;

    // R36: nodes a grid of a view holds at most
    inline constexpr size_t TERRAIN_WORLD_MAX_NODES = 4096;

    // D14: a World's hole variant is the world's (hasHoles), not per tile
    JASPERENGINE_API bool terrainWorldHasHoles(const TerrainComponent& terrain);

    class JASPERENGINE_API TerrainWorldRender {
    public:
        explicit TerrainWorldRender(TerrainWorldRuntime& runtime);
        ~TerrainWorldRender();

        TerrainWorldRender(const TerrainWorldRender&) = delete;
        TerrainWorldRender& operator=(const TerrainWorldRender&) = delete;

        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}

#endif // TERRAIN_WORLD_RENDER_H
