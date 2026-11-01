// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim layers spec 6.2, 7.2, L9)
// - Purpose: the Avatar Mask asset (.mask, Unity's AvatarMask): a weight per humanoid body part
//   and per transform path, which an Animation Player layer multiplies its tracks by.
//   * AvatarMaskData: the value form, parsed and written by parseAvatarMask / writeAvatarMask
//     (the engine's AvatarMaskPool, the editor's mask document, MCP, the CLI checks).
//   * AvatarMask: the immutable runtime form (AvatarMaskPool), with weightOf.
// - Format: UTF-8 JSON {"format": "jasperengine.mask", "version": 1, "guid", "default",
//   "humanoid", "transforms", "preview", unknown keys...}. Canonical and lenient like .anim:
//   format + version required (and Unity YAML refused), every other bad value dropped or
//   repaired with a warning carrying its JSON path, unknown keys kept and written back.
//   writeAvatarMask(parseAvatarMask(writeAvatarMask(d))) == writeAvatarMask(d) byte for byte.
// - Canonical details: 2-space indent; "guid" only when set; "default" always; "humanoid" only
//   when present, its parts in AvatarMaskPart order, only the parts set; "transforms" always
//   (sorted by path, one per line {"path", "weight", "children"}, unique paths: the later one of a
//   duplicate wins with a warning); "preview" only when set; weights clamped to [0, 1], %.6g.
// - Weight of an entity E under the player root R (spec 6.2), the first rule that applies:
//   1. the transform entry of E's own path, else of E's nearest ancestor (below R, or R itself
//      for the path "") whose entry has children: true (the deepest entry wins);
//   2. with `humanoid` present and a humanoid rig on R: E's body part (a slot bone's part; any
//      other entity below a slot bone its nearest slot ancestor's part, `<side>_fingers` below a
//      hand; `root` for R and the non-slot entities between R and the hips);
//   3. `default`.
// - Thread-safety: pure functions; an AvatarMask never changes after build (any thread reads it).

#ifndef AVATAR_MASK_H
#define AVATAR_MASK_H

#include "Export.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipTypes.h"
#include "ecs/Entity.h"
#include "pose/HumanBones.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine{
    class EntityRegistry;
    class Scene;
}

namespace jasperengine::anim{

    constexpr const char* MASK_FORMAT = "jasperengine.mask";
    constexpr int MASK_VERSION = 1;
    constexpr const char* MASK_EXTENSION = ".mask";

    // Unity's AvatarMaskBodyPart without the four IK goals (file names snake_case)
    enum class AvatarMaskPart : uint8_t {
        Root, Body, Head, LeftLeg, RightLeg, LeftArm, RightArm, LeftFingers, RightFingers, Count
    };
    constexpr int AVATAR_MASK_PART_COUNT = (int)AvatarMaskPart::Count;

    // "root", "body", "head", "left_leg", "right_leg", "left_arm", "right_arm", "left_fingers",
    // "right_fingers"
    JASPERENGINE_API const char* avatarMaskPartName(AvatarMaskPart part);
    JASPERENGINE_API bool avatarMaskPartFromName(const std::string& name, AvatarMaskPart& out);
    // the part of a humanoid slot bone (hips..upper_chest: body; neck, head: head; ...)
    JASPERENGINE_API AvatarMaskPart avatarMaskPartOfSlot(pose::HumanBone slot);

    struct JASPERENGINE_API AvatarMaskTransform{
        std::string path;           // a clip path relative to the player root ("" = the root)
        float weight = 1.0f;        // 0..1
        bool children = true;       // the entry also covers the descendants

        bool operator==(const AvatarMaskTransform& other) const{
            return path == other.path && weight == other.weight && children == other.children;
        }
    };

    struct JASPERENGINE_API AvatarMaskData{
        std::string guid;
        // weight of every entity no rule covers (an empty mask passes everything, as in Unity)
        float defaultWeight = 1.0f;
        // the "humanoid" object is present (an empty one counts: then every part takes default)
        bool hasHumanoid = false;
        // per part; unset parts take defaultWeight
        std::array<std::optional<float>, AVATAR_MASK_PART_COUNT> parts;
        std::vector<AvatarMaskTransform> transforms;
        // editor-only: the model or prefab the Inspector builds its tree and figure from
        std::string previewModel;
        std::vector<std::pair<std::string, std::string>> unknownKeys;

