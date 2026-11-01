// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R7 and "Upstream absorption")
// - Purpose: the one weight rule of an Avatar Mask (AvatarMaskDef, `.mask` version 2), used by
//   the Animator's layers (AnimatorInstance::bind), the Animation Player's layers
//   (ActionSystem::preparePlayerLayers), the editor (masked rows, coverage) and MCP coverage.
// - The weight of a GameObject E under the masked root R, the first rule that applies:
//   1. paths: E's own entry (path relative to R, "" = R), else the entry of E's nearest
//      ancestor (up to R) with `children` (the deepest entry wins);
//   2. with a humanoid on R: E's body part. A slot bone has its slot's part (hips, spine,
//      chest, upper chest: body; neck, head: head; shoulder, upper and lower arm: <side>_arm;
//      the hand: <side>_hand; legs, feet, toes: <side>_leg); a GameObject below a slot bone has
//      its nearest slot ancestor's part, below a hand <side>_fingers. With `root_hierarchy`, R
//      and the GameObjects between R and the hips have the root part. A part that is not set
//      weighs `default`;
//   3. `default`.
//   Root motion weighs the root part (its own, else `default`).
// - S21's version 1 masks convert to this rule exactly for bones (default = their body part's
//   weight, a hand's part also covers what is below it, paths exact); upstream's version 1
//   masks too (UpstreamAnimFormats.h).
// - Thread-safety: pure functions over a registry the caller owns (main thread for a Scene).

#ifndef AVATAR_MASK_WEIGHTS_H
#define AVATAR_MASK_WEIGHTS_H

#include "Export.h"
#include "animation/ControllerDef.h"
#include "ecs/Entity.h"
#include "pose/HumanBones.h"

#include <array>
#include <string>
#include <vector>

namespace jasperengine{
    class EntityRegistry;
    class Scene;
}

namespace jasperengine::anim{

    // The humanoid slots of a masked root: the entity of each slot (NULL_ENTITY: unmapped).
    // `valid` false when the root has no humanoid rig.
    struct JASPERENGINE_API HumanoidSlots{
        bool valid = false;
        std::array<Entity, pose::HUMAN_BONE_COUNT> entities{};
        std::string error;          // why there is no rig (not valid)

        HumanoidSlots(){ entities.fill(NULL_ENTITY); }
    };

    // The Animation Player's rig rule: the first ModelComponent on or under `root` in hierarchy
    // order whose pose::buildModelRig succeeds; its humanoid map's bones as entities. Main thread.
    JASPERENGINE_API HumanoidSlots resolveHumanoidSlots(Scene* scene, Entity root);
    // The slots of a hierarchy whose entity names are the map's bone names (the first entity of
    // that name in `root`'s branch, hierarchy order): tests and tools without a Model
    JASPERENGINE_API HumanoidSlots humanoidSlotsByName(EntityRegistry* scene, Entity root, const std::array<std::string, pose::HUMAN_BONE_COUNT>& boneNames);

    // the body part of a humanoid slot bone
    JASPERENGINE_API AvatarBodyPart bodyPartOfSlot(pose::HumanBone slot);

    // which rule gave a GameObject its weight (MCP coverage: "path", "body_part:<part>", "default")
    struct JASPERENGINE_API MaskWeight{
        enum class Rule : uint8_t { Path, BodyPart, Default };
        float weight = 1.0f;
        Rule rule = Rule::Default;
        int path = -1;                              // Rule::Path: index into paths
        AvatarBodyPart part = AvatarBodyPart::Body; // Rule::BodyPart
        std::string ruleName() const;
    };

    // the mask's path entries resolved under `root` (ClipBinding grammar; NULL_ENTITY when one
    // does not resolve): what maskWeightOf needs, computed once per bind
    JASPERENGINE_API std::vector<Entity> bindMaskPaths(const AvatarMaskDef& mask, EntityRegistry* scene, Entity root);
    // E's weight under R (the rule above); `pathEntities` from bindMaskPaths(mask, scene, root),
    // `humanoid` null or not valid: the body parts are skipped
    JASPERENGINE_API MaskWeight maskWeightOf(const AvatarMaskDef& mask, EntityRegistry* scene, Entity root, Entity entity,
                                             const std::vector<Entity>& pathEntities, const HumanoidSlots* humanoid);
    // the same without a prepared bind (MCP, tools)
    JASPERENGINE_API float maskWeightOf(const AvatarMaskDef& mask, EntityRegistry* scene, Entity root, Entity entity, const HumanoidSlots* humanoid);
    // the root part's weight (root motion)
    JASPERENGINE_API float maskRootMotionWeight(const AvatarMaskDef& mask);

}

#endif // AVATAR_MASK_WEIGHTS_H
