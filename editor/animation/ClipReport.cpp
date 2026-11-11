// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipReport.h"

#include "animation/AnimatorAssetIO.h"
#include "animation/AnimatorPose.h"
#include "animation/AnimatorRetarget.h"
#include "animation/KeyframeSampling.h"
#include "animation/ModelSubClips.h"
#include "io/FileData.h"
#include "pool/AnimationClipPool.h"
#include "pose/HumanoidMapIO.h"
#include "pose/ModelRig.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

using namespace jasperengine;

namespace{

    bool same(float a, float b){ return a == b; }
    bool same(const Vector3& a, const Vector3& b){ return a.x == b.x && a.y == b.y && a.z == b.z; }
    bool same(const Quaternion& a, const Quaternion& b){ return a.w == b.w && a.x == b.x && a.y == b.y && a.z == b.z; }

    bool isZero(float value){ return value == 0.0f; }
    bool isZero(const Vector3& value){ return value.x == 0.0f && value.y == 0.0f && value.z == 0.0f; }
    bool isZero(const Quaternion& value){ return value.w == 0.0f && value.x == 0.0f && value.y == 0.0f && value.z == 0.0f; }

    // every row of `width` values equals the first; tangents, when there are any, are all zero
    template<class T>
    bool holdsStill(const T* values, const T* in, const T* out, size_t keys, size_t width){
        for (size_t i = 0; i < keys * width; i++){
            if (!same(values[i], values[i % width])) return false;
            if (in && !(isZero(in[i]) && isZero(out[i]))) return false;
        }
        return true;
    }

    // Exact: near-constant is the reducer's decision, with a measured error, not a fact to report
    bool isConstant(const AnimationClip::Track& track){
        if (track.keyCount < 2) return false;
        if (track.rotations) return holdsStill(track.rotations, track.inRotations, track.outRotations, track.keyCount, track.width);
        if (track.vectors) return holdsStill(track.vectors, track.inVectors, track.outVectors, track.keyCount, track.width);
        if (track.weights) return holdsStill(track.weights, track.inWeights, track.outWeights, track.keyCount, track.width);
        return false;
    }

    std::optional<uint64_t> fileSizeOf(const std::string& reference){
        if (reference.empty() || anim::isSubClipReference(reference)) return std::nullopt;
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(editor::FileUtils::pathFromUtf8(FileData::getSystemPath(reference)), error);
        if (error) return std::nullopt;
        return (uint64_t)size;
    }

    std::string withThousands(uint64_t value){
        std::string digits = std::to_string(value);
        for (size_t at = digits.size(); at > 3; at -= 3) digits.insert(at - 3, ",");
        return digits;
    }

    // ---- posing on a rig ----

    struct Point{
        double x = 0.0, y = 0.0, z = 0.0;
    };

    // One clip posed on a rig the way it plays: sampleTrack per track, the relative rule of
    // AnimatorRig, then poseToRigSpace. A bone with no track holds its rest.
    class RigPoser{
    public:
        RigPoser(const AnimationClip& clip, const anim::ClipRig& rig): parents(rig.parents.data()){
            const size_t bones = rig.size();
            rest.resize(bones, 0);
            local.resize(bones, 0);
            for (size_t b = 0; b < bones; b++){
                rest.setPosition(b, rig.restPositions[b]);
                rest.setRotation(b, rig.restRotations[b]);
                rest.setScale(b, rig.restScales[b]);
            }
            world.resize(bones);
            positions.resize(bones);
            rotations.resize(bones);
            scales.resize(bones);
            for (const AnimationClip::Track& track : clip.getTracks()){
                if (track.keyCount == 0) continue;
                if (track.channel != ClipChannel::Position && track.channel != ClipChannel::Rotation && track.channel != ClipChannel::Scale) continue;
                const int bone = rig.findBone(track.path);
                if (bone >= 0) bindings.push_back({&track, bone, anim::KeyCursor{}});
            }
        }

        void pose(float time){
            sampleAt(time);
            resolve(local);
        }

        // the local pose at `time` (localPose()), without its rig space
        void sampleAt(float time){
            local.copyFrom(rest);
            for (Binding& binding : bindings){
                if (!anim::sampleTrack(*binding.track, time, sample, &binding.cursor)) continue;
                const size_t bone = (size_t)binding.bone;
                switch (binding.track->channel){
                    case ClipChannel::Position:
                        local.setPosition(bone, binding.track->relative ? rest.position(bone) + sample.vector : sample.vector);
                        break;
                    case ClipChannel::Scale:
                        local.setScale(bone, binding.track->relative ? rest.scale(bone) * sample.vector : sample.vector);
                        break;
                    default:
                        local.setRotation(bone, binding.track->relative ? rest.rotation(bone) * sample.rotation : sample.rotation);
                        break;
                }
            }
        }

        // the rig space of `from` (localPose() or a blend of two posers' local poses)
        void resolve(const anim::Pose& from){
            anim::poseToRigSpace(from, parents, anim::RigSpace{world.data(), positions.data(), rotations.data(), scales.data()});
        }

        const anim::Pose& localPose() const { return local; }
        const anim::Affine& matrix(size_t bone) const { return world[bone]; }
        const Vector3& position(size_t bone) const { return positions[bone]; }
        const Quaternion& rotation(size_t bone) const { return rotations[bone]; }
        // the bones a track of this clip moves, itself and every descendant
        void markMoved(std::vector<uint8_t>& moved) const{
            for (const Binding& binding : bindings) moved[(size_t)binding.bone] = 1;
        }

    private:
        struct Binding{
            const AnimationClip::Track* track;
            int bone;
            anim::KeyCursor cursor;
        };
        const int* parents;
        anim::Pose rest, local;
        std::vector<anim::Affine> world;
        std::vector<Vector3> positions;
        std::vector<Quaternion> rotations;
        std::vector<Vector3> scales;
        std::vector<Binding> bindings;
        anim::TrackSample sample;
    };

    void spreadToDescendants(const anim::ClipRig& rig, std::vector<uint8_t>& moved){
        for (size_t b = 0; b < rig.size(); b++){
            if (rig.parents[b] >= 0 && moved[(size_t)rig.parents[b]]) moved[b] = 1;
        }
    }

