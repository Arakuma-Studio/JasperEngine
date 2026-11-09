// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S46 AC10 (R24-R27): every lint of the catalogue on one hand-made positive and one negative
// fixture (editor/tests/fixtures/pixelart/lint/, written by make_lint_fixtures.py there), the R25
// shape of every result, determinism over runs and threads, and the R38 budgets (PixelLintBench,
// printed and enforced with perfSlack()).

#include "pixelart/Palette.h"
#include "pixelart/PixelImage.h"
#include "pixelart/lint/CraftLints.h"
#include "pixelart/lint/PixelLint.h"
#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::lint;
namespace pl = jasperengine::editor::pixelart::lint;

namespace {

    PixelImage lintFixture(const std::string& name) {
        std::string error;
        auto image = PixelImage::loadPng(editor::test::fixture("pixelart/lint/" + name + ".png"), &error);
        EXPECT_TRUE(image.has_value()) << name << ": " << error;
        return image ? *image : PixelImage();
    }

    std::vector<PixelImage> lintFrames(const std::string& name, int count) {
        std::vector<PixelImage> frames;
        for (int f = 1; f <= count; f++) frames.push_back(lintFixture(name + "_f" + std::to_string(f)));
        return frames;
    }

    const LintResult& resultOf(const LintReport& report, const std::string& id) {
        for (const LintResult& r : report.results) {
            if (r.id == id) return r;
        }
        static LintResult none;
        ADD_FAILURE() << "no result " << id;
        return none;
    }

    // one lint on one picture
    LintResult lintOne(const std::vector<PixelImage>& frames, const std::string& id, LintInput input = {}) {
        input.frames = frames;
        LintOptions options;
        options.only = {id};
        LintReport report = pl::lint(input, options);
        EXPECT_EQ(report.results.size(), 1u) << id;
        return report.results.empty() ? LintResult() : report.results.front();
    }

    LintResult lintOne(const std::string& fixture, const std::string& id, LintInput input = {}) {
        return lintOne(std::vector<PixelImage>{lintFixture(fixture)}, id, input);
    }

    pixelart::Palette sweetie16() {
        pixelart::Palette p;
        p.name = "Sweetie 16";
        for (const char* hex : {"1a1c2c", "5d275d", "b13e53", "ef7d57", "ffcd75", "a7f070", "38b764", "257179",
                                "29366f", "3b5dc9", "41a6f6", "73eff7", "f4f4f4", "94b0c2", "566c86", "333c57"}) {
            pixelart::Rgba8 c;
            EXPECT_TRUE(pixelart::parseHex(hex, c));
            p.colors.push_back(c);
        }
        return p;
    }

    void expectStatus(const LintResult& r, LintStatus status, const char* what) {
        EXPECT_EQ(r.status, status) << what << ": " << r.id << " was " << lintStatusName(r.status) << " (measured " << r.measured
                                    << ", count " << r.count << ") " << r.message;
    }

}

TEST(PixelLint, CatalogueIsInR26Order) {
    const std::vector<std::string> expected = {"off_palette", "colors", "partial_alpha", "transparent_background", "size",
                                               "near_duplicates", "orphans", "jaggies", "doubles", "holes", "flicker",
                                               "contrast", "banding", "pillow_shading", "empty_frame", "one_frame", "frame_scale"};
    std::vector<std::string> ids(lintIds().begin(), lintIds().end());
    EXPECT_EQ(ids, expected);
    EXPECT_TRUE(isLintId("pillow_shading"));
    EXPECT_FALSE(isLintId("spill"));
    EXPECT_NE(lintIdList().find("off_palette, colors"), std::string::npos);
}

TEST(PixelLint, OffPaletteFailsOnAnyOffPixelAndSkipsWithoutAPalette) {
    const pixelart::Palette palette = sweetie16();
    LintInput input;
    input.palette = &palette;
    const LintResult pos = lintOne("off_palette_pos", "off_palette", input);
    expectStatus(pos, LintStatus::Fail, "3 off pixels");
    EXPECT_EQ(pos.measured, 3.0);
    EXPECT_NE(pos.fix.find("fix_pixel_art with palette: Sweetie 16"), std::string::npos) << pos.fix;
    expectStatus(lintOne("off_palette_neg", "off_palette", input), LintStatus::Pass, "on the palette");
    const LintResult skip = lintOne("off_palette_pos", "off_palette");
    expectStatus(skip, LintStatus::Skip, "no palette");
    EXPECT_EQ(skip.message, "no palette given");
}

