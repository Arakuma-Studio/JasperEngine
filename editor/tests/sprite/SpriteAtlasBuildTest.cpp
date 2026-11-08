// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lossless pages and the inline remap (docs/specs/S45-sprite-atlas.md AC5, AC9, R8, R16, R17):
// every page texel inside a frame equals its source texel (RGBA, the colour of transparent
// texels included) in the page files as built and in the export's S14 cook of them (quality none,
// RGBA8 + Zstandard, level 0 decoded), block formats align rects to 4, remaps keep the member's
// names, order, sizes and pivots, an unchanged page keeps its file, and a legacy sprite's inline
// rects (crossing the trim too) map onto the same pixels of the page. A finished build never
// removes the pages of a newer one written to its folder, and an export builds in a folder of its
// own. The GPU tests' atlas (engine/tests/core/fixtures/sprite-atlas/) is this builder's output.

#include "SpriteAtlasTestSupport.h"
#include "support/EditorTestSupport.h"

#include "asset/ImportedAssets.h"
#include "sprite/SpriteAtlasCook.h"
#include "texture/Ktx2.h"
#include "texture/TextureCookQueue.h"
#include "texture/TextureCooker.h"
#include "sprite/SpriteSheetIO.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>

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

// AC5: the export cooks a lossless page through S14 (quality none: RGBA8 + Zstandard, no mips, no
// alpha bleed); level 0 decoded is the page, so every frame texel is still its source texel
TEST(SpriteAtlasBuild, LosslessCookIsThePage) {
    editor::test::ensureEngine();
    ScratchDir dir("cook");
    std::map<std::string, PixelImage> sources;
    std::vector<AtlasMemberInput> members;
    for (int i = 0; i < 4; i++) {
        const std::string path = "items/c" + std::to_string(i) + ".png";
        sources[path] = memberImage(17 + i, 15, IntRect{2, 2 + i % 2, 11, 9}, 20 + i);
        members.push_back(textureMember(dir, path, sources[path]));
    }
    AtlasBuildInput input = atlasInput(dir, members);
    input.doc.settings.trim = false;   // the transparent texels' colour reaches the page, and must survive the cook
    std::shared_ptr<AtlasBuildResult> r = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(r->ok) << r->error.message;
    ASSERT_EQ(r->compression, "lossless");
    editor::TextureCookCache cache;
    cache.open(dir.root / "cook-cache", "", 1ull << 30);
    for (size_t p = 0; p < r->pages.size(); p++) {
        const editor::TextureCookInputs in = atlasPageCookInputs(*r, p);
        EXPECT_EQ(in.assetPath.rfind("je-imported/atlas/", 0), 0u) << in.assetPath;
        const editor::TextureCookResult cooked = editor::TextureCooker::cook(cache, editor::makeTextureCookRequest(in, editor::TextureProjectSettings(), editor::TextureFamily::Desktop), nullptr);
        ASSERT_TRUE(cooked.ok) << cooked.error;
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(cache.readArtifact(cooked.record.key, bytes));
        Ktx2MemorySource source(bytes);
        Ktx2Image image;
        std::string error;
        ASSERT_TRUE(Ktx2::read(source, 0, image, error)) << error;
        EXPECT_EQ(image.info.format, GpuTextureFormat::RGBA8);
        EXPECT_EQ(image.info.supercompression, Ktx2Supercompression::Zstd);
        EXPECT_EQ(image.info.levelCount, 1u) << "no mips";
        ASSERT_EQ(image.info.width, static_cast<uint32_t>(r->pages[p].width));
        ASSERT_EQ(image.info.height, static_cast<uint32_t>(r->pages[p].height));
        std::optional<PixelImage> page = PixelImage::loadPng(r->pageFile(p), nullptr);
        ASSERT_TRUE(page);
        const std::span<const uint8_t> pixels = page->pixels();
        ASSERT_EQ(image.levels[0].size(), pixels.size());
        EXPECT_TRUE(std::equal(pixels.begin(), pixels.end(), image.levels[0].begin())) << "the cooked level 0 is the page, byte for byte";
        // and every frame texel its source texel, transparent colours included
        for (const AtlasFrameRow& f : r->frames) {
            if (f.page != static_cast<int>(p)) continue;
            const PixelImage& src = sources.at(f.member);
            for (int y = 0; y < f.rect.h; y++) {
                for (int x = 0; x < f.rect.w; x++) {
                    const size_t at = (static_cast<size_t>(f.rect.y + y) * static_cast<size_t>(image.info.width) + static_cast<size_t>(f.rect.x + x)) * 4;
                    const editor::pixelart::PixelRgba want = src.at(f.offset.x + x, f.offset.y + y);
                    ASSERT_EQ((editor::pixelart::PixelRgba{image.levels[0][at], image.levels[0][at + 1], image.levels[0][at + 2], image.levels[0][at + 3]}), want)
                        << f.name << " at " << x << "," << y;
                }
            }
        }
    }
    cache.close();
}

