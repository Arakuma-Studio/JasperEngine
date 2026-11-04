// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SpritePacker (docs/specs/S44-aseprite-import.md R52, §6.10, AC22): cells never overlap and stay
// inside their pages, padding and extrusion are exact, pages split, members stay together, pages
// round, oversize frames fail, trims restore, duplicates share, the output is deterministic, the
// usage of a generated corpus and the layout time of 2,000 frames.

#include "sprite/SpritePacker.h"
#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine::editor::sprite;
using jasperengine::editor::pixelart::IntPoint;
using jasperengine::editor::pixelart::IntRect;
using jasperengine::editor::pixelart::IntSize;
using jasperengine::editor::pixelart::PixelImage;
using jasperengine::editor::pixelart::PixelRgba;

namespace {

    // std::uniform_int_distribution differs between standard libraries; this does not
    int pick(std::mt19937& rng, int lo, int hi) {
        return lo + static_cast<int>(rng() % static_cast<uint32_t>(hi - lo + 1));
    }

    // A canvas, transparent ({0, 0, 0, 0}) but for `opaque`, whose pixels differ by position and seed
    std::shared_ptr<const PixelImage> frame(int w, int h, IntRect opaque, int seed) {
        auto image = std::make_shared<PixelImage>(w, h);
        for (int y = opaque.y; y < opaque.y + opaque.h; y++) {
            for (int x = opaque.x; x < opaque.x + opaque.w; x++) {
                image->set(x, y, {static_cast<uint8_t>(x * 37 + seed), static_cast<uint8_t>(y * 59 + seed * 3),
                                  static_cast<uint8_t>(x * y + seed * 7 + (seed >> 8)), 255});
            }
        }
        return image;
    }

    std::shared_ptr<const PixelImage> opaqueFrame(int w, int h, int seed) { return frame(w, h, {0, 0, w, h}, seed); }

    PackInputFrame input(std::string name, std::shared_ptr<const PixelImage> image, bool noTrim = false) {
        return {std::move(name), std::move(image), noTrim};
    }

    int cellSide(int side, const PackSettings& settings) {
        const int cell = side + 2 * settings.extrude + settings.padding;
        return settings.blockAlign ? (cell + 3) / 4 * 4 : cell;
    }

    // Every packed frame inside its page, its cell (extrusion and padding included) clear of the
    // page's top-left padding and of every other cell; duplicates on their original's rect
    void expectValidLayout(const PackResult& result, const PackSettings& settings) {
        ASSERT_TRUE(result.ok()) << result.error.message;
        ASSERT_EQ(result.pages.size(), result.layout.pages.size());
        for (size_t p = 0; p < result.pages.size(); p++) {
            EXPECT_EQ(result.pages[p].width(), result.layout.pages[p].w);
            EXPECT_EQ(result.pages[p].height(), result.layout.pages[p].h);
        }
        std::vector<std::vector<IntRect>> cells(result.pages.size());
        for (size_t i = 0; i < result.layout.frames.size(); i++) {
            const PackedFrame& f = result.layout.frames[i];
            if (f.rect.empty()) continue;
            ASSERT_GE(f.page, 0);
            ASSERT_LT(static_cast<size_t>(f.page), result.pages.size());
            const IntSize page = result.layout.pages[f.page];
            EXPECT_EQ(f.rect.intersect({0, 0, page.w, page.h}), f.rect) << "frame " << i;
            if (f.duplicateOf >= 0) {
                EXPECT_EQ(f.rect, result.layout.frames[f.duplicateOf].rect);
                EXPECT_EQ(f.page, result.layout.frames[f.duplicateOf].page);
                continue;
            }
            const IntRect cell{f.rect.x - settings.extrude, f.rect.y - settings.extrude, cellSide(f.rect.w, settings),
                               cellSide(f.rect.h, settings)};
            EXPECT_GE(cell.x, settings.padding) << "frame " << i;
            EXPECT_GE(cell.y, settings.padding) << "frame " << i;
            EXPECT_LE(cell.x + cell.w, page.w) << "frame " << i;
            EXPECT_LE(cell.y + cell.h, page.h) << "frame " << i;
            if (settings.blockAlign) {
                EXPECT_EQ(cell.x % 4, 0);
                EXPECT_EQ(cell.y % 4, 0);
            }
            for (const IntRect& other : cells[f.page]) EXPECT_TRUE(cell.intersect(other).empty()) << "frame " << i;
            cells[f.page].push_back(cell);
        }
    }

