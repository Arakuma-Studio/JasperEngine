// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Atlas settings reach S44's packer (docs/specs/S45-sprite-atlas.md AC4, R14, R15, R27; the packer
// itself is S44's AC22): a platform's max size, Keep Members Together across members, block
// rounding with a block format, byte-identical pages from the same atlas and from a shuffled
// member list, frame names unique in the atlas, the .spriteatlas text round trip, and a 500-frame
// atlas from warm member caches within 400 ms (SpriteAtlasBench).

#include "SpriteAtlasTestSupport.h"
#include "support/EditorTestSupport.h"

#include "sprite/SpriteSheetIO.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor::sprite;
using namespace jasperengine::editor::sprite::test;
using jasperengine::editor::pixelart::IntRect;
using jasperengine::editor::pixelart::PixelImage;

namespace {

    std::vector<AtlasMemberInput> someMembers(const ScratchDir& dir, int count, int size) {
        std::vector<AtlasMemberInput> members;
        for (int i = 0; i < count; i++) {
            members.push_back(textureMember(dir, "icons/icon" + std::to_string(i) + ".png",
                                            memberImage(size, size, IntRect{1 + i % 3, 2, size - 3 - i % 3, size - 4}, i)));
        }
        return members;
    }

}

TEST(SpriteAtlasLayout, PlatformMaxSizeOverride) {
    editor::test::ensureEngine();
    ScratchDir dir("platform");
    std::vector<AtlasMemberInput> members = someMembers(dir, 12, 30);
    AtlasBuildInput desktop = atlasInput(dir, members, "desktop", false);
    desktop.doc.settings.platforms["web"] = {true, 64};
    AtlasBuildInput web = desktop;
    web.platform = "web";
    std::shared_ptr<AtlasBuildResult> d = SpriteAtlasBuilder::get().build(desktop);
    std::shared_ptr<AtlasBuildResult> w = SpriteAtlasBuilder::get().build(web);
    ASSERT_TRUE(d->ok) << d->error.message;
    ASSERT_TRUE(w->ok) << w->error.message;
    EXPECT_EQ(d->pages.size(), 1u);
    EXPECT_GT(w->pages.size(), 1u) << "the web override (64) splits what one desktop page holds";
    for (const AtlasPageRow& p : w->pages) {
        EXPECT_LE(p.width, 64);
        EXPECT_LE(p.height, 64);
    }
    EXPECT_EQ(desktop.doc.settings.maxSizeFor("web"), 64);
    EXPECT_EQ(desktop.doc.settings.maxSizeFor("mobile"), 2048);
}

TEST(SpriteAtlasLayout, KeepMembersTogetherAcrossMembers) {
    editor::test::ensureEngine();
    ScratchDir dir("together");
    // three grid members of 6 frames each, a page holding about 1.5 members
    std::vector<AtlasMemberInput> members;
    for (int m = 0; m < 3; m++) {
        const std::string path = "anim/walk" + std::to_string(m) + ".png";
        PixelImage sheet(6 * 16, 16);
        for (int f = 0; f < 6; f++) sheet.blit(memberImage(16, 16, IntRect{1, 1, 14, 14}, m * 10 + f), f * 16, 0);
        members.push_back(textureMember(dir, path, sheet));
        TextureSheetSettings grid;
        grid.mode = TextureSpriteMode::GRID;
        grid.grid.cellWidth = 16;
        grid.grid.cellHeight = 16;
        writeSpriteMode(members.back().absolute, grid);
    }
    AtlasBuildInput input = atlasInput(dir, members, "desktop", false);
    input.doc.settings.maxSize = 64;
    input.doc.settings.padding = 0;
    input.doc.settings.extrude = 0;
    input.doc.settings.trim = false;
    std::shared_ptr<AtlasBuildResult> r = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(r->ok) << r->error.message;
    ASSERT_EQ(r->frames.size(), 18u);
    ASSERT_GT(r->pages.size(), 1u);
    std::map<std::string, std::set<int>> pagesOf;
    for (const AtlasFrameRow& f : r->frames) pagesOf[f.member].insert(f.page);
    for (const auto& [member, pages] : pagesOf) EXPECT_EQ(pages.size(), 1u) << member << " is split across pages";

    input.doc.settings.keepMembersTogether = false;
    std::shared_ptr<AtlasBuildResult> loose = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(loose->ok);
    size_t split = 0;
    pagesOf.clear();
    for (const AtlasFrameRow& f : loose->frames) pagesOf[f.member].insert(f.page);
    for (const auto& [member, pages] : pagesOf) split += pages.size() > 1 ? 1 : 0;
    EXPECT_LE(loose->pages.size(), r->pages.size());
    (void)split;
}

