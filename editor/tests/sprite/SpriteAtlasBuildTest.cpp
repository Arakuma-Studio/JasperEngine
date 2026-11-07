// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lossless pages and the inline remap (docs/specs/S45-sprite-atlas.md AC5, AC9, R8, R16, R17):
// every page texel inside a frame equals its source texel (RGBA, the colour of transparent
// texels included) in the page files as built (lossless pages ship as they are, no cook), block
// formats align rects to 4, remaps keep the member's names, order, sizes and pivots, an unchanged
// page keeps its file, and a legacy sprite's inline rects (crossing the trim too) map onto the
// same pixels of the page.

#include "SpriteAtlasTestSupport.h"
#include "support/EditorTestSupport.h"

#include "asset/ImportedAssets.h"
#include "sprite/SpriteSheetIO.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"

#include <gtest/gtest.h>

#include <fstream>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor::sprite;
using namespace jasperengine::editor::sprite::test;
using jasperengine::editor::pixelart::IntRect;
using jasperengine::editor::pixelart::PixelImage;

namespace {

    std::string readFile(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // every page texel of every frame equals the member's source texel
    void expectLossless(const AtlasBuildResult& r, const std::map<std::string, PixelImage>& sources, size_t& compared) {
        std::vector<PixelImage> pages;
        for (size_t p = 0; p < r.pages.size(); p++) {
            std::optional<PixelImage> page = PixelImage::loadPng(r.pageFile(p), nullptr);
            ASSERT_TRUE(page) << r.pageFile(p);
            pages.push_back(*page);
        }
        for (const AtlasFrameRow& f : r.frames) {
            const PixelImage& source = sources.at(f.member);
            for (int y = 0; y < f.rect.h; y++) {
                for (int x = 0; x < f.rect.w; x++) {
                    ASSERT_EQ(pages[static_cast<size_t>(f.page)].at(f.rect.x + x, f.rect.y + y), source.at(f.offset.x + x, f.offset.y + y))
                        << f.name << " at " << x << "," << y;
                    compared++;
                }
            }
        }
    }

}

TEST(SpriteAtlasBuild, PagesAreLossless) {
    editor::test::ensureEngine();
    ScratchDir dir("lossless");
    std::map<std::string, PixelImage> sources;
    std::vector<AtlasMemberInput> members;
    for (int i = 0; i < 6; i++) {
        const std::string path = "items/item" + std::to_string(i) + ".png";
        sources[path] = memberImage(20 + i, 18, IntRect{2 + i % 2, 3, 12, 11}, i);
        members.push_back(textureMember(dir, path, sources[path]));
    }
    AtlasBuildInput input = atlasInput(dir, members);
    std::shared_ptr<AtlasBuildResult> trimmed = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(trimmed->ok) << trimmed->error.message;
    EXPECT_EQ(trimmed->compression, "lossless");
    size_t compared = 0;
    expectLossless(*trimmed, sources, compared);
    EXPECT_GT(compared, 6u * 12u * 11u - 1u);

    // untrimmed: the transparent texels' colour reaches the page too
    input.doc.settings.trim = false;
    input.outDir = dir.root / "untrimmed";
    std::shared_ptr<AtlasBuildResult> whole = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(whole->ok);
    compared = 0;
    expectLossless(*whole, sources, compared);
    size_t transparent = 0;
    for (const auto& [path, image] : sources) {
        for (int y = 0; y < image.height(); y++) {
            for (int x = 0; x < image.width(); x++) transparent += image.at(x, y)[3] == 0 && image.at(x, y)[0] != 0 ? 1 : 0;
        }
    }
    EXPECT_GT(transparent, 0u) << "the sources have coloured transparent texels";

    // a forced block format: the rects (inside their extrusion) on the 4-pixel grid
    input.forceBlockAlign = true;
    input.outDir = dir.root / "blocks";
    std::shared_ptr<AtlasBuildResult> blocks = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(blocks->ok);
    for (const AtlasFrameRow& f : blocks->frames) {
        EXPECT_EQ((f.rect.x - input.doc.settings.extrude) % 4, 0);
        EXPECT_EQ((f.rect.y - input.doc.settings.extrude) % 4, 0);
    }
    compared = 0;
    expectLossless(*blocks, sources, compared);
}

// R2: a member's remap is its own sheet with the atlas's pages: same names, order, sizes, pivots
TEST(SpriteAtlasBuild, RemapsKeepTheMembersFrames) {
    editor::test::ensureEngine();
    ScratchDir dir("remaps");
    PixelImage strip(3 * 12, 12);
    for (int f = 0; f < 3; f++) strip.blit(memberImage(12, 12, IntRect{f, 1, 8, 9}, f), f * 12, 0);
    AtlasMemberInput member = textureMember(dir, "anim/run@12x12.png", strip);
    AtlasBuildInput input = atlasInput(dir, {member, textureMember(dir, "items/gem.png", memberImage(9, 9, IntRect{1, 1, 7, 7}, 9))});
    std::shared_ptr<AtlasBuildResult> r = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(r->ok) << r->error.message;
    const TextureSheetInfo info = textureSheetSettings(member.absolute);
    const TextureSheetBuild own = buildTextureSheet(member.absolute, member.path, info.settings);
    SpriteSheetParseResult remap = parseSheet(readFile(r->memberFile(member.path)));
    ASSERT_TRUE(remap.ok()) << remap.error;
    ASSERT_EQ(remap.sheet->frames.size(), own.sheet.frames.size());
    for (size_t i = 0; i < own.sheet.frames.size(); i++) {
        EXPECT_EQ(remap.sheet->frames[i].name, own.sheet.frames[i].name);
        EXPECT_EQ(remap.sheet->frames[i].sourceSize, own.sheet.frames[i].sourceSize);
        EXPECT_EQ(remap.sheet->frames[i].pivot, own.sheet.frames[i].pivot);
        EXPECT_EQ(remap.sheet->frames[i].key, own.sheet.frames[i].key);
    }
    EXPECT_EQ(remap.sheet->source, member.path);
    EXPECT_EQ(remap.sheet->pages.size(), r->pages.size());
    // the atlas's own sheet: every frame, named in the atlas
    EXPECT_EQ(r->sheet.frameCount(), 4);
    EXPECT_GE(r->sheet.findFrame("run_2"), 0);
    EXPECT_GE(r->sheet.findFrame("gem"), 0);
}

// R17: a member's new pixels rewrite only the page they are on; the others keep their files
TEST(SpriteAtlasBuild, OnlyChangedPagesAreRewritten) {
    editor::test::ensureEngine();
    ScratchDir dir("incremental");
    std::vector<AtlasMemberInput> members;
    for (int i = 0; i < 2; i++) members.push_back(textureMember(dir, "big/m" + std::to_string(i) + ".png", memberImage(60, 60, IntRect{0, 0, 60, 60}, i)));
    AtlasBuildInput input = atlasInput(dir, members);
    input.doc.settings.maxSize = 64;
    std::shared_ptr<AtlasBuildResult> first = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(first->ok) << first->error.message;
    ASSERT_EQ(first->pages.size(), 2u);
    // m1's pixels change, not its size
    input.members[1] = textureMember(dir, "big/m1.png", memberImage(60, 60, IntRect{0, 0, 60, 60}, 7));
    std::shared_ptr<AtlasBuildResult> second = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(second->ok);
    int changed = 0;
    for (size_t p = 0; p < 2; p++) {
        if (second->pages[p].hash != first->pages[p].hash) {
            changed++;
            EXPECT_TRUE(second->pages[p].written);
        } else {
            EXPECT_FALSE(second->pages[p].written) << "an unchanged page is not written again";
        }
    }
    EXPECT_EQ(changed, 1);
}

// AC9: a legacy sprite whose texture an atlas packs trimmed shows the same pixels for each of its
// inline rects, rects across the trim included
TEST(SpriteAtlasBuild, InlineRectsRemap) {
    editor::test::ensureEngine();
    ScratchDir dir("inline");
    // a 32 x 16 legacy sheet whose opaque box is [5, 27) x [2, 13): the atlas trims it
    const PixelImage source = memberImage(32, 16, IntRect{5, 2, 22, 11}, 4);
    AtlasMemberInput member = textureMember(dir, "legacy/sheet.png", source);
    AtlasBuildInput input = atlasInput(dir, {member, textureMember(dir, "legacy/other.png", memberImage(10, 10, IntRect{0, 0, 10, 10}, 5))});
    std::shared_ptr<AtlasBuildResult> r = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(r->ok) << r->error.message;
    std::map<std::string, std::string> files = {{"legacy/sheet.png#sheet", r->memberFile(member.path).string()},
                                                {"legacy/sheet.png#atlas", r->sheetFile().string()}};
    for (size_t p = 0; p < r->pages.size(); p++) files["legacy/sheet.png#image#" + std::to_string(p)] = r->pageFile(p).string();
    ImportedAssets::setSheetProvider([&files](const std::string& ref, std::string& out) {
        auto it = files.find(ref);
        if (it == files.end()) return false;
        out = it->second;
        return true;
    });
    SpriteSheetPool::invalidate("legacy/sheet.png");
    std::vector<PixelImage> pages;
    for (size_t p = 0; p < r->pages.size(); p++) pages.push_back(*PixelImage::loadPng(r->pageFile(p), nullptr));

    // four frames of 16 x 8, two of them across the trim's edges
    const Rect inlineRects[] = {Rect(0, 0, 16, 8), Rect(16, 0, 16, 8), Rect(0, 8, 16, 8), Rect(16, 8, 16, 8), Rect(8, 4, 16, 8)};
    for (const Rect& rect : inlineRects) {
        SpriteFrame mapped;
        ASSERT_TRUE(SpriteSheetPool::mapInlineRect("legacy/sheet.png", rect, mapped));
        Rect copy = rect;
        const int rx = static_cast<int>(copy.getX()), ry = static_cast<int>(copy.getY());
        EXPECT_EQ(mapped.sourceSize, Vector2(rect.getWidth(), rect.getHeight()));
        const int ox = static_cast<int>(mapped.offset.x), oy = static_cast<int>(mapped.offset.y);
        const int px = static_cast<int>(mapped.rect.getX()), py = static_cast<int>(mapped.rect.getY());
        const int pw = static_cast<int>(mapped.rect.getWidth()), ph = static_cast<int>(mapped.rect.getHeight());
        // the inline rect as drawn: page pixels where the mapped rect is, transparent elsewhere
        for (int y = 0; y < static_cast<int>(rect.getHeight()); y++) {
            for (int x = 0; x < static_cast<int>(rect.getWidth()); x++) {
                const bool inside = x >= ox && y >= oy && x < ox + pw && y < oy + ph;
                const editor::pixelart::PixelRgba expected = source.at(rx + x, ry + y);
                if (inside) {
                    ASSERT_EQ(pages[mapped.page].at(px + x - ox, py + y - oy), expected) << "rect " << rx << "," << ry << " at " << x << "," << y;
                } else {
                    ASSERT_EQ(expected[3], 0) << "a trimmed-away pixel is transparent in the source";
                }
            }
        }
    }
    ImportedAssets::setSheetProvider(nullptr);
    SpriteSheetPool::invalidate("legacy/sheet.png");
}
