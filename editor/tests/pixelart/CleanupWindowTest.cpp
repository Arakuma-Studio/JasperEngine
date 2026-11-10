// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Pixel Art Cleanup window's model (docs/specs/S46-pixel-art-cleanup.md R39, R42, R43) through its
// scripted API and PixelArtCleanupWindowTestAccess, with the demo source (nothing here needs a project,
// a GPU or an ImGui frame): the local undo stack, moved lines, Compare's image on the source's cells.

#include <gtest/gtest.h>

#include "jobs/JobSystem.h"
#include "support/TextureCorpus.h"
#include "window/PixelArtCleanupEntry.h"
#include "pixelart/cleanup/CleanupRecipe.h"
#include "pixelart/cleanup/CleanupSynthetic.h"
#include "window/PixelArtCleanupWindow.h"

#include <cmath>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace cu = jasperengine::editor::pixelart::cleanup;

namespace jasperengine::editor {
    namespace lint = pixelart::lint;
    struct PixelArtCleanupWindowTestAccess {
        static const pixelart::PixelImage& compare(const PixelArtCleanupWindow& w) { return w.shown->compare; }
        static const pixelart::PixelImage& result(const PixelArtCleanupWindow& w) { return w.shown->image; }
        static void tick(PixelArtCleanupWindow& w) { w.tick(); }
        static void focusAndFlash(PixelArtCleanupWindow& w, double x0, double y0, double x1, double y1) { w.focusOn(x0, y0, x1, y1, true); }
        static double flashWidth(const PixelArtCleanupWindow& w) { return w.flashCellW; }
        static bool dirty(const PixelArtCleanupWindow& w) { return w.dirty; }
        static const void* shown(const PixelArtCleanupWindow& w) { return w.shown.get(); }
        static void predict(PixelArtCleanupWindow& w, double lastMs, double fullMs) {
            w.lastRunMs = lastMs;
            w.lastFullRunMs = fullMs;
        }
        static void waitForJob(PixelArtCleanupWindow& w) { w.waitForRunningJob(); }
        static void adopt(PixelArtCleanupWindow& w, cu::Prepared&& prepared, const std::string& name) {
            w.adopt(std::move(prepared), name, true);
        }
        static bool spotRect(const PixelArtCleanupWindow& w, const lint::LintSpot& spot, double& x0, double& y0, double& x1, double& y1) {
            return w.lintSpotRect(spot, x0, y0, x1, y1);
        }
        static bool cellInView(const PixelArtCleanupWindow& w, int frame, int ax, int ay, double& x0, double& y0, double& x1, double& y1) {
            return w.cellRectInView(frame, ax, ay, x0, y0, x1, y1);
        }
        static const std::vector<cu::AtlasRect>& atlas(const PixelArtCleanupWindow& w) { return w.shown->atlasRects; }
        static void setFocused(PixelArtCleanupWindow& w, bool value) { w.focused = value; }
        static bool cacheHolds(const PixelArtCleanupWindow& w) { return w.cache && (w.cache->pipeline.hasGrid || w.cache->pipeline.hasSamples); }
        static double lastRunMs(const PixelArtCleanupWindow& w) { return w.lastRunMs; }
        static double lastFullRunMs(const PixelArtCleanupWindow& w) { return w.lastFullRunMs; }
        static void applyCancelled(PixelArtCleanupWindow& w, double ms) {
            auto bundle = std::make_shared<PixelArtCleanupWindow::Bundle>();
            bundle->result.cancelled = true;
            bundle->ms = ms;
            w.applyBundle(bundle, true);
        }
        static void stroke(PixelArtCleanupWindow& w, int x, int y, bool area) {
            w.beginStroke(x, y, area);
            w.painting = false;
            w.commitEdit();
        }
    };
}

using Access = jasperengine::editor::PixelArtCleanupWindowTestAccess;

namespace {

    double movedPosition(const cu::Recipe& recipe, const std::string& id) {
        for (const auto& entry : recipe.moved) {
            if (entry.first == id) return entry.second;
        }
        return -1.0;
    }

}

