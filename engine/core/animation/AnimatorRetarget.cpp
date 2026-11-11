// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorRetarget.h"

#include "animation/AnimationClipIO.h"
#include "animation/AnimatorRig.h"
#include "animation/ClipBinding.h"
#include "animation/ModelSubClips.h"
#include "pose/HumanoidMapIO.h"
#include "pose/ModelRig.h"
#include "pose/PoseIK.h"
#include "subsystem/MeshSystem.h"

#include "tiny_gltf.h"

#include <algorithm>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::anim;
using namespace jasperengine::pose;

namespace{

    Quaternion bridgeRotation(const Matrix4& bridge){
        Vector3 position, scale;
        Quaternion rotation;
        bridge.decompose(position, scale, rotation);
        return rotation;
    }

    // the neutral pose's canonical world rotation of every skeleton bone
    std::vector<Quatd> neutralFrames(const PoseRig& rig){
        std::vector<Quatd> rot;
        std::vector<Affine3d> world;
        rig.computeWorldPrecise(rig.neutralPose(), rot, world);
        const Quatd characterInv = Quatd(rig.characterRotation()).inverse();
        for (Quatd& q : rot) q = (characterInv * q).normalized();
        return rot;
    }

    bool isAncestorBone(const Skeleton& skeleton, int ancestor, int bone){
        int guard = 0;
        for (int b = skeleton.bones[(size_t)bone].parent; b >= 0 && guard < 4096; b = skeleton.bones[(size_t)b].parent, guard++){
            if (b == ancestor) return true;
        }
        return false;
    }

    Quaternion mirrored(const Quaternion& q){ return Quaternion(q.w, q.x, -q.y, -q.z); }

    // ---- the source cache ----

    struct SourceEntry{
        std::string key;
        std::string stamp;
        std::shared_ptr<const RetargetSource> source;
    };

    struct SourceCacheState{
        std::mutex mutex;
        std::vector<SourceEntry> entries;
        uint64_t epoch = 0;
    };

    // never destroyed: the pool may convert during static destruction
    SourceCacheState& sourceCache(){
        static SourceCacheState* cache = new SourceCacheState();
        return *cache;
    }

    // the rig, extra bones and the source maps once the skeleton, its paths, bridges and rests are set
    bool finishSource(const Skeleton& skeleton, const std::vector<int>* boneNodes, const tinygltf::Model* gltf, const std::string& sidecarText,
                      RetargetSource& out, std::string* error){
        HumanoidOverrides overrides;
        if (!sidecarText.empty()){
            std::vector<std::string> warnings;
            std::string parseError;
            if (!parseHumanoidOverrides(sidecarText, overrides, warnings, &parseError)){
                if (error) *error = humanoidSidecarPath(out.modelPath) + ": " + parseError;
                return false;
            }
            for (const std::string& warning : warnings) out.notes.push_back(humanoidSidecarPath(out.modelPath) + ": " + warning);
        }
        HumanExtraMap extra;
        std::string resolveError;
        if (!resolveHumanoid(skeleton, overrides, out.humanoid, extra, &resolveError, &out.notes)){
            if (error) *error = (out.modelPath.empty() ? std::string("the rig") : out.modelPath) + ": not a humanoid: " + resolveError;
            return false;
        }
        auto rig = std::make_shared<PoseRig>();
        if (gltf && boneNodes){
            if (std::shared_ptr<PoseSkin> skin = buildSkinFromGltf(*gltf, skeleton, *boneNodes)) rig->setSkin(skin);
        }
        const std::array<float, HUMAN_BONE_COUNT> twist = overrides.twistDegrees();
        std::string buildError;
        if (!rig->build(skeleton, out.humanoid, &buildError, &twist)){
            if (error) *error = (out.modelPath.empty() ? std::string("the rig") : out.modelPath) + ": " + buildError;
            return false;
        }
        setRetargetRig(out, rig, out.humanoid, extra);
        return true;
    }

}

