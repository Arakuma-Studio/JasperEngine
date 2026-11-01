// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AvatarMaskWeights.h"

#include "Scene.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/ClipBinding.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "pose/ModelRig.h"

using namespace jasperengine;

namespace{

    Entity parentOf(EntityRegistry* scene, Entity entity){
        const Transform* transform = scene->findComponent<Transform>(entity);
        return transform ? transform->parent : NULL_ENTITY;
    }

}

AvatarBodyPart anim::bodyPartOfSlot(pose::HumanBone slot){
    using pose::HumanBone;
    switch (slot){
        case HumanBone::Hips: case HumanBone::Spine: case HumanBone::Chest: case HumanBone::UpperChest:
            return AvatarBodyPart::Body;
        case HumanBone::Neck: case HumanBone::Head:
            return AvatarBodyPart::Head;
        case HumanBone::LeftShoulder: case HumanBone::LeftUpperArm: case HumanBone::LeftLowerArm:
            return AvatarBodyPart::LeftArm;
        case HumanBone::LeftHand:
            return AvatarBodyPart::LeftHand;
        case HumanBone::RightShoulder: case HumanBone::RightUpperArm: case HumanBone::RightLowerArm:
            return AvatarBodyPart::RightArm;
        case HumanBone::RightHand:
            return AvatarBodyPart::RightHand;
        case HumanBone::LeftUpperLeg: case HumanBone::LeftLowerLeg: case HumanBone::LeftFoot: case HumanBone::LeftToes:
            return AvatarBodyPart::LeftLeg;
        case HumanBone::RightUpperLeg: case HumanBone::RightLowerLeg: case HumanBone::RightFoot: case HumanBone::RightToes:
            return AvatarBodyPart::RightLeg;
        case HumanBone::Count:
            break;
    }
    return AvatarBodyPart::Body;
}

float anim::AvatarMaskDef::partWeight(AvatarBodyPart part) const{
    const int index = (int)part;
    if (index < 0 || index >= AVATAR_BODY_PART_COUNT || !bodyParts[index]) return defaultWeight;
    return *bodyParts[index];
}

int anim::AvatarMaskDef::findPath(const std::string& path) const{
    for (size_t i = 0; i < paths.size(); i++){
        if (paths[i].path == path) return (int)i;
    }
    return -1;
}

std::string anim::MaskWeight::ruleName() const{
    switch (rule){
        case Rule::Path: return "path";
        case Rule::BodyPart: return std::string("body_part:") + bodyPartName(part);
        case Rule::Default: break;
    }
    return "default";
}

anim::HumanoidSlots anim::resolveHumanoidSlots(Scene* scene, Entity root){
    HumanoidSlots slots;
    if (!scene || root == NULL_ENTITY || !scene->isEntityCreated(root)){
        slots.error = "no root";
        return slots;
    }
    // ModelComponents on and under the root, in hierarchy (Transform array) order
    std::vector<Entity> models;
    if (scene->findComponent<ModelComponent>(root)) models.push_back(root);
    auto transforms = scene->getComponentArray<Transform>();
    Transform* rootTransform = transforms->findComponent(root);
    if (rootTransform){
        const size_t first = (size_t)(rootTransform - transforms->findComponentFromIndex(0));
        const size_t last = scene->findBranchLastIndex(root);
        for (size_t i = first + 1; i <= last && i < transforms->size(); i++){
            const Entity entity = transforms->getEntity(i);
            if (scene->findComponent<ModelComponent>(entity)) models.push_back(entity);
        }
    }
    if (models.empty()){
        slots.error = "no Model on or under the root";
        return slots;
    }
    std::string firstError;
    for (Entity model : models){
        pose::ModelRigInfo info;
        std::vector<Entity> boneEntities;
        std::string error;
        if (!pose::buildModelRig(scene, model, info, boneEntities, nullptr, &error)){
            if (firstError.empty()) firstError = error;
            continue;
        }
        for (int slot = 0; slot < pose::HUMAN_BONE_COUNT; slot++){
            const std::string& bone = info.humanoid.bones[(size_t)slot];
            if (bone.empty()) continue;
            for (Entity entity : boneEntities){
                if (scene->getEntityNameRef(entity) == bone){
                    slots.entities[(size_t)slot] = entity;
                    break;
                }
            }
        }
        slots.valid = true;
        return slots;
    }
    slots.error = firstError.empty() ? "no humanoid rig" : firstError;
    return slots;
}

