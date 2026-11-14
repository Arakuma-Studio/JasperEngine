// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// .mesh files (S98 F1, model/MeshAsset.h) another program changed: every Mesh Renderer of the open
// scenes and prefab stages showing one reads it again on the next frame (the editor's own writes
// do it themselves: MeshAssetFiles::write).

#include "ReloadHandlers.h"

#include "Project.h"
#include "subsystem/MeshSystem.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::reloadMeshAssets(Context& ctx, const std::vector<fs::path>& files) {
    for (const fs::path& path : files) {
        const std::string assetPath = ctx.project.normalizeToAssetsRelative(path).generic_string();
        for (SceneProject* sceneProject : ctx.project.getScenesAndPrefabStages()) {
            if (!sceneProject->scene) continue;
            if (std::shared_ptr<MeshSystem> meshSystem = sceneProject->scene->findSystem<MeshSystem>()) {
                meshSystem->invalidateMeshAsset(assetPath);
                sceneProject->needUpdateRender = true;
            }
        }
        ctx.service.recordDone("mesh", path, 0.0);
    }
}
