// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/test/agent.test.ts, "check"):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// S46 AC11 unit part (R6, R29): the verdicts. ritmo's "check" cases with the same generator and
// seeds, plus S46's stricter guards: a photo, a palette that loses cells, an empty frame, an
// unsure shared cell, each listed with its value and threshold.

#include "pixelart/cleanup/CleanupReport.h"
#include "pixelart/cleanup/CleanupSample.h"
#include "pixelart/cleanup/CleanupSheet.h"
#include "pixelart/cleanup/CleanupSynthetic.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::cleanup;
using jasperengine::pixelart::Rgba8;

namespace {

    RecipePalette recipePalette(std::string name, std::vector<Rgba8> colors) {
        RecipePalette palette;
        palette.name = std::move(name);
        palette.colors = std::move(colors);
        return palette;
    }

    Recipe recipeWith(const RecipePalette& palette) {
        Recipe recipe;
        recipe.paletteMode = PaletteMode::Fixed;
        recipe.palette = palette;
        return recipe;
    }

    bool hasGuard(const Report& report, const std::string& id) {
        for (const Guard& guard : report.guards) {
            if (guard.id == id) return true;
        }
        return false;
    }

    const lint::LintResult* findLint(const Report& report, const std::string& id) {
        for (const lint::LintResult& r : report.lints.results) {
            if (r.id == id) return &r;
        }
        return nullptr;
    }

    Upscale upscaleOf(double scale, double offset, int blur = 0, double noise = 0, uint32_t seed = 7) {
        Upscale options;
        options.scale = scale;
        options.offset = offset;
        options.blur = blur;
        options.noise = noise;
        options.seed = seed;
        return options;
    }

}

// AC11: the AC2 recovery cases are fixable
TEST(CleanupReport, TheRecoveryCasesAreFixable) {
    const PixelArt sprite = makeSprite(32, 24, sweetie16(), 11);
    const Recipe recipe = recipeWith(recipePalette("Sweetie 16", sweetie16()));
    for (const Upscale& options : {upscaleOf(8, 0), upscaleOf(5, 2), upscaleOf(7.6, 3, 1, 6), upscaleOf(12.3, 5, 2, 4), upscaleOf(3, 1)}) {
        const Checked checked = checkImage(upscale(sprite, options), recipe);
        EXPECT_EQ(checked.report.verdict, Verdict::Fixable) << options.scale << ": " << checked.report.summary;
        EXPECT_EQ(checked.report.kind, "upscaled") << options.scale;
        EXPECT_GT(checked.report.confidence, 0.5) << options.scale;
        EXPECT_TRUE(checked.report.guards.empty()) << options.scale;
        EXPECT_EQ(checked.report.outputWidth, 32) << options.scale;
        EXPECT_EQ(checked.report.outputHeight, 24) << options.scale;
    }
}

// ritmo: "finds upscaled pixel art fixable, with the grid it found"
TEST(CleanupReport, TheDemoIsFixableWithTheGridItFound) {
    const Checked checked = checkImage(demoImage(), recipeWith(recipePalette("Sweetie 16", sweetie16())));
    EXPECT_EQ(checked.report.verdict, Verdict::Fixable) << checked.report.summary;
    EXPECT_EQ(checked.report.kind, "upscaled");
    EXPECT_EQ(checked.report.outputWidth, 16);
    EXPECT_EQ(checked.report.outputHeight, 16);
    ASSERT_TRUE(checked.report.grid);
    EXPECT_GT(checked.report.grid->fit, 0.95);
    // the resolved recipe holds the grid found, so a re-run does not detect again
    EXPECT_EQ(checked.recipe.grid.kind, GridKind::Cells);
}

// ritmo: "finds pixel art at its own size clean"; AC11: the native mushroom is clean
TEST(CleanupReport, TheNativeMushroomIsClean) {
    const Checked checked = checkImage(toRgba(demoSprite()), Recipe());
    EXPECT_EQ(checked.report.verdict, Verdict::Clean) << checked.report.summary;
    EXPECT_EQ(checked.report.kind, "pixel-art");
    EXPECT_EQ(checked.report.confidence, 1);
    EXPECT_EQ(checked.report.outputWidth, 16);
    EXPECT_EQ(checked.report.guards.size(), 0u);
}

