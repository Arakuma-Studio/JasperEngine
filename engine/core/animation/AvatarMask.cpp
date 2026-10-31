// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AvatarMask.h"

#include "animation/AnimJson.h"
#include "animation/ClipBinding.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "pose/ModelRig.h"
#include "Scene.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace{

    using namespace jasperengine::anim::json;

    const char* const partNames[] = {"root", "body", "head", "left_leg", "right_leg", "left_arm", "right_arm",
                                     "left_fingers", "right_fingers"};
    // Unity's IK goals: no IK in masks here (spec 2)
    const char* const ikPartNames[] = {"left_foot_ik", "right_foot_ik", "left_hand_ik", "right_hand_ik"};

    const char* const knownTopLevelKeys[] = {"format", "version", "guid", "default", "humanoid", "transforms", "preview"};

    float clampWeight(float weight){
        return std::isfinite(weight) ? std::min(std::max(weight, 0.0f), 1.0f) : 1.0f;
    }

    Entity parentOf(Scene* scene, Entity entity){
        const Transform* transform = scene->findComponent<Transform>(entity);
        return transform ? transform->parent : NULL_ENTITY;
    }

    anim::AvatarMaskParseResult parseText(const std::string& text){
        anim::AvatarMaskParseResult result;
        if (anim::isUnityYamlMask(text)){
            result.error.message = anim::unityYamlMaskRefusal();
            return result;
        }
        OJson root;
        if (!parseDocument(text, root, result.error)) return result;
        if (!checkHeader(root, anim::MASK_FORMAT, anim::MASK_VERSION, "an Avatar Mask", "Avatar Mask", result.error)) return result;

        std::vector<ClipWarning>& warnings = result.warnings;
        anim::AvatarMaskData data;
        std::string unknownNames;
        for (auto it = root.begin(); it != root.end(); ++it){
            const std::string& name = it.key();
            const OJson& value = *it;
            if (name == "format" || name == "version") continue;
            if (name == "guid"){
                if (!value.is_string() || !anim::isClipGuid(value.get<std::string>())){
                    warn(&warnings, "guid", "expected 32 lowercase hex digits: dropped (a new one is given on save)");
                }else{
                    data.guid = value.get<std::string>();
                }
            }else if (name == "default"){
                float weight = 1.0f;
                if (!toWeight(value, weight)){
                    warn(&warnings, "default", "expected a weight from 0 to 1: using 1");
                    weight = 1.0f;
                }
                data.defaultWeight = weight;
            }else if (name == "humanoid"){
                if (!value.is_object()){
                    warn(&warnings, "humanoid", "expected {\"<body part>\": weight, ...}: ignored");
                    continue;
                }
                data.hasHumanoid = true;
                for (auto part = value.begin(); part != value.end(); ++part){
                    const std::string path = member("humanoid", part.key());
                    anim::AvatarMaskPart which;
                    if (!anim::avatarMaskPartFromName(part.key(), which)){
                        bool ik = false;
                        for (const char* ikName : ikPartNames){
                            if (part.key() == ikName) ik = true;
                        }
                        warn(&warnings, path, ik ? "IK goals are not masked (there is no IK pass): ignored"
                                                 : "unknown body part (root, body, head, left_leg, right_leg, left_arm, right_arm, "
                                                   "left_fingers, right_fingers): ignored");
                        continue;
                    }
                    float weight = 0.0f;
                    if (!toWeight(*part, weight)){
                        warn(&warnings, path, "expected a weight from 0 to 1 (or true/false): ignored");
                        continue;
                    }
                    data.parts[(size_t)which] = weight;
                }
            }else if (name == "transforms"){
                if (!value.is_array()){
                    warn(&warnings, "transforms", "expected an array of {\"path\", \"weight\", \"children\"}: ignored");
                    continue;
                }
                for (size_t i = 0; i < value.size(); i++){
                    const OJson& entry = value[i];
                    const std::string path = indexed("", "transforms", i);
                    if (!entry.is_object()){
                        warn(&warnings, path, "expected {\"path\", \"weight\", \"children\"}: dropped");
                        continue;
                    }
                    anim::AvatarMaskTransform transform;
                    auto pathIt = entry.find("path");
                    std::vector<anim::PathSegment> segments;
                    if (pathIt == entry.end() || !pathIt->is_string()){
                        warn(&warnings, member(path, "path"), "expected a path relative to the player root: entry dropped");
                        continue;
                    }
                    transform.path = pathIt->get<std::string>();
                    if (!transform.path.empty() && !anim::parsePath(transform.path, segments)){
                        warn(&warnings, member(path, "path"), "\"" + transform.path + "\" is not a valid path: entry dropped");
                        continue;
                    }
                    for (auto field = entry.begin(); field != entry.end(); ++field){
                        if (field.key() == "path") continue;
                        if (field.key() == "weight"){
                            if (!toWeight(*field, transform.weight)){
                                warn(&warnings, member(path, "weight"), "expected a weight from 0 to 1: using 1");
                                transform.weight = 1.0f;
                            }
                        }else if (field.key() == "children"){
                            if (!field->is_boolean()){
                                warn(&warnings, member(path, "children"), "expected true or false: using true");
                            }else{
                                transform.children = field->get<bool>();
                            }
                        }else{
                            warn(&warnings, member(path, field.key()), "unknown key ignored");
                        }
                    }
                    data.transforms.push_back(std::move(transform));
                }
            }else if (name == "preview"){
                if (!value.is_object()){
                    warn(&warnings, "preview", "expected {\"model\": \"<model or prefab path>\"}: ignored");
                    continue;
                }
                for (auto entry = value.begin(); entry != value.end(); ++entry){
                    if (entry.key() == "model" && entry->is_string()){
                        data.previewModel = entry->get<std::string>();
                    }else if (entry.key() == "model"){
                        warn(&warnings, "preview.model", "expected a model or prefab path: ignored");
                    }else{
                        warn(&warnings, "preview." + entry.key(), "unknown key ignored");
                    }
                }
            }else{
                data.unknownKeys.push_back({name, compact(value)});
                unknownNames += (unknownNames.empty() ? "" : ", ") + name;
            }
        }
        if (!unknownNames.empty()){
            warn(&warnings, "", "unknown keys kept as they are (written back on save): " + unknownNames);
        }
        anim::normalizeAvatarMask(data, &warnings);
        result.data = std::move(data);
        return result;
    }

}

