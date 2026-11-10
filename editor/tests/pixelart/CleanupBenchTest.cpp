// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The cleanup's budgets (docs/specs/S46-pixel-art-cleanup.md R36, R37, R39, AC16) on pictures built
// here at the spec's sizes: printed as [bench] lines, enforced with perfSlack() on the benchmark
// server only (a shared builder's load alone misses wall-clock budgets). R38's lint budgets are
// PixelLintBench's (LintTest.cpp).

#include "pixelart/cleanup/CleanupAnimation.h"
#include "pixelart/cleanup/CleanupGrid.h"
#include "pixelart/cleanup/CleanupPipeline.h"
#include "pixelart/cleanup/CleanupSynthetic.h"
#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::cleanup;

namespace {

    double msSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void reportBudget(const char* what, double ms, double budget) {
        std::printf("[bench] CleanupBench: %s %.2f ms (budget %.0f ms)\n", what, ms, budget);
        if (jasperengine::editor::test::perfSlack() > 0.0) {
            EXPECT_LT(ms, budget * jasperengine::editor::test::perfSlack()) << what;
        }
    }

    // R36 measures with 8 workers
    struct EightWorkers {
        EightWorkers() {
            setParallelFor([](int count, const std::function<void(int)>& body) {
                std::atomic<int> next{0};
                std::vector<std::thread> workers;
                for (int w = 0; w < 8; w++) {
                    workers.emplace_back([&]() {
                        for (int i = next++; i < count; i = next++) body(i);
                    });
                }
                for (std::thread& worker : workers) worker.join();
            });
        }
        ~EightWorkers() { setParallelFor(nullptr); }
    };

    RgbaImage benchUpscaled(int cells, double scale, uint32_t seed) {
        Upscale options;
        options.scale = scale;
        options.blur = 1;
        options.noise = 4;
        options.seed = seed;
        return upscale(makeSprite(cells, cells, sweetie16(), seed), options);
    }

}

TEST(CleanupBench, DetectionWithEightWorkers) {
    EightWorkers workers;
    for (const auto& [cells, budget] : {std::pair<int, double>{128, 120.0}, std::pair<int, double>{256, 450.0}}) {
        const RgbaImage image = benchUpscaled(cells, 8.0, 11);
        const auto start = std::chrono::steady_clock::now();
        const std::optional<Detection> found = detectGrid(image);
        const double ms = msSince(start);
        ASSERT_TRUE(found.has_value());
        EXPECT_NEAR(found->spec.cellWidth, 8.0, 0.05);
        reportBudget(cells == 128 ? "detectGrid 1024x1024" : "detectGrid 2048x2048", ms, budget);
    }
}

TEST(CleanupBench, WholeFixOfOnePicture) {
    const RgbaImage image = benchUpscaled(128, 8.0, 11);
    Recipe recipe;
    recipe.paletteMode = PaletteMode::Auto;
    recipe.autoPalette = AutoPalette{32, ProposeMethod::Dominant};
    const auto start = std::chrono::steady_clock::now();
    const std::optional<Outcome> outcome = runRecipe(image, recipe);
    const double ms = msSince(start);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->art.width, 128);
    reportBudget("whole fix 1024x1024, 32-colour auto palette", ms, 300.0);
}

TEST(CleanupBench, WholeFixOfASheet) {
    std::vector<PixelArt> frames;
    // 2008 x 1000: the 84-row sprite R37's 1024 would need lands fakeSheet's frame boxes 3 rows from its
    // painted grid, and ritmo's removeGridLines then takes the real fill for a grid line (spec, Deviations)
    for (uint32_t f = 0; f < 8; f++) frames.push_back(makeSprite(30, 82, sweetie16(), 21 + f, true));
    const FakeSheet sheetArt = fakeSheet(frames, 6.0);
    std::printf("[bench] CleanupBench: sheet picture %d x %d\n", sheetArt.image.width(), sheetArt.image.height());
    Recipe recipe;
    recipe.removeBackground = true;
    recipe.paletteMode = PaletteMode::Auto;
    recipe.autoPalette = AutoPalette{32, ProposeMethod::Dominant};
    const auto start = std::chrono::steady_clock::now();
    AnimationCache cache;
    Sheet sheet;
    sheet.frames = detectFrames(sheetArt.image, recipe, cache);
    ASSERT_EQ(sheet.frames.size(), frames.size());
    std::vector<int> all(sheet.frames.size());
    for (size_t i = 0; i < all.size(); i++) all[i] = static_cast<int>(i);
    sheet.animations.push_back(SheetAnimation{"animation-1", all});
    const AnimationOutcome outcome = runSheet(sheetArt.image, recipe, sheet, cache);
    const double ms = msSince(start);
    EXPECT_EQ(outcome.frames.size(), frames.size());
    reportBudget("whole fix of an 8-frame sheet", ms, 900.0);
}

TEST(CleanupBench, InteractiveRerunAfterOneSlider) {
    const RgbaImage image = benchUpscaled(128, 8.0, 11);
    Recipe recipe;
    recipe.palette = RecipePalette{"Sweetie 16", sweetie16(), {}, "", ""};
    recipe.paletteMode = PaletteMode::Fixed;
    recipe.cleanup.size = 2;
    PipelineCache cache;
    const std::optional<ResolvedGrid> grid = resolveGrid(image, recipe.grid);
    ASSERT_TRUE(grid.has_value());
    (void)runWithSpec(image, recipe, grid->spec, grid->detection, &cache);
    recipe.cleanup.strength = 0.9;
    const auto start = std::chrono::steady_clock::now();
    const Outcome outcome = runWithSpec(image, recipe, grid->spec, grid->detection, &cache);
    const double ms = msSince(start);
    EXPECT_EQ(outcome.art.width, 128);
    reportBudget("re-run of a 128x128 output after one slider", ms, 33.0);
}
