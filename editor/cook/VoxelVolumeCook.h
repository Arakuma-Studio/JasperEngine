// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The cook of a .voxelvolume (docs/specs/T6-voxel-volumes.md R7, R78): a baked file holding every
// block at every level with zstd at a fixed level, so a player evaluates no graph and needs no terrain
// or mesh to load it, and the export's check of each volume against its target. The component's
// values and slots need no encoder of their own: they are on the generated field tables like a
// Terrain's layers (RuntimeFields.gen.inc), with the afterVoxelVolume hook.

#include "ShippingTypes.h"
#include "voxel/VoxelProjectSettings.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::voxel {
    class BaseSource;
}

namespace jasperengine::editor {
    struct ModuleScan;
}

namespace jasperengine::editor::cook {

    struct VoxelCookOptions {
        // R78: the volume uses runtimeGeneration; its base stays a generator
        bool keepGenerator = false;
        // a target to check against (a source export has none)
        bool hasTarget = false;
        BuildPlatform target = BuildPlatform::Linux;
        VoxelProjectSettings settings;
        // R78: the ground of the Terrain a shipped scene attaches the volume to, for a volume whose base
        // is that Terrain (voxelTerrainBases)
        std::shared_ptr<const voxel::BaseSource> terrainBase;
    };

    bool isVoxelVolumeFile(const std::filesystem::path& path);

    // `name` is what an error calls the volume. False, with the reason, when it is over the target's
    // limit, does not read, or has a generator nothing here can evaluate. Runs on the export's thread:
    // the VoxelAssets base resolver is called there.
    bool cookVoxelVolume(std::span<const uint8_t> source, const std::string& name, const VoxelCookOptions& options,
                         std::vector<uint8_t>& out, std::string& error);
    bool cookVoxelVolumeFile(const std::filesystem::path& source, const std::filesystem::path& destination, const std::string& name,
                             const VoxelCookOptions& options, std::string& error);

    // The volumes (assets-relative) some component ships with runtimeGeneration on
    std::set<std::string> voxelVolumesGeneratedAtRuntime(const ModuleScan& scan);

    // R78: for each volume (assets-relative) attached to a Terrain in what ships, that Terrain's ground under
    // it, copied, so it outlives the scenes; maps the scene has not loaded are read from their files. A
    // volume attached to two Terrains, or over a Terrain with no heights, adds a problem naming it.
    std::map<std::string, std::shared_ptr<const voxel::BaseSource>> voxelTerrainBases(const ModuleScan& scan, const std::filesystem::path& assetsDir,
                                                                                       std::vector<std::string>& problems);

}
