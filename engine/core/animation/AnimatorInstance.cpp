// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorInstance.h"
#include "animation/AvatarMaskWeights.h"

#include "Scene.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorIK.h"
#include "animation/ClipBinding.h"
#include "animation/KeyframeSampling.h"
#include "animation/ModelSubClips.h"
#include "component/Transform.h"
#include "pose/HumanBones.h"
#include "subsystem/MeshSystem.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::anim;

namespace{

    constexpr int kMaxDepth = 16;
    constexpr double kMaxLaps = 1000.0;
    constexpr float kPi = 3.14159265358979323846f;
    // Foot IK's contact for the debug readouts: the animated ankle within this of its planted
    // height (footHeight above the rig's floor), the height its lift is measured from
    constexpr float kFootContactLift = 0.02f;

    float wrapAngle(float a){
        while (a > kPi) a -= 2.0f * kPi;
        while (a <= -kPi) a += 2.0f * kPi;
        return a;
    }

    float yawOf(const Quaternion& q){
        const Vector3 f = q * Vector3(0, 0, 1);
        return std::atan2(f.x, f.z);
    }

    // a rotation by `yaw` radians about +y applied to v
    Vector3 rotateYaw(const Vector3& v, float yaw){
        const float c = std::cos(yaw), s = std::sin(yaw);
        return Vector3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
    }

    Quaternion yawRotation(float yaw){
        return Quaternion(std::cos(0.5f * yaw), 0.0f, std::sin(0.5f * yaw), 0.0f);
    }

    double fract(double t){
        return t - std::floor(t);
    }

    // a normalised time of a looping (or ping-pong) state, as a time in a clip of `length`
    float clipTimeAt(double normalized, float length, bool loops, bool pingPong){
        if (!(length > 0.0f)) return 0.0f;
        if (!loops) return (float)std::min(std::max(normalized, 0.0), 1.0) * length;
        if (pingPong){
            const double phase = fract(normalized * 0.5) * 2.0;
            return (float)(phase <= 1.0 ? phase : 2.0 - phase) * length;
        }
        return (float)fract(normalized) * length;
    }

    // Unity's Mathf.SmoothDamp (Game Programming Gems 4, 1.10)
    float smoothDamp(float current, float target, float& velocity, float smoothTime, float dt){
        smoothTime = std::max(0.0001f, smoothTime);
        const float omega = 2.0f / smoothTime;
        const float x = omega * dt;
        const float decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
        const float change = current - target;
        const float temp = (velocity + omega * change) * dt;
        velocity = (velocity - omega * temp) * decay;
        float result = target + (change + temp) * decay;
        if ((target - current > 0.0f) == (result > target)){
            result = target;
            velocity = dt > 0.0f ? (result - target) / dt : 0.0f;
        }
        return result;
    }

    // the clip's model ("hero.gltf#Walk" -> "hero.gltf"; a .anim's source or preview model)
    std::string sourceModelOf(const AnimationClip& clip, const std::string& path){
        return clipSourceModel(clip, path);
    }

    bool sameModel(const std::string& a, const std::string& b){
        return !a.empty() && !b.empty() && MeshSystem::getModelFilenameKey(a) == MeshSystem::getModelFilenameKey(b);
    }

    // a generic clip of another model plays retargeted: the Animator's Retarget Auto (with a model
    // of its own to retarget onto), or its state asks for the humanoid path
    bool humanoidIntent(const AnimatorClipSlot& slot, const AnimatorComponent& component, const std::string& modelPath){
        return slot.humanoid || (component.retarget == AnimatorRetarget::Auto && !modelPath.empty());
    }

}

// ---------------------------------------------------------------- set up

void anim::AnimatorInstance::setController(const std::shared_ptr<const CompiledController>& compiled, uint64_t generation,
                                           const std::vector<AnimatorClipOverride>& overrides){
    std::shared_ptr<const CompiledController> previous = controller;
    std::vector<float> oldParameters = parameters;
    controller = compiled;
    controllerGeneration = generation;
    bound = false;
    if (!controller){
        parameters.clear();
        velocities.clear();
        dampTargets.clear();
        dampTimes.clear();
        consumedTriggers.clear();
        layers.clear();
        clips.clear();
        controllerClipCount = 0;
        montages.clear();
        return;
    }

    // parameters: defaults, the values of same-named same-typed ones kept
    parameters.assign(controller->parameters.size(), 0.0f);
    velocities.assign(controller->parameters.size(), 0.0f);
    dampTargets.assign(controller->parameters.size(), 0.0f);
    dampTimes.assign(controller->parameters.size(), 0.0f);
    consumedTriggers.assign(controller->parameters.size(), 0);
    for (size_t i = 0; i < controller->parameters.size(); i++){
        const ParameterDef& p = controller->parameters[i];
        parameters[i] = p.type == AnimatorParameterType::Trigger ? 0.0f : p.defaultValue;
        if (previous){
            const int old = previous->findParameter(p.name);
            if (old >= 0 && (size_t)old < oldParameters.size() && previous->parameters[(size_t)old].type == p.type){
                parameters[i] = oldParameters[(size_t)old];
            }
        }
    }

    // clip slots (overrides applied); slots of the same clip keep their load
    std::vector<AnimatorClipSlot> oldClips = std::move(clips);
    clips.clear();
    for (size_t c = 0; c < controller->clips.size(); c++){
        const AnimatorAssetRef& ref = controller->clips[c];
        AnimatorClipSlot slot;
        slot.ref = ref;
        slot.path = ref.path;
        slot.humanoid = c < controller->clipHumanoid.size() && controller->clipHumanoid[c] != 0;
        for (const AnimatorClipOverride& entry : overrides){
            if (entry.original == ref.path && !entry.replacement.empty()){
                slot.path = entry.replacement;
                break;
            }
        }
        for (AnimatorClipSlot& old : oldClips){
            if (old.path == slot.path && old.clip){
                slot.clip = old.clip;
                slot.generation = old.generation;
                break;
            }
        }
        clips.push_back(std::move(slot));
    }
    controllerClipCount = clips.size();
    montages.clear();

    // layers: a state whose full path still exists keeps its time
    std::vector<AnimatorLayerRun> oldLayers = std::move(layers);
    layers.clear();
    layers.resize(controller->layers.size());
    for (size_t l = 0; l < layers.size(); l++){
        layers[l].weight = controller->layers[l].weight;
        bool kept = false;
        // a transition in flight completes: its destination is the layer's state (the source exits)
        if (previous && l < oldLayers.size() && oldLayers[l].transition != -1 && oldLayers[l].next.state >= 0 &&
                (size_t)oldLayers[l].next.state < previous->states.size()){
            const int state = controller->findState(previous->states[(size_t)oldLayers[l].next.state].fullPath, (int)l);
            if (state >= 0){
                if (!oldLayers[l].snapshot && oldLayers[l].current.state >= 0){
                    const int exited = controller->findState(previous->states[(size_t)oldLayers[l].current.state].fullPath, (int)l);
                    if (exited >= 0) output.states.push_back({false, (int)l, exited});
                }
                layers[l].current = oldLayers[l].next;
                layers[l].current.state = state;
                layers[l].current.entered = false;
                layers[l].weight = oldLayers[l].weight;
                kept = true;
            }
        }
        if (!kept && previous && l < oldLayers.size() && oldLayers[l].current.state >= 0 &&
                (size_t)oldLayers[l].current.state < previous->states.size()){
            const int state = controller->findState(previous->states[(size_t)oldLayers[l].current.state].fullPath, (int)l);
            if (state >= 0){
                layers[l].current = oldLayers[l].current;
                layers[l].current.state = state;
                layers[l].current.entered = false;
                layers[l].weight = oldLayers[l].weight;
                kept = true;
            }
        }
        if (!kept) resetLayer(l);
    }
}

void anim::AnimatorInstance::applyOverrides(const std::vector<AnimatorClipOverride>& overrides){
    for (size_t i = 0; i < controllerClipCount && i < clips.size(); i++){
        AnimatorClipSlot& slot = clips[i];
        std::string path = slot.ref.path;
        for (const AnimatorClipOverride& entry : overrides){
            if (entry.original == slot.ref.path && !entry.replacement.empty()){
                path = entry.replacement;
                break;
            }
        }
        if (path == slot.path) continue;
        slot.path = path;
        slot.clip.reset();
        slot.generation = 0;
        slot.bound = false;
        slot.failed = false;
        slot.reported = false;
    }
}

void anim::AnimatorInstance::resetLayer(size_t layer){
    AnimatorLayerRun& run = layers[layer];
    run.next = AnimatorStateRun();
    run.transition = -1;
    run.snapshot = false;
    run.elapsed = run.duration = 0.0f;
    run.current = AnimatorStateRun();
    const int root = controller->layers[layer].rootMachine;
    if (root >= 0) enterNode(layer, TransitionTargetKind::Machine, root, 0.0, run.current, 0);
    if (run.current.state >= 0) output.states.push_back({true, (int)layer, run.current.state});
}

void anim::AnimatorInstance::rigRequests(const AnimatorComponent& component, std::vector<std::string>& paths,
                                         std::vector<std::pair<std::string, std::pair<ClipChannel, int>>>& channelPaths,
                                         std::vector<RigPropertyRequest>* propertyPaths) const{
    paths.clear();
    channelPaths.clear();
    if (propertyPaths) propertyPaths->clear();
    for (const AnimatorClipSlot& slot : clips){
        if (!slot.clip) continue;
        // a clip that will play through the humanoid path names the source's paths, not this
        // hierarchy's: only its generic tracks (props, unmapped bones) bind by path. A humanoid
        // clip's humanoid tracks have no path.
        std::shared_ptr<const RetargetSource> source;
        const bool otherModel = !slot.clip->isHumanoid() && humanoidIntent(slot, component, modelPath) &&
                                !sameModel(sourceModelOf(*slot.clip, slot.path), modelPath) && !sourceModelOf(*slot.clip, slot.path).empty();
        if (otherModel){
            source = retargetSource(sourceModelOf(*slot.clip, slot.path), nullptr);
            if (!source) continue;
        }
        for (const AnimationClip::Track& track : slot.clip->getTracks()){
            if (!track.isSampled() || isHumanoidChannel(track.channel)) continue;
            if (source && source->roleOf(track.channel, source->boneOfPath(track.path)) != RetargetTrackRole::Generic) continue;
            if (track.channel == ClipChannel::Morph) channelPaths.push_back({track.path, {ClipChannel::Morph, (int)track.width}});
            else if (track.channel == ClipChannel::Lattice){
                for (size_t n = 0; n < track.width && track.points; n++) channelPaths.push_back({track.path, {ClipChannel::Lattice, track.points[n]}});
            }else if (track.channel == ClipChannel::Property){
                // its entity is not a bone: the property is a rig channel
                if (propertyPaths) propertyPaths->push_back({track.path, track.component, track.property, track.width});
            }else paths.push_back(track.path);
        }
    }
    pose::HumanBone slot;
    auto addPath = [&](const std::string& path){
        if (!path.empty() && !pose::humanBoneFromName(path, slot)) paths.push_back(path);
    };
    for (const AnimatorIKConstraint& constraint : component.ik){
        for (const std::string& bone : constraint.bones) addPath(bone);
        addPath(constraint.pelvis);
    }
    for (const std::string& bone : component.exposedBones) addPath(bone);
    addPath(component.rootMotionBone);
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
}

int anim::AnimatorInstance::resolveBone(const std::string& pathOrSlot) const{
    if (pathOrSlot.empty()) return -1;
    pose::HumanBone slot;
    if (pose::humanBoneFromName(pathOrSlot, slot)){
        return humanoid.valid ? humanoid.slotBones[(size_t)slot] : -1;
    }
    return rig.findBone(pathOrSlot);
}

