// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (S77 R19, R22, R24, R46, D2, D6)
// - Purpose: the pipeline behind optimize_animation and the Inspector's Reduce Keys... dialog, so a
//   person and an agent get the same numbers: load the source as it plays, reduce it (ClipReduction),
//   record the recipe, and measure what the file form of the result does against the source.
//   verifyWrittenClip measures again from the bytes on disk: the error a caller reports after a write
//   is never the one from before it.
// - Writes nothing itself. Who writes does it as one undo step (a project step, or with `root` the
//   scene step of ClipSceneActions::writeOptimizedClips).
// - Thread-safety: any thread; `rig` and `cancel` stay valid for the call. The pool's load takes its
//   own lock, and a clip this call loaded leaves the pool again.
// - Editor only: nothing here compiles into a player.

#pragma once

#include "animation/ClipReduction.h"
#include "animation/ClipReport.h"

#include "json.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::anim{

    struct ClipOptimizeRequest{
        // what plays: an asset-relative or absolute .anim path, or "<model>#<clip>"
        std::string reference;
        // how the clip is named to the caller, and in the output's record ("assets/anim/walk.anim")
        std::string displayName;
        const ClipRig* rig = nullptr;
        ClipReductionOptions reduction;
        // the output replaces the source: its record then names no source to go stale against
        bool inPlace = false;
        const std::atomic<bool>* cancel = nullptr;
    };

    struct ClipOptimizeResult{
        bool ok = false;
        std::string errorKind;              // read_failed, non_finite_key, unknown_joint, nothing_measured, cancelled
        std::string error;
        std::string fix;
        std::string displayName;

        std::shared_ptr<const AnimationClip> source;    // as it plays: the reference of every measurement
        ClipData reduced;                   // with its `optimize` record
        ClipReport before;
        ClipReport after;
        ClipErrorReport measured;           // the file form of `reduced` against `source`
        ClipSkippedTracks skipped;
        double toleranceMm = 0.0;
        double effectiveToleranceMm = 0.0;
        double skinDistanceMm = 0.0;
        bool keptAllKeys = false;           // no per-track value met the limit
        size_t eventsKept = 0;
        std::string writtenTo;              // project-relative, once written
    };

    ClipOptimizeResult runClipOptimize(const ClipOptimizeRequest& request);

    // Reads `file` back and measures it against result.source: result.after and result.measured are
    // then what is on disk. False + `error` when it can't be read, is not a clip or nothing could be
    // measured.
    bool verifyWrittenClip(const std::filesystem::path& file, const ClipRig& rig, ClipOptimizeResult& result, std::string& error);

    // {clip, written_to, bytes_before, bytes_after, keys_before, keys_after, max_error_mm, mean_error_mm,
    // worst_joint, worst_time_s, skin_distance_mm, skipped_tracks, kept: {events}, kept_all_keys}
    nlohmann::json clipOptimizeJson(const ClipOptimizeResult& result);

    // the model a clip measures on when the caller names none: a sub-clip's model, else the clip's
    // source model, else its preview when that is a model; "" when it names none
    std::string clipRigModel(const std::string& reference, const ClipData& data);

    // the same for a reference, reading its file when it needs to; "" when it names none or can't be read
    std::string clipRigModelOf(const std::string& reference);

    // SHA-1 of the file behind a reference (a sub-clip's model), "" when it can't be read
    std::string clipSourceSha1(const std::string& reference);

}
