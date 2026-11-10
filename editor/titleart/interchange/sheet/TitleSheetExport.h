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
// - Frames come from S85's renderer through the frame source it registers
//   (registerTitleAnimFrameSource): its animations, their fps and duration, and each frame at a
//   time for a language (rendered Exact, every track at its own time, canvas-sized as a
//   flipbook bake is). Without one, a document with animations answers not_available naming S85.
// - Thread-safety: main thread.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/interchange/TitleExport.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TitleAnimInfo {
        std::string name;
        double fps = 30.0;          // the flipbook's (S85: the document fps, at most 60)
        double duration = 0.0;      // seconds
        bool loop = false;          // a loop's frames cover [0, D); a once animation's include D
    };

    // the frames of one document's animations (S85's renderer, one per language of an export)
    struct TitleAnimFrameRenderer {
        std::vector<TitleAnimInfo> animations;      // the default first
        // `animation` at `seconds` over `region` (canvas px, image space; empty: the canvas) at
        // `scale`: straight sRGB RGBA8 pixels and the canvas pixels they cover; false with `error`
        std::function<bool(const std::string& animation, double seconds, double scale, const titleart::TitleRectI& region,
                           texsynth::Image& pixels, titleart::TitleRectI& covers, std::string& error)> render;
    };

    struct TitleAnimFrameSource {
        // a renderer of `doc` (resolved for the export's language); null with `error`
        std::function<std::shared_ptr<TitleAnimFrameRenderer>(Project* project, std::shared_ptr<const titleart::TitleArtDoc> doc, std::string& error)> open;
    };

    // S85's registration (TitleAnimRender.cpp); replaces an earlier one
    void registerTitleAnimFrameSource(TitleAnimFrameSource source);
    const TitleAnimFrameSource* titleAnimFrameSource();

    // the frame times of an animation: `frames` a count, [from, to] indexes, or null (every frame);
    // at most 1,024
    bool titleAnimFrameTimes(const TitleAnimInfo& info, double fps, const titleart::Json& frames, std::vector<double>& times, std::string& error);

    // the rendered frames of one language (one size, the shared frame), as the writers take them
    struct TitleSheetFrames {
        std::string language;
        TitleAnimInfo info;
        double fps = 30.0;
        std::vector<double> times;
        std::vector<texsynth::Image> pixels;
    };

    // writes `format` (sheet, atlas or sequence) of the frames; false after ctx.fail()
    bool writeTitleSheetFrames(TitleExportContext& ctx, const std::string& format, const TitleSheetFrames& frames);

    void registerTitleSheetExport();

}
