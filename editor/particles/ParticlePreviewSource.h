// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLEPREVIEWSOURCE_H
#define PARTICLEPREVIEWSOURCE_H

#include "ecs/Entity.h"
#include "json.hpp"
#include "particles/ParticleEffectPreview.h"

#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor {

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
    };

}

#endif //PARTICLEPREVIEWSOURCE_H
