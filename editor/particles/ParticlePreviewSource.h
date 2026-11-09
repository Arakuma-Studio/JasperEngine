// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLEPREVIEWSOURCE_H
#define PARTICLEPREVIEWSOURCE_H

#include "ecs/Entity.h"
#include "json.hpp"
#include "particles/ParticleEffectPreview.h"
#include "util/QaChecks.h"

#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    // What a preview runs (ParticleEffectPreview): an asset alone, or a GameObject and every
    // Particle System beneath it, with their transforms relative to it (S91 R26, R31). The tool, the
    // Particle Effect window and check --particles read their systems through these, so they agree.
    class ParticlePreviewSource {
    public:
        // an assets-relative .particles path
        static bool fromAsset(const std::string& assetPath, std::vector<ParticlePreviewSystem>& out, std::string& error);
        // `root` and the Particle Systems beneath it, ordered by their path below it
        static bool fromSubtree(Scene* scene, Entity root, std::vector<ParticlePreviewSystem>& out, std::string& error);
        // the Particle Systems of a subtree (root included when it has one), in that order
        static std::vector<Entity> subtreeSystems(Scene* scene, Entity root);
        // preview_particle_effect's run arguments (seed, frames, times, seconds, view, background,
        // overdraw_map, platform) into a request, with their defaults; false and the fix when one is wrong
        static bool applyArgs(const nlohmann::json& args, ParticlePreviewRequest& request, std::string& error);

        // The rest of a request both preview_particle_effect's handler and the window's panel make:
        // the run arguments (applyArgs), the project's thresholds, the consecutive FAILs of `scope`
        // so far, and the validation. False and the fix when something is wrong.
        static bool complete(Project* project, const nlohmann::json& args, const std::string& scope, ParticlePreviewRequest& request,
                             std::string& error);
        // what fix rounds are counted by: the asset's path, or "scene <id>: <GameObject path>"
        static std::string subtreeScope(const SceneProject& sceneProject, Entity root);
        // the consecutive FAILs of each check per scope this editor session, the tool's and the
        // window's runs alike (record a finished run's checks here). Main thread.
        static qa::FailRounds& rounds();
    };

}

#endif //PARTICLEPREVIEWSOURCE_H
