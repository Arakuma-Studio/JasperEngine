// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ClipReduction.h"

#include "animation/AnimationClipIO.h"
#include "animation/AnimatorPose.h"
#include "animation/KeyframeSampling.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

using namespace jasperengine;

namespace{

    constexpr int kMaxSearchSteps = 8;
    constexpr double kRigHeightFraction = 0.001;    // R22: 0.1 %
    // where the search stops looking down: a thousandth of the limit is below anything sampling shows
    constexpr double kLowestRatio = 1e-3;

    struct Metric{
        std::vector<double> scale;      // accumulated, from the largest key of each bone
        std::vector<double> ownScale;   // the bone's own largest scale component
        std::vector<double> reach;      // metres from the joint to the farthest point it moves
        std::vector<double> tolerance;  // metres, after the up-chain min
    };

    double largestComponent(const Vector3& value){
        return std::max({std::fabs((double)value.x), std::fabs((double)value.y), std::fabs((double)value.z)});
    }

    double lengthOf(const Vector3& value){
        const double x = value.x, y = value.y, z = value.z;
        return std::sqrt(x * x + y * y + z * z);
    }

    Vector3 vectorOf(const ClipKey& key){
        return Vector3(key.value[0], key.value[1], key.value[2]);
    }

    // The forward and backward pass over the rig: reach is what makes a hip's rotation cost more
    // than a finger's
    Metric makeMetric(const ClipData& source, const anim::ClipRig& rig, const std::vector<double>& limits, double skinMetres, double ratio){
        const size_t bones = rig.size();
        Metric metric;
        metric.scale.assign(bones, 1.0);
        metric.ownScale.assign(bones, 1.0);
        metric.reach.assign(bones, skinMetres);
        metric.tolerance.resize(bones);
        for (size_t b = 0; b < bones; b++) metric.tolerance[b] = limits[b] * ratio / 1000.0;

        std::vector<const ClipTrackData*> positions(bones, nullptr), scales(bones, nullptr);
        for (const ClipTrackData& track : source.tracks){
            if (track.channel != ClipChannel::Position && track.channel != ClipChannel::Scale) continue;
            const int bone = rig.findBone(track.path);
            if (bone < 0 || track.keys.empty()) continue;
            (track.channel == ClipChannel::Position ? positions : scales)[(size_t)bone] = &track;
        }
        for (size_t b = 0; b < bones; b++){
            double own = largestComponent(rig.restScales[b]);
            if (const ClipTrackData* track = scales[b]){
                own = 0.0;
                for (const ClipKey& key : track->keys){
                    const Vector3 value = track->relative ? rig.restScales[b] * vectorOf(key) : vectorOf(key);
                    own = std::max(own, largestComponent(value));
                }
            }
            metric.ownScale[b] = own;
            metric.scale[b] = own * (rig.parents[b] >= 0 ? metric.scale[(size_t)rig.parents[b]] : 1.0);
        }
        for (size_t i = bones; i-- > 0;){
            const int parent = rig.parents[i];
            if (parent < 0) continue;
            // no translation keys (glTF often has none on bones): the rest translation is the bone
            double length = lengthOf(rig.restPositions[i]);
            if (const ClipTrackData* track = positions[i]){
                length = 0.0;
                for (const ClipKey& key : track->keys){
                    const Vector3 value = track->relative ? rig.restPositions[i] + vectorOf(key) : vectorOf(key);
                    length = std::max(length, lengthOf(value));
                }
            }
            metric.reach[(size_t)parent] = std::max(metric.reach[(size_t)parent], metric.reach[i] + length * metric.scale[(size_t)parent]);
            metric.tolerance[(size_t)parent] = std::min(metric.tolerance[(size_t)parent], metric.tolerance[i]);
        }
        return metric;
    }

    // The sampler's location of `time` in the segment first..last, as sampleTrack computes it
    anim::KeyLocation segmentLocation(float first, float last, float time){
        const float times[2] = {first, last};
        return anim::locateKey(times, 2, nullptr, 0, time);
    }