void anim::setRetargetRig(RetargetSource& out, const std::shared_ptr<const PoseRig>& rig, const HumanoidMap& humanoidMap, const HumanExtraMap& extra){
    out.humanoid = humanoidMap;
    out.RetargetRig::build(rig, extra);
    if (!out.valid()) return;
    const Skeleton& skeleton = rig->skeleton();
    {
        // the source maps (what Convert to Humanoid Clip writes key for key)
        const size_t boneCount = skeleton.bones.size();
        auto bridgeRotOf = [&](int bone){
            return bone >= 0 && (size_t)bone < out.bridges.size() ? Quatd(bridgeRotation(out.bridges[(size_t)bone])) : Quatd();
        };
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            const HumanBone slot = boneAt(s);
            out.slotA[(size_t)s] = out.slotB[(size_t)s] = Quatd();
            out.restE[(size_t)s] = Quaternion();
            out.slotBridge[(size_t)s] = Quaternion();
            if (!rig->has(slot)) continue;
            const int bone = rig->boneIndex(slot);
            const Quatd neutral = rig->neutralWorldCanonical(slot);
            // E = left^-1 * bridge * q * N^-1 (jointRotationFromLocal of N is left^-1)
            out.slotA[(size_t)s] = rig->jointRotationFromLocal(slot, neutral) * bridgeRotOf(bone);
            out.slotB[(size_t)s] = neutral.inverse();
            out.restE[(size_t)s] = rig->jointRotationFromLocal(slot, Quatd(rig->restPose().rotations[(size_t)s])).normalized().toQuaternion();
            out.slotBridge[(size_t)s] = bridgeRotOf(bone).toQuaternion();
        }
        for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
            out.extraA[(size_t)x] = out.extraB[(size_t)x] = Quatd();
            const int bone = out.extraBones[(size_t)x];
            if (bone < 0) continue;
            // d = W * rest^-1 * bridge * q * W^-1
            const Quatd frame(out.extraFrame[(size_t)x]);
            const Quatd rest = Quatd(skeleton.bones[(size_t)bone].rotation).normalized();
            out.extraA[(size_t)x] = frame * rest.inverse() * bridgeRotOf(bone);
            out.extraB[(size_t)x] = frame.inverse();
        }
        // the body offset: C^-1 (hipsWorld(bridge * p) - neutralHips) / humanScale, affine in p
        const int hipsBone = rig->boneIndex(HumanBone::Hips);
        out.hipsBridge = hipsBone >= 0 && (size_t)hipsBone < out.bridges.size() ? out.bridges[(size_t)hipsBone] : Matrix4();
        const Quatd characterInv = Quatd(rig->characterRotation()).inverse();
        const Vec3d neutralHips(rig->neutralHipsWorldPosition());
        const double scale = rig->humanScale();
        auto offsetOfSkeletonLocal = [&](const Vector3& skeletonLocal){
            HumanPose pose = rig->restPose();
            pose.hipsPosition = skeletonLocal;
            return characterInv * (Vec3d(rig->hipsWorldPosition(pose)) - neutralHips) / scale;
        };
        out.hipsOrigin = offsetOfSkeletonLocal(out.hipsBridge * Vector3(0, 0, 0));
        const Vector3 axes[3] = {Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1)};
        for (int c = 0; c < 3; c++) out.hipsLinear[c] = offsetOfSkeletonLocal(out.hipsBridge * axes[c]) - out.hipsOrigin;
        out.restHips = offsetOfSkeletonLocal(rig->restPose().hipsPosition).toVector3();

        // bones between slots
        out.between.assign(boneCount, 0);
        std::vector<int> extraOfBone(boneCount, -1);
        for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
            if (out.extraBones[(size_t)x] >= 0) extraOfBone[(size_t)out.extraBones[(size_t)x]] = x;
        }
        for (size_t j = 0; j < boneCount; j++){
            if (rig->slotOfBone((int)j) >= 0 || extraOfBone[j] >= 0) continue;
            for (int s = 0; s < HUMAN_BONE_COUNT && !out.between[j]; s++){
                const int bone = rig->boneIndex(boneAt(s));
                if (bone >= 0 && isAncestorBone(skeleton, (int)j, bone)) out.between[j] = 1;
            }
        }
    }
}

// ---- the sample ----

void anim::mirrorHumanoidSample(const HumanoidSample& in, HumanoidSample& out){
    out.hasE.reset();
    out.hasExtra.reset();
    for (int s = 0; s < HUMAN_BONE_COUNT; s++){
        const int m = boneIndex(humanBoneInfo(boneAt(s)).mirror);
        out.hasE[(size_t)m] = in.hasE[(size_t)s];
        out.E[(size_t)m] = mirrored(in.E[(size_t)s]);
    }
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        const int m = extraIndex(humanExtraBoneInfo(extraAt(x)).mirror);
        out.hasExtra[(size_t)m] = in.hasExtra[(size_t)x];
        out.extra[(size_t)m] = mirrored(in.extra[(size_t)x]);
    }
    out.hasHips = in.hasHips;
    out.hips = Vector3(-in.hips.x, in.hips.y, in.hips.z);
    for (int side = 0; side < 2; side++){
        out.hasFeet[1 - side] = in.hasFeet[side];
        out.feet[1 - side] = Vector3(-in.feet[side].x, in.feet[side].y, in.feet[side].z);
    }
}