TEST(SpriteAtlasLayout, BlockFormatsRoundToFour) {
    editor::test::ensureEngine();
    ScratchDir dir("blocks");
    AtlasBuildInput input = atlasInput(dir, someMembers(dir, 7, 13), "desktop", false);
    input.forceBlockAlign = true;
    input.doc.settings.extrude = 1;
    input.doc.settings.padding = 1;
    std::shared_ptr<AtlasBuildResult> r = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(r->ok) << r->error.message;
    for (const AtlasPageRow& p : r->pages) {
        EXPECT_EQ(p.width % 4, 0);
        EXPECT_EQ(p.height % 4, 0);
    }
    // each frame's cell (its pixels and their extrusion) starts on the 4-pixel grid
    for (const AtlasFrameRow& f : r->frames) {
        EXPECT_EQ((f.rect.x - 1) % 4, 0) << f.name;
        EXPECT_EQ((f.rect.y - 1) % 4, 0) << f.name;
    }
}

TEST(SpriteAtlasLayout, DeterministicPagesAndShuffledMembers) {
    editor::test::ensureEngine();
    ScratchDir dir("determinism");
    std::vector<AtlasMemberInput> members = someMembers(dir, 20, 24);
    std::shared_ptr<AtlasBuildResult> a = SpriteAtlasBuilder::get().build(atlasInput(dir, members, "desktop", false));
    std::shared_ptr<AtlasBuildResult> b = SpriteAtlasBuilder::get().build(atlasInput(dir, members, "desktop", false));
    std::vector<AtlasMemberInput> shuffled(members.rbegin(), members.rend());
    std::swap(shuffled[3], shuffled[11]);
    std::shared_ptr<AtlasBuildResult> c = SpriteAtlasBuilder::get().build(atlasInput(dir, shuffled, "desktop", false));
    ASSERT_TRUE(a->ok && b->ok && c->ok);
    ASSERT_EQ(a->pageImages.size(), c->pageImages.size());
    for (size_t p = 0; p < a->pageImages.size(); p++) {
        EXPECT_TRUE(a->pageImages[p] == b->pageImages[p]) << "page " << p << " differs between two builds";
        EXPECT_TRUE(a->pageImages[p] == c->pageImages[p]) << "page " << p << " differs with the members shuffled";
        EXPECT_EQ(a->pages[p].hash, c->pages[p].hash);
    }
    EXPECT_EQ(writeSheet(a->sheet), writeSheet(c->sheet));
}

TEST(SpriteAtlasLayout, FrameNamesAreUniqueInTheAtlas) {
    editor::test::ensureEngine();
    ScratchDir dir("names");
    // two grid textures whose frames share names ("cell_0"...), a single texture named by its stem
    std::vector<AtlasMemberInput> members;
    for (const char* path : {"a/cells.png", "b/cells.png"}) {
        PixelImage image(16, 8);
        image.blit(memberImage(8, 8, IntRect{0, 0, 8, 8}, path[0]), 0, 0);
        image.blit(memberImage(8, 8, IntRect{0, 0, 8, 8}, path[0] + 1), 8, 0);
        members.push_back(textureMember(dir, path, image));
        TextureSheetSettings grid = SpriteSheetSources::cellSizeSettings(8, 8, "cell");
        writeSpriteMode(members.back().absolute, grid);
    }
    members.push_back(textureMember(dir, "items/sword.png", memberImage(10, 10, IntRect{2, 2, 6, 6}, 3)));
    std::shared_ptr<AtlasBuildResult> r = SpriteAtlasBuilder::get().build(atlasInput(dir, members, "desktop", false));
    ASSERT_TRUE(r->ok) << r->error.message;
    std::set<std::string> names;
    for (const SpriteFrame& f : r->sheet.frames) EXPECT_TRUE(names.insert(f.name).second) << f.name;
    EXPECT_TRUE(names.count("a/cells/cell_0"));
    EXPECT_TRUE(names.count("b/cells/cell_1"));
    EXPECT_TRUE(names.count("sword"));
}