    // Keeps the first and last key and every key whose segment's interpolation misses it by more
    // than `tolerance`, splitting at the worst each time. `mustSplit` forces a split whatever the
    // error (a rotation segment whose ends lie on opposite hemispheres).
    std::vector<uint8_t> decimate(size_t count, double tolerance, const std::function<double(size_t, size_t, size_t)>& errorAt,
                                  const std::function<bool(size_t, size_t)>& mustSplit){
        std::vector<uint8_t> keep(count, 0);
        keep[0] = keep[count - 1] = 1;
        std::vector<std::pair<size_t, size_t>> stack = {{0, count - 1}};
        while (!stack.empty()){
            const auto [first, last] = stack.back();
            stack.pop_back();
            if (last - first < 2) continue;
            size_t split = first;
            double worst = -1.0;
            const bool forced = mustSplit(first, last);
            for (size_t k = first + 1; k < last; k++){
                const double error = errorAt(first, last, k);
                if ((forced || error > tolerance) && error > worst){
                    worst = error;
                    split = k;
                }
            }
            if (split == first) continue;
            keep[split] = 1;
            stack.push_back({first, split});
            stack.push_back({split, last});
        }
        return keep;
    }

    // The keys of one track to keep, with a constant track down to its first key
    std::vector<uint8_t> keysToKeep(const ClipTrackData& track, const anim::ClipRig& rig, const Metric& metric, size_t bone){
        const std::vector<ClipKey>& keys = track.keys;
        const double tolerance = metric.tolerance[bone];
        std::function<double(size_t, size_t)> keyDifference;
        std::function<double(size_t, size_t, size_t)> errorAt;
        std::function<bool(size_t, size_t)> mustSplit = [](size_t, size_t){ return false; };
        if (track.channel == ClipChannel::Rotation){
            const double radius = metric.reach[bone];
            auto quaternionAt = [&keys](size_t k){ return quaternionFromXYZW(keys[k].value.data()); };
            errorAt = [&keys, quaternionAt, radius](size_t first, size_t last, size_t k){
                const Quaternion ends[2] = {quaternionAt(first), quaternionAt(last)};
                const anim::KeyLocation at = segmentLocation(keys[first].time, keys[last].time, keys[k].time);
                return anim::rotationChordMetres(anim::sampleQuaternion(at, ends, nullptr, nullptr), quaternionAt(k), radius);
            };
            keyDifference = [quaternionAt, radius](size_t a, size_t b){ return anim::rotationChordMetres(quaternionAt(a), quaternionAt(b), radius); };
            // the reader keeps each key on the hemisphere of the one before: kept ends on opposite ones
            // would come back with other signs, and the sampler's short way round is not the path
            mustSplit = [&keys](size_t first, size_t last){
                const std::vector<float>& a = keys[first].value;
                const std::vector<float>& b = keys[last].value;
                return (double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2] + (double)a[3] * b[3] < 0.0;
            };
        }else{
            // translation error moves what hangs on the parent; a scale error moves what the bone reaches,
            // whose size in the bone's own space is the reach over the scale
            const int parent = rig.parents[bone];
            const double parentScale = parent >= 0 ? metric.scale[(size_t)parent] : 1.0;
            const double factor = track.channel == ClipChannel::Position ? parentScale
                                  : metric.reach[bone] / std::max(metric.ownScale[bone], 1e-6);
            auto difference = [factor](const Vector3& a, const Vector3& b){
                const double x = (double)a.x - b.x, y = (double)a.y - b.y, z = (double)a.z - b.z;
                return std::sqrt(x * x + y * y + z * z) * factor;
            };
            errorAt = [&keys, difference](size_t first, size_t last, size_t k){
                const Vector3 ends[2] = {vectorOf(keys[first]), vectorOf(keys[last])};
                const anim::KeyLocation at = segmentLocation(keys[first].time, keys[last].time, keys[k].time);
                return difference(anim::sampleVector3(at, ends, nullptr, nullptr), vectorOf(keys[k]));
            };
            keyDifference = [&keys, difference](size_t a, size_t b){ return difference(vectorOf(keys[a]), vectorOf(keys[b])); };
        }
        std::vector<uint8_t> keep = decimate(keys.size(), tolerance, errorAt, mustSplit);
        if (std::count(keep.begin(), keep.end(), (uint8_t)1) == 2 && keyDifference(0, keys.size() - 1) <= tolerance) keep[keys.size() - 1] = 0;
        return keep;
    }

    // A file form round trip: the 6 digits a value that Save writes are part of what plays
    std::shared_ptr<const AnimationClip> throughFile(const ClipData& data){
        return AnimationClip::build(anim::parseClip(anim::writeClip(data)));
    }