// R29: a WARN keeps art from being clean only under fail_on: warn
TEST(CleanupReport, AWarningMakesArtFixableOnlyWhenAsked) {
    // two colours too close to tell apart: near_duplicates WARNs
    PixelArt art;
    art.width = art.height = 4;
    art.palette = {Rgba8{100, 100, 100, 255}, Rgba8{101, 100, 100, 255}};
    art.indices.assign(16, 0);
    art.indices[5] = 1;
    const Report relaxed = reportOfArt(art);
    ASSERT_GT(relaxed.lints.warn, 0);
    EXPECT_EQ(relaxed.verdict, Verdict::Clean);
    ReportOptions strict;
    strict.failOnWarn = true;
    EXPECT_EQ(reportOfArt(art, strict).verdict, Verdict::Fixable);
}

// AC11: the mushroom with 3 off-palette pixels is fixable, and off_palette is what it says
TEST(CleanupReport, ThreeOffPalettePixelsAreFixable) {
    RgbaImage image = toRgba(demoSprite());
    image.set(2, 7, {0xb4, 0x41, 0x55, 255});
    image.set(9, 3, {0xb0, 0x40, 0x50, 255});
    image.set(5, 10, {0x5e, 0x29, 0x5e, 255});
    const Checked checked = checkImage(image, recipeWith(recipePalette("Sweetie 16", sweetie16())));
    EXPECT_EQ(checked.report.verdict, Verdict::Fixable) << checked.report.summary;
    EXPECT_EQ(checked.report.kind, "pixel-art");
    const lint::LintResult* off = findLint(checked.report, "off_palette");
    ASSERT_NE(off, nullptr);
    EXPECT_EQ(off->status, lint::LintStatus::Fail);
    EXPECT_EQ(off->measured, 3);
    EXPECT_FALSE(hasGuard(checked.report, "off_palette"));
}

// A rule the pipeline cannot fix keeps native art from being fixable (R29)
TEST(CleanupReport, ANativeFailThePipelineCannotFixNeedsAPerson) {
    ReportOptions options;
    options.rules.sizes = {{32, 32}};
    const Report report = reportOfArt(demoSprite(), options);
    EXPECT_EQ(report.verdict, Verdict::NeedsPerson);
    EXPECT_TRUE(hasGuard(report, "size"));
}

// R29: native art is fixable only when its OUTPUT has no FAIL. Without a palette of one colour the
// fix keeps the mushroom's colours, so max_colors 1 still fails on what would be written: that is
// needs_person, not fixable (the earlier pin read the verdict from the source's lints)
TEST(CleanupReport, ANativeFailTheOutputKeepsNeedsAPerson) {
    ReportOptions colors;
    colors.rules.maxColors = 1;
    const Checked checked = checkImage(toRgba(demoSprite()), Recipe(), colors);
    EXPECT_EQ(checked.report.verdict, Verdict::NeedsPerson) << checked.report.summary;
    EXPECT_TRUE(hasGuard(checked.report, "colors"));
    const lint::LintResult* lintColors = findLint(checked.report, "colors");
    ASSERT_NE(lintColors, nullptr);
    EXPECT_EQ(lintColors->status, lint::LintStatus::Fail);
}

// Partial alpha counts on the source, though the fixed art has none
TEST(CleanupReport, PartialAlphaIsReadFromTheSource) {
    RgbaImage image = toRgba(demoSprite());
    image.set(3, 3, {0x1a, 0x1c, 0x2c, 200});
    image.set(4, 3, {0x1a, 0x1c, 0x2c, 40});
    const Checked checked = checkImage(image, Recipe());
    EXPECT_EQ(checked.report.partialAlpha, 2);
    const lint::LintResult* partial = findLint(checked.report, "partial_alpha");
    ASSERT_NE(partial, nullptr);
    EXPECT_EQ(partial->status, lint::LintStatus::Fail);
    EXPECT_EQ(partial->measured, 2);
    EXPECT_EQ(checked.report.verdict, Verdict::Fixable);
}

