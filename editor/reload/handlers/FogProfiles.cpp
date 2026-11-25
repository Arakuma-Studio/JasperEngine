// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// .fogprofile files (docs/specs/S101-vapor.md R5, S05): the profile library reads the file again, and
// every Fog and Fog Zone using it takes the new values on the next frame (they look their
// profile up each frame they resolve). A profile nothing has read yet is left for its first use.

#include "ReloadHandlers.h"

#include "Project.h"
#include "render/FogProfile.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::reloadFogProfiles(Context& ctx, const std::vector<fs::path>& files) {
    bool any = false;
    for (const fs::path& path : files) {
        const std::string assetPath = ctx.project.normalizeToAssetsRelative(path).generic_string();
        if (FogProfileLibrary::version(assetPath) == 0) continue;
        FogProfileLibrary::reload(assetPath);
        ctx.service.recordDone("fogprofile", path, 0.0);
        any = true;
    }
    if (!any) return;
    for (SceneProject* sceneProject : ctx.project.getScenesAndPrefabStages())
        if (sceneProject->scene) sceneProject->needUpdateRender = true;
}
