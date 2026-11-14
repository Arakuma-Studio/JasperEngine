// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RagdollBuilder.h"

#include "Scene.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "pose/HumanoidMap.h"
#include "pose/HumanoidMapIO.h"
#include "pose/ModelRig.h"
#include "util/Angle.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

using namespace jasperengine;

namespace {

    struct Frame{
        Vector3 position;
        Quaternion rotation;
        Vector3 scale;
    };

    Frame frameOf(Scene* scene, Entity entity){
        Frame frame{Vector3::ZERO, Quaternion::IDENTITY, Vector3::UNIT_SCALE};
        if (Transform* transform = scene->findComponent<Transform>(entity)){
            frame.position = transform->worldPosition;
            frame.rotation = transform->worldRotation;
            frame.scale = transform->worldScale;
        }
        return frame;
    }

    float safeDivide(float value, float by){
        return std::fabs(by) > 1.0e-9f ? value / by : value;
    }

    // Unity Transform.InverseTransformPoint: into the bone's local units (rotation and scale)
    Vector3 inverseTransformPoint(const Frame& frame, const Vector3& point){
        const Vector3 local = frame.rotation.inverse() * (point - frame.position);
        return Vector3(safeDivide(local.x, frame.scale.x), safeDivide(local.y, frame.scale.y), safeDivide(local.z, frame.scale.z));
    }

    // Unity Transform.InverseTransformDirection / TransformDirection: rotation only
    Vector3 inverseTransformDirection(const Frame& frame, const Vector3& direction){
        return frame.rotation.inverse() * direction;
    }

    Vector3 transformDirection(const Frame& frame, const Vector3& direction){
        return frame.rotation * direction;
    }

    float component(const Vector3& v, int axis){
        return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
    }

    void setComponent(Vector3& v, int axis, float value){
        if (axis == 0) v.x = value;
        else if (axis == 1) v.y = value;
        else v.z = value;
    }

    int largestComponent(const Vector3& v){
        int axis = 0;
        if (std::fabs(v.y) > std::fabs(component(v, axis))) axis = 1;
        if (std::fabs(v.z) > std::fabs(component(v, axis))) axis = 2;
        return axis;
    }

    int smallestComponent(const Vector3& v){
        int axis = 0;
        if (std::fabs(v.y) < std::fabs(component(v, axis))) axis = 1;
        if (std::fabs(v.z) < std::fabs(component(v, axis))) axis = 2;
        return axis;
    }

    // Unity CalculateDirection: the largest axis of a point and its signed distance along it
    void calculateDirection(const Vector3& point, int& direction, float& distance){
        direction = largestComponent(point);
        distance = component(point, direction);
    }

    // Unity CalculateDirectionAxis: that axis as a signed unit vector
    Vector3 directionAxis(const Vector3& point){
        int direction;
        float distance;
        calculateDirection(point, direction, distance);
        Vector3 axis = Vector3::ZERO;
        setComponent(axis, direction, distance > 0.0f ? 1.0f : -1.0f);
        return axis;
    }

    struct Bounds{
        Vector3 min = Vector3::ZERO;
        Vector3 max = Vector3::ZERO;

        void encapsulate(const Vector3& p){
            min = Vector3(std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z));
            max = Vector3(std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z));
        }
        Vector3 size() const { return max - min; }
        Vector3 center() const { return (min + max) * 0.5f; }
        void setSize(const Vector3& size){
            const Vector3 c = center();
            min = c - size * 0.5f;
            max = c + size * 0.5f;
        }
    };

    // the bone table of Unity's wizard
    struct BoneInfo{
        const char* slot;
        Entity anchor;
        const char* parentSlot;
        Vector3 axis;       // world twist axis
        Vector3 normalAxis; // world swing axis
        float minLimit;
        float maxLimit;
        float swingLimit;
        enum Collider { NONE, CAPSULE, BOX, SPHERE } collider;
        float radiusScale;
        float density;
        Entity body = NULL_ENTITY;
        float mass = 0.0f;
    };

    // a capsule along a bone-local axis, Jolt's capsule being along Y
    Quaternion axisRotation(int direction){
        if (direction == 0){
            return Quaternion(Angle::degToDefault(-90.0f), Vector3(0, 0, 1));
        }
        if (direction == 2){
            return Quaternion(Angle::degToDefault(90.0f), Vector3(1, 0, 0));
        }
        return Quaternion::IDENTITY;
    }

}