TEST(PixelLint, ColorsFailsAboveTheRuleAndSkipsWithoutIt) {
    LintInput input;
    input.rules.maxColors = 4;
    const LintResult pos = lintOne("colors_pos", "colors", input);
    expectStatus(pos, LintStatus::Fail, "5 of 4");
    EXPECT_EQ(pos.measured, 5.0);
    EXPECT_EQ(pos.threshold, 4.0);
    EXPECT_EQ(pos.thresholdSource, "rules.max_colors");
    expectStatus(lintOne("colors_neg", "colors", input), LintStatus::Pass, "4 of 4");
    expectStatus(lintOne("colors_pos", "colors"), LintStatus::Skip, "no rule");
}

TEST(PixelLint, PartialAlphaFailsOnSoftPixels) {
    const LintResult pos = lintOne("partial_alpha_pos", "partial_alpha");
    expectStatus(pos, LintStatus::Fail, "soft edge");
    EXPECT_EQ(pos.count, 6);
    EXPECT_NE(pos.fix.find("fix_pixel_art"), std::string::npos);
    expectStatus(lintOne("partial_alpha_neg", "partial_alpha"), LintStatus::Pass, "hard edge");
}

TEST(PixelLint, TransparentBackgroundFailsWhenTheBorderIsDrawn) {
    LintInput input;
    input.rules.transparentBackground = true;
    const LintResult pos = lintOne("transparent_background_pos", "transparent_background", input);
    expectStatus(pos, LintStatus::Fail, "painted background");
    EXPECT_EQ(pos.measured, 36.0);   // lint.ts's border walk of a 10 x 10 frame: 2 x 10 + 2 x 8
    EXPECT_EQ(pos.threshold, 18.0);
    expectStatus(lintOne("transparent_background_neg", "transparent_background", input), LintStatus::Pass, "cut out");
    expectStatus(lintOne("transparent_background_pos", "transparent_background"), LintStatus::Skip, "not required");
}

TEST(PixelLint, SizeFailsOffTheAllowedSizes) {
    LintInput input;
    input.rules.sizes = {{16, 16}};
    const LintResult pos = lintOne("size_pos", "size", input);
    expectStatus(pos, LintStatus::Fail, "17x16");
    EXPECT_NE(pos.message.find("17x16"), std::string::npos);
    expectStatus(lintOne("size_neg", "size", input), LintStatus::Pass, "16x16");
    expectStatus(lintOne("size_neg", "size"), LintStatus::Skip, "no sizes");
}

TEST(PixelLint, NearDuplicatesWarnsOnColoursTooCloseToTellApart) {
    const LintResult pos = lintOne("near_duplicates_pos", "near_duplicates");
    expectStatus(pos, LintStatus::Warn, "120/121");
    EXPECT_EQ(pos.count, 1);
    EXPECT_LT(pos.measured, 0.03);
    expectStatus(lintOne("near_duplicates_neg", "near_duplicates"), LintStatus::Pass, "far apart");
}

TEST(PixelLint, OrphansWarnsOnALonePixelAndSkipsOnSheets) {
    const LintResult pos = lintOne("orphans_pos", "orphans");
    expectStatus(pos, LintStatus::Warn, "lone pixel");
    ASSERT_EQ(pos.at.size(), 1u);
    EXPECT_EQ(pos.at[0], (LintSpot{1, 4, 4}));
    expectStatus(lintOne("orphans_neg", "orphans"), LintStatus::Pass, "two-pixel highlight");
    LintInput sheet;
    sheet.sheet = true;
    expectStatus(lintOne("orphans_pos", "orphans", sheet), LintStatus::Skip, "a sheet");
}

TEST(PixelLint, JaggiesWarnsOn313AndPassesARegularRhythm) {
    const LintResult pos = lintOne("jaggies_pos", "jaggies");
    expectStatus(pos, LintStatus::Warn, "3-1-3-1-3");
    EXPECT_EQ(pos.count, 3);
    EXPECT_EQ(pos.threshold, 3.0);
    // the steps' first pixels: the 1 at x 4 (row 2), the 3 at x 5 (row 3), the 1 at x 8 (row 4)
    ASSERT_EQ(pos.at.size(), 3u);
    EXPECT_EQ(pos.at[0], (LintSpot{1, 4, 2}));
    EXPECT_EQ(pos.at[1], (LintSpot{1, 5, 3}));
    EXPECT_EQ(pos.at[2], (LintSpot{1, 8, 4}));
    const LintResult neg = lintOne("jaggies_neg", "jaggies");
    expectStatus(neg, LintStatus::Pass, "2-1-2-1");
    EXPECT_EQ(neg.count, 0);
}