    std::vector<PackMember> randomMembers(uint32_t seed, int memberCount, int framesPerMember) {
        std::mt19937 rng(seed);
        std::vector<PackMember> members;
        int counter = 0;
        for (int m = 0; m < memberCount; m++) {
            PackMember member;
            member.path = "sprites/m" + std::to_string(m) + ".aseprite";
            for (int f = 0; f < framesPerMember; f++) {
                const int w = pick(rng, 4, 40), h = pick(rng, 4, 40);
                const int ow = pick(rng, 1, w), oh = pick(rng, 1, h);
                const IntRect opaque{pick(rng, 0, w - ow), pick(rng, 0, h - oh), ow, oh};
                member.frames.push_back(input(member.path + " " + std::to_string(f), frame(w, h, opaque, counter++)));
            }
            members.push_back(std::move(member));
        }
        return members;
    }

    bool samePlacement(const PackedFrame& a, const PackedFrame& b) {
        return a.page == b.page && a.rect == b.rect && a.sourceSize == b.sourceSize && a.offset == b.offset;
    }

}

TEST(SpritePackerTest, EmptyInputGivesNoPages) {
    const PackResult result = pack({}, PackSettings{});
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.pages.empty());
    EXPECT_TRUE(result.layout.frames.empty());
    EXPECT_EQ(result.usage, 0.0);
}

TEST(SpritePackerTest, CellsNeverOverlapAndStayInsidePages) {
    for (const bool together : {true, false}) {
        PackSettings settings;
        settings.maxSize = 128;
        settings.padding = 2;
        settings.extrude = 1;
        settings.keepMembersTogether = together;
        const PackResult result = pack(randomMembers(7, 6, 25), settings);
        expectValidLayout(result, settings);
        EXPECT_GT(result.pages.size(), 1u);
        for (const IntSize& page : result.layout.pages) {
            EXPECT_LE(page.w, 128);
            EXPECT_LE(page.h, 128);
        }
    }
}

TEST(SpritePackerTest, PaddingAndExtrusionAreExact) {
    PackSettings settings;
    settings.padding = 2;
    settings.extrude = 2;
    // the largest frame goes first, at the page's top-left padding
    PackMember member{"a.aseprite", {input("big", opaqueFrame(5, 3, 1)), input("small", opaqueFrame(2, 2, 2))}};
    const PackResult result = pack({member}, settings);
    expectValidLayout(result, settings);
    ASSERT_EQ(result.pages.size(), 1u);
    const PackedFrame& big = result.layout.frames[0];
    EXPECT_EQ(big.rect, (IntRect{4, 4, 5, 3}));

    const PixelImage& page = result.pages[0];
    std::vector<IntRect> extruded;
    for (size_t i = 0; i < 2; i++) {
        const PackedFrame& f = result.layout.frames[i];
        const PixelImage& source = *member.frames[i].image;
        const IntRect area{f.rect.x - 2, f.rect.y - 2, f.rect.w + 4, f.rect.h + 4};
        extruded.push_back(area);
        for (int y = area.y; y < area.y + area.h; y++) {
            for (int x = area.x; x < area.x + area.w; x++) {
                const int sx = std::clamp(x - f.rect.x, 0, f.rect.w - 1);
                const int sy = std::clamp(y - f.rect.y, 0, f.rect.h - 1);
                EXPECT_EQ(page.at(x, y), source.at(sx, sy)) << "frame " << i << " at " << x << "," << y;
            }
        }
    }
    // everything else (the padding) is transparent, and the page ends right after it
    for (int y = 0; y < page.height(); y++) {
        for (int x = 0; x < page.width(); x++) {
            if (extruded[0].contains(x, y) || extruded[1].contains(x, y)) continue;
            EXPECT_EQ(page.at(x, y), (PixelRgba{0, 0, 0, 0})) << x << "," << y;
        }
    }
    int right = 0, bottom = 0;
    for (const IntRect& area : extruded) {
        right = std::max(right, area.x + area.w + 2);
        bottom = std::max(bottom, area.y + area.h + 2);
    }
    EXPECT_EQ(page.width(), right);
    EXPECT_EQ(page.height(), bottom);
}

TEST(SpritePackerTest, SmallMaxSizeSplitsIntoPages) {
    PackSettings settings;
    settings.maxSize = 64;
    settings.padding = 0;
    settings.extrude = 0;
    PackMember member{"a.aseprite", {}};
    for (int i = 0; i < 20; i++) member.frames.push_back(input("f" + std::to_string(i), opaqueFrame(30, 30, i)));
    const PackResult result = pack({member}, settings);
    expectValidLayout(result, settings);
    // four 30x30 frames per 64x64 page
    ASSERT_EQ(result.pages.size(), 5u);
    for (const IntSize& page : result.layout.pages) EXPECT_EQ(page, (IntSize{60, 60}));
    EXPECT_NEAR(result.usage, 1.0, 1e-9);
}