    // A joint's origin and the six points `distance` metres from it along its axes. `reference`
    // gives each axis its length, so a scale error in `pose` shows and the distance is in metres
    // whatever scale the rig is authored at.
    void jointPoints(const anim::Affine& pose, const anim::Affine& reference, double distance, Point out[7]){
        out[0] = Point{pose.m[9], pose.m[10], pose.m[11]};
        for (int axis = 0; axis < 3; axis++){
            const double rx = reference.m[axis * 3], ry = reference.m[axis * 3 + 1], rz = reference.m[axis * 3 + 2];
            const double length = std::sqrt(rx * rx + ry * ry + rz * rz);
            const double factor = length > 1e-12 ? distance / length : 0.0;
            const Point offset{pose.m[axis * 3] * factor, pose.m[axis * 3 + 1] * factor, pose.m[axis * 3 + 2] * factor};
            out[1 + axis * 2] = Point{out[0].x + offset.x, out[0].y + offset.y, out[0].z + offset.z};
            out[2 + axis * 2] = Point{out[0].x - offset.x, out[0].y - offset.y, out[0].z - offset.z};
        }
    }

    double distanceBetween(const Point& a, const Point& b){
        const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Every time a difference can peak at: the key times of both clips (a looping track's over the
    // whole length), and halfway between them, where two slerps part most
    std::vector<float> measureTimes(const AnimationClip& a, const AnimationClip& b){
        std::vector<float> keyTimes = {0.0f};
        const float length = std::max(a.getLength(), b.getLength());
        keyTimes.push_back(length);
        for (const AnimationClip* clip : {&a, &b}){
            for (const AnimationClip::Track& track : clip->getTracks()){
                if (!track.times || track.keyCount == 0) continue;
                keyTimes.insert(keyTimes.end(), track.times, track.times + track.keyCount);
                if (!track.loop || track.duration <= 0.0f) continue;
                for (int lap = 1; (float)lap * track.duration < length; lap++){
                    for (size_t k = 0; k < track.keyCount; k++) keyTimes.push_back((float)lap * track.duration + track.times[k]);
                }
            }
        }
        std::sort(keyTimes.begin(), keyTimes.end());
        keyTimes.erase(std::unique(keyTimes.begin(), keyTimes.end()), keyTimes.end());
        std::vector<float> times;
        times.reserve(keyTimes.size() * 2);
        for (size_t i = 0; i < keyTimes.size(); i++){
            if (i > 0) times.push_back(keyTimes[i - 1] + (keyTimes[i] - keyTimes[i - 1]) * 0.5f);
            times.push_back(keyTimes[i]);
        }
        return times;
    }

    bool sameRows(const float* a, const float* b, size_t count){
        return count == 0 || (a && b && std::memcmp(a, b, count * sizeof(float)) == 0);
    }

    // bit for bit: the same keys, values and tangents
    bool sameTrackData(const AnimationClip::Track& a, const AnimationClip::Track& b){
        if (a.keyCount != b.keyCount || a.width != b.width || a.relative != b.relative || a.loop != b.loop) return false;
        if (!sameRows(a.times, b.times, a.keyCount)) return false;
        for (size_t k = 0; k < a.keyCount; k++){
            const EaseType easeA = a.easings ? a.easings[k] : EaseType::LINEAR;
            const EaseType easeB = b.easings ? b.easings[k] : EaseType::LINEAR;
            if (k + 1 < a.keyCount && easeA != easeB) return false;
        }
        const size_t values = a.keyCount * a.width;
        auto floats = [](const auto* row){ return reinterpret_cast<const float*>(row); };
        if ((a.vectors != nullptr) != (b.vectors != nullptr) || (a.rotations != nullptr) != (b.rotations != nullptr) || (a.weights != nullptr) != (b.weights != nullptr)) return false;
        if (a.hasTangents() != b.hasTangents()) return false;
        if (a.vectors && !(sameRows(floats(a.vectors), floats(b.vectors), values * 3))) return false;
        if (a.rotations && !(sameRows(floats(a.rotations), floats(b.rotations), values * 4))) return false;
        if (a.weights && !sameRows(a.weights, b.weights, values)) return false;
        if (a.inVectors && !(sameRows(floats(a.inVectors), floats(b.inVectors), values * 3) && sameRows(floats(a.outVectors), floats(b.outVectors), values * 3))) return false;
        if (a.inRotations && !(sameRows(floats(a.inRotations), floats(b.inRotations), values * 4) && sameRows(floats(a.outRotations), floats(b.outRotations), values * 4))) return false;
        if (a.inWeights && !(sameRows(a.inWeights, b.inWeights, values) && sameRows(a.outWeights, b.outWeights, values))) return false;
        return true;
    }

    bool sameTrackIdentity(const AnimationClip::Track& a, const AnimationClip::Track& b){
        return a.path == b.path && a.channel == b.channel && a.humanBone == b.humanBone && a.component == b.component && a.property == b.property;
    }

    bool isBoneChannel(ClipChannel channel){
        return channel == ClipChannel::Position || channel == ClipChannel::Rotation || channel == ClipChannel::Scale;
    }

    // tracks a rig cannot put a number on: they are the same, or they count
    size_t countUnmeasuredDifferences(const AnimationClip& a, const AnimationClip& b, const anim::ClipRig& rig){
        auto measured = [&](const AnimationClip::Track& track){ return isBoneChannel(track.channel) && rig.findBone(track.path) >= 0; };
        size_t differences = 0;
        for (const AnimationClip::Track& track : a.getTracks()){
            if (measured(track)) continue;
            const auto& others = b.getTracks();
            const auto other = std::find_if(others.begin(), others.end(), [&](const AnimationClip::Track& t){ return sameTrackIdentity(track, t); });
            if (other == others.end() || !sameTrackData(track, *other)) differences++;
        }
        for (const AnimationClip::Track& track : b.getTracks()){
            if (measured(track)) continue;
            const auto& others = a.getTracks();
            if (std::none_of(others.begin(), others.end(), [&](const AnimationClip::Track& t){ return sameTrackIdentity(track, t); })) differences++;
        }
        return differences;
    }

    bool sameEvents(const AnimationClip& a, const AnimationClip& b){
        const std::vector<ClipEventData>& x = a.getEvents();
        const std::vector<ClipEventData>& y = b.getEvents();
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); i++){
            if (x[i].time != y[i].time || x[i].function != y[i].function || x[i].args.size() != y[i].args.size()) return false;
            for (size_t k = 0; k < x[i].args.size(); k++){
                if (!(x[i].args[k] == y[i].args[k])) return false;
            }
        }
        return true;
    }