// ritmo: "takes a small sprite with an AI's noisy colors as pixel art at its own size, though a grid shows in it"
TEST(CleanupReport, ANoisySmallSpriteIsNative) {
    std::vector<Rgba8> pico = {{0, 0, 0, 255}, {0x1d, 0x2b, 0x53, 255}, {0x7e, 0x25, 0x53, 255}, {0, 0x87, 0x51, 255}, {0xab, 0x52, 0x36, 255}, {0x5f, 0x57, 0x4f, 255}};
    RgbaImage image = toRgba(makeSprite(24, 24, pico, 7, true));
    std::span<uint8_t> data = image.editPixels();
    for (size_t i = 0; i < data.size(); i++) {
        if (i % 4 != 3 && data[i - (i % 4) + 3] > 0) data[i] = (uint8_t)std::clamp(int(data[i]) + int((i * 7919) % 7) - 3, 0, 255);
    }
    EXPECT_TRUE(isNative(image, runRecipe(image, Recipe())));
    const Checked checked = checkImage(image, Recipe());
    EXPECT_EQ(checked.report.outputWidth, 24);
    EXPECT_EQ(checked.report.outputHeight, 24);
}

// ritmo: "asks for a person when the grid given does not fit"
TEST(CleanupReport, AGivenGridThatDoesNotFitNeedsAPerson) {
    const PixelArt sprite = makeSprite(20, 20, sweetie16(), 7);
    const RgbaImage image = upscale(sprite, upscaleOf(10, 0, 1, 4, 3));
    Recipe wrong;
    wrong.grid.kind = GridKind::Cells;
    wrong.grid.spec = {7, 7, 0, 0};
    const Checked bad = checkImage(image, wrong);
    EXPECT_EQ(bad.report.verdict, Verdict::NeedsPerson);
    EXPECT_TRUE(hasGuard(bad.report, "low_fit"));
    Recipe right;
    right.grid.kind = GridKind::Cells;
    right.grid.spec = {10, 10, 0, 0};
    EXPECT_EQ(checkImage(image, right).report.verdict, Verdict::Fixable);
}

// AC11: a photo needs a person, with guard no_grid or low fit
TEST(CleanupReport, APhotoNeedsAPerson) {
    const std::filesystem::path path = std::filesystem::path(JASPER_EDITOR_TEST_FIXTURES) / "pixelart" / "cleanup" / "photo.jpg";
    std::string error;
    const std::optional<PixelImage> photo = PixelImage::loadPng(path, &error);
    ASSERT_TRUE(photo.has_value()) << error;
    const Checked checked = checkImage(*photo, Recipe());
    EXPECT_EQ(checked.report.verdict, Verdict::NeedsPerson) << checked.report.summary;
    EXPECT_TRUE(hasGuard(checked.report, "no_grid") || hasGuard(checked.report, "low_fit") || hasGuard(checked.report, "low_confidence"));
    for (const Guard& guard : checked.report.guards) EXPECT_FALSE(guard.id.empty());
}

// AC11: a palette losing 8 % of the cells needs a person, with guard lost_share
TEST(CleanupReport, APaletteLosingCellsNeedsAPerson) {
    // Greys, plus 8 % of the cells in a saturated accent the palette has no match for
    const std::vector<Rgba8> greys = {{20, 20, 20, 255}, {90, 90, 90, 255}, {170, 170, 170, 255}, {240, 240, 240, 255}};
    PixelArt sprite = makeSprite(25, 20, greys, 4);
    sprite.palette.push_back(Rgba8{250, 20, 200, 255});
    const int accent = (int)sprite.palette.size() - 1;
    for (int i = 0; i < 40; i++) sprite.indices[i] = accent;   // 40 of 500 cells, side by side
    const RgbaImage image = upscale(sprite, upscaleOf(8, 0));
    const Checked checked = checkImage(image, recipeWith(recipePalette("Greys", greys)));
    EXPECT_EQ(checked.report.verdict, Verdict::NeedsPerson) << checked.report.summary;
    ASSERT_TRUE(hasGuard(checked.report, "lost_share"));
    ASSERT_FALSE(checked.report.lost.empty());
    EXPECT_GE(checked.report.lost[0].pixels, 40);
    EXPECT_LE(checked.report.lost[0].at.size(), 20u);
    for (const Guard& guard : checked.report.guards) {
        if (guard.id == "lost_share") {
            EXPECT_NEAR(guard.value, 0.08, 0.02);
            EXPECT_EQ(guard.threshold, 0.05);
        }
    }
}

