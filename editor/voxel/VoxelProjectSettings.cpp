// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelProjectSettings.h"

#include "subsystem/VoxelSystem.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr int64_t kMiB = 1024 * 1024;

    int readPositive(const YAML::Node& node, const char* key) {
        const YAML::Node value = node[key];
        if (!value || !value.IsScalar()) return 0;
        try {
            return std::max(value.as<int>(), 0);
        } catch (const YAML::Exception&) {
            return 0;
        }
    }

}

YAML::Node editor::encodeVoxelProjectSettings(const VoxelProjectSettings& settings) {
    YAML::Node node(YAML::NodeType::Map);
    if (settings.ignoreDimensionLimits) node["ignoreDimensionLimits"] = true;
    if (settings.maxDimension > 0) node["maxDimension"] = settings.maxDimension;
    if (settings.cpuBudgetMB > 0) node["cpuBudgetMB"] = settings.cpuBudgetMB;
    if (settings.meshBudgetMB > 0) node["meshBudgetMB"] = settings.meshBudgetMB;
    return node;
}

VoxelProjectSettings editor::decodeVoxelProjectSettings(const YAML::Node& node) {
    VoxelProjectSettings settings;
    if (!node || !node.IsMap()) return settings;
    if (const YAML::Node ignore = node["ignoreDimensionLimits"]; ignore && ignore.IsScalar()) {
        settings.ignoreDimensionLimits = ignore.as<bool>(false);
    }
    settings.maxDimension = readPositive(node, "maxDimension");
    settings.cpuBudgetMB = readPositive(node, "cpuBudgetMB");
    settings.meshBudgetMB = readPositive(node, "meshBudgetMB");
    return settings;
}

VoxelLimits editor::voxelTargetLimits(BuildPlatform target) {
    VoxelLimits limits;
    switch (target) {
        case BuildPlatform::Windows:
        case BuildPlatform::Linux:
        case BuildPlatform::MacOS:
            limits.maxDimension = 1024;
            limits.cpuBudget = 256 * kMiB;
            limits.meshBudget = 128 * kMiB;
            limits.autoTier = VoxelTier::High;
            break;
        case BuildPlatform::Android:
        case BuildPlatform::IOS:
            limits.maxDimension = 512;
            limits.cpuBudget = 64 * kMiB;
            limits.meshBudget = 32 * kMiB;
            limits.autoTier = VoxelTier::Low;
            break;
        case BuildPlatform::Web:
            limits.maxDimension = 256;
            limits.cpuBudget = 32 * kMiB;
            limits.meshBudget = 24 * kMiB;
            limits.autoTier = VoxelTier::Low;
            break;
    }
    return limits;
}

VoxelLimits editor::lowerVoxelLimits(const VoxelLimits& base, const VoxelProjectSettings& settings) {
    VoxelLimits limits = base;
    if (settings.maxDimension > 0) limits.maxDimension = std::min(limits.maxDimension, settings.maxDimension);
    if (settings.cpuBudgetMB > 0) limits.cpuBudget = std::min(limits.cpuBudget, settings.cpuBudgetMB * kMiB);
    if (settings.meshBudgetMB > 0) limits.meshBudget = std::min(limits.meshBudget, settings.meshBudgetMB * kMiB);
    return limits;
}

void editor::applyVoxelProjectSettings(const VoxelProjectSettings& settings) {
    setVoxelLimitsOverride(lowerVoxelLimits(voxelPlatformLimits(), settings));
}

std::string editor::voxelDimensionProblem(const std::string& volumeName, const voxel::Int3& dimensions, BuildPlatform target,
                                          const VoxelProjectSettings& settings) {
    if (settings.ignoreDimensionLimits) return "";
    const int limit = lowerVoxelLimits(voxelTargetLimits(target), settings).maxDimension;
    const struct { const char* axis; int value; } axes[] = {{"X", dimensions.x}, {"Y", dimensions.y}, {"Z", dimensions.z}};
    for (const auto& [axis, value] : axes) {
        if (value > limit) {
            return "Voxel Volume \"" + volumeName + "\" is " + std::to_string(value) + " voxels along " + axis + ", over the limit of " +
                   std::to_string(limit) + " for " + buildPlatformName(target) +
                   ". Make it smaller, or turn on Project Settings > Voxels > Ignore Dimension Limits.";
        }
    }
    return "";
}

std::string editor::generateVoxelSettingsCode(const VoxelProjectSettings& settings, const std::string& indent) {
    if (settings.maxDimension <= 0 && settings.cpuBudgetMB <= 0 && settings.meshBudgetMB <= 0) return "";
    // the game's own platform decides the rest, so these are comparisons and not values. The
    // module may be off in this build (S34): the settings then have nothing to lower.
    const std::string in = indent + "    ";
    std::string code = "#if JASPERENGINE_VOXEL\n" + indent + "{\n";
    code += in + "VoxelLimits voxelLimits = voxelPlatformLimits();\n";
    if (settings.maxDimension > 0) {
        const std::string value = std::to_string(settings.maxDimension);
        code += in + "if (voxelLimits.maxDimension > " + value + ") voxelLimits.maxDimension = " + value + ";\n";
    }
    if (settings.cpuBudgetMB > 0) {
        const std::string value = std::to_string(settings.cpuBudgetMB) + "ll * 1024 * 1024";
        code += in + "if (voxelLimits.cpuBudget > " + value + ") voxelLimits.cpuBudget = " + value + ";\n";
    }
    if (settings.meshBudgetMB > 0) {
        const std::string value = std::to_string(settings.meshBudgetMB) + "ll * 1024 * 1024";
        code += in + "if (voxelLimits.meshBudget > " + value + ") voxelLimits.meshBudget = " + value + ";\n";
    }
    code += in + "setVoxelLimitsOverride(voxelLimits);\n" + indent + "}\n#endif\n";
    return code;
}