// ---- a humanoid rig ----

void anim::RetargetRig::build(const std::shared_ptr<const PoseRig>& poseRig, const HumanExtraMap& extraMap){
    rig = poseRig;
    extra = extraMap;
    extraBones.fill(-1);
    for (auto& chain : chains) chain.clear();
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        extraRest[(size_t)x] = Quaternion();
        extraFrame[(size_t)x] = Quaternion();
    }
    if (!rig || !rig->valid()) return;
    for (int s = 0; s < HUMAN_BONE_COUNT; s++){
        const HumanBone slot = boneAt(s);
        if (!rig->has(slot)) continue;
        // the slots this slot stands for: up the anatomical chain to its effective parent
        const HumanBone parent = rig->effectiveParent(slot);
        std::vector<uint8_t>& chain = chains[(size_t)s];
        for (HumanBone c = slot; c != HumanBone::Count && c != parent; c = humanBoneInfo(c).parent){
            chain.insert(chain.begin(), (uint8_t)boneIndex(c));
        }
    }
    const std::vector<Quatd> frames = neutralFrames(*rig);
    const Skeleton& skeleton = rig->skeleton();
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        const int bone = findSkeletonBone(skeleton, extra.bones[(size_t)x]);
        if (bone < 0) continue;
        extraBones[(size_t)x] = bone;
        extraRest[(size_t)x] = Quatd(skeleton.bones[(size_t)bone].rotation).normalized().toQuaternion();
        extraFrame[(size_t)x] = frames[(size_t)bone].toQuaternion();
    }
}

void anim::realiseHumanoid(const RetargetRig& target, const HumanoidSample& sample, bool mirror, bool footIK,
                           HumanPose& out, std::array<Quaternion, HUMAN_EXTRA_BONE_COUNT>& extraLocal,
                           std::bitset<HUMAN_EXTRA_BONE_COUNT>* extraLive){
    if (!target.valid()) return;
    const PoseRig& rig = *target.rig;
    HumanoidSample mirroredSample;
    const HumanoidSample* s = &sample;
    if (mirror){
        mirrorHumanoidSample(sample, mirroredSample);
        s = &mirroredSample;
    }
    out.hipsPosition = rig.neutralPose().hipsPosition;
    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
        const HumanBone slot = boneAt(b);
        if (!rig.has(slot)){
            out.rotations[(size_t)b] = Quaternion();
            continue;
        }
        // E_t(b): the sample's E along the chain (missing slots: identity), parent first
        Quatd E;
        for (uint8_t c : target.chains[(size_t)b]){
            if (s->hasE[c]) E = E * Quatd(s->E[c]);
        }
        out.rotations[(size_t)b] = rig.localFromJointRotation(slot, E.normalized());
    }
    const Vec3d neutralHips(rig.neutralHipsWorldPosition());
    const Quatd character(rig.characterRotation());
    const double scale = rig.humanScale();
    auto model = [&](const Vector3& value){
        return (neutralHips + character * (Vec3d(value) * scale)).toVector3();
    };
    if (s->hasHips) rig.setHipsWorldPosition(out, model(s->hips));
    if (footIK){
        for (int side = 0; side < 2; side++){
            if (!s->hasFeet[side]) continue;
            solveLimbIK(rig, out, side == 0 ? Limb::LeftLeg : Limb::RightLeg, model(s->feet[side]), true);
        }
    }
    if (extraLive) extraLive->reset();
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        if (target.extraBones[(size_t)x] < 0) continue;
        // a bone the (mirrored) sample has no value for stays at its rest: E = identity
        if (!s->hasExtra[(size_t)x]){
            extraLocal[(size_t)x] = target.extraRest[(size_t)x];
            continue;
        }
        if (extraLive) extraLive->set((size_t)x);
        const Quatd frame(target.extraFrame[(size_t)x]);
        extraLocal[(size_t)x] = (Quatd(target.extraRest[(size_t)x]) * frame.inverse() * Quatd(s->extra[(size_t)x]) * frame).normalized().toQuaternion();
    }
}

