// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S48 R28 / D13: the effect lints on synthetic captures, each with a red fixture and its fixed twin

#include "pixelart/lint/EffectLints.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::lint;
using jasperengine::pixelart::Rgba8;

namespace {

    const PixelRgba kInk{11, 10, 16, 255};
    const PixelRgba kRed{200, 40, 40, 255};
    const PixelRgba kRim{255, 154, 46, 255};

    PixelImage box(int w, int h, int x, int y, int bw, int bh, PixelRgba c) {
        PixelImage image(w, h);
        for (int yy = y; yy < y + bh; yy++)
            for (int xx = x; xx < x + bw; xx++) image.set(xx, yy, c);
        return image;
    }

    const LintResult* find(const LintReport& report, const std::string& id) {
        for (const LintResult& r : report.results)
            if (r.id == id) return &r;
        return nullptr;
    }

    // A transition frame drawn by the field rule (pixelfx _lib.js field): ink where f < k - rim,
    // the rim where the distance to the level set is below `rim`, the screen beyond
    template <typename F>
    PixelImage fieldFrame(int w, int h, int rim, F distance) {
        PixelImage image(w, h);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const double d = distance(x + 0.5, y + 0.5);   // > 0 inside the covered part, in pixels
                if (d >= rim) image.set(x, y, kInk);
                else if (d >= 0) image.set(x, y, kRim);
            }
        }
        return image;
    }

}

TEST(EffectLints, IdsAreTheSpecsTen) {
    EXPECT_EQ(effectLintIds().size(), 10u);
    EXPECT_TRUE(isEffectLintId("transition_edges"));
    EXPECT_FALSE(isEffectLintId("jaggies"));
}

TEST(EffectLints, SpillWarnsPastTheMarginWithItsSpans) {
    // bounds 8x8 at (40, 40) in a 120x120 frame, margin 24: a pixel 30 px right of the bounds spills
    const IntRect bounds{40, 40, 8, 8};
    PixelImage clean = box(120, 120, 40, 40, 8, 8, kRed);
    clean.set(47 + 20, 44, kRed);                   // 20 px out: inside the margin
    PixelImage spilled = clean;
    spilled.set(47 + 30, 44, kRed);                 // 30 px out: 6 past the margin
    const LintResult fixed = lintSpill({{&clean}}, {1}, bounds, 24);
    EXPECT_EQ(fixed.status, LintStatus::Pass);
    EXPECT_EQ(fixed.measured, 0);
    // the spill shows only with another seed: every run counts
    const LintResult red = lintSpill({{&clean}, {&spilled}}, {1, 2}, bounds, 24);
    EXPECT_EQ(red.status, LintStatus::Warn);
    EXPECT_EQ(red.measured, 1);
    ASSERT_EQ(red.at.size(), 1u);
    EXPECT_EQ(red.at[0].x, 77);
    EXPECT_NE(red.message.find("seed 2"), std::string::npos) << red.message;
    EXPECT_NE(red.message.find("right 6"), std::string::npos) << red.message;
    EXPECT_FALSE(red.fix.empty());
}

TEST(EffectLints, OneShotEndWarnsOnALeftoverPixel) {
    PixelImage empty(16, 16);
    PixelImage leftover(16, 16);
    leftover.set(3, 12, kRed);
    EXPECT_EQ(lintOneShotEnd(&empty, false).status, LintStatus::Pass);
    const LintResult red = lintOneShotEnd(&leftover, false);
    EXPECT_EQ(red.status, LintStatus::Warn);
    EXPECT_EQ(red.count, 1);
    EXPECT_EQ(red.at[0].x, 3);
    EXPECT_EQ(red.at[0].y, 12);
    EXPECT_EQ(lintOneShotEnd(&leftover, true).status, LintStatus::Skip);
}

TEST(EffectLints, LoopSeamComparesTheWrapWithTheTypicalStep) {
    // a 4x4 dot moving one pixel a frame: each step changes 8 px, so the seam threshold is
    // max(2 x 8, 12) = 16
    std::vector<PixelImage> pingPong, walk;
    for (int i = 0; i < 8; i++) {
        pingPong.push_back(box(40, 8, i < 4 ? i : 8 - i, 2, 4, 4, kRed));   // ends one step from the start
        walk.push_back(box(40, 8, i, 2, 4, 4, kRed));                       // ends 7 px from where it starts
    }
    const LintResult fixed = lintLoopSeam(frameView(pingPong), true);
    EXPECT_EQ(fixed.status, LintStatus::Pass) << fixed.measured << " vs " << fixed.threshold;
    EXPECT_EQ(fixed.threshold, 16);
    EXPECT_EQ(fixed.measured, 8);
    const LintResult red = lintLoopSeam(frameView(walk), true);
    EXPECT_EQ(red.status, LintStatus::Warn);
    EXPECT_EQ(red.measured, 32);
    EXPECT_FALSE(red.message.empty());
    EXPECT_EQ(lintLoopSeam(frameView(walk), false).status, LintStatus::Skip);
}

