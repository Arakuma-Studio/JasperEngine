// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R38, R39, R49)
// - Purpose: the layer plan of a layered PSD export, ported from Pachislop src/io/psd.ts
//   (titleParts, siblingsPlan, nodePlan, stackPlan) by SafEight, used with the author's
//   permission: every layer rendered alone on the export's shared frame with its blend, opacity,
//   fill opacity and visibility on the record; groups as PSD groups (a group with enabled effects
//   holds its members hidden and its baked render "fx" on top); clipped layers flagged and
//   masked by their base's alpha before the base's effects (a layer clipped to a group is written
//   masked but unclipped, with a warning); layer masks as raster masks, the pixels drawn whole;
//   adjustment layers native for levels, curves, hue/saturation, brightness/contrast, invert,
//   posterize and threshold, any other baked into "<name> (baked)" holding everything below it in
//   its parent, adjusted, with what it covers hidden; `split_titles` turning each 2D title into a
//   group of its rims alone, its face alone (hidden, their own part effects only) and "fx".
// - planPsdExport() makes the plan and its render jobs (a document variant each, rendered by S80's
//   renderer on the shared frame); assemblePsd() turns the rendered jobs into PsdWriter's input;
//   the `psd` format of export_title (TitleExport.h) steps the jobs and writes the file.
// - Thread-safety: plan and assembly are pure; the format's plan runs on the main thread.

#pragma once

#include "PsdWriter.h"
#include "texsynth/SynthImage.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::psd {

    // one render on the shared frame: `doc` drawn whole, or `isolateLayer` alone
    struct PsdRenderJob {
        std::shared_ptr<const titleart::TitleArtDoc> doc;
        std::string isolateLayer;
        std::string purpose;            // "merged", "layer", "clip_base", "mask"
    };

    struct PsdPlanNode {
        enum class Kind : uint8_t { Pixels, Adjust, Group };
        Kind kind = Kind::Pixels;
        std::string name;
        bool hidden = false;
        std::string blendKey = "norm";  // groups: "pass" for pass-through (split titles)
        uint8_t opacity = 255;
        uint8_t fillOpacity = 255;
        bool clipping = false;
        int job = -1;                   // Pixels: its render
        std::vector<int> clipBases;     // renders whose alpha multiplies this one (the clip bases before their effects)
        bool opaque = false;            // alpha made opaque before the clip bases (a baked clipped adjustment)
        int maskJob = -1;               // the render of its layer mask (alpha), -1 none
        std::string adjustmentKey;      // Adjust
        std::vector<uint8_t> adjustmentData;
        std::vector<PsdPlanNode> children;  // Group: bottom first
    };

    struct PsdExportPlan {
        std::vector<PsdRenderJob> jobs;
        std::vector<PsdPlanNode> nodes;     // bottom first
        int mergedJob = 0;
        std::vector<std::string> warnings;
    };

    struct PsdPlanOptions {
        bool splitTitles = false;
        bool includeGuides = false;
    };

    // `doc`: the document as it renders (resolved for the export's language)
    PsdExportPlan planPsdExport(const titleart::TitleArtDoc& doc, const PsdPlanOptions& options);

    // `renders[i]` the straight RGBA (8- or 16-bit) of plan.jobs[i], every one frame-sized
    bool assemblePsd(const PsdExportPlan& plan, const std::vector<texsynth::Image>& renders, int bitDepth, double dpi,
                     PsdWriteDocument& out, std::string& error);

    // export_title's `psd` format (R39, R40): one PSD per language on its trimmed frame (or the
    // shared one with `registered`), named `<stem>[.<code>].psd`; options split_titles and the
    // image options (bit_depth 8 or 16; PSD stores straight alpha, so `alpha` must stay straight)
    void registerTitlePsdExport();

}