// ---- a source ----

int anim::RetargetSource::boneOfPath(const std::string& path) const{
    if (!rig) return -1;
    for (size_t i = 0; i < bonePaths.size(); i++){
        if (bonePaths[i] == path) return (int)i;
    }
    // a clip made on a parent of the model: its paths have more segments in front
    std::vector<PathSegment> segments;
    if (!parsePath(path, segments) || segments.empty()) return -1;
    for (size_t first = 1; first < segments.size(); first++){
        const std::vector<PathSegment> rest(segments.begin() + (long)first, segments.end());
        const std::string suffix = formatPath(rest);
        for (size_t i = 0; i < bonePaths.size(); i++){
            if (bonePaths[i] == suffix) return (int)i;
        }
    }
    // a unique bone name (the last segment)
    const PathSegment& last = segments.back();
    if (last.index != 0) return -1;
    int found = -1;
    const Skeleton& skeleton = rig->skeleton();
    for (size_t i = 0; i < skeleton.bones.size(); i++){
        if (skeleton.bones[i].name == last.name){
            if (found >= 0) return -1;
            found = (int)i;
        }
    }
    return found;
}

RetargetTrackRole anim::RetargetSource::roleOf(ClipChannel channel, int bone) const{
    const bool transform = channel == ClipChannel::Position || channel == ClipChannel::Rotation || channel == ClipChannel::Scale;
    if (!transform || bone < 0 || !rig) return RetargetTrackRole::Generic;
    const int slot = rig->slotOfBone(bone);
    int extraBone = -1;
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        if (extraBones[(size_t)x] == bone) extraBone = x;
    }
    const bool isBetween = (size_t)bone < between.size() && between[(size_t)bone];
    if (channel == ClipChannel::Rotation && slot >= 0) return RetargetTrackRole::Slot;
    if (channel == ClipChannel::Rotation && extraBone >= 0) return RetargetTrackRole::Extra;
    if (channel == ClipChannel::Position && slot == boneIndex(HumanBone::Hips)) return RetargetTrackRole::Hips;
    // a bone the target maps by role, not by name: its translation or scale would land on a
    // same-named bone; bones between slots keep their rest (humanoid playback does not drive them)
    if (slot >= 0 || extraBone >= 0 || isBetween) return RetargetTrackRole::Dropped;
    return RetargetTrackRole::Generic;
}

bool anim::buildRetargetSource(const tinygltf::Model& gltf, const std::string& modelPath, const std::string& sidecarText,
                               RetargetSource& out, std::string* error){
    out = RetargetSource();
    out.modelPath = modelPath;
    Skeleton skeleton;
    std::vector<int> boneNodes;
    if (!buildSkeletonFromGltf(gltf, skeleton, out.bonePaths, &out.bridges, &boneNodes)){
        if (error) *error = "the model '" + modelPath + "' has no skeleton (no glTF skin)";
        return false;
    }
    out.restRotation.resize(boneNodes.size());
    out.restPosition.resize(boneNodes.size());
    out.restScale.resize(boneNodes.size());
    for (size_t i = 0; i < boneNodes.size(); i++){
        gltfNodeLocal(gltf, boneNodes[i], out.restPosition[i], out.restRotation[i], out.restScale[i]);
    }
    return finishSource(skeleton, &boneNodes, &gltf, sidecarText, out, error);
}

bool anim::buildRetargetSource(const Skeleton& skeleton, const std::vector<std::string>& bonePaths, const std::string& modelPath,
                               const std::string& sidecarText, RetargetSource& out, std::string* error){
    out = RetargetSource();
    out.modelPath = modelPath;
    out.bonePaths = bonePaths;
    out.bonePaths.resize(skeleton.bones.size());
    out.bridges.assign(skeleton.bones.size(), Matrix4());
    for (const SkeletonBone& bone : skeleton.bones){
        out.restRotation.push_back(bone.rotation);
        out.restPosition.push_back(bone.position);
        out.restScale.push_back(bone.scale);
    }
    return finishSource(skeleton, nullptr, nullptr, sidecarText, out, error);
}