anim::HumanoidSlots anim::humanoidSlotsByName(EntityRegistry* scene, Entity root, const std::array<std::string, pose::HUMAN_BONE_COUNT>& boneNames){
    HumanoidSlots slots;
    if (!scene || root == NULL_ENTITY || !scene->isEntityCreated(root)) return slots;
    auto transforms = scene->getComponentArray<Transform>();
    Transform* rootTransform = transforms->findComponent(root);
    if (!rootTransform) return slots;
    const size_t first = (size_t)(rootTransform - transforms->findComponentFromIndex(0));
    const size_t last = scene->findBranchLastIndex(root);
    for (size_t i = first; i <= last && i < transforms->size(); i++){
        const Entity entity = transforms->getEntity(i);
        const std::string& name = scene->getEntityNameRef(entity);
        for (int slot = 0; slot < pose::HUMAN_BONE_COUNT; slot++){
            if (slots.entities[(size_t)slot] == NULL_ENTITY && !boneNames[(size_t)slot].empty() && boneNames[(size_t)slot] == name){
                slots.entities[(size_t)slot] = entity;
            }
        }
    }
    slots.valid = slots.entities[(size_t)pose::HumanBone::Hips] != NULL_ENTITY;
    if (!slots.valid) slots.error = "no hips";
    return slots;
}

std::vector<Entity> anim::bindMaskPaths(const AvatarMaskDef& mask, EntityRegistry* scene, Entity root){
    std::vector<Entity> entities(mask.paths.size(), NULL_ENTITY);
    if (!scene) return entities;
    for (size_t i = 0; i < mask.paths.size(); i++){
        entities[i] = mask.paths[i].path.empty() ? root : resolvePath(scene, root, mask.paths[i].path);
    }
    return entities;
}

anim::MaskWeight anim::maskWeightOf(const AvatarMaskDef& mask, EntityRegistry* scene, Entity root, Entity entity,
                                    const std::vector<Entity>& pathEntities, const HumanoidSlots* humanoid){
    MaskWeight result;
    result.weight = mask.defaultWeight;
    if (!scene || entity == NULL_ENTITY) return result;

    // rule 1: the entity's own entry, else the nearest ancestor's with children (up to the root)
    if (!pathEntities.empty()){
        Entity current = entity;
        for (int depth = 0; depth < 1024 && current != NULL_ENTITY; depth++){
            for (size_t i = 0; i < pathEntities.size() && i < mask.paths.size(); i++){
                if (pathEntities[i] == current && (depth == 0 || mask.paths[i].children)){
                    result.weight = mask.paths[i].weight;
                    result.rule = MaskWeight::Rule::Path;
                    result.path = (int)i;
                    return result;
                }
            }
            if (current == root) break;
            current = parentOf(scene, current);
        }
    }

    // rule 2: the body part
    if (humanoid && humanoid->valid){
        auto slotOf = [&](Entity e) -> int {
            for (int slot = 0; slot < pose::HUMAN_BONE_COUNT; slot++){
                if (humanoid->entities[(size_t)slot] == e) return slot;
            }
            return -1;
        };
        bool found = false;
        AvatarBodyPart part = AvatarBodyPart::Body;
        Entity current = entity;
        bool below = false;     // `current` is an ancestor of the entity
        for (int depth = 0; depth < 1024 && current != NULL_ENTITY; depth++){
            const int slot = slotOf(current);
            if (slot >= 0){
                const pose::HumanBone bone = (pose::HumanBone)slot;
                if (below && bone == pose::HumanBone::LeftHand) part = AvatarBodyPart::LeftFingers;
                else if (below && bone == pose::HumanBone::RightHand) part = AvatarBodyPart::RightFingers;
                else part = bodyPartOfSlot(bone);
                found = true;
                break;
            }
            if (current == root) break;
            current = parentOf(scene, current);
            below = true;
        }
        if (!found && mask.rootHierarchy){
            // the root itself and the non-slot entities between it and the hips
            const Entity hips = humanoid->entities[(size_t)pose::HumanBone::Hips];
            bool between = entity == root;
            for (Entity e = hips; !between && e != NULL_ENTITY; e = parentOf(scene, e)){
                if (e == entity) between = true;
                if (e == root) break;
            }
            if (between){
                part = AvatarBodyPart::Root;
                found = true;
            }
        }
        if (found){
            result.weight = mask.partWeight(part);
            result.rule = mask.hasPart(part) ? MaskWeight::Rule::BodyPart : MaskWeight::Rule::Default;
            result.part = part;
            return result;
        }
    }

    return result;
}

float anim::maskWeightOf(const AvatarMaskDef& mask, EntityRegistry* scene, Entity root, Entity entity, const HumanoidSlots* humanoid){
    return maskWeightOf(mask, scene, root, entity, bindMaskPaths(mask, scene, root), humanoid).weight;
}

float anim::maskRootMotionWeight(const AvatarMaskDef& mask){
    return mask.partWeight(AvatarBodyPart::Root);
}
