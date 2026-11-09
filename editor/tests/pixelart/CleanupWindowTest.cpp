// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Pixel Art Cleanup window's model (docs/specs/S46-pixel-art-cleanup.md R39, R42, R43) through its
// scripted API and PixelArtCleanupWindowTestAccess, with the demo source (nothing here needs a project,
// a GPU or an ImGui frame): the local undo stack, moved lines, Compare's image on the source's cells.

#include <gtest/gtest.h>

#include "pixelart/cleanup/CleanupRecipe.h"
#include "window/PixelArtCleanupWindow.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace cu = jasperengine::editor::pixelart::cleanup;

namespace jasperengine::editor {
    struct PixelArtCleanupWindowTestAccess {
        static const pixelart::PixelImage& compare(const PixelArtCleanupWindow& w) { return w.shown->compare; }
        static const pixelart::PixelImage& result(const PixelArtCleanupWindow& w) { return w.shown->image; }
        static void tick(PixelArtCleanupWindow& w) { w.tick(); }
        static bool dirty(const PixelArtCleanupWindow& w) { return w.dirty; }
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