TEST(SpritePackerTest, KeepMembersTogetherMovesAWholeMember) {
    PackSettings settings;
    settings.maxSize = 64;
    settings.padding = 0;
    settings.extrude = 0;
    // four 30x30 slots per page: one member takes two, the other's three do not fit the rest
    std::vector<PackMember> members(2);
    members[0].path = "a.aseprite";
    members[1].path = "b.aseprite";
    for (int i = 0; i < 3; i++) members[0].frames.push_back(input("a" + std::to_string(i), opaqueFrame(30, 30, i)));
    for (int i = 0; i < 2; i++) members[1].frames.push_back(input("b" + std::to_string(i), opaqueFrame(30, 30, 10 + i)));

    const auto pagesOf = [](const PackResult& result, size_t first, size_t count) {
        std::vector<int> pages;
        for (size_t i = first; i < first + count; i++) pages.push_back(result.layout.frames[i].page);
        std::sort(pages.begin(), pages.end());
        pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
        return pages;
    };

    const PackResult together = pack(members, settings);
    expectValidLayout(together, settings);
    ASSERT_EQ(together.pages.size(), 2u);
    EXPECT_EQ(pagesOf(together, 0, 3).size(), 1u);
    EXPECT_EQ(pagesOf(together, 3, 2).size(), 1u);
    EXPECT_NE(pagesOf(together, 0, 3), pagesOf(together, 3, 2));

    settings.keepMembersTogether = false;
    const PackResult apart = pack(members, settings);
    expectValidLayout(apart, settings);
    ASSERT_EQ(apart.pages.size(), 2u);
    EXPECT_EQ(pagesOf(apart, 0, 3).size() + pagesOf(apart, 3, 2).size(), 3u);   // one member straddles
}

TEST(SpritePackerTest, PageSidesRound) {
    const auto pageOf = [](PackSettings settings) {
        settings.padding = 0;
        settings.extrude = 0;
        const PackResult result = pack({{"a.aseprite", {input("f", opaqueFrame(10, 6, 1))}}}, settings);
        EXPECT_TRUE(result.ok());
        return result.layout.pages.empty() ? IntSize{} : result.layout.pages[0];
    };
    PackSettings settings;
    EXPECT_EQ(pageOf(settings), (IntSize{10, 6}));
    settings.blockAlign = true;
    EXPECT_EQ(pageOf(settings), (IntSize{12, 8}));
    settings.blockAlign = false;
    settings.powerOfTwo = true;
    EXPECT_EQ(pageOf(settings), (IntSize{16, 8}));
    settings.square = true;
    EXPECT_EQ(pageOf(settings), (IntSize{16, 16}));
    settings.powerOfTwo = false;
    EXPECT_EQ(pageOf(settings), (IntSize{10, 10}));

    // block alignment puts every cell on the 4x4 grid
    PackSettings block;
    block.blockAlign = true;
    block.maxSize = 128;
    const PackResult result = pack(randomMembers(3, 2, 20), block);
    expectValidLayout(result, block);
    for (const IntSize& page : result.layout.pages) {
        EXPECT_EQ(page.w % 4, 0);
        EXPECT_EQ(page.h % 4, 0);
    }
}

TEST(SpritePackerTest, OversizeFrameIsTooLarge) {
    PackSettings settings;
    settings.maxSize = 256;
    // 1 (top padding) + 252 + 2 * 1 (extrusion) + 1 (padding) = 256: fits
    PackResult result = pack({{"a.aseprite", {input("fits", opaqueFrame(252, 10, 1))}}}, settings);
    EXPECT_TRUE(result.ok()) << result.error.message;

    result = pack({{"a.aseprite", {input("small", opaqueFrame(8, 8, 1)), input("wide", opaqueFrame(253, 10, 2))}}}, settings);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error.kind, "too_large");
    EXPECT_EQ(result.error.field, "texture.maxSize");
    EXPECT_NE(result.error.message.find("wide"), std::string::npos) << result.error.message;
    EXPECT_NE(result.error.message.find("253x10"), std::string::npos) << result.error.message;
    EXPECT_EQ(result.error.fix, "raise Max Size or split the frame");
    EXPECT_TRUE(result.pages.empty());

    settings.padding = -1;
    result = pack({{"a.aseprite", {input("f", opaqueFrame(8, 8, 1))}}}, settings);
    EXPECT_EQ(result.error.kind, "invalid_settings");
}