TEST(CleanupWindow, OpensTheDemoAndFindsAVerdict) {
    PixelArtCleanupWindow window(nullptr);
    EXPECT_TRUE(window.openDemo());
    window.runNow();
    ASSERT_TRUE(window.hasResult());
    EXPECT_EQ(window.result()->report.verdict, jasperengine::editor::pixelart::Verdict::Fixable);
    EXPECT_FALSE(window.isStale());
}

TEST(CleanupWindow, CompareIsTheSourceSizedResult) {
    PixelArtCleanupWindow window(nullptr);
    window.openDemo();
    window.runNow();
    ASSERT_TRUE(window.hasResult());
    const auto& compare = Access::compare(window);
    const auto* prepared = window.getPrepared();
    ASSERT_NE(prepared, nullptr);
    EXPECT_EQ(compare.width(), prepared->frames[0].width());
    EXPECT_EQ(compare.height(), prepared->frames[0].height());
    // the result at its own size is the demo's 16 x 16 mushroom
    EXPECT_EQ(Access::result(window).width(), 16);
}

TEST(CleanupWindow, AMovedLineIsOneUndoStepAndChangesTheGrid) {
    PixelArtCleanupWindow window(nullptr);
    window.openDemo();
    window.runNow();
    ASSERT_TRUE(window.hasResult());
    ASSERT_TRUE(window.result()->outcome.has_value());
    const std::vector<double> before = window.result()->outcome->grid.xs;
    ASSERT_GT(before.size(), 4u);
    const double moved = before[3] + 2.0;
    window.moveGridLine('x', 3, moved);
    EXPECT_EQ(window.undoDepth(), 1u);
    EXPECT_NEAR(movedPosition(window.getRecipe(), "x:3"), moved, 0.01);
    EXPECT_TRUE(window.isStale());
    window.runNow();
    EXPECT_NEAR(window.result()->outcome->grid.xs[3], moved, 0.01);
    // undo takes the moved line out, redo puts it back
    EXPECT_TRUE(window.undoEdit());
    EXPECT_TRUE(window.getRecipe().moved.empty());
    EXPECT_TRUE(window.canRedo());
    EXPECT_TRUE(window.redoEdit());
    EXPECT_NEAR(movedPosition(window.getRecipe(), "x:3"), moved, 0.01);
}

TEST(CleanupWindow, ADragIsOneUndoStep) {
    PixelArtCleanupWindow window(nullptr);
    window.openDemo();
    window.beginEdit();
    window.setGridLine('x', 3, 10.0);
    window.setGridLine('x', 3, 11.0);
    window.setGridLine('x', 3, 12.0);
    window.commitEdit();
    EXPECT_EQ(window.undoDepth(), 1u);
    EXPECT_NEAR(movedPosition(window.getRecipe(), "x:3"), 12.0, 0.01);
    window.undoEdit();
    EXPECT_TRUE(window.getRecipe().moved.empty());
    EXPECT_EQ(window.undoDepth(), 0u);
}

TEST(CleanupWindow, ACommittedEditWithNoChangeAddsNoStep) {
    PixelArtCleanupWindow window(nullptr);
    window.openDemo();
    window.beginEdit();
    window.commitEdit();
    window.applyEdit([](cu::Recipe& r) { r.seed = r.seed; });
    EXPECT_EQ(window.undoDepth(), 0u);
}

TEST(CleanupWindow, AnEditIsStaleUntilTheNextRun) {
    PixelArtCleanupWindow window(nullptr);
    window.openDemo();
    window.runNow();
    EXPECT_FALSE(window.isStale());
    window.applyEdit([](cu::Recipe& r) { r.crop = true; });
    EXPECT_TRUE(window.isStale());
    window.runNow();
    EXPECT_FALSE(window.isStale());
}

TEST(CleanupWindow, TheDemoCannotBeWrittenOrSaved) {
    PixelArtCleanupWindow window(nullptr);
    window.openDemo();
    window.runNow();
    std::string error;
    EXPECT_FALSE(window.write(error));
    EXPECT_NE(error.find("demo"), std::string::npos);
    error.clear();
    EXPECT_FALSE(window.saveAsImportStep(error));
    EXPECT_FALSE(error.empty());
}

