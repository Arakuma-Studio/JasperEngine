// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (S74 R18, D2, D3)
// - Purpose: the one producer of a clip's numbers in the editor. analyzeClip fills a ClipReport;
//   panels and tools print its struct and never calculate. kClipReportFields names every number
//   once (the JSON key agents read, the label people read, its unit and how it prints), so the
//   two cannot disagree: a gtest holds the table and clipReportJson equal.
// - The stats (Memory, File, Keys, Tracks, Length, Constant Tracks, Audio Cues) need no rig. Loop Match, the root
//   motion facts and a comparison measure on a ClipRig (D5, R19). A new number adds a
//   member, a table row and a JSON key.
// - Thread-safety: any thread (AnimationClipPool takes its own lock). `rig` and `compareTo` stay
//   valid for the call.
// - Editor only: nothing here compiles into a player.

#pragma once

#include "animation/AnimationClip.h"
#include "animation/ModelSubClips.h"

#include "json.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jasperengine::anim{

    // The skeleton a clip's error is measured on: a model file's nodes or a GameObject's hierarchy,
    // parents first. A channel with no track holds the rest values here, as it does when played.
    struct ClipRig{
        std::string source;                 // what it came from, for people ("assets/models/hero.glb")
        std::vector<std::string> names;
        std::vector<std::string> paths;     // relative to the root, as tracks bind
        std::vector<int> parents;           // parents[i] < i; -1 under the root
        std::vector<Vector3> restPositions;
        std::vector<Quaternion> restRotations;
        std::vector<Vector3> restScales;
        std::vector<uint8_t> feet;          // feet and toes: the humanoid map's, else by name (R23)
        int hips = -1;                      // the humanoid map's, -1 when there is none

        size_t size() const { return parents.size(); }
        int findBone(const std::string& path) const;
        // from bones in any order, each naming its parent's position in that order (-1 none). Marks
        // feet by name; a parent cycle counts as no parent.
        void assign(const std::string& sourceName, const std::vector<ModelNodeInfo>& nodes);

    private:
        std::unordered_map<std::string, int> byPath;
    };

    // Any thread (ModelSubClips' rule). The model's humanoid map, when its sidecar says Humanoid,
    // adds to the feet and sets the hips.
    bool clipRigFromModel(const std::string& modelPath, ClipRig& out, std::string* error);

    // Why a reduction leaves a track alone (D5). Every one is counted, none silently.
    struct ClipSkippedTracks{
        size_t tangents = 0;    // Hermite segments
        size_t eases = 0;       // a non-linear ease on some segment
        size_t step = 0;        // every segment a step: kept whole
        size_t morph = 0;
        size_t lattice = 0;
        size_t property = 0;
        size_t humanoid = 0;    // humanoid channels name a slot, not a bone
        size_t unbound = 0;     // its path is not on the rig: nothing can measure it

        size_t total() const { return tangents + eases + step + morph + lattice + property + humanoid + unbound; }
    };

    // Whether a track can lose keys; when not, `skipped` (when given) counts why. A single-key track
    // has nothing to lose and counts nowhere.
    bool clipTrackIsReducible(const AnimationClip::Track& track, const ClipRig& rig, ClipSkippedTracks* skipped = nullptr);

    // Measured, never estimated (R19). The worst joint and time are of max_error.
    struct ClipErrorReport{
        bool measured = false;              // false: no track of either clip moves a bone of the rig
        double maxErrorMm = 0.0;
        double meanErrorMm = 0.0;           // over joints and samples, each its worst point
        std::string worstJoint;
        float worstTimeSeconds = 0.0f;
        double skinDistanceMm = 0.0;
        size_t samples = 0;
        std::vector<double> jointMaxMm;     // per rig bone; 0 for the bones nothing moves
    };

    // Samples both clips with sampleTrack at every key time of both and halfway between them, runs
    // poseToRigSpace, and compares each moved joint's origin and the six points skinDistanceMm from
    // it along its axes. Any thread. `cancel` stops it early: measured stays false.
    ClipErrorReport measureClipError(const AnimationClip& reference, const AnimationClip& other, const ClipRig& rig,
                                     double skinDistanceMm, const std::atomic<bool>* cancel = nullptr);

    // Loop Match: the pose at the end against the pose at the start, the root's travel (horizontal
    // position and heading) taken out, so a clip that walks has a seam only where its body jumps
    struct ClipLoopSeam{
        double maxErrorMm = 0.0;
        std::string worstJoint;
    };

    struct ClipRootMotionFacts{
        std::string bone;                   // the runtime's choice: the clip's, the hips, the shallowest moved bone
        Vector3 netTranslationMetres = Vector3(0, 0, 0);
        double netYawDegrees = 0.0;
        double averageSpeedMps = 0.0;       // horizontal, net over the length
        double averageYawDegS = 0.0;
        double loopGapMm = 0.0;             // height difference between the two ends
    };

    // What a comparison found besides the bones (they have no mm to report)
    struct ClipCompareFacts{
        std::string reference;
        std::optional<double> toleranceMm;
        std::optional<bool> withinTolerance;    // only with a tolerance
        size_t unmeasuredDifferences = 0;       // tracks that are not bones and differ, or are in one clip only
        bool eventsMatch = true;
        bool audioCuesMatch = true;
        bool lengthMatches = true;
        ClipErrorReport error;
        nlohmann::json worstJoints;             // clipWorstJointsJson: the Worst Joints row
    };

    // The checks beyond the stats are off unless asked for, so a stats request costs what it did.
    struct ClipReportRequest{
        // an .anim path or "<model>#<clip>", in any form AnimationClipPool takes. Loaded through
        // the pool, so the numbers are those of what plays; a clip this call loaded leaves the
        // pool again, so asking does not change Clip memory
        std::string reference;
        // a clip already built (an unsaved result): reported as it is, `reference` only names its
        // file for file_bytes
        std::shared_ptr<const AnimationClip> clip;
        // what the rig checks below measure on (null: they cannot run and stay empty)
        const ClipRig* rig = nullptr;
        bool loopSeam = false;
        bool rootMotion = false;
        std::shared_ptr<const AnimationClip> compareTo;
        std::string compareToName;              // how the report names it
        std::optional<double> toleranceMm;
        double skinDistanceMm = 100.0;
        const std::atomic<bool>* cancel = nullptr;
    };

    struct ClipReport{
        bool ok = false;
        std::string error;                      // !ok: why the clip could not be read
        uint64_t runtimeBytes = 0;              // AnimationClip::memoryBytes()
        std::optional<uint64_t> fileBytes;      // the .anim on disk; none for a model clip
        size_t keys = 0;                        // over all tracks
        size_t tracks = 0;
        float lengthSeconds = 0.0f;
        float frameRate = 0.0f;                 // the info line's Hz (not a measurement: no field)
        ClipWrapMode wrap = ClipWrapMode::Once; // what says a seam matters (no field: get_animation has it)
        // tracks of more than one key that all hold exactly the same value (and no tangents that
        // leave it): they cost keys and never move. A single-key track is already minimal
        size_t constantTracks = 0;
        size_t audioCues = 0;                   // S40's cues: kept whole by a reduction, as events
        // set when the request has a rig
        std::optional<ClipSkippedTracks> skippedTracks;
        std::optional<ClipLoopSeam> loopSeam;
        std::optional<ClipRootMotionFacts> rootMotion;
        std::optional<ClipCompareFacts> compare;
    };

    ClipReport analyzeClip(const ClipReportRequest& request);

    enum class ClipFieldFormat{ Bytes, Count, Seconds, Millimetres, MetresPerSecond, DegreesPerSecond, Degrees, Metres, Text, Flag };

    // Where a field lives: Stats at the top of `stats`, Compare under `compare`, the other reports
    // under their own dotted key, Reduction in optimize_animation's clip entries
    enum class ClipFieldSection{ Stats, Skipped, Compare, LoopSeam, RootMotion, Reduction };

    struct ClipReportField{
        const char* key;        // the JSON key
        const char* label;      // the Inspector's label
        const char* unit;
        ClipFieldFormat format;
        ClipFieldSection section;
    };

    inline constexpr ClipReportField kClipReportFields[] = {
        {"runtime_bytes", "Memory", "bytes", ClipFieldFormat::Bytes, ClipFieldSection::Stats},
        {"file_bytes", "File", "bytes", ClipFieldFormat::Bytes, ClipFieldSection::Stats},
        {"keys", "Keys", "count", ClipFieldFormat::Count, ClipFieldSection::Stats},
        {"tracks", "Tracks", "count", ClipFieldFormat::Count, ClipFieldSection::Stats},
        {"length_s", "Length", "s", ClipFieldFormat::Seconds, ClipFieldSection::Stats},
        {"constant_tracks", "Constant Tracks", "count", ClipFieldFormat::Count, ClipFieldSection::Stats},
        {"audio_cues", "Audio Cues", "count", ClipFieldFormat::Count, ClipFieldSection::Stats},

        {"skipped_tracks", "Skipped Tracks", "count", ClipFieldFormat::Count, ClipFieldSection::Skipped},

        {"reference", "Compared To", "name", ClipFieldFormat::Text, ClipFieldSection::Compare},
        {"tolerance_mm", "Error Tolerance", "mm", ClipFieldFormat::Millimetres, ClipFieldSection::Compare},
        {"within_tolerance", "Within Tolerance", "flag", ClipFieldFormat::Flag, ClipFieldSection::Compare},
        {"skin_distance_mm", "Skin Distance", "mm", ClipFieldFormat::Millimetres, ClipFieldSection::Compare},
        {"max_error_mm", "Max Error", "mm", ClipFieldFormat::Millimetres, ClipFieldSection::Compare},
        {"mean_error_mm", "Mean Error", "mm", ClipFieldFormat::Millimetres, ClipFieldSection::Compare},
        {"worst_joint", "Worst Joint", "name", ClipFieldFormat::Text, ClipFieldSection::Compare},
        {"worst_time_s", "at time", "s", ClipFieldFormat::Seconds, ClipFieldSection::Compare},
        {"joints", "Worst Joints", "mm", ClipFieldFormat::Text, ClipFieldSection::Compare},
        {"unmeasured_differences", "Other Tracks Differ", "count", ClipFieldFormat::Count, ClipFieldSection::Compare},
        {"events_match", "Events Match", "flag", ClipFieldFormat::Flag, ClipFieldSection::Compare},
        {"audio_cues_match", "Audio Cues Match", "flag", ClipFieldFormat::Flag, ClipFieldSection::Compare},
        {"length_matches", "Length Matches", "flag", ClipFieldFormat::Flag, ClipFieldSection::Compare},

        {"loop_seam.max_error_mm", "Loop Match", "mm", ClipFieldFormat::Millimetres, ClipFieldSection::LoopSeam},
        {"loop_seam.worst_joint", "Loop Match Joint", "name", ClipFieldFormat::Text, ClipFieldSection::LoopSeam},

        {"root_motion.bone", "Root Motion Node", "name", ClipFieldFormat::Text, ClipFieldSection::RootMotion},
        {"root_motion.net_translation_m", "Net Translation", "m", ClipFieldFormat::Metres, ClipFieldSection::RootMotion},
        {"root_motion.net_yaw_deg", "Net Yaw", "deg", ClipFieldFormat::Degrees, ClipFieldSection::RootMotion},
        {"root_motion.average_speed_mps", "Average Speed (Root Transform)", "m/s", ClipFieldFormat::MetresPerSecond, ClipFieldSection::RootMotion},
        {"root_motion.average_yaw_deg_s", "Average Yaw (Root Transform)", "deg/s", ClipFieldFormat::DegreesPerSecond, ClipFieldSection::RootMotion},
        {"root_motion.loop_gap_mm", "Loop Gap", "mm", ClipFieldFormat::Millimetres, ClipFieldSection::RootMotion},

        // D6: bytes_before and bytes_after are runtime_bytes of the two clips, under one label
        {"bytes_before", "Memory", "bytes", ClipFieldFormat::Bytes, ClipFieldSection::Reduction},
        {"bytes_after", "Memory", "bytes", ClipFieldFormat::Bytes, ClipFieldSection::Reduction},
        {"keys_before", "Keys", "count", ClipFieldFormat::Count, ClipFieldSection::Reduction},
        {"keys_after", "Keys", "count", ClipFieldFormat::Count, ClipFieldSection::Reduction},
    };

    // the table's row of `key`; null for a key it does not have
    const ClipReportField* findClipReportField(std::string_view key);
    // for people: "31 KB", "2,340", "1.20 s", "0.81 mm". Agents read the exact number from the JSON
    std::string formatClipField(const ClipReportField& field, double value);
    // formatClipField of the row named `key`
    std::string formatClipField(std::string_view key, double value);
    // the .anim Inspector's line: "1.20 s · 30 Hz · 65 tracks · 2,340 keys · Memory 31 KB · File 112 KB · 9 constant tracks"
    std::string clipInfoLine(const ClipReport& report);
    // Error: max 0.81 mm, mean 0.21 mm (VAT's wording, InspectorWindowVat)
    std::string clipErrorLine(const ClipErrorReport& error);
    // what tools return as `stats`, with the table's keys (file_bytes null when there is no file);
    // each report the request asked for joins under its own key
    nlohmann::json clipReportJson(const ClipReport& report);
    nlohmann::json clipSkippedTracksJson(const ClipSkippedTracks& skipped);
    // the Compare fields, without `joints`: for one clip's result, a report's `compare`
    nlohmann::json clipErrorJson(const ClipErrorReport& error);
    // the worst joints: [{"bone", "max_mm"}], at most `top`, the worst first, none at 0
    nlohmann::json clipWorstJointsJson(const ClipErrorReport& error, const ClipRig& rig, size_t top);

    // ---- what `check --anims` warns of (R25): every number has one producer ----

    // Keys on reducible-looking tracks (linear, position / rotation / scale) that their neighbours
    // already give to within 1e-5 per component: removing them changes nothing that plays
    size_t countRedundantKeys(const ClipData& data);
    struct ClipRotationFlip{
        std::string path;
        float time = 0.0f;
    };
    // Pairs of keys a slerp cannot be trusted across: more than 160 degrees apart (either way round
    // reads almost the same) or, on a Hermite track, opposite signs (the curve is componentwise)
    std::vector<ClipRotationFlip> findRotationFlips(const ClipData& data);

    // The block a reduced clip records (R24): {tolerance_mm, skin_distance_mm, source_sha1,
    // max_error_mm}, plus `source` naming what the hash is of. An in-place reduction has no source
    // left to compare with and names none.
    struct ClipOptimizeRecord{
        bool present = false;
        double toleranceMm = 0.0;
        double skinDistanceMm = 0.0;
        double maxErrorMm = 0.0;
        std::string sourceSha1;
        std::string source;
    };
    ClipOptimizeRecord readClipOptimizeRecord(const ClipData& data);
    void writeClipOptimizeRecord(ClipData& data, const ClipOptimizeRecord& record);

}
