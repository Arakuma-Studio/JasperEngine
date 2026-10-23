// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseSampler.h"

#include "pose/PoseDofs.h"
#include "pose/PoseMath.h"
#include "pose/PoseSecondary.h"
#include "pose/PoseTween.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::pose {

    namespace {

        uint64_t splitmix64(uint64_t x){
            uint64_t z = x + 0x9E3779B97F4A7C15ull;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        Quatd lifeRotationPrecise(int bone, double t, double length, double lifeDegrees, uint32_t seed){
            const double cycles = std::max(1.0, std::round(length * 0.7));
            Quatd result;
            for (int axis = 0; axis < 3; axis++){
                double sum = 0.0;
                for (int k = 1; k <= 3; k++){
                    const double phase = 2.0 * POSE_PI * hash01(seed, bone, axis, k);
                    sum += std::sin(2.0 * POSE_PI * cycles * k * t / length + phase) / k;
                }
                const double angle = degToRad(sum * lifeDegrees / 1.83);
                const Vec3d unit(axis == 0 ? 1 : 0, axis == 1 ? 1 : 0, axis == 2 ? 1 : 0);
                result = result * axisAngleRad(unit, angle);
            }
            return result;
        }

        double frameLength(const PoseAnimationSettings& settings){
            return 1.0 / (settings.frameRate > 0 ? settings.frameRate : 30);
        }

        // per-segment motion for auto timing: mean bone angle (degrees) + 100 * hips travel / humanScale
        template <typename HipsTravel>
        std::vector<double> segmentMotion(const std::vector<HumanPose>& realised, size_t segments, double humanScale, HipsTravel travel){
            const size_t n = realised.size();
            std::vector<double> motion(segments, 0.0);
            for (size_t s = 0; s < segments; s++){
                const HumanPose& a = realised[s];
                const HumanPose& b = realised[(s + 1) % n];
                double angles = 0.0;
                for (int bone = 0; bone < HUMAN_BONE_COUNT; bone++){
                    angles += radToDeg(angleBetween(Quatd(a.rotations[bone]).normalized(), Quatd(b.rotations[bone]).normalized()));
                }
                motion[s] = angles / HUMAN_BONE_COUNT + 100.0 * travel(a, b) / humanScale;
            }
            return motion;
        }

        void applyAutoTiming(PoseAnimationData& data, const std::vector<double>& motion){
            const size_t n = data.poses.size();
            const size_t segments = motion.size();
            double mean = 0.0;
            for (double m : motion) mean += m;
            mean /= static_cast<double>(segments);
            if (!(mean > 1e-9)){
                return;   // no motion: timing unchanged
            }
            const double oldLength = buildTimeline(data).length;

            double meanHold = 0.0;
            for (const PoseKey& key : data.poses) meanHold += key.hold;
            meanHold /= static_cast<double>(n);
            const double holdBase = std::max(meanHold, 0.06);
            const double baseDuration = data.settings.transition.duration;

            std::vector<double> holds(n), durations(n, baseDuration);
            std::vector<Transition> transitions(n);
            for (size_t i = 0; i < n; i++){
                double incoming = mean;
                if (data.settings.loop){
                    incoming = motion[(i + n - 1) % n];
                }else if (i > 0){
                    incoming = motion[i - 1];
                }
                const double ratio = incoming / mean;
                holds[i] = holdBase * clampd(ratio * ratio, 0.1, 3.0);
                if (i < segments){
                    const double x = clampd(motion[i] / mean / 2.0, 0.0, 1.0);
                    durations[i] = baseDuration * (0.6 + (1.6 - 0.6) * x);
                }
                transitions[i] = transitionOut(data, i);
            }

            // keep the total length
            double newLength = 0.0;
            for (size_t i = 0; i < n; i++){
                newLength += holds[i] + (i < segments ? durations[i] : 0.0);
            }
            if (newLength > 1e-9 && oldLength > 1e-9){
                const double factor = oldLength / newLength;
                for (size_t i = 0; i < n; i++){
                    holds[i] *= factor;
                    if (i < segments) durations[i] *= factor;
                }
            }

            const double stepRate = data.settings.stepRate;
            for (size_t i = 0; i < n; i++){
                if (stepRate > 0.0){
                    const double step = 1.0 / stepRate;
                    holds[i] = std::max(step, std::round(holds[i] / step) * step);
                    durations[i] = std::max(step, std::round(durations[i] / step) * step);
                }
                PoseKey& key = data.poses[i];
                key.hold = static_cast<float>(clampd(holds[i], 0.0, 5.0));
                key.customTransition = true;
                key.transition = transitions[i];
                key.transition.duration = static_cast<float>(clampd(durations[i], 0.02, 1.0));
            }
        }

    }

    double hash01(uint32_t seed, int bone, int axis, int k){
        const uint64_t packed = (static_cast<uint64_t>(seed) << 32) | (static_cast<uint64_t>(bone & 0xFFFF) << 16) |
                                (static_cast<uint64_t>(axis & 0xFF) << 8) | static_cast<uint64_t>(k & 0xFF);
        return static_cast<double>(splitmix64(packed) >> 40) / 16777216.0;
    }

    Quaternion lifeRotation(int bone, float t, float length, float lifeDegrees, uint32_t seed){
        if (!(length > 0.0f)){
            return Quaternion();
        }
        return lifeRotationPrecise(bone, t, length, lifeDegrees, seed).normalized().toQuaternion();
    }

    PoseTimeline buildTimeline(const PoseAnimationData& data){
        PoseTimeline timeline;
        const size_t n = data.poses.size();
        timeline.loop = data.settings.loop;
        timeline.holds.resize(n);
        for (size_t i = 0; i < n; i++){
            timeline.holds[i] = std::max(0.0, static_cast<double>(data.poses[i].hold));
        }
        if (n == 0){
            return timeline;
        }
        if (n == 1){
            timeline.length = std::max(timeline.holds[0], frameLength(data.settings));
            return timeline;
        }
        const size_t segments = data.settings.loop ? n : n - 1;
        double key = 0.0;
        for (size_t s = 0; s < segments; s++){
            const Transition& tr = transitionOut(data, s);
            timeline.start.push_back(key + timeline.holds[s]);
            timeline.transitions.push_back(tr);
            key = timeline.start.back() + std::max(0.0, static_cast<double>(tr.duration));
        }
        timeline.length = key + (data.settings.loop ? 0.0 : timeline.holds[n - 1]);
        timeline.length = std::max(timeline.length, 1e-4);
        return timeline;
    }

    SamplerInput makeSamplerInput(const PoseAnimationData& data, std::vector<HumanPose> keys, const PoseRig* rig){
        SamplerInput input;
        input.keys = std::move(keys);
        input.settings = data.settings;
        input.timeline = buildTimeline(data);
        input.rig = rig;
        return input;
    }

    std::vector<HumanPose> realiseKeys(const PoseAnimationData& data, const PoseRig& rig, const PoseSkin* skin){
        std::vector<HumanPose> keys;
        keys.reserve(data.poses.size());
        for (const PoseKey& key : data.poses){
            BodyPose body = key.body;
            if (data.settings.humanize.jointLimits){
                clampToLimits(body);
            }
            keys.push_back(rig.fromBodyPose(body, skin, key.grounded, key.lift));
        }
        return keys;
    }

    void sample(const SamplerInput& input, float time, HumanPose& out){
        const std::vector<HumanPose>& keys = input.keys;
        const PoseTimeline& timeline = input.timeline;
        const size_t n = keys.size();
        if (n == 0){
            return;
        }
        const size_t segments = std::min(timeline.segmentCount(), timeline.loop ? n : n - 1);
        if (n == 1 || segments == 0){
            out = keys[0];
            return;
        }
        const PoseAnimationSettings& settings = input.settings;
        const HumanizeSettings& humanize = settings.humanize;
        const double length = timeline.length;

        // loops: a time within 1e-6 s below a loop boundary (float times) samples the boundary
        double t = time;
        if (timeline.loop && length - positiveFmod(t, length) < 1e-6){
            t = std::round(t / length) * length;
        }

        // choppiness
        if (settings.stepRate > 0.0f){
            t = std::floor(t * settings.stepRate + 1e-4) / settings.stepRate;
        }

        const bool movingHolds = humanize.enabled && humanize.movingHolds > 0.0f;
        const double drift = humanize.movingHolds;
        const bool life = humanize.enabled && humanize.life > 0.0f;

        for (int b = 0; b < HUMAN_BONE_COUNT; b++){
            const HumanBoneInfo& info = humanBoneInfo(boneAt(b));
            const double weight = settings.weights[static_cast<int>(info.group)];
            double tau = t - settings.overlap * weight * info.depth;
            const double loose = clampd(settings.looseness * weight * info.depth, 0.0, 1.0);
            tau = timeline.loop ? positiveFmod(tau, length) : std::max(tau, 0.0);

            Quatd r = Quatd(keys[0].rotations[b]).normalized();
            Vec3d hips(keys[0].hipsPosition);
            const int firstCycle = timeline.loop ? -1 : 0;
            for (int c = firstCycle; c <= 0; c++){
                for (size_t s = 0; s < segments; s++){
                    const double local = tau - timeline.start[s] - c * length;
                    const Transition tr = loose > 0.0 ? loosen(timeline.transitions[s], static_cast<float>(loose)) : timeline.transitions[s];
                    double e = easePrecise(local / std::max(static_cast<double>(tr.duration), 1e-4), tr);
                    if (movingHolds){
                        const double hold = timeline.holds[s];
                        const double hp = hold > 1e-4 ? smoothstep(0.0, 1.0, (local + hold) / hold) : (local >= 0.0 ? 1.0 : 0.0);
                        e = drift * hp + (1.0 - drift) * e;
                    }
                    if (e == 0.0){
                        continue;
                    }
                    const HumanPose& from = keys[s];
                    const HumanPose& to = keys[(s + 1) % n];
                    const Quatd delta = Quatd(from.rotations[b]).normalized().inverse() * Quatd(to.rotations[b]).normalized();
                    r = r * powRotation(delta, e);
                    if (b == 0){
                        hips += (Vec3d(to.hipsPosition) - Vec3d(from.hipsPosition)) * e;
                    }
                }
            }
            if (life && b != 0 && info.group != BodyGroup::Legs && (!input.rig || input.rig->has(boneAt(b)))){
                r = r * lifeRotationPrecise(b, t, length, humanize.life, humanize.seed);
            }
            out.rotations[b] = r.normalized().toQuaternion();
            if (b == 0){
                out.hipsPosition = hips.toVector3();
            }
        }
    }

    std::vector<HumanPose> frames(const SamplerInput& input, int fps){
        std::vector<HumanPose> result;
        if (input.keys.empty()){
            return result;
        }
        if (fps <= 0){
            fps = input.settings.frameRate > 0 ? input.settings.frameRate : 30;
        }
        const double length = input.timeline.length;
        const int count = std::max(1, static_cast<int>(std::lround(length * fps)));
        result.resize(count + 1);
        for (int f = 0; f <= count; f++){
            sample(input, static_cast<float>(f * length / count), result[f]);
        }

        const HumanizeSettings& humanize = input.settings.humanize;
        const bool loop = input.timeline.loop && input.keys.size() > 1;

        // in-betweens: cap the per-frame rotation of every bone
        if (humanize.enabled && humanize.inbetweens > 0.0f){
            const double maxRad = degToRad((180.0 + (12.0 - 180.0) * humanize.inbetweens) * 30.0 / fps);
            std::array<Quatd, HUMAN_BONE_COUNT> state;
            for (int b = 0; b < HUMAN_BONE_COUNT; b++){
                state[b] = Quatd(result[0].rotations[b]).normalized();
            }
            // frame 0 of a loop is the same instant as frame `count`: the write pass starts from
            // the warmed-up state there
            auto pass = [&](bool write){
                if (write){
                    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
                        result[0].rotations[b] = state[b].toQuaternion();
                    }
                }
                for (int f = 1; f <= count; f++){
                    for (int b = 0; b < HUMAN_BONE_COUNT; b++){
                        state[b] = rotateTowards(state[b], Quatd(result[f].rotations[b]).normalized(), maxRad);
                        if (write){
                            result[f].rotations[b] = state[b].toQuaternion();
                        }
                    }
                }
            };
            if (loop){
                pass(false);   // warm-up: the state arrives at the loop point already settled
            }
            pass(true);
            if (loop){
                result[count] = result[0];
            }
        }

        if (input.rig && humanize.enabled && (humanize.torsoFollow > 0.0f || humanize.headFollow > 0.0f)){
            applyFollowThrough(result, *input.rig, fps, loop, humanize);
        }

        if (input.rig && humanize.jointLimits){
            const PoseRig& rig = *input.rig;
            for (HumanPose& frame : result){
                for (int b = 0; b < HUMAN_BONE_COUNT; b++){
                    const HumanBone bone = boneAt(b);
                    if (!rig.has(bone)){
                        continue;
                    }
                    float angles[DOFS_PER_BONE];
                    rig.anglesFromLocal(bone, frame.rotations[b], angles);
                    bool clamped = false;
                    for (int k = 0; k < DOFS_PER_BONE; k++){
                        const DofInfo& dof = dofInfo(dofIndex(bone, k));
                        const float limited = std::min(std::max(angles[k], dof.minDegrees), dof.maxDegrees);
                        if (limited != angles[k]){
                            angles[k] = limited;
                            clamped = true;
                        }
                    }
                    // only clamped bones change: the others keep their exact rotation
                    if (clamped){
                        frame.rotations[b] = rig.localFromAngles(bone, angles);
                    }
                }
            }
        }

        if (loop){
            result[count] = result[0];
        }
        return result;
    }

    void autoTiming(PoseAnimationData& data, const std::vector<HumanPose>& realised, float humanScale){
        const size_t n = data.poses.size();
        if (n < 2 || realised.size() != n || !(humanScale > 0.0f)){
            return;
        }
        const size_t segments = data.settings.loop ? n : n - 1;
        applyAutoTiming(data, segmentMotion(realised, segments, humanScale, [](const HumanPose& a, const HumanPose& b){
            return (Vec3d(b.hipsPosition) - Vec3d(a.hipsPosition)).length();
        }));
    }

    void autoTiming(PoseAnimationData& data, const std::vector<HumanPose>& realised, const PoseRig& rig){
        const size_t n = data.poses.size();
        if (n < 2 || realised.size() != n || !rig.valid()){
            return;
        }
        const size_t segments = data.settings.loop ? n : n - 1;
        applyAutoTiming(data, segmentMotion(realised, segments, rig.humanScale(), [&rig](const HumanPose& a, const HumanPose& b){
            return (Vec3d(rig.hipsWorldPosition(b)) - Vec3d(rig.hipsWorldPosition(a))).length();
        }));
    }

}