    bool withinLimits(const anim::ClipErrorReport& report, const std::vector<double>& limits){
        if (!report.measured) return false;
        for (size_t b = 0; b < limits.size(); b++){
            if (report.jointMaxMm[b] > limits[b]) return false;
        }
        return true;
    }

    bool matchesPattern(const std::string& pattern, const std::string& text){
        if (!pattern.empty() && pattern.back() == '*') return text.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0;
        return pattern == text;
    }

    const char* channelLabel(ClipChannel channel){
        switch (channel){
            case ClipChannel::Position: return "position";
            case ClipChannel::Rotation: return "rotation";
            case ClipChannel::Scale: return "scale";
            case ClipChannel::Morph: return "morph";
            case ClipChannel::Lattice: return "lattice";
            case ClipChannel::Property: return "property";
            case ClipChannel::Humanoid: return "humanoid";
            case ClipChannel::HumanoidPosition: return "humanoid_position";
        }
        return "";
    }

    bool findNonFinite(const ClipData& data, std::string& where){
        for (const ClipTrackData& track : data.tracks){
            for (size_t k = 0; k < track.keys.size(); k++){
                const ClipKey& key = track.keys[k];
                bool finite = std::isfinite(key.time);
                for (const std::vector<float>* row : {&key.value, &key.inTangent, &key.outTangent}){
                    for (float value : *row) finite = finite && std::isfinite(value);
                }
                if (!finite){
                    where = "track \"" + track.path + "\" " + channelLabel(track.channel) + " key " + std::to_string(k);
                    return true;
                }
            }
        }
        return false;
    }

    // the rest-pose extent, on its longest axis, of the bones the clip moves: a mesh node a long way
    // off in the file does not make a small character tall
    double movedHeightMetres(const ClipData& data, const anim::ClipRig& rig){
        const size_t bones = rig.size();
        std::vector<uint8_t> moved(bones, 0);
        for (const ClipTrackData& track : data.tracks){
            const int bone = rig.findBone(track.path);
            if (bone >= 0) moved[(size_t)bone] = 1;
        }
        for (size_t b = 0; b < bones; b++){
            if (rig.parents[b] >= 0 && moved[(size_t)rig.parents[b]]) moved[b] = 1;
        }
        anim::Pose rest(bones, 0);
        for (size_t b = 0; b < bones; b++){
            rest.setPosition(b, rig.restPositions[b]);
            rest.setRotation(b, rig.restRotations[b]);
            rest.setScale(b, rig.restScales[b]);
        }
        std::vector<anim::Affine> world(bones);
        std::vector<Vector3> positions(bones), scales(bones);
        std::vector<Quaternion> rotations(bones);
        anim::poseToRigSpace(rest, rig.parents.data(), anim::RigSpace{world.data(), positions.data(), rotations.data(), scales.data()});
        double low[3] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
        double high[3] = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
        bool any = false;
        for (size_t b = 0; b < bones; b++){
            if (!moved[b]) continue;
            any = true;
            for (int axis = 0; axis < 3; axis++){
                low[axis] = std::min(low[axis], (double)world[b].m[9 + axis]);
                high[axis] = std::max(high[axis], (double)world[b].m[9 + axis]);
            }
        }
        if (!any) return 0.0;
        return std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]});
    }

}

