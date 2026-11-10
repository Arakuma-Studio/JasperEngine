// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Terrain's CDLOD nodes, per-view selections and size limits: what the render and mesh systems'
// headers need of a Terrain, without TerrainComponent.h and its layers (header fan-out, docs/testing.md
// "Build speed"). TerrainComponent.h includes it.

#ifndef TERRAIN_NODES_H
#define TERRAIN_NODES_H

#define MAX_TERRAINGRID 16
// CDLOD node selection is per-view: index 0 is the main camera (also reused by the
// shadow depth pass), 1..N-1 are render-to-texture cameras (mirror reflections,
// scene captures), each selecting + morphing its own node cut. Extra RTT cameras
// beyond this cap fall back to the main camera's selection (view 0).
#define MAX_TERRAIN_VIEWS 4

// The quadtree materializes rootGridSize^2 * (4^levels - 1)/3 nodes, so "levels"
// grows the node count exponentially. These bounds keep the node vector from
// requesting an impossible allocation (a large "levels" would abort with bad_alloc).
// MAX_TERRAIN_LEVELS also keeps getTerrainGridArraySize's 4^i math within size_t.
#define MAX_TERRAIN_LEVELS 20
#define MAX_TERRAIN_NODES 2000000u

// A blend map weights three layers in its RGB, and on a terrain of more than nine layers one more by
// its alpha, inverted (T3 R15): saved maps are opaque, which reads as weight 0. A shader's block keeps
// 9 entries up to 9 layers (TERRAIN_EXTENDED_LAYERS: 12).
#define MAX_TERRAIN_BLENDMAPS 3
#define MAX_TERRAIN_LAYERS 12
#define TERRAIN_SHADER_BASE_LAYERS 9

// One array texture holds every layer map, so a bigger source is downscaled into it
// instead of resizing every other slice up with it.
#define MAX_TERRAIN_DETAIL_SIZE 2048

#include "buffer/InterleavedBuffer.h"
#include "math/Vector2.h"
#include "math/Vector3.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine{

    struct TerrainNode{
        //-----u_vs_terrainNodeParams
        Vector2 position = Vector2(0, 0);
        float size = 0;
        float currentRange = 0;
        float resolution = 0; //int
        uint8_t _pad_20[12];
        //-----

        size_t childs[4];
        bool hasChilds = false;

        float maxHeight = 0;
        float minHeight = 0;
        
        float visible = false;
    };

    // Per-view CDLOD state. Index 0 is the main camera (also reused by the shadow
    // depth pass); 1..N-1 are render-to-texture cameras (mirror reflections, scene
    // captures), each selecting and morphing its own node cut. Grouping the per-view
    // buffers in a struct (instead of a 2-D array of InterleavedBuffer) keeps MSVC's
    // code generation from crashing when the ECS pool instantiates TerrainComponent.
    struct TerrainView{
        // 0 = fullRes, 1 = halfRes; selected and uploaded independently once per frame
        InterleavedBuffer nodesbuffer[2];
        // the TerrainComponent::nodes each record of nodesbuffer is, in its order (a shadow slot
        // culls them one by one, spec S55 R5)
        std::vector<uint32_t> nodeIndices[2];
        // per-view morph origin, paired with this view's node selection
        Vector3 nodesEyePos;
        bool needUpdateNodesBuffer = false;
    };

}

#endif // TERRAIN_NODES_H