    // the runtime's order (AnimatorInstance::bindClip): the clip's bone, the hips, then the
    // shallowest bone the clip has a position track on; -1 when it has none
    int rootMotionBone(const AnimationClip& clip, const anim::ClipRig& rig){
        const anim::ClipRootMotion declared = anim::readClipRootMotion(clip.getUnknownKeys());
        if (!declared.bone.empty()){
            const int bone = rig.findBone(declared.bone);
            if (bone >= 0) return bone;
        }
        if (rig.hips >= 0) return rig.hips;
        int best = -1, bestDepth = 1 << 30;
        for (const AnimationClip::Track& track : clip.getTracks()){
            if (track.channel != ClipChannel::Position) continue;
            const int bone = rig.findBone(track.path);
            if (bone < 0) continue;
            int depth = 0;
            for (int p = rig.parents[(size_t)bone]; p >= 0; p = rig.parents[(size_t)p]) depth++;
            if (depth < bestDepth){
                bestDepth = depth;
                best = bone;
            }
        }
        return best;
    }

    double yawRadians(const Quaternion& rotation){
        const Vector3 forward = rotation * Vector3(0, 0, 1);
        return std::atan2((double)forward.x, (double)forward.z);
    }

    double wrapRadians(double angle){
        return std::remainder(angle, 2.0 * 3.14159265358979323846);
    }

    // the heading a bone's rotation gives, when its forward axis is not close to vertical
    bool headingOf(const Quaternion& rotation, double& heading){
        const Vector3 forward = rotation * Vector3(0, 0, 1);
        if (std::hypot((double)forward.x, (double)forward.z) < 0.2) return false;
        heading = std::atan2((double)forward.x, (double)forward.z);
        return true;
    }

    anim::ClipRootMotionFacts measureRootMotion(const AnimationClip& clip, const anim::ClipRig& rig){
        anim::ClipRootMotionFacts facts;
        const int bone = rootMotionBone(clip, rig);
        const double length = clip.getLength();
        if (bone < 0 || !(length > 0.0)) return facts;
        facts.bone = rig.paths[(size_t)bone];
        RigPoser poser(clip, rig);
        poser.pose(0.0f);
        const Vector3 start = poser.matrix((size_t)bone).translation();
        // unwrapped at about 30 Hz, so a clip that turns twice says 720 degrees
        const int steps = std::clamp((int)std::ceil(length * 30.0), 1, 4000);
        double previous = yawRadians(poser.rotation((size_t)bone));
        double turned = 0.0;
        for (int step = 1; step <= steps; step++){
            poser.pose(step == steps ? clip.getLength() : (float)(length * step / steps));
            const double yaw = yawRadians(poser.rotation((size_t)bone));
            turned += wrapRadians(yaw - previous);
            previous = yaw;
        }
        const Vector3 end = poser.matrix((size_t)bone).translation();
        facts.netTranslationMetres = end - start;
        facts.netYawDegrees = turned * 180.0 / 3.14159265358979323846;
        const double horizontal = std::hypot((double)facts.netTranslationMetres.x, (double)facts.netTranslationMetres.z);
        facts.averageSpeedMps = horizontal / length;
        facts.averageYawDegS = facts.netYawDegrees / length;
        facts.loopGapMm = std::fabs((double)facts.netTranslationMetres.y) * 1000.0;
        return facts;
    }

    // a foot within this of its lowest in the clip is on the ground (metres)
    constexpr double FOOT_CONTACT_BAND = 0.02;

    anim::ClipFootSliding measureFootSliding(const AnimationClip& clip, const anim::ClipRig& rig, const std::vector<std::string>& asked,
                                             std::optional<double> moveSpeed){
        anim::ClipFootSliding out;
        std::vector<int> feet;
        for (const std::string& name : asked){
            int bone = rig.findBone(name);
            if (bone < 0){
                const auto found = std::find(rig.names.begin(), rig.names.end(), name);
                if (found != rig.names.end()) bone = (int)(found - rig.names.begin());
            }
            if (bone < 0) out.unknownFeet.push_back(name);
            else if (std::find(feet.begin(), feet.end(), bone) == feet.end()) feet.push_back(bone);
        }
        if (!feet.empty()) out.contactSource = "feet";
        else if (asked.empty() && !rig.footGoals.empty()){
            feet = rig.footGoals;
            out.contactSource = "foot_goals";
        }
        const double length = clip.getLength();
        if (feet.empty() || !(length > 0.0)) return out;

        const int steps = std::clamp((int)std::ceil(length * 60.0), 2, 6000);
        const double dt = length / steps;
        std::vector<std::vector<Point>> at(feet.size(), std::vector<Point>((size_t)steps + 1));
        RigPoser poser(clip, rig);
        for (int i = 0; i <= steps; i++){
            poser.pose(i == steps ? clip.getLength() : (float)(dt * i));
            for (size_t f = 0; f < feet.size(); f++){
                const Vector3& p = poser.position((size_t)feet[f]);
                at[f][(size_t)i] = Point{p.x, p.y, p.z};
            }
        }
        std::vector<std::vector<uint8_t>> contact(feet.size(), std::vector<uint8_t>((size_t)steps + 1, 0));
        double backward = 0.0, contactTime = 0.0;
        for (size_t f = 0; f < feet.size(); f++){
            double lowest = at[f][0].y;
            for (const Point& p : at[f]) lowest = std::min(lowest, p.y);
            for (size_t i = 0; i < at[f].size(); i++) contact[f][i] = at[f][i].y <= lowest + FOOT_CONTACT_BAND ? 1 : 0;
            for (size_t i = 1; i < at[f].size(); i++){
                if (!contact[f][i] || !contact[f][i - 1]) continue;
                backward -= at[f][i].z - at[f][i - 1].z;
                contactTime += dt;
            }
        }
        out.matchingGroundSpeedMps = contactTime > 0.0 ? backward / contactTime : 0.0;
        if (moveSpeed) out.moveSpeedMps = *moveSpeed;
        else if (measureRootMotion(clip, rig).averageSpeedMps < anim::kClipInPlaceSpeedMps) out.moveSpeedMps = out.matchingGroundSpeedMps;

        for (size_t f = 0; f < feet.size(); f++){
            anim::ClipFootSlide slide;
            slide.bone = rig.names[(size_t)feet[f]];
            size_t onGround = 0;
            size_t start = 0;
            bool inContact = false;
            for (size_t i = 0; i < at[f].size(); i++){
                if (!contact[f][i]){
                    inContact = false;
                    continue;
                }
                onGround++;
                if (!inContact){
                    inContact = true;
                    start = i;
                }
                // on the ground moving at the move speed: where the foot is against where it touched down
                const double dx = at[f][i].x - at[f][start].x;
                const double dz = (at[f][i].z - at[f][start].z) + out.moveSpeedMps * dt * (double)(i - start);
                const double mm = std::hypot(dx, dz) * 1000.0;
                if (mm > slide.maxSlideMm){
                    slide.maxSlideMm = mm;
                    slide.worstStart = (float)(dt * (double)start);
                    slide.worstEnd = (float)(dt * (double)i);
                }
            }
            slide.contactRatio = (double)onGround / (double)at[f].size();
            out.maxSlideMm = std::max(out.maxSlideMm, slide.maxSlideMm);
            out.feet.push_back(slide);
        }
        return out;
    }