bool anim::AnimatorInstance::humanoidSource(const AnimatorClipSlot& slot, const AnimatorComponent& component,
                                            std::shared_ptr<const RetargetSource>& source, std::string* error) const{
    source.reset();
    if (!slot.clip) return false;
    const AnimationClip& clip = *slot.clip;
    if (!humanoid.valid){
        if (clip.isHumanoid() && error) *error = "a humanoid clip needs a humanoid avatar (" + humanoid.error + "): its humanoid tracks do not play";
        return false;
    }
    // a humanoid clip: its own tracks
    if (clip.isHumanoid()) return true;
    const std::string model = sourceModelOf(clip, slot.path);
    if (!model.empty() && !sameModel(model, modelPath) && humanoidIntent(slot, component, modelPath)){
        source = retargetSource(model, error);
        return source != nullptr;
    }
    // the Animator's own model's clip (or one naming no model) on a state asking for humanoid
    // playback: read through the Animator's avatar
    if (slot.humanoid){
        source = humanoid.avatar;
        return source != nullptr;
    }
    return false;
}

void anim::AnimatorInstance::bind(Scene* scene, Entity root, const AnimatorComponent& component,
                                  const std::vector<std::shared_ptr<const AvatarMaskDef>>& masks){
    std::vector<std::string> paths;
    std::vector<std::pair<std::string, std::pair<ClipChannel, int>>> channelPaths;
    std::vector<RigPropertyRequest> propertyPaths;
    rigRequests(component, paths, channelPaths, &propertyPaths);
    rig.build(scene, root, paths, channelPaths, rest, propertyPaths);
    humanoid.build(rig, modelPath);
    hierarchyRevision = scene->hierarchyRevision();
    rootMotionTotal = Vector3(0, 0, 0);
    rootMotionTotalYaw = 0.0f;

    const size_t boneCount = rig.bones.size();
    const size_t channelCount = rig.channels.size();
    deferredProperties.assign(rig.properties.empty() ? 0 : channelCount, 0.0f);
    propertiesPending = false;
    for (Pose* pose : {&temp, &accumulate, &source, &destination, &montagePose, &finalPose}) pose->resize(boneCount, channelCount);
    finalPose.copyFrom(rig.rest);
    spaceWorld.assign(boneCount, Affine());
    spacePosition.assign(boneCount, Vector3(0, 0, 0));
    spaceRotation.assign(boneCount, Quaternion());
    spaceScale.assign(boneCount, Vector3(1, 1, 1));
    leaves.assign(controller ? (size_t)controller->maxMotionLeaves + 1 : 1, MotionLeaf());
    leafRuns.reserve(controller ? (size_t)controller->maxMotionLeaves * 2 + 4 : 4);
    crossed.reserve(32);
    output.events.reserve(64);
    output.states.reserve(16);
    output.montages.reserve(8);

    // per-layer poses and masks
    for (size_t l = 0; l < layers.size(); l++){
        AnimatorLayerRun& run = layers[l];
        run.output.resize(boneCount, channelCount);
        run.output.copyFrom(rig.rest);
        run.snapshotPose.resize(boneCount, channelCount);
        run.hasOutput = false;
        run.outputSkipped = false;
        run.snapshotPending = false;
        const AvatarMaskDef* mask = l < masks.size() ? masks[l].get() : nullptr;
        // a layer whose mask does not load contributes nothing (safer than the whole body; the
        // error is logged once by AnimatorSystem)
        const bool missing = !mask && controller && l < controller->layers.size() && !controller->layers[l].mask.empty();
        run.masked = mask != nullptr || missing;
        run.rootMask = missing ? 0.0f : 1.0f;
        run.mask.assign(rig.rest.stride(), 0.0f);
        run.channelMask.assign(rig.rest.channelStride(), 0.0f);
        if (!mask) continue;
        // S21's mask rule (AvatarMaskWeights.h) over the rig's GameObjects; the humanoid is the
        // Animator's avatar (its slot bones)
        run.rootMask = maskRootMotionWeight(*mask);
        HumanoidSlots slots;
        if (humanoid.valid){
            slots.valid = true;
            for (int s = 0; s < pose::HUMAN_BONE_COUNT; s++){
                const int bone = humanoid.slotBones[(size_t)s];
                if (bone >= 0) slots.entities[(size_t)s] = rig.bones[(size_t)bone].entity;
            }
        }
        const std::vector<Entity> pathEntities = bindMaskPaths(*mask, scene, root);
        for (size_t b = 0; b < boneCount; b++){
            run.mask[b] = maskWeightOf(*mask, scene, root, rig.bones[b].entity, pathEntities, &slots).weight;
        }
        for (size_t c = 0; c < channelCount; c++){
            // masks select transforms: property tracks always pass (the player's rule)
            run.channelMask[c] = rig.channels[c].kind == ClipChannel::Property
                ? 1.0f : maskWeightOf(*mask, scene, root, rig.channels[c].entity, pathEntities, &slots).weight;
        }
    }

    // bones written to their entities
    writeMask.assign(boneCount, component.writeBones == AnimatorWriteBones::All);
    if (component.writeBones == AnimatorWriteBones::Exposed){
        for (const std::string& path : component.exposedBones){
            const int bone = resolveBone(path);
            if (bone >= 0) writeMask[(size_t)bone] = true;
        }
        for (size_t b = 0; b < boneCount; b++){
            if (rig.rootBone && b == 0){
                writeMask[b] = true;
                continue;
            }
            // a bone with something else under it (an attachment) is written
            for (Entity child : getChildren(scene, rig.bones[b].entity)){
                if (rig.findBone(child) < 0 && scene->findComponent<Transform>(child)){
                    bool isSkinMesh = false;
                    for (const RigSkin& skin : rig.skins) isSkinMesh = isSkinMesh || skin.mesh == child;
                    if (!isSkinMesh){
                        writeMask[b] = true;
                        break;
                    }
                }
            }
        }
    }

    // IK bones
    ik.resize(component.ik.size());
    for (size_t i = 0; i < component.ik.size(); i++){
        const AnimatorIKConstraint& c = component.ik[i];
        AnimatorIKRun& run = ik[i];
        run.boneCount = 0;
        for (const std::string& bone : c.bones){
            if (run.boneCount >= 16) break;
            run.bones[run.boneCount++] = resolveBone(bone);
        }
        if ((c.type == AnimatorIKType::TwoBone || c.type == AnimatorIKType::FootIK || c.type == AnimatorIKType::FootLock) && run.boneCount == 1){
            const int end = run.bones[0];
            const int mid = end >= 0 ? rig.parents[(size_t)end] : -1;
            const int upper = mid >= 0 ? rig.parents[(size_t)mid] : -1;
            run.bones[0] = upper;
            run.bones[1] = mid;
            run.bones[2] = end;
            run.boneCount = 3;
        }
        for (int k = 0; k < 4; k++) run.boneWeights[k] = (size_t)k < c.boneWeights.size() ? c.boneWeights[(size_t)k] : 1.0f;
        run.pelvis = resolveBone(c.pelvis);
        if (run.pelvis < 0 && humanoid.valid) run.pelvis = humanoid.slotBones[(size_t)pose::HumanBone::Hips];
        if (run.pelvis < 0 && run.boneCount >= 1 && run.bones[0] >= 0) run.pelvis = rig.parents[(size_t)run.bones[0]];
    }

    bound = true;
    for (size_t s = 0; s < clips.size(); s++){
        clips[s].bound = false;
        if (clips[s].clip) bindClip(s, component);
    }
}

void anim::AnimatorInstance::bindClip(size_t index, const AnimatorComponent& component){
    AnimatorClipSlot& slot = clips[index];
    slot.bound = false;
    slot.retargeted = false;
    slot.hasReference = false;
    if (!slot.clip || !bound) return;
    const AnimationClip& clip = *slot.clip;
    std::shared_ptr<const RetargetSource> source;
    std::string error;
    if (humanoidSource(slot, component, source, &error)){
        slot.retargeted = slot.retarget.bind(clip, source);
        if (slot.retargeted) slot.retargetCursors.assign(clip.getTracks().size(), KeyCursor());
    }else if (!error.empty() && !slot.reported){
        slot.error = error;
    }
    // the tracks that play by path: every track, or a retargeted clip's generic ones
    slot.binding.bind(slot.clip, rig, slot.retargeted ? &slot.retarget.byPath : nullptr);
    slot.cursors.resize(slot.binding.groups.size());

    // root motion: the clip's bone, the Animator's, the hips, the shallowest bone the clip moves
    slot.rootMotion = readClipRootMotion(clip.getUnknownKeys());
    slot.rootBone = -1;
    slot.rootPositionTrack = slot.rootRotationTrack = -1;
    if (slot.retargeted){
        slot.rootBone = humanoid.valid ? humanoid.slotBones[(size_t)pose::HumanBone::Hips] : -1;
    }else{
        slot.rootBone = resolveBone(slot.rootMotion.bone);
        if (slot.rootBone < 0) slot.rootBone = resolveBone(component.rootMotionBone);
        if (slot.rootBone < 0 && humanoid.valid) slot.rootBone = humanoid.slotBones[(size_t)pose::HumanBone::Hips];
        if (slot.rootBone < 0){
            int bestDepth = 1 << 30;
            for (const RigClipBinding::Target& target : slot.binding.targets){
                if (target.channel != ClipChannel::Position || target.index < 0) continue;
                if (rig.rootBone && target.index == 0) continue;
                int depth = 0;
                for (int p = rig.parents[(size_t)target.index]; p >= 0; p = rig.parents[(size_t)p]) depth++;
                if (depth < bestDepth){
                    bestDepth = depth;
                    slot.rootBone = target.index;
                }
            }
        }
        if (slot.rootBone >= 0){
            for (const RigClipBinding::Target& target : slot.binding.targets){
                if (target.index != slot.rootBone) continue;
                if (target.channel == ClipChannel::Position) slot.rootPositionTrack = (int)target.track;
                if (target.channel == ClipChannel::Rotation) slot.rootRotationTrack = (int)target.track;
            }
        }
    }
    if (slot.rootBone >= 0){
        // the parent chain at rest
        Affine chain;
        Quaternion rotation;
        std::vector<int> ancestors;
        for (int p = rig.parents[(size_t)slot.rootBone]; p >= 0; p = rig.parents[(size_t)p]) ancestors.push_back(p);
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it){
            chain = chain * Affine::compose(rig.rest.position((size_t)*it), rig.rest.rotation((size_t)*it), rig.rest.scale((size_t)*it));
            rotation = rotation * rig.rest.rotation((size_t)*it);
        }
        slot.rootParent = chain;
        slot.rootParentInverse = chain.inverse();
        slot.rootParentRotation = rotation;
        Quaternion start;
        rootSample(slot, 0.0f, slot.rootStart, start);
        slot.rootStartYaw = yawOf(start);
        rootPoint(slot, 0.0f, slot.lapStart, slot.lapStartHeading);
        rootPoint(slot, slot.length(), slot.lapEnd, slot.lapEndHeading);
    }
    slot.bound = true;
}

// ---------------------------------------------------------------- control

int anim::AnimatorInstance::findParameter(const std::string& name) const{
    return controller ? controller->findParameter(name) : -1;
}

bool anim::AnimatorInstance::setFloat(const std::string& name, float value){
    const int index = findParameter(name);
    if (index < 0 || !std::isfinite(value)) return false;
    const AnimatorParameterType type = controller->parameters[(size_t)index].type;
    if (type == AnimatorParameterType::Int) value = std::round(value);
    else if (type != AnimatorParameterType::Float) value = value != 0.0f ? 1.0f : 0.0f;
    parameters[(size_t)index] = value;
    velocities[(size_t)index] = 0.0f;
    dampTimes[(size_t)index] = 0.0f;
    return true;
}