void anim::sourceFootGoals(const RetargetSource& source, const HumanPose& skeletonPose, Vector3 goals[2], bool has[2]){
    has[0] = has[1] = false;
    if (!source.valid()) return;
    const PoseRig& rig = *source.rig;
    // scratch kept per thread (players sample on the main thread, Animators on workers)
    thread_local std::vector<Quatd> worldRot;
    thread_local std::vector<Affine3d> world;
    rig.computeWorldPrecise(skeletonPose, worldRot, world);
    const Quatd characterInv = Quatd(rig.characterRotation()).inverse();
    const Vec3d neutralHips(rig.neutralHipsWorldPosition());
    const double scale = rig.humanScale();
    for (int side = 0; side < 2; side++){
        const int foot = rig.boneIndex(side == 0 ? HumanBone::LeftFoot : HumanBone::RightFoot);
        if (foot < 0 || (size_t)foot >= world.size()) continue;
        goals[side] = (characterInv * (world[(size_t)foot].t - neutralHips) / scale).toVector3();
        has[side] = true;
    }
}

std::shared_ptr<const RetargetSource> anim::retargetSource(const std::string& modelPath, std::string* error){
    if (!isModelFilePath(modelPath)){
        if (error) *error = "retarget: '" + modelPath + "' is not a glTF, GLB or FBX model";
        return nullptr;
    }
    const std::string key = MeshSystem::getModelFilenameKey(modelPath);
    std::string stamp;
    if (!humanoidSidecarStamp(modelPath, stamp, error)) return nullptr;
    SourceCacheState& cache = sourceCache();
    uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        for (const SourceEntry& entry : cache.entries){
            if (entry.key == key && entry.stamp == stamp) return entry.source;
        }
        epoch = cache.epoch;
    }
    // built without the lock (a parse of the model)
    auto source = std::make_shared<RetargetSource>();
    bool built = false;
    if (!readModelFile(modelPath, [&](const tinygltf::Model& gltf){
            built = buildRetargetSource(gltf, modelPath, stamp, *source, error);
            return built;
        }, error)){
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.epoch == epoch){
        cache.entries.erase(std::remove_if(cache.entries.begin(), cache.entries.end(),
                                           [&](const SourceEntry& entry){ return entry.key == key; }), cache.entries.end());
        cache.entries.push_back({key, stamp, source});
    }
    return source;
}

void anim::dropRetargetSources(const std::string& modelPath){
    SourceCacheState& cache = sourceCache();
    const std::string key = modelPath.empty() ? std::string() : MeshSystem::getModelFilenameKey(modelPath);
    std::vector<SourceEntry> dropped;     // freed after the unlock
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.epoch++;
    for (auto it = cache.entries.begin(); it != cache.entries.end();){
        if (key.empty() || it->key == key){
            dropped.push_back(std::move(*it));
            it = cache.entries.erase(it);
        }else{
            ++it;
        }
    }
}

std::string anim::clipSourceModel(const AnimationClip& clip, const std::string& reference){
    SubClipReference parsed;
    if (parseSubClipReference(reference, parsed)) return parsed.model;
    if (clip.getSource().kind == "model" && !clip.getSource().path.empty()){
        SubClipReference sourceReference;
        if (parseSubClipReference(clip.getSource().path, sourceReference)) return sourceReference.model;
        return clip.getSource().path;
    }
    return clip.getPreviewModel();
}

std::shared_ptr<const RetargetSource> anim::clipRetargetSource(const AnimationClip& clip, const std::string& reference, std::string* error){
    const std::string model = clipSourceModel(clip, reference);
    if (model.empty()){
        if (error) *error = "retarget: the clip names no source model (set its Preview Model)";
        return nullptr;
    }
    if (!isModelFilePath(model)){
        if (error) *error = "retarget: the clip's source '" + model + "' is not a model (set a .gltf, .glb or .fbx as its Preview Model)";
        return nullptr;
    }
    std::string sourceError;
    std::shared_ptr<const RetargetSource> source = retargetSource(model, &sourceError);
    if (!source && error) *error = "retarget from " + model + ": " + sourceError;
    return source;
}

// ---- a clip read as humanoid samples ----

