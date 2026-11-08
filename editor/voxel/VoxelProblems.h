// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What is wrong with a Voxel Volume, as the red strip in the panel and the Inspector tells it
// (docs/specs/T6-voxel-volumes.md R61): one code per thing, what to do about it, and the matching log
// line once. A failed load keeps the file untouched (saveVoxelAsset refuses it), so nothing here writes.

#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    enum class VoxelProblemCode : uint8_t {
        NoAsset,            // no .voxelvolume assigned
        MissingFile,        // assigned, and not on disk
        CorruptFile,        // on disk, and it did not load
        MissingNoiseGraph,  // the generator's graph is gone
        MissingTerrain,     // the generator's or the component's terrain is gone
        BaseStale,          // the base could not be built from the generator as it is now (R33, V10)
        OverBudget,         // memory over a target's budget
        OversizeForTarget,  // a dimension over a target's limit (R7)
        NoSlots,            // no material slots
        ModuleOff,          // the running build has no voxel module
        NonUnitScale        // R1: a volume's Transform takes no scale
    };

    // "no_asset", "missing_file", ...: the name the tools and the log use
    const char* voxelProblemName(VoxelProblemCode code);

    struct VoxelProblem {
        VoxelProblemCode code = VoxelProblemCode::NoAsset;
        std::string what;      // what is wrong, naming the file or the target
        std::string action;    // what to do about it
    };

    // moduleOn: is the voxel module compiled in (this build's answer is the default)
    std::vector<VoxelProblem> voxelProblems(Project& project, const SceneProject& sceneProject, Entity owner,
#if JASPERENGINE_VOXEL
                                            bool moduleOn = true
#else
                                            bool moduleOn = false
#endif
    );

    // The log lines of these problems, each code once per volume until it goes away and comes back.
    // Returns how many it wrote.
    int logVoxelProblems(const SceneProject& sceneProject, Entity owner, const std::vector<VoxelProblem>& problems);
    // Forgets what was logged (a project closing)
    void resetVoxelProblemLog();

}