const char* anim::avatarMaskPartName(AvatarMaskPart part){
    const size_t index = (size_t)part;
    return index < (size_t)AVATAR_MASK_PART_COUNT ? partNames[index] : "";
}

bool anim::avatarMaskPartFromName(const std::string& name, AvatarMaskPart& out){
    for (int i = 0; i < AVATAR_MASK_PART_COUNT; i++){
        if (name == partNames[i]){
            out = (AvatarMaskPart)i;
            return true;
        }
    }
    return false;
}

anim::AvatarMaskPart anim::avatarMaskPartOfSlot(pose::HumanBone slot){
    using pose::HumanBone;
    switch (slot){
        case HumanBone::Hips: case HumanBone::Spine: case HumanBone::Chest: case HumanBone::UpperChest:
            return AvatarMaskPart::Body;
        case HumanBone::Neck: case HumanBone::Head:
            return AvatarMaskPart::Head;
        case HumanBone::LeftShoulder: case HumanBone::LeftUpperArm: case HumanBone::LeftLowerArm: case HumanBone::LeftHand:
            return AvatarMaskPart::LeftArm;
        case HumanBone::RightShoulder: case HumanBone::RightUpperArm: case HumanBone::RightLowerArm: case HumanBone::RightHand:
            return AvatarMaskPart::RightArm;
        case HumanBone::LeftUpperLeg: case HumanBone::LeftLowerLeg: case HumanBone::LeftFoot: case HumanBone::LeftToes:
            return AvatarMaskPart::LeftLeg;
        case HumanBone::RightUpperLeg: case HumanBone::RightLowerLeg: case HumanBone::RightFoot: case HumanBone::RightToes:
            return AvatarMaskPart::RightLeg;
        case HumanBone::Count:
            break;
    }
    return AvatarMaskPart::Root;
}

float anim::AvatarMaskData::partWeight(AvatarMaskPart part) const{
    const size_t index = (size_t)part;
    if (index >= parts.size() || !parts[index]) return defaultWeight;
    return *parts[index];
}

int anim::AvatarMaskData::findTransform(const std::string& path) const{
    for (size_t i = 0; i < transforms.size(); i++){
        if (transforms[i].path == path) return (int)i;
    }
    return -1;
}

