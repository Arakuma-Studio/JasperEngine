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

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::pose;

namespace{

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

    // every key of a position / rotation / scale track at `rest` (an offset of zero, the identity
    // or a factor of one when relative): a dropped track like that loses nothing worth a warning
    bool trackAtRest(const ClipTrackData& track, const Vector3& restPosition, const Quaternion& restRotation, const Vector3& restScale,
                     double positionTolerance){
        for (const ClipKey& key : track.keys){
            if (key.hasTangents()){
                for (float v : key.inTangent) if (std::fabs(v) > 1e-6f) return false;
                for (float v : key.outTangent) if (std::fabs(v) > 1e-6f) return false;
            }
            if (track.channel == ClipChannel::Rotation){
                if (key.value.size() < 4) return false;
                const Quatd rest = track.relative ? Quatd() : Quatd(restRotation);
                if (std::fabs(fromXYZW(key.value).normalized().dot(rest.normalized())) < 1.0 - 1e-9) return false;
                continue;
            }
            if (key.value.size() < 3) return false;
            const Vec3d value(key.value[0], key.value[1], key.value[2]);
            const bool position = track.channel == ClipChannel::Position;
            const Vec3d rest = position ? (track.relative ? Vec3d(0, 0, 0) : Vec3d(restPosition))
                                        : (track.relative ? Vec3d(1, 1, 1) : Vec3d(restScale));
            if ((value - rest).length() > (position ? positionTolerance : 1e-6)) return false;
        }
        return true;
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
    HumanoidConversionReport localReport;
    HumanoidConversionReport& rep = report ? *report : localReport;
    rep = HumanoidConversionReport();

    // the tracks per bone, by the one path's rule (RetargetSource::roleOf: what play-time
    // retargeting reads, what it leaves to the path binding, what it drops)
    std::vector<int> slotTrack(HUMAN_BONE_COUNT, -1);
    std::vector<int> extraTrack(HUMAN_EXTRA_BONE_COUNT, -1);
    int hipsTrack = -1;
    std::vector<size_t> copied;
    std::set<std::string> dropped;
    for (size_t i = 0; i < data.tracks.size(); i++){
        const ClipTrackData& track = data.tracks[i];
        const int bone = source.boneOfPath(track.path);
        switch (source.roleOf(track.channel, bone)){
            case RetargetTrackRole::Generic:
                copied.push_back(i);
                break;
            case RetargetTrackRole::Slot:
                slotTrack[(size_t)rig.slotOfBone(bone)] = (int)i;
                break;
            case RetargetTrackRole::Extra:
                for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
                    if (source.extraBones[(size_t)x] == bone) extraTrack[(size_t)x] = (int)i;
                }
                break;
            case RetargetTrackRole::Hips:
                hipsTrack = (int)i;
                break;
            case RetargetTrackRole::Dropped: {
                // a dropped track whose keys all sit at the bone's rest loses nothing: no warning
                const Vector3 restPosition = (size_t)bone < source.restPosition.size() ? source.restPosition[(size_t)bone] : Vector3(0, 0, 0);
                const Quaternion restRotation = (size_t)bone < source.restRotation.size() ? source.restRotation[(size_t)bone] : Quaternion();
                // the node's own rest scale (the skeleton's folds non-bone parents in, as Mixamo's 0.01 Armature)
                const Vector3 restScale = (size_t)bone < source.restScale.size() ? source.restScale[(size_t)bone] : Vector3(1, 1, 1);
                if (!trackAtRest(track, restPosition, restRotation, restScale, 1e-5 * rig.humanScale())){
                    const bool mapped = rig.slotOfBone(bone) >= 0 || (size_t)bone >= source.between.size() || !source.between[(size_t)bone];
                    dropped.insert(track.path + (track.channel == ClipChannel::Scale ? " (scale)"
                                                 : mapped ? " (translation)" : ""));
                }
                break;
            }
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

    // a rotation track's keys through q -> a * q * b (relative tracks: a * rest * q * b): the
    // source's maps, key for key
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

    // slots: E = slotA * q * slotB; a slot without a track holds its rest E
    for (int s = 0; s < HUMAN_BONE_COUNT; s++){
        const HumanBone slot = boneAt(s);
        if (!rig.has(slot)){
            rep.unmappedSlots.push_back(humanBoneName(slot));
            continue;
        }
        rep.mappedSlots.push_back(humanBoneName(slot));
        const int bone = rig.boneIndex(slot);
        if (slotTrack[(size_t)s] >= 0){
            out.tracks.push_back(mapRotationTrack(data.tracks[(size_t)slotTrack[(size_t)s]],
                                                  QuatMap{source.slotA[(size_t)s], source.slotB[(size_t)s]}, bone, s));
        }else{
            ClipTrackData track;
            track.channel = ClipChannel::Humanoid;
            track.humanBone = s;
            ClipKey key;
            toXYZW(Quatd(source.restE[(size_t)s]), key.value);
            track.keys.push_back(key);
            out.tracks.push_back(track);
        }
    }

    // extra bones with a rotation track: d = extraA * q * extraB
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        const int bone = source.extraBones[(size_t)x];
        if (bone < 0 || extraTrack[(size_t)x] < 0) continue;
        rep.mappedExtra.push_back(humanExtraBoneName(extraAt(x)));
        out.tracks.push_back(mapRotationTrack(data.tracks[(size_t)extraTrack[(size_t)x]],
                                              QuatMap{source.extraA[(size_t)x], source.extraB[(size_t)x]}, bone, HUMAN_BONE_COUNT + x));
    }

    // the body offset: source.bodyOffset (affine in the hips' Transform-local position)
    const int hipsBone = rig.boneIndex(HumanBone::Hips);
    {
        ClipTrackData track;
        track.channel = ClipChannel::HumanoidPosition;
        track.humanBone = HUMANOID_POSITION_HIPS;
        auto store = [](const Vec3d& v, std::vector<float>& row){ row = {(float)v.x, (float)v.y, (float)v.z}; };
        auto linear = [&](const std::vector<float>& t){
            return source.hipsLinear[0] * t[0] + source.hipsLinear[1] * t[1] + source.hipsLinear[2] * t[2];
        };
        if (hipsTrack >= 0){
            const ClipTrackData& from = data.tracks[(size_t)hipsTrack];
            track.loop = from.loop;
            const Vector3 rest = (size_t)hipsBone < source.restPosition.size() ? source.restPosition[(size_t)hipsBone] : Vector3(0, 0, 0);
            for (const ClipKey& key : from.keys){
                ClipKey mapped = key;
                Vector3 value(key.value[0], key.value[1], key.value[2]);
                if (from.relative) value = rest + value;
                store(source.bodyOffset(value), mapped.value);
                if (key.hasTangents()){
                    // the linear part only
                    store(linear(key.inTangent), mapped.inTangent);
                    store(linear(key.outTangent), mapped.outTangent);
                }
                track.keys.push_back(std::move(mapped));
            }
        }else{
            ClipKey key;
            store(Vec3d(source.restHips), key.value);
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
        for (float t : times){
            if (t > length + 1e-6f) continue;
            // the source's skeleton pose at t, its foot goals (the one path's sourceFootGoals)
            HumanPose pose = rig.restPose();
            for (int s = 0; s < HUMAN_BONE_COUNT; s++){
                const int i = slotTrack[(size_t)s];
                if (i < 0 || !rig.has(boneAt(s))) continue;
                if (!sampleTrack(tracks[(size_t)i], t, sample)) continue;
                const int bone = rig.boneIndex(boneAt(s));
                Quatd q(sample.rotation);
                if (data.tracks[(size_t)i].relative && (size_t)bone < source.restRotation.size()) q = Quatd(source.restRotation[(size_t)bone]) * q;
                pose.rotations[(size_t)s] = (Quatd(source.slotBridge[(size_t)s]) * q).normalized().toQuaternion();
            }
            if (hipsTrack >= 0 && sampleTrack(tracks[(size_t)hipsTrack], t, sample)){
                Vector3 value = sample.vector;
                if (data.tracks[(size_t)hipsTrack].relative && (size_t)hipsBone < source.restPosition.size()) value = source.restPosition[(size_t)hipsBone] + value;
                pose.hipsPosition = source.hipsBridge * value;
            }
            Vector3 goals[2];
            bool has[2];
            sourceFootGoals(source, pose, goals, has);
            for (int side = 0; side < 2; side++){
                if (!has[side]) continue;
                ClipKey key;
                key.time = t;
                key.value = {goals[side].x, goals[side].y, goals[side].z};
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
                                      "rotations of mapped bones and the hips translation, not their scale): " + list});
    }
    normalize(out, &warnings);
    return true;
}

// ---- realisation ----

bool anim::buildHumanoidTarget(const ModelRigInfo& info, const std::vector<Entity>& boneEntities, const std::vector<Matrix4>& bridges,
                               Entity model, const std::string& modelPath, HumanoidTarget& out){
    out = HumanoidTarget();
    if (!info.rig || !info.rig->valid()) return false;
    // the rig side (extra frames, fold chains): the one path's RetargetRig
    out.RetargetRig::build(info.rig, info.extra);
    const PoseRig& rig = *info.rig;
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
    }
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        out.extraBridgeRotationInverse[(size_t)x] = Quaternion();
        const int bone = out.extraBones[(size_t)x];
        if (bone < 0) continue;
        out.extraEntities[(size_t)x] = (size_t)bone < boneEntities.size() ? boneEntities[(size_t)bone] : NULL_ENTITY;
        if ((size_t)bone < bridges.size()) out.extraBridgeRotationInverse[(size_t)x] = bridgeRotation(bridges[(size_t)bone]).inverse();
    }
    return true;
}

