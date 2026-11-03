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

        // Clamps `values` to their ranges keeping their sum at `total` where the ranges allow: the
        // remainder of a clamp is spread over the values still free to move (proportionally).
        // With stepRate > 0 the values are then whole steps (at least one), rounded so the sum is
        // the step count nearest to `total` where the ranges allow (largest remainders first).
        void fitLength(std::vector<double>& values, const std::vector<double>& lo, const std::vector<double>& hi, double total, double stepRate){
            const size_t count = values.size();
            for (size_t i = 0; i < count; i++) values[i] = clampd(values[i], lo[i], hi[i]);
            for (int iteration = 0; iteration < 32 && total > 0.0; iteration++){
                double sum = 0.0;
                for (double v : values) sum += v;
                const double residual = total - sum;
                if (std::fabs(residual) < 1e-12 * std::max(1.0, total)){
                    break;
                }
                double freeSum = 0.0;
                size_t freeCount = 0;
                for (size_t i = 0; i < count; i++){
                    if (residual > 0.0 ? values[i] < hi[i] : values[i] > lo[i]){
                        freeSum += values[i];
                        freeCount++;
                    }
                }
                if (freeCount == 0){
                    break;   // every value at its bound: the residual stays
                }
                for (size_t i = 0; i < count; i++){
                    if (residual > 0.0 ? values[i] < hi[i] : values[i] > lo[i]){
                        const double share = freeSum > 1e-12 ? values[i] / freeSum : 1.0 / static_cast<double>(freeCount);
                        values[i] = clampd(values[i] + residual * share, lo[i], hi[i]);
                    }
                }
            }
            if (!(stepRate > 0.0)){
                return;
            }
            std::vector<long> steps(count), minSteps(count), maxSteps(count);
            long sum = 0;
            for (size_t i = 0; i < count; i++){
                minSteps[i] = std::max(1L, static_cast<long>(std::ceil(lo[i] * stepRate - 1e-9)));
                maxSteps[i] = std::max(minSteps[i], static_cast<long>(std::floor(hi[i] * stepRate + 1e-9)));
                steps[i] = std::min(std::max(std::lround(values[i] * stepRate), minSteps[i]), maxSteps[i]);
                sum += steps[i];
            }
            long diff = std::lround(total * stepRate) - sum;
            while (diff != 0){
                // the value whose rounding is furthest from its exact step count in diff's direction
                long best = -1;
                double bestGap = 0.0;
                for (size_t i = 0; i < count; i++){
                    const double gap = (values[i] * stepRate - static_cast<double>(steps[i])) * (diff > 0 ? 1.0 : -1.0);
                    const bool free = diff > 0 ? steps[i] < maxSteps[i] : steps[i] > minSteps[i];
                    if (free && (best < 0 || gap > bestGap)){
                        best = static_cast<long>(i);
                        bestGap = gap;
                    }
                }
                if (best < 0){
                    break;
                }
                steps[best] += diff > 0 ? 1 : -1;
                diff += diff > 0 ? -1 : 1;
            }
            for (size_t i = 0; i < count; i++){
                values[i] = static_cast<double>(steps[i]) / stepRate;
            }
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

            // the timed values (holds, then the segment durations) and their ranges
            std::vector<double> values(holds);
            std::vector<double> lo(n, 0.0), hi(n, 5.0);
            for (size_t i = 0; i < segments; i++){
                values.push_back(durations[i]);
                lo.push_back(0.02);
                hi.push_back(1.0);
            }
            fitLength(values, lo, hi, oldLength, data.settings.stepRate);

            for (size_t i = 0; i < n; i++){
                PoseKey& key = data.poses[i];
                key.hold = static_cast<float>(values[i]);
                key.customTransition = true;
                key.transition = transitions[i];
                key.transition.duration = static_cast<float>(i < segments ? values[n + i] : clampd(durations[i], 0.02, 1.0));
            }
        }

        // the choppiness: the time sample() poses
        double steppedTime(const PoseAnimationSettings& settings, double t){
            return settings.stepRate > 0.0f ? std::floor(t * settings.stepRate + 1e-4) / settings.stepRate : t;
        }

        // bone b's delayed time (overlap) and looseness at the stepped time t
        void boneTiming(const SamplerInput& input, int b, double t, double& tau, double& loose){
            const PoseAnimationSettings& settings = input.settings;
            const HumanBoneInfo& info = humanBoneInfo(boneAt(b));
            const double weight = settings.weights[static_cast<int>(info.group)];
            tau = t - settings.overlap * weight * info.depth;
            loose = clampd(settings.looseness * weight * info.depth, 0.0, 1.0);
            tau = input.timeline.loop ? positiveFmod(tau, input.timeline.length) : std::max(tau, 0.0);
        }

        // the ease value e of segment s (cycle c: 0, or -1 for a loop's previous cycle) for a bone
        // at its delayed time tau (4.8 step 4 with moving holds and the previous cycle's settling)
        double segmentEase(const SamplerInput& input, double tau, double loose, size_t s, int c){
            const PoseTimeline& timeline = input.timeline;
            const HumanizeSettings& humanize = input.settings.humanize;
            const bool movingHolds = humanize.enabled && humanize.movingHolds > 0.0f;
            const double length = timeline.length;
            const double local = tau - timeline.start[s] - c * length;
            if (c == 0 && local <= 0.0 && !movingHolds){
                return 0.0;   // not started: e = 0 (the wind-up is at x > 0)
            }
            const Transition tr = loose > 0.0 ? loosen(timeline.transitions[s], static_cast<float>(loose)) : timeline.transitions[s];
            double e = easePrecise(local / std::max(static_cast<double>(tr.duration), 1e-4), tr);
            if (movingHolds){
                const double drift = humanize.movingHolds;
                const double hold = timeline.holds[s];
                const double hp = hold > 1e-4 ? smoothstep(0.0, 1.0, (local + hold) / hold) : (local >= 0.0 ? 1.0 : 0.0);
                e = drift * hp + (1.0 - drift) * e;
            }
            if (c < 0){
                // the previous cycle settles exactly by the end of the loop, so the product is
                // the identity at tau = len and the loop is seamless
                e += (1.0 - e) * smoothstep(0.0, 1.0, tau / length);
            }
            return e;
        }

        // The ground weight g of the frame at time t (4.8 frames): 1 in the hold of a grounded key
        // and in a segment between two grounded keys, 0 in a lifted key's hold and between two
        // lifted keys; a launch segment (grounded -> lifted) fades it out and a landing segment
        // (lifted -> grounded) in with smoothstep(clamp01(e)) of the hips' ease, so a wind-up dip
        // (e < 0) stays fully grounded and g is continuous where the hips' motion is. The hold
        // before a launch or a landing uses the same formula: with moving holds the hips' ease of
        // the next segment already drifts during the hold (0 at the hold's start, the drift value
        // at the segment's start), and a flat 0 / 1 there would jump at the segment's start.
        double groundWeight(const SamplerInput& input, double time){
            const PoseTimeline& timeline = input.timeline;
            const size_t n = input.keys.size();
            const size_t segments = std::min(timeline.segmentCount(), timeline.loop ? n : n - 1);
            const double t = steppedTime(input.settings, time);
            auto grounded = [&](size_t key){ return input.grounded[key % n] ? 1.0 : 0.0; };
            // segment s's weight at t (inside it, or in the hold before it)
            auto segmentWeight = [&](size_t s){
                const double from = grounded(s), to = grounded(s + 1);
                if (from == to){
                    return from;
                }
                double tau = 0.0, loose = 0.0;
                boneTiming(input, 0, t, tau, loose);
                return transitionGroundWeight(from > 0.0, to > 0.0, segmentEase(input, tau, loose, s, 0));
            };
            for (size_t s = 0; s < segments; s++){
                if (t < timeline.start[s] + std::max(0.0, static_cast<double>(timeline.transitions[s].duration))){
                    return segmentWeight(s);   // the hold of key s or segment s
                }
            }
            // after the last segment: the last key's hold (a loop's last segment ends at the
            // loop's length, the instant of frame 0 in key 0's hold)
            return grounded(timeline.loop ? 0 : n - 1);
        }

        // The ground correction of the frames: the hips are interpolated linearly while the legs
        // swing, so a crouch to stand dips the body into the floor. A frame with ground weight g
        // and lowest point h above the floor (uncorrected) ends at (1 - g) * max(h, 0): on the
        // floor when g = 1, only pushed out of it when g = 0 (lifts and jumps kept), a blend in
        // between (no pop where a launch leaves the floor or a landing reaches it).
        struct FrameGround {
            double weight = 0.0;    // g
            double shift = 0.0;     // how far the correction moved the lowest point
            HumanPose corrected;    // the frame right after the correction (to detect later changes)
        };

        // moves `frame` (lowest point `lowest`, model space; `shift` already applied) to its
        // corrected height; returns the lowest point it ends at
        double groundFrame(const PoseRig& rig, HumanPose& frame, double weight, double lowest, double shift){
            const double floor = rig.floorHeight();
            const double raw = lowest - shift - floor;   // the height without any correction
            const double target = floor + (1.0 - weight) * std::max(raw, 0.0);
            if (std::fabs(target - lowest) <= 1e-9 * rig.humanScale()){
                return lowest;
            }
            return rig.moveLowestTo(frame, target, lowest, rig.skin());
        }

        // first pass, before follow-through (so it follows the corrected hips path)
        std::vector<FrameGround> groundFrames(std::vector<HumanPose>& result, const SamplerInput& input, double length, int count){
            const PoseRig& rig = *input.rig;
            std::vector<FrameGround> ground(result.size());
            for (int f = 0; f <= count; f++){
                FrameGround& g = ground[f];
                g.weight = groundWeight(input, f * length / count);
                const double lowest = rig.lowestPointPrecise(result[f], rig.skin());
                g.shift = groundFrame(rig, result[f], g.weight, lowest, 0.0) - lowest;
                g.corrected = result[f];
            }
            return ground;
        }

        bool samePose(const HumanPose& a, const HumanPose& b){
            if (!(a.hipsPosition == b.hipsPosition)){
                return false;
            }
            for (int k = 0; k < HUMAN_BONE_COUNT; k++){
                const Quaternion& x = a.rotations[k];
                const Quaternion& y = b.rotations[k];
                if (x.w != y.w || x.x != y.x || x.y != y.y || x.z != y.z){
                    return false;
                }
            }
            return true;
        }

        // final pass: only the frames follow-through or the limits changed are measured again;
        // their uncorrected height is the new lowest point minus the first pass's shift
        void regroundChangedFrames(std::vector<HumanPose>& result, const SamplerInput& input, const std::vector<FrameGround>& ground){
            const PoseRig& rig = *input.rig;
            for (size_t f = 0; f < result.size(); f++){
                if (samePose(result[f], ground[f].corrected)){
                    continue;
                }
                groundFrame(rig, result[f], ground[f].weight, rig.lowestPointPrecise(result[f], rig.skin()), ground[f].shift);
            }
        }

    }

    double transitionGroundWeight(bool fromGrounded, bool toGrounded, double hipsEase){
        if (fromGrounded == toGrounded){
            return fromGrounded ? 1.0 : 0.0;
        }
        const double w = smoothstep(0.0, 1.0, clampd(hipsEase, 0.0, 1.0));
        return fromGrounded ? 1.0 - w : w;
    }

    double groundToWeight(const PoseRig& rig, HumanPose& pose, double weight){
        return groundFrame(rig, pose, weight, rig.lowestPointPrecise(pose, rig.skin()), 0.0);
    }

    void clampPoseToLimits(const PoseRig& rig, HumanPose& pose){
        for (int b = 0; b < HUMAN_BONE_COUNT; b++){
            const HumanBone bone = boneAt(b);
            if (!rig.has(bone)){
                continue;
            }
            float angles[DOFS_PER_BONE];
            rig.anglesFromLocal(bone, pose.rotations[b], angles);
            if (boneWithinLimits(bone, angles)){
                continue;
            }
            // a swing longer than 180 degrees (inside the limits) decomposes to the short
            // form the other way: keep the rotation when its long form is within limits
            const double swing = std::sqrt(static_cast<double>(angles[0]) * angles[0] + static_cast<double>(angles[1]) * angles[1]);
            if (swing > 1e-6){
                const double factor = 1.0 - 360.0 / swing;
                const float longForm[DOFS_PER_BONE] = {static_cast<float>(angles[0] * factor), static_cast<float>(angles[1] * factor), angles[2]};
                bool inside = true;
                for (int k = 0; k < DOFS_PER_BONE; k++){
                    const DofInfo& dof = dofInfo(dofIndex(bone, k));
                    inside = inside && longForm[k] >= dof.minDegrees && longForm[k] <= dof.maxDegrees;
                }
                if (inside){
                    continue;
                }
            }
            // only clamped bones change: the others keep their exact rotation
            clampBoneToLimits(bone, angles);
            pose.rotations[b] = rig.localFromAngles(bone, angles);
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
        for (const PoseKey& key : data.poses){
            input.grounded.push_back(key.grounded);
        }
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

        const double t = steppedTime(settings, time);   // choppiness
        const bool life = humanize.enabled && humanize.life > 0.0f;

        for (int b = 0; b < HUMAN_BONE_COUNT; b++){
            const HumanBoneInfo& info = humanBoneInfo(boneAt(b));
            double tau = 0.0, loose = 0.0;
            boneTiming(input, b, t, tau, loose);

            Quatd r = Quatd(keys[0].rotations[b]).normalized();
            Vec3d hips(keys[0].hipsPosition);
            const int firstCycle = timeline.loop ? -1 : 0;
            for (int c = firstCycle; c <= 0; c++){
                for (size_t s = 0; s < segments; s++){
                    const double e = segmentEase(input, tau, loose, s, c);
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

        // ground correction (before follow-through, which then follows the corrected hips path)
        const bool groundCorrection = input.rig && input.keys.size() > 1 && input.grounded.size() == input.keys.size();
        std::vector<FrameGround> ground;
        if (groundCorrection){
            ground = groundFrames(result, input, length, count);
        }

        if (input.rig && humanize.enabled && (humanize.torsoFollow > 0.0f || humanize.headFollow > 0.0f)){
            applyFollowThrough(result, *input.rig, fps, loop, humanize);
        }

        if (input.rig && humanize.jointLimits){
            for (HumanPose& frame : result){
                clampPoseToLimits(*input.rig, frame);
            }
        }

        // follow-through and the limits can move the lowest point: those frames are corrected again
        if (groundCorrection){
            regroundChangedFrames(result, input, ground);
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
