// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BoneOverlay.h"

#include "Scene.h"
#include "animation/AnimatorInstance.h"
#include "component/AnimatorComponent.h"
#include "component/BoneComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"

#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{

    // The Exposed Animator on `entity` or above it whose rig holds `entity`, with a pose evaluated
    struct ExposedBone{
        const anim::AnimatorInstance* instance = nullptr;
        Entity root = NULL_ENTITY;
        int bone = -1;
    };

    ExposedBone findExposedBone(Scene* scene, Entity entity){
        ExposedBone found;
        for (Entity at = entity; at != NULL_ENTITY;){
            const AnimatorComponent* animator = scene->findComponent<AnimatorComponent>(at);
            if (animator && animator->writeBones == AnimatorWriteBones::Exposed){
                const anim::AnimatorInstance* instance = animator->runtime.get();
                if (instance){
                    const int bone = instance->rig.findBone(entity);
                    if (bone >= 0){
                        found.instance = instance;
                        found.root = at;
                        found.bone = bone;
                        return found;
                    }
                }
            }
            const Transform* transform = scene->findComponent<Transform>(at);
            at = transform ? transform->parent : NULL_ENTITY;
        }
        return found;
    }

}

std::vector<BoneOverlaySegment> editor::boneOverlaySegments(Scene* scene, const ModelComponent& model){
    std::vector<BoneOverlaySegment> segments;
    segments.reserve(model.bonesIdMapping.size());
    // a bone's position once: each bone is usually some other's parent
    std::unordered_map<Entity, Vector3> positions;
    auto positionOf = [&](Entity entity, const Transform& transform) -> Vector3{
        const auto known = positions.find(entity);
        if (known != positions.end()) return known->second;
        Vector3 position = transform.worldPosition;
        const ExposedBone exposed = findExposedBone(scene, entity);
        if (exposed.instance && (size_t)exposed.bone < exposed.instance->rigWorld().size()){
            const Vector3 inRig = exposed.instance->rigWorld()[(size_t)exposed.bone].translation();
            const Transform* root = scene->findComponent<Transform>(exposed.root);
            position = root ? root->modelMatrix * inRig : inRig;
        }
        positions.emplace(entity, position);
        return position;
    };
    for (const auto& [boneId, boneEntity] : model.bonesIdMapping){
        const Transform* transform = scene->findComponent<Transform>(boneEntity);
        if (!transform || !transform->visible) continue;
        BoneOverlaySegment segment;
        segment.bone = positionOf(boneEntity, *transform);
        if (transform->parent != NULL_ENTITY && scene->findComponent<BoneComponent>(transform->parent)){
            if (const Transform* parent = scene->findComponent<Transform>(transform->parent)){
                segment.parent = positionOf(transform->parent, *parent);
                segment.hasParent = true;
            }
        }
        segments.push_back(segment);
    }
    return segments;
}

bool editor::isUnwrittenAnimatorBone(Scene* scene, Entity entity){
    const ExposedBone exposed = findExposedBone(scene, entity);
    if (!exposed.instance || (size_t)exposed.bone >= exposed.instance->writeMask.size()) return false;
    return !exposed.instance->writeMask[(size_t)exposed.bone];
}
