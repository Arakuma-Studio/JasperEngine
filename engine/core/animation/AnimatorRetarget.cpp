// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorRetarget.h"

#include "animation/AnimatorRig.h"
#include "animation/ModelSubClips.h"
#include "pose/HumanoidMapIO.h"
#include "pose/ModelRig.h"
#include "subsystem/MeshSystem.h"

#include <map>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::anim;

namespace{

    struct SourceCache{
        std::mutex mutex;
        // by model key: the sidecar text it was built with, and the source
        std::map<std::string, std::pair<std::string, std::shared_ptr<const RetargetSource>>> entries;
    };

    SourceCache& sourceCache(){
        static SourceCache* cache = new SourceCache();
        return *cache;
    }

    // the sidecar's overrides; false with `error` when it exists but does not parse
    bool readOverrides(const std::string& modelPath, std::string& stamp, pose::HumanoidOverrides& overrides, std::string* error){
        stamp.clear();
        if (modelPath.empty()) return true;
        if (!pose::humanoidSidecarStamp(modelPath, stamp, error)) return false;
        if (stamp.empty()) return true;
        std::vector<std::string> warnings;
        return pose::parseHumanoidOverrides(stamp, overrides, warnings, error);
    }

    // the rest transform of `bone`'s parent chain in the skeleton's model space
    Affine parentChain(const pose::Skeleton& skeleton, int bone){
        Affine result;
        std::vector<int> chain;
        for (int p = bone >= 0 ? skeleton.bones[(size_t)bone].parent : -1; p >= 0; p = skeleton.bones[(size_t)p].parent){
            chain.push_back(p);
            if (chain.size() > skeleton.bones.size()) break;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it){
            const pose::SkeletonBone& b = skeleton.bones[(size_t)*it];
            result = result * Affine::compose(b.position, b.rotation, b.scale);
        }
        return result;
    }

}

std::shared_ptr<const RetargetSource> anim::retargetSource(const std::string& modelPath, std::string* error){
    const std::string key = MeshSystem::getModelFilenameKey(modelPath);
    std::string stamp;
    pose::HumanoidOverrides overrides;
    if (!readOverrides(modelPath, stamp, overrides, error)) return nullptr;
    SourceCache& cache = sourceCache();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        auto it = cache.entries.find(key);
        if (it != cache.entries.end() && it->second.first == stamp) return it->second.second;
    }
    std::vector<ModelNodeInfo> nodes;
    if (!readModelFileNodes(modelPath, nodes, error)) return nullptr;
    auto source = std::make_shared<RetargetSource>();
    source->modelPath = modelPath;
    for (const ModelNodeInfo& node : nodes){
        pose::SkeletonBone bone;
        bone.name = node.name;
        bone.parent = node.parent;
        bone.position = node.position;
        bone.rotation = node.rotation;
        bone.scale = node.scale;
        source->skeleton.bones.push_back(bone);
        source->paths.push_back(node.addressable ? node.path : std::string());
    }
    pose::HumanoidMap map;
    std::string mapError;
    if (!pose::resolveHumanoid(source->skeleton, overrides, map, &mapError) || !source->rig.build(source->skeleton, map, &mapError)){
        if (error) *error = modelPath + " is not a usable humanoid: " + mapError;
        return nullptr;
    }
    source->hipsParent = parentChain(source->skeleton, source->rig.boneIndex(pose::HumanBone::Hips));
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.entries[key] = {stamp, source};
    return source;
}

void anim::clearRetargetSources(){
    SourceCache& cache = sourceCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.entries.clear();
}

void anim::RetargetTarget::build(const AnimatorRig& rigBones, const std::string& modelPath){
    valid = false;
    error.clear();
    skeleton.bones.clear();
    slotBones.fill(-1);
    // the root bone (an animated root) is not part of the skeleton: rig space is the root's own
    const size_t first = rigBones.rootBone ? 1 : 0;
    std::vector<int> skeletonIndex(rigBones.bones.size(), -1);
    for (size_t i = first; i < rigBones.bones.size(); i++){
        pose::SkeletonBone bone;
        bone.name = rigBones.bones[i].name;
        const int parent = rigBones.bones[i].parent;
        bone.parent = parent >= 0 ? skeletonIndex[(size_t)parent] : -1;
        bone.position = rigBones.rest.position(i);
        bone.rotation = rigBones.rest.rotation(i);
        bone.scale = rigBones.rest.scale(i);
        skeletonIndex[i] = (int)skeleton.bones.size();
        skeleton.bones.push_back(bone);
    }
    std::string stamp;
    pose::HumanoidOverrides overrides;
    if (!readOverrides(modelPath, stamp, overrides, &error)) return;
    pose::HumanoidMap map;
    if (!pose::resolveHumanoid(skeleton, overrides, map, &error) || !rig.build(skeleton, map, &error)) return;
    for (int s = 0; s < pose::HUMAN_BONE_COUNT; s++){
        const int bone = rig.boneIndex(pose::boneAt(s));
        slotBones[(size_t)s] = bone >= 0 ? bone + (int)first : -1;
    }
    hipsParentInverse = parentChain(skeleton, rig.boneIndex(pose::HumanBone::Hips)).inverse();
    valid = true;
}

