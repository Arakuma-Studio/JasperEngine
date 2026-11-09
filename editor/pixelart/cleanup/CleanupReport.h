// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/report.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// What a run says about a picture, for agents first (docs/specs/S46-pixel-art-cleanup.md R6, R29):
// is it clean pixel art, can it be fixed with confidence, or does a person need to look?
//
// The grid is judged twice, independently: by detection's own confidence and by the fit (how much
// of the middle of each cell really is one colour). A wrong grid can fool one measure; it rarely
// fools both. S46 is stricter than ritmo: on top of those two thresholds, independent guards send a
// run to needs_person whatever the confidence (lost colours, an empty frame, an unsure shared cell,
// a rule the pipeline cannot fix, no grid on a picture that is not native), each listed with its
// value and threshold.

#pragma once

#include "pixelart/PixelProblem.h"
#include "pixelart/cleanup/CleanupAnimation.h"
#include "pixelart/cleanup/CleanupPipeline.h"
#include "pixelart/cleanup/CleanupTypes.h"
#include "pixelart/lint/PixelLint.h"

#include "json.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    // Thresholds for a confident fix (report.ts CONFIDENT)
    constexpr double CONFIDENT_CONFIDENCE = 0.5;
    constexpr double CONFIDENT_FIT = 0.9;
    // A guard (R29): more than this share of the opaque cells lost
    constexpr double LOST_SHARE = 0.05;

    // How much of the middle of each cell (the pixels read) is close to the cell's own colour: 1
    // when every cell is flat
    double gridFit(const CellSamples& samples);

    // R6: a picture counts as pixel art at its own size (`outcome` is the fix with the grid found)
    bool isNative(const RgbaImage& image, const std::optional<Outcome>& outcome);

    struct Guard {
        std::string id;               // "lost_share", "empty_frame", "shared_cell", "no_grid", "low_confidence", "low_fit", or a FAIL lint's id
        double value = 0, threshold = 0;
    };

    struct ReportGrid {
        double cellWidth = 0, cellHeight = 0, offsetX = 0, offsetY = 0;
        std::optional<double> confidence;   // absent for a given grid
        double fit = 0;
        std::optional<bool> outlines;
    };

    struct ReportClip {
        std::string name;
        std::vector<int> frames;      // numbered from 1, in play order
        bool include = true;
        double fps = DEFAULT_FPS;
        Loop loop = Loop::Forward;
        int width = 0, height = 0;
    };

    struct ReportBox {
        int frame = 0;                // from 1
        FrameBox box;
        bool include = true;
        std::optional<double> cell, confidence;   // what this frame alone looks drawn at
    };

    struct ReportAnimation {
        int frames = 0, included = 0;
        Anchor anchor = Anchor::Ground;
        std::vector<ReportClip> clips;
        std::vector<ReportBox> boxes;
    };

    struct ReportLost {
        Rgba8 color, became;
        int pixels = 0;
        std::vector<std::array<int, 2>> at;   // up to 20 cells, [x, y]
        int frame = -1;               // a sheet's frame (from 1), -1 for a single picture
    };

    struct Report {
        Verdict verdict = Verdict::NeedsPerson;
        std::string summary;
        std::string kind;             // "pixel-art", "upscaled", "animation"
        double confidence = 0;        // min(detection confidence, fit)
        std::vector<Guard> guards;
        int sourceWidth = 0, sourceHeight = 0;
        std::optional<ReportGrid> grid;
        int outputWidth = 0, outputHeight = 0, colors = 0;
        std::optional<ReportAnimation> animation;
        bool hasPalette = false;
        std::string paletteName;
        std::vector<ReportLost> lost;
        lint::LintReport lints;
        int partialAlpha = 0;         // source pixels with 0 < alpha < 255 (R14)
    };

    struct ReportOptions {
        lint::LintRules rules;
        std::vector<Rgba8> backdrops;
        std::string backdropsSource = "options.backdrops";
        std::vector<std::string> only;                   // lint ids; empty: every one
        std::map<std::string, double> thresholds;
        bool failOnWarn = false;                         // fail_on: warn
    };

    struct Checked {
        Report report;
        Recipe recipe;                 // with its grid resolved: what a fix runs and saves
        std::optional<Outcome> outcome;
    };

    // Checks a picture: with grid Auto, a picture without a grid of fake pixels is taken as pixel
    // art at its own size and only linted; otherwise it is fixed with the recipe, and the fix judged
    Checked checkImage(const RgbaImage& image, const Recipe& recipe, const ReportOptions& options = {});

    // Pixel art at its own size (lints on `art`; partial alpha from `source` when given)
    Report reportOfArt(const PixelArt& art, const ReportOptions& options = {}, const RgbaImage* source = nullptr,
                       const std::optional<RecipePalette>& palette = std::nullopt);
    // A fix that has run
    Report reportOfFix(const RgbaImage& image, const Recipe& recipe, const Outcome& outcome, const ReportOptions& options = {});
    // An animation that has run
    Report reportOfSheet(const RgbaImage& image, const Recipe& recipe, const AnimationOutcome& outcome, const ReportOptions& options = {});

    struct Comparison {
        char better = '=';             // 'a', 'b' or '='
        std::string reason;
    };
    // The better verdict, then fewer FAILs, then fewer WARNs, then the better fit
    Comparison compareReports(const Report& a, const Report& b);

    // {verdict, summary, kind, confidence, guards, source, grid?, output, animation?, palette?, lints, partial_alpha}
    nlohmann::json reportToJson(const Report& report);

}
