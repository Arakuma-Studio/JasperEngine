// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AudioAnalysis.h"
#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // A multi-take file cut into one clip per take (spec S36 M9): its onsets, gated and spaced, each
    // slice ending at its own tail or the next take. The source is never changed; a slice's file holds its samples
    // unfaded and its .meta the click-safe fades of the edges that would click.
    class SoundSlicer {
    public:
        struct Options {
            double gateDb = -40.0;
            double minGapMs = 80.0;
            double preMs = 4.0;
            double maxMs = 2000.0;
        };

        struct Slice {
            uint32_t startFrame = 0;
            uint32_t endFrame = 0;
            double peakDb = 0.0;
            // AudioAnalysis::cutLevelDb at each edge, re the file's peak
            double headCutDb = 0.0;
            double tailCutDb = 0.0;
            // 0: the edge does not click
            double fadeInMs = 0.0;
            double fadeOutMs = 0.0;
        };

        struct File {
            std::filesystem::path wav;
            std::string wavBytes;
            std::string metaText;
        };

        // The slices of a decoded file of this kind (it sets the fades). False + the reason: an option out
        // of range, more than SoundTypes::SLICE_MAX onsets, or a file with no slice.
        static bool detect(const AudioAnalysis::Signal& signal, const Options& options, const std::string& kind, std::vector<Slice>& slices,
                           std::string& error);

        // <event dir>/<event stem>_<nn>.wav and .meta for the picked slices (1-based; empty: all): PCM16
        // at the source's rate and channels; each .meta has the source's type, labels and licence,
        // and provenance.sliced_from {path, sha256, start_frame, end_frame}. False + the reason, before anything
        // is made: a pick out of range, a target that exists, more than SLICE_MAX_BYTES in all.
        static bool plan(const std::filesystem::path& source, const std::string& sourceRel, const std::string& sourceSha256,
                         const AudioAnalysis::Signal& signal, const std::vector<Slice>& slices, const std::vector<int>& pick,
                         const std::filesystem::path& eventFile, std::vector<File>& files, std::string& error);

        // A source's length a slice call takes: false + the reason past SLICE_MAX_SOURCE_SECONDS
        static bool checkSource(const AudioAnalysis::Signal& signal, std::string& error);

        // {n, start_s, end_s, peak_db, head_cut_db, tail_cut_db, fade_in_ms, fade_out_ms}
        static nlohmann::json toJson(const Slice& slice, int n, unsigned int rate);
    };

}
