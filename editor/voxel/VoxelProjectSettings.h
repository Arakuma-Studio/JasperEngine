// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Project Settings > Voxels (docs/specs/T6-voxel-volumes.md R7): what a project lowers of each
// target's limits, and the limits per target. subsystem/VoxelSystem.h knows only the platform it was
// built for, and an export looks at another one's.

#include "ShippingTypes.h"
#include "voxel/VoxelTypes.h"

#include "yaml-cpp/yaml.h"

#include <string>

namespace jasperengine {
    struct VoxelLimits;
}

namespace jasperengine::editor {

    struct VoxelProjectSettings {
        // an export goes on with a volume over its target's limit (R7)
        bool ignoreDimensionLimits = false;
        // 0: the target's. They lower it, they never raise it.
        int maxDimension = 0;
        int cpuBudgetMB = 0;
        int meshBudgetMB = 0;

        bool operator==(const VoxelProjectSettings& other) const {
            return ignoreDimensionLimits == other.ignoreDimensionLimits && maxDimension == other.maxDimension &&
                   cpuBudgetMB == other.cpuBudgetMB && meshBudgetMB == other.meshBudgetMB;
        }
        bool operator!=(const VoxelProjectSettings& other) const { return !(*this == other); }
    };

    // only what differs from the defaults: a project without any gets no `voxel` key (R80)
    YAML::Node encodeVoxelProjectSettings(const VoxelProjectSettings& settings);
    VoxelProjectSettings decodeVoxelProjectSettings(const YAML::Node& node);

    // R7's table
    VoxelLimits voxelTargetLimits(BuildPlatform target);
    // `limits` with what the settings lower
    VoxelLimits lowerVoxelLimits(const VoxelLimits& limits, const VoxelProjectSettings& settings);
    // what the running editor uses: the settings over this platform's limits
    void applyVoxelProjectSettings(const VoxelProjectSettings& settings);

    // "" when the volume fits. Names the volume, the axis and the target (R7, AC6).
    std::string voxelDimensionProblem(const std::string& volumeName, const voxel::Int3& dimensions, BuildPlatform target,
                                      const VoxelProjectSettings& settings);

    // the lines of a standalone build's main.cpp ("" when nothing is lowered, so its source does
    // not change)
    std::string generateVoxelSettingsCode(const VoxelProjectSettings& settings, const std::string& indent);

}