bool anim::RetargetBinding::bind(const AnimationClip& clip, const std::shared_ptr<const RetargetSource>& sourceRig){
    active = false;
    source.reset();
    slots.reset();
    extras.reset();
    hips = false;
    feet[0] = feet[1] = false;
    dropped.clear();
    const std::vector<AnimationClip::Track>& tracks = clip.getTracks();
    trackField.assign(tracks.size(), -1);
    trackBone.assign(tracks.size(), -1);
    byPath.assign(tracks.size(), 1);
    if (clip.isHumanoid()){
        for (size_t i = 0; i < tracks.size(); i++){
            const AnimationClip::Track& track = tracks[i];
            if (!isHumanoidChannel(track.channel)) continue;
            byPath[i] = 0;
            if (!track.isSampled()) continue;
            if (track.channel == ClipChannel::Humanoid && track.humanBone >= 0 && track.humanBone < HUMANOID_TRACK_BONE_COUNT){
                trackField[i] = (int16_t)track.humanBone;
                if (track.humanBone < HUMAN_BONE_COUNT) slots[(size_t)track.humanBone] = true;
                else extras[(size_t)(track.humanBone - HUMAN_BONE_COUNT)] = true;
            }else if (track.channel == ClipChannel::HumanoidPosition && track.humanBone >= 0 && track.humanBone < HUMANOID_POSITION_COUNT){
                trackField[i] = (int16_t)(FIELD_POSITION + track.humanBone);
                if (track.humanBone == HUMANOID_POSITION_HIPS) hips = true;
                else feet[track.humanBone - HUMANOID_POSITION_LEFT_FOOT] = true;
            }
        }
        active = slots.any() || extras.any() || hips || feet[0] || feet[1];
        return active;
    }
    if (!sourceRig || !sourceRig->valid()) return false;
    source = sourceRig;
    const PoseRig& rig = *source->rig;
    for (size_t i = 0; i < tracks.size(); i++){
        const AnimationClip::Track& track = tracks[i];
        const int bone = source->boneOfPath(track.path);
        const RetargetTrackRole role = source->roleOf(track.channel, bone);
        if (role == RetargetTrackRole::Generic) continue;
        byPath[i] = 0;
        trackBone[i] = bone;
        if (!track.isSampled()) continue;
        switch (role){
            case RetargetTrackRole::Slot:
                trackField[i] = (int16_t)rig.slotOfBone(bone);
                break;
            case RetargetTrackRole::Extra:
                for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
                    if (source->extraBones[(size_t)x] == bone){
                        trackField[i] = (int16_t)(HUMAN_BONE_COUNT + x);
                        extras[(size_t)x] = true;
                    }
                }
                break;
            case RetargetTrackRole::Hips:
                trackField[i] = (int16_t)(FIELD_POSITION + HUMANOID_POSITION_HIPS);
                break;
            case RetargetTrackRole::Dropped:
                dropped.push_back(track.path + (track.channel == ClipChannel::Scale ? " (scale)"
                                               : track.channel == ClipChannel::Position ? " (translation)" : ""));
                break;
            case RetargetTrackRole::Generic:
                break;
        }
    }
    // every mapped slot (its rest E without a track) and the hips, as Convert writes them
    for (int s = 0; s < HUMAN_BONE_COUNT; s++) slots[(size_t)s] = rig.has(boneAt(s));
    hips = rig.has(HumanBone::Hips);
    feet[0] = rig.has(HumanBone::LeftFoot);
    feet[1] = rig.has(HumanBone::RightFoot);
    active = true;
    return true;
}