const std::vector<const char*>& RagdollBones::slotNames(){
    static const std::vector<const char*> names = {
        "pelvis", "left_hips", "left_knee", "left_foot", "right_hips", "right_knee", "right_foot",
        "left_arm", "left_elbow", "right_arm", "right_elbow", "middle_spine", "head"};
    return names;
}

Entity& RagdollBones::slot(const std::string& name){
    if (name == "pelvis") return pelvis;
    if (name == "left_hips") return leftHips;
    if (name == "left_knee") return leftKnee;
    if (name == "left_foot") return leftFoot;
    if (name == "right_hips") return rightHips;
    if (name == "right_knee") return rightKnee;
    if (name == "right_foot") return rightFoot;
    if (name == "left_arm") return leftArm;
    if (name == "left_elbow") return leftElbow;
    if (name == "right_arm") return rightArm;
    if (name == "right_elbow") return rightElbow;
    if (name == "middle_spine") return middleSpine;
    if (name == "head") return head;
    static Entity none = NULL_ENTITY;
    none = NULL_ENTITY;
    return none;
}

Entity RagdollBones::slot(const std::string& name) const{
    return const_cast<RagdollBones*>(this)->slot(name);
}

RagdollBones RagdollBuilder::guessBones(Scene* scene, Entity root, std::vector<std::string>* notes){
    RagdollBones bones;
    if (!scene || !scene->isEntityCreated(root)){
        return bones;
    }
    auto transforms = scene->getComponentArray<Transform>();
    if (!transforms->hasEntity(root)){
        return bones;
    }
    const size_t first = transforms->getIndex(root);
    const size_t last = scene->findBranchLastIndex(root);

    // the model's skeleton (its bones, the .humanoid sidecar), else every GameObject below root
    pose::Skeleton skeleton;
    std::vector<Entity> boneEntities;
    pose::HumanoidOverrides overrides;
    std::string modelPath;
    for (size_t i = first; i <= last && i < transforms->size(); i++){
        const Entity candidate = transforms->getEntity(i);
        ModelComponent* model = scene->findComponent<ModelComponent>(candidate);
        if (model && pose::buildSkeleton(scene, candidate, skeleton, boneEntities)){
            modelPath = model->filename;
            break;
        }
        skeleton = pose::Skeleton();
        boneEntities.clear();
    }
    if (!modelPath.empty()){
        std::string stamp, error;
        if (pose::humanoidSidecarStamp(modelPath, stamp, &error) && !stamp.empty()){
            std::vector<std::string> warnings;
            if (pose::parseHumanoidOverrides(stamp, overrides, warnings, &error)){
                if (notes) notes->push_back("bones from " + pose::humanoidSidecarPath(modelPath));
            }
        }
    }
    if (boneEntities.empty()){
        std::unordered_map<Entity, int> indexOf;
        for (size_t i = first; i <= last && i < transforms->size(); i++){
            const Entity candidate = transforms->getEntity(i);
            pose::SkeletonBone bone;
            bone.name = scene->getEntityName(candidate);
            const Transform& transform = transforms->getComponentFromIndex(i);
            auto parent = indexOf.find(transform.parent);
            bone.parent = (candidate != root && parent != indexOf.end()) ? parent->second : -1;
            indexOf[candidate] = (int)skeleton.bones.size();
            skeleton.bones.push_back(bone);
            boneEntities.push_back(candidate);
        }
        if (notes) notes->push_back("bones found by name");
    }

    pose::HumanoidMap map;
    std::string error;
    if (overrides.empty() || !pose::resolveHumanoid(skeleton, overrides, map, &error)){
        map = pose::autoDetectHumanoid(skeleton);
    }
    auto entityOf = [&](pose::HumanBone slot) -> Entity {
        const std::string& name = map.bones[pose::boneIndex(slot)];
        if (name.empty()) return NULL_ENTITY;
        const int index = pose::findSkeletonBone(skeleton, name);
        return (index >= 0 && (size_t)index < boneEntities.size()) ? boneEntities[(size_t)index] : NULL_ENTITY;
    };
    bones.pelvis = entityOf(pose::HumanBone::Hips);
    bones.leftHips = entityOf(pose::HumanBone::LeftUpperLeg);
    bones.leftKnee = entityOf(pose::HumanBone::LeftLowerLeg);
    bones.leftFoot = entityOf(pose::HumanBone::LeftFoot);
    bones.rightHips = entityOf(pose::HumanBone::RightUpperLeg);
    bones.rightKnee = entityOf(pose::HumanBone::RightLowerLeg);
    bones.rightFoot = entityOf(pose::HumanBone::RightFoot);
    bones.leftArm = entityOf(pose::HumanBone::LeftUpperArm);
    bones.leftElbow = entityOf(pose::HumanBone::LeftLowerArm);
    bones.rightArm = entityOf(pose::HumanBone::RightUpperArm);
    bones.rightElbow = entityOf(pose::HumanBone::RightLowerArm);
    // Unity's "Middle Spine" is the chest of a long spine (Mixamo Spine1), else the spine
    bones.middleSpine = entityOf(pose::HumanBone::Chest);
    if (bones.middleSpine == NULL_ENTITY) bones.middleSpine = entityOf(pose::HumanBone::Spine);
    bones.head = entityOf(pose::HumanBone::Head);
    return bones;
}