    // the two posers' local poses mixed by `weight` of `to`, as a crossfade blends them
    void blendPoses(const anim::Pose& from, const anim::Pose& to, float weight, size_t bones, anim::Pose& out){
        for (size_t b = 0; b < bones; b++){
            out.setPosition(b, from.position(b) * (1.0f - weight) + to.position(b) * weight);
            out.setScale(b, from.scale(b) * (1.0f - weight) + to.scale(b) * weight);
            out.setRotation(b, Quaternion::slerp(weight, from.rotation(b), to.rotation(b)));
        }
    }

    anim::ClipTransitionFacts measureTransition(const AnimationClip& from, const AnimationClip& to, const anim::ClipRig& rig,
                                                float duration, float at){
        anim::ClipTransitionFacts facts;
        facts.durationSeconds = std::max(duration, 0.0f);
        facts.atSeconds = std::max(at, 0.0f);
        const size_t bones = rig.size();
        RigPoser a(from, rig), b(to, rig);
        std::vector<uint8_t> movedA(bones, 0), movedB(bones, 0);
        a.markMoved(movedA);
        b.markMoved(movedB);
        std::vector<uint8_t> moved(bones, 0);
        for (size_t i = 0; i < bones; i++) moved[i] = movedA[i] || movedB[i];
        spreadToDescendants(rig, moved);
        anim::Pose blend;
        blend.resize(bones, 0);
        constexpr double frame = 1.0 / 60.0;
        constexpr double around = 0.25;     // seconds of each clip measured around the crossfade
        const double start = std::max(0.0, (double)facts.atSeconds - around);
        const double end = (double)facts.atSeconds + facts.durationSeconds + around;
        const int frames = std::max(2, (int)std::ceil((end - start) / frame));
        std::vector<Point> previous(bones);
        for (int i = 0; i <= frames; i++){
            const double t = start + frame * i;
            const double into = t - facts.atSeconds;
            if (into < 0.0){
                a.pose((float)t);
                b.resolve(a.localPose());
            }else{
                a.sampleAt((float)t);
                b.sampleAt((float)into);
                const float weight = facts.durationSeconds > 0.0f ? (float)std::min(1.0, into / facts.durationSeconds) : 1.0f;
                blendPoses(a.localPose(), b.localPose(), weight, bones, blend);
                b.resolve(blend);
            }
            // b holds the pose that plays now; the first frame only sets where the steps start from
            const bool during = into >= 0.0 && into - frame <= facts.durationSeconds;
            for (size_t bone = 0; bone < bones; bone++){
                if (!moved[bone]) continue;
                const Vector3& p = b.position(bone);
                const Point now{p.x, p.y, p.z};
                if (i > 0){
                    const double mm = distanceBetween(previous[bone], now) * 1000.0;
                    if (during){
                        if (mm > facts.maxStepMm){
                            facts.maxStepMm = mm;
                            facts.worstJoint = rig.names[bone];
                            facts.worstTimeSeconds = (float)std::max(0.0, into);
                        }
                    }else{
                        facts.baselineStepMm = std::max(facts.baselineStepMm, mm);
                    }
                }
                previous[bone] = now;
            }
        }
        facts.popRatio = facts.maxStepMm / std::max(facts.baselineStepMm, 0.1);
        if (facts.durationSeconds <= 0.0f) facts.causes.push_back("duration 0: the switch is instant");
        auto onlyIn = [&](const std::vector<uint8_t>& own, const std::vector<uint8_t>& other, const std::string& clip, const char* what){
            std::vector<std::string> names;
            for (size_t i = 0; i < bones; i++){
                if (own[i] && !other[i]) names.push_back(rig.names[i]);
            }
            if (names.empty()) return;
            std::string list;
            for (size_t i = 0; i < names.size() && i < 5; i++) list += (i ? ", " : "") + names[i];
            if (names.size() > 5) list += " and " + std::to_string(names.size() - 5) + " more";
            facts.causes.push_back("bones keyed only in " + clip + what + list);
        };
        onlyIn(movedA, movedB, "the first clip", " go back to their rest: ");
        onlyIn(movedB, movedA, "the second clip", " leave their rest: ");
        return facts;
    }

