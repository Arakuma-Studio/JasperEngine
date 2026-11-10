// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// R33-R37, R71: giving a volume a new base. Three steps, since it is long work and the store's tables
// belong to the main thread: prepare on the main thread (validates, compiles, makes a private copy
// of the store), run on a job (resolves every block it touches, so the result has real numbers), finish
// on the main thread (swaps the result in, returning the undo state). The same steps serve MCP's
// generate_voxels, the Create dialog and the panel's Regenerate.

#include "subsystem/VoxelSystem.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {

    struct VoxelGenerateRequest {
        std::string source;        // noise_graph, terrain, mesh, preset
        std::string graphPath;     // noise_graph: a .noisegraph, project-relative
        uint64_t seed = 0;
        Entity terrain = NULL_ENTITY;
        double blendWidth = 0.0;   // metres
        Entity mesh = NULL_ENTITY; // mesh: a GameObject's mesh
        std::string meshPath;      // or an asset, project-relative
        double meshTransform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::string preset;        // caves, overhangs, cave_entrance, empty, solid_ground
        double sizeScale = 1.0;
        bool keepEdits = true;
        bool dryRun = false;       // a coarse level only, and it says it is an estimate
        bool hasRegion = false;
        voxel::IntBox region;      // voxels; what to resolve, all of it by default
    };

    struct VoxelGenerateResult {
        bool ok = false;
        std::string error;         // a code: missing_asset, invalid_source, grid_only_node, ...
        std::string message;       // what to do about it; names the node
        int64_t blocksNonUniform = 0;
        int64_t memory = 0;
        int64_t trianglesEstimate = 0;
        uint64_t contentHash = 0;
        bool baseChanged = false;
        bool nonWatertight = false;
        bool estimate = false;     // dry run
    };

    // What one undo step holds (R71): the old base description and every slot it replaced
    struct VoxelGenerateUndo {
        voxel::GeneratorDesc oldGenerator;
        std::shared_ptr<const voxel::BaseSource> oldBase;
        std::vector<voxel::SlotChange> slots;
    };

    class VoxelGeneration {
    public:
        // Main thread. On failure the result says why, and run and finish must not be called.
        static std::shared_ptr<VoxelGeneration> prepare(Scene* scene, Entity owner, const VoxelGenerateRequest& request, VoxelGenerateResult& result);
        // Any thread. cancelled is polled between blocks: true leaves everything as it was.
        void run(const std::function<bool()>& cancelled, const std::function<void(float)>& progress);
        // Main thread. Applies it, unless a dry run or cancelled, and reports the numbers.
        VoxelGenerateResult finish(Scene* scene, VoxelGenerateUndo* undo);
        // Puts an undo state back, or forward with the state finish returned
        static void applyUndo(Scene* scene, Entity owner, const VoxelGenerateUndo& undo, bool undoing);

        VoxelGeneration() = default;

    private:
        struct State;
        std::shared_ptr<State> state;
    };

    // V10: has the source changed since the generator's stored hash? Never changes data by itself:
    // the panel and sample_voxels report base_stale, and Regenerate is explicit. A base with no
    // source to compare (empty, solid ground, a preset of ops) is never stale.
    bool isVoxelBaseStale(Scene* scene, Entity owner);

    // R33: the base a volume's file describes, rebuilt when the file is opened again (the editor gives it
    // to VoxelAssets::setBaseResolver). Null when only an attached Terrain can make it.
    std::shared_ptr<const voxel::BaseSource> resolveVoxelBase(const voxel::GeneratorDesc& generator, const voxel::VolumeDesc& desc);

    // R37: the presets, as editor::TerrainPresets lists T2's
    struct VoxelPresetInfo {
        std::string name;
        std::string label;
        std::string description;
    };
    const std::vector<VoxelPresetInfo>& voxelPresets();

}