bool anim::AnimatorInstance::setFloatTarget(const std::string& name, float value, float dampTime){
    const int index = findParameter(name);
    if (index < 0 || !std::isfinite(value)) return false;
    if (controller->parameters[(size_t)index].type != AnimatorParameterType::Float || !(dampTime > 0.0f) || !std::isfinite(dampTime)){
        return setFloat(name, value);
    }
    dampTargets[(size_t)index] = value;
    dampTimes[(size_t)index] = dampTime;
    return true;
}

bool anim::AnimatorInstance::setFloat(const std::string& name, float value, float dampTime, float deltaTime){
    const int index = findParameter(name);
    if (index < 0 || !std::isfinite(value)) return false;
    dampTimes[(size_t)index] = 0.0f;
    if (controller->parameters[(size_t)index].type != AnimatorParameterType::Float) return setFloat(name, value);
    if (!(dampTime > 0.0f) || !(deltaTime > 0.0f)){
        parameters[(size_t)index] = value;
        velocities[(size_t)index] = 0.0f;
        return true;
    }
    parameters[(size_t)index] = smoothDamp(parameters[(size_t)index], value, velocities[(size_t)index], dampTime, deltaTime);
    return true;
}

bool anim::AnimatorInstance::setInteger(const std::string& name, int value){
    return setFloat(name, (float)value);
}

bool anim::AnimatorInstance::setBool(const std::string& name, bool value){
    return setFloat(name, value ? 1.0f : 0.0f);
}

bool anim::AnimatorInstance::setTrigger(const std::string& name){
    return setFloat(name, 1.0f);
}

bool anim::AnimatorInstance::resetTrigger(const std::string& name){
    return setFloat(name, 0.0f);
}

float anim::AnimatorInstance::getParameter(const std::string& name) const{
    const int index = findParameter(name);
    return index >= 0 ? parameters[(size_t)index] : 0.0f;
}

bool anim::AnimatorInstance::play(const std::string& stateName, int layer, float normalizedTime){
    if (!controller) return false;
    const int state = controller->findState(stateName, layer);
    if (state < 0) return false;
    const size_t l = (size_t)controller->states[(size_t)state].layer;
    AnimatorLayerRun& run = layers[l];
    const bool same = !run.snapshot && run.transition == -1 && run.current.state == state;
    if (!same){
        if (run.current.state >= 0 && !run.snapshot) output.states.push_back({false, (int)l, run.current.state});
        if (run.transition != -1 && run.next.state >= 0) output.states.push_back({false, (int)l, run.next.state});
        output.states.push_back({true, (int)l, state});
    }
    const double time = normalizedTime >= 0.0f ? (double)normalizedTime : (same ? run.current.time : 0.0);
    run.current = AnimatorStateRun();
    run.current.state = state;
    run.current.time = run.current.previous = time;
    run.current.entered = false;    // a seek fires nothing
    run.next = AnimatorStateRun();
    run.transition = -1;
    run.snapshot = false;
    return true;
}

bool anim::AnimatorInstance::crossFade(const std::string& stateName, float duration, bool fixed, int layer, float offset){
    if (!controller) return false;
    const int state = controller->findState(stateName, layer);
    if (state < 0) return false;
    const size_t l = (size_t)controller->states[(size_t)state].layer;
    AnimatorLayerRun& run = layers[l];
    const float sourceLength = run.transition != -1 ? run.next.length : run.current.length;
    const float seconds = std::max(0.0f, fixed ? duration : duration * sourceLength);
    if (run.current.state < 0 || seconds <= 0.0f) return play(stateName, (int)l, std::max(0.0f, offset));
    if (run.transition != -1){
        // an interruption: blend from the frozen pose
        freezeLayerOutput(run);
        if (!run.snapshot && run.current.state >= 0) output.states.push_back({false, (int)l, run.current.state});
        if (run.next.state >= 0) output.states.push_back({false, (int)l, run.next.state});
        run.current = run.next;
        run.snapshot = true;
    }
    run.next = AnimatorStateRun();
    run.next.state = state;
    run.next.time = run.next.previous = std::min(std::max(offset, 0.0f), 1.0f);
    run.next.entered = true;
    run.next.startPending = true;
    run.next.start = run.next.time;
    output.states.push_back({true, (int)l, state});
    run.transition = -2;
    run.elapsed = 0.0f;
    run.duration = seconds;
    return true;
}

void anim::AnimatorInstance::setLayerWeight(int layer, float weight){
    setLayerWeight(layer, weight, 0.0f);
}

void anim::AnimatorInstance::setLayerWeight(int layer, float weight, float fade){
    if (layer < 0 || (size_t)layer >= layers.size() || !std::isfinite(weight)) return;
    AnimatorLayerRun& run = layers[(size_t)layer];
    run.targetWeight = layer == 0 ? 1.0f : std::min(std::max(weight, 0.0f), 1.0f);
    if (!(fade > 0.0f) || !std::isfinite(fade) || layer == 0){
        run.weight = run.targetWeight;
        run.weightFadeSpeed = 0.0f;
        return;
    }
    // linear over `fade` seconds of the Animator's time (upstream's layer fades)
    run.weightFadeSpeed = std::fabs(run.targetWeight - run.weight) / fade;
}

void anim::AnimatorInstance::restart(){
    if (!controller) return;
    for (size_t l = 0; l < layers.size(); l++){
        if (layers[l].current.state >= 0 && !layers[l].snapshot) output.states.push_back({false, (int)l, layers[l].current.state});
        if (layers[l].transition != -1 && layers[l].next.state >= 0) output.states.push_back({false, (int)l, layers[l].next.state});
        resetLayer(l);
    }
    stopMontage(nullptr, 0.0f);
}

float anim::AnimatorInstance::getLayerWeight(int layer) const{
    if (layer < 0 || (size_t)layer >= layers.size()) return 0.0f;
    return layers[(size_t)layer].weight;
}

int anim::AnimatorInstance::getLayerIndex(const std::string& name) const{
    return controller ? controller->findLayer(name) : -1;
}

size_t anim::AnimatorInstance::montageClipSlot(const AnimatorAssetRef& ref){
    for (size_t i = controllerClipCount; i < clips.size(); i++){
        if (clips[i].path == ref.path) return i;
    }
    for (size_t i = 0; i < controllerClipCount; i++){
        if (clips[i].path == ref.path) return i;
    }
    AnimatorClipSlot slot;
    slot.ref = ref;
    slot.path = ref.path;
    clips.push_back(std::move(slot));
    return clips.size() - 1;
}

bool anim::AnimatorInstance::playMontage(const std::string& path, const std::shared_ptr<const MontageDef>& montage, size_t clipSlot,
                                         float rate, const std::string& section){
    if (!controller || !montage) return false;
    int layer = -1;
    for (size_t l = 0; l < controller->layers.size(); l++){
        if (controller->layers[l].slot == montage->slot){
            layer = (int)l;
            break;
        }
    }
    if (layer < 0 && montage->slot == "Default" && !controller->layers.empty()) layer = 0;
    if (layer < 0) return false;
    // the montage playing on that slot blends out, interrupted; older ones end now
    for (size_t m = 0; m < montages.size(); m++){
        AnimatorMontageRun& other = montages[m];
        if (other.layer != layer || other.phase == AnimatorMontageRun::Phase::Ended) continue;
        if (other.phase == AnimatorMontageRun::Phase::BlendOut){
            other.phase = AnimatorMontageRun::Phase::Ended;
            other.weight = 0.0f;
            output.montages.push_back({(int)m, true});
            continue;
        }
        other.phase = AnimatorMontageRun::Phase::BlendOut;
        other.interrupted = true;
        other.blendFrom = other.weight;
        other.blendElapsed = 0.0f;
        other.blendDuration = montage->blendIn.time;
        EaseType ease = EaseType::LINEAR;
        easeFromName(montage->blendIn.ease, ease);
        other.ease = ease;
    }
    AnimatorMontageRun run;
    run.path = path;
    run.montage = montage;
    run.clip = (int)clipSlot;
    run.layer = layer;
    run.rate = (std::isfinite(rate) && rate != 0.0f ? rate : 1.0f) * montage->rate;
    run.section = 0;
    if (!section.empty()){
        const int index = montage->findSection(section);
        if (index >= 0) run.section = index;
    }
    run.nextOverride.assign(montage->sections.size(), std::string());
    run.hasNextOverride.assign(montage->sections.size(), false);
    run.phase = AnimatorMontageRun::Phase::Waiting;
    EaseType ease = EaseType::LINEAR;
    easeFromName(montage->blendIn.ease, ease);
    run.ease = ease;
    run.blendDuration = montage->blendIn.time;
    montages.push_back(std::move(run));
    return true;
}

void anim::AnimatorInstance::stopMontage(const std::string* path, float blendOut){
    for (AnimatorMontageRun& run : montages){
        if (run.phase == AnimatorMontageRun::Phase::Ended || run.phase == AnimatorMontageRun::Phase::BlendOut) continue;
        if (path && run.path != *path) continue;
        run.phase = AnimatorMontageRun::Phase::BlendOut;
        run.interrupted = true;
        run.blendFrom = run.weight;
        run.blendElapsed = 0.0f;
        run.blendDuration = blendOut >= 0.0f ? blendOut : run.montage->blendOut.time;
        EaseType ease = EaseType::LINEAR;
        easeFromName(run.montage->blendOut.ease, ease);
        run.ease = ease;
    }
}

bool anim::AnimatorInstance::isMontagePlaying() const{
    return activeMontage() != nullptr;
}

const AnimatorMontageRun* anim::AnimatorInstance::activeMontage() const{
    for (auto it = montages.rbegin(); it != montages.rend(); ++it){
        if (it->phase != AnimatorMontageRun::Phase::Ended && it->phase != AnimatorMontageRun::Phase::BlendOut) return &*it;
    }
    return nullptr;
}

bool anim::AnimatorInstance::montageJumpToSection(const std::string& section){
    for (auto it = montages.rbegin(); it != montages.rend(); ++it){
        if (it->phase == AnimatorMontageRun::Phase::Ended || it->phase == AnimatorMontageRun::Phase::BlendOut) continue;
        const int index = it->montage->findSection(section);
        if (index < 0) return false;
        it->section = index;
        it->time = it->previous = it->montage->sections[(size_t)index].start;
        return true;
    }
    return false;
}

bool anim::AnimatorInstance::montageSetNextSection(const std::string& from, const std::string& to){
    for (auto it = montages.rbegin(); it != montages.rend(); ++it){
        if (it->phase == AnimatorMontageRun::Phase::Ended || it->phase == AnimatorMontageRun::Phase::BlendOut) continue;
        const int index = it->montage->findSection(from);
        if (index < 0 || (!to.empty() && it->montage->findSection(to) < 0)) return false;
        it->nextOverride[(size_t)index] = to;
        it->hasNextOverride[(size_t)index] = true;
        return true;
    }
    return false;
}

bool anim::AnimatorInstance::warnOnce(uint64_t key){
    if (std::find(warnedKeys.begin(), warnedKeys.end(), key) != warnedKeys.end()) return false;
    warnedKeys.push_back(key);
    return true;
}

bool anim::AnimatorInstance::warnOnce(const std::string& message){
    if (std::find(warnedNames.begin(), warnedNames.end(), message) != warnedNames.end()) return false;
    warnedNames.push_back(message);
    return true;
}

// ---------------------------------------------------------------- queries