    anim::ClipLoopSeam measureLoopSeam(const AnimationClip& clip, const anim::ClipRig& rig, double skinDistanceMm){
        anim::ClipLoopSeam seam;
        if (!(clip.getLength() > 0.0f)) return seam;
        const double distance = std::max(skinDistanceMm, 0.0) / 1000.0;
        RigPoser poser(clip, rig);
        std::vector<uint8_t> moved(rig.size(), 0);
        poser.markMoved(moved);
        spreadToDescendants(rig, moved);
        poser.pose(0.0f);
        std::vector<anim::Affine> first(rig.size());
        for (size_t b = 0; b < rig.size(); b++) first[b] = poser.matrix(b);
        const int bone = rootMotionBone(clip, rig);
        Point rootStart, rootEnd;
        double headingStart = 0.0;
        bool hasHeading = false;
        if (bone >= 0){
            const anim::Affine& root = poser.matrix((size_t)bone);
            rootStart = Point{root.m[9], root.m[10], root.m[11]};
            hasHeading = headingOf(poser.rotation((size_t)bone), headingStart);
        }
        poser.pose(clip.getLength());
        double turn = 0.0;
        if (bone >= 0){
            const anim::Affine& root = poser.matrix((size_t)bone);
            rootEnd = Point{root.m[9], root.m[10], root.m[11]};
            double headingEnd = 0.0;
            if (hasHeading && headingOf(poser.rotation((size_t)bone), headingEnd)) turn = wrapRadians(headingStart - headingEnd);
        }
        const double cosTurn = std::cos(turn), sinTurn = std::sin(turn);
        for (size_t b = 0; b < rig.size(); b++){
            if (!moved[b]) continue;
            Point at[7], back[7];
            jointPoints(first[b], first[b], distance, at);
            jointPoints(poser.matrix(b), first[b], distance, back);
            double worst = 0.0;
            for (int i = 0; i < 7; i++){
                // the end pose carried back along the root's travel: its height stays, so a jump counts
                const double x = back[i].x - rootEnd.x, z = back[i].z - rootEnd.z;
                const Point aligned{x * cosTurn + z * sinTurn + rootStart.x, back[i].y, -x * sinTurn + z * cosTurn + rootStart.z};
                worst = std::max(worst, distanceBetween(at[i], aligned));
            }
            if (worst * 1000.0 > seam.maxErrorMm){
                seam.maxErrorMm = worst * 1000.0;
                seam.worstJoint = rig.names[b];
            }
        }
        return seam;
    }

    double clipReportRounded(double value, double decimals = 1000.0){
        return std::round(value * decimals) / decimals;
    }

    std::string lowered(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return text;
    }

    // R23's names: mixamorig:LeftFoot, LeftToeBase, foot_l, ball_l (Unreal's toe), ankle_R
    bool namesAFoot(const std::string& name){
        const std::string text = lowered(name);
        return text.find("foot") != std::string::npos || text.find("toe") != std::string::npos ||
               text.find("ankle") != std::string::npos || text.rfind("ball_", 0) == 0;
    }

}

int anim::ClipRig::findBone(const std::string& path) const{
    const auto found = byPath.find(path);
    return found == byPath.end() ? -1 : found->second;
}

void anim::ClipRig::assign(const std::string& sourceName, const std::vector<ModelNodeInfo>& nodes){
    source = sourceName;
    names.clear();
    paths.clear();
    parents.clear();
    restPositions.clear();
    restRotations.clear();
    restScales.clear();
    feet.clear();
    footGoals.clear();
    byPath.clear();
    hips = -1;
    const size_t count = nodes.size();
    // parents first, whatever order the file lists them in: by depth, the file's order within one
    std::vector<int> depth(count, 0);
    for (size_t i = 0; i < count; i++){
        size_t steps = 0;
        for (int p = nodes[i].parent; p >= 0 && (size_t)p < count && steps <= count; p = nodes[(size_t)p].parent, steps++) depth[i]++;
        if (steps > count) depth[i] = 0;
    }
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b){ return depth[a] < depth[b]; });
    std::vector<int> position(count, -1);
    for (size_t i = 0; i < count; i++){
        const ModelNodeInfo& node = nodes[order[i]];
        position[order[i]] = (int)i;
        names.push_back(node.name);
        paths.push_back(node.path);
        const bool hasParent = node.parent >= 0 && (size_t)node.parent < count && position[(size_t)node.parent] >= 0;
        parents.push_back(hasParent ? position[(size_t)node.parent] : -1);
        restPositions.push_back(node.position);
        restRotations.push_back(node.rotation);
        restScales.push_back(node.scale);
        feet.push_back(namesAFoot(node.name) ? 1 : 0);
        // a repeat of a path cannot bind: the first node wins, as tracks resolve
        if (node.addressable) byPath.emplace(node.path, (int)i);
    }
}

bool anim::clipRigFromModel(const std::string& modelPath, ClipRig& out, std::string* error){
    std::vector<ModelNodeInfo> nodes;
    if (!readModelFileNodes(modelPath, nodes, error)) return false;
    out.assign(modelPath, nodes);
    std::string stamp;
    pose::HumanoidOverrides overrides;
    std::vector<std::string> warnings;
    if (!pose::humanoidSidecarStamp(modelPath, stamp, nullptr) || stamp.empty()) return true;
    if (!pose::parseHumanoidOverrides(stamp, overrides, warnings, nullptr) || !overrides.isHumanoid()) return true;
    std::string sourceError;
    const std::shared_ptr<const RetargetSource> source = retargetSource(modelPath, &sourceError);
    if (!source) return true;
    auto boneOfSlot = [&](pose::HumanBone slot) -> int{
        const std::string& name = source->humanoid.bones[(size_t)slot];
        if (name.empty()) return -1;
        const auto found = std::find(out.names.begin(), out.names.end(), name);
        return found == out.names.end() ? -1 : (int)(found - out.names.begin());
    };
    out.hips = boneOfSlot(pose::HumanBone::Hips);
    for (pose::HumanBone slot : {pose::HumanBone::LeftFoot, pose::HumanBone::RightFoot}){
        const int bone = boneOfSlot(slot);
        if (bone >= 0) out.footGoals.push_back(bone);
    }
    for (pose::HumanBone slot : {pose::HumanBone::LeftFoot, pose::HumanBone::RightFoot, pose::HumanBone::LeftToes, pose::HumanBone::RightToes}){
        const int bone = boneOfSlot(slot);
        if (bone >= 0) out.feet[(size_t)bone] = 1;
    }
    return true;
}

bool anim::clipTrackIsReducible(const AnimationClip::Track& track, const ClipRig& rig, ClipSkippedTracks* skipped){
    ClipSkippedTracks ignored;
    ClipSkippedTracks& count = skipped ? *skipped : ignored;
    switch (track.channel){
        case ClipChannel::Morph: count.morph++; return false;
        case ClipChannel::Lattice: count.lattice++; return false;
        case ClipChannel::Property: count.property++; return false;
        case ClipChannel::Humanoid:
        case ClipChannel::HumanoidPosition: count.humanoid++; return false;
        default: break;
    }
    if (rig.findBone(track.path) < 0){
        count.unbound++;
        return false;
    }
    if (track.hasTangents()){
        count.tangents++;
        return false;
    }
    if (track.easings && track.keyCount > 1){
        bool anyEase = false, allStep = true;
        for (size_t k = 0; k + 1 < track.keyCount; k++){
            if (track.easings[k] != EaseType::LINEAR) anyEase = true;
            if (track.easings[k] != EaseType::STEP) allStep = false;
        }
        if (allStep){
            count.step++;
            return false;
        }
        if (anyEase){
            count.eases++;
            return false;
        }
    }
    return track.keyCount > 1;
}