std::shared_ptr<const AnimationClip> anim::humanoidClipOf(const std::shared_ptr<const AnimationClip>& clip, const std::string& reference,
                                                         std::string* error){
    if (!clip) return nullptr;
    if (clip->isHumanoid()) return clip;
    std::shared_ptr<const HumanoidSource> source = clipRetargetSource(*clip, reference, error);
    if (!source) return nullptr;
    ClipData converted;
    std::vector<ClipWarning> warnings = clip->getWarnings();
    if (!convertToHumanoid(clip->toData(), *source, converted, warnings, error)) return nullptr;
    return AnimationClip::build(converted, warnings, nullptr);
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

bool anim::bindHumanoidClip(const AnimationClip& clip, const std::shared_ptr<const HumanoidTarget>& target, HumanoidBinding& out,
                            const std::shared_ptr<const HumanoidSource>& source){
    out = HumanoidBinding();
    if (!target || !target->valid()) return false;
    if (!clip.isHumanoid() && !source) return false;
    if (!out.reader.bind(clip, clip.isHumanoid() ? nullptr : source)){
        out = HumanoidBinding();
        return false;
    }
    const PoseRig& rig = *target->rig;
    out.target = target;
    const RetargetBinding& reader = out.reader;
    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
        if (reader.slots[(size_t)b] && !rig.has(boneAt(b))) out.missingOnTarget.push_back(humanBoneName(boneAt(b)));
    }
    if (reader.slots.any()){
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
                if (reader.slots[chain[c]]){
                    out.folded.push_back(std::string(humanBoneName(boneAt(chain[c]))) + " folded into " + humanBoneName(boneAt(b)));
                }
            }
        }
    }
    if (reader.hips && target->slotEntities[(size_t)boneIndex(HumanBone::Hips)] != NULL_ENTITY){
        HumanoidOutput output;
        output.entity = target->slotEntities[(size_t)boneIndex(HumanBone::Hips)];
        output.channel = ClipChannel::Position;
        output.kind = HumanoidOutput::Kind::Hips;
        out.outputs.push_back(output);
    }
    // the clip's extra bones and their mirror counterparts (Mirror can turn on at any time), each
    // live only while the (mirrored) sample has a value for it: a right-hand-only curl leaves the
    // left hand to the other layers and clips, mirrored it drives the left hand and leaves the
    // right one alone
    std::bitset<HUMAN_EXTRA_BONE_COUNT> driven;
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        if (!reader.extras[(size_t)x]) continue;
        driven[(size_t)x] = true;
        driven[(size_t)extraIndex(humanExtraBoneInfo(extraAt(x)).mirror)] = true;
    }
    for (int x = 0; x < HUMAN_EXTRA_BONE_COUNT; x++){
        if (!driven[(size_t)x]) continue;
        if (target->extraEntities[(size_t)x] == NULL_ENTITY){
            if (reader.extras[(size_t)x]) out.missingOnTarget.push_back(humanExtraBoneName(extraAt(x)));
            continue;
        }
        HumanoidOutput output;
        output.entity = target->extraEntities[(size_t)x];
        output.channel = ClipChannel::Rotation;
        output.kind = HumanoidOutput::Kind::Extra;
        output.index = (uint8_t)x;
        output.liveUnmirrored = reader.extras[(size_t)x];
        output.liveMirrored = reader.extras[(size_t)extraIndex(humanExtraBoneInfo(extraAt(x)).mirror)];
        out.outputs.push_back(output);
    }
    out.pose = rig.neutralPose();
    return true;
}

void anim::sampleHumanoidBinding(const AnimationClip& clip, HumanoidBinding& binding, float clipTime, std::vector<KeyCursor>* cursors){
    if (!binding.target) return;
    KeyCursor* cursor = cursors && cursors->size() >= clip.getTracks().size() ? cursors->data() : nullptr;
    binding.reader.sample(clip, clipTime, cursor, binding.footIK, binding.sample);
    const HumanoidTarget& target = *binding.target;
    realiseHumanoid(target, binding.sample, binding.mirror, binding.footIK, binding.pose, binding.extraLocal);
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
