// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HumanoidClip.h"

#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "animation/KeyframeSampling.h"
#include "animation/ModelSubClips.h"
#include "component/Transform.h"
#include "registry/EntityRegistry.h"
#include "pose/HumanoidMapIO.h"
#include "pose/PoseIK.h"
#include "pose/PoseMath.h"
#include "subsystem/MeshSystem.h"

#include "tiny_gltf.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::pose;

namespace{

    constexpr int FIELD_POSITION = 100;

    Quaternion bridgeRotation(const Matrix4& bridge){
        Vector3 position, scale;
        Quaternion rotation;
        bridge.decompose(position, scale, rotation);
        return rotation;
    }

    // a fixed map q -> a * q * b on quaternion values (a linear map of the four components, so
    // it commutes with slerp, Hermite curves and normalization)
    struct QuatMap{
        Quatd a, b;
        Quatd apply(const Quatd& q) const { return a * q * b; }
    };

    void toXYZW(const Quatd& q, std::vector<float>& out){
        out.resize(4);
        out[0] = (float)q.x;
        out[1] = (float)q.y;
        out[2] = (float)q.z;
        out[3] = (float)q.w;
    }

    Quatd fromXYZW(const std::vector<float>& v){
        return Quatd(v[3], v[0], v[1], v[2]);
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

    // a key list keyframe-reduced: keys a linear reconstruction of their neighbours keeps within
    // `tolerance` go (greedy, deterministic)
    std::vector<ClipKey> reduceVectorKeys(const std::vector<ClipKey>& keys, double tolerance){
        if (keys.size() <= 2) return keys;
        std::vector<ClipKey> out;
        out.push_back(keys[0]);
        size_t start = 0;
        while (start + 1 < keys.size()){
            size_t end = start + 1;
            // the farthest key the segment from `start` can reach
            while (end + 1 < keys.size()){
                const size_t candidate = end + 1;
                bool fits = true;
                const ClipKey& a = keys[start];
                const ClipKey& b = keys[candidate];
                const double span = (double)b.time - a.time;
                for (size_t k = start + 1; k < candidate && fits; k++){
                    const double t = span > 0.0 ? ((double)keys[k].time - a.time) / span : 0.0;
                    for (int c = 0; c < 3; c++){
                        const double value = a.value[(size_t)c] + (b.value[(size_t)c] - a.value[(size_t)c]) * t;
                        if (std::fabs(value - keys[k].value[(size_t)c]) > tolerance) fits = false;
                    }
                }
                if (!fits) break;
                end = candidate;
            }
            out.push_back(keys[end]);
            start = end;
        }
        // a constant track is one key
        if (out.size() == 2){
            bool same = true;
            for (int c = 0; c < 3; c++) same = same && std::fabs(out[0].value[(size_t)c] - out[1].value[(size_t)c]) <= tolerance;
            if (same) out.resize(1);
        }
        return out;
    }

    bool isAncestorBone(const Skeleton& skeleton, int ancestor, int bone){
        int guard = 0;
        for (int b = skeleton.bones[(size_t)bone].parent; b >= 0 && guard < 4096; b = skeleton.bones[(size_t)b].parent, guard++){
            if (b == ancestor) return true;
        }
        return false;
    }

    // ---- the source cache ----

    struct SourceEntry{
        std::string key;
        std::string stamp;
        std::shared_ptr<const anim::HumanoidSource> source;
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

}

// ---- the source ----

int anim::humanoidSourceBone(const HumanoidSource& source, const std::string& path){
    if (!source.rig) return -1;
    for (size_t i = 0; i < source.bonePaths.size(); i++){
        if (source.bonePaths[i] == path) return (int)i;
    }
    // a clip made on a parent of the model: its paths have more segments in front
    std::vector<PathSegment> segments;
    if (!parsePath(path, segments) || segments.empty()) return -1;
    for (size_t first = 1; first < segments.size(); first++){
        const std::vector<PathSegment> rest(segments.begin() + (long)first, segments.end());
        const std::string suffix = formatPath(rest);
        for (size_t i = 0; i < source.bonePaths.size(); i++){
            if (source.bonePaths[i] == suffix) return (int)i;
        }
    }
    // a unique bone name (the last segment)
    const PathSegment& last = segments.back();
    if (last.index != 0) return -1;
    int found = -1;
    const Skeleton& skeleton = source.rig->skeleton();
    for (size_t i = 0; i < skeleton.bones.size(); i++){
        if (skeleton.bones[i].name == last.name){
            if (found >= 0) return -1;
            found = (int)i;
        }
    }
    return found;
}

bool anim::buildHumanoidSource(const tinygltf::Model& gltf, const std::string& modelPath, const std::string& sidecarText,
                               HumanoidSource& out, std::string* error){
    out = HumanoidSource();
    out.modelPath = modelPath;
    Skeleton skeleton;
    std::vector<int> boneNodes;
    if (!buildSkeletonFromGltf(gltf, skeleton, out.bonePaths, &out.bridges, &boneNodes)){
        if (error) *error = "the model '" + modelPath + "' has no skeleton (no glTF skin)";
        return false;
    }
    HumanoidOverrides overrides;
    if (!sidecarText.empty()){
        std::vector<std::string> warnings;
        std::string parseError;
        if (!parseHumanoidOverrides(sidecarText, overrides, warnings, &parseError)){
            if (error) *error = humanoidSidecarPath(modelPath) + ": " + parseError;
            return false;
        }
        for (const std::string& warning : warnings) out.notes.push_back(humanoidSidecarPath(modelPath) + ": " + warning);
    }
    std::string resolveError;
    if (!resolveHumanoid(skeleton, overrides, out.humanoid, out.extra, &resolveError, &out.notes)){
        if (error) *error = modelPath + ": not a humanoid: " + resolveError;
        return false;
    }
    auto rig = std::make_shared<PoseRig>();
    if (std::shared_ptr<PoseSkin> skin = buildSkinFromGltf(gltf, skeleton, boneNodes)) rig->setSkin(skin);
    const std::array<float, HUMAN_BONE_COUNT> twist = overrides.twistDegrees();
    std::string buildError;
    if (!rig->build(skeleton, out.humanoid, &buildError, &twist)){
        if (error) *error = modelPath + ": " + buildError;
        return false;
    }
    out.rig = rig;
    out.extraBones.assign(HUMAN_EXTRA_BONE_COUNT, -1);
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        out.extraBones[(size_t)x] = findSkeletonBone(skeleton, out.extra.bones[(size_t)x]);
    }
    out.restRotation.resize(boneNodes.size());
    out.restPosition.resize(boneNodes.size());
    for (size_t i = 0; i < boneNodes.size(); i++){
        Vector3 scale;
        gltfNodeLocal(gltf, boneNodes[i], out.restPosition[i], out.restRotation[i], scale);
    }
    return true;
}

bool anim::HumanoidSourceCache::get(const std::string& modelPath, std::shared_ptr<const HumanoidSource>& out, std::string* error){
    out.reset();
    if (!isModelFilePath(modelPath)){
        if (error) *error = "retarget: '" + modelPath + "' is not a glTF, GLB or FBX model";
        return false;
    }
    const std::string key = MeshSystem::getModelFilenameKey(modelPath);
    std::string stamp;
    if (!humanoidSidecarStamp(modelPath, stamp, error)) return false;
    SourceCacheState& cache = sourceCache();
    uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        for (const SourceEntry& entry : cache.entries){
            if (entry.key == key && entry.stamp == stamp){
                out = entry.source;
                return true;
            }
        }
        epoch = cache.epoch;
    }
    // built without the lock (a parse of the model)
    auto source = std::make_shared<HumanoidSource>();
    bool built = false;
    if (!readModelFile(modelPath, [&](const tinygltf::Model& gltf){
            built = buildHumanoidSource(gltf, modelPath, stamp, *source, error);
            return built;
        }, error)){
        return false;
    }
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.epoch == epoch){
        cache.entries.erase(std::remove_if(cache.entries.begin(), cache.entries.end(),
                                           [&](const SourceEntry& entry){ return entry.key == key; }), cache.entries.end());
        cache.entries.push_back({key, stamp, source});
    }
    out = source;
    return true;
}