void anim::RetargetBinding::bind(const AnimationClip& clip, const RetargetSource& source, const RetargetTarget& target){
    active = false;
    cursors.assign(clip.getTracks().size(), KeyCursor());
    if (!target.valid) return;
    const std::vector<AnimationClip::Track>& tracks = clip.getTracks();
    auto findTrack = [&](const std::string& path, ClipChannel channel){
        if (path.empty()) return -1;
        for (size_t t = 0; t < tracks.size(); t++){
            if (tracks[t].channel == channel && tracks[t].path == path && tracks[t].isSampled()) return (int)t;
        }
        return -1;
    };
    for (int s = 0; s < pose::HUMAN_BONE_COUNT; s++){
        const pose::HumanBone bone = pose::boneAt(s);
        Slot& slot = slots[(size_t)s];
        slot = Slot();
        const int sourceBone = source.rig.boneIndex(bone);
        if (sourceBone < 0 || target.slotBones[(size_t)s] < 0) continue;
        Quaternion leftS, neutralS, leftT, neutralT;
        source.rig.slotFrame(bone, leftS, neutralS);
        target.rig.slotFrame(bone, leftT, neutralT);
        slot.targetBone = target.slotBones[(size_t)s];
        slot.rotationTrack = findTrack(source.paths[(size_t)sourceBone], ClipChannel::Rotation);
        slot.sourceRest = source.skeleton.bones[(size_t)sourceBone].rotation;
        slot.pre = (leftT * leftS.inverse()).normalized();
        slot.post = (neutralS.inverse() * neutralT).normalized();
        if (s == (int)pose::HumanBone::Hips && slot.rotationTrack >= 0) hipsRotationRelative = tracks[(size_t)slot.rotationTrack].relative;
    }
    const int sourceHips = source.rig.boneIndex(pose::HumanBone::Hips);
    targetHips = target.slotBones[(size_t)pose::HumanBone::Hips];
    if (sourceHips < 0 || targetHips < 0) return;
    hipsPositionTrack = findTrack(source.paths[(size_t)sourceHips], ClipChannel::Position);
    sourceHipsRelative = hipsPositionTrack >= 0 && tracks[(size_t)hipsPositionTrack].relative;
    sourceHipsRest = source.skeleton.bones[(size_t)sourceHips].position;
    sourceHipsParent = source.hipsParent;
    sourceNeutralHips = source.rig.neutralHipsWorldPosition();
    targetNeutralHips = target.rig.neutralHipsWorldPosition();
    canonical = (target.rig.characterRotation() * source.rig.characterRotation().inverse()).normalized();
    scale = source.rig.humanScale() > 1e-6f ? target.rig.humanScale() / source.rig.humanScale() : 1.0f;
    targetHipsParentInverse = target.hipsParentInverse;
    active = true;
}

void anim::sampleRetargeted(RetargetBinding& binding, const AnimationClip& clip, float clipTime, Pose& out){
    if (!binding.active) return;
    const std::vector<AnimationClip::Track>& tracks = clip.getTracks();
    TrackSample sample;
    for (size_t s = 0; s < binding.slots.size(); s++){
        const RetargetBinding::Slot& slot = binding.slots[s];
        if (slot.targetBone < 0) continue;
        Quaternion local = slot.sourceRest;
        if (slot.rotationTrack >= 0){
            const AnimationClip::Track& track = tracks[(size_t)slot.rotationTrack];
            if (sampleTrack(track, clipTime, sample, &binding.cursors[(size_t)slot.rotationTrack])){
                local = track.relative ? slot.sourceRest * sample.rotation : sample.rotation;
            }
        }
        out.setRotation((size_t)slot.targetBone, (slot.pre * local * slot.post).normalized());
    }
    // the hips: their model-space offset from the neutral hips, in canonical axes, scaled
    Vector3 hips = binding.sourceHipsRest;
    if (binding.hipsPositionTrack >= 0){
        const AnimationClip::Track& track = tracks[(size_t)binding.hipsPositionTrack];
        if (sampleTrack(track, clipTime, sample, &binding.cursors[(size_t)binding.hipsPositionTrack])){
            hips = track.relative ? binding.sourceHipsRest + sample.vector : sample.vector;
        }
    }
    const Vector3 sourceModel = binding.sourceHipsParent.transformPoint(hips);
    const Vector3 targetModel = binding.targetNeutralHips + binding.canonical * ((sourceModel - binding.sourceNeutralHips) * binding.scale);
    out.setPosition((size_t)binding.targetHips, binding.targetHipsParentInverse.transformPoint(targetModel));
}