RagdollBuildResult RagdollBuilder::build(Scene* scene, Entity root, const RagdollBones& bones, const RagdollBuildSettings& settings){
    RagdollBuildResult result;
    auto valid = [&](Entity e){ return e != NULL_ENTITY && scene->isEntityCreated(e) && scene->findComponent<Transform>(e); };

    std::vector<std::string> missing;
    for (const char* slot : {"pelvis", "left_hips", "left_knee", "right_hips", "right_knee"}){
        if (!valid(bones.slot(slot))) missing.push_back(slot);
    }
    if (!missing.empty()){
        result.error = "a ragdoll needs the pelvis, both hips and both knees; missing:";
        for (const std::string& slot : missing) result.error += " " + slot;
        return result;
    }
    // every bone once
    std::vector<Entity> used;
    for (const char* slot : RagdollBones::slotNames()){
        const Entity e = bones.slot(slot);
        if (e == NULL_ENTITY) continue;
        if (std::find(used.begin(), used.end(), e) != used.end()){
            result.error = std::string("the same GameObject is given for two bones (") + slot + ")";
            return result;
        }
        used.push_back(e);
    }

    const Entity middleSpine = valid(bones.middleSpine) ? bones.middleSpine : NULL_ENTITY;
    const Entity head = valid(bones.head) ? bones.head : NULL_ENTITY;
    const bool leftArm = valid(bones.leftArm);
    const bool rightArm = valid(bones.rightArm);
    const bool leftElbow = leftArm && valid(bones.leftElbow);
    const bool rightElbow = rightArm && valid(bones.rightElbow);
    if (!middleSpine) result.skipped.push_back("middle_spine: none given (the arms and head join the pelvis)");
    if (!head) result.skipped.push_back("head: none given");
    if (!leftArm) result.skipped.push_back("left_arm: none given");
    if (!rightArm) result.skipped.push_back("right_arm: none given");
    if (leftArm && !leftElbow) result.skipped.push_back("left_elbow: none given");
    if (rightArm && !rightElbow) result.skipped.push_back("right_elbow: none given");

    const Frame pelvis = frameOf(scene, bones.pelvis);

    // Unity CalculateAxes: the character's axes in the pelvis's frame, snapped
    Vector3 up(0, 1, 0), right(1, 0, 0), forward(0, 0, 1);
    if (head){
        up = directionAxis(inverseTransformPoint(pelvis, frameOf(scene, head).position));
    }
    if (rightElbow){
        const Vector3 toElbow = inverseTransformPoint(pelvis, frameOf(scene, bones.rightElbow).position);
        const Vector3 removed = toElbow - up * toElbow.dotProduct(up);
        right = directionAxis(removed);
    }
    forward = right.crossProduct(up);
    if (settings.flipForward){
        forward = -forward;
    }
    const Vector3 worldRight = transformDirection(pelvis, right);
    const Vector3 worldUp = transformDirection(pelvis, up);
    const Vector3 worldForward = transformDirection(pelvis, forward);

    // the bone table (Unity's, in its order)
    std::vector<BoneInfo> table;
    table.push_back({"pelvis", bones.pelvis, nullptr, Vector3::ZERO, Vector3::ZERO, 0, 0, 0, BoneInfo::BOX, 0.0f, 2.5f});
    table.push_back({"left_hips", bones.leftHips, "pelvis", worldRight, worldForward, -20, 70, 30, BoneInfo::CAPSULE, 0.3f, 1.5f});
    table.push_back({"right_hips", bones.rightHips, "pelvis", worldRight, worldForward, -20, 70, 30, BoneInfo::CAPSULE, 0.3f, 1.5f});
    table.push_back({"left_knee", bones.leftKnee, "left_hips", worldRight, worldForward, -80, 0, 0, BoneInfo::CAPSULE, 0.25f, 1.5f});
    table.push_back({"right_knee", bones.rightKnee, "right_hips", worldRight, worldForward, -80, 0, 0, BoneInfo::CAPSULE, 0.25f, 1.5f});
    if (middleSpine){
        table.push_back({"middle_spine", middleSpine, "pelvis", worldRight, worldForward, -20, 20, 10, BoneInfo::BOX, 1.0f, 2.5f});
    }
    const char* armParent = middleSpine ? "middle_spine" : "pelvis";
    if (leftArm) table.push_back({"left_arm", bones.leftArm, armParent, worldUp, worldForward, -70, 10, 50, BoneInfo::CAPSULE, 0.25f, 1.0f});
    if (rightArm) table.push_back({"right_arm", bones.rightArm, armParent, worldUp, worldForward, -70, 10, 50, BoneInfo::CAPSULE, 0.25f, 1.0f});
    if (leftElbow) table.push_back({"left_elbow", bones.leftElbow, "left_arm", worldForward, worldUp, -90, 0, 0, BoneInfo::CAPSULE, 0.20f, 1.0f});
    if (rightElbow) table.push_back({"right_elbow", bones.rightElbow, "right_arm", worldForward, worldUp, -90, 0, 0, BoneInfo::CAPSULE, 0.20f, 1.0f});
    if (head) table.push_back({"head", head, armParent, worldRight, worldForward, -40, 25, 25, BoneInfo::SPHERE, 1.0f, 1.0f});

    auto findInfo = [&](const char* slot) -> const BoneInfo* {
        if (!slot) return nullptr;
        for (const BoneInfo& info : table){
            if (std::string(info.slot) == slot) return &info;
        }
        return nullptr;
    };
    // the bone table's children of a bone (Unity BoneInfo.children)
    auto childrenOf = [&](const char* slot){
        std::vector<const BoneInfo*> children;
        for (const BoneInfo& info : table){
            if (info.parentSlot && std::string(info.parentSlot) == slot) children.push_back(&info);
        }
        return children;
    };

    auto transforms = scene->getComponentArray<Transform>();

    // masses: density x totalMass / the sum of the densities
    float densitySum = 0.0f;
    for (const BoneInfo& info : table) densitySum += info.density;
    const float massScale = densitySum > 0.0f ? std::max(0.0f, settings.totalMass) / densitySum : 0.0f;

    // Unity GetBreastBounds: the hips and arms around a bone, the thinnest side half the largest
    auto breastBounds = [&](const Frame& relativeTo){
        Bounds bounds;
        bounds.encapsulate(inverseTransformPoint(relativeTo, frameOf(scene, bones.leftHips).position));
        bounds.encapsulate(inverseTransformPoint(relativeTo, frameOf(scene, bones.rightHips).position));
        if (leftArm) bounds.encapsulate(inverseTransformPoint(relativeTo, frameOf(scene, bones.leftArm).position));
        if (rightArm) bounds.encapsulate(inverseTransformPoint(relativeTo, frameOf(scene, bones.rightArm).position));
        Vector3 size = bounds.size();
        setComponent(size, smallestComponent(size), component(size, largestComponent(size)) / 2.0f);
        bounds.setSize(size);
        return bounds;
    };
    // Unity Clip: cut the bounds at the clip bone along their largest side, keeping the part
    // below it (or above)
    auto clip = [&](Bounds bounds, const Frame& relativeTo, Entity clipBone, bool below){
        const int axis = largestComponent(bounds.size());
        const auto toWorld = [&](const Vector3& local){
            return relativeTo.position + relativeTo.rotation * (local * relativeTo.scale);
        };
        const float clipValue = component(inverseTransformPoint(relativeTo, frameOf(scene, clipBone).position), axis);
        if ((worldUp.dotProduct(toWorld(bounds.max)) > worldUp.dotProduct(toWorld(bounds.min))) == below){
            Vector3 min = bounds.min;
            setComponent(min, axis, clipValue);
            bounds.min = min;
        }else{
            Vector3 max = bounds.max;
            setComponent(max, axis, clipValue);
            bounds.max = max;
        }
        return bounds;
    };

    for (const BoneInfo& info : table){
        const Frame frame = frameOf(scene, info.anchor);
        RagdollBuiltPart part;
        part.slot = info.slot;
        part.entity = info.anchor;
        part.mass = info.density * massScale;

        Collider3D collider;
        if (info.collider == BoneInfo::CAPSULE){
            // to the child bone; without one, as long as the bone itself, reaching its children
            int direction = 1;
            float distance = 0.0f;
            const std::vector<const BoneInfo*> children = childrenOf(info.slot);
            if (children.size() == 1){
                calculateDirection(inverseTransformPoint(frame, frameOf(scene, children[0]->anchor).position), direction, distance);
            }else{
                const BoneInfo* parent = findInfo(info.parentSlot);
                const Vector3 parentPosition = parent ? frameOf(scene, parent->anchor).position : frame.position;
                const Vector3 endPoint = (frame.position - parentPosition) + frame.position;
                calculateDirection(inverseTransformPoint(frame, endPoint), direction, distance);
                if (transforms->hasEntity(info.anchor)){
                    const size_t firstIndex = transforms->getIndex(info.anchor);
                    const size_t lastIndex = scene->findBranchLastIndex(info.anchor);
                    if (lastIndex > firstIndex){
                        Bounds bounds;
                        for (size_t i = firstIndex; i <= lastIndex && i < transforms->size(); i++){
                            bounds.encapsulate(inverseTransformPoint(frame, transforms->getComponentFromIndex(i).worldPosition));
                        }
                        distance = distance > 0.0f ? component(bounds.max, direction) : component(bounds.min, direction);
                    }
                }
            }
            const float height = std::fabs(distance);
            const float radius = std::max(1.0e-3f, height * info.radiusScale);
            collider.type = Collider3DType::CAPSULE;
            collider.radius = radius;
            // Unity's height counts the caps, Jolt's half height does not
            collider.halfHeight = std::max(0.0f, 0.5f * height - radius);
            Vector3 center = Vector3::ZERO;
            setComponent(center, direction, distance * 0.5f);
            collider.position = center;
            collider.rotation = axisRotation(direction);
            part.collider = "capsule";
            if (height < 1.0e-4f){
                result.warnings.push_back(std::string(info.slot) + ": the bone has no length, its capsule is tiny");
            }
        }else if (info.collider == BoneInfo::BOX){
            Bounds bounds;
            if (std::string(info.slot) == "pelvis"){
                bounds = middleSpine ? clip(breastBounds(frame), frame, middleSpine, false) : breastBounds(frame);
            }else{
                bounds = clip(breastBounds(frame), frame, middleSpine, true);
            }
            const Vector3 size = bounds.size();
            collider.type = Collider3DType::BOX;
            collider.width = std::max(1.0e-3f, std::fabs(size.x));
            collider.height = std::max(1.0e-3f, std::fabs(size.y));
            collider.depth = std::max(1.0e-3f, std::fabs(size.z));
            collider.position = bounds.center();
            part.collider = "box";
        }else if (info.collider == BoneInfo::SPHERE){
            // a quarter of the arm span (without arms: a sixth of the head's height above the pelvis)
            float worldRadius;
            if (leftArm && rightArm){
                worldRadius = (frameOf(scene, bones.leftArm).position - frameOf(scene, bones.rightArm).position).length() / 4.0f;
            }else{
                worldRadius = (frame.position - pelvis.position).length() / 6.0f;
                result.warnings.push_back("head: no arm span to size it, sized from its height above the pelvis");
            }
            const float maxScale = std::max(std::fabs(frame.scale.x), std::max(std::fabs(frame.scale.y), std::fabs(frame.scale.z)));
            const float radius = std::max(1.0e-3f, safeDivide(worldRadius, maxScale));
            int direction;
            float distance;
            calculateDirection(inverseTransformPoint(frame, pelvis.position), direction, distance);
            Vector3 center = Vector3::ZERO;
            setComponent(center, direction, distance > 0.0f ? -radius : radius);
            collider.type = Collider3DType::SPHERE;
            collider.radius = radius;
            collider.position = center;
            part.collider = "sphere";
        }

        RigidbodyComponent body;
        body.type = settings.startSimulated ? BodyType::DYNAMIC : BodyType::KINEMATIC;
        body.interpolation = RigidbodyInterpolation::INTERPOLATE;
        body.automaticMass = false;
        body.mass = part.mass;
        body.colliders.push_back(collider);
        part.body = body;

        if (const BoneInfo* parent = findInfo(info.parentSlot)){
            Joint3DComponent joint = makeJoint3D(Joint3DType::SWINGTWIST);
            joint.anchorSpace = JointAnchorSpace::LOCAL;
            joint.bodyA = info.anchor;
            joint.bodyB = parent->anchor;
            joint.anchor = Vector3::ZERO;
            joint.autoAnchors = true;
            joint.enableCollision = false;
            // Unity CharacterJoint: axis = twist, swingAxis = swing 1
            joint.twistAxis = directionAxis(inverseTransformDirection(frame, info.axis));
            joint.planeAxis = directionAxis(inverseTransformDirection(frame, info.normalAxis));
            joint.twistMinAngle = Angle::degToDefault(info.minLimit);
            joint.twistMaxAngle = Angle::degToDefault(info.maxLimit);
            // swing 1 turns about the swing axis: Jolt's normal half cone (measured, Physics3DJointsTest)
            joint.normalHalfConeAngle = Angle::degToDefault(info.swingLimit);
            joint.planeHalfConeAngle = 0.0f;
            part.hasJoint = true;
            part.joint = joint;
        }

        result.parts.push_back(part);
    }

    RagdollComponent ragdoll;
    ragdoll.simulated = settings.startSimulated;
    ragdoll.selfCollision = settings.selfCollision;
    ragdoll.totalMass = settings.totalMass;
    ragdoll.root = NULL_ENTITY;
    (void)root;
    result.ragdoll = ragdoll;
    result.ok = true;
    return result;
}