void anim::HumanoidSourceCache::drop(const std::string& modelPath){
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

// ---- conversion ----

bool anim::convertToHumanoid(const ClipData& generic, const HumanoidSource& source, ClipData& out,
                             std::vector<ClipWarning>& warnings, std::string* error, HumanoidConversionReport* report){
    out = ClipData();
    if (!source.rig || !source.rig->valid()){
        if (error) *error = "retarget: the source model '" + source.modelPath + "' has no humanoid rig";
        return false;
    }
    ClipData data = generic;
    normalize(data);
    if (data.isHumanoid()){
        out = data;
        return true;
    }
    const PoseRig& rig = *source.rig;
    const Skeleton& skeleton = rig.skeleton();
    const size_t boneCount = skeleton.bones.size();
    HumanoidConversionReport localReport;
    HumanoidConversionReport& rep = report ? *report : localReport;
    rep = HumanoidConversionReport();

    // which bones are slots, extra bones, or between slots
    std::vector<int> extraOfBone(boneCount, -1);
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT && x < (int)source.extraBones.size(); x++){
        if (source.extraBones[(size_t)x] >= 0) extraOfBone[(size_t)source.extraBones[(size_t)x]] = x;
    }
    std::vector<bool> between(boneCount, false);
    for (size_t j = 0; j < boneCount; j++){
        if (rig.slotOfBone((int)j) >= 0 || extraOfBone[j] >= 0) continue;
        for (int s = 0; s < HUMAN_BONE_COUNT && !between[j]; s++){
            const int bone = rig.boneIndex(boneAt(s));
            if (bone >= 0 && isAncestorBone(skeleton, (int)j, bone)) between[j] = true;
        }
    }

    // the tracks per bone
    std::vector<int> slotTrack(HUMAN_BONE_COUNT, -1);
    std::vector<int> extraTrack(HUMAN_EXTRA_BONE_COUNT, -1);
    int hipsTrack = -1;
    std::vector<size_t> copied;
    std::set<std::string> dropped;
    for (size_t i = 0; i < data.tracks.size(); i++){
        const ClipTrackData& track = data.tracks[i];
        const bool transform = track.channel == ClipChannel::Position || track.channel == ClipChannel::Rotation;
        const int bone = transform ? humanoidSourceBone(source, track.path) : -1;
        if (bone < 0){
            copied.push_back(i);
            continue;
        }
        const int slot = rig.slotOfBone(bone);
        const int extra = extraOfBone[(size_t)bone];
        if (track.channel == ClipChannel::Rotation && slot >= 0){
            slotTrack[(size_t)slot] = (int)i;
        }else if (track.channel == ClipChannel::Rotation && extra >= 0){
            extraTrack[(size_t)extra] = (int)i;
        }else if (track.channel == ClipChannel::Position && slot == boneIndex(HumanBone::Hips)){
            hipsTrack = (int)i;
        }else if (slot >= 0 || extra >= 0){
            dropped.insert(track.path + " (translation)");
        }else if (between[(size_t)bone]){
            dropped.insert(track.path);
        }else{
            copied.push_back(i);
        }
    }

    out.frameRate = data.frameRate;
    out.wrap = data.wrap;
    out.length = data.length;
    out.additiveReference = data.additiveReference;
    out.events = data.events;
    out.previewModel = data.previewModel;
    out.source = data.source;
    out.unknownKeys = data.unknownKeys;
    out.humanoid.sourceModel = source.modelPath;
    out.humanoid.humanScale = rig.humanScale();
    for (size_t i : copied){
        out.tracks.push_back(data.tracks[i]);
        rep.genericTracks.push_back(data.tracks[i].path);
    }

    const Quatd characterInv = Quatd(rig.characterRotation()).inverse();
    const Vec3d neutralHips(rig.neutralHipsWorldPosition());
    const double scale = rig.humanScale();
    auto bridgeRotOf = [&](int bone){
        return bone >= 0 && (size_t)bone < source.bridges.size() ? Quatd(bridgeRotation(source.bridges[(size_t)bone])) : Quatd();
    };
    // a rotation track's keys through q -> a * q * b (relative tracks: a * rest * q * b)
    auto mapRotationTrack = [&](const ClipTrackData& from, QuatMap map, int bone, int humanBone){
        ClipTrackData track;
        track.channel = ClipChannel::Humanoid;
        track.humanBone = humanBone;
        track.loop = from.loop;
        if (from.relative && bone >= 0 && (size_t)bone < source.restRotation.size()){
            map.a = map.a * Quatd(source.restRotation[(size_t)bone]);
        }
        for (const ClipKey& key : from.keys){
            ClipKey mapped = key;
            toXYZW(map.apply(fromXYZW(key.value)).normalized(), mapped.value);
            if (key.hasTangents()){
                toXYZW(map.apply(fromXYZW(key.inTangent)), mapped.inTangent);
                toXYZW(map.apply(fromXYZW(key.outTangent)), mapped.outTangent);
            }
            track.keys.push_back(std::move(mapped));
        }
        return track;
    };

    // slots: E = left^-1 * bridge * q * N^-1
    for (int s = 0; s < HUMAN_BONE_COUNT; s++){
        const HumanBone slot = boneAt(s);
        if (!rig.has(slot)){
            rep.unmappedSlots.push_back(humanBoneName(slot));
            continue;
        }
        rep.mappedSlots.push_back(humanBoneName(slot));
        const int bone = rig.boneIndex(slot);
        const Quatd neutral = rig.neutralWorldCanonical(slot);
        const Quatd leftInverse = rig.jointRotationFromLocal(slot, neutral);
        QuatMap map{leftInverse * bridgeRotOf(bone), neutral.inverse()};
        if (slotTrack[(size_t)s] >= 0){
            out.tracks.push_back(mapRotationTrack(data.tracks[(size_t)slotTrack[(size_t)s]], map, bone, s));
        }else{
            ClipTrackData track;
            track.channel = ClipChannel::Humanoid;
            track.humanBone = s;
            ClipKey key;
            toXYZW(rig.jointRotationFromLocal(slot, Quatd(rig.restPose().rotations[(size_t)s])), key.value);
            track.keys.push_back(key);
            out.tracks.push_back(track);
        }
    }

    // extra bones with a rotation track: d = W * rest^-1 * bridge * q * W^-1 (W: the bone's
    // canonical world rotation in the neutral pose)
    const std::vector<Quatd> frames = neutralFrames(rig);
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        const int bone = (size_t)x < source.extraBones.size() ? source.extraBones[(size_t)x] : -1;
        if (bone < 0 || extraTrack[(size_t)x] < 0) continue;
        rep.mappedExtra.push_back(humanExtraBoneName(extraAt(x)));
        const Quatd frame = frames[(size_t)bone];
        const Quatd rest = Quatd(skeleton.bones[(size_t)bone].rotation).normalized();
        QuatMap map{frame * rest.inverse() * bridgeRotOf(bone), frame.inverse()};
        out.tracks.push_back(mapRotationTrack(data.tracks[(size_t)extraTrack[(size_t)x]], map, bone, HUMAN_BONE_COUNT + x));
    }

    // the body offset: C^-1 (hipsParent * bridge * p - neutralHips) / humanScale (affine in p)
    const int hipsBone = rig.boneIndex(HumanBone::Hips);
    const Matrix4 hipsBridge = hipsBone >= 0 && (size_t)hipsBone < source.bridges.size() ? source.bridges[(size_t)hipsBone] : Matrix4();
    auto skeletonHips = [&](const Vector3& transformLocal){ return hipsBridge * transformLocal; };
    auto bodyOffset = [&](const Vector3& skeletonLocal){
        HumanPose pose = rig.restPose();
        pose.hipsPosition = skeletonLocal;
        const Vec3d offset = characterInv * (Vec3d(rig.hipsWorldPosition(pose)) - neutralHips) / scale;
        return offset;
    };
    {
        ClipTrackData track;
        track.channel = ClipChannel::HumanoidPosition;
        track.humanBone = HUMANOID_POSITION_HIPS;
        auto store = [](const Vec3d& v, std::vector<float>& row){ row = {(float)v.x, (float)v.y, (float)v.z}; };
        if (hipsTrack >= 0){
            const ClipTrackData& from = data.tracks[(size_t)hipsTrack];
            track.loop = from.loop;
            const Vector3 rest = (size_t)hipsBone < source.restPosition.size() ? source.restPosition[(size_t)hipsBone] : Vector3(0, 0, 0);
            const Vec3d origin = bodyOffset(skeletonHips(Vector3(0, 0, 0)));
            for (const ClipKey& key : from.keys){
                ClipKey mapped = key;
                Vector3 value(key.value[0], key.value[1], key.value[2]);
                if (from.relative) value = rest + value;
                store(bodyOffset(skeletonHips(value)), mapped.value);
                if (key.hasTangents()){
                    // the linear part only
                    store(bodyOffset(skeletonHips(Vector3(key.inTangent[0], key.inTangent[1], key.inTangent[2]))) - origin, mapped.inTangent);
                    store(bodyOffset(skeletonHips(Vector3(key.outTangent[0], key.outTangent[1], key.outTangent[2]))) - origin, mapped.outTangent);
                }
                track.keys.push_back(std::move(mapped));
            }
        }else{
            ClipKey key;
            store(bodyOffset(rig.restPose().hipsPosition), key.value);
            track.keys.push_back(key);
        }
        out.tracks.push_back(track);
    }

    // the foot goals: forward kinematics of the legs, sampled
    if (rig.has(HumanBone::LeftFoot) || rig.has(HumanBone::RightFoot)){
        std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data);
        std::set<float> times;
        const HumanBone legSlots[] = {HumanBone::Hips, HumanBone::LeftUpperLeg, HumanBone::LeftLowerLeg, HumanBone::LeftFoot,
                                      HumanBone::RightUpperLeg, HumanBone::RightLowerLeg, HumanBone::RightFoot};
        std::vector<int> legTracks;
        for (HumanBone slot : legSlots){
            if (slotTrack[(size_t)boneIndex(slot)] >= 0) legTracks.push_back(slotTrack[(size_t)boneIndex(slot)]);
        }
        if (hipsTrack >= 0) legTracks.push_back(hipsTrack);
        for (int t : legTracks){
            for (const ClipKey& key : data.tracks[(size_t)t].keys) times.insert(key.time);
        }
        const float length = data.effectiveLength();
        const double rate = std::max(30.0, (double)data.frameRate);
        const int frames = (int)std::ceil(length * rate - 1e-6);
        for (int k = 0; k <= frames; k++){
            const float t = (float)std::min((double)length, k / rate);
            times.insert((float)(std::round((double)t * 1e6) / 1e6));
        }
        times.insert(0.0f);

        ClipTrackData feet[2];
        for (int side = 0; side < 2; side++){
            feet[side].channel = ClipChannel::HumanoidPosition;
            feet[side].humanBone = side == 0 ? HUMANOID_POSITION_LEFT_FOOT : HUMANOID_POSITION_RIGHT_FOOT;
        }
        const std::vector<AnimationClip::Track>& tracks = clip->getTracks();
        TrackSample sample;
        std::vector<Quatd> worldRot;
        std::vector<Affine3d> world;
        for (float t : times){
            if (t > length + 1e-6f) continue;
            HumanPose pose = rig.restPose();
            for (int s = 0; s < HUMAN_BONE_COUNT; s++){
                const int i = slotTrack[(size_t)s];
                if (i < 0 || !rig.has(boneAt(s))) continue;
                if (!sampleTrack(tracks[(size_t)i], t, sample)) continue;
                const int bone = rig.boneIndex(boneAt(s));
                Quatd q(sample.rotation);
                if (data.tracks[(size_t)i].relative && (size_t)bone < source.restRotation.size()) q = Quatd(source.restRotation[(size_t)bone]) * q;
                pose.rotations[(size_t)s] = (bridgeRotOf(bone) * q).normalized().toQuaternion();
            }
            if (hipsTrack >= 0 && sampleTrack(tracks[(size_t)hipsTrack], t, sample)){
                Vector3 value = sample.vector;
                if (data.tracks[(size_t)hipsTrack].relative && (size_t)hipsBone < source.restPosition.size()) value = source.restPosition[(size_t)hipsBone] + value;
                pose.hipsPosition = skeletonHips(value);
            }
            rig.computeWorldPrecise(pose, worldRot, world);
            for (int side = 0; side < 2; side++){
                const int foot = rig.boneIndex(side == 0 ? HumanBone::LeftFoot : HumanBone::RightFoot);
                if (foot < 0) continue;
                const Vec3d goal = characterInv * (world[(size_t)foot].t - neutralHips) / scale;
                ClipKey key;
                key.time = t;
                key.value = {(float)goal.x, (float)goal.y, (float)goal.z};
                feet[side].keys.push_back(key);
            }
        }
        for (int side = 0; side < 2; side++){
            if (feet[side].keys.empty()) continue;
            feet[side].keys = reduceVectorKeys(feet[side].keys, 1e-4);
            out.tracks.push_back(feet[side]);
        }
    }

    if (!dropped.empty()){
        std::string list;
        for (const std::string& name : dropped){
            list += (list.empty() ? "" : ", ") + name;
            rep.droppedBones.push_back(name);
        }
        warnings.push_back({"tracks", "not retargeted (bones between humanoid slots keep their rest; humanoid clips carry the "
                                      "rotations of mapped bones and the hips translation): " + list});
    }
    normalize(out, &warnings);
    return true;
}