anim::AnimatorInstance::StateInfo anim::AnimatorInstance::currentState(int layer) const{
    StateInfo info;
    if (!controller || layer < 0 || (size_t)layer >= layers.size()) return info;
    const AnimatorStateRun& run = layers[(size_t)layer].current;
    if (run.state < 0) return info;
    const CompiledState& state = controller->states[(size_t)run.state];
    info.state = run.state;
    info.name = state.name;
    info.fullPath = state.fullPath;
    info.tag = state.tag;
    info.normalizedTime = (float)run.time;
    info.length = run.length;
    info.loop = stateLoops(run.state, -1);
    info.speed = stateSpeed(run.state);
    info.mirror = controller->states[(size_t)run.state].mirror;
    info.footIK = controller->states[(size_t)run.state].footIK;
    return info;
}

anim::AnimatorInstance::StateInfo anim::AnimatorInstance::nextState(int layer) const{
    StateInfo info;
    if (!controller || layer < 0 || (size_t)layer >= layers.size() || layers[(size_t)layer].transition == -1) return info;
    const AnimatorStateRun& run = layers[(size_t)layer].next;
    if (run.state < 0) return info;
    const CompiledState& state = controller->states[(size_t)run.state];
    info.state = run.state;
    info.name = state.name;
    info.fullPath = state.fullPath;
    info.tag = state.tag;
    info.normalizedTime = (float)run.time;
    info.length = run.length;
    info.loop = stateLoops(run.state, -1);
    info.speed = stateSpeed(run.state);
    info.mirror = controller->states[(size_t)run.state].mirror;
    info.footIK = controller->states[(size_t)run.state].footIK;
    return info;
}

bool anim::AnimatorInstance::isInTransition(int layer) const{
    return layer >= 0 && (size_t)layer < layers.size() && layers[(size_t)layer].transition != -1;
}

float anim::AnimatorInstance::transitionProgress(int layer) const{
    if (!isInTransition(layer)) return 0.0f;
    const AnimatorLayerRun& run = layers[(size_t)layer];
    return run.duration > 0.0f ? std::min(1.0f, run.elapsed / run.duration) : 1.0f;
}

// ---------------------------------------------------------------- state machine

void anim::AnimatorInstance::enterNode(size_t layer, TransitionTargetKind kind, int target, double offset, AnimatorStateRun& run, int depth){
    run = AnimatorStateRun();
    if (depth > kMaxDepth || target < 0) return;
    switch (kind){
        case TransitionTargetKind::State:
            run.state = target;
            run.time = run.previous = offset;
            run.entered = true;
            run.startPending = true;
            run.start = offset;
            return;
        case TransitionTargetKind::Machine: {
            const CompiledMachine& machine = controller->machines[(size_t)target];
            for (uint32_t i = 0; i < machine.entryCount; i++){
                const CompiledTransition& t = controller->transitions[machine.entryBegin + i];
                if (t.mute || t.kind == TransitionTargetKind::None || !conditionsHold(t)) continue;
                consumeTriggers(t);
                enterNode(layer, t.kind, t.target, offset, run, depth + 1);
                return;
            }
            enterNode(layer, machine.defaultKind, machine.defaultTarget, offset, run, depth + 1);
            return;
        }
        case TransitionTargetKind::Exit: {
            TransitionTargetKind nextKind;
            int nextTarget;
            if (resolveExit(target, nextKind, nextTarget, depth + 1)) enterNode(layer, nextKind, nextTarget, offset, run, depth + 1);
            return;
        }
        case TransitionTargetKind::None:
            return;
    }
}

bool anim::AnimatorInstance::resolveExit(int machineIndex, TransitionTargetKind& kind, int& target, int depth){
    if (depth > kMaxDepth) return false;
    const CompiledMachine& machine = controller->machines[(size_t)machineIndex];
    if (machine.parent < 0){
        kind = TransitionTargetKind::Machine;
        target = machineIndex;
        return true;
    }
    for (uint32_t i = 0; i < machine.exitCount; i++){
        const CompiledTransition& t = controller->transitions[machine.exitBegin + i];
        if (t.mute || t.kind == TransitionTargetKind::None || !conditionsHold(t)) continue;
        consumeTriggers(t);
        kind = t.kind;
        target = t.target;
        return true;
    }
    kind = TransitionTargetKind::Machine;
    target = machine.parent;
    return true;
}

bool anim::AnimatorInstance::conditionsHold(const CompiledTransition& t) const{
    for (uint32_t i = 0; i < t.conditionCount; i++){
        const CompiledCondition& c = controller->conditions[t.conditionBegin + i];
        if (c.parameter < 0) return false;
        const float v = parameters[(size_t)c.parameter];
        switch (c.mode){
            case AnimatorConditionMode::If: if (!(v != 0.0f)) return false; break;
            case AnimatorConditionMode::IfNot: if (v != 0.0f) return false; break;
            case AnimatorConditionMode::Greater: if (!(v > c.threshold)) return false; break;
            case AnimatorConditionMode::Less: if (!(v < c.threshold)) return false; break;
            case AnimatorConditionMode::Equals: if (std::round(v) != std::round(c.threshold)) return false; break;
            case AnimatorConditionMode::NotEqual: if (std::round(v) == std::round(c.threshold)) return false; break;
        }
    }
    return true;
}

void anim::AnimatorInstance::consumeTriggers(const CompiledTransition& t){
    for (uint32_t i = 0; i < t.conditionCount; i++){
        const CompiledCondition& c = controller->conditions[t.conditionBegin + i];
        if (c.parameter >= 0 && controller->parameters[(size_t)c.parameter].type == AnimatorParameterType::Trigger){
            // reset after every layer of this update looked at it: a trigger two layers read
            // fires on both (Unity's rule; upstream's player too)
            if ((size_t)c.parameter < consumedTriggers.size()) consumedTriggers[(size_t)c.parameter] = 1;
            else parameters[(size_t)c.parameter] = 0.0f;
        }
    }
}

bool anim::AnimatorInstance::exitTimeReached(const CompiledTransition& t, const AnimatorStateRun& run) const{
    if (!t.hasExitTime) return true;
    if (!(run.time > run.previous)) return false;
    const double e = t.exitTime;
    if (e < 1.0) return std::floor(run.time - e) > std::floor(run.previous - e);
    return run.previous < e && run.time >= e;
}

bool anim::AnimatorInstance::tryTransitions(size_t layer, uint32_t begin, uint32_t count, bool anyState, int stopAt, const AnimatorStateRun& from){
    const AnimatorLayerRun& run = layers[layer];
    for (uint32_t i = begin; i < begin + count; i++){
        if (stopAt >= 0 && (int)i >= stopAt) break;
        if ((int)i == run.transition) continue;
        const CompiledTransition& t = controller->transitions[i];
        if (t.mute || t.kind == TransitionTargetKind::None) continue;
        if (!t.hasExitTime && t.conditionCount == 0) continue;
        if (anyState && !t.canTransitionToSelf && t.kind == TransitionTargetKind::State){
            const int showing = run.transition != -1 ? run.next.state : run.current.state;
            if (t.target == showing) continue;
        }
        if (!exitTimeReached(t, from) || !conditionsHold(t)) continue;
        startTransition(layer, (int)i);
        return true;
    }
    return false;
}

void anim::AnimatorInstance::freezeLayerOutput(AnimatorLayerRun& run){
    if (run.outputSkipped){
        run.snapshotPending = true;
    }else if (run.hasOutput){
        run.snapshotPose.copyFrom(run.output);
    }else{
        run.snapshotPose.copyFrom(rig.rest);
    }
}

void anim::AnimatorInstance::startTransition(size_t layer, int index){
    AnimatorLayerRun& run = layers[layer];
    const CompiledTransition& t = controller->transitions[(size_t)index];
    consumeTriggers(t);
    AnimatorStateRun destination;
    enterNode(layer, t.kind, t.target, t.offset, destination, 0);
    if (destination.state < 0) return;
    const bool interrupting = run.transition != -1;
    // the duration in the source state's lengths: the state the transition leaves
    float sourceLength = interrupting ? run.next.length : run.current.length;
    if (t.ownerState >= 0 && t.ownerState == run.current.state && !run.snapshot) sourceLength = run.current.length;
    const float seconds = std::max(0.0f, t.fixedDuration ? t.duration : t.duration * sourceLength);
    if (seconds <= 0.0f){
        if (run.current.state >= 0 && !run.snapshot){
            output.states.push_back({false, (int)layer, run.current.state});
            if (!interrupting) run.instantSource = run.current;
        }
        if (interrupting && run.next.state >= 0) output.states.push_back({false, (int)layer, run.next.state});
        output.states.push_back({true, (int)layer, destination.state});
        run.current = destination;
        run.next = AnimatorStateRun();
        run.transition = -1;
        run.snapshot = false;
        return;
    }
    if (interrupting){
        // freeze what the layer showed: the new transition blends from it
        freezeLayerOutput(run);
        if (!run.snapshot && run.current.state >= 0) output.states.push_back({false, (int)layer, run.current.state});
        if (run.next.state >= 0) output.states.push_back({false, (int)layer, run.next.state});
        run.current = run.next;
        run.current.entered = false;
        run.snapshot = true;
    }
    output.states.push_back({true, (int)layer, destination.state});
    run.next = destination;
    run.transition = index;
    run.elapsed = 0.0f;
    run.duration = seconds;
}

float anim::AnimatorInstance::stateSpeed(int state) const{
    const CompiledState& s = controller->states[(size_t)state];
    float speed = s.speed;
    if (s.speedParameter >= 0) speed *= parameters[(size_t)s.speedParameter];
    return std::isfinite(speed) ? speed : 0.0f;
}

float anim::AnimatorInstance::motionLength(const CompiledMotion& motion){
    const size_t count = motionLeaves(*controller, motion, parameters.data(), 1.0f, leaves.data(), 0, leaves.size());
    float length = 0.0f;
    float total = 0.0f;
    for (size_t i = 0; i < count; i++){
        const AnimatorClipSlot& slot = clips[(size_t)leaves[i].clip];
        if (!slot.ready()) continue;
        const float scale = leaves[i].timeScale > 1e-6f ? leaves[i].timeScale : 1e-6f;
        length += leaves[i].weight * slot.length() / scale;
        total += leaves[i].weight;
    }
    return total > 0.0f ? length / total : 0.0f;
}

bool anim::AnimatorInstance::stateLoops(int state, int clipSlot) const{
    const CompiledState& s = controller->states[(size_t)state];
    if (s.loop == AnimatorStateLoop::Loop || s.loop == AnimatorStateLoop::PingPong || s.loop == AnimatorStateLoop::Repeat) return true;
    if (s.loop == AnimatorStateLoop::Once || s.loop == AnimatorStateLoop::ClampForever) return false;
    // inherit: the clip's wrap (a tree: its first ready leaf's)
    if (clipSlot < 0){
        if (s.motion.kind == CompiledMotion::Kind::Clip) clipSlot = s.motion.index;
        else return true;
    }
    const AnimatorClipSlot& slot = clips[(size_t)clipSlot];
    if (!slot.clip) return false;
    const ClipWrapMode wrap = slot.clip->getWrap();
    return wrap == ClipWrapMode::Loop || wrap == ClipWrapMode::PingPong;
}

bool anim::AnimatorInstance::statePingPong(int state, int clipSlot) const{
    const CompiledState& s = controller->states[(size_t)state];
    if (s.loop == AnimatorStateLoop::PingPong) return true;
    // repeat: always forward; once / clamp_forever: no loop
    if (s.loop != AnimatorStateLoop::Inherit && s.loop != AnimatorStateLoop::Loop) return false;
    if (clipSlot < 0 || !clips[(size_t)clipSlot].clip) return false;
    return stateLoops(state, clipSlot) && clips[(size_t)clipSlot].clip->getWrap() == ClipWrapMode::PingPong;
}