TEST(CleanupWindow, AHandEraseIsOneStepAndClearsThePixel) {
    PixelArtCleanupWindow window(nullptr);
    window.openDemo();
    window.runNow();
    ASSERT_TRUE(window.hasResult());
    const cu::PixelArt& art = window.result()->outcome->art;
    ASSERT_EQ(art.width, 16);
    const int x = 8, y = 8;
    ASSERT_NE(art.indices[static_cast<size_t>(y) * art.width + x], cu::TRANSPARENT);
    window.setTool(PixelArtCleanupWindow::HandTool::Erase);
    Access::stroke(window, x, y, false);
    EXPECT_EQ(window.undoDepth(), 1u);
    EXPECT_EQ(window.getRecipe().edits.strokes.size(), 1u);
    window.runNow();
    EXPECT_EQ(window.result()->outcome->art.indices[static_cast<size_t>(y) * 16 + x], cu::TRANSPARENT);
    window.undoEdit();
    EXPECT_TRUE(window.getRecipe().edits.strokes.empty());
    window.runNow();
    EXPECT_NE(window.result()->outcome->art.indices[static_cast<size_t>(y) * 16 + x], cu::TRANSPARENT);
}

TEST(CleanupWindow, ReopeningARunningWindowDropsThePreviousEdits) {
    PixelArtCleanupWindow window(nullptr);
    window.openDemo();
    window.moveGridLine('x', 3, 10.0);
    EXPECT_EQ(window.undoDepth(), 1u);
    window.openDemo();
    EXPECT_EQ(window.undoDepth(), 0u);
    EXPECT_TRUE(window.getRecipe().moved.empty());
}

namespace {

    // the job system the window's background runs need, for this test only when none is up
    struct JobsUp {
        bool started = false;
        JobsUp() {
            if (!jasperengine::jobs::hasSystem()) {
                jasperengine::jobs::initialize({});
                started = true;
            }
        }
        ~JobsUp() {
            if (started) jasperengine::jobs::shutdown();
        }
    };

}

TEST(CleanupWindow, ARunPredictedWithinTheBudgetIsInline) {
    PixelArtCleanupWindow window(nullptr);
    window.openDemo();
    window.runNow();
    const void* first = Access::shown(window);
    ASSERT_NE(first, nullptr);
    Access::predict(window, 5.0, 5.0);
    window.applyEdit([](cu::Recipe& r) { r.crop = true; });
    Access::tick(window);
    EXPECT_FALSE(window.isRunning());
    EXPECT_NE(Access::shown(window), first);
    EXPECT_FALSE(window.isStale());
}

TEST(CleanupWindow, ARunPredictedOverTheBudgetIsAJobAndThePreviousResultStaysUntilItFinishes) {
    JobsUp jobs;
    PixelArtCleanupWindow window(nullptr);
    window.openDemo();
    window.runNow();
    const void* previous = Access::shown(window);
    ASSERT_NE(previous, nullptr);
    Access::predict(window, 500.0, 500.0);
    window.applyEdit([](cu::Recipe& r) { r.crop = true; });
    Access::tick(window);
    ASSERT_TRUE(window.isRunning());
    EXPECT_EQ(Access::shown(window), previous);
    EXPECT_TRUE(window.isStale());
    Access::waitForJob(window);
    // finished, not adopted yet: the frame loop adopts it
    EXPECT_EQ(Access::shown(window), previous);
    Access::tick(window);
    EXPECT_FALSE(window.isRunning());
    EXPECT_NE(Access::shown(window), previous);
    EXPECT_FALSE(window.isStale());
}

TEST(CleanupWindow, AGridEditIsPredictedFromTheLastFullRun) {
    JobsUp jobs;
    PixelArtCleanupWindow window(nullptr);
    window.openDemo();
    window.runNow();
    const void* previous = Access::shown(window);
    // the last run was fast, the last full run was not: only a non-grid change stays inline
    Access::predict(window, 5.0, 500.0);
    window.applyEdit([](cu::Recipe& r) { r.follow = cu::Follow::Straight; });
    Access::tick(window);
    ASSERT_TRUE(window.isRunning());
    EXPECT_EQ(Access::shown(window), previous);
    Access::waitForJob(window);
    Access::tick(window);
    EXPECT_NE(Access::shown(window), previous);
}