// ---- realisation ----

void anim::mirrorHumanoidSample(const HumanoidSample& in, HumanoidSample& out){
    auto mirror = [](const Quaternion& q){ return Quaternion(q.w, q.x, -q.y, -q.z); };
    out.hasE.reset();
    out.hasExtra.reset();
    for (int s = 0; s < HUMAN_BONE_COUNT; s++){
        const int m = boneIndex(humanBoneInfo(boneAt(s)).mirror);
        out.hasE[(size_t)m] = in.hasE[(size_t)s];
        out.E[(size_t)m] = mirror(in.E[(size_t)s]);
    }
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        const int m = extraIndex(humanExtraBoneInfo(extraAt(x)).mirror);
        out.hasExtra[(size_t)m] = in.hasExtra[(size_t)x];
        out.extra[(size_t)m] = mirror(in.extra[(size_t)x]);
    }
    out.hasHips = in.hasHips;
    out.hips = Vector3(-in.hips.x, in.hips.y, in.hips.z);
    for (int side = 0; side < 2; side++){
        out.hasFeet[1 - side] = in.hasFeet[side];
        out.feet[1 - side] = Vector3(-in.feet[side].x, in.feet[side].y, in.feet[side].z);
    }
}

bool anim::buildHumanoidTarget(const ModelRigInfo& info, const std::vector<Entity>& boneEntities, const std::vector<Matrix4>& bridges,
                               Entity model, const std::string& modelPath, HumanoidTarget& out){
    out = HumanoidTarget();
    if (!info.rig || !info.rig->valid()) return false;
    const PoseRig& rig = *info.rig;
    out.rig = info.rig;
    out.extra = info.extra;
    out.model = model;
    out.modelPath = modelPath;
    for (int s = 0; s < HUMAN_BONE_COUNT; s++){
        const HumanBone slot = boneAt(s);
        const int bone = rig.boneIndex(slot);
        out.bridgeRotationInverse[(size_t)s] = Quaternion();
        if (bone < 0) continue;
        out.slotEntities[(size_t)s] = (size_t)bone < boneEntities.size() ? boneEntities[(size_t)bone] : NULL_ENTITY;
        if ((size_t)bone < bridges.size()){
            out.bridgeRotationInverse[(size_t)s] = bridgeRotation(bridges[(size_t)bone]).inverse();
            if (slot == HumanBone::Hips) out.hipsBridgeInverse = bridges[(size_t)bone].inverse();
        }
        // the clip slots this slot stands for: up the anatomical chain to its effective parent
        const HumanBone parent = rig.effectiveParent(slot);
        std::vector<uint8_t> chain;
        for (HumanBone c = slot; c != HumanBone::Count && c != parent; c = humanBoneInfo(c).parent){
            chain.insert(chain.begin(), (uint8_t)boneIndex(c));
        }
        out.chains[(size_t)s] = chain;
    }
    const std::vector<Quatd> frames = neutralFrames(rig);
    const Skeleton& skeleton = rig.skeleton();
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        out.extraBridgeRotationInverse[(size_t)x] = Quaternion();
        const int bone = findSkeletonBone(skeleton, info.extra.bones[(size_t)x]);
        if (bone < 0) continue;
        out.extraEntities[(size_t)x] = (size_t)bone < boneEntities.size() ? boneEntities[(size_t)bone] : NULL_ENTITY;
        if ((size_t)bone < bridges.size()) out.extraBridgeRotationInverse[(size_t)x] = bridgeRotation(bridges[(size_t)bone]).inverse();
        out.extraRest[(size_t)x] = Quatd(skeleton.bones[(size_t)bone].rotation).normalized().toQuaternion();
        out.extraFrame[(size_t)x] = frames[(size_t)bone].toQuaternion();
    }
    return true;
}

