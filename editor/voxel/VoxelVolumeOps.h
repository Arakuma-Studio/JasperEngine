// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What the Inspector and the panel do to a volume besides strokes and slots
// (docs/specs/T6-voxel-volumes.md R46, R64, R65, R77), each one undo step where it changes the scene.

#include "ecs/Entity.h"
#include "voxel/VoxelEdit.h"
#include "voxel/VoxelTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    struct VoxelStats;
    struct VoxelLimits;
    class Scene;
}

namespace jasperengine::editor {

    class Project;
    struct VoxelProjectSettings;

    // ---- R65: the Inspector's rows, Unity-style labels on the component's fields

    enum class VoxelInspectorRowKind : uint8_t { Bool, Int, Float, Enum, Entity, Asset };
    struct VoxelInspectorRow {
        const char* property;   // the Catalog's name
        const char* label;
        VoxelInspectorRowKind kind;
        const char* help;
    };
    const std::vector<VoxelInspectorRow>& voxelInspectorRows();

    // ---- R65: Make Unique

    // How many GameObjects of the project's open scenes show this file. Duplicate shares it.
    int voxelAssetUserCount(Project* project, const std::string& assetPath);
    // A copy of the volume with a new GUID beside the file, as it is now, and this GameObject on it.
    // One undo step; the copy stays on disk when it is undone. The other GameObjects keep the old one.
    bool makeVoxelVolumeUnique(Project* project, uint32_t sceneId, Entity owner, std::string* error);

    // ---- R1: Apply to Voxel Size

    // The Transform's uniform scale goes into the asset's Voxel Size and the Scale back to 1. False,
    // saying why, for a scale that is not uniform or a volume that did not load. One undo step.
    bool applyScaleToVoxelSize(Project* project, uint32_t sceneId, Entity owner, std::string* error);

    // ---- R46: Re-seed From Terrain

    // Modified blocks in the box (voxels; widened to whole blocks of the coarsest level, since a coarse
    // block holds what its fine ones hold) go back to the terrain base. Null: the whole volume. One
    // undo step. Returns the blocks it changed, -1 with `error`.
    int reseedVoxelVolumeFromTerrain(Project* project, uint32_t sceneId, Entity owner, const voxel::IntBox* region, std::string* error);

    // ---- R77: the panel's Save

    bool saveVoxelVolume(Project* project, uint32_t sceneId, Entity owner, std::string* error);

    // ---- R64: the Stats panel

    struct VoxelStatsRow {
        std::string label;
        std::string value;
    };
    // Blocks by kind, memory by kind against each target's budget, triangles, chunks, draws, jobs,
    // bodies, last mesh ms, last latency: from the same numbers sample_voxels {stats} reports
    std::vector<VoxelStatsRow> voxelStatsRows(const VoxelStats& stats, const VoxelProjectSettings& settings);

    // ---- R60: what the Mesh and Stamp shapes stamp with

    // A .voxelvolume's level 0, the shape's origin at its centre (R26). Null when it did not load.
    std::shared_ptr<const voxel::StampField> voxelStampFieldFromAsset(const std::string& assetPath);
    // A GameObject's mesh voxelized at the given size (R36). Null without a mesh.
    std::shared_ptr<const voxel::StampField> voxelMeshField(Scene* scene, Entity entity, double voxelSize);

}
