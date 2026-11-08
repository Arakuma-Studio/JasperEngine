// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R42, §6.6)
// - Purpose: export_title's `sheet`, `atlas` and `sequence` formats of a title's animations:
//   frame i is the document posed at t = i / fps for the export's language, rendered on the
//   frames' union frame (sheet, sequence) or trimmed by alpha_threshold (atlas).
//   - sheet: a uniform grid (`columns`, default ceil(sqrt(n))) in `<stem>@<W>x<H>.png` (S45's grid
//     convention: W x H is a cell) and `<stem>.sheet.json` (the JSON-array shape);
//   - atlas: stb_rect_pack pages (height descending, then frame index; padding 2, extrude 1;
//     pages up to max_atlas 4096, at most 16) in `<stem>_page<N>.png` and `<stem>.atlas.json`
//     (the JSON-hash shape with trimmed, spriteSourceSize, sourceSize, duration, frameTags);
//   - sequence: `<stem>_<NNNN>.png` on the shared frame.
// - Poses come from S85's sampling through the frame source S85 registers
//   (registerTitleAnimFrameSource): its animations, their fps and duration, and the document
//   posed at a time for a language (S85 rebuilds its TitleAnimPlan per language). Without one, a
//   document with animations answers not_available naming S85.
// - Thread-safety: main thread.

#pragma once

#include "titleart/TitleArtDoc.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TitleAnimInfo {
        std::string name;
        double fps = 30.0;          // the flipbook's (S85: the document fps, at most 60)
        double duration = 0.0;      // seconds
        bool loop = false;          // a loop's frames cover [0, D); a once animation's include D
    };

    struct TitleAnimFrameSource {
        // the document's animations, the default first
        std::function<std::vector<TitleAnimInfo>(const titleart::TitleArtDoc& doc)> animations;
        // `doc` (resolved for the export's language) posed at `seconds` of `animation`; false with `error`
        std::function<bool(const titleart::TitleArtDoc& doc, const std::string& animation, double seconds, titleart::TitleArtDoc& posed,
                           std::string& error)> pose;
    };

    // S85's registration (its sampling); replaces an earlier one
    void registerTitleAnimFrameSource(TitleAnimFrameSource source);
    const TitleAnimFrameSource* titleAnimFrameSource();

    // the frame times of an animation: `frames` a count, [from, to] indexes, or null (every frame);
    // at most 1,024
    bool titleAnimFrameTimes(const TitleAnimInfo& info, double fps, const titleart::Json& frames, std::vector<double>& times, std::string& error);

    void registerTitleSheetExport();

}