void anim::realiseHumanoid(const HumanoidTarget& target, const HumanoidSample& sample, bool mirror, bool footIK,
                           HumanPose& out, std::array<Quaternion, HUMAN_EXTRA_BONE_COUNT>& extraLocal){
    if (!target.rig || !target.rig->valid()) return;
    const PoseRig& rig = *target.rig;
    HumanoidSample mirrored;
    const HumanoidSample* s = &sample;
    if (mirror){
        mirrorHumanoidSample(sample, mirrored);
        s = &mirrored;
    }
    out.hipsPosition = rig.neutralPose().hipsPosition;
    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
        const HumanBone slot = boneAt(b);
        if (!rig.has(slot)){
            out.rotations[(size_t)b] = Quaternion();
            continue;
        }
        // E_t(b): the clip's E along the chain (missing slots: identity), parent first
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
    if (s->hasHips){
        rig.setHipsWorldPosition(out, model(s->hips));
    }
    if (footIK){
        for (int side = 0; side < 2; side++){
            if (!s->hasFeet[side]) continue;
            solveLimbIK(rig, out, side == 0 ? Limb::LeftLeg : Limb::RightLeg, model(s->feet[side]), true);
        }
    }
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        if (target.extraEntities[(size_t)x] == NULL_ENTITY || !s->hasExtra[(size_t)x]) continue;
        const Quatd frame(target.extraFrame[(size_t)x]);
        extraLocal[(size_t)x] = (Quatd(target.extraRest[(size_t)x]) * frame.inverse() * Quatd(s->extra[(size_t)x]) * frame).normalized().toQuaternion();
    }
}

