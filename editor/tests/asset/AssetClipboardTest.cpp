// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Project window's clipboard rules and paste targets (project-window spec 5 D1, D4)

#include "asset/AssetClipboard.h"

#include "TempDir.h"

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace jasperengine::editor;

TEST(AssetClipboard, PasteTargetOfAFileIsItsFolder) {
    const fs::path file = "/p/assets/sprites/hero.png";
    EXPECT_EQ(AssetClipboard::pasteTarget("/p/assets", &file, false), fs::path("/p/assets/sprites"));
}

TEST(AssetClipboard, PasteTargetOfAFolderIsTheFolder) {
    const fs::path folder = "/p/assets/sprites";
    EXPECT_EQ(AssetClipboard::pasteTarget("/p/assets", &folder, true), folder);
}

TEST(AssetClipboard, PasteTargetOfEmptySpaceIsTheListingFolder) {
    EXPECT_EQ(AssetClipboard::pasteTarget("/p/assets", nullptr, false), fs::path("/p/assets"));
    const fs::path none;
    EXPECT_EQ(AssetClipboard::pasteTarget("/p/start", &none, false), fs::path("/p/start"));
}

TEST(AssetClipboard, FailedPasteKeepsACut) {
    AssetClipboard clipboard;
    clipboard.set({"/p/a.png"}, true);
    clipboard.pasted(false);
    EXPECT_FALSE(clipboard.empty());
    EXPECT_TRUE(clipboard.isCut());
    EXPECT_TRUE(clipboard.isCutItem("/p/./a.png"));
}

TEST(AssetClipboard, SuccessfulPasteEmptiesACutButKeepsACopy) {
    AssetClipboard clipboard;
    clipboard.set({"/p/a.png"}, true);
    clipboard.pasted(true);
    EXPECT_TRUE(clipboard.empty());
    clipboard.set({"/p/a.png"}, false);
    clipboard.pasted(true);
    EXPECT_FALSE(clipboard.empty());
    EXPECT_FALSE(clipboard.isCutItem("/p/a.png")) << "a copy is not dimmed";
}

TEST(AssetClipboard, GoneItemsAreSkippedAndCounted) {
    jasperengine::test::TempDir dir;
    dir.write("a.png", "a");
    AssetClipboard clipboard;
    clipboard.set({dir.path() / "a.png", dir.path() / "gone.png"}, false);
    size_t gone = 0;
    const std::vector<fs::path> left = clipboard.existing(&gone);
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0], (dir.path() / "a.png").lexically_normal());
    EXPECT_EQ(gone, 1u);
}

// During a search the listing is no folder: empty space and Ctrl+V paste into the folder the search
// started from (ProjectWindow::listingFolder gives it), a result's menu into that result's folder
TEST(AssetClipboard, PasteTargetDuringASearchIsTheStartFolder) {
    const fs::path start = "/p/assets/levels";
    EXPECT_EQ(AssetClipboard::pasteTarget(start, nullptr, false), start);
    const fs::path result = "/p/assets/sprites/deep/hero.png";
    EXPECT_EQ(AssetClipboard::pasteTarget(start, &result, false), fs::path("/p/assets/sprites/deep"));
}