void anim::normalizeAvatarMask(AvatarMaskData& data, std::vector<ClipWarning>* warnings){
    auto clamped = [&](float value, const std::string& path){
        const float result = clampWeight(value);
        if (result != value) warn(warnings, path, shortNumber(value) + " is outside 0..1, using " + shortNumber(result));
        return result;
    };
    data.defaultWeight = clamped(data.defaultWeight, "default");
    for (int i = 0; i < AVATAR_MASK_PART_COUNT; i++){
        if (data.parts[(size_t)i]) data.parts[(size_t)i] = clamped(*data.parts[(size_t)i], member("humanoid", partNames[i]));
        if (data.parts[(size_t)i]) data.hasHumanoid = true;
    }
    if (!data.guid.empty() && !isClipGuid(data.guid)){
        warn(warnings, "guid", "not 32 lowercase hex digits: dropped");
        data.guid.clear();
    }
    // sorted by path (byte order), one entry per path: the later one wins
    std::vector<AvatarMaskTransform> transforms;
    for (size_t i = 0; i < data.transforms.size(); i++){
        AvatarMaskTransform entry = data.transforms[i];
        entry.weight = clamped(entry.weight, indexed("", "transforms", i) + ".weight");
        auto existing = std::find_if(transforms.begin(), transforms.end(), [&](const AvatarMaskTransform& t){ return t.path == entry.path; });
        if (existing != transforms.end()){
            warn(warnings, indexed("", "transforms", i), "\"" + entry.path + "\" is listed twice: the later entry is used");
            *existing = entry;
        }else{
            transforms.push_back(entry);
        }
    }
    std::stable_sort(transforms.begin(), transforms.end(), [](const AvatarMaskTransform& a, const AvatarMaskTransform& b){
        return a.path < b.path;
    });
    data.transforms = std::move(transforms);
}

anim::AvatarMaskParseResult anim::parseAvatarMask(const std::string& text){
    try{
        return parseText(text);
    }catch (const std::exception& e){
        AvatarMaskParseResult result;
        result.error.message = std::string("cannot read the Avatar Mask: ") + e.what();
        return result;
    }catch (...){
        AvatarMaskParseResult result;
        result.error.message = "cannot read the Avatar Mask";
        return result;
    }
}

std::string anim::writeAvatarMask(const AvatarMaskData& input){
    AvatarMaskData data = input;
    normalizeAvatarMask(data, nullptr);
    std::string text = "{\n";
    text += std::string("  \"format\": ") + quote(MASK_FORMAT) + ",\n";
    text += "  \"version\": " + std::to_string(MASK_VERSION) + ",\n";
    if (!data.guid.empty()) text += "  \"guid\": " + quote(data.guid) + ",\n";
    text += "  \"default\": " + number(data.defaultWeight) + ",\n";
    if (data.hasHumanoid){
        std::string parts;
        for (int i = 0; i < AVATAR_MASK_PART_COUNT; i++){
            if (!data.parts[(size_t)i]) continue;
            if (!parts.empty()) parts += ", ";
            parts += quote(partNames[i]) + ": " + number(*data.parts[(size_t)i]);
        }
        text += "  \"humanoid\": {" + parts + "},\n";
    }
    if (data.transforms.empty()){
        text += "  \"transforms\": [],\n";
    }else{
        text += "  \"transforms\": [\n";
        for (size_t i = 0; i < data.transforms.size(); i++){
            const AvatarMaskTransform& entry = data.transforms[i];
            text += "    {\"path\": " + quote(entry.path) + ", \"weight\": " + number(entry.weight) +
                    ", \"children\": " + (entry.children ? "true" : "false") + "}";
            text += (i + 1 < data.transforms.size()) ? ",\n" : "\n";
        }
        text += "  ],\n";
    }
    if (!data.previewModel.empty()) text += "  \"preview\": {\"model\": " + quote(data.previewModel) + "},\n";
    text += unknownKeysText(data.unknownKeys, knownTopLevelKeys, sizeof(knownTopLevelKeys) / sizeof(knownTopLevelKeys[0]));
    text.erase(text.size() - 2, 1);
    text += "}\n";
    return text;
}

bool anim::isUnityYamlMask(const std::string& text){
    return isUnityYaml(text, "AvatarMask:");
}

const char* anim::unityYamlMaskRefusal(){
    return "This is a Unity .mask (YAML). Create a JasperEngine Avatar Mask (Assets > Create > Avatar Mask).";
}