TEST(CleanupWindow, ASheetLintSpotLandsOnItsCellOfTheSourceFrame) {
    // a spot is on the clip's canvas (anchor, padding): the source cell is the frame's own
    const std::vector<cu::Rgba8> palette = cu::sweetie16();
    std::vector<cu::PixelArt> sprites;
    for (uint32_t seed = 3; seed < 7; seed++) sprites.push_back(cu::makeSprite(12, 12, palette, seed, true));
    cu::Prepared prepared;
    prepared.frames = {cu::fakeSheet(sprites, 6).image};
    prepared.spec.person = true;
    prepared.spec.format = cu::OutputFormat::Png;
    prepared.spec.sheet = cu::SheetArg::On;
    prepared.recipe.removeBackground = true;
    PixelArtCleanupWindow window(nullptr);
    Access::adopt(window, std::move(prepared), "(sheet)");
    window.runNow();
    ASSERT_TRUE(window.hasResult());
    ASSERT_TRUE(window.result()->animation.has_value());
    const cu::AnimationOutcome& animation = *window.result()->animation;
    const std::vector<const cu::ClipOutcome*> clips = cu::exportedClips(animation);
    ASSERT_FALSE(clips.empty());
    ASSERT_FALSE(clips.front()->placed.empty());
    const cu::ClipFrame& placed = clips.front()->placed.front();
    const cu::FixedFrame& fixed = animation.frames[static_cast<size_t>(placed.index)];
    const auto firstOpaque = [](const cu::PixelArt& art, int& x, int& y) {
        for (int i = 0; i < static_cast<int>(art.indices.size()); i++) {
            if (art.indices[static_cast<size_t>(i)] != cu::TRANSPARENT) {
                x = i % art.width;
                y = i / art.width;
                return true;
            }
        }
        return false;
    };
    int px = 0, py = 0, ax = 0, ay = 0;
    ASSERT_TRUE(firstOpaque(placed.art, px, py));
    ASSERT_TRUE(firstOpaque(fixed.art, ax, ay));
    ASSERT_TRUE(px != ax || py != ay) << "padding places the art away from (0, 0), so the offset matters";
    jasperengine::editor::pixelart::lint::LintSpot spot;
    spot.frame = 1;
    spot.x = px;
    spot.y = py;
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    ASSERT_TRUE(Access::spotRect(window, spot, x0, y0, x1, y1));
    EXPECT_DOUBLE_EQ(x0, fixed.box.x + fixed.outcome.grid.xs[static_cast<size_t>(ax)]);
    EXPECT_DOUBLE_EQ(y0, fixed.box.y + fixed.outcome.grid.ys[static_cast<size_t>(ay)]);
    EXPECT_DOUBLE_EQ(x1, fixed.box.x + fixed.outcome.grid.xs[static_cast<size_t>(ax) + 1]);
    // R42: the flash outlines the whole cell, not one source pixel of it
    Access::focusAndFlash(window, x0, y0, x1, y1);
    EXPECT_DOUBLE_EQ(Access::flashWidth(window), x1 - x0);
    EXPECT_GT(Access::flashWidth(window), 1.0);
}

