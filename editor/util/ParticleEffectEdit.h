// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The edits of particle effects (docs/specs/S20-particles.md R27, R30), shared by the
// Inspector's module list and the MCP particles_* tools so both take the same undoable path.
// A Particle System edits its effect asset when it has one (the file is written and every user
// updates) and its inline effect otherwise (only that GameObject, a prefab override on an
// instance).

#include "ecs/Entity.h"
#include "particles/ParticleEffect.h"

#include "json.hpp"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine {
    struct ParticleSystemComponent;
}

namespace jasperengine::editor {

    class Command;
    class Project;

    class ParticleEffectEdit {
    public:
        // changes a document; false leaves it as it was
        using Edit = std::function<bool(particles::ParticleEffectDocument& document)>;

        // The effect a Particle System plays: its asset's document, else its inline one. False
        // (with the asset's diagnostics) when the asset cannot be read.
        static bool read(const ParticleSystemComponent& particles, particles::ParticleEffectDocument& document,
                         std::vector<particles::Diagnostic>* diagnostics = nullptr);
        // The asset at an assets-relative path (as the effect library holds it)
        static bool readAsset(const std::string& assetPath, particles::ParticleEffectDocument& document,
                              std::vector<particles::Diagnostic>* diagnostics = nullptr);

        // The RFC 7386 merge patch turning `before` into `after` (objects merge, everything else
        // is replaced, a key gone is null)
        static nlohmann::ordered_json diff(const nlohmann::ordered_json& before, const nlohmann::ordered_json& after);

        // One undoable step over the effects of `entities`: each asset once, each inline effect
        // on its GameObject. Null when no document changed.
        static Command* build(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, const Edit& edit);
        // An asset on its own (the Project window preview, a tool given a path)
        static Command* buildAsset(Project* project, const std::string& assetPath, const Edit& edit);

        // Save as Asset: a new .particles file at `assetPath` holding the effect of the first
        // GameObject, assigned to all of them
        static Command* buildSaveAsAsset(Project* project, uint32_t sceneId, const std::vector<Entity>& entities, const std::string& assetPath);
        // Make Inline: each GameObject gets a copy of its asset's effect and drops the reference
        static Command* buildMakeInline(Project* project, uint32_t sceneId, const std::vector<Entity>& entities);

        // A new "<directory>/<name>.particles" path (assets-relative) no file has yet
        static std::string freeAssetPath(Project* project, const std::string& directory, const std::string& name);
    };

}