// ---- humanoid slots ----

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

anim::HumanoidSlots anim::humanoidSlotsByName(Scene* scene, Entity root, const std::array<std::string, pose::HUMAN_BONE_COUNT>& boneNames){
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

// ---- runtime ----

std::string anim::AvatarMaskWeight::ruleName() const{
    switch (rule){
        case Rule::Transform: return "transform";
        case Rule::Humanoid: return std::string("humanoid:") + avatarMaskPartName(part);
        case Rule::Default: break;
    }
    return "default";
}

std::shared_ptr<const anim::AvatarMask> anim::AvatarMask::build(const AvatarMaskData& data, std::vector<ClipWarning> warnings){
    AvatarMask* mask = new AvatarMask();
    std::shared_ptr<const AvatarMask> shared(mask);
    mask->data = data;
    normalizeAvatarMask(mask->data, &warnings);
    mask->warnings = std::move(warnings);
    return shared;
}

std::vector<Entity> anim::AvatarMask::bindTransforms(Scene* scene, Entity root) const{
    std::vector<Entity> entities(data.transforms.size(), NULL_ENTITY);
    for (size_t i = 0; i < data.transforms.size(); i++){
        entities[i] = data.transforms[i].path.empty() ? root : resolvePath(scene, root, data.transforms[i].path);
    }
    return entities;
}

anim::AvatarMaskWeight anim::AvatarMask::weightOf(Scene* scene, Entity root, Entity entity, const std::vector<Entity>& transformEntities,
                                                  const HumanoidSlots* humanoid) const{
    AvatarMaskWeight result;
    result.weight = data.defaultWeight;
    if (!scene || entity == NULL_ENTITY) return result;

    // rule 1: the entity's own entry, else the nearest ancestor's with children (up to the root)
    if (!transformEntities.empty()){
        Entity current = entity;
        for (int depth = 0; depth < 1024 && current != NULL_ENTITY; depth++){
            for (size_t i = 0; i < transformEntities.size() && i < data.transforms.size(); i++){
                if (transformEntities[i] == current && (depth == 0 || data.transforms[i].children)){
                    result.weight = data.transforms[i].weight;
                    result.rule = AvatarMaskWeight::Rule::Transform;
                    result.transform = (int)i;
                    return result;
                }
            }
            if (current == root) break;
            current = parentOf(scene, current);
        }
    }

    // rule 2: the body part
    if (data.hasHumanoid && humanoid && humanoid->valid){
        auto slotOf = [&](Entity e) -> int {
            for (int slot = 0; slot < pose::HUMAN_BONE_COUNT; slot++){
                if (humanoid->entities[(size_t)slot] == e) return slot;
            }
            return -1;
        };
        bool found = false;
        AvatarMaskPart part = AvatarMaskPart::Root;
        Entity current = entity;
        bool below = false;     // `current` is an ancestor of the entity
        for (int depth = 0; depth < 1024 && current != NULL_ENTITY; depth++){
            const int slot = slotOf(current);
            if (slot >= 0){
                const pose::HumanBone bone = (pose::HumanBone)slot;
                if (below && bone == pose::HumanBone::LeftHand) part = AvatarMaskPart::LeftFingers;
                else if (below && bone == pose::HumanBone::RightHand) part = AvatarMaskPart::RightFingers;
                else part = avatarMaskPartOfSlot(bone);
                found = true;
                break;
            }
            if (current == root) break;
            current = parentOf(scene, current);
            below = true;
        }
        if (!found){
            // root: the root itself and the non-slot entities between it and the hips
            const Entity hips = humanoid->entities[(size_t)pose::HumanBone::Hips];
            bool between = entity == root;
            for (Entity e = hips; !between && e != NULL_ENTITY; e = parentOf(scene, e)){
                if (e == entity) between = true;
                if (e == root) break;
            }
            if (between){
                part = AvatarMaskPart::Root;
                found = true;
            }
        }
        if (found){
            result.weight = data.partWeight(part);
            result.rule = AvatarMaskWeight::Rule::Humanoid;
            result.part = part;
            return result;
        }
    }

    return result;
}

float anim::AvatarMask::weightOf(Scene* scene, Entity root, Entity entity, const HumanoidSlots* humanoid) const{
    return weightOf(scene, root, entity, bindTransforms(scene, root), humanoid).weight;
}