void anim::RetargetBinding::sample(const AnimationClip& clip, float clipTime, KeyCursor* cursors, bool withFeet, HumanoidSample& out,
                                   float stepFps) const{
    out.clear();
    if (!active) return;
    const std::vector<AnimationClip::Track>& tracks = clip.getTracks();
    const size_t count = std::min(tracks.size(), trackField.size());
    TrackSample value;
    if (!source){
        for (size_t i = 0; i < count; i++){
            const int field = trackField[i];
            if (field < 0) continue;
            const float fps = field == FIELD_POSITION + HUMANOID_POSITION_HIPS ? 0.0f : stepFps;
            if (!sampleTrack(tracks[i], clipTime, value, cursors ? &cursors[i] : nullptr, fps)) continue;
            if (field < HUMAN_BONE_COUNT){
                out.E[(size_t)field] = value.rotation;
                out.hasE[(size_t)field] = true;
            }else if (field < HUMANOID_TRACK_BONE_COUNT){
                out.extra[(size_t)(field - HUMAN_BONE_COUNT)] = value.rotation;
                out.hasExtra[(size_t)(field - HUMAN_BONE_COUNT)] = true;
            }else if (field == FIELD_POSITION + HUMANOID_POSITION_HIPS){
                out.hips = value.vector;
                out.hasHips = true;
            }else{
                const int side = field - FIELD_POSITION - HUMANOID_POSITION_LEFT_FOOT;
                if (side == 0 || side == 1){
                    out.feet[side] = value.vector;
                    out.hasFeet[side] = true;
                }
            }
        }
        return;
    }
    // a generic clip through its source: every mapped slot at its rest E, the hips at rest,
    // then the tracks (D12's source half)
    const RetargetSource& src = *source;
    const PoseRig& rig = *src.rig;
    HumanPose skeletonPose;
    if (withFeet) skeletonPose = rig.restPose();
    for (int s = 0; s < HUMAN_BONE_COUNT; s++){
        if (!slots[(size_t)s]) continue;
        out.E[(size_t)s] = src.restE[(size_t)s];
        out.hasE[(size_t)s] = true;
    }
    if (hips){
        out.hips = src.restHips;
        out.hasHips = true;
    }
    for (size_t i = 0; i < count; i++){
        const int field = trackField[i];
        if (field < 0) continue;
        const AnimationClip::Track& track = tracks[i];
        const float fps = field == FIELD_POSITION + HUMANOID_POSITION_HIPS ? 0.0f : stepFps;
        if (!sampleTrack(track, clipTime, value, cursors ? &cursors[i] : nullptr, fps)) continue;
        const int bone = trackBone[i];
        if (field < HUMANOID_TRACK_BONE_COUNT){
            Quatd q(value.rotation);
            if (track.relative && bone >= 0 && (size_t)bone < src.restRotation.size()) q = Quatd(src.restRotation[(size_t)bone]) * q;
            if (field < HUMAN_BONE_COUNT){
                out.E[(size_t)field] = (src.slotA[(size_t)field] * q * src.slotB[(size_t)field]).normalized().toQuaternion();
                if (withFeet) skeletonPose.rotations[(size_t)field] = (Quatd(src.slotBridge[(size_t)field]) * q).normalized().toQuaternion();
            }else{
                const size_t x = (size_t)(field - HUMAN_BONE_COUNT);
                out.extra[x] = (src.extraA[x] * q * src.extraB[x]).normalized().toQuaternion();
                out.hasExtra[x] = true;
            }
        }else if (field == FIELD_POSITION + HUMANOID_POSITION_HIPS){
            Vector3 p = value.vector;
            if (track.relative && bone >= 0 && (size_t)bone < src.restPosition.size()) p = src.restPosition[(size_t)bone] + p;
            out.hips = src.bodyOffset(p).toVector3();
            if (withFeet) skeletonPose.hipsPosition = src.hipsBridge * p;
        }
    }
    if (withFeet && (feet[0] || feet[1])) sourceFootGoals(src, skeletonPose, out.feet, out.hasFeet);
}

// ---- the Animator's target ----