// R2 size: there is no true grid, and the summary says it is a judgement call
TEST(CleanupReport, PixelatingIsAJudgementCall) {
    const RgbaImage image = upscale(makeSprite(12, 8, sweetie16(), 2), upscaleOf(9.5, 0, 3, 10, 1));
    Recipe recipe = recipeWith(recipePalette("Sweetie 16", sweetie16()));
    recipe.grid.kind = GridKind::Size;
    recipe.grid.width = 24;
    const Checked checked = checkImage(image, recipe);
    EXPECT_EQ(checked.report.verdict, Verdict::Fixable) << checked.report.summary;
    EXPECT_NE(checked.report.summary.find("judgement call"), std::string::npos);
    EXPECT_EQ(checked.report.outputWidth, 24);
    EXPECT_EQ(checked.report.outputHeight, 16);
}

// ritmo: "prefers the cleaner of two versions"
TEST(CleanupReport, CompareReportsPrefersTheCleaner) {
    ReportOptions four, one;
    four.rules.maxColors = 16;
    one.rules.maxColors = 1;
    const Report clean = reportOfArt(demoSprite(), four);
    const Report broken = reportOfArt(demoSprite(), one);
    EXPECT_EQ(compareReports(broken, clean).better, 'b');
    EXPECT_EQ(compareReports(clean, clean).better, '=');
}

namespace {

    struct SheetCase {
        RgbaImage image;
        Recipe recipe;
        Sheet sheet;
        AnimationOutcome outcome;
    };

    // The cell is given: a sheet's verdict is judged on its fit and rules here, not on how sure
    // detection is of a small random sprite (ritmo's own test gives the cell for the same reason)
    SheetCase runFakeSheet() {
        const std::vector<Rgba8> sweetie = sweetie16();
        const std::vector<Rgba8> fire = {sweetie[2], sweetie[3], sweetie[4], sweetie[1]};
        const PixelArt sprite = makeSprite(24, 24, fire, 3, true);
        std::vector<PixelArt> frames;
        for (int f = 0; f < 5; f++) frames.push_back(place(sprite, 26, 28, 1, 2 + (f % 2)));
        SheetCase c;
        c.image = fakeSheet(frames, 8, 5, false).image;
        c.recipe.removeBackground = true;
        c.recipe.grid.kind = GridKind::Cells;
        c.recipe.grid.spec = {8, 8, 0, 0};
        AnimationCache cache;
        c.sheet = withFrames(Sheet{}, detectFrames(c.image, c.recipe, cache));
        c.outcome = runSheet(c.image, c.recipe, c.sheet, cache);
        return c;
    }

}

TEST(CleanupReport, ASheetIsFixableAndDescribesItsAnimations) {
    const SheetCase c = runFakeSheet();
    const Report report = reportOfSheet(c.image, c.recipe, c.outcome);
    EXPECT_EQ(report.verdict, Verdict::Fixable) << report.summary;
    EXPECT_EQ(report.kind, "animation");
    ASSERT_TRUE(report.animation);
    EXPECT_EQ(report.animation->frames, 5);
    EXPECT_EQ(report.animation->included, 5);
    ASSERT_EQ(report.animation->clips.size(), 1u);
    EXPECT_EQ(report.animation->clips[0].frames, (std::vector<int>{1, 2, 3, 4, 5}));
    const lint::LintResult* orphans = findLint(report, "orphans");
    ASSERT_NE(orphans, nullptr);
    EXPECT_EQ(orphans->status, lint::LintStatus::Skip);   // sparks are single pixels on purpose
    const nlohmann::json json = reportToJson(report);
    EXPECT_EQ(json["verdict"], "fixable");
    EXPECT_TRUE(json.contains("lints"));
    EXPECT_EQ(json["animation"]["animations"][0]["loop"], "forward");
}