anim::ClipErrorReport anim::measureClipError(const AnimationClip& reference, const AnimationClip& other, const ClipRig& rig,
                                             double skinDistanceMm, const std::atomic<bool>* cancel){
    ClipErrorReport report;
    report.skinDistanceMm = skinDistanceMm;
    report.jointMaxMm.assign(rig.size(), 0.0);
    RigPoser a(reference, rig);
    RigPoser b(other, rig);
    std::vector<uint8_t> moved(rig.size(), 0);
    a.markMoved(moved);
    b.markMoved(moved);
    if (std::find(moved.begin(), moved.end(), (uint8_t)1) == moved.end()) return report;
    spreadToDescendants(rig, moved);
    const double distance = skinDistanceMm / 1000.0;
    const std::vector<float> times = measureTimes(reference, other);
    double sum = 0.0;
    size_t count = 0;
    for (float time : times){
        if (cancel && cancel->load(std::memory_order_relaxed)) return report;
        a.pose(time);
        b.pose(time);
        for (size_t bone = 0; bone < rig.size(); bone++){
            if (!moved[bone]) continue;
            Point pa[7], pb[7];
            jointPoints(a.matrix(bone), a.matrix(bone), distance, pa);
            jointPoints(b.matrix(bone), a.matrix(bone), distance, pb);
            double worst = 0.0;
            for (int i = 0; i < 7; i++) worst = std::max(worst, distanceBetween(pa[i], pb[i]));
            const double millimetres = worst * 1000.0;
            sum += millimetres;
            count++;
            report.jointMaxMm[bone] = std::max(report.jointMaxMm[bone], millimetres);
            if (millimetres > report.maxErrorMm){
                report.maxErrorMm = millimetres;
                report.worstJoint = rig.names[bone];
                report.worstTimeSeconds = time;
            }
        }
    }
    report.samples = times.size();
    report.meanErrorMm = count ? sum / (double)count : 0.0;
    report.measured = true;
    return report;
}

anim::ClipReport anim::analyzeClip(const ClipReportRequest& request){
    ClipReport report;
    std::shared_ptr<const AnimationClip> clip = request.clip;
    bool loaded = false;
    if (!clip){
        if (request.reference.empty()){
            report.error = "no clip";
            return report;
        }
        clip = AnimationClipPool::get(request.reference);
        if (!clip){
            clip = AnimationClipPool::getOrLoad(request.reference, &report.error);
            loaded = clip != nullptr;
        }
        if (!clip) return report;
    }

    report.ok = true;
    report.runtimeBytes = clip->memoryBytes();
    report.fileBytes = fileSizeOf(request.reference);
    report.tracks = clip->getTracks().size();
    report.lengthSeconds = clip->getLength();
    report.frameRate = clip->getFrameRate();
    report.wrap = clip->getWrap();
    report.audioCues = clip->getAudioCues().size();
    for (const AnimationClip::Track& track : clip->getTracks()){
        report.keys += track.keyCount;
        if (isConstant(track)) report.constantTracks++;
    }
    if (request.rig){
        ClipSkippedTracks skipped;
        for (const AnimationClip::Track& track : clip->getTracks()) clipTrackIsReducible(track, *request.rig, &skipped);
        report.skippedTracks = skipped;
        if (request.rootMotion) report.rootMotion = measureRootMotion(*clip, *request.rig);
        if (request.loopSeam) report.loopSeam = measureLoopSeam(*clip, *request.rig, request.skinDistanceMm);
        if (request.compareTo){
            ClipCompareFacts compare;
            compare.reference = request.compareToName;
            compare.toleranceMm = request.toleranceMm;
            compare.error = measureClipError(*request.compareTo, *clip, *request.rig, request.skinDistanceMm, request.cancel);
            compare.worstJoints = clipWorstJointsJson(compare.error, *request.rig, 8);
            compare.unmeasuredDifferences = countUnmeasuredDifferences(*request.compareTo, *clip, *request.rig);
            compare.eventsMatch = sameEvents(*request.compareTo, *clip);
            compare.audioCuesMatch = request.compareTo->getAudioCues() == clip->getAudioCues();
            compare.lengthMatches = request.compareTo->getLength() == clip->getLength();
            if (request.toleranceMm && compare.error.measured) compare.withinTolerance = compare.error.maxErrorMm <= *request.toleranceMm;
            report.compare = std::move(compare);
        }
        if (request.footSliding) report.footSliding = measureFootSliding(*clip, *request.rig, request.feet, request.moveSpeedMps);
        if (request.transitionTo){
            ClipTransitionFacts facts = measureTransition(*clip, *request.transitionTo, *request.rig, request.transitionDuration, request.transitionAt);
            facts.from = request.transitionFromName;
            facts.to = request.transitionToName;
            report.transition = std::move(facts);
        }
    }

    if (loaded){
        clip.reset();
        AnimationClipPool::releaseUnused(request.reference);
    }
    return report;
}

const anim::ClipReportField* anim::findClipReportField(std::string_view key){
    for (const ClipReportField& field : kClipReportFields){
        if (key == field.key) return &field;
    }
    return nullptr;
}

std::string anim::formatClipField(const ClipReportField& field, double value){
    char text[32];
    switch (field.format){
        case ClipFieldFormat::Bytes:
            if (value < 1024.0) return withThousands((uint64_t)value) + " B";
            if (value < 1024.0 * 1024.0){
                std::snprintf(text, sizeof(text), "%.0f KB", value / 1024.0);
            }else{
                std::snprintf(text, sizeof(text), "%.1f MB", value / (1024.0 * 1024.0));
            }
            return text;
        case ClipFieldFormat::Count:
            return withThousands((uint64_t)value);
        case ClipFieldFormat::Seconds:
            std::snprintf(text, sizeof(text), "%.2f s", value);
            return text;
        case ClipFieldFormat::Millimetres:
            std::snprintf(text, sizeof(text), "%.2f mm", value);
            return text;
        case ClipFieldFormat::MetresPerSecond:
            std::snprintf(text, sizeof(text), "%.2f m/s", value);
            return text;
        case ClipFieldFormat::DegreesPerSecond:
            std::snprintf(text, sizeof(text), "%.1f deg/s", value);
            return text;
        case ClipFieldFormat::Degrees:
            std::snprintf(text, sizeof(text), "%.1f deg", value);
            return text;
        case ClipFieldFormat::Metres:
            std::snprintf(text, sizeof(text), "%.3f m", value);
            return text;
        case ClipFieldFormat::Flag:
            return value != 0.0 ? "yes" : "no";
        case ClipFieldFormat::Text:
            break;
    }
    return std::string();
}