void anim::AnimatorInstance::advanceLayer(size_t layer, float dt){
    AnimatorLayerRun& run = layers[layer];
    run.current.entered = false;
    run.next.entered = false;
    // times
    AnimatorStateRun* runs[2] = {run.snapshot ? nullptr : &run.current, run.transition != -1 ? &run.next : nullptr};
    for (AnimatorStateRun* state : runs){
        if (!state || state->state < 0) continue;
        const CompiledState& s = controller->states[(size_t)state->state];
        state->previous = state->time;
        state->length = motionLength(s.motion);
        if (s.timeParameter >= 0){
            state->time = parameters[(size_t)s.timeParameter];
        }else{
            const float length = state->length > 0.0f ? state->length : 1.0f;
            state->time += (double)(dt * stateSpeed(state->state) / length);
        }
    }
    if (run.snapshot && run.current.state >= 0) run.current.previous = run.current.time;

    // a transition starts or interrupts (D5)
    const CompiledLayer& compiled = controller->layers[layer];
    bool started = tryTransitions(layer, compiled.anyBegin, compiled.anyCount, true, -1, run.transition != -1 ? run.next : run.current);
    if (!started){
        if (run.transition == -1){
            if (run.current.state >= 0){
                const CompiledState& s = controller->states[(size_t)run.current.state];
                started = tryTransitions(layer, s.transitionBegin, s.transitionCount, false, -1, run.current);
            }
        }else{
            const TransitionInterruptionSource source = run.transition >= 0 ?
                controller->transitions[(size_t)run.transition].interruptionSource : TransitionInterruptionSource::None;
            const int activeOwner = run.transition >= 0 ? controller->transitions[(size_t)run.transition].ownerState : -1;
            auto tryCurrent = [&](){
                if (run.snapshot || run.current.state < 0) return false;
                const CompiledState& s = controller->states[(size_t)run.current.state];
                int stopAt = -1;
                if (run.transition >= 0 && controller->transitions[(size_t)run.transition].orderedInterruption && activeOwner == run.current.state){
                    stopAt = run.transition;
                }
                return tryTransitions(layer, s.transitionBegin, s.transitionCount, false, stopAt, run.current);
            };
            auto tryNext = [&](){
                if (run.next.state < 0) return false;
                const CompiledState& s = controller->states[(size_t)run.next.state];
                return tryTransitions(layer, s.transitionBegin, s.transitionCount, false, -1, run.next);
            };
            switch (source){
                case TransitionInterruptionSource::None: break;
                case TransitionInterruptionSource::Current: started = tryCurrent(); break;
                case TransitionInterruptionSource::Next: started = tryNext(); break;
                case TransitionInterruptionSource::CurrentThenNext: started = tryCurrent() || tryNext(); break;
                case TransitionInterruptionSource::NextThenCurrent: started = tryNext() || tryCurrent(); break;
            }
        }
    }

    // the transition's progress (not in the update that started it)
    if (!started && run.transition != -1){
        run.elapsed += std::fabs(dt);
        if (run.elapsed >= run.duration){
            if (run.current.state >= 0 && !run.snapshot) output.states.push_back({false, (int)layer, run.current.state});
            run.current = run.next;
            run.next = AnimatorStateRun();
            run.transition = -1;
            run.snapshot = false;
            run.elapsed = run.duration = 0.0f;
        }
    }
}

// ---------------------------------------------------------------- events and root motion

void anim::AnimatorInstance::collectLeaves(const AnimatorStateRun& run, bool next, float weight, bool leaderOnly, float& leaderWeight, int& leaderIndex){
    if (run.state < 0 || !(weight > 0.0f)) return;
    const CompiledState& s = controller->states[(size_t)run.state];
    const size_t count = motionLeaves(*controller, s.motion, parameters.data(), weight, leaves.data(), 0, leaves.size());
    float offset = s.cycleOffset;
    if (s.cycleOffsetParameter >= 0) offset = parameters[(size_t)s.cycleOffsetParameter];
    for (size_t i = 0; i < count; i++){
        const AnimatorClipSlot& slot = clips[(size_t)leaves[i].clip];
        if (!slot.ready()) continue;
        const float length = slot.length();
        LeafRun leaf;
        leaf.slot = leaves[i].clip;
        leaf.weight = leaves[i].weight;
        leaf.loops = stateLoops(run.state, leaves[i].clip);
        leaf.pingPong = leaf.loops && statePingPong(run.state, leaves[i].clip);
        leaf.from = (run.previous + offset) * length;
        leaf.to = (run.time + offset) * length;
        leaf.includeStart = run.entered;
        leaf.state = run.state;
        leaf.next = next;
        // the events at the start of a state entered without leading its layer (a transition's
        // destination) fire once it leads (upstream's rule; the rest of its time fires as usual)
        if (run.startPending && !run.entered) leaf.pendingStart = (run.start + offset) * length;
        if (leafRuns.size() < leafRuns.capacity()) leafRuns.push_back(leaf);
        if (leaderOnly && leaf.weight > leaderWeight){
            leaderWeight = leaf.weight;
            leaderIndex = (int)leafRuns.size() - 1;
        }
    }
}

void anim::AnimatorInstance::collectLayer(size_t layer, const AnimatorComponent& component){
    AnimatorLayerRun& run = layers[layer];
    run.rootDelta = Vector3(0, 0, 0);
    run.rootYaw = 0.0f;
    run.rootWeight = 0.0f;
    leafRuns.clear();
    const float progress = run.transition != -1 && run.duration > 0.0f ? std::min(1.0f, run.elapsed / run.duration) : 0.0f;
    const bool leaderOnly = component.eventMode == AnimatorEventMode::LeaderOnly;
    float leaderWeight = 0.0f;
    int leader = -1;
    // a state a 0 s transition left this update: its step's clip events (no pose, no root motion)
    if (run.instantSource.state >= 0){
        collectLeaves(run.instantSource, false, 1.0f, leaderOnly, leaderWeight, leader);
        for (size_t i = 0; i < leafRuns.size(); i++){
            if (leaderOnly && (int)i != leader) continue;
            const LeafRun& leaf = leafRuns[i];
            const ClipWrapMode wrap = leaf.loops ? (leaf.pingPong ? ClipWrapMode::PingPong : ClipWrapMode::Loop) : ClipWrapMode::Once;
            fireEvents(leaf.slot, wrap, leaf.from, leaf.to, leaf.includeStart, leaf.state, (int)layer);
        }
        leafRuns.clear();
        leaderWeight = 0.0f;
        leader = -1;
        run.instantSource = AnimatorStateRun();
    }
    if (!run.snapshot) collectLeaves(run.current, false, run.transition != -1 ? 1.0f - progress : 1.0f, leaderOnly, leaderWeight, leader);
    if (run.transition != -1) collectLeaves(run.next, true, progress, leaderOnly, leaderWeight, leader);
    const bool additive = controller->layers[layer].blending == AnimatorLayerBlending::Additive && layer > 0;
    for (size_t i = 0; i < leafRuns.size(); i++){
        const LeafRun& leaf = leafRuns[i];
        if (!leaderOnly || (int)i == leader){
            const ClipWrapMode wrap = leaf.loops ? (leaf.pingPong ? ClipWrapMode::PingPong : ClipWrapMode::Loop) : ClipWrapMode::Once;
            if (leaf.pendingStart >= 0.0) fireEvents(leaf.slot, wrap, leaf.pendingStart, leaf.pendingStart + 1e-6, true, leaf.state, (int)layer);
            fireEvents(leaf.slot, wrap, leaf.from, leaf.to, leaf.includeStart, leaf.state, (int)layer);
            // its start went out (a step that did not move keeps it for the next one)
            if (leaf.to != leaf.from) (leaf.next ? run.next : run.current).startPending = false;
        }
        // a layer of weight 0 moves nothing: its root is not sampled
        if (component.applyRootMotion && !additive && (layer == 0 || run.weight > 0.0f)){
            const AnimatorClipSlot& slot = clips[(size_t)leaf.slot];
            if (slot.rootBone >= 0){
                Vector3 delta;
                float yaw = 0.0f;
                rootDelta(slot, leaf.from, leaf.to, leaf.loops, leaf.pingPong, delta, yaw);
                run.rootDelta = run.rootDelta + delta * leaf.weight;
                run.rootYaw += yaw * leaf.weight;
                run.rootWeight += leaf.weight;
            }
        }
    }
}

void anim::AnimatorInstance::fireEvents(int slot, ClipWrapMode wrap, double from, double to, bool includeStart, int state, int layer){
    const AnimatorClipSlot& clipSlot = clips[(size_t)slot];
    if (!clipSlot.clip || from == to) return;
    if (!clipSlot.clip->getEvents().empty()){
        crossed.clear();
        collectCrossedEvents(*clipSlot.clip, wrap, (float)from, (float)to, includeStart, crossed);
        for (const CrossedEvent& event : crossed){
            AnimatorPendingEvent pending;
            pending.clip = slot;
            pending.index = (int)event.index;
            pending.state = state;
            pending.layer = layer;
            pending.position = event.position;
            output.events.push_back(pending);
        }
    }
    // audio cues by the same crossing (S40 R4), placed by where in the step they were crossed
    if (!clipSlot.clip->getAudioCues().empty()){
        crossed.clear();
        collectCrossedCues(*clipSlot.clip, wrap, (float)from, (float)to, includeStart, crossed);
        for (const CrossedEvent& cue : crossed){
            AnimatorPendingCue pending;
            pending.clip = slot;
            pending.index = (int)cue.index;
            pending.state = state;
            pending.layer = layer;
            pending.position = cue.position;
            pending.fraction = (float)std::min(1.0, std::max(0.0, (cue.position - from) / (to - from)));
            pending.speed = state >= 0 ? stateSpeed(state) : 1.0f;
            output.cues.push_back(pending);
        }
    }
}

bool anim::AnimatorInstance::rootSample(const AnimatorClipSlot& slot, float clipTime, Vector3& position, Quaternion& rotation) const{
    if (slot.rootBone < 0 || !slot.clip) return false;
    Vector3 p = rig.rest.position((size_t)slot.rootBone);
    Quaternion q = rig.rest.rotation((size_t)slot.rootBone);
    const std::vector<AnimationClip::Track>& tracks = slot.clip->getTracks();
    TrackSample sample;
    if (slot.retargeted){
        // the hips of the realised clip (no Mirror, no Foot IK: the body's motion)
        HumanoidSample sample;
        slot.retarget.sample(*slot.clip, clipTime, nullptr, false, sample);
        pose::HumanPose realised;
        std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extraLocal;
        realiseHumanoid(*humanoid.avatar, sample, false, false, realised, extraLocal);
        const size_t hips = (size_t)pose::HumanBone::Hips;
        q = (humanoid.slotBridgeInverse[hips] * realised.rotations[hips]).normalized();
        if (sample.hasHips) p = humanoid.hipsBridgeInverse * realised.hipsPosition;
    }else{
        if (slot.rootPositionTrack >= 0 && sampleTrack(tracks[(size_t)slot.rootPositionTrack], clipTime, sample)){
            p = tracks[(size_t)slot.rootPositionTrack].relative ? rig.rest.position((size_t)slot.rootBone) + sample.vector : sample.vector;
        }
        if (slot.rootRotationTrack >= 0 && sampleTrack(tracks[(size_t)slot.rootRotationTrack], clipTime, sample)){
            q = tracks[(size_t)slot.rootRotationTrack].relative ? rig.rest.rotation((size_t)slot.rootBone) * sample.rotation : sample.rotation;
        }
    }
    position = slot.rootParent.transformPoint(p);
    rotation = slot.rootParentRotation * q;
    return true;
}