TEST(SpritePackerTest, TrimOffsetsRestoreEachFrame) {
    PackSettings settings;
    std::vector<PackMember> members = randomMembers(11, 3, 30);
    // a fully transparent frame takes no space; a bordered one keeps its transparent edges
    members[0].frames.push_back(input("empty", std::make_shared<PixelImage>(12, 9)));
    members[1].frames.push_back(input("bordered", frame(16, 16, {4, 4, 8, 8}, 999), true));
    const PackResult result = pack(members, settings);
    expectValidLayout(result, settings);

    size_t flat = 0;
    for (const PackMember& member : members) {
        for (const PackInputFrame& entry : member.frames) {
            const PackedFrame& f = result.layout.frames[flat++];
            const PixelImage& source = *entry.image;
            EXPECT_EQ(f.sourceSize, (IntSize{source.width(), source.height()}));
            PixelImage canvas(f.sourceSize.w, f.sourceSize.h);
            if (!f.rect.empty()) canvas.blit(result.pages[f.page].crop(f.rect), f.offset.x, f.offset.y);
            EXPECT_TRUE(canvas == source) << entry.name;
            if (entry.name == "empty") {
                EXPECT_EQ(f.rect, (IntRect{0, 0, 0, 0}));
                EXPECT_EQ(f.page, 0);
                EXPECT_EQ(f.offset, (IntPoint{0, 0}));
            }
            if (entry.name == "bordered") {
                EXPECT_EQ(f.rect.w, 16);
                EXPECT_EQ(f.offset, (IntPoint{0, 0}));
            }
        }
    }
    settings.trim = false;
    const PackResult untrimmed = pack(members, settings);
    expectValidLayout(untrimmed, settings);
    flat = 0;
    for (const PackMember& member : members) {
        for (const PackInputFrame& entry : member.frames) {
            const PackedFrame& f = untrimmed.layout.frames[flat++];
            EXPECT_EQ(f.rect.w, entry.image->width());
            EXPECT_EQ(f.rect.h, entry.image->height());
        }
    }
}

TEST(SpritePackerTest, DuplicatesShareOneRect) {
    const IntRect inner{0, 0, 6, 5};
    const auto a = frame(6, 5, inner, 1);
    // the same pixels at another place of a larger canvas: identical once trimmed
    auto shifted = std::make_shared<PixelImage>(10, 9);
    shifted->blit(*a, 3, 2);
    std::vector<PackMember> members(2);
    members[0].path = "a.aseprite";
    members[0].frames = {input("a", a), input("shifted", shifted), input("other", frame(6, 5, inner, 2))};
    members[1].path = "b.aseprite";
    members[1].frames = {input("again", a)};
    const PackResult result = pack(members, PackSettings{});
    expectValidLayout(result, PackSettings{});
    const std::vector<PackedFrame>& f = result.layout.frames;
    ASSERT_EQ(f.size(), 4u);
    EXPECT_EQ(f[0].duplicateOf, -1);
    EXPECT_EQ(f[1].duplicateOf, 0);
    EXPECT_EQ(f[1].rect, f[0].rect);
    EXPECT_EQ(f[1].offset, (IntPoint{3, 2}));
    EXPECT_EQ(f[1].sourceSize, (IntSize{10, 9}));
    EXPECT_EQ(f[2].duplicateOf, -1);
    EXPECT_NE(f[2].rect, f[0].rect);
    EXPECT_EQ(f[3].duplicateOf, 0);
    EXPECT_EQ(f[3].rect, f[0].rect);
    // two frames packed, not four
    const double area = 2.0 * 6 * 5;
    double pageArea = 0;
    for (const IntSize& page : result.layout.pages) pageArea += static_cast<double>(page.w) * page.h;
    EXPECT_NEAR(result.usage, area / pageArea, 1e-12);
}

