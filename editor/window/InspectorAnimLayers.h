// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The non-ImGui part of the Inspector's animation layer UI (upstream anim layers spec 9, 11.4;
// docs/specs/S21-animation.md "Upstream absorption"; InspectorWindowAnimLayers.cpp and the mask
// asset view of InspectorAnimator.cpp): the Avatar Mask figure's geometry and toggles, the mask
// "Create Mask from Selection" build and its scene step, "Create Animator Controller from Clips"
// (the controller written and played by an Animator the player hands over to, one scene step),
// the mask coverage line of the Bindings report, the preview hierarchy of a model the mask
// Inspector draws its tree and figure from, and the Layers table's fields (one scene step each)
// with the Mesh Space Rotation row (docs/specs/mesh-space-rotation.md R25 to R27, R32). S21's masks (AvatarMaskDef) and weight rule
// (AvatarMaskWeights.h). Kept apart so the editor tests drive exactly what the Inspector's clicks
// run. Main thread.

#pragma once

#include "animation/AvatarMaskWeights.h"
#include "animation/ControllerDef.h"
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
    class Command;
    class Project;
}

namespace jasperengine::editor::animlayersui{

    // ---- the humanoid figure (the mask Inspector), in figure space: x and y in [0, 1] ----

    struct FigureShape{
        AvatarBodyPart part = AvatarBodyPart::Root;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;    // a rounded box, or the root disc's bounds
        bool disc = false;
    };
    // every shape of the figure, drawn in order (the hands over the arms)
    const std::vector<FigureShape>& figureShapes();
    // the part under a point of figure space (the last shape drawn there), -1 when none
    int figurePartAt(float x, float y);
    // the centre of a part's first shape (tests click there)
    void figurePartCenter(AvatarBodyPart part, float& x, float& y);
    // Unity's labels: "Root", "Body", "Head", "Left Arm", ..., "Right Fingers"
    const char* partLabel(AvatarBodyPart part);
    // a click on a part: 1 -> 0, anything below 1 -> 1 (the part is set when it was not)
    anim::AvatarMaskDef togglePart(const anim::AvatarMaskDef& data, AvatarBodyPart part);
    // a Shift-click in the transform tree: the entry and every entry below `path` set to `on`
    // (on: weight 1 with children; off: removed)
    anim::AvatarMaskDef setTransform(const anim::AvatarMaskDef& data, const std::string& path, bool on, bool subtree,
                                     const std::vector<std::string>& allPaths);

    // ---- builds ----

    // A mask that animates the selected GameObjects (and what is below them): one transform
    // entry per selected entity under `root` (its path, children: true), default 0. False when
    // none of them is under the root.
    bool maskFromSelection(EntityRegistry* registry, Entity root, const std::vector<Entity>& selection, anim::AvatarMaskDef& out,
                           std::string* error);
    // An Animator Controller with one state per clip entry (named like the player's states), the
    // player's layers as controller layers (weight, blending, mask), the default clip's state as
    // its layer's default, laid out on a grid, no transitions and no parameters
    // (AnimatorAssetDocument.h appendPlayerStates)
    anim::ControllerDef controllerFromClips(const AnimationPlayerComponent& player);

    // Create Mask from Selection... as ONE scene step that writes `maskPath` (project-relative,
    // a new file) and sets layers[layer].mask/maskGuid (CreateClipCmd)
    bool createMaskFromSelection(Project* project, uint32_t sceneId, Entity entity, int layer, const std::string& maskPath,
                                 const std::vector<Entity>& selection, std::string& error);
    // Create Animator Controller from Clips: writes `controllerPath` (project-relative, new),
    // gives the GameObject an Animator playing it (added when it has none) and disables the
    // player, which hands over to the Animator (object/AnimationPlayer.h), in one scene step
    bool createControllerFromClips(Project* project, uint32_t sceneId, Entity entity, const std::string& controllerPath, std::string& error);
    // the same step with given controller data
    bool createControllerFile(Project* project, uint32_t sceneId, Entity entity, const std::string& controllerPath,
                              const anim::ControllerDef& data, std::string& error);
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
    MaskCoverage maskCoverage(Scene* scene, Entity root, const anim::AvatarMaskDef& mask, const std::vector<Entity>& targets);

    // ---- the mask Inspector's preview hierarchy ----

    // A model's nodes (or a GameObject's descendants) as a registry of named entities, so the
    // mask Inspector can resolve paths, draw the tree and evaluate the mask exactly as a player
    // would (anim::maskWeightOf), with the humanoid map auto-detected from the node names and
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

    // ---- the Layers table ----

    // One field of an authored layer (layers[i].<field>) as a scene command; a player with the
    // default layering gets its whole list (one Base Layer) so the field has a place. Null when
    // the layer does not exist. Defined for float (weight), AnimationLayerBlend and bool.
    template<class T>
    Command* layerFieldCommand(Project* project, uint32_t sceneId, Entity entity, const AnimationPlayerComponent& player, int layer,
                               const std::string& field, const T& value, T AnimationPlayerLayer::* member);
    // the row's summary of a layer above 0: "override, mask arm.mask", ", mesh space" when on
    std::string layerSummary(const AnimationPlayerLayer& layer);

    // the Mesh Space Rotation checkbox's tooltip (spec R25)
    extern const char* const MESH_SPACE_ROTATION_TOOLTIP;
    // why the checkbox is disabled on a layer (layer 0, an Additive layer: spec R26); null when
    // it is not
    const char* meshSpaceRotationDisabledReason(int layer, AnimationLayerBlend blend);
    // The GameObjects the player's clips animate below `root` (their transform tracks' targets;
    // a humanoid clip's, the rig's slot bones): what its Mesh Space Rotation rig is built from
    std::vector<Entity> playerAnimatedTargets(Project* project, Scene* scene, Entity root, const AnimationPlayerComponent& player);
    // The GameObjects below `root` where Mesh Space Rotation applies with `mask`, on the Player's
    // rig of `animated` (anim::playerRig, anim::playerMaskBoundaryBones: the runtime's rule and
    // rig, body parts from the humanoid rig of its Models); parents first, by depth
    std::vector<Entity> meshSpaceBoundary(Scene* scene, Entity root, const anim::AvatarMaskDef& mask, const std::vector<Entity>& animated);
    // the read-only line under the checkbox (spec R27): "Applies at: Spine, ..." (names), the
    // generic "Applies where the mask starts or stops" when there is no mask data or no rig
    std::string meshSpaceBoundaryLine(Scene* scene, Entity root, const anim::AvatarMaskDef* mask, const std::vector<Entity>& animated);

}