std::string anim::formatClipField(std::string_view key, double value){
    const ClipReportField* field = findClipReportField(key);
    return field ? formatClipField(*field, value) : std::string();
}

std::string anim::clipInfoLine(const ClipReport& report){
    const std::string dot = " \xC2\xB7 ";
    auto counted = [](size_t count, const char* one){ return withThousands(count) + " " + one + (count == 1 ? "" : "s"); };
    char rate[32];
    std::snprintf(rate, sizeof(rate), "%g Hz", (double)report.frameRate);
    std::string line = formatClipField("length_s", report.lengthSeconds) + dot + rate + dot + counted(report.tracks, "track") + dot +
                       counted(report.keys, "key") + dot + "Memory " + formatClipField("runtime_bytes", (double)report.runtimeBytes);
    if (report.fileBytes) line += dot + "File " + formatClipField("file_bytes", (double)*report.fileBytes);
    if (report.constantTracks > 0) line += dot + withThousands(report.constantTracks) + (report.constantTracks == 1 ? " constant track" : " constant tracks");
    return line;
}

std::string anim::clipErrorLine(const ClipErrorReport& error){
    return "Error: max " + formatClipField("max_error_mm", error.maxErrorMm) + ", mean " + formatClipField("mean_error_mm", error.meanErrorMm);
}

nlohmann::json anim::clipReportJson(const ClipReport& report){
    nlohmann::json out = nlohmann::json::object();
    out["runtime_bytes"] = report.runtimeBytes;
    out["file_bytes"] = report.fileBytes ? nlohmann::json(*report.fileBytes) : nlohmann::json(nullptr);
    out["keys"] = report.keys;
    out["tracks"] = report.tracks;
    // the float's own digits, not its expansion as a double (1.33, not 1.3300000429)
    out["length_s"] = std::round((double)report.lengthSeconds * 1000000.0) / 1000000.0;
    out["constant_tracks"] = report.constantTracks;
    out["audio_cues"] = report.audioCues;
    if (report.skippedTracks) out["skipped_tracks"] = clipSkippedTracksJson(*report.skippedTracks);
    if (report.compare){
        nlohmann::json compare = clipErrorJson(report.compare->error);
        compare["reference"] = report.compare->reference;
        compare["joints"] = report.compare->worstJoints;
        compare["tolerance_mm"] = report.compare->toleranceMm ? nlohmann::json(*report.compare->toleranceMm) : nlohmann::json(nullptr);
        compare["within_tolerance"] = report.compare->withinTolerance ? nlohmann::json(*report.compare->withinTolerance) : nlohmann::json(nullptr);
        compare["unmeasured_differences"] = report.compare->unmeasuredDifferences;
        compare["events_match"] = report.compare->eventsMatch;
        compare["audio_cues_match"] = report.compare->audioCuesMatch;
        compare["length_matches"] = report.compare->lengthMatches;
        out["compare"] = compare;
    }
    if (report.loopSeam){
        out["loop_seam"] = {{"max_error_mm", clipReportRounded(report.loopSeam->maxErrorMm)}, {"worst_joint", report.loopSeam->worstJoint}};
    }
    if (report.rootMotion){
        const ClipRootMotionFacts& facts = *report.rootMotion;
        out["root_motion"] = {{"bone", facts.bone},
                              {"net_translation_m", {clipReportRounded(facts.netTranslationMetres.x, 1e6), clipReportRounded(facts.netTranslationMetres.y, 1e6), clipReportRounded(facts.netTranslationMetres.z, 1e6)}},
                              {"net_yaw_deg", clipReportRounded(facts.netYawDegrees)},
                              {"average_speed_mps", clipReportRounded(facts.averageSpeedMps)},
                              {"average_yaw_deg_s", clipReportRounded(facts.averageYawDegS)},
                              {"loop_gap_mm", clipReportRounded(facts.loopGapMm)}};
    }
    if (report.footSliding){
        const ClipFootSliding& sliding = *report.footSliding;
        nlohmann::json feet = nlohmann::json::array();
        for (const ClipFootSlide& foot : sliding.feet){
            feet.push_back({{"bone", foot.bone}, {"contact_ratio", clipReportRounded(foot.contactRatio)}, {"max_slide_mm", clipReportRounded(foot.maxSlideMm)},
                            {"worst_contact", {{"start", clipReportRounded(foot.worstStart)}, {"end", clipReportRounded(foot.worstEnd)}}}});
        }
        const bool judged = sliding.contactSource != "none";
        nlohmann::json block = {{"contact_source", sliding.contactSource},
                                {"max_slide_mm", judged ? nlohmann::json(clipReportRounded(sliding.maxSlideMm)) : nlohmann::json(nullptr)},
                                {"matching_ground_speed_mps", judged ? nlohmann::json(clipReportRounded(sliding.matchingGroundSpeedMps)) : nlohmann::json(nullptr)},
                                {"move_speed_mps", judged ? nlohmann::json(clipReportRounded(sliding.moveSpeedMps)) : nlohmann::json(nullptr)},
                                {"feet", feet}};
        if (!sliding.unknownFeet.empty()) block["unknown_feet"] = sliding.unknownFeet;
        out["foot_sliding"] = block;
    }
    if (report.transition){
        const ClipTransitionFacts& t = *report.transition;
        out["transition"] = {{"from", t.from}, {"to", t.to}, {"duration_s", clipReportRounded(t.durationSeconds)}, {"at_s", clipReportRounded(t.atSeconds)},
                             {"baseline_step_mm", clipReportRounded(t.baselineStepMm)}, {"max_step_mm", clipReportRounded(t.maxStepMm)},
                             {"pop_ratio", clipReportRounded(t.popRatio, 100.0)}, {"worst_joint", t.worstJoint},
                             {"worst_time_s", clipReportRounded(t.worstTimeSeconds)}, {"causes", t.causes}};
    }
    return out;
}