TEST(SpritePackerTest, SameInputGivesTheSameBytesInAnyMemberOrder) {
    PackSettings settings;
    settings.maxSize = 64;
    std::vector<PackMember> members = randomMembers(23, 5, 20);
    // a duplicate across members
    members[3].frames.push_back(members[1].frames[2]);
    const PackResult first = pack(members, settings);
    expectValidLayout(first, settings);
    ASSERT_GT(first.pages.size(), 1u);

    const PackResult second = pack(members, settings);
    ASSERT_EQ(second.layout.pages, first.layout.pages);
    ASSERT_EQ(second.layout.frames.size(), first.layout.frames.size());
    for (size_t i = 0; i < first.layout.frames.size(); i++) {
        EXPECT_TRUE(samePlacement(first.layout.frames[i], second.layout.frames[i]));
        EXPECT_EQ(first.layout.frames[i].duplicateOf, second.layout.frames[i].duplicateOf);
    }
    for (size_t p = 0; p < first.pages.size(); p++) EXPECT_TRUE(first.pages[p] == second.pages[p]) << "page " << p;

    const std::vector<size_t> permutation = {3, 0, 4, 2, 1};
    std::vector<PackMember> shuffled;
    for (size_t m : permutation) shuffled.push_back(members[m]);
    const PackResult reordered = pack(shuffled, settings);
    expectValidLayout(reordered, settings);
    ASSERT_EQ(reordered.layout.pages, first.layout.pages);
    std::vector<size_t> firstFlat(members.size());
    for (size_t m = 1; m < members.size(); m++) firstFlat[m] = firstFlat[m - 1] + members[m - 1].frames.size();
    size_t flat = 0;
    for (size_t m : permutation) {
        for (size_t f = 0; f < members[m].frames.size(); f++) {
            EXPECT_TRUE(samePlacement(reordered.layout.frames[flat++], first.layout.frames[firstFlat[m] + f]))
                << members[m].path << " frame " << f;
        }
    }
    for (size_t p = 0; p < first.pages.size(); p++) EXPECT_TRUE(first.pages[p] == reordered.pages[p]) << "page " << p;
}

TEST(SpritePackerTest, GeneratedCorpusUsesMostOfThePages) {
    // 500 frames whose opaque part (at least half the canvas each way) sits anywhere in it.
    // Padding and extrusion off: usage counts the frames' own pixels, so the cells' margins would
    // measure the settings rather than the packer. The default max size: one page, which the bin
    // search has to keep tight.
    std::mt19937 rng(20260601u);
    PackMember member{"corpus.aseprite", {}};
    for (int i = 0; i < 500; i++) {
        const int w = pick(rng, 8, 96), h = pick(rng, 8, 96);
        const int ow = pick(rng, w / 2, w), oh = pick(rng, h / 2, h);
        const IntRect opaque{pick(rng, 0, w - ow), pick(rng, 0, h - oh), ow, oh};
        member.frames.push_back(input("f" + std::to_string(i), frame(w, h, opaque, i)));
    }
    PackSettings settings;
    settings.padding = 0;
    settings.extrude = 0;
    const PackResult result = pack({member}, settings);
    expectValidLayout(result, settings);
    std::printf("SpritePackerTest corpus: %zu pages, usage %.3f\n", result.pages.size(), result.usage);
    EXPECT_GE(result.usage, 0.85);
}

TEST(SpritePackerTest, SmallSheetPacksIntoASmallPage) {
    // with the 2048 bin alone the 24 cells would run along the top edge, 1609x68
    PackMember member{"a.aseprite", {}};
    for (int i = 0; i < 24; i++) member.frames.push_back(input("f" + std::to_string(i), opaqueFrame(64, 64, i)));
    const PackSettings settings;
    const PackResult result = pack({member}, settings);
    expectValidLayout(result, settings);
    ASSERT_EQ(result.pages.size(), 1u);
    std::printf("SpritePackerTest 24 frames of 64x64: page %dx%d\n", result.layout.pages[0].w, result.layout.pages[0].h);
    EXPECT_LE(result.layout.pages[0].w, 512);
    EXPECT_LE(result.layout.pages[0].h, 512);
}

TEST(SpritePackerTest, SpritePackerBench) {
    std::mt19937 rng(424242u);
    std::vector<LayoutInput> items;
    for (int i = 0; i < 2000; i++) {
        const int member = i / 20;
        items.push_back({member, "sprites/m" + std::to_string(member) + ".aseprite", "f" + std::to_string(i),
                         {pick(rng, 8, 64), pick(rng, 8, 64)}});
    }
    PackSettings settings;
    settings.maxSize = 2048;
    std::vector<int> pages;
    std::vector<IntPoint> positions;
    std::vector<IntSize> pageSizes;
    jasperengine::editor::pixelart::PixelProblem error;
    const auto start = std::chrono::steady_clock::now();
    const bool ok = layout(items, settings, pages, positions, pageSizes, &error);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("SpritePackerBench: layout of %zu frames into %zu page(s) of at most 2048x2048: %.2f ms\n",
                items.size(), pageSizes.size(), ms);
    ASSERT_TRUE(ok) << error.message;
    EXPECT_EQ(pages.size(), items.size());
    if (jasperengine::editor::test::perfSlack() > 0.0) {
        EXPECT_LT(ms, 100.0 * jasperengine::editor::test::perfSlack());
    }
}