        // the part's weight (its own, else defaultWeight)
        float partWeight(AvatarMaskPart part) const;
        // index into transforms, -1 when none
        int findTransform(const std::string& path) const;
    };

    struct JASPERENGINE_API AvatarMaskParseResult{
        std::optional<AvatarMaskData> data;
        std::vector<ClipWarning> warnings;
        ClipParseError error;

        bool ok() const { return data.has_value(); }
    };

    JASPERENGINE_API AvatarMaskParseResult parseAvatarMask(const std::string& text);
    // the canonical text of `data` (normalized first)
    JASPERENGINE_API std::string writeAvatarMask(const AvatarMaskData& data);
    // clamps weights, sorts and merges transforms; what changed goes to `warnings`
    JASPERENGINE_API void normalizeAvatarMask(AvatarMaskData& data, std::vector<ClipWarning>* warnings = nullptr);

    // Unity's YAML mask (%YAML header or an "AvatarMask:" document) and the refusal message
    JASPERENGINE_API bool isUnityYamlMask(const std::string& text);
    JASPERENGINE_API const char* unityYamlMaskRefusal();

    // The humanoid slots of a player root: the entity of each slot (NULL_ENTITY: unmapped).
    // `valid` false when the root has no humanoid rig.
    struct HumanoidTarget;
    struct JASPERENGINE_API HumanoidSlots{
        bool valid = false;
        std::array<Entity, pose::HUMAN_BONE_COUNT> entities{};
        std::string error;          // why there is no rig (not valid)
        // anim-retarget: the rig humanoid clips realise on (its model, bone entities, bridges);
        // null for slots made by name (humanoidSlotsByName)
        std::shared_ptr<const HumanoidTarget> target;

        HumanoidSlots(){ entities.fill(NULL_ENTITY); }
    };
    // The rig rule of spec 6.2 rule 2 (also anim-retarget's target rig): the first ModelComponent
    // on or under `root` in hierarchy order whose pose::buildModelRig succeeds; its humanoid
    // map's bones as entities. Main thread.
    JASPERENGINE_API HumanoidSlots resolveHumanoidSlots(Scene* scene, Entity root);
    // The slots of a hierarchy whose entity names are the map's bone names (the first entity of
    // that name in `root`'s branch, hierarchy order): tests and tools without a Model
    JASPERENGINE_API HumanoidSlots humanoidSlotsByName(EntityRegistry* scene, Entity root, const std::array<std::string, pose::HUMAN_BONE_COUNT>& boneNames);

    // which rule gave an entity its weight (MCP coverage: "transform", "humanoid:<part>", "default")
    struct JASPERENGINE_API AvatarMaskWeight{
        enum class Rule : uint8_t { Transform, Humanoid, Default };
        float weight = 1.0f;
        Rule rule = Rule::Default;
        int transform = -1;                         // Rule::Transform: index into transforms
        AvatarMaskPart part = AvatarMaskPart::Root; // Rule::Humanoid
        std::string ruleName() const;
    };

    class JASPERENGINE_API AvatarMask{
    public:
        AvatarMask(const AvatarMask&) = delete;
        AvatarMask& operator=(const AvatarMask&) = delete;

        // never null; `data` is normalized first
        static std::shared_ptr<const AvatarMask> build(const AvatarMaskData& data, std::vector<ClipWarning> warnings = {});

        const AvatarMaskData& getData() const { return data; }
        const std::vector<ClipWarning>& getWarnings() const { return warnings; }
        bool hasHumanoid() const { return data.hasHumanoid; }

        // The transform entries resolved on a root (ClipBinding path grammar): what weightOf
        // needs, computed once per bind. Entries that do not resolve are NULL_ENTITY.
        std::vector<Entity> bindTransforms(EntityRegistry* scene, Entity root) const;
        // E's weight under R (rules above); `transformEntities` from bindTransforms(scene, root),
        // `humanoid` null or not valid: rule 2 is skipped
        AvatarMaskWeight weightOf(EntityRegistry* scene, Entity root, Entity entity, const std::vector<Entity>& transformEntities,
                                  const HumanoidSlots* humanoid) const;
        // the same without a prepared bind (MCP, tools; any registry: a scene, a preview hierarchy)
        float weightOf(EntityRegistry* scene, Entity root, Entity entity, const HumanoidSlots* humanoid) const;

    private:
        AvatarMask() = default;
        AvatarMaskData data;
        std::vector<ClipWarning> warnings;
    };

}

#endif // AVATAR_MASK_H
