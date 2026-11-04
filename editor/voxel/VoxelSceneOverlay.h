// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What the Scene view draws for Voxel Volumes (docs/specs/T6-voxel-volumes.md R60, R63, R64): the brush
// decal, the Slice view's plane, and the overlays setVoxelOverlay turns on. Editor-only objects in the
// scene they annotate, drawn by the Scene view and never a game or camera image. Made on first use, so a
// scene with none of it has nothing to pay (R58).

#include "VoxelSliceRender.h"

#include "ecs/Entity.h"

#include <cstddef>
#include <cstdint>

namespace jasperengine {
    class Scene;
    class SceneRender;
}

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    // The Slice view's plane as a gizmo (R63)
    void setVoxelSlicePlane(uint32_t sceneId, Entity owner, VoxelSliceAxis axis, int position);
    void clearVoxelSlicePlane();

    class VoxelSceneOverlay {
    public:
        // Each frame, from the panel's tick: draws what is on, for the open scenes
        static void frame(Project& project);
        // The SceneRender of this scene is going: its objects go first, while the scene is alive
        static void release(Scene* scene);
        // Line and triangle counts of an overlay as last drawn (tests, visualize_voxels' numbers)
        static size_t overlayLineCount(Scene* scene);
        static size_t overlayTriangleCount(Scene* scene);

        // ---- pure: what a mesh's triangles and a block's lines are
        // A level's colour in LOD Colours and Blocks: finest green, through yellow and orange, to red
        struct Colour {
            float r = 1.0f, g = 1.0f, b = 1.0f;
        };
        static Colour levelColour(int level);
    };

}