double anim::rotationChordMetres(const Quaternion& a, const Quaternion& b, double radius){
    const double la = std::sqrt((double)a.w * a.w + (double)a.x * a.x + (double)a.y * a.y + (double)a.z * a.z);
    const double lb = std::sqrt((double)b.w * b.w + (double)b.x * b.x + (double)b.y * b.y + (double)b.z * b.z);
    if (la <= 0.0 || lb <= 0.0) return 0.0;
    const double cosHalf = ((double)a.w * b.w + (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z) / (la * lb);
    // squaring takes out the sign: q and -q are one rotation
    return 2.0 * std::sqrt(std::max(0.0, 1.0 - std::min(1.0, cosHalf * cosHalf))) * radius;
}

bool anim::clipJointLimits(const ClipRig& rig, const ClipReductionOptions& options, double heightCapMm,
                           std::vector<double>& limits, std::string* unknown){
    const double tolerance = std::min(options.toleranceMm, heightCapMm);
    limits.resize(rig.size());
    for (size_t b = 0; b < rig.size(); b++) limits[b] = rig.feet[b] ? tolerance * 0.5 : tolerance;
    for (const ClipJointTolerance& joint : options.joints){
        bool matched = false;
        for (size_t b = 0; b < rig.size(); b++){
            if (!matchesPattern(joint.path, rig.paths[b]) && !matchesPattern(joint.path, rig.names[b])) continue;
            matched = true;
            limits[b] = std::min(joint.toleranceMm, heightCapMm);
        }
        if (!matched){
            if (unknown) *unknown = joint.path;
            return false;
        }
    }
    return true;
}

ClipData anim::reduceClipAtRatio(const ClipData& source, const AnimationClip& built, const ClipRig& rig, const std::vector<double>& limits,
                                 double skinDistanceMm, double ratio){
    const Metric metric = makeMetric(source, rig, limits, std::max(skinDistanceMm, kClipReductionSkinFloorMm) / 1000.0, ratio);
    ClipData out = source;
    const std::vector<AnimationClip::Track>& tracks = built.getTracks();
    for (size_t t = 0; t < out.tracks.size() && t < tracks.size(); t++){
        ClipTrackData& track = out.tracks[t];
        if (tracks[t].path != track.path || tracks[t].channel != track.channel || !clipTrackIsReducible(tracks[t], rig)) continue;
        const std::vector<uint8_t> keep = keysToKeep(track, rig, metric, (size_t)rig.findBone(track.path));
        std::vector<ClipKey> kept;
        kept.reserve(track.keys.size());
        for (size_t k = 0; k < track.keys.size(); k++){
            if (keep[k]) kept.push_back(std::move(track.keys[k]));
        }
        track.keys = std::move(kept);
    }
    // The length is the last key or event when it is not explicit: if every moving track ended early
    // and the track that held the end was constant, that track keeps its last key after all
    if (!source.hasExplicitLength()){
        const float length = source.effectiveLength();
        if (out.effectiveLength() < length){
            for (size_t t = 0; t < out.tracks.size(); t++){
                const std::vector<ClipKey>& keys = source.tracks[t].keys;
                if (keys.empty() || keys.back().time != length || out.tracks[t].keys.size() == keys.size()) continue;
                out.tracks[t].keys.push_back(keys.back());
                break;
            }
        }
    }
    return out;
}

anim::ClipReductionResult anim::reduceClip(const ClipData& input, const ClipRig& rig, const ClipReductionOptions& options,
                                           const std::atomic<bool>* cancel){
    ClipReductionResult result;
    std::string where;
    if (findNonFinite(input, where)){
        result.errorKind = "non_finite_key";
        result.error = where + " is not a finite number; re-export the clip";
        return result;
    }
    ClipData source = input;
    normalize(source);
    // what plays now is the reference, so what the file form rounds away counts as error
    const std::shared_ptr<const AnimationClip> built = AnimationClip::build(source);
    const double skin = std::max(options.skinDistanceMm, kClipReductionSkinFloorMm);
    const double height = movedHeightMetres(source, rig);
    const double heightCapMm = height > 0.0 ? height * kRigHeightFraction * 1000.0 : std::numeric_limits<double>::max();
    result.effectiveToleranceMm = std::min(options.toleranceMm, heightCapMm);
    std::vector<double> limits;
    std::string unknown;
    if (!clipJointLimits(rig, options, heightCapMm, limits, &unknown)){
        result.errorKind = "unknown_joint";
        result.error = "joints: no bone of " + rig.source + " matches \"" + unknown + "\"";
        return result;
    }
    result.clip = source;
    result.keptAllKeys = true;
    double low = kLowestRatio, high = 1.0;
    for (int step = 0; step < kMaxSearchSteps; step++){
        if (cancel && cancel->load(std::memory_order_relaxed)){
            result.errorKind = "cancelled";
            result.error = "cancelled";
            return result;
        }
        result.searchSteps = step + 1;
        const double ratio = step == 0 ? 1.0 : std::sqrt(low * high);
        ClipData candidate = reduceClipAtRatio(source, *built, rig, limits, skin, ratio);
        const ClipErrorReport measured = measureClipError(*built, *throughFile(candidate), rig, skin, cancel);
        if (withinLimits(measured, limits)){
            result.clip = std::move(candidate);
            result.keptAllKeys = false;
            if (step == 0) break;
            low = ratio;
        }else{
            high = ratio;
        }
    }
    result.ok = true;
    return result;
}