TEST(CleanupWindow, ASheetLostCellInTheResultViewIsOnItsFrameInTheAtlas) {
    const std::vector<cu::Rgba8> palette = cu::sweetie16();
    std::vector<cu::PixelArt> sprites;
    for (uint32_t seed = 3; seed < 7; seed++) sprites.push_back(cu::makeSprite(12, 12, palette, seed, true));
    cu::Prepared prepared;
    prepared.frames = {cu::fakeSheet(sprites, 6).image};
    prepared.spec.person = true;
    prepared.spec.format = cu::OutputFormat::Png;
    prepared.spec.sheet = cu::SheetArg::On;
    prepared.recipe.removeBackground = true;
    PixelArtCleanupWindow window(nullptr);
    Access::adopt(window, std::move(prepared), "(sheet)");
    window.runNow();
    ASSERT_TRUE(window.hasResult());
    ASSERT_TRUE(window.result()->animation.has_value());
    window.setView(PixelArtCleanupWindow::View::Result);
    const cu::AnimationOutcome& animation = *window.result()->animation;
    const cu::ClipFrame& placed = cu::exportedClips(animation).front()->placed.front();
    const cu::FixedFrame& fixed = animation.frames[static_cast<size_t>(placed.index)];
    const auto firstOpaque = [](const cu::PixelArt& art, int& x, int& y) {
        for (int i = 0; i < static_cast<int>(art.indices.size()); i++) {
            if (art.indices[static_cast<size_t>(i)] != cu::TRANSPARENT) {
                x = i % art.width;
                y = i / art.width;
                return true;
            }
        }
        return false;
    };
    int px = 0, py = 0, ax = 0, ay = 0;
    ASSERT_TRUE(firstOpaque(placed.art, px, py));
    ASSERT_TRUE(firstOpaque(fixed.art, ax, ay));
    const cu::AtlasRect* rect = nullptr;
    for (const cu::AtlasRect& candidate : Access::atlas(window)) {
        if (candidate.clip == 0 && candidate.frame == 0) rect = &candidate;
    }
    ASSERT_NE(rect, nullptr);
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    ASSERT_TRUE(Access::cellInView(window, placed.index, ax, ay, x0, y0, x1, y1));
    EXPECT_DOUBLE_EQ(x0, rect->x + px);
    EXPECT_DOUBLE_EQ(y0, rect->y + py);
}

TEST(CleanupWindow, ClosingTheWindowDropsItsFocusAndItsRunCache) {
    PixelArtCleanupWindow window(nullptr);
    window.openDemo();
    window.runNow();
    Access::setFocused(window, true);
    ASSERT_TRUE(Access::cacheHolds(window));
    window.setOpen(false);
    // a closed window takes no Ctrl+Z (App's lastFocusedWindow) and holds no cache
    EXPECT_FALSE(window.isFocused());
    EXPECT_FALSE(Access::cacheHolds(window));
    // what it showed stays for when it opens again
    EXPECT_TRUE(window.hasResult());
}

TEST(CleanupWindow, ACancelledRunDoesNotChangeThePrediction) {
    PixelArtCleanupWindow window(nullptr);
    window.openDemo();
    Access::predict(window, 500.0, 500.0);
    Access::applyCancelled(window, 1.0);
    EXPECT_DOUBLE_EQ(Access::lastRunMs(window), 500.0);
    EXPECT_DOUBLE_EQ(Access::lastFullRunMs(window), 500.0);
}

TEST(CleanupWindow, AFailedReplaceLeavesNoOverwriteBehind) {
    // a plain file with its output beside it: Write asks to replace (the base host answers Yes), and
    // the write fails (no project to write into): the next Write must ask again
    jasperengine::editor::test::ScratchDir scratch("cleanup-replace");
    const std::filesystem::path source = scratch.path / "art.png";
    const std::string bytes = cu::demoImage().encodePng();
    std::ofstream(source, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    std::ofstream(scratch.path / "art_clean.png", std::ios::binary) << "existing";
    PixelArtCleanupWindow window(nullptr);
    std::string error;
    ASSERT_TRUE(window.openSource(source, error)) << error;
    window.runNow();
    ASSERT_TRUE(window.hasResult());
    window.write(error);
    ASSERT_NE(window.getPrepared(), nullptr);
    EXPECT_FALSE(window.getPrepared()->spec.overwrite);
}

TEST(CleanupWindow, TheEntryCacheReadsAPlainImageOnce) {
    jasperengine::editor::test::ScratchDir scratch("cleanup-entry");
    const std::filesystem::path image = scratch.path / "plain.png";
    const std::string bytes = jasperengine::editor::pixelart::PixelImage(4, 4).encodePng();
    std::ofstream(image, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    int reads = 0;
    const auto check = [&reads](const std::filesystem::path& projectDir, const std::filesystem::path& output) {
        reads++;
        return cu::checkStaleness(projectDir, output);
    };
    EXPECT_FALSE(cleanupentry::outputStatus(scratch.path, image, check).isOutput);
    EXPECT_FALSE(cleanupentry::outputStatus(scratch.path, image, check).isOutput);
    EXPECT_EQ(reads, 1);
}
