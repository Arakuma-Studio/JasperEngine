// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The non-ImGui part of the Inspector's animation layer UI (anim layers spec 9, 11.4;
// InspectorWindowAnimLayers.cpp): the Avatar Mask figure's geometry and toggles, the mask
// "Create Mask from Selection" and "Create Controller from Clips" builds and their scene steps,
// the mask coverage line of the Bindings report, and the preview hierarchy of a model the mask
// Inspector draws its tree and figure from. Kept apart so the editor tests drive exactly what the
// Inspector's clicks run. Main thread.

#pragma once

#include "animation/AnimatorController.h"
#include "animation/AvatarMask.h"
#include "component/AnimationPlayerComponent.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{
    class EntityRegistry;
    class Scene;
}

namespace jasperengine::editor{
    class Project;
}

namespace jasperengine::editor::animlayersui{

    // ---- the humanoid figure (the mask Inspector), in figure space: x and y in [0, 1] ----

    struct FigureShape{
        anim::AvatarMaskPart part = anim::AvatarMaskPart::Root;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;    // a rounded box, or the root disc's bounds
        bool disc = false;
    };
    // every shape of the figure, drawn in order (the hands over the arms)
    const std::vector<FigureShape>& figureShapes();
    // the part under a point of figure space (the last shape drawn there), -1 when none
    int figurePartAt(float x, float y);
    // the centre of a part's first shape (tests click there)
    void figurePartCenter(anim::AvatarMaskPart part, float& x, float& y);
    // Unity's labels: "Root", "Body", "Head", "Left Leg", ..., "Right Fingers"
    const char* partLabel(anim::AvatarMaskPart part);
    // a click on a part: 1 -> 0, anything below 1 -> 1 (the humanoid object is added when missing)
    anim::AvatarMaskData togglePart(const anim::AvatarMaskData& data, anim::AvatarMaskPart part);
    // a Shift-click in the transform tree: the entry and every entry below `path` set to `on`
    // (on: weight 1 with children; off: removed)
    anim::AvatarMaskData setTransform(const anim::AvatarMaskData& data, const std::string& path, bool on, bool subtree,
                                      const std::vector<std::string>& allPaths);

    // ---- builds ----

    // A mask that animates the selected GameObjects (and what is below them): one transform
    // entry per selected entity under `root` (its path, children: true), default 0. False when
    // none of them is under the root.
    bool maskFromSelection(EntityRegistry* registry, Entity root, const std::vector<Entity>& selection, anim::AvatarMaskData& out,
                           std::string* error);
    // An Animator Controller with one state per clip entry (named like the player's states), the
    // player's layers as controller layers (weight, blend, mask), the default clip's state as the
    // base layer's default, laid out on a grid, no transitions and no parameters
    anim::ControllerData controllerFromClips(const AnimationPlayerComponent& player);

    // Create Mask from Selection... as ONE scene step that writes `maskPath` (project-relative,
    // a new file) and sets layers[layer].mask/maskGuid (CreateClipCmd)
    bool createMaskFromSelection(Project* project, uint32_t sceneId, Entity entity, int layer, const std::string& maskPath,
                                 const std::vector<Entity>& selection, std::string& error);
    // Create Controller from Clips: writes `controllerPath` (project-relative, new) and sets the
    // player's controller/controllerGuid in one scene step
    bool createControllerFromClips(Project* project, uint32_t sceneId, Entity entity, const std::string& controllerPath, std::string& error);
    // the same step with given controller data (MCP edit_animator {create: {from_entity}, ops})
    bool createControllerFile(Project* project, uint32_t sceneId, Entity entity, const std::string& controllerPath,
                              const anim::ControllerData& data, std::string& error);
    // a free project-relative path "<dir>/<stem>.<ext>", "<stem> 2.<ext>"...
    std::string freeAssetPath(Project* project, const std::string& directory, const std::string& stem, const std::string& extension);

    // ---- Bindings: "mask: N/M bound entities covered" ----

    struct MaskCoverage{
        size_t covered = 0;                 // bound entities with a weight above 0
        size_t total = 0;                   // the distinct entities the layer's clips bind
        bool humanoidWithoutRig = false;    // the mask has body parts, the root no humanoid rig
        std::string rigError;
    };
    // `targets`: the entities the layer's clips bind (duplicates allowed)
    MaskCoverage maskCoverage(Scene* scene, Entity root, const anim::AvatarMask& mask, const std::vector<Entity>& targets);

    // ---- the mask Inspector's preview hierarchy ----

    // A model's nodes (or a GameObject's descendants) as a registry of named entities, so the
    // mask Inspector can resolve paths, draw the tree and evaluate the mask exactly as a player
    // would (AvatarMask::weightOf), with the humanoid map auto-detected from the node names and
    // the model's .humanoid sidecar.
    struct PreviewHierarchy{
        std::unique_ptr<EntityRegistry> registry;
        Entity root = NULL_ENTITY;
        std::vector<std::string> paths;     // every node below the root, depth first
        std::vector<Entity> entities;       // parallel to paths
        std::vector<int> depths;            // parallel to paths (0 = a child of the root)
        anim::HumanoidSlots humanoid;       // valid: the hierarchy is humanoid
        std::string error;                  // why it has no nodes / no humanoid rig
        PreviewHierarchy();
        ~PreviewHierarchy();
        PreviewHierarchy(PreviewHierarchy&&) noexcept;
        PreviewHierarchy& operator=(PreviewHierarchy&&) noexcept;
    };
    // from a model file (asset-relative) or a prefab's first model; false + error when it can't be read
    bool previewHierarchyFromModel(Project* project, const std::string& model, PreviewHierarchy& out);
    // from a GameObject of a scene (its descendants; the humanoid rig of its Models)
    bool previewHierarchyFromEntity(Scene* scene, Entity root, PreviewHierarchy& out);

}
