// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AudioAnalysis.h"

#include "json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // The fades of a manifest clip's meta (meta.import.audioImporter): what an import writes and
    // the region `essentials:` and `library:` references are analysed on. Only fade_in_ms and
    // fade_out_ms, each 0..1000 ms, are looked at here.
    struct SoundManifestImport {
        std::optional<double> fadeInMs;
        std::optional<double> fadeOutMs;

        bool empty() const { return !fadeInMs && !fadeOutMs; }
        // the object as the manifest has it ({} when empty)
        nlohmann::json toJson() const;
        // The region a clip with only these fades plays, at the file's rate: frames
        // round(ms x rate / 1000), as AudioImportSettings::resolve rounds them (the clamps to the
        // file's length apply where the region is used, Region::fadeIn and fadeOut)
        AudioAnalysis::Region region(unsigned int sampleRate) const;
        // The clip's import.audioImporter; null reads as empty
        static bool parse(const nlohmann::json& value, SoundManifestImport& out, std::string& error);
    };

    // The `semantic` object of a library clip: the offline
    // model's label check and at most three heard tags, a guess below the clip's own metadata
    struct SoundManifestSemantic {
        std::string label;                // "ok", "weak" or "review"
        std::vector<std::string> heard;   // at most 3, each at most 48 characters
    };

    // A clip's `measured` block: the pipeline's independent measurement of the file (soundfile,
    // numpy, pyloudnorm). Values the manifest leaves out or sets to null stay empty.
    struct SoundManifestMeasured {
        unsigned int channels = 0;
        unsigned int sampleRate = 0;
        uint64_t frames = 0;
        double durationS = 0.0;
        std::optional<double> samplePeakDbfs;
        std::optional<double> truePeakDbtp;        // library only
        std::optional<double> momentaryMaxLufs;
        std::optional<double> integratedLufs;      // library only; null under 400 ms
        std::optional<double> headMs;
        std::optional<double> tailMs;
        std::optional<double> seamStepDb;          // library loops
        std::optional<double> seamLevelDb;         // library loops
    };

    // A library clip's `processing` block (what the pipeline did): the values the
    // editor shows; the raw object has every key
    struct SoundManifestProcessing {
        std::optional<double> gainDb;
        std::string limitedBy;                     // "true_peak", or empty for null
        std::optional<unsigned int> resampledFrom; // null: the source was 48 kHz
        std::optional<unsigned int> channelsFrom;
        std::optional<double> highpassHz;
        std::optional<double> lowpassHz;
        std::optional<double> fadeInMs;
        std::optional<double> fadeOutMs;
        std::optional<double> loopCrossfadeMs;
        std::optional<double> quality;
        std::string preview;                       // Freesound: "hq.ogg" or "hq.mp3"
    };

    // One clip of a built-in collection's manifest: the
    // essentials' (Phase 1 spec 2.12.4) and the starter library's share the schema
    struct SoundManifestClip {
        std::string id;              // "<group>/<name>"
        std::string group;           // the id's part before the slash (a library clip's type)
        std::string path;            // id + ".ogg"
        std::string sha256;          // 64 lowercase hex digits
        uint64_t bytes = 0;
        std::string type;
        std::string kind;
        std::vector<std::string> tags;
        std::string description;
        std::string licence;         // "CC0-1.0"
        std::string source;          // "kenney" or "freesound"
        std::string pack;            // Kenney clips
        std::string sourceFile;      // Kenney clips: the zip member
        int64_t sourceId = 0;        // Freesound clips
        std::string author;
        std::string url;
        std::string licenceUrl;      // Freesound clips
        std::string attribution;     // empty when the manifest has none
        SoundManifestMeasured measured;
        std::optional<SoundManifestProcessing> processing;   // library clips
        SoundManifestImport importDefaults;
        std::optional<SoundManifestSemantic> semantic;
        // the .meta sections an import writes: {import: {audioImporter}, provenance, labels}
        nlohmann::json meta;
        nlohmann::json raw;          // the manifest's clip object

        // The region a clip with only the manifest's import fades plays (SoundManifestImport::region):
        // the CLI and analyze_audio take a built-in clip's played region from it
        AudioAnalysis::Region importRegion(unsigned int sampleRate) const { return importDefaults.region(sampleRate); }
    };

    struct SoundManifestPack {
        std::string id;
        std::string title;
        std::string url;
        std::string landing;
        std::string sha256;
        uint64_t bytes = 0;
        std::string licence;
        std::string licenceFile;     // "licenses/<name>.txt", relative to the collection's root
        std::string creditsFile;     // optional: "licenses/<name>.credits.txt"
        std::string author;
        nlohmann::json raw;
    };

    // A parsed and validated manifest
    struct SoundManifest {
        static constexpr const char* ESSENTIALS_FORMAT = "jasperengine-sound-essentials";
        static constexpr const char* LIBRARY_FORMAT = "jasperengine-sound-library";
        // the first versions with a per-clip `meta`; an older manifest is refused
        static constexpr int ESSENTIALS_VERSION = 2;
        static constexpr int LIBRARY_VERSION = 4;
        // ^[a-z0-9_]+/[a-z0-9_]{1,48}$
        static constexpr size_t MAX_NAME_LENGTH = 48;
        static constexpr size_t SEMANTIC_MAX_HEARD = 3;
        static constexpr size_t SEMANTIC_MAX_TAG_LENGTH = 48;

        std::string format;
        int version = 0;
        std::string licence;
        std::string generatedBy;
        std::vector<SoundManifestPack> packs;
        std::vector<SoundManifestClip> clips;   // sorted by id
        nlohmann::json raw;

        const SoundManifestClip* find(const std::string& id) const;
        const SoundManifestPack* findPack(const std::string& id) const;

        // The schema of spec 2.2.2 with the rules of spec 2.5.1, every one fatal for the whole
        // manifest: format equals expectedFormat; version a positive integer; ids match the pattern
        // and are unique; path is id + ".ogg"; type one of the 31 (SoundTypes.h) and, for the
        // library, the id's group; kind one of the 7; sha256 64 lowercase hex digits; bytes > 0;
        // licence CC0-1.0; source kenney (a pack of the manifest and its member) or freesound
        // (source_id, licence_url; no pack); a meta of only import, provenance and labels; semantic
        // only label and heard; a library clip has processing. Every pack has an id, its licence file
        // under licenses/. The error names the clip or pack and the key.
        static bool parse(const std::string& text, const std::string& expectedFormat, SoundManifest& out, std::string& error);

        // id is "<group>/<name>" with the pattern of spec 2.2.1
        static bool isValidId(const std::string& id);
        // one of the 7 kinds (SoundTypes.h)
        static bool isKind(const std::string& kind);
    };

}