TEST(SpriteAtlasLayout, AtlasFileRoundTripsAndRefusesNewerVersions) {
    SpriteAtlasDocument doc;
    doc.guid = Guid::parse("7c0e5d1f3a2b4c6d8e9f0a1b2c3d4e5f");
    SpriteAtlasMember file;
    file.path = "sprites/hero.aseprite";
    file.guid = Guid::parse("4b1e9c0d2a7f4e31b6c58d0f9e2a7c13");
    SpriteAtlasMember folder;
    folder.folder = "ui/icons";
    folder.types = {"texture", "aseprite_sprite"};
    doc.members = {file, folder};
    doc.exclude = {"ui/icons/old"};
    doc.settings.platforms["web"] = {true, 1024};
    doc.unknown["future_key"] = 3;
    const std::string text = writeSpriteAtlas(doc);
    SpriteAtlasParseResult parsed = parseSpriteAtlas(text);
    ASSERT_TRUE(parsed.ok()) << parsed.error.message;
    EXPECT_TRUE(parsed.problems.empty());
    EXPECT_EQ(writeSpriteAtlas(*parsed.document), text);
    EXPECT_EQ(parsed.document->settings.maxSizeFor("web"), 1024);
    EXPECT_NE(text.find("\"future_key\": 3"), std::string::npos) << "unknown keys are kept";

    std::string newer = text;
    newer.replace(newer.find("\"version\": 1"), 12, "\"version\": 2");
    SpriteAtlasParseResult refused = parseSpriteAtlas(newer);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.error.kind, "atlas_version");

    SpriteAtlasSettings settings;
    std::vector<editor::pixelart::PixelProblem> problems;
    EXPECT_FALSE(applyAtlasSettings(nlohmann::json{{"padding", 40}}, "", settings, problems));
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0].kind, "invalid_settings");
    EXPECT_EQ(problems[0].field, "padding");
    EXPECT_TRUE(applyAtlasSettings(nlohmann::json{{"max_size", 512}}, "mobile", settings, problems));
    EXPECT_EQ(settings.maxSizeFor("mobile"), 512);
}

// a frame larger than the max size is frame_too_large, naming the frame
TEST(SpriteAtlasLayout, OversizeFrameIsAnError) {
    editor::test::ensureEngine();
    ScratchDir dir("oversize");
    AtlasBuildInput input = atlasInput(dir, {textureMember(dir, "big.png", memberImage(80, 40, IntRect{0, 0, 80, 40}, 1))}, "desktop", false);
    input.doc.settings.maxSize = 64;
    std::shared_ptr<AtlasBuildResult> r = SpriteAtlasBuilder::get().build(input);
    EXPECT_FALSE(r->ok);
    EXPECT_EQ(r->error.kind, "frame_too_large");
    EXPECT_NE(r->error.message.find("big"), std::string::npos) << r->error.message;
}

// R27: a full build of 500 frames from warm member caches within 400 ms
TEST(SpriteAtlasLayout, SpriteAtlasBench) {
    editor::test::ensureEngine();
    ScratchDir dir("bench");
    std::vector<AtlasMemberInput> members;
    std::mt19937 rng(45);
    for (int m = 0; m < 50; m++) {
        PixelImage sheet(10 * 24, 24);
        for (int f = 0; f < 10; f++) {
            const int w = 8 + static_cast<int>(rng() % 16), h = 8 + static_cast<int>(rng() % 16);
            sheet.blit(memberImage(24, 24, IntRect{(24 - w) / 2, (24 - h) / 2, w, h}, m * 10 + f), f * 24, 0);
        }
        members.push_back(textureMember(dir, "corpus/m" + std::to_string(m) + ".png", sheet));
        TextureSheetSettings grid;
        grid.mode = TextureSpriteMode::GRID;
        grid.grid.cellWidth = 24;
        grid.grid.cellHeight = 24;
        grid.grid.skipEmpty = false;
        writeSpriteMode(members.back().absolute, grid);
    }
    AtlasBuildInput input = atlasInput(dir, members, "desktop", true);
    std::shared_ptr<AtlasBuildResult> cold = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(cold->ok) << cold->error.message;
    ASSERT_EQ(cold->frames.size(), 500u);
    // warm member caches, a new build (another output folder: no build.json hit)
    input.outDir = dir.root / "warm";
    const auto start = std::chrono::steady_clock::now();
    std::shared_ptr<AtlasBuildResult> warm = SpriteAtlasBuilder::get().build(input);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(warm->ok);
    EXPECT_FALSE(warm->cacheHit);
    std::printf("[bench] SpriteAtlasBench: 500 frames from warm member caches %.1f ms (budget 400 ms), %d %% used\n", ms,
                static_cast<int>(warm->usage * 100.0));
    EXPECT_GE(warm->usage, 0.5);
    if (editor::test::perfSlack() > 0.0) {
        EXPECT_LT(ms, 400.0 * editor::test::perfSlack());
    }
    // the same inputs again: build.json is adopted as it is
    std::shared_ptr<AtlasBuildResult> again = SpriteAtlasBuilder::get().build(input);
    EXPECT_TRUE(again->cacheHit);
    EXPECT_EQ(again->frames.size(), 500u);
}