// ---- the target's current pose ----

bool anim::currentHumanoidPose(EntityRegistry* registry, const HumanoidTarget& target, HumanPose& out){
    if (!registry || !target.rig || !target.rig->valid()) return false;
    const PoseRig& rig = *target.rig;
    out = rig.restPose();
    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
        const Entity entity = target.slotEntities[(size_t)b];
        if (entity == NULL_ENTITY || !rig.has(boneAt(b))) continue;
        const Transform* transform = registry->findComponent<Transform>(entity);
        if (!transform) return false;
        out.rotations[(size_t)b] = (target.bridgeRotationInverse[(size_t)b].inverse() * transform->rotation).normalized();
        if (b == boneIndex(HumanBone::Hips)) out.hipsPosition = target.hipsBridgeInverse.inverse() * transform->position;
    }
    return true;
}

bool anim::readHumanoidValue(EntityRegistry* registry, const HumanoidTarget& target, ClipChannel channel, int humanBone,
                             std::vector<float>& out){
    out.clear();
    if (!registry || !target.rig || !target.rig->valid()) return false;
    const PoseRig& rig = *target.rig;
    if (channel == ClipChannel::Humanoid && humanBone >= 0 && humanBone < HUMAN_BONE_COUNT){
        const Entity entity = target.slotEntities[(size_t)humanBone];
        const Transform* transform = entity != NULL_ENTITY ? registry->findComponent<Transform>(entity) : nullptr;
        if (!transform) return false;
        const Quatd local = Quatd(target.bridgeRotationInverse[(size_t)humanBone].inverse() * transform->rotation).normalized();
        toXYZW(rig.jointRotationFromLocal(boneAt(humanBone), local), out);
        return true;
    }
    if (channel == ClipChannel::Humanoid && humanBone >= HUMAN_BONE_COUNT && humanBone < HUMANOID_TRACK_BONE_COUNT){
        const int x = humanBone - HUMAN_BONE_COUNT;
        const Entity entity = target.extraEntities[(size_t)x];
        const Transform* transform = entity != NULL_ENTITY ? registry->findComponent<Transform>(entity) : nullptr;
        if (!transform) return false;
        const Quatd local(target.extraBridgeRotationInverse[(size_t)x].inverse() * transform->rotation);
        const Quatd frame(target.extraFrame[(size_t)x]);
        toXYZW((frame * Quatd(target.extraRest[(size_t)x]).inverse() * local * frame.inverse()).normalized(), out);
        return true;
    }
    if (channel != ClipChannel::HumanoidPosition || humanBone < 0 || humanBone >= HUMANOID_POSITION_COUNT) return false;
    HumanPose pose;
    if (!currentHumanoidPose(registry, target, pose)) return false;
    const Quatd characterInv = Quatd(rig.characterRotation()).inverse();
    const Vec3d neutralHips(rig.neutralHipsWorldPosition());
    Vec3d model;
    if (humanBone == HUMANOID_POSITION_HIPS){
        model = Vec3d(rig.hipsWorldPosition(pose));
    }else{
        const int foot = rig.boneIndex(humanBone == HUMANOID_POSITION_LEFT_FOOT ? HumanBone::LeftFoot : HumanBone::RightFoot);
        if (foot < 0) return false;
        std::vector<Quatd> rot;
        std::vector<Affine3d> world;
        rig.computeWorldPrecise(pose, rot, world);
        model = world[(size_t)foot].t;
    }
    const Vec3d value = characterInv * (model - neutralHips) / rig.humanScale();
    out = {(float)value.x, (float)value.y, (float)value.z};
    return true;
}

