// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim layers spec 9, 10, L7, L8)
// - Purpose: the authored layer edits of an Animation Player as single scene undo steps (the
//   Inspector's Layers table, MCP). Layers are referenced by index from the clip entries
//   (AnimationPlayerClip::layer), so an edit that changes indices moves the entries with their
//   layer in the same step: `layers` and `clips` change together in one MultiPropertyCmd.
// - The compute* functions are the pure part (what the step would set; false + error when the
//   edit is refused); the others push the step to the scene's command history (CommandHandle).
// - Rules: layer 0 is the base layer: it is not removed or moved and nothing moves to its place;
//   names are unique and non-empty; a list back to the single default Base Layer is stored empty.
// - Main thread.

#pragma once

#include "component/AnimationPlayerComponent.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class Project;

    namespace PlayerLayerActions{

        // the authored (layers, clips) after an edit of `player`
        struct Result{
            std::vector<AnimationPlayerLayer> layers;
            std::vector<AnimationPlayerClip> clips;
            int index = -1;             // add: the new layer's index; move: where it went
        };

        bool computeAdd(const AnimationPlayerComponent& player, const AnimationPlayerLayer& layer, Result& out, std::string* error);
        // its entries move to layer 0, the entries of the layers above it down one
        bool computeRemove(const AnimationPlayerComponent& player, int layer, Result& out, std::string* error);
        bool computeRename(const AnimationPlayerComponent& player, int layer, const std::string& name, Result& out, std::string* error);
        // `from` goes to index `to` (both >= 1); every entry follows its layer
        bool computeMove(const AnimationPlayerComponent& player, int from, int to, Result& out, std::string* error);

        // the same as one scene undo step on the GameObject's player of scene `sceneId`
        bool addLayer(Project* project, uint32_t sceneId, Entity entity, const AnimationPlayerLayer& layer, std::string* error, int* index = nullptr);
        bool removeLayer(Project* project, uint32_t sceneId, Entity entity, int layer, std::string* error);
        bool renameLayer(Project* project, uint32_t sceneId, Entity entity, int layer, const std::string& name, std::string* error);
        bool moveLayer(Project* project, uint32_t sceneId, Entity entity, int from, int to, std::string* error);

    }

}
