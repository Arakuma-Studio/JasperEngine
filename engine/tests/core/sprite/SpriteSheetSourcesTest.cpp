// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S45's sheet sources (docs/specs/S45-sprite-atlas.md AC1, R1, R2, R8, R10, R11, R39): texture
// sheets (grid with offset, spacing and skipped empty cells; multiple frames with pivots and
// borders), "name@WxH" files, records that S44's SpriteSheetIO round-trips canonically, the
// .spriteatlas and remapped-texture sources through ImportedAssets, and a legacy inline rect
// mapped into the page that packs its (trimmed) texture.

#include <gtest/gtest.h>

#include "asset/ImportedAssets.h"
#include "io/AtomicFile.h"
#include "sprite/SpriteSheetIO.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"

#include "ImageCompare.h"
#include "TestTempDir.h"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    Rect rectOf(const SpriteFrame& f) { return f.rect; }

    bool sameRect(Rect a, const Rect& b) { return !(a != b); }

    // the editor's provider in a test: a table of "<source>#<key>" -> file
    struct FakeProvider {
        std::map<std::string, std::string> files;
        FakeProvider() {
            ImportedAssets::setSheetProvider([this](const std::string& ref, std::string& out) {
                auto it = files.find(ref);
                if (it == files.end()) return false;
                out = it->second;
                return true;
            });
        }
        ~FakeProvider() { ImportedAssets::setSheetProvider(nullptr); }
    };

    std::string writeText(const std::filesystem::path& path, const std::string& text) {
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        EXPECT_TRUE(io::writeFileAtomic(path, text, options).ok());
        return path.string();
    }

}

TEST(SpriteSheetSources, GridWithOffsetSpacingAndSkippedEmptyCells) {
    TextureSheetSettings settings;
    settings.mode = TextureSpriteMode::GRID;
    settings.grid.cellWidth = 8;
    settings.grid.cellHeight = 8;
    settings.grid.offsetX = 2;
    settings.grid.offsetY = 1;
    settings.grid.spacingX = 1;
    settings.grid.spacingY = 1;
    settings.grid.prefix = "run_";
    settings.pivotPreset = PivotPreset::BOTTOM_CENTER;
    // 4 x 2 cells fit 40 x 20 with 3 pixels left on the right and 2 at the bottom; cell (1, 0) is empty
    std::vector<std::string> warnings;
    const SpriteSheetData sheet = SpriteSheetSources::buildTextureSheet("hero.png", 40, 20, settings, [](int x, int y, int, int) {
        return !(x == 2 + 9 && y == 1);
    }, &warnings);
    ASSERT_EQ(sheet.frames.size(), 7u);
    EXPECT_EQ(sheet.frames[0].name, "run_0");
    EXPECT_TRUE(sameRect(rectOf(sheet.frames[0]), Rect(2, 1, 8, 8)));
    // the empty cell is skipped and the names stay consecutive
    EXPECT_EQ(sheet.frames[1].name, "run_1");
    EXPECT_TRUE(sameRect(rectOf(sheet.frames[1]), Rect(20, 1, 8, 8)));
    EXPECT_TRUE(sameRect(rectOf(sheet.frames[3]), Rect(2, 10, 8, 8)));
    // on the texture itself, untrimmed, pivot from the preset (pixels from the frame's top-left)
    EXPECT_EQ(sheet.pages.size(), 1u);
    EXPECT_EQ(sheet.pages[0].texture, "hero.png");
    EXPECT_EQ(sheet.frames[0].offset, Vector2(0, 0));
    EXPECT_EQ(sheet.frames[0].sourceSize, Vector2(8, 8));
    EXPECT_EQ(sheet.frames[0].pivot, Vector2(4, 8));
    EXPECT_EQ(sheet.frames[0].key, "rect:2,1,8,8");
    ASSERT_FALSE(warnings.empty());
    EXPECT_NE(warnings[0].find("3 pixel(s) on the right and 2 at the bottom"), std::string::npos) << warnings[0];

    // count stops after that many frames
    settings.grid.count = 3;
    EXPECT_EQ(SpriteSheetSources::buildTextureSheet("hero.png", 40, 20, settings, nullptr).frames.size(), 3u);
}