// ---- bindings ----

bool anim::bindHumanoidClip(const AnimationClip& clip, const std::shared_ptr<const HumanoidTarget>& target, HumanoidBinding& out){
    out = HumanoidBinding();
    if (!clip.isHumanoid() || !target || !target->rig || !target->rig->valid()) return false;
    const PoseRig& rig = *target->rig;
    out.target = target;
    const std::vector<AnimationClip::Track>& tracks = clip.getTracks();
    out.trackField.assign(tracks.size(), -1);
    bool slots = false;
    bool hips = false;
    std::bitset<HUMAN_BONE_COUNT> clipSlots;
    std::bitset<HUMAN_EXTRA_BONE_COUNT> clipExtra;
    for (size_t i = 0; i < tracks.size(); i++){
        const AnimationClip::Track& track = tracks[i];
        if (!track.isSampled()) continue;
        if (track.channel == ClipChannel::Humanoid && track.humanBone >= 0 && track.humanBone < HUMANOID_TRACK_BONE_COUNT){
            out.trackField[i] = (int16_t)track.humanBone;
            if (track.humanBone < HUMAN_BONE_COUNT){
                slots = true;
                clipSlots[(size_t)track.humanBone] = true;
            }else{
                clipExtra[(size_t)(track.humanBone - HUMAN_BONE_COUNT)] = true;
            }
        }else if (track.channel == ClipChannel::HumanoidPosition && track.humanBone >= 0 && track.humanBone < HUMANOID_POSITION_COUNT){
            out.trackField[i] = (int16_t)(FIELD_POSITION + track.humanBone);
            hips = hips || track.humanBone == HUMANOID_POSITION_HIPS;
        }
    }
    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
        if (clipSlots[(size_t)b] && !rig.has(boneAt(b))) out.missingOnTarget.push_back(humanBoneName(boneAt(b)));
    }
    if (slots){
        for (int b = 0; b < HUMAN_BONE_COUNT; b++){
            const Entity entity = target->slotEntities[(size_t)b];
            if (!rig.has(boneAt(b)) || entity == NULL_ENTITY) continue;
            HumanoidOutput output;
            output.entity = entity;
            output.channel = ClipChannel::Rotation;
            output.kind = HumanoidOutput::Kind::Slot;
            output.index = (uint8_t)b;
            out.outputs.push_back(output);
            const std::vector<uint8_t>& chain = target->chains[(size_t)b];
            for (size_t c = 0; c + 1 < chain.size(); c++){
                if (clipSlots[chain[c]]){
                    out.folded.push_back(std::string(humanBoneName(boneAt(chain[c]))) + " folded into " + humanBoneName(boneAt(b)));
                }
            }
        }
    }
    if (hips && target->slotEntities[(size_t)boneIndex(HumanBone::Hips)] != NULL_ENTITY){
        HumanoidOutput output;
        output.entity = target->slotEntities[(size_t)boneIndex(HumanBone::Hips)];
        output.channel = ClipChannel::Position;
        output.kind = HumanoidOutput::Kind::Hips;
        out.outputs.push_back(output);
    }
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        if (!clipExtra[(size_t)x]) continue;
        if (target->extraEntities[(size_t)x] == NULL_ENTITY){
            out.missingOnTarget.push_back(humanExtraBoneName(extraAt(x)));
            continue;
        }
        HumanoidOutput output;
        output.entity = target->extraEntities[(size_t)x];
        output.channel = ClipChannel::Rotation;
        output.kind = HumanoidOutput::Kind::Extra;
        output.index = (uint8_t)x;
        out.outputs.push_back(output);
    }
    out.pose = rig.neutralPose();
    return true;
}