void anim::RetargetTarget::build(const AnimatorRig& rigBones, const std::string& modelPath){
    valid = false;
    error.clear();
    avatar.reset();
    rig.reset();
    slotBones.fill(-1);
    extraBones.fill(-1);
    for (Quaternion& q : slotBridgeInverse) q = Quaternion();
    for (Quaternion& q : extraBridgeInverse) q = Quaternion();
    hipsBridgeInverse = Matrix4();
    // A model file: its avatar is the file's (its skin skeleton, the sidecar), the very rig a clip
    // of that file retargets from, so a clip of the same model plays unchanged; slots map to the
    // rig's bones by path. The rig's own bones are a subset of the nodes (what the clips, their
    // ancestors and the skins name), which can change what the auto-detect finds.
    if (!modelPath.empty()){
        std::string fileError;
        std::shared_ptr<const RetargetSource> file = retargetSource(modelPath, &fileError);
        if (file){
            // a file path's rig bone: the same path; the Animator above the model (the rig's path
            // ends with the file's); or a model whose hierarchy holds its joints only (the file's
            // path ends with the rig's: its root nodes are no entities). The longest match wins.
            auto rigBoneOf = [&](const std::string& path) -> int {
                if (path.empty()) return -1;
                const int exact = rigBones.findBone(path);
                if (exact >= 0) return exact;
                auto endsWith = [](const std::string& text, const std::string& tail){
                    return text.size() > tail.size() + 1 && text.compare(text.size() - tail.size(), tail.size(), tail) == 0 &&
                           text[text.size() - tail.size() - 1] == '/';
                };
                int best = -1;
                size_t bestLength = 0;
                for (size_t i = 0; i < rigBones.bones.size(); i++){
                    const std::string& candidate = rigBones.bones[i].path;
                    if (candidate.empty()) continue;
                    const bool match = endsWith(candidate, path) || endsWith(path, candidate);
                    const size_t length = std::min(candidate.size(), path.size());
                    if (match && length > bestLength){
                        best = (int)i;
                        bestLength = length;
                    }
                }
                return best;
            };
            for (int s = 0; s < HUMAN_BONE_COUNT; s++){
                const int bone = file->rig->boneIndex(boneAt(s));
                if (bone < 0 || (size_t)bone >= file->bonePaths.size()) continue;
                slotBones[(size_t)s] = rigBoneOf(file->bonePaths[(size_t)bone]);
                slotBridgeInverse[(size_t)s] = file->slotBridge[(size_t)s].inverse();
            }
            for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
                const int bone = file->extraBones[(size_t)x];
                if (bone < 0 || (size_t)bone >= file->bonePaths.size()) continue;
                extraBones[(size_t)x] = rigBoneOf(file->bonePaths[(size_t)bone]);
                if ((size_t)bone < file->bridges.size()) extraBridgeInverse[(size_t)x] = bridgeRotation(file->bridges[(size_t)bone]).inverse();
            }
            if (slotBones[(size_t)HumanBone::Hips] >= 0){
                hipsBridgeInverse = file->hipsBridge.inverse();
                avatar = file;
                rig = file->rig;
                valid = true;
                return;
            }
            slotBones.fill(-1);
            extraBones.fill(-1);
            for (Quaternion& q : slotBridgeInverse) q = Quaternion();
            for (Quaternion& q : extraBridgeInverse) q = Quaternion();
        }
    }
    // the rig's own bones (names, rest; no bridges); the root bone (an animated root) is not part
    // of the skeleton: rig space is the root's own
    const size_t first = rigBones.rootBone ? 1 : 0;
    Skeleton skeleton;
    std::vector<std::string> paths;
    std::vector<int> skeletonIndex(rigBones.bones.size(), -1);
    for (size_t i = first; i < rigBones.bones.size(); i++){
        SkeletonBone bone;
        bone.name = rigBones.bones[i].name;
        const int parent = rigBones.bones[i].parent;
        bone.parent = parent >= 0 ? skeletonIndex[(size_t)parent] : -1;
        bone.position = rigBones.rest.position(i);
        bone.rotation = rigBones.rest.rotation(i);
        bone.scale = rigBones.rest.scale(i);
        skeletonIndex[i] = (int)skeleton.bones.size();
        skeleton.bones.push_back(bone);
        paths.push_back(rigBones.bones[i].path);
    }
    std::string stamp;
    if (!modelPath.empty() && !humanoidSidecarStamp(modelPath, stamp, &error)) return;
    auto own = std::make_shared<RetargetSource>();
    if (!buildRetargetSource(skeleton, paths, modelPath, stamp, *own, &error)) return;
    for (int s = 0; s < HUMAN_BONE_COUNT; s++){
        const int bone = own->rig->boneIndex(boneAt(s));
        slotBones[(size_t)s] = bone >= 0 ? bone + (int)first : -1;
    }
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        const int bone = own->extraBones[(size_t)x];
        extraBones[(size_t)x] = bone >= 0 ? bone + (int)first : -1;
    }
    avatar = own;
    rig = own->rig;
    valid = true;
}

void anim::writeRealised(const RetargetTarget& target, const HumanPose& pose, const std::array<Quaternion, HUMAN_EXTRA_BONE_COUNT>& extraLocal,
                         const std::bitset<HUMAN_EXTRA_BONE_COUNT>& extraLive, bool hips, bool slots, Pose& out){
    if (!target.valid) return;
    const PoseRig& rig = *target.rig;
    if (slots){
        for (int s = 0; s < HUMAN_BONE_COUNT; s++){
            const int bone = target.slotBones[(size_t)s];
            if (bone < 0 || !rig.has(boneAt(s))) continue;
            out.setRotation((size_t)bone, (target.slotBridgeInverse[(size_t)s] * pose.rotations[(size_t)s]).normalized());
        }
    }
    const int hipsBone = target.slotBones[(size_t)HumanBone::Hips];
    if (hips && hipsBone >= 0) out.setPosition((size_t)hipsBone, target.hipsBridgeInverse * pose.hipsPosition);
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        const int bone = target.extraBones[(size_t)x];
        if (bone < 0 || !extraLive[(size_t)x]) continue;
        out.setRotation((size_t)bone, (target.extraBridgeInverse[(size_t)x] * extraLocal[(size_t)x]).normalized());
    }
}
