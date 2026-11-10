// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT
//
// .tileset files (S50 R11, S05): the pool drops its copy, so the layers using the file resolve it
// again on their next update and rebuild their chunks (its revision moved).

#include "ReloadHandlers.h"

#include "Project.h"
#include "tilemap/TilesetPool.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::reloadTilesets(Context& ctx, const std::vector<fs::path>& files) {
    for (const fs::path& path : files) {
        const std::string assetPath = ctx.project.normalizeToAssetsRelative(path).generic_string();
        TilesetPool::invalidate(assetPath);
        for (SceneProject* sceneProject : ctx.project.getScenesAndPrefabStages())
            if (sceneProject->scene) sceneProject->needUpdateRender = true;
        ctx.service.recordDone("tileset", path, 0.0);
    }
}