TEST(PixelLint, DoublesWarnsOnLCorners) {
    const LintResult pos = lintOne("doubles_pos", "doubles");
    expectStatus(pos, LintStatus::Warn, "three L corners");
    EXPECT_EQ(pos.count, 3);
    EXPECT_EQ(pos.at[0], (LintSpot{1, 3, 1}));
    EXPECT_NE(pos.fix.find("edit_sprite: remove the corner pixel at 3,1"), std::string::npos) << pos.fix;
    expectStatus(lintOne("doubles_neg", "doubles"), LintStatus::Pass, "clean lines");
}

TEST(PixelLint, HolesWarnsOnPinholes) {
    const LintResult pos = lintOne("holes_pos", "holes");
    expectStatus(pos, LintStatus::Warn, "three pinholes");
    EXPECT_EQ(pos.count, 3);
    expectStatus(lintOne("holes_neg", "holes"), LintStatus::Pass, "solid");
}

TEST(PixelLint, FlickerWarnsOnAOneFrameBlinkAndSkipsUnderThreeFrames) {
    const LintResult pos = lintOne(lintFrames("flicker_pos", 4), "flicker");
    expectStatus(pos, LintStatus::Warn, "10% blink");
    EXPECT_DOUBLE_EQ(pos.measured, 0.1);
    EXPECT_EQ(pos.count, 10);
    EXPECT_EQ(pos.at[0].frame, 2);
    expectStatus(lintOne(lintFrames("flicker_neg", 4), "flicker"), LintStatus::Pass, "held two frames");
    std::vector<PixelImage> two = lintFrames("flicker_pos", 2);
    expectStatus(lintOne(two, "flicker"), LintStatus::Skip, "two frames");
}

TEST(PixelLint, ContrastWarnsOnADarkSpriteOnADarkBackdrop) {
    LintInput input;
    input.backdrops = {pixelart::Rgba8{0x10, 0x10, 0x18, 255}};
    input.backdropsSource = "pixel_art.backdrops";
    const LintResult pos = lintOne("contrast_pos", "contrast", input);
    expectStatus(pos, LintStatus::Warn, "dark on dark");
    EXPECT_DOUBLE_EQ(pos.measured, 1.0);
    EXPECT_EQ(pos.thresholdSource, "pixel_art.backdrops");
    expectStatus(lintOne("contrast_neg", "contrast", input), LintStatus::Pass, "light on dark");
    expectStatus(lintOne("contrast_pos", "contrast"), LintStatus::Skip, "no backdrop");
}

TEST(PixelLint, BandingWarnsOnParallelRampsAndPassesDitherAndAntiAliasing) {
    const LintResult pos = lintOne("banding_pos", "banding");
    expectStatus(pos, LintStatus::Warn, "three parallel ramps");
    EXPECT_EQ(pos.measured, 15.0);
    EXPECT_EQ(pos.count, 1);
    ASSERT_EQ(pos.at.size(), 1u);
    EXPECT_EQ(pos.at[0], (LintSpot{1, 2, 2}));
    const LintResult dither = lintOne("banding_neg", "banding");
    expectStatus(dither, LintStatus::Pass, "dithered ramp");
    EXPECT_EQ(dither.measured, 0.0);
    const LintResult corner = lintOne("banding_corner", "banding");
    expectStatus(corner, LintStatus::Pass, "anti-aliased corner");
    EXPECT_EQ(corner.measured, 0.0);
}

TEST(PixelLint, PillowShadingWarnsOnRingsPassesALightDirectionAndSkipsAFlatShape) {
    const LintResult pos = lintOne("pillow_shading_pos", "pillow_shading");
    expectStatus(pos, LintStatus::Warn, "concentric rings");
    EXPECT_GE(pos.measured, 0.6);
    EXPECT_EQ(pos.count, 3);   // the centroid and the two extreme rings
    const LintResult neg = lintOne("pillow_shading_neg", "pillow_shading");
    expectStatus(neg, LintStatus::Pass, "lit from the top-left");
    const LintResult flat = lintOne("pillow_shading_flat", "pillow_shading");
    expectStatus(flat, LintStatus::Skip, "flat");
}

