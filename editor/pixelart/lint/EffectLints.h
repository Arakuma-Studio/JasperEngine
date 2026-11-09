// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The pixel-art family's effect lints and three rule lints (docs/specs/S48-pixel-fx.md R28, D13):
// what eyes miss in a captured effect (pixelfx tools/check.mjs: spill past the margin, one-shots
// that do not end empty, loop seams, randomness that is not seeded, slow frames, long freezes)
// plus off_palette, partial_alpha and flicker with S46 R26's definitions. One implementation:
// capture_contact_sheet runs them all, S49's capture_particles calls spill, one_shot_end,
// loop_seam and determinism, and S46's lint() calls the three rule lints. Every function is pure
// (frames in, LintResult out) and runs on any thread.
//
// Frames are PixelImage (S44): rows top first, 1 pixel per art pixel. "Drawn" is alpha > 0, so
// premultiplied frames on transparent black (the captures') and straight-alpha art read alike.

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/PixelProblem.h"
#include "pixelart/Rgba8.h"
#include "pixelart/lint/LintTypes.h"
#include "util/QaChecks.h"

#include <span>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::lint {

    // A capture's frames in time order; the images stay the caller's
    using FrameView = std::vector<const PixelImage*>;
    FrameView frameView(std::span<const PixelImage> frames);

    // capture_contact_sheet's ids, in report order
    const std::vector<std::string>& effectLintIds();
    bool isEffectLintId(const std::string& id);

    // ---- spill: pixels drawn outside the target's bounds grown by the margin, in any frame of any
    // run (pixelfx draws seeds 1, 2, 3 and 7). `runs[i]` are the frames of seed `seeds[i]` (seeds may
    // be empty: the runs are numbered). measured: the most pixels outside in one frame; WARN > 0.
    LintResult lintSpill(const std::vector<FrameView>& runs, const std::vector<uint32_t>& seeds, const IntRect& bounds, int margin);

    // ---- one_shot_end: pixels still drawn at t = duration (the last frame) of a non-looping target;
    // SKIP for a loop. WARN > 0.
    LintResult lintOneShotEnd(const PixelImage* last, bool loop);

    // ---- loop_seam: the pixels changed from the last frame to the first, against
    // max(2 x p90 of the frame-to-frame changes, 12). The frames are one period, evenly spaced, the
    // last one before the period ends. WARN over the threshold, FAIL over twice it; SKIP for a
    // one-shot or under 3 frames.
    LintResult lintLoopSeam(const FrameView& frames, bool loop);

    // ---- determinism: pixels that differ between two fresh captures of the same time (pixelfx:
    // t = 0.37 x duration). FAIL > 0; SKIP without both.
    LintResult lintDeterminism(const PixelImage* first, const PixelImage* second, double time);

    // ---- flicker (S46 R26): a pixel that changes for exactly one frame and changes back (a loop
    // wraps). measured: the share of the pixels drawn in any frame that blink; WARN above
    // `warnShare` (0.06); SKIP under 3 frames.
    LintResult lintFlicker(const FrameView& frames, bool loop, double warnShare = 0.06);

    // ---- off_palette (S46 R26): opaque pixels (alpha 255; partial ones are partial_alpha's) whose RGB
    // is no palette colour exactly. measured: the off pixels; the message lists up to 8 colours.
    // WARN when more than `warnCount`, FAIL when their share of the opaque pixels is above `failShare`
    // (S48's table: 0 and 1 %; S46's catalogue passes failShare 0: FAIL when any is off). An empty
    // palette is SKIP ("no palette given").
    struct OffPaletteOptions {
        double warnCount = 0.0;
        double failShare = 0.01;
        std::string thresholdSource = "default";
        std::string paletteName;          // named in the fix ("fix_pixel_art with palette: <name>")
    };
    LintResult lintOffPalette(const FrameView& frames, std::span<const jasperengine::pixelart::Rgba8> palette, const OffPaletteOptions& options = {});

    // ---- partial_alpha (S46 R26): pixels with 0 < alpha < 255. FAIL > 0.
    LintResult lintPartialAlpha(const FrameView& frames);

    // ---- frame_cost: the median CPU time a frame takes with the target shown minus hidden (and the GPU
    // time when measured), plus the variant's static texture taps against R33's budget. WARN above
    // 1 ms or taps over the budget, FAIL above 4 ms; SKIP when nothing was measured.
    struct FrameCost {
        double shownMs = -1.0, hiddenMs = -1.0;   // medians; < 0: not measured
        double gpuMs = -1.0;                      // added GPU time (S15 timers); < 0: not measured
        int taps = -1;                            // < 0: not counted
        int tapBudget = 24;                       // R33: every S48 feature on, pixel width 1
        std::string skipReason;                   // why nothing was measured
    };
    LintResult lintFrameCost(const FrameCost& cost);

    // ---- hit_stop_length: the longest hit-stop requested during a Play capture; WARN above 0.3 s
    // (pixelfx: "past 0.3 s it reads as a stall"). seconds < 0: not measured (SKIP with `skipReason`).
    LintResult lintHitStopLength(double seconds, const std::string& skipReason = std::string());

    // ---- transition_edges: the rim's width in art pixels along the edge, per frame (R13: the same
    // width along its whole length). Rim pixels are the opaque ones whose colour is one of
    // `rimColors` (the transition's ramp); the other opaque pixels are the ink, alpha 0 is the
    // uncovered screen. measured: the largest spread (widest minus narrowest) in a frame; WARN > 0.
    struct RimMeasure {
        int samples = 0;                  // edge positions measured
        int narrowest = 0, widest = 0;    // art pixels, rounded
        IntPoint at;                      // where the narrowest or widest differs from the most common width
    };
    RimMeasure measureRim(const PixelImage& frame, std::span<const jasperengine::pixelart::Rgba8> rimColors);
    LintResult lintTransitionEdges(const FrameView& frames, std::span<const jasperengine::pixelart::Rgba8> rimColors, bool transition, int artPixel = 1);

    // ---- all of them over one capture
    struct EffectLintInput {
        FrameView frames;                  // the sheet's frames, in time order
        std::vector<double> times;
        double duration = 0.0;
        bool loop = false;
        const PixelImage* last = nullptr;  // the frame at t = duration (one-shots); null: the last of `frames`
        IntRect bounds;                    // the target's bounds in frame pixels (rows top first)
        int margin = 24;
        bool spillApplies = true;          // false in scene mode: the whole view is drawn
        std::string spillSkipReason;
        std::vector<FrameView> spillRuns;  // extra runs (seeds 2, 3, 7); the frames above are seed 1
        std::vector<uint32_t> spillSeeds;  // the seeds of `frames` then `spillRuns`
        const PixelImage* determinismFirst = nullptr;
        const PixelImage* determinismSecond = nullptr;
        double determinismTime = 0.0;
        std::string determinismSkipReason;
        std::vector<jasperengine::pixelart::Rgba8> palette;   // empty: off_palette SKIP
        std::string paletteName;
        FrameCost cost;
        double hitStopSeconds = -1.0;
        std::string hitStopSkipReason;
        bool transition = false;
        int artPixel = 1;                  // frame pixels per art pixel of a transition's capture
        std::string rimSkipReason;         // set when the kind draws no continuous edge to measure
        std::vector<jasperengine::pixelart::Rgba8> rimColors;
    };
    // `only`: ids to run (empty: every one); unknown ids are the caller's to refuse (isEffectLintId)
    LintReport runEffectLints(const EffectLintInput& input, const std::vector<std::string>& only = {});

    // The check row of the verdict tools (util/QaChecks.h, S35 R21 / S91 D5: what
    // preview_particle_effect returns) for a lint result, so capture_contact_sheet's checks read like
    // S91's: id, kind "effect", status, round 0 (qa::countRounds sets it), measured, threshold,
    // threshold_source, limited_by (the message), at (the spotted frames' times, seconds), fix_hint
    // (the fix), method "capture"; plus S44's count and its pixel spots as spots [[frame, x, y]].
    // `times`: frame i's time (frames count from 1 in the spots).
    qa::Json toCheckRow(const LintResult& result, const std::vector<double>& times);
    qa::Json toCheckRows(const LintReport& report, const std::vector<double>& times);

    // The family's verdict (PIXEL-ART-LEDGER.md "Agent behaviour", ritmo's order): clean without a WARN
    // or FAIL; needs_person when determinism or frame_cost FAILs (code or budget decisions a setting
    // does not fix); else fixable (each WARN and FAIL names its fix)
    Verdict effectVerdict(const LintReport& report);

}