TEST(EffectLints, LoopSeamFailsOverTwiceTheThreshold) {
    // a bar growing one 30 px row a frame: each step changes 30 px (threshold 60), the wrap 270
    std::vector<PixelImage> frames;
    for (int i = 0; i < 10; i++) frames.push_back(box(64, 64, 10, 10, 30, i + 1, kRed));
    const LintResult r = lintLoopSeam(frameView(frames), true);
    EXPECT_EQ(r.threshold, 60);
    EXPECT_EQ(r.measured, 270);
    EXPECT_EQ(r.status, LintStatus::Fail);
}

TEST(EffectLints, DeterminismFailsOnAnyDifference) {
    PixelImage a = box(16, 16, 2, 2, 4, 4, kRed);
    PixelImage b = a;
    EXPECT_EQ(lintDeterminism(&a, &b, 0.37).status, LintStatus::Pass);
    b.set(9, 9, kRed);
    const LintResult red = lintDeterminism(&a, &b, 0.37);
    EXPECT_EQ(red.status, LintStatus::Fail);
    EXPECT_EQ(red.measured, 1);
    EXPECT_EQ(lintDeterminism(&a, nullptr, 0.37).status, LintStatus::Skip);
}

TEST(EffectLints, FlickerFindsOneFrameBlinks) {
    // a 10x10 sprite over 6 frames; in frame 3 a third of it blinks off and back
    std::vector<PixelImage> frames(6, box(10, 10, 0, 0, 10, 10, kRed));
    const LintResult fixed = lintFlicker(frameView(frames), true);
    EXPECT_EQ(fixed.status, LintStatus::Pass);
    // a two-frame change is not a blink
    std::vector<PixelImage> held = frames;
    held[2] = box(10, 10, 0, 0, 10, 7, kRed);
    held[3] = held[2];
    EXPECT_EQ(lintFlicker(frameView(held), true).status, LintStatus::Pass);
    std::vector<PixelImage> blink = frames;
    blink[2] = box(10, 10, 0, 0, 10, 7, kRed);
    const LintResult red = lintFlicker(frameView(blink), true);
    EXPECT_EQ(red.status, LintStatus::Warn);
    EXPECT_NEAR(red.measured, 0.3, 1e-9);
    EXPECT_EQ(red.at[0].frame, 3);
    // 6 % or less passes: 6 of 100 pixels
    std::vector<PixelImage> few = frames;
    few[2] = frames[2];
    for (int x = 0; x < 6; x++) few[2].set(x, 9, PixelRgba{0, 0, 0, 0});
    EXPECT_EQ(lintFlicker(frameView(few), true).status, LintStatus::Pass);
    // the wrap is a neighbour only for a loop
    std::vector<PixelImage> wrap = frames;
    wrap[0] = box(10, 10, 0, 0, 10, 5, kRed);
    EXPECT_EQ(lintFlicker(frameView(wrap), true).status, LintStatus::Warn);
    EXPECT_EQ(lintFlicker(frameView(wrap), false).status, LintStatus::Pass);
    EXPECT_EQ(lintFlicker(FrameView{frameView(frames)[0], frameView(frames)[1]}, true).status, LintStatus::Skip);
}

TEST(EffectLints, OffPaletteWarnsThenFailsAboveOnePercent) {
    const std::vector<Rgba8> palette = {{200, 40, 40, 255}, {11, 10, 16, 255}};
    PixelImage art = box(20, 20, 0, 0, 20, 20, kRed);   // 400 opaque px
    EXPECT_EQ(lintOffPalette({&art}, palette).status, LintStatus::Pass);
    PixelImage one = art;
    one.set(5, 5, PixelRgba{201, 40, 40, 255});
    const LintResult warn = lintOffPalette({&one}, palette);
    EXPECT_EQ(warn.status, LintStatus::Warn);
    EXPECT_NE(warn.message.find("#c92828"), std::string::npos) << warn.message;
    PixelImage many = art;
    for (int x = 0; x < 5; x++) many.set(x, 0, PixelRgba{0, 255, 0, 255});   // 1.25 %
    EXPECT_EQ(lintOffPalette({&many}, palette).status, LintStatus::Fail);
    // S46's catalogue: FAIL when any pixel is off
    OffPaletteOptions strict;
    strict.failShare = 0.0;
    strict.paletteName = "ui/game.palette";
    const LintResult s46 = lintOffPalette({&one}, palette, strict);
    EXPECT_EQ(s46.status, LintStatus::Fail);
    EXPECT_NE(s46.fix.find("ui/game.palette"), std::string::npos);
    // partly transparent pixels are partial_alpha's
    PixelImage soft = art;
    soft.set(1, 1, PixelRgba{1, 2, 3, 128});
    EXPECT_EQ(lintOffPalette({&soft}, palette).status, LintStatus::Pass);
    EXPECT_EQ(lintOffPalette({&art}, {}).status, LintStatus::Skip);
}

