// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// .particles effects (S20 R1, S05 R11): the effect library reads the file again and raises its
// version, so every Particle System playing it rebinds on its next update (keeping its live
// particles when only parameters changed). A file read nowhere yet is left for its first use.
// What the file says wrong is a problem either way: a misspelled field only warns when read.

#include "ReloadHandlers.h"

#include "Project.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
#include "util/ProjectChecks.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::addParticleEffectFileProblems(Project& project, const fs::path& path, Json& problems) {
    std::error_code ec;
    std::string text;
    if (!fs::is_regular_file(path, ec) || !ProjectChecks::readFile(path, text)) return;
    particles::ParticleEffectDocument document;
    std::vector<particles::Diagnostic> diagnostics;
    particles::ParticleEffectCodec::read(text, document, diagnostics);
    for (const particles::Diagnostic& d : diagnostics) {
        problems.push_back({{"path", reload::rel(project, path)},
                            {"severity", d.severity == particles::Diagnostic::Severity::Error ? "error" : "warning"},
                            {"message", d.path + ": " + d.message}});
    }
}

void reload::reloadParticleEffects(Context& ctx, const std::vector<fs::path>& files) {
    for (const fs::path& path : files) {
        addParticleEffectFileProblems(ctx.project, path, ctx.result.problems);
        const std::string assetPath = ctx.project.normalizeToAssetsRelative(path).generic_string();
        if (particles::ParticleEffectLibrary::version(assetPath) == 0) continue;   // read by nothing yet
        particles::ParticleEffectLibrary::reload(assetPath);
        for (SceneProject* sceneProject : ctx.project.getScenesAndPrefabStages()) {
            if (sceneProject->scene) sceneProject->needUpdateRender = true;
        }
        ctx.service.recordDone("particles", path, 0.0);
    }
}