bool RagdollBuilder::apply(Scene* scene, Entity root, const RagdollBuildResult& result, std::string* error){
    if (!result.ok || !scene || !scene->isEntityCreated(root)){
        if (error) *error = result.ok ? "no such GameObject" : result.error;
        return false;
    }
    // a removal waits for the deferring scope to close while an add is immediate: replacing
    // there would fail half way, so it is refused before anything changes
    if (scene->isStructuralDeferActive()){
        bool replaces = scene->findComponent<RagdollComponent>(root) != nullptr;
        for (const RagdollBuiltPart& part : result.parts){
            replaces = replaces || scene->findComponent<RigidbodyComponent>(part.entity) || scene->findComponent<Joint3DComponent>(part.entity);
        }
        if (replaces){
            if (error) *error = "a bone already has a Rigidbody or Joint 3D (or the root a Ragdoll), which cannot be replaced inside a UI event handler: do it from onUpdate";
            return false;
        }
    }
    for (const RagdollBuiltPart& part : result.parts){
        if (scene->findComponent<Joint3DComponent>(part.entity)){
            scene->removeComponent<Joint3DComponent>(part.entity);
        }
        if (scene->findComponent<RigidbodyComponent>(part.entity)){
            scene->removeComponent<RigidbodyComponent>(part.entity);
        }
        scene->addComponent<RigidbodyComponent>(part.entity, part.body);
    }
    for (const RagdollBuiltPart& part : result.parts){
        if (part.hasJoint){
            scene->addComponent<Joint3DComponent>(part.entity, part.joint);
        }
    }
    if (scene->findComponent<RagdollComponent>(root)){
        scene->removeComponent<RagdollComponent>(root);
    }
    scene->addComponent<RagdollComponent>(root, result.ragdoll);
    return true;
}