TEST(EffectLints, PartialAlphaFailsOnAnySoftPixel) {
    PixelImage art = box(8, 8, 0, 0, 8, 8, kRed);
    EXPECT_EQ(lintPartialAlpha({&art}).status, LintStatus::Pass);
    art.set(2, 3, PixelRgba{200, 40, 40, 200});
    const LintResult red = lintPartialAlpha({&art});
    EXPECT_EQ(red.status, LintStatus::Fail);
    EXPECT_EQ(red.count, 1);
}

TEST(EffectLints, FrameCostTiers) {
    FrameCost none;
    none.skipReason = "release builds only";
    EXPECT_EQ(lintFrameCost(none).status, LintStatus::Skip);
    EXPECT_EQ(lintFrameCost(none).message, "release builds only");
    FrameCost cheap{2.1, 2.0};
    EXPECT_EQ(lintFrameCost(cheap).status, LintStatus::Pass);
    FrameCost slow{4.0, 2.0};
    EXPECT_EQ(lintFrameCost(slow).status, LintStatus::Warn);
    FrameCost awful{8.0, 2.0};
    EXPECT_EQ(lintFrameCost(awful).status, LintStatus::Fail);
    FrameCost taps{2.0, 2.0};
    taps.taps = 30;
    EXPECT_EQ(lintFrameCost(taps).status, LintStatus::Warn);
    taps.taps = 24;
    EXPECT_EQ(lintFrameCost(taps).status, LintStatus::Pass);
}

TEST(EffectLints, HitStopPastAThirdOfASecondReadsAsAStall) {
    EXPECT_EQ(lintHitStopLength(-1.0).status, LintStatus::Skip);
    EXPECT_EQ(lintHitStopLength(0.08).status, LintStatus::Pass);
    EXPECT_EQ(lintHitStopLength(0.3).status, LintStatus::Pass);
    EXPECT_EQ(lintHitStopLength(0.45).status, LintStatus::Warn);
}

TEST(EffectLints, TransitionRimsOfConstantWidthPass) {
    const std::vector<Rgba8> rim = {{kRim[0], kRim[1], kRim[2], 255}};
    // a vertical wipe, a 30 degree slash and an iris circle, all with a 2 px rim by the field rule
    const PixelImage wipe = fieldFrame(64, 48, 2, [](double x, double) { return 30.0 - x; });
    const double a = 30.0 * 3.14159265358979 / 180.0;
    const PixelImage slash = fieldFrame(96, 64, 2, [a](double x, double y) { return 50.0 - (x * std::cos(a) + y * std::sin(a)); });
    const PixelImage iris = fieldFrame(96, 96, 2, [](double x, double y) { return std::hypot(x - 48.0, y - 48.0) - 24.0; });
    for (const PixelImage* frame : {&wipe, &slash, &iris}) {
        const RimMeasure m = measureRim(*frame, rim);
        EXPECT_GT(m.samples, 10);
        EXPECT_EQ(m.narrowest, 2);
        EXPECT_EQ(m.widest, 2);
    }
    const LintResult pass = lintTransitionEdges({&wipe, &slash, &iris}, rim, true);
    EXPECT_EQ(pass.status, LintStatus::Pass);
    EXPECT_EQ(lintTransitionEdges({&wipe}, rim, false).status, LintStatus::Skip);
}