// A build that finishes after a newer one was written to the same folder (a queued rebuild) keeps
// the newer build's pages: the folder's build.json names them. An export builds elsewhere.
TEST(SpriteAtlasBuild, AFinishedBuildKeepsANewerBuildsPages) {
    editor::test::ensureEngine();
    ScratchDir dir("race");
    std::vector<AtlasMemberInput> members = {textureMember(dir, "r/a.png", memberImage(20, 20, IntRect{0, 0, 20, 20}, 1)),
                                             textureMember(dir, "r/b.png", memberImage(20, 20, IntRect{0, 0, 20, 20}, 2))};
    AtlasBuildInput input = atlasInput(dir, members);
    std::shared_ptr<AtlasBuildResult> older = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(older->ok) << older->error.message;
    input.members[1] = textureMember(dir, "r/b.png", memberImage(20, 20, IntRect{0, 0, 20, 20}, 9));
    std::shared_ptr<AtlasBuildResult> newer = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(newer->ok) << newer->error.message;
    std::set<std::string> olderOnly;
    for (const AtlasPageRow& p : older->pages) olderOnly.insert(p.file);
    for (const AtlasPageRow& p : newer->pages) olderOnly.erase(p.file);
    ASSERT_FALSE(olderOnly.empty()) << "the newer build wrote a page of its own";

    // the older build is adopted last (its swap finished after the newer one was written)
    SpriteAtlasBuilder::removeStaleFiles(*older);
    for (size_t p = 0; p < newer->pages.size(); p++) EXPECT_TRUE(std::filesystem::exists(newer->pageFile(p))) << "the newer build's page " << p << " is kept";
    SpriteSheetParseResult sheet = parseSheet(readFile(newer->sheetFile()));
    ASSERT_TRUE(sheet.ok()) << sheet.error;
    for (const SpritePage& page : sheet.sheet->pages) {
        EXPECT_TRUE(std::filesystem::exists(newer->dir / std::filesystem::path(page.texture).filename())) << page.texture << ": sheet.json names no missing page";
    }
    // the newer one adopted: what only the older one named goes
    SpriteAtlasBuilder::removeStaleFiles(*newer);
    for (const std::string& file : olderOnly) EXPECT_FALSE(std::filesystem::exists(newer->dir / file)) << file;

    // an export's folder is its own: its builds and prunes never touch the editor's pages
    const std::filesystem::path exportDir = SpriteAtlasBuilder::exportDir(input);
    EXPECT_NE(exportDir, input.outDir);
    EXPECT_EQ(exportDir.parent_path(), input.outDir.parent_path());
    AtlasBuildInput exported = input;
    exported.members[1] = textureMember(dir, "r/b.png", memberImage(20, 20, IntRect{0, 0, 20, 20}, 13));
    exported.outDir = exportDir;
    std::shared_ptr<AtlasBuildResult> exportBuild = SpriteAtlasBuilder::get().build(exported);
    ASSERT_TRUE(exportBuild->ok);
    SpriteAtlasBuilder::removeStaleFiles(*exportBuild);
    for (size_t p = 0; p < newer->pages.size(); p++) EXPECT_TRUE(std::filesystem::exists(newer->pageFile(p))) << "the export left the editor's pages";
}

