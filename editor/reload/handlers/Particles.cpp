// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// .particles effects (S20 R1, S05 R11): the effect library reads the file again and raises its
// version, so every Particle System playing it rebinds on its next update (keeping its live
// particles when only parameters changed). A file read nowhere yet is left for its first use.

#include "ReloadHandlers.h"

#include "Project.h"
#include "particles/ParticleEffectLibrary.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::reloadParticleEffects(Context& ctx, const std::vector<fs::path>& files) {
    for (const fs::path& path : files) {
        const std::string assetPath = ctx.project.normalizeToAssetsRelative(path).generic_string();
        if (particles::ParticleEffectLibrary::version(assetPath) == 0) continue;   // read by nothing yet
        particles::ParticleEffectLibrary::reload(assetPath);
        for (SceneProject* sceneProject : ctx.project.getScenesAndPrefabStages()) {
            if (sceneProject->scene) sceneProject->needUpdateRender = true;
        }
        ctx.service.recordDone("particles", path, 0.0);
    }
}