TEST(EffectLints, TransitionRimIsMeasuredInArtPixels) {
    const std::vector<Rgba8> rim = {{kRim[0], kRim[1], kRim[2], 255}};
    const PixelImage art = fieldFrame(96, 96, 2, [](double x, double y) { return std::hypot(x - 48.0, y - 48.0) - 24.0; });
    PixelImage scaled(96 * 3, 96 * 3);
    for (int y = 0; y < scaled.height(); y++) {
        for (int x = 0; x < scaled.width(); x++) scaled.set(x, y, art.at(x / 3, y / 3));
    }
    const LintResult pass = lintTransitionEdges({&scaled}, rim, true, 3);
    EXPECT_EQ(pass.status, LintStatus::Pass) << pass.message;
    EXPECT_EQ(pass.measured, 0);
}

TEST(EffectLints, TransitionRimThatChangesWidthWarns) {
    const std::vector<Rgba8> rim = {{kRim[0], kRim[1], kRim[2], 255}};
    // a wipe whose rim is 3 px (x 27..29); in the top half its inner column is ink: 2 px there
    PixelImage frame = fieldFrame(64, 48, 3, [](double x, double) { return 30.0 - x; });
    const RimMeasure even = measureRim(frame, rim);
    EXPECT_EQ(even.narrowest, 3);
    EXPECT_EQ(even.widest, 3);
    for (int y = 0; y < 24; y++) frame.set(27, y, kInk);
    const LintResult red = lintTransitionEdges({&frame}, rim, true);
    EXPECT_EQ(red.status, LintStatus::Warn) << red.measured;
    EXPECT_EQ(red.measured, 1);
    EXPECT_EQ(red.count, 1);
}

TEST(EffectLints, ReportAndVerdict) {
    PixelImage clean = box(80, 80, 30, 30, 8, 8, kRed);
    PixelImage empty(80, 80);
    EffectLintInput input;
    input.frames = {&clean, &clean, &clean, &empty};
    input.bounds = {30, 30, 8, 8};
    input.duration = 1.0;
    input.determinismFirst = &clean;
    input.determinismSecond = &clean;
    const LintReport ok = runEffectLints(input);
    EXPECT_EQ(ok.results.size(), 10u);
    EXPECT_EQ(ok.fail + ok.warn, 0) << toJson(ok).dump();
    EXPECT_EQ(effectVerdict(ok), Verdict::Clean);
    EXPECT_EQ(find(ok, "transition_edges")->status, LintStatus::Skip);

    // a leftover pixel: fixable
    PixelImage leftover(80, 80);
    leftover.set(33, 33, kRed);
    input.frames.back() = &leftover;
    const LintReport warn = runEffectLints(input);
    EXPECT_EQ(find(warn, "one_shot_end")->status, LintStatus::Warn);
    EXPECT_EQ(effectVerdict(warn), Verdict::Fixable);

    // not deterministic: a person decides (code)
    PixelImage other = clean;
    other.set(0, 0, kRed);
    input.determinismSecond = &other;
    EXPECT_EQ(effectVerdict(runEffectLints(input)), Verdict::NeedsPerson);

    // only the named lints
    const LintReport some = runEffectLints(input, {"partial_alpha", "spill"});
    ASSERT_EQ(some.results.size(), 2u);
    EXPECT_EQ(some.results[0].id, "spill");
}

// capture_contact_sheet's checks read like preview_particle_effect's (S35 / S91 rows)
TEST(EffectLints, CheckRowsHaveTheVerdictToolsShape) {
    PixelImage leftover(16, 16);
    leftover.set(3, 12, kRed);
    const LintResult warn = lintOneShotEnd(&leftover, false);
    const qa::Json row = toCheckRow(warn, {0.0, 0.5});
    EXPECT_EQ(row["id"], "one_shot_end");
    EXPECT_EQ(row["kind"], "effect");
    EXPECT_EQ(row["status"], "WARN");
    EXPECT_EQ(row["round"], 0);
    EXPECT_EQ(row["measured"], 1.0);
    EXPECT_EQ(row["threshold_source"], "default");
    EXPECT_EQ(row["at"], qa::Json::array({0.0}));
    EXPECT_EQ(row["spots"], qa::Json::array({qa::Json::array({1, 3, 12})}));
    EXPECT_EQ(row["count"], 1);
    EXPECT_EQ(row["method"], "capture");
    EXPECT_TRUE(row["limited_by"].is_string());
    EXPECT_TRUE(row["fix_hint"].is_string());
    EXPECT_EQ(qa::rowLine(row).rfind("WARN one_shot_end: 1", 0), 0u) << qa::rowLine(row);
    const qa::Json skip = toCheckRow(lintOneShotEnd(&leftover, true), {});
    EXPECT_EQ(skip["status"], "SKIP");
    EXPECT_TRUE(skip["measured"].is_null());
    EXPECT_EQ(skip["limited_by"], "a loop has no end");
}