// Two members that differ only in their extension name their frames by their full paths; an SVG
// member is rasterized as the engine does (every format a texture loads from)
TEST(SpriteAtlasBuild, MembersOfOneStemAndSvgMembers) {
    editor::test::ensureEngine();
    ScratchDir dir("stems");
    AtlasMemberInput png = textureMember(dir, "hero.png", memberImage(10, 10, IntRect{0, 0, 10, 10}, 3));
    // (stb reads by content: PNG bytes under another image extension)
    AtlasMemberInput bmp = textureMember(dir, "hero.bmp", memberImage(12, 12, IntRect{0, 0, 12, 12}, 4));
    const std::filesystem::path svgFile = dir.assets() / "dot.svg";
    {
        std::ofstream out(svgFile);
        out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"6\"><rect width=\"8\" height=\"6\" fill=\"#ff0000\"/></svg>\n";
    }
    AtlasMemberInput svg;
    svg.path = "dot.svg";
    svg.absolute = svgFile;
    svg.kind = "texture";
    svg.stamp = "svg";
    AtlasBuildInput input = atlasInput(dir, {png, bmp, svg});
    std::shared_ptr<AtlasBuildResult> r = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(r->ok) << r->error.message;
    std::set<std::string> names;
    for (const AtlasFrameRow& f : r->frames) names.insert(f.name);
    EXPECT_TRUE(names.count("hero.png/hero") && names.count("hero.bmp/hero")) << "no two frames named hero/hero";
    EXPECT_TRUE(names.count("dot")) << "the SVG member packs";
    for (const AtlasFrameRow& f : r->frames) {
        if (f.member == "dot.svg") {
            EXPECT_EQ(f.sourceSize, (editor::pixelart::IntSize{8, 6}));
        }
    }
    for (const editor::pixelart::PixelProblem& w : r->warnings) ADD_FAILURE() << "warning: " << w.message;
}

// R29: the stats name the Desktop and Web VRAM; Web counts its own layout when its max size differs
TEST(SpriteAtlasBuild, WebVramCountsTheWebLayout) {
    editor::test::ensureEngine();
    ScratchDir dir("vram");
    std::vector<AtlasMemberInput> members;
    for (int i = 0; i < 4; i++) members.push_back(textureMember(dir, "v/m" + std::to_string(i) + ".png", memberImage(30, 30, IntRect{0, 0, 30, 30}, i)));
    AtlasBuildInput input = atlasInput(dir, members);
    input.doc.settings.maxSize = 128;
    input.doc.settings.platforms["web"] = AtlasPlatformOverride{true, 64};
    std::shared_ptr<AtlasBuildResult> r = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(r->ok) << r->error.message;
    EXPECT_EQ(r->pages.size(), 1u);
    EXPECT_EQ(r->webPages.size(), 4u) << "one padded and extruded 30 x 30 member a 64 px web page";
    EXPECT_NE(r->vramBytes("web"), r->vramBytes("desktop"));
    EXPECT_NE(r->statsLine().find("VRAM (Desktop)"), std::string::npos) << r->statsLine();
    EXPECT_NE(r->statsLine().find("(Web)"), std::string::npos) << r->statsLine();
    input.doc.settings.platforms.clear();
    input.outDir = dir.root / "same";
    std::shared_ptr<AtlasBuildResult> same = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(same->ok);
    EXPECT_TRUE(same->webPages.empty());
    EXPECT_EQ(same->vramBytes("web"), same->vramBytes("desktop"));
}

namespace {

    // the GPU tests' sources: three 4-frame strips (a 12 x 12 opaque box inset differently per frame)
    std::array<uint8_t, 4> gpuPixel(int s, int f, int x, int y) {
        const int left = 1 + f, top = 2 + (f + s) % 3, right = 12 - 1 - s, bottom = 12 - 1 - f / 2;
        if (x < left || x >= right || y < top || y >= bottom) return {0, 0, 0, 0};
        return {static_cast<uint8_t>(30 + x * 15 + s * 40), static_cast<uint8_t>(20 + y * 17), static_cast<uint8_t>(50 * f + 30), 255};
    }

}