void anim::AnimatorInstance::rootPoint(const AnimatorClipSlot& slot, float clipTime, Vector3& p, float& h) const{
    const ClipRootMotion& rm = slot.rootMotion;
    Quaternion q;
    rootSample(slot, clipTime, p, q);
    h = rm.bakeRotation ? 0.0f : wrapAngle(yawOf(q) - slot.rootStartYaw);
    if (rm.bakePositionY) p.y = slot.rootStart.y;
    if (rm.bakePositionXZ){
        p.x = slot.rootStart.x;
        p.z = slot.rootStart.z;
    }
}

void anim::AnimatorInstance::rootDelta(const AnimatorClipSlot& slot, double from, double to, bool loops, bool pingPong,
                                       Vector3& delta, float& yaw) const{
    delta = Vector3(0, 0, 0);
    yaw = 0.0f;
    const float length = slot.length();
    if (!(length > 0.0f) || from == to) return;
    auto sampleAt = [&](float t, Vector3& p, float& h){ rootPoint(slot, t, p, h); };
    Vector3 p0, p1;
    float h0 = 0.0f, h1 = 0.0f;
    if (!loops || pingPong){
        float a, b;
        if (!loops){
            a = (float)std::min(std::max(from, 0.0), (double)length);
            b = (float)std::min(std::max(to, 0.0), (double)length);
        }else{
            a = clipTimeAt(from / length, length, true, true);
            b = clipTimeAt(to / length, length, true, true);
        }
        sampleAt(a, p0, h0);
        sampleAt(b, p1, h1);
        delta = rotateYaw(p1 - p0, -h0);
        yaw = h1 - h0;
        return;
    }
    double k0 = std::floor(from / length);
    double k1 = std::floor(to / length);
    const float t0 = (float)(from - k0 * length);
    const float t1 = (float)(to - k1 * length);
    double laps = std::min(std::max(k1 - k0, -kMaxLaps), kMaxLaps);
    const int n = (int)laps;
    // the lap, sampled at bind
    const Vector3 start = slot.lapStart;
    const Vector3 lap = slot.lapEnd - slot.lapStart;
    const float turn = slot.lapEndHeading;
    Vector3 sum(0, 0, 0);
    if (n > 0){
        for (int m = 0; m < n; m++) sum = sum + rotateYaw(lap, (float)m * turn);
    }else if (n < 0){
        for (int m = n; m < 0; m++) sum = sum - rotateYaw(lap, (float)m * turn);
    }
    sampleAt(t0, p0, h0);
    sampleAt(t1, p1, h1);
    const Vector3 inner = sum + rotateYaw(p1 - start, (float)n * turn) - (p0 - start);
    delta = rotateYaw(inner, -h0);
    yaw = (float)n * turn + h1 - h0;
}

void anim::AnimatorInstance::removeRootMotion(const AnimatorClipSlot& slot, Pose& out) const{
    if (slot.rootBone < 0) return;
    const ClipRootMotion& rm = slot.rootMotion;
    const size_t b = (size_t)slot.rootBone;
    Vector3 p = slot.rootParent.transformPoint(out.position(b));
    Quaternion q = slot.rootParentRotation * out.rotation(b);
    if (!rm.bakeRotation){
        const float h = wrapAngle(yawOf(q) - slot.rootStartYaw);
        const Quaternion unturn = yawRotation(-h);
        q = unturn * q;
        p = slot.rootStart + rotateYaw(p - slot.rootStart, -h);
    }
    if (!rm.bakePositionXZ){
        p.x = slot.rootStart.x;
        p.z = slot.rootStart.z;
    }
    if (!rm.bakePositionY) p.y = slot.rootStart.y;
    out.setPosition(b, slot.rootParentInverse.transformPoint(p));
    out.setRotation(b, (slot.rootParentRotation.inverse() * q).normalized());
}

// ---------------------------------------------------------------- montages

void anim::AnimatorInstance::advanceMontages(float dt, const AnimatorComponent& component){
    for (size_t m = 0; m < montages.size(); m++){
        AnimatorMontageRun& run = montages[m];
        run.rootDelta = Vector3(0, 0, 0);
        run.rootYaw = 0.0f;
        run.startedThisUpdate = false;
        if (run.phase == AnimatorMontageRun::Phase::Ended) continue;
        const AnimatorClipSlot* slot = run.clip >= 0 && (size_t)run.clip < clips.size() ? &clips[(size_t)run.clip] : nullptr;
        if (run.phase == AnimatorMontageRun::Phase::Waiting){
            if (!slot || !slot->ready()) continue;
            run.phase = AnimatorMontageRun::Phase::BlendIn;
            run.time = run.previous = run.montage->sections[(size_t)run.section].start;
            run.blendElapsed = 0.0f;
            run.blendFrom = 0.0f;
            run.startedThisUpdate = true;
        }
        if (!slot || !slot->ready()) continue;
        const MontageDef& def = *run.montage;
        const float length = slot->length();

        // blend weight
        run.blendElapsed += std::fabs(dt);
        const float f = run.blendDuration > 0.0f ? std::min(1.0f, run.blendElapsed / run.blendDuration) : 1.0f;
        const float eased = applyEase(run.ease, f);
        if (run.phase == AnimatorMontageRun::Phase::BlendIn){
            run.weight = run.blendFrom + (1.0f - run.blendFrom) * eased;
            if (f >= 1.0f) run.phase = AnimatorMontageRun::Phase::Playing;
        }else if (run.phase == AnimatorMontageRun::Phase::BlendOut){
            run.weight = run.blendFrom * (1.0f - eased);
            if (f >= 1.0f){
                run.phase = AnimatorMontageRun::Phase::Ended;
                run.weight = 0.0f;
                output.montages.push_back({(int)m, run.interrupted});
                continue;
            }
        }else{
            run.weight = 1.0f;
        }

        // time, sections, events
        const float step = dt * run.rate;
        float time = run.time;
        float remaining = step;
        int guard = 0;
        const bool rootMotion = component.applyRootMotion && slot->rootBone >= 0;
        auto segment = [&](float a, float b, bool start){
            // events of the clip and of the montage crossed between a and b (forward or backward)
            if ((a != b || start) && !slot->clip->getAudioCues().empty()){
                // its audio cues (S40 R4), placed by the step travelled before the crossing
                const float travelled = std::fabs(step - remaining);
                crossed.clear();
                collectCrossedCues(*slot->clip, ClipWrapMode::Once, a, b, start, crossed);
                for (const CrossedEvent& cue : crossed){
                    AnimatorPendingCue pending;
                    pending.clip = run.clip;
                    pending.index = (int)cue.index;
                    pending.montage = (int)m;
                    pending.layer = run.layer;
                    pending.position = cue.position;
                    pending.fraction = step != 0.0f ? std::min(1.0f, (travelled + (float)std::fabs(cue.position - a)) / std::fabs(step)) : 0.0f;
                    pending.speed = run.rate;
                    output.cues.push_back(pending);
                }
            }
            if (a != b || start){
                crossed.clear();
                collectCrossedEvents(*slot->clip, ClipWrapMode::Once, a, b, start, crossed);
                for (const CrossedEvent& event : crossed){
                    AnimatorPendingEvent pending;
                    pending.clip = run.clip;
                    pending.index = (int)event.index;
                    pending.montage = (int)m;
                    pending.layer = run.layer;
                    pending.position = 1e9 + event.position;
                    output.events.push_back(pending);
                }
                for (size_t e = 0; e < def.events.size(); e++){
                    const float at = def.events[e].time;
                    const bool hit = b >= a ? ((at > a && at <= b) || (start && at == a)) : ((at < a && at >= b) || (start && at == a));
                    if (!hit) continue;
                    AnimatorPendingEvent pending;
                    pending.montage = (int)m;
                    pending.montageEvent = (int)e;
                    pending.layer = run.layer;
                    pending.position = 1e9 + at;
                    output.events.push_back(pending);
                }
            }
            if (rootMotion && a != b){
                Vector3 delta;
                float yaw = 0.0f;
                rootDelta(*slot, a, b, false, false, delta, yaw);
                run.rootDelta = run.rootDelta + rotateYaw(delta, run.rootYaw);
                run.rootYaw += yaw;
            }
        };
        bool first = run.startedThisUpdate;
        if (first) segment(time, time, true);
        first = false;
        while (guard++ < 64){
            const int section = run.section;
            const float sectionStart = def.sections[(size_t)section].start;
            const float sectionEnd = (size_t)section + 1 < def.sections.size() ? std::min(def.sections[(size_t)section + 1].start, length) : length;
            if (remaining >= 0.0f){
                if (time + remaining < sectionEnd || (run.phase == AnimatorMontageRun::Phase::BlendOut && time + remaining <= length)){
                    const float target = std::min(time + remaining, length);
                    segment(time, target, false);
                    time = target;
                    break;
                }
                // reached the section's end
                segment(time, sectionEnd, false);
                remaining -= sectionEnd - time;
                time = sectionEnd;
                const std::string& next = run.hasNextOverride[(size_t)section] ? run.nextOverride[(size_t)section] : def.sections[(size_t)section].next;
                const int nextIndex = next.empty() ? -1 : def.findSection(next);
                if (nextIndex < 0){
                    // the end: blend out, holding the last frame
                    if (run.phase != AnimatorMontageRun::Phase::BlendOut){
                        run.phase = AnimatorMontageRun::Phase::BlendOut;
                        run.interrupted = false;
                        run.blendFrom = run.weight;
                        run.blendElapsed = 0.0f;
                        run.blendDuration = def.blendOut.time;
                        EaseType ease = EaseType::LINEAR;
                        easeFromName(def.blendOut.ease, ease);
                        run.ease = ease;
                        if (run.blendDuration <= 0.0f){
                            run.phase = AnimatorMontageRun::Phase::Ended;
                            run.weight = 0.0f;
                            output.montages.push_back({(int)m, false});
                        }
                    }
                    break;
                }
                run.section = nextIndex;
                time = def.sections[(size_t)nextIndex].start;
                segment(time, time, true);
            }else{
                // backwards: within the section, ending at its start
                if (time + remaining > sectionStart){
                    segment(time, time + remaining, false);
                    time += remaining;
                    break;
                }
                segment(time, sectionStart, false);
                time = sectionStart;
                if (run.phase != AnimatorMontageRun::Phase::BlendOut){
                    run.phase = AnimatorMontageRun::Phase::BlendOut;
                    run.interrupted = false;
                    run.blendFrom = run.weight;
                    run.blendElapsed = 0.0f;
                    run.blendDuration = def.blendOut.time;
                }
                break;
            }
        }
        run.previous = run.time;
        run.time = time;
        // the automatic blend-out starts blendOut.time before the end of the last section
        if (run.phase == AnimatorMontageRun::Phase::Playing || run.phase == AnimatorMontageRun::Phase::BlendIn){
            const std::string& next = run.hasNextOverride[(size_t)run.section] ? run.nextOverride[(size_t)run.section] : def.sections[(size_t)run.section].next;
            const bool last = next.empty() || def.findSection(next) < 0;
            const float sectionEnd = (size_t)run.section + 1 < def.sections.size() ? std::min(def.sections[(size_t)run.section + 1].start, length) : length;
            if (last && run.rate > 0.0f && sectionEnd - run.time <= def.blendOut.time && def.blendOut.time > 0.0f){
                run.phase = AnimatorMontageRun::Phase::BlendOut;
                run.interrupted = false;
                run.blendFrom = run.weight;
                run.blendElapsed = 0.0f;
                run.blendDuration = std::max(0.0f, (sectionEnd - run.time) / std::fabs(run.rate));
                EaseType ease = EaseType::LINEAR;
                easeFromName(def.blendOut.ease, ease);
                run.ease = ease;
            }
        }
    }
}

