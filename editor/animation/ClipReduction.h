// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (S77 R20-R23, D5)
// - Purpose: removes the keys of a clip that a skeleton does not need. A hierarchical error metric
//   plus Ramer-Douglas-Peucker decimation, the idea of ozz-animation's AnimationOptimizer, written
//   for this engine (NOTICE.md); its differences are D5's table:
//   * double precision, and rotations renormalised before their dot product (rotationChordMetres);
//   * the engine's own sampler interpolates, so the metric judges what plays;
//   * a constant track keeps 1 key, a moving one its first and last, and no track is ever removed:
//     a track missing means rest here, and a looping track's period is its last key;
//   * a bone with no translation keys reaches as far as its rest translation;
//   * tolerance_mm is a bound on the error measureClipError reads from the output, not a per-track
//     budget: the per-track value is bisected until it holds.
// - Output: keys are a strict subset of the source's, in order, with their values unchanged. Clip
//   length, events, audio cues (S40) and every track it cannot measure stay as they are.
// - No reporting path: the result says what was kept, never how good it is. Whoever wants the error
//   measures it (ClipOptimize, ClipReport).
// - Thread-safety: any thread; nothing shared. Deterministic: the same input gives the same bytes.
// - Editor only: nothing here compiles into a player.

#pragma once

#include "animation/ClipReport.h"

#include <atomic>
#include <string>
#include <vector>

namespace jasperengine::anim{

    inline constexpr double kClipReductionSkinFloorMm = 10.0;

    struct ClipJointTolerance{
        std::string path;           // a bone's path or name; a trailing '*' matches any rest
        double toleranceMm = 0.0;
    };

    struct ClipReductionOptions{
        double toleranceMm = 1.0;
        double skinDistanceMm = 100.0;  // raised to the floor: at 0 a leaf's rotation goes free
        std::vector<ClipJointTolerance> joints;
    };

    struct ClipReductionResult{
        bool ok = false;
        std::string errorKind;      // non_finite_key, unknown_joint, cancelled
        std::string error;
        ClipData clip;              // the reduced clip (ok); its tracks are the source's, normalized
        // min(tolerance, 0.1 % of the height of what the clip moves): R22
        double effectiveToleranceMm = 0.0;
        // the search found no per-track value within the limit: `clip` keeps every key
        bool keptAllKeys = false;
        int searchSteps = 0;        // R22: how many per-track values it measured
    };

    // The distance a point `radius` metres from a joint moves when the joint turns from `a` to `b`:
    // the chord of the angle. Both are renormalised in double first: a float32 quaternion is off unit
    // by about 1e-7, and an .anim's 6 digits by up to 1e-5, and that alone reads an error under about
    // 0.7 mm per metre as 0, which is the difference between a real bound and a false one.
    double rotationChordMetres(const Quaternion& a, const Quaternion& b, double radius);

    // What one joint may reach, in mm, per rig bone: its override, else half the tolerance on a foot
    // (a planted foot's path shows under 1 mm of joint error), else the tolerance; none above
    // `heightCapMm` (R22). False + `unknown` when an override names no bone.
    bool clipJointLimits(const ClipRig& rig, const ClipReductionOptions& options, double heightCapMm,
                         std::vector<double>& limits, std::string* unknown);

    // Reduces at one per-track value: `ratio` of each joint's limit, in the hierarchical metric.
    // `limits` are clipJointLimits'; a tightened joint tightens its ancestors (up-chain min), since
    // they move it too, but neither its siblings nor its children. The step inside reduceClip,
    // public for tests of the metric. `source` is normalized.
    ClipData reduceClipAtRatio(const ClipData& source, const AnimationClip& built, const ClipRig& rig, const std::vector<double>& limits,
                               double skinDistanceMm, double ratio);

    // R22's search: at most 8 steps, the first at the full tolerance, each measured through the file
    // form of the clip (6 digits a value) with the runtime sampler
    ClipReductionResult reduceClip(const ClipData& source, const ClipRig& rig, const ClipReductionOptions& options,
                                   const std::atomic<bool>* cancel = nullptr);

}