TEST(CleanupReport, AnEmptyFrameTripsItsGuard) {
    SheetCase c = runFakeSheet();
    // A box over the empty top margin of the sheet: its frame comes out with nothing in it
    Sheet sheet = c.sheet;
    sheet.frames.push_back(frameOf({0, 0, 40, 8}));
    sheet.animations[0].frames.push_back((int)sheet.frames.size() - 1);
    AnimationCache cache;
    const AnimationOutcome outcome = runSheet(c.image, c.recipe, sheet, cache);
    const Report report = reportOfSheet(c.image, c.recipe, outcome);
    EXPECT_EQ(report.verdict, Verdict::NeedsPerson) << report.summary;
    EXPECT_TRUE(hasGuard(report, "empty_frame"));
    const lint::LintResult* empty = findLint(report, "empty_frame");
    ASSERT_NE(empty, nullptr);
    EXPECT_EQ(empty->status, lint::LintStatus::Fail);
}

TEST(CleanupReport, AnUnsureSharedCellTripsItsGuard) {
    SheetCase c = runFakeSheet();
    c.outcome.confidence = 0.2;
    const Report report = reportOfSheet(c.image, c.recipe, c.outcome);
    EXPECT_EQ(report.verdict, Verdict::NeedsPerson);
    ASSERT_TRUE(hasGuard(report, "shared_cell"));
    EXPECT_NEAR(report.confidence, 0.2, 1e-9);
}

// R29: a sheet whose frames show no grid at all has no shared cell to trust (detection found
// nothing, so the 8 px fallback is a guess): needs_person, not fixable
TEST(CleanupReport, ASheetWithNoGridFoundNeedsAPerson) {
    std::vector<RgbaImage> frames;
    for (int f = 0; f < 3; f++) {
        RgbaImage frame(16, 16);
        frame.fill({90, 140, 200, 255});
        frames.push_back(frame);
    }
    AnimationCache cache;
    const Recipe recipe;
    const AnimationOutcome outcome = runFrames(frames, {}, "flat", recipe, std::nullopt, cache);
    const Report report = reportOfSheet(frames[0], recipe, outcome);
    EXPECT_EQ(report.verdict, Verdict::NeedsPerson) << report.summary;
    EXPECT_TRUE(hasGuard(report, "shared_cell"));
}

// R29: a sheet with no animation to export writes nothing: needs_person, not fixable
TEST(CleanupReport, ASheetWithNoClipsNeedsAPerson) {
    SheetCase c = runFakeSheet();
    Sheet sheet = c.sheet;
    for (SheetAnimation& animation : sheet.animations) animation.include = false;
    AnimationCache cache;
    const AnimationOutcome outcome = runSheet(c.image, c.recipe, sheet, cache);
    const Report report = reportOfSheet(c.image, c.recipe, outcome);
    EXPECT_EQ(report.verdict, Verdict::NeedsPerson) << report.summary;
    EXPECT_TRUE(hasGuard(report, "no_clips"));
}

// R29: a sheet read on a grid that does not fit its frames trips low_fit, as a single picture does
TEST(CleanupReport, ASheetThatFitsBadlyNeedsAPerson) {
    SheetCase c = runFakeSheet();
    c.recipe.grid.spec = {13, 13, 0, 0};
    AnimationCache cache;
    const AnimationOutcome outcome = runSheet(c.image, c.recipe, c.sheet, cache);
    const Report report = reportOfSheet(c.image, c.recipe, outcome);
    EXPECT_EQ(report.verdict, Verdict::NeedsPerson) << report.summary;
    EXPECT_TRUE(hasGuard(report, "low_fit"));
}
