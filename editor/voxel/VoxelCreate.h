// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// GameObject > 3D Object > Voxel Volume (docs/specs/T6-voxel-volumes.md R59): what the dialog asks, its
// live memory estimate and warnings, and making the volume, its file next to the scene's assets (S04
// GUID), in one undo step. The dialog draws this; the tests call it.

#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "voxel/VoxelGeneration.h"
#include "voxel/VoxelGenerator.h"
#include "voxel/VoxelTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct VoxelProjectSettings;

    enum class VoxelCreatePreset : uint8_t { Small, Medium, Large, Custom };
    enum class VoxelCreateBase : uint8_t { Empty, SolidGround, FromTerrain, Caves, Overhangs, CaveEntrance, NoiseGraph, FromMesh };

    const char* voxelCreatePresetLabel(VoxelCreatePreset preset);
    const char* voxelCreateBaseLabel(VoxelCreateBase base);
    // Small 64 cubed, Medium 128 x 64 x 128, Large 256 x 128 x 256. Custom: {} stays what it is.
    voxel::Int3 voxelCreatePresetSize(VoxelCreatePreset preset);

    struct VoxelCreateOptions {
        std::string name = "Voxel Volume";
        VoxelCreatePreset preset = VoxelCreatePreset::Small;
        voxel::Int3 dimensions{64, 64, 64};   // read for Custom; multiples of 16, rounded up
        float voxelSize = 1.0f;
        VoxelCreateBase base = VoxelCreateBase::Empty;
        uint64_t seed = 0;
        Entity terrain = NULL_ENTITY;
        std::string graphPath;                // Noise Graph..., relative to the assets
        Entity mesh = NULL_ENTITY;            // From Mesh...: a GameObject of the scene with a Mesh
        Vector3 position = Vector3(0, 0, 0);
        Entity parent = NULL_ENTITY;
    };

    // The dimensions the options come to
    voxel::Int3 voxelCreateDimensions(const VoxelCreateOptions& options);
    // The generator the options describe
    voxel::GeneratorDesc voxelCreateGenerator(const VoxelCreateOptions& options);
    // Whether the base needs the generation steps (VoxelGeneration); the rest resolve on their own
    bool voxelCreateNeedsGeneration(const VoxelCreateOptions& options);
    // What those steps are asked
    VoxelGenerateRequest voxelCreateRequest(const VoxelCreateOptions& options);

    struct VoxelCreateEstimate {
        int64_t bytes = 0;
        // buildPlatformName of each target it is over the budget or a dimension of
        std::vector<std::string> overBudgetTargets;
        std::vector<std::string> oversizeTargets;
        // "" when nothing is over; else the line the dialog shows in its warning colour
        std::string warning;
    };
    // R59's live estimate: a table of 8 bytes per block per level, 16 KB per block a base of this kind
    // is expected to leave non-uniform
    VoxelCreateEstimate estimateVoxelCreate(const VoxelCreateOptions& options, const VoxelProjectSettings& settings);

    // "" when the options can make a volume; else what is wrong with them
    std::string voxelCreateProblem(const VoxelCreateOptions& options);

    // Makes the file and the GameObject, one undo step named "Create Voxel Volume". The file stays when
    // the step is undone, so a redo finds it. NULL_ENTITY with `error` when it cannot.
    Entity createVoxelVolume(Project* project, uint32_t sceneId, const VoxelCreateOptions& options, std::string* error);

}