void anim::AnimatorInstance::applyMontages(size_t layer, Pose& out, const AnimatorComponent& component){
    const bool additive = controller->layers[layer].blending == AnimatorLayerBlending::Additive && layer > 0;
    AnimatorLayerRun& run = layers[layer];
    for (AnimatorMontageRun& montage : montages){
        if (montage.layer != (int)layer || !(montage.weight > 0.0f) || montage.phase == AnimatorMontageRun::Phase::Ended) continue;
        if (montage.clip < 0 || !clips[(size_t)montage.clip].ready()) continue;
        sampleSlot((size_t)montage.clip, montage.time, montagePose, additive, AdditiveReference::FirstFrame, 0.0f, component);
        poseBlend(out, out, montagePose, montage.weight, nullptr, nullptr);
        if (component.applyRootMotion && !additive){
            const float w = montage.weight;
            run.rootDelta = run.rootDelta * (1.0f - w) + montage.rootDelta * w;
            run.rootYaw = run.rootYaw * (1.0f - w) + montage.rootYaw * w;
            run.rootWeight = run.rootWeight * (1.0f - w) + w;
        }
    }
}

// ---------------------------------------------------------------- evaluation

void anim::AnimatorInstance::sampleHumanoid(AnimatorClipSlot& slot, float clipTime, Pose& out, bool mirror, bool footIK, KeyCursor* cursors,
                                            float stepFps) const{
    // the one retarget path: the clip as a humanoid sample, realised on the avatar, written to
    // the rig's bones (the generic tracks of the clip play by path before)
    HumanoidSample sample;
    slot.retarget.sample(*slot.clip, clipTime, cursors, footIK, sample, stepFps);
    pose::HumanPose realised;
    std::array<Quaternion, pose::HUMAN_EXTRA_BONE_COUNT> extraLocal;
    std::bitset<pose::HUMAN_EXTRA_BONE_COUNT> extraLive;
    realiseHumanoid(*humanoid.avatar, sample, mirror, footIK, realised, extraLocal, &extraLive);
    writeRealised(humanoid, realised, extraLocal, extraLive, sample.hasHips, slot.retarget.slots.any(), out);
}

void anim::AnimatorInstance::sampleSlot(size_t index, float clipTime, Pose& out, bool additive, AdditiveReference reference,
                                        float referenceTime, const AnimatorComponent& component, bool mirror, bool footIK){
    AnimatorClipSlot& slot = clips[index];
    out.copyFrom(rig.rest);
    // S48 Pose Frame Rate: held poses, the root bone's translation smooth
    sampleClip(slot.binding, rig, clipTime, slot.cursors, out, component.poseFrameRate, slot.rootBone);
    if (slot.retargeted) sampleHumanoid(slot, clipTime, out, mirror, footIK, slot.retargetCursors.data(), component.poseFrameRate);
    if (component.applyRootMotion) removeRootMotion(slot, out);
    if (!additive) return;
    const uint8_t flags = (uint8_t)((mirror ? 1 : 0) | (footIK ? 2 : 0));
    if (!slot.hasReference || slot.referenceKind != reference || (reference == AdditiveReference::Time && slot.referenceTime != referenceTime) ||
            (slot.retargeted && slot.referenceFlags != flags)){
        slot.reference.copyFrom(rig.rest);
        if (reference != AdditiveReference::Rest){
            // Clip: the .anim's own additive_reference (upstream's rule; 0 = the first frame)
            const float at = reference == AdditiveReference::Time ? std::min(std::max(referenceTime, 0.0f), slot.length())
                           : reference == AdditiveReference::Clip ? std::min(std::max(slot.clip->getAdditiveReference(), 0.0f), slot.length())
                           : 0.0f;
            sampleClip(slot.binding, rig, at, slot.cursors, slot.reference);
            if (slot.retargeted) sampleHumanoid(slot, at, slot.reference, mirror, footIK, nullptr);
            if (component.applyRootMotion) removeRootMotion(slot, slot.reference);
        }
        slot.hasReference = true;
        slot.referenceKind = reference;
        slot.referenceTime = referenceTime;
        slot.referenceFlags = flags;
    }
    poseMakeDelta(out, out, slot.reference);
}

void anim::AnimatorInstance::evaluateMotion(size_t layer, const AnimatorStateRun& run, Pose& out, const AnimatorComponent& component){
    const bool additive = controller->layers[layer].blending == AnimatorLayerBlending::Additive && layer > 0;
    if (run.state < 0){
        if (additive) poseIdentityDelta(out);
        else out.copyFrom(rig.rest);
        return;
    }
    const CompiledState& s = controller->states[(size_t)run.state];
    const size_t count = motionLeaves(*controller, s.motion, parameters.data(), 1.0f, leaves.data(), 0, leaves.size());
    float offset = s.cycleOffset;
    if (s.cycleOffsetParameter >= 0) offset = parameters[(size_t)s.cycleOffsetParameter];
    const double normalized = run.time + offset;
    size_t ready = 0;
    for (size_t i = 0; i < count; i++) ready += clips[(size_t)leaves[i].clip].ready() ? 1 : 0;
    if (ready == 0){
        if (additive) poseIdentityDelta(out);
        else out.copyFrom(rig.rest);
        return;
    }
    if (count == 1){
        const AnimatorClipSlot& slot = clips[(size_t)leaves[0].clip];
        const bool loops = stateLoops(run.state, leaves[0].clip);
        const float t = clipTimeAt(normalized, slot.length(), loops, loops && statePingPong(run.state, leaves[0].clip));
        sampleSlot((size_t)leaves[0].clip, t, out, additive, s.additiveReference, s.additiveTime, component, s.mirror != leaves[0].mirror, s.footIK);
        return;
    }
    poseClear(out);
    float total = 0.0f;
    for (size_t i = 0; i < count; i++){
        const AnimatorClipSlot& slot = clips[(size_t)leaves[i].clip];
        if (!slot.ready()) continue;
        const bool loops = stateLoops(run.state, leaves[i].clip);
        const float t = clipTimeAt(normalized, slot.length(), loops, loops && statePingPong(run.state, leaves[i].clip));
        sampleSlot((size_t)leaves[i].clip, t, temp, additive, s.additiveReference, s.additiveTime, component, s.mirror != leaves[i].mirror,
                   s.footIK);
        poseAccumulate(out, temp, leaves[i].weight);
        total += leaves[i].weight;
    }
    if (additive){
        // a zero rotation falls back to the identity delta
        poseIdentityDelta(temp);
        poseNormalize(out, total, temp);
    }else{
        poseNormalize(out, total, rig.rest);
    }
}

// A layer of weight 0 adds nothing to the final pose (S77 R14), but a transition or a snapshot may be
// interrupted, and a montage's pose is frozen with the layer's: those keep `output` current. The
// weight is final here (fades moved it at the start of this update); a change after this update
// reaches the next one, which evaluates the layer before it uses it.
bool anim::AnimatorInstance::layerNeedsPose(size_t layer) const{
    const AnimatorLayerRun& run = layers[layer];
    if (layer == 0 || run.weight > 0.0f || run.transition != -1 || run.snapshot) return true;
    for (const AnimatorMontageRun& montage : montages){
        if (montage.layer == (int)layer && montage.weight > 0.0f && montage.phase != AnimatorMontageRun::Phase::Ended) return true;
    }
    return false;
}

void anim::AnimatorInstance::evaluateLayer(size_t layer, const AnimatorComponent& component){
    AnimatorLayerRun& run = layers[layer];
    if (run.snapshotPending){
        run.snapshotPending = false;
        if (run.snapshot) evaluateMotion(layer, run.skippedState, run.snapshotPose, component);
    }
    run.outputSkipped = false;
    if (run.transition != -1){
        const Pose* from = &run.snapshotPose;
        if (!run.snapshot){
            evaluateMotion(layer, run.current, source, component);
            from = &source;
        }
        evaluateMotion(layer, run.next, destination, component);
        // S48 Pose Frame Rate: the blend's weight steps on the same ticks as the poses (events
        // and root motion keep the true progress)
        const float elapsed = holdStepTime(run.elapsed, component.poseFrameRate);
        const float progress = run.duration > 0.0f ? std::min(1.0f, elapsed / run.duration) : 1.0f;
        poseBlend(run.output, *from, destination, progress, nullptr, nullptr);
    }else if (run.snapshot){
        run.output.copyFrom(run.snapshotPose);
    }else{
        evaluateMotion(layer, run.current, run.output, component);
    }
    applyMontages(layer, run.output, component);
    run.hasOutput = true;
}

void anim::AnimatorInstance::update(const AnimatorComponent& component, const AnimatorUpdateContext& context){
    // state and montage notes made by calls since the last update stay until AnimatorSystem
    // dispatches them; events and cues are this update's
    output.events.clear();
    output.cues.clear();
    output.hasRootMotion = false;
    output.rootDeltaPosition = Vector3(0, 0, 0);
    output.rootDeltaYaw = 0.0f;
    output.deltaTime = std::fabs(context.dt);
    output.sampled = false;
    if (!controller || !bound) return;
    evaluations++;
    const float dt = context.dt * (std::isfinite(component.speed) ? component.speed : 0.0f);
    clock += std::fabs(context.dt);

    // damped targets (setFloatTarget): one SmoothDamp step per update, in the Animator's time
    for (size_t i = 0; i < dampTimes.size() && i < parameters.size(); i++){
        if (dampTimes[i] > 0.0f && dt != 0.0f){
            parameters[i] = smoothDamp(parameters[i], dampTargets[i], velocities[i], dampTimes[i], std::fabs(dt));
        }
    }

    for (size_t l = 0; l < layers.size(); l++){
        // a layer weight fade (setLayerWeight with a fade)
        AnimatorLayerRun& run = layers[l];
        if (run.weightFadeSpeed > 0.0f){
            const float step = run.weightFadeSpeed * std::fabs(dt);
            if (run.weight < run.targetWeight) run.weight = std::min(run.targetWeight, run.weight + step);
            else run.weight = std::max(run.targetWeight, run.weight - step);
            if (run.weight == run.targetWeight) run.weightFadeSpeed = 0.0f;
        }
        advanceLayer(l, dt);
    }
    // the triggers this update's transitions consumed
    for (size_t i = 0; i < consumedTriggers.size() && i < parameters.size(); i++){
        if (consumedTriggers[i]){
            parameters[i] = 0.0f;
            consumedTriggers[i] = 0;
        }
    }
    advanceMontages(dt, component);
    for (size_t l = 0; l < layers.size(); l++) collectLayer(l, component);

    if (context.sample){
        for (size_t l = 0; l < layers.size(); l++){
            if (layerNeedsPose(l)){
                evaluateLayer(l, component);
            }else{
                layers[l].outputSkipped = true;
                layers[l].skippedState = layers[l].current;
            }
        }
        // layers combined (D3)
        AnimatorLayerRun& base = layers[0];
        if (base.masked) poseBlend(finalPose, rig.rest, base.output, 1.0f, base.mask.data(), base.channelMask.data());
        else finalPose.copyFrom(base.output);
        for (size_t l = 1; l < layers.size(); l++){
            AnimatorLayerRun& run = layers[l];
            if (!(run.weight > 0.0f) || !run.hasOutput) continue;
            const float* mask = run.masked ? run.mask.data() : nullptr;
            const float* channelMask = run.masked ? run.channelMask.data() : nullptr;
            if (controller->layers[l].blending == AnimatorLayerBlending::Additive){
                poseAdditive(finalPose, finalPose, run.output, run.weight, mask, channelMask);
            }else{
                poseBlend(finalPose, finalPose, run.output, run.weight, mask, channelMask);
            }
        }
        runIK(component, context);
        output.sampled = true;
    }else{
        // culled: the montages' root motion still counts
        for (size_t l = 0; l < layers.size(); l++){
            for (AnimatorMontageRun& montage : montages){
                if (montage.layer != (int)l || !(montage.weight > 0.0f) || !component.applyRootMotion) continue;
                AnimatorLayerRun& run = layers[l];
                const float w = montage.weight;
                run.rootDelta = run.rootDelta * (1.0f - w) + montage.rootDelta * w;
                run.rootYaw = run.rootYaw * (1.0f - w) + montage.rootYaw * w;
                run.rootWeight = run.rootWeight * (1.0f - w) + w;
            }
        }
    }

    // root motion: the base layer's, override layers lerp over it by weight and Root mask
    if (component.applyRootMotion){
        Vector3 delta(0, 0, 0);
        float yaw = 0.0f;
        bool any = false;
        for (size_t l = 0; l < layers.size(); l++){
            const AnimatorLayerRun& run = layers[l];
            if (!(run.rootWeight > 0.0f)) continue;
            if (l > 0 && controller->layers[l].blending == AnimatorLayerBlending::Additive) continue;
            const Vector3 layerDelta = run.rootDelta * (1.0f / run.rootWeight);
            const float layerYaw = run.rootYaw / run.rootWeight;
            const float w = l == 0 ? 1.0f : run.weight * run.rootMask * std::min(1.0f, run.rootWeight);
            delta = delta * (1.0f - w) + layerDelta * w;
            yaw = yaw * (1.0f - w) + layerYaw * w;
            any = true;
        }
        output.hasRootMotion = any;
        output.rootDeltaPosition = delta;
        output.rootDeltaYaw = yaw;
        if (any){
            rootMotionTotal = rootMotionTotal + rotateYaw(delta, rootMotionTotalYaw);
            rootMotionTotalYaw += yaw;
        }
    }
}