TEST(SpriteSheetSources, MultipleFramesWithPivotsAndBorders) {
    TextureSheetSettings settings;
    settings.mode = TextureSpriteMode::MULTIPLE;
    settings.customPivot = true;
    settings.pivotNormalized = Vector2(0.5f, 0.0f);   // bottom centre
    TextureSheetFrame panel;
    panel.name = "panel";
    panel.rect = Rect(0, 0, 24, 16);
    panel.border = Vector4(4, 3, 4, 3);
    TextureSheetFrame idle;
    idle.name = "idle";
    idle.rect = Rect(24, 0, 8, 16);
    idle.hasPivot = true;
    idle.pivot = Vector2(3, 15);
    TextureSheetFrame outside = idle;
    outside.name = "outside";
    outside.rect = Rect(30, 0, 8, 16);
    TextureSheetFrame duplicate = idle;
    duplicate.rect = Rect(24, 0, 4, 4);
    settings.frames = {panel, idle, outside, duplicate};
    std::vector<std::string> warnings;
    const SpriteSheetData sheet = SpriteSheetSources::buildTextureSheet("ui.png", 32, 16, settings, nullptr, &warnings);
    ASSERT_EQ(sheet.frames.size(), 3u);
    EXPECT_EQ(sheet.frames[0].pivot, Vector2(12, 16));
    EXPECT_EQ(sheet.frames[0].border, Vector4(4, 3, 4, 3));
    EXPECT_EQ(sheet.frames[1].pivot, Vector2(3, 15));
    // a duplicate name is renamed, a frame outside the image left out
    EXPECT_EQ(sheet.frames[2].name, "idle 2");
    ASSERT_EQ(warnings.size(), 2u);
    EXPECT_NE(warnings[0].find("outside"), std::string::npos);

    // single: the whole image, named by the stem, pivot at its centre (the pool's built-in frame)
    const SpriteSheetData single = SpriteSheetSources::buildTextureSheet("items/sword.png", 15, 9, TextureSheetSettings(), nullptr);
    ASSERT_EQ(single.frames.size(), 1u);
    EXPECT_EQ(single.frames[0].name, "sword");
    EXPECT_EQ(single.frames[0].pivot, Vector2(7, 4));
}

TEST(SpriteSheetSources, CellSizeFromTheFileName) {
    int w = 0, h = 0;
    std::string base;
    ASSERT_TRUE(SpriteSheetSources::parseCellSize("items/coin@16x16.png", w, h, &base));
    EXPECT_EQ(w, 16);
    EXPECT_EQ(h, 16);
    EXPECT_EQ(base, "coin");
    EXPECT_TRUE(SpriteSheetSources::parseCellSize("buff@22X20.PNG", w, h));
    EXPECT_EQ(h, 20);
    for (const char* bad : {"coin.png", "coin@16.png", "coin@0x16.png", "@16x16.png", "coin@16x16x.png", "coin@ax16.png"}) {
        EXPECT_FALSE(SpriteSheetSources::parseCellSize(bad, w, h)) << bad;
    }
    const TextureSheetSettings grid = SpriteSheetSources::cellSizeSettings(16, 16, "coin");
    EXPECT_EQ(grid.mode, TextureSpriteMode::GRID);
    EXPECT_TRUE(grid.grid.skipEmpty);
    EXPECT_EQ(grid.grid.prefix, "coin_");
}

TEST(SpriteSheetSources, RecordsRoundTripCanonically) {
    TextureSheetSettings settings;
    settings.mode = TextureSpriteMode::GRID;
    settings.grid.cellWidth = 5;
    settings.grid.cellHeight = 7;
    for (const SpriteSheetData& sheet : {SpriteSheetSources::buildTextureSheet("a@5x7.png", 20, 14, settings, nullptr),
                                         SpriteSheetSources::buildTextureSheet("b.png", 9, 9, TextureSheetSettings(), nullptr)}) {
        const std::string text = writeSheet(sheet);
        SpriteSheetParseResult parsed = parseSheet(text);
        ASSERT_TRUE(parsed.ok()) << parsed.error;
        EXPECT_TRUE(parsed.warnings.empty());
        EXPECT_EQ(writeSheet(*parsed.sheet), text);
        EXPECT_TRUE(parsed.sheet->tags.empty());
        EXPECT_TRUE(parsed.sheet->slices.empty());
        Rect place;
        EXPECT_TRUE(SpriteSheetSources::frameTextureRect(parsed.sheet->frames.back(), place));
    }
}

// a name@WxH texture loaded without an editor: its own pixels decide the empty cells
TEST(SpriteSheetSources, CellSizeTextureLoadsAsAGrid) {
    test::TestTempDir dir("sheet-sources-cells");
    const std::filesystem::path png = dir.path() / "coin@4x4.png";
    std::vector<uint8_t> rgba(12 * 4 * 4, 0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 12; x++) {
            if (x >= 4 && x < 8) continue;   // the middle cell is transparent
            uint8_t* p = &rgba[(static_cast<size_t>(y) * 12 + x) * 4];
            p[0] = 200; p[3] = 255;
        }
    }
    ASSERT_TRUE(test::writePng(png.string(), rgba, 12, 4));
    std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::get(png.string());
    ASSERT_TRUE(sheet);
    ASSERT_EQ(sheet->frames.size(), 2u);
    EXPECT_EQ(sheet->frames[0].name, "coin_0");
    EXPECT_EQ(sheet->frames[1].name, "coin_1");
    EXPECT_TRUE(sameRect(rectOf(sheet->frames[1]), Rect(8, 0, 4, 4)));
    SpriteSheetPool::invalidate(png.string());
}