void anim::sampleHumanoidBinding(const AnimationClip& clip, HumanoidBinding& binding, float clipTime, std::vector<KeyCursor>* cursors){
    if (!binding.target) return;
    HumanoidSample& sample = binding.sample;
    sample.hasE.reset();
    sample.hasExtra.reset();
    sample.hasHips = false;
    sample.hasFeet[0] = sample.hasFeet[1] = false;
    const std::vector<AnimationClip::Track>& tracks = clip.getTracks();
    TrackSample value;
    for (size_t i = 0; i < tracks.size() && i < binding.trackField.size(); i++){
        const int field = binding.trackField[i];
        if (field < 0) continue;
        KeyCursor* cursor = cursors && i < cursors->size() ? &(*cursors)[i] : nullptr;
        if (!sampleTrack(tracks[i], clipTime, value, cursor)) continue;
        if (field < HUMAN_BONE_COUNT){
            sample.E[(size_t)field] = value.rotation;
            sample.hasE[(size_t)field] = true;
        }else if (field < HUMANOID_TRACK_BONE_COUNT){
            sample.extra[(size_t)(field - HUMAN_BONE_COUNT)] = value.rotation;
            sample.hasExtra[(size_t)(field - HUMAN_BONE_COUNT)] = true;
        }else if (field == FIELD_POSITION + HUMANOID_POSITION_HIPS){
            sample.hips = value.vector;
            sample.hasHips = true;
        }else{
            const int side = field - FIELD_POSITION - HUMANOID_POSITION_LEFT_FOOT;
            if (side == 0 || side == 1){
                sample.feet[side] = value.vector;
                sample.hasFeet[side] = true;
            }
        }
    }
    const HumanoidTarget& target = *binding.target;
    realiseHumanoid(target, sample, binding.mirror, binding.footIK, binding.pose, binding.extraLocal);
    for (HumanoidOutput& output : binding.outputs){
        switch (output.kind){
            case HumanoidOutput::Kind::Slot:
                output.rotation = target.bridgeRotationInverse[output.index] * binding.pose.rotations[output.index];
                break;
            case HumanoidOutput::Kind::Extra:
                output.rotation = target.extraBridgeRotationInverse[output.index] * binding.extraLocal[output.index];
                break;
            case HumanoidOutput::Kind::Hips:
                output.position = target.hipsBridgeInverse * binding.pose.hipsPosition;
                break;
        }
    }
}