void anim::AnimatorInstance::runIK(const AnimatorComponent& component, const AnimatorUpdateContext& context){
    RigSpace space{spaceWorld.data(), spacePosition.data(), spaceRotation.data(), spaceScale.data()};
    poseToRigSpace(finalPose, rig.parents.data(), space);
    clearIKResults();
    if (component.ik.empty() || ik.size() != component.ik.size()) return;
    IKContext ctx{&finalPose, rig.parents.data(), space, &context.rootWorld};
    const float dt = std::fabs(context.dt * component.speed);
    const float frameTime = std::fabs(context.dt);     // the debug velocities are per real second
    const Quaternion rootRotationInverse = context.rootWorldRotation.inverse();
    auto toRig = [&](const Vector3& world){ return context.rootWorldInverse * world; };
    auto toWorld = [&](const Vector3& rigPosition){ return context.rootWorld * rigPosition; };
    auto record = [](AnimatorIKRun& run, const IKResult& result, float weight, const Vector3& targetWorld){
        run.reached = result.reached;
        run.reachError = result.error;
        run.solvedWeight = weight;
        run.solvedTargetWorld = targetWorld;
    };
    auto legOptions = [](const AnimatorIKConstraint& c){
        TwoBoneOptions options;
        options.soften = c.soften;
        options.hingeAxis = c.hingeAxis;
        options.twist = c.twist;
        return options;
    };
    auto weightOf = [&](size_t i){
        const AnimatorIKConstraint& c = component.ik[i];
        float w = c.enabled ? c.weight : 0.0f;
        if (ik[i].scriptWeight >= 0.0f) w = ik[i].scriptWeight;
        if (!c.weightParameter.empty()){
            const int p = findParameter(c.weightParameter);
            w *= p >= 0 ? parameters[(size_t)p] : 0.0f;
        }
        return std::min(std::max(w, 0.0f), 1.0f);
    };
    bool footDone = false;
    for (size_t i = 0; i < component.ik.size(); i++){
        const AnimatorIKConstraint& c = component.ik[i];
        AnimatorIKRun& run = ik[i];
        const float weight = weightOf(i);
        const bool scripted = run.scriptTarget;
        const Vector3 targetWorld = scripted ? run.scriptTargetWorld : run.targetWorld;
        const bool hasTarget = scripted || run.hasTarget;
        switch (c.type){
            case AnimatorIKType::TwoBone: {
                if (!hasTarget || run.boneCount < 3) break;
                const Vector3 pole = toRig(run.poleWorld);
                Quaternion endRotation;
                if (c.useTargetRotation && run.hasTargetRotation) endRotation = rootRotationInverse * run.targetRotationWorld;
                record(run, solveTwoBone(ctx, run.bones[0], run.bones[1], run.bones[2], toRig(targetWorld), run.hasPole ? &pole : nullptr,
                                         c.useTargetRotation && run.hasTargetRotation ? &endRotation : nullptr, weight, legOptions(c)),
                       weight, targetWorld);
                break;
            }
            case AnimatorIKType::Fabrik:
                if (!hasTarget || run.boneCount < 2) break;
                record(run, solveFabrik(ctx, run.bones, run.boneCount, toRig(targetWorld), c.iterations, c.tolerance, weight), weight, targetWorld);
                break;
            case AnimatorIKType::LookAt:
                if (!hasTarget || run.boneCount < 1) break;
                record(run, solveLookAt(ctx, run.bones, run.boneWeights, std::min(run.boneCount, 4), c.forwardAxis, toRig(targetWorld),
                                        c.clampAngle, weight, c.aimOffset), weight, targetWorld);
                break;
            case AnimatorIKType::FootIK: {
                if (footDone) break;
                footDone = true;
                // every FootIK of the stack together: the pelvis drops for the lowest foot
                float pelvisOffset = 0.0f;
                int pelvis = -1;
                for (size_t j = i; j < component.ik.size(); j++){
                    const AnimatorIKConstraint& f = component.ik[j];
                    AnimatorIKRun& foot = ik[j];
                    if (f.type != AnimatorIKType::FootIK || foot.boneCount < 3 || foot.bones[2] < 0) continue;
                    const Vector3 footPosition = spacePosition[(size_t)foot.bones[2]];
                    const Vector3 footWorld = toWorld(footPosition);
                    foot.footVelocity = foot.hasFoot && frameTime > 0.0f ? (footWorld - foot.footWorld) * (1.0f / frameTime) : Vector3(0, 0, 0);
                    foot.footWorld = footWorld;
                    foot.hasFoot = true;
                    foot.contact = foot.hit && footPosition.y - f.footHeight <= kFootContactLift;
                    float desired = 0.0f;
                    if (foot.hit){
                        const float lift = std::max(0.0f, footPosition.y - f.footHeight);
                        desired = foot.hitPoint.y + footAnkleHeight(f.footHeight, foot.hitNormal, f.maxFootAngle) + lift - footPosition.y;
                    }
                    foot.offset = springDamp(foot.offset, desired, foot.offsetVelocity, f.smoothing, dt);
                    const float w = weightOf(j);
                    pelvisOffset = std::min(pelvisOffset, foot.offset * w);
                    if (pelvis < 0) pelvis = foot.pelvis;
                }
                if (pelvis >= 0 && pelvisOffset < 0.0f) translateBoneInRigSpace(ctx, pelvis, Vector3(0, pelvisOffset, 0));
                for (size_t j = i; j < component.ik.size(); j++){
                    const AnimatorIKConstraint& f = component.ik[j];
                    AnimatorIKRun& foot = ik[j];
                    if (f.type != AnimatorIKType::FootIK || foot.boneCount < 3 || foot.bones[2] < 0) continue;
                    const float w = weightOf(j);
                    if (!(w > 0.0f)) continue;
                    const Vector3 animated = context.rootWorldInverse * foot.footWorld;
                    const Vector3 now = spacePosition[(size_t)foot.bones[2]];
                    const Vector3 target(now.x, animated.y + foot.offset, now.z);
                    // the foot keeps its animated rotation, turned to the ground: not what bending
                    // the knee would make of it through the shin
                    Quaternion footRotation = spaceRotation[(size_t)foot.bones[2]];
                    if (foot.hit){
                        const Quaternion align = shortestArc(Vector3(0, 1, 0), foot.hitNormal);
                        const float angle = 2.0f * std::acos(std::min(1.0f, std::fabs(align.w)));
                        const float maxAngle = f.maxFootAngle * 0.017453292519943295f;
                        footRotation = (angle > maxAngle && angle > 1e-6f ? Quaternion::slerp(maxAngle / angle, Quaternion(), align) : align) * footRotation;
                    }
                    IKResult result = solveTwoBone(ctx, foot.bones[0], foot.bones[1], foot.bones[2], target, nullptr, &footRotation, w, legOptions(f));
                    result.reached = result.reached && foot.hit;    // no ground: nothing to reach
                    record(foot, result, w, toWorld(target));
                }
                break;
            }
            case AnimatorIKType::FootLock: {
                if (run.boneCount < 3 || run.bones[2] < 0) break;
                const Vector3 world = toWorld(spacePosition[(size_t)run.bones[2]]);
                bool contact;
                if (!c.contactParameter.empty()){
                    const int p = findParameter(c.contactParameter);
                    contact = p >= 0 && parameters[(size_t)p] >= 0.5f;
                }else{
                    const float speed = run.hadPrevious && dt > 0.0f ? (world - run.previousWorld).length() / dt : 1e9f;
                    contact = speed < c.speedThreshold;
                }
                run.footVelocity = run.hadPrevious && frameTime > 0.0f ? (world - run.previousWorld) * (1.0f / frameTime) : Vector3(0, 0, 0);
                run.contact = contact;
                run.previousWorld = world;
                run.hadPrevious = true;
                if (contact){
                    if (!run.locked){
                        run.locked = true;
                        run.lockWorld = world;
                    }
                    run.lockWeight = 1.0f;
                }else{
                    run.locked = false;
                    run.lockWeight = c.releaseTime > 0.0f ? std::max(0.0f, run.lockWeight - dt / c.releaseTime) : 0.0f;
                }
                if (run.lockWeight > 0.0f){
                    // the foot keeps the rotation it came with (the animation's, or Foot IK's turn to
                    // the ground): holding the ankle bends the knee, which must not turn it
                    const Quaternion footRotation = spaceRotation[(size_t)run.bones[2]];
                    record(run, solveTwoBone(ctx, run.bones[0], run.bones[1], run.bones[2], toRig(run.lockWorld), nullptr, &footRotation,
                                             weight * run.lockWeight, legOptions(c)), weight * run.lockWeight, run.lockWorld);
                }
                break;
            }
        }
    }
}

void anim::AnimatorInstance::clearIKResults(){
    for (AnimatorIKRun& run : ik){
        run.reached = false;
        run.reachError = 0.0f;
        run.solvedWeight = 0.0f;
        run.contact = false;
        run.footVelocity = Vector3(0, 0, 0);
    }
}

size_t anim::AnimatorInstance::runtimeStateBytes() const{
    // the instance less what it holds by value that is bone- or clip-sized
    size_t bytes = sizeof(*this) - sizeof(rig) - sizeof(humanoid) - 6 * sizeof(Pose);
    bytes += parameters.capacity() * sizeof(float) + velocities.capacity() * sizeof(float);
    bytes += dampTargets.capacity() * sizeof(float) + dampTimes.capacity() * sizeof(float);
    bytes += layers.capacity() * sizeof(AnimatorLayerRun);
    bytes += montages.capacity() * sizeof(AnimatorMontageRun);
    for (const AnimatorMontageRun& run : montages){
        bytes += run.nextOverride.capacity() * sizeof(std::string) + run.hasNextOverride.capacity() / 8;
    }
    bytes += ik.capacity() * sizeof(AnimatorIKRun);
    bytes += output.events.capacity() * sizeof(AnimatorPendingEvent);
    bytes += output.states.capacity() * sizeof(AnimatorStateNote);
    bytes += output.montages.capacity() * sizeof(AnimatorMontageNote);
    return bytes;
}