TEST(PixelLint, SheetLintsFindEmptyAndLoneFramesAndOffScaleFrames) {
    std::vector<PixelImage> frames = {lintFixture("holes_neg"), PixelImage(12, 9), lintFixture("holes_neg")};
    LintInput input;
    input.frames = frames;
    input.animations = {{"walk", {0, 1}}, {"idle", {2}}};
    input.frameCells = {8.0, 0.0, 9.0};
    input.sharedCell = 8.0;
    LintOptions options;
    options.only = {"empty_frame", "one_frame", "frame_scale"};
    const LintReport report = pl::lint(input, options);
    const LintResult& empty = resultOf(report, "empty_frame");
    expectStatus(empty, LintStatus::Fail, "frame 2 empty");
    EXPECT_EQ(empty.at[0].frame, 2);
    const LintResult& one = resultOf(report, "one_frame");
    expectStatus(one, LintStatus::Warn, "idle has one frame");
    EXPECT_NE(one.message.find("idle"), std::string::npos);
    const LintResult& scale = resultOf(report, "frame_scale");
    expectStatus(scale, LintStatus::Warn, "9 against 8");
    EXPECT_EQ(scale.at[0].frame, 3);

    input.animations = {{"walk", {0, 2}}};
    input.frameCells = {8.0, 8.0, 8.2};
    const LintReport clean = pl::lint(input, options);
    expectStatus(resultOf(clean, "empty_frame"), LintStatus::Pass, "no empty frame");
    expectStatus(resultOf(clean, "one_frame"), LintStatus::Pass, "two frames");
    expectStatus(resultOf(clean, "frame_scale"), LintStatus::Pass, "within 5 %");

    LintInput single;
    single.frames = frames;
    const LintReport none = pl::lint(single, options);
    for (const LintResult& r : none.results) expectStatus(r, LintStatus::Skip, "no animations");
}

TEST(PixelLint, ThresholdOptionsAreReportedWithTheirSource) {
    LintOptions options;
    options.only = {"jaggies"};
    options.thresholds["jaggies"] = 4;
    std::vector<PixelImage> frames = {lintFixture("jaggies_pos")};
    LintInput input;
    input.frames = frames;
    const LintResult r = pl::lint(input, options).results.at(0);
    EXPECT_EQ(r.threshold, 4.0);
    EXPECT_EQ(r.thresholdSource, "options.jaggies");
    EXPECT_EQ(r.status, LintStatus::Pass);   // 3 spots under a raised threshold: why the guide forbids raising it
}

TEST(PixelLint, EveryResultHasTheR25KeysAndAtMostEightSpots) {
    const pixelart::Palette palette = sweetie16();
    std::vector<PixelImage> frames = lintFrames("flicker_pos", 4);
    // many spots: a frame of single pixels in a checkerboard
    PixelImage noisy(32, 32);
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            if ((x + y) % 2 == 0) noisy.set(x, y, {200, 50, 50, 255});
        }
    }
    for (const std::vector<PixelImage>& set : {frames, std::vector<PixelImage>{noisy}, std::vector<PixelImage>{lintFixture("pillow_shading_pos")}}) {
        LintInput input;
        input.frames = set;
        input.palette = &palette;
        input.backdrops = {pixelart::Rgba8{0, 0, 0, 255}};
        input.rules.maxColors = 2;
        input.rules.sizes = {{8, 8}};
        input.rules.transparentBackground = true;
        input.animations = {{"all", {0}}};
        input.frameCells = {4.0};
        input.sharedCell = 4.0;
        const LintReport report = pl::lint(input);
        ASSERT_EQ(report.results.size(), lintIds().size());
        for (const LintResult& r : report.results) {
            const nlohmann::json j = toJson(r);
            for (const char* key : {"id", "status", "measured", "threshold", "threshold_source", "count", "at"}) {
                EXPECT_TRUE(j.contains(key)) << r.id << " lacks " << key;
            }
            EXPECT_LE(r.at.size(), LintResult::kMaxSpots) << r.id;
            EXPECT_GE(r.count, (int)r.at.size()) << r.id;
            EXPECT_FALSE(r.thresholdSource.empty()) << r.id;
            if (r.status == LintStatus::Skip) {
                EXPECT_FALSE(r.message.empty()) << r.id;
            }
            if (r.status == LintStatus::Warn || r.status == LintStatus::Fail) {
                EXPECT_FALSE(r.fix.empty()) << r.id;
            }
            for (const LintSpot& s : r.at) EXPECT_GE(s.frame, 1) << r.id;
        }
    }
    // craft lints never FAIL (R26)
    LintInput input;
    std::vector<PixelImage> one = {noisy};
    input.frames = one;
    input.backdrops = {pixelart::Rgba8{200, 50, 50, 255}};
    for (const LintResult& r : pl::lint(input).results) {
        for (const char* craft : {"near_duplicates", "orphans", "jaggies", "doubles", "holes", "flicker", "contrast", "banding", "pillow_shading"}) {
            if (r.id == craft) {
                EXPECT_NE(r.status, LintStatus::Fail) << r.id;
            }
        }
    }
}

