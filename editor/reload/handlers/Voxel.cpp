// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// .voxelvolume files another program changed (docs/specs/T6-voxel-volumes.md R77). The editor's own
// saves never reach this: apply() files them under the editor's writes.

#include "ReloadHandlers.h"

#include "Project.h"
#include "voxel/VoxelAssetFile.h"

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;

void reload::reloadVoxelVolumes(Context& ctx, const std::vector<fs::path>& files) {
    for (const fs::path& path : files) {
        const std::string assetPath = ctx.project.normalizeToAssetsRelative(path).generic_string();
        if (reloadVoxelVolumeFile(ctx.project, assetPath)) ctx.service.recordDone("voxel", path, 0.0);
    }
}
