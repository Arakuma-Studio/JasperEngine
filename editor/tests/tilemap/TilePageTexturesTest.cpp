// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The tile pages the Tile Palette, the Tileset Editor and the Scene tools draw from
// (docs/specs/S50-grid-tilemap.md R64, R65, R67): each tile asks for its page every frame, so a page
// whose image does not load is read again only when its file changes, not on every call.

#include "tilemap/TilePageTextures.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor::tilemap;

TEST(TilePageTextures, PageThatFailsIsReadAgainOnlyWhenItsFileChanges) {
    jasperengine::test::TempDir dir;
    // not an image: the decode fails before anything reaches the GPU
    const fs::path page = dir.write("broken.png", "not a png");
    TilesetRuntime tileset;
    tileset.generation = 1;
    TilePageTextures& pages = TilePageTextures::get();
    const uint64_t start = pages.decodes();

    EXPECT_FALSE(pages.page(page.generic_string(), tileset));
    for (int tile = 0; tile < 10; tile++) pages.page(page.generic_string(), tileset);
    pages.beginFrame();
    pages.page(page.generic_string(), tileset);
    EXPECT_EQ(pages.decodes() - start, 1u) << "the failure is remembered across calls and frames";

    // written again: read once more on the next frame
    fs::last_write_time(page, fs::last_write_time(page) + std::chrono::seconds(5));
    pages.beginFrame();
    EXPECT_FALSE(pages.page(page.generic_string(), tileset));
    pages.page(page.generic_string(), tileset);
    EXPECT_EQ(pages.decodes() - start, 2u) << "a changed file is read again, once";

    // a new tileset generation (a reload) reads it too
    tileset.generation = 2;
    pages.page(page.generic_string(), tileset);
    EXPECT_EQ(pages.decodes() - start, 3u);
    pages.clear();
}
