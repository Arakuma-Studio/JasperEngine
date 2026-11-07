// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (S74 R18, D2, D3)
// - Purpose: the one producer of a clip's numbers in the editor. analyzeClip fills a ClipReport;
//   panels and tools print its struct and never calculate. kClipReportFields names every number
//   once (the JSON key agents read, the label people read, its unit and how it prints), so the
//   two cannot disagree: a gtest holds the table and clipReportJson equal.
// - This slice: stats (Memory, File, Keys, Tracks, Length, Constant Tracks). A new number adds a
//   member, a table row and a JSON key.
// - Thread-safety: main thread when it loads through AnimationClipPool; with `clip` set, any.
// - Editor only: nothing here compiles into a player.

#pragma once

#include "animation/AnimationClip.h"

#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace jasperengine::anim{

    struct ClipReportRequest{
        // an .anim path or "<model>#<clip>", in any form AnimationClipPool takes. Loaded through
        // the pool, so the numbers are those of what plays; a clip this call loaded leaves the
        // pool again, so asking does not change Clip memory
        std::string reference;
        // a clip already built (an unsaved result): reported as it is, `reference` only names its
        // file for file_bytes
        std::shared_ptr<const AnimationClip> clip;
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
    };

    ClipReport analyzeClip(const ClipReportRequest& request);

    enum class ClipFieldFormat{ Bytes, Count, Seconds };

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
    };

    // the table's row of `key`; null for a key it does not have
    const ClipReportField* findClipReportField(std::string_view key);
    // for people: "31 KB", "2,340", "1.20 s". Agents read the exact number from the JSON
    std::string formatClipField(const ClipReportField& field, double value);
    // what tools return as `stats`, with the table's keys (file_bytes null when there is no file)
    nlohmann::json clipReportJson(const ClipReport& report);

}