TEST(PixelLint, IsDeterministicOverRunsAndThreads) {
    const pixelart::Palette palette = sweetie16();
    std::vector<PixelImage> frames = lintFrames("flicker_pos", 4);
    frames.push_back(lintFixture("banding_pos"));   // a size of its own: per-frame lints still run
    LintInput input;
    input.frames = frames;
    input.palette = &palette;
    input.backdrops = {pixelart::Rgba8{0x10, 0x10, 0x18, 255}};
    const std::string first = toJson(pl::lint(input)).dump();
    for (int run = 0; run < 3; run++) EXPECT_EQ(toJson(pl::lint(input)).dump(), first) << "run " << run;
    std::vector<std::string> results(8);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++) {
        threads.emplace_back([&, t] { results[(size_t)t] = toJson(pl::lint(input)).dump(); });
    }
    for (std::thread& t : threads) t.join();
    for (const std::string& r : results) EXPECT_EQ(r, first);
}

TEST(PixelLint, NoFramesSkipsEveryLint) {
    const LintReport report = pl::lint(LintInput{});
    EXPECT_EQ(report.results.size(), lintIds().size());
    EXPECT_EQ(report.skip, (int)lintIds().size());
}

namespace {

    double lintElapsedMs(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void lintBenchReport(const char* what, double ms, double budget) {
        std::printf("[bench] PixelLintBench: %s %.3f ms (budget %.0f ms)\n", what, ms, budget);
        if (editor::test::perfSlack() > 0.0) {
            EXPECT_LT(ms, budget * editor::test::perfSlack()) << what;
        }
    }

    // a busy frame: noise in a few tones over shapes, so every lint has work
    PixelImage lintBenchFrame(int size, uint32_t seed) {
        PixelImage frame(size, size);
        uint32_t state = seed;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                state = state * 1664525u + 1013904223u;
                const int dx = x - size / 2, dy = y - size / 2;
                if (dx * dx + dy * dy > (size * size) / 5) continue;
                const uint8_t v = (uint8_t)(40 + ((state >> 24) % 5) * 40);
                frame.set(x, y, {v, (uint8_t)(v / 2), (uint8_t)(255 - v), 255});
            }
        }
        return frame;
    }

}

// R38: every lint on 16 frames of 64 x 64 <= 15 ms; banding and pillow_shading <= 5 ms per 128 x 128 frame
TEST(PixelLintBench, LintsStayInteractive) {
    const pixelart::Palette palette = sweetie16();
    std::vector<PixelImage> frames;
    for (uint32_t f = 0; f < 16; f++) frames.push_back(lintBenchFrame(64, f + 1));
    LintInput input;
    input.frames = frames;
    input.palette = &palette;
    input.backdrops = {pixelart::Rgba8{0, 0, 0, 255}, pixelart::Rgba8{255, 255, 255, 255}};
    input.rules.maxColors = 16;
    (void)pl::lint(input);   // warm the OKLab path
    double best = 1e9;
    for (int run = 0; run < 5; run++) {
        const auto start = std::chrono::steady_clock::now();
        const LintReport report = pl::lint(input);
        best = std::min(best, lintElapsedMs(start));
        ASSERT_EQ(report.results.size(), lintIds().size());
    }
    lintBenchReport("all lints, 16 frames of 64x64", best, 15.0);

    std::vector<PixelImage> big = {lintBenchFrame(128, 7)};
    for (const char* id : {"banding", "pillow_shading"}) {
        LintInput one;
        one.frames = big;
        LintOptions options;
        options.only = {id};
        double ms = 1e9;
        for (int run = 0; run < 5; run++) {
            const auto start = std::chrono::steady_clock::now();
            (void)pl::lint(one, options);
            ms = std::min(ms, lintElapsedMs(start));
        }
        lintBenchReport((std::string(id) + ", one 128x128 frame").c_str(), ms, 5.0);
    }
}