nlohmann::json anim::clipSkippedTracksJson(const ClipSkippedTracks& skipped){
    nlohmann::json out = nlohmann::json::object();
    auto add = [&](const char* key, size_t count){ if (count > 0) out[key] = count; };
    add("tangents", skipped.tangents);
    add("eases", skipped.eases);
    add("step", skipped.step);
    add("morph", skipped.morph);
    add("lattice", skipped.lattice);
    add("property", skipped.property);
    add("humanoid", skipped.humanoid);
    add("unbound", skipped.unbound);
    return out;
}

nlohmann::json anim::clipErrorJson(const ClipErrorReport& error){
    return {{"skin_distance_mm", error.skinDistanceMm},
            {"max_error_mm", error.measured ? nlohmann::json(clipReportRounded(error.maxErrorMm)) : nlohmann::json(nullptr)},
            {"mean_error_mm", error.measured ? nlohmann::json(clipReportRounded(error.meanErrorMm)) : nlohmann::json(nullptr)},
            {"worst_joint", error.worstJoint},
            {"worst_time_s", clipReportRounded(error.worstTimeSeconds, 1e6)}};
}

nlohmann::json anim::clipWorstJointsJson(const ClipErrorReport& error, const ClipRig& rig, size_t top){
    std::vector<size_t> order;
    for (size_t bone = 0; bone < error.jointMaxMm.size() && bone < rig.size(); bone++){
        if (error.jointMaxMm[bone] > 0.0) order.push_back(bone);
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b){
        return error.jointMaxMm[a] != error.jointMaxMm[b] ? error.jointMaxMm[a] > error.jointMaxMm[b] : a < b;
    });
    if (order.size() > top) order.resize(top);
    nlohmann::json out = nlohmann::json::array();
    for (size_t bone : order) out.push_back({{"bone", rig.names[bone]}, {"max_mm", clipReportRounded(error.jointMaxMm[bone])}});
    return out;
}

size_t anim::countRedundantKeys(const ClipData& data){
    size_t redundant = 0;
    for (const ClipTrackData& track : data.tracks){
        if (!isBoneChannel(track.channel) || track.keys.size() < 3) continue;
        const bool linear = std::all_of(track.keys.begin(), track.keys.end(), [](const ClipKey& key){ return !key.hasTangents() && key.ease == EaseType::LINEAR; });
        if (!linear) continue;
        // against the last key kept, as a run of removable keys all go at once
        size_t kept = 0;
        for (size_t k = 1; k + 1 < track.keys.size(); k++){
            const ClipKey& a = track.keys[kept];
            const ClipKey& at = track.keys[k];
            const ClipKey& b = track.keys[k + 1];
            const float times[2] = {a.time, b.time};
            const KeyLocation location = locateKey(times, 2, nullptr, 0, at.time);
            bool same = true;
            if (track.channel == ClipChannel::Rotation){
                const Quaternion ends[2] = {quaternionFromXYZW(a.value.data()), quaternionFromXYZW(b.value.data())};
                float value[4];
                quaternionToXYZW(sampleQuaternion(location, ends, nullptr, nullptr), value);
                // either sign is the same rotation
                const float sign = value[0] * at.value[0] + value[1] * at.value[1] + value[2] * at.value[2] + value[3] * at.value[3] < 0.0f ? -1.0f : 1.0f;
                for (int i = 0; i < 4; i++) same = same && std::fabs(value[i] * sign - at.value[(size_t)i]) <= 1e-5f;
            }else{
                const Vector3 ends[2] = {Vector3(a.value[0], a.value[1], a.value[2]), Vector3(b.value[0], b.value[1], b.value[2])};
                const Vector3 value = sampleVector3(location, ends, nullptr, nullptr);
                same = std::fabs(value.x - at.value[0]) <= 1e-5f && std::fabs(value.y - at.value[1]) <= 1e-5f && std::fabs(value.z - at.value[2]) <= 1e-5f;
            }
            if (same) redundant++;
            else kept = k;
        }
    }
    return redundant;
}

std::vector<anim::ClipRotationFlip> anim::findRotationFlips(const ClipData& data){
    // cos(160 / 2 degrees): keys this far apart have a short way round only just shorter than the long one
    constexpr float kAlmostOpposite = 0.17365f;
    std::vector<ClipRotationFlip> flips;
    for (const ClipTrackData& track : data.tracks){
        if (track.channel != ClipChannel::Rotation) continue;
        for (size_t k = 1; k < track.keys.size(); k++){
            const std::vector<float>& a = track.keys[k - 1].value;
            const std::vector<float>& b = track.keys[k].value;
            if (track.keys[k - 1].ease == EaseType::STEP) continue;
            const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
            const bool hermite = track.keys[k - 1].hasTangents() && track.keys[k].hasTangents();
            if (std::fabs(dot) < kAlmostOpposite || (hermite && dot < 0.0f)){
                flips.push_back({track.path, track.keys[k].time});
                break;
            }
        }
    }
    return flips;
}

anim::ClipOptimizeRecord anim::readClipOptimizeRecord(const ClipData& data){
    ClipOptimizeRecord record;
    for (const auto& [key, text] : data.unknownKeys){
        if (key != "optimize") continue;
        const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
        if (!json.is_object()) return record;
        record.present = true;
        record.toleranceMm = json.value("tolerance_mm", 0.0);
        record.skinDistanceMm = json.value("skin_distance_mm", 0.0);
        record.maxErrorMm = json.value("max_error_mm", 0.0);
        record.sourceSha1 = json.value("source_sha1", std::string());
        record.source = json.value("source", std::string());
        return record;
    }
    return record;
}

void anim::writeClipOptimizeRecord(ClipData& data, const ClipOptimizeRecord& record){
    nlohmann::json json = {{"tolerance_mm", record.toleranceMm}, {"skin_distance_mm", record.skinDistanceMm},
                           {"source_sha1", record.sourceSha1}, {"max_error_mm", record.maxErrorMm}};
    if (!record.source.empty()) json["source"] = record.source;
    const std::string text = json.dump();
    for (auto& [key, value] : data.unknownKeys){
        if (key == "optimize"){
            value = text;
            return;
        }
    }
    data.unknownKeys.push_back({"optimize", text});
}