// AC2/AC3's atlas: engine/tests/core/fixtures/sprite-atlas/ holds this builder's page and remaps
// (pages renamed page<n>.png), so the GPU tests draw what the builder makes; the build changing
// fails here (JASPER_TEST_UPDATE_GOLDEN=1 writes the fixture again)
TEST(SpriteAtlasBuild, GpuFixtureIsTheBuildersOutput) {
    editor::test::ensureEngine();
    ScratchDir dir("gpu-fixture");
    std::map<std::string, PixelImage> images;
    std::vector<AtlasMemberInput> members;
    for (int s = 0; s < 3; s++) {
        PixelImage strip(48, 12);
        for (int f = 0; f < 4; f++) {
            for (int y = 0; y < 12; y++) {
                for (int x = 0; x < 12; x++) {
                    const std::array<uint8_t, 4> p = gpuPixel(s, f, x, y);
                    strip.set(f * 12 + x, y, {p[0], p[1], p[2], p[3]});
                }
            }
        }
        const std::string path = "source" + std::to_string(s) + "@12x12.png";
        images[path] = strip;
        members.push_back(textureMember(dir, path, strip));
    }
    // a 12 x 12 panel: 4-pixel corners red, green, blue, yellow; edges grey; centre white; border 4
    PixelImage panel(12, 12);
    for (int y = 0; y < 12; y++) {
        for (int x = 0; x < 12; x++) {
            const bool left = x < 4, right = x >= 8, top = y < 4, bottom = y >= 8;
            editor::pixelart::PixelRgba c = {255, 255, 255, 255};
            if (left && top) c = {255, 0, 0, 255};
            else if (right && top) c = {0, 255, 0, 255};
            else if (left && bottom) c = {0, 0, 255, 255};
            else if (right && bottom) c = {255, 255, 0, 255};
            else if (left || right || top || bottom) c = {120, 120, 120, 255};
            panel.set(x, y, c);
        }
    }
    images["panel.png"] = panel;
    members.push_back(textureMember(dir, "panel.png", panel));
    TextureSheetSettings panelMode;
    panelMode.mode = TextureSpriteMode::MULTIPLE;
    TextureSheetFrame frame;
    frame.name = "panel";
    frame.rect = Rect(0, 0, 12, 12);
    frame.border = Vector4(4, 4, 4, 4);
    panelMode.frames.push_back(frame);
    writeSpriteMode(dir.assets() / "panel.png", panelMode);
    AtlasBuildInput input = atlasInput(dir, members);
    input.doc.settings.maxSize = 128;
    input.doc.settings.padding = 1;
    input.doc.settings.extrude = 0;
    std::shared_ptr<AtlasBuildResult> r = SpriteAtlasBuilder::get().build(input);
    ASSERT_TRUE(r->ok) << r->error.message;
    ASSERT_EQ(r->pages.size(), 1u);

    // the files as the GPU tests read them
    std::map<std::string, std::string> files;
    for (const auto& [path, image] : images) files[path] = image.encodePng();
    files["panel.png.meta"] = readFile(editor::SetAssetMetaCmd::metaPathOf(dir.assets() / "panel.png"));
    files["page0.png"] = PixelImage::loadPng(r->pageFile(0), nullptr)->encodePng();
    for (const std::string& member : r->members) {
        SpriteSheetParseResult remap = parseSheet(readFile(r->memberFile(member)));
        ASSERT_TRUE(remap.ok()) << remap.error;
        for (size_t p = 0; p < remap.sheet->pages.size(); p++) remap.sheet->pages[p].texture = "page" + std::to_string(p) + ".png";
        files[std::filesystem::path(member).stem().string() + ".remap.json"] = writeSheet(*remap.sheet);
    }
    const std::filesystem::path fixture = std::filesystem::path(JASPER_TEST_REPO_DIR) / "engine" / "tests" / "core" / "fixtures" / "sprite-atlas";
    if (std::getenv("JASPER_TEST_UPDATE_GOLDEN")) {
        std::filesystem::create_directories(fixture);
        for (const auto& [name, bytes] : files) {
            std::ofstream out(fixture / name, std::ios::binary);
            out << bytes;
        }
    }
    for (const auto& [name, bytes] : files) {
        const std::string committed = readFile(fixture / name);
        if (name.size() > 4 && name.substr(name.size() - 4) == ".png") {
            std::optional<PixelImage> want = PixelImage::loadPng(fixture / name, nullptr);
            std::optional<PixelImage> got = PixelImage::decodePng(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), nullptr);
            ASSERT_TRUE(want && got) << name;
            EXPECT_TRUE(*want == *got) << name << " differs from the builder's (JASPER_TEST_UPDATE_GOLDEN=1 writes it)";
        } else {
            EXPECT_EQ(committed, bytes) << name << " differs from the builder's (JASPER_TEST_UPDATE_GOLDEN=1 writes it)";
        }
    }
}
