// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (S74 R18, D2, D3)
// - Purpose: the one producer of a clip's numbers in the editor. analyzeClip fills a ClipReport;
//   panels and tools print its struct and never calculate. kClipReportFields names every number
//   once (the JSON key agents read, the label people read, its unit and how it prints), so the
//   two cannot disagree: a gtest holds the table and clipReportJson equal.
// - The stats (Memory, File, Keys, Tracks, Length, Constant Tracks) need no rig. Loop Match
//   measures on a ClipRig (D5, R19). A new number adds a member, a table row and a JSON key.
// - Thread-safety: main thread when it loads through AnimationClipPool; with `clip` set, any.
//   `rig` stays valid for the call.
// - Editor only: nothing here compiles into a player.

#pragma once

#include "animation/AnimationClip.h"
#include "animation/ModelSubClips.h"

#include "json.hpp"

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

    // Loop Match: the pose at the end against the pose at the start, the root's travel (horizontal
    // position and heading) taken out, so a clip that walks has a seam only where its body jumps
    struct ClipLoopSeam{
        double maxErrorMm = 0.0;
        std::string worstJoint;
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
        double skinDistanceMm = 100.0;
    };

    struct ClipReport{
        bool ok = false;
        std::string error;                      // !ok: why the clip could not be read
        uint64_t runtimeBytes = 0;              // AnimationClip::memoryBytes()
        std::optional<uint64_t> fileBytes;      // the .anim on disk; none for a model clip
        size_t keys = 0;                        // over all tracks
        size_t tracks = 0;
        float lengthSeconds = 0.0f;
        // tracks of more than one key that all hold exactly the same value (and no tangents that
        // leave it): they cost keys and never move. A single-key track is already minimal
        size_t constantTracks = 0;
        // set when the request has a rig
        std::optional<ClipLoopSeam> loopSeam;
    };

    ClipReport analyzeClip(const ClipReportRequest& request);

    enum class ClipFieldFormat{ Bytes, Count, Seconds, Millimetres, Text };

    struct ClipReportField{
        const char* key;        // the JSON key
        const char* label;      // the Inspector's label
        const char* unit;
        ClipFieldFormat format;
    };

    inline constexpr ClipReportField kClipReportFields[] = {
        {"runtime_bytes", "Memory", "bytes", ClipFieldFormat::Bytes},
        {"file_bytes", "File", "bytes", ClipFieldFormat::Bytes},
        {"keys", "Keys", "count", ClipFieldFormat::Count},
        {"tracks", "Tracks", "count", ClipFieldFormat::Count},
        {"length_s", "Length", "s", ClipFieldFormat::Seconds},
        {"constant_tracks", "Constant Tracks", "count", ClipFieldFormat::Count},

        {"loop_seam.max_error_mm", "Loop Match", "mm", ClipFieldFormat::Millimetres},
        {"loop_seam.worst_joint", "Loop Match Joint", "name", ClipFieldFormat::Text},
    };

    // the table's row of `key`; null for a key it does not have
    const ClipReportField* findClipReportField(std::string_view key);
    // for people: "31 KB", "2,340", "1.20 s", "0.81 mm". Agents read the exact number from the JSON
    std::string formatClipField(const ClipReportField& field, double value);
    // formatClipField of the row named `key`
    std::string formatClipField(std::string_view key, double value);
    // what tools return as `stats`, with the table's keys (file_bytes null when there is no file);
    // each report the request asked for joins under its own key
    nlohmann::json clipReportJson(const ClipReport& report);

}
