// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Which debug view a Voxel Volume shows in the Scene view (docs/specs/T6-voxel-volumes.md R64): the
// panel's and MCP's visualize_voxels' one switch. Editor only, never saved. With none on, the Scene
// view pays one bool per frame (R58).

#include "ecs/Entity.h"

#include <cstdint>
#include <string_view>

namespace jasperengine::editor {

    enum class VoxelOverlay : uint8_t {
        None,
        Wireframe,         // the meshes on screen, as lines
        LodColours,        // the same, coloured by the level each block is drawn at
        Blocks,            // the block boxes on screen, coloured by level
        Collision,         // the level-0 collision geometry around the focus
        Holes,             // the terrain holes this volume cut (T3 R30's)
        DominantMaterial   // the meshes on screen, each triangle in its dominant slot's colour
    };

    // owner: the volume's GameObject, in the scene the Scene view shows
    void setVoxelOverlay(Entity owner, VoxelOverlay overlay);
    VoxelOverlay getVoxelOverlay(Entity owner);
    // every volume's back to none
    void clearVoxelOverlays();
    bool anyVoxelOverlay();
    // Bumped by each change: the Scene view redraws its lines then, or when the meshes moved on
    uint64_t voxelOverlayRevision();

    // The stable name (wireframe, lod_colours, blocks, collision, holes, dominant_material; none)
    const char* voxelOverlayName(VoxelOverlay overlay);
    // The label in the panel ("LOD Colours")
    const char* voxelOverlayLabel(VoxelOverlay overlay);
    // The name above; false for anything else
    bool parseVoxelOverlay(std::string_view text, VoxelOverlay& overlay);

    inline constexpr int VOXEL_OVERLAY_COUNT = 7;

}