// .spriteatlas sheets and a packed source's remap come through ImportedAssets ("#sheet",
// "#image#<n>"); a texture with neither stays the pool's single frame
TEST(SpriteSheetSources, AtlasAndRemapsThroughImportedAssets) {
    test::TestTempDir dir("sheet-sources-atlas");
    FakeProvider provider;
    SpriteSheetData atlas;
    atlas.source = "ui/icons.spriteatlas";
    atlas.pages.push_back({"je-imported/atlas/x/desktop/page0-a.png", 64, 32});
    SpriteFrame sword;
    sword.name = "sword";
    sword.rect = Rect(1, 1, 10, 12);
    sword.sourceSize = Vector2(16, 16);
    sword.offset = Vector2(3, 2);
    sword.pivot = Vector2(8, 8);
    atlas.frames.push_back(sword);
    provider.files["ui/icons.spriteatlas#sheet"] = writeText(dir.path() / "sheet.json", writeSheet(atlas));
    provider.files["ui/icons.spriteatlas#image#0"] = (dir.path() / "page0-a.png").string();
    SpriteSheetData remap = atlas;
    remap.source = "items/sword.png";
    provider.files["items/sword.png#sheet"] = writeText(dir.path() / "member.json", writeSheet(remap));
    provider.files["items/sword.png#image#0"] = (dir.path() / "page0-a.png").string();
    SpriteSheetPool::invalidate("ui/icons.spriteatlas");
    SpriteSheetPool::invalidate("items/sword.png");

    std::shared_ptr<const SpriteSheetData> fromAtlas = SpriteSheetPool::get("ui/icons.spriteatlas");
    ASSERT_TRUE(fromAtlas);
    ASSERT_EQ(fromAtlas->frameCount(), 1);
    EXPECT_EQ(fromAtlas->findFrame("sword"), 0);
    EXPECT_EQ(fromAtlas->pages[0].texture, (dir.path() / "page0-a.png").string());
    std::shared_ptr<const SpriteSheetData> fromSource = SpriteSheetPool::get("items/sword.png");
    ASSERT_TRUE(fromSource);
    EXPECT_EQ(fromSource->source, "items/sword.png");
    EXPECT_EQ(fromSource->frames[0].offset, Vector2(3, 2));
    EXPECT_EQ(fromSource->pages[0].texture, (dir.path() / "page0-a.png").string());
    SpriteSheetPool::invalidate("ui/icons.spriteatlas");
    SpriteSheetPool::invalidate("items/sword.png");
}

// R8 / AC9's mapping: an inline rect of a texture packed trimmed, translated into the page and
// clipped to the trim (the clipped part an offset); a rect of only trimmed pixels draws nothing
TEST(SpriteSheetSources, InlineRectMapsIntoTheTrimmedPage) {
    test::TestTempDir dir("sheet-sources-inline");
    FakeProvider provider;
    SpriteSheetData remap;
    remap.source = "legacy/sheet.png";
    remap.pages.push_back({"page", 64, 64});
    SpriteFrame whole;
    whole.name = "sheet";
    whole.key = "rect:0,0,32,16";
    whole.rect = Rect(10, 20, 20, 12);   // the texture's opaque box [4, 24) x [2, 14), packed at (10, 20)
    whole.sourceSize = Vector2(32, 16);
    whole.offset = Vector2(4, 2);
    remap.frames.push_back(whole);
    provider.files["legacy/sheet.png#sheet"] = writeText(dir.path() / "member.json", writeSheet(remap));
    provider.files["legacy/sheet.png#atlas"] = provider.files["legacy/sheet.png#sheet"];
    SpriteSheetPool::invalidate("legacy/sheet.png");

    SpriteFrame out;
    int w = 0, h = 0;
    ASSERT_TRUE(SpriteSheetSources::mapInlineRect("legacy/sheet.png", Rect(0, 0, 16, 16), out, &w, &h));
    EXPECT_EQ(w, 32);
    EXPECT_EQ(h, 16);
    EXPECT_TRUE(sameRect(out.rect, Rect(10, 20, 12, 12)));
    EXPECT_EQ(out.offset, Vector2(4, 2));
    EXPECT_EQ(out.sourceSize, Vector2(16, 16));
    EXPECT_EQ(out.pivot, Vector2(8, 8));
    // across the trim on the right: translated, no offset
    ASSERT_TRUE(SpriteSheetSources::mapInlineRect("legacy/sheet.png", Rect(16, 4, 16, 8), out));
    EXPECT_TRUE(sameRect(out.rect, Rect(22, 22, 8, 8)));
    EXPECT_EQ(out.offset, Vector2(0, 0));
    // only trimmed (transparent) pixels
    ASSERT_TRUE(SpriteSheetSources::mapInlineRect("legacy/sheet.png", Rect(26, 0, 6, 16), out));
    EXPECT_EQ(out.rect.getWidth(), 0.0f);
    // past the texture, and a texture no atlas packs
    EXPECT_FALSE(SpriteSheetSources::mapInlineRect("legacy/sheet.png", Rect(30, 0, 8, 8), out));
    EXPECT_FALSE(SpriteSheetSources::mapInlineRect("legacy/other.png", Rect(0, 0, 8, 8), out));
    EXPECT_TRUE(SpriteSheetSources::isPacked("legacy/sheet.png"));
    EXPECT_FALSE(SpriteSheetSources::isPacked("legacy/other.png"));
    SpriteSheetPool::invalidate("legacy/sheet.png");
}
