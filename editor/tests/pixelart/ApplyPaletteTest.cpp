// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Apply Palette, Reduce Colours and Remap Indices (docs/specs/S43-palettes.md R23, R24, R40, AC9,
// AC15's verdicts), and the RGBA8 image they work on.

#include "pixelart/ApplyPalette.h"
#include "pixelart/RgbaImage.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace applypalette_test {

    namespace px = jasperengine::pixelart;
    using namespace jasperengine::editor::pixelart;
    using px::Rgba8;

    const Rgba8 kBlack{0, 0, 0, 255};
    const Rgba8 kWhite{255, 255, 255, 255};
    // OKLab L 0.4997 and 0.5795: 0.08 apart, past the lost threshold (0.06), within fixable (0.15)
    const Rgba8 kGrey99{99, 99, 99, 255};
    const Rgba8 kGrey122{122, 122, 122, 255};

    // PICO-8's first eight
    std::vector<Rgba8> eightColors() {
        return {{0, 0, 0, 255}, {29, 43, 83, 255}, {126, 37, 83, 255}, {0, 135, 81, 255},
                {171, 82, 54, 255}, {95, 87, 79, 255}, {194, 195, 199, 255}, {255, 241, 232, 255}};
    }

    RgbaImage patternImage(int width, int height, const std::vector<Rgba8>& palette) {
        RgbaImage image(width, height);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) image.set(x, y, palette[(size_t)(x * 3 + y * 5 + (x * y) % 7) % palette.size()]);
        }
        return image;
    }

    // a smooth photo-like patch: every pixel its own colour
    RgbaImage gradientImage(int width, int height) {
        RgbaImage image(width, height);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                image.set(x, y, Rgba8{(uint8_t)(x * 255 / std::max(1, width - 1)), (uint8_t)(y * 255 / std::max(1, height - 1)),
                                      (uint8_t)(255 - (x + y) * 255 / std::max(1, width + height - 2)), 255});
            }
        }
        return image;
    }

    bool sameRgb(Rgba8 x, Rgba8 y) { return x.r == y.r && x.g == y.g && x.b == y.b; }

    bool inPalette(Rgba8 color, const std::vector<Rgba8>& palette) {
        for (const Rgba8& entry : palette) {
            if (sameRgb(entry, color)) return true;
        }
        return false;
    }

    ApplyResult applyWith(const RgbaImage& image, const std::vector<Rgba8>& palette, Dither dither = Dither::None) {
        ApplyRequest request;
        request.palette = palette;
        request.dither = dither;
        return applyPalette(image, request);
    }

    TEST(ApplyPalette, InPaletteIsCleanInEveryDitherMode) {
        const std::vector<Rgba8> palette = eightColors();
        RgbaImage image = patternImage(16, 16, palette);
        image.set(0, 0, Rgba8{12, 34, 56, 0});                                     // transparent, any RGB: untouched
        image.set(1, 0, Rgba8{palette[2].r, palette[2].g, palette[2].b, 128});      // Keep leaves the alpha
        for (Dither dither : {Dither::None, Dither::Bayer2, Dither::Bayer4, Dither::Bayer8, Dither::FloydSteinberg}) {
            SCOPED_TRACE(ditherName(dither));
            const ApplyResult result = applyWith(image, palette, dither);
            EXPECT_TRUE(result.image == image);
            EXPECT_EQ(result.stats.pixelsChanged, 0);
            EXPECT_EQ(result.stats.pixelsMapped, 255);
            EXPECT_EQ(result.stats.maxDeltaE, 0.0);
            EXPECT_EQ(result.stats.colorsBefore, result.stats.colorsAfter);
            EXPECT_TRUE(result.lost.empty());
            EXPECT_EQ(result.verdict, Verdict::Clean);
            ASSERT_EQ(result.usage.size(), palette.size());
            for (const ColorUsage& usage : result.usage) {
                int64_t direct = 0;
                for (const Rgba8& pixel : image.pixels) {
                    if (pixel.a != 0 && sameRgb(pixel, palette[(size_t)usage.index])) direct++;
                }
                EXPECT_EQ(usage.count, direct) << "index " << usage.index;
                EXPECT_EQ(usage.color, palette[(size_t)usage.index]);
            }
        }
    }

    TEST(ApplyPalette, PhotoPatchNeedsPersonWithLostGroups) {
        const RgbaImage image = gradientImage(32, 32);
        const ApplyResult result = applyWith(image, {kBlack, kWhite, {255, 0, 0, 255}, {0, 0, 255, 255}});
        EXPECT_EQ(result.verdict, Verdict::NeedsPerson);
        EXPECT_GT(result.stats.maxDeltaE, 15.0);
        EXPECT_EQ(result.stats.colorsBefore, 32 * 32);
        EXPECT_LE(result.stats.colorsAfter, 4);
        ASSERT_FALSE(result.lost.empty());
        EXPECT_GE(result.lost.front().count, 16);
        int64_t total = 0;
        for (size_t i = 0; i < result.lost.size(); i++) {
            const LostGroup& group = result.lost[i];
            if (i > 0) {
                EXPECT_GE(result.lost[i - 1].count, group.count);
            }
            EXPECT_GE(group.count, 2);
            EXPECT_GE(group.x, 0);
            EXPECT_GE(group.y, 0);
            EXPECT_LE(group.x + group.w, 32);
            EXPECT_LE(group.y + group.h, 32);
            EXPECT_LE(group.count, group.w * group.h);
            EXPECT_TRUE(inPalette(group.mapped, result.palette));
            total += group.count;
        }
        EXPECT_LE(total, 32 * 32);
    }

    TEST(ApplyPalette, NearPaletteIsFixable) {
        const std::vector<Rgba8> palette = eightColors();
        const RgbaImage original = patternImage(12, 12, palette);
        RgbaImage nudged = original;
        const int spots[6][2] = {{0, 0}, {3, 1}, {5, 5}, {11, 2}, {7, 9}, {10, 11}};
        for (const auto& spot : spots) {
            Rgba8 color = nudged.at(spot[0], spot[1]);
            color.r = (uint8_t)(color.r > 250 ? color.r - 3 : color.r + 3);
            color.b = (uint8_t)(color.b > 250 ? color.b - 2 : color.b + 2);
            nudged.set(spot[0], spot[1], color);
        }
        const ApplyResult result = applyWith(nudged, palette);
        EXPECT_TRUE(result.image == original);
        EXPECT_EQ(result.stats.pixelsChanged, 6);
        EXPECT_GT(result.stats.maxDeltaE, 0.0);
        EXPECT_LE(result.stats.maxDeltaE, 15.0);
        EXPECT_GT(result.stats.meanDeltaE, 0.0);
        EXPECT_LT(result.stats.meanDeltaE, result.stats.maxDeltaE);
        EXPECT_TRUE(result.lost.empty());
        EXPECT_EQ(result.verdict, Verdict::Fixable);
    }

    TEST(ApplyPalette, LostGroupOfSixteenNeedsPerson) {
        const std::vector<Rgba8> palette = {kGrey99, kWhite};
        auto withBlock = [](int x0, int y0, int w, int h) {
            RgbaImage image(8, 8, kGrey99);
            for (int y = y0; y < y0 + h; y++) {
                for (int x = x0; x < x0 + w; x++) image.set(x, y, kGrey122);
            }
            return image;
        };

        const ApplyResult sixteen = applyWith(withBlock(2, 2, 4, 4), palette);
        EXPECT_EQ(sixteen.verdict, Verdict::NeedsPerson);
        EXPECT_GT(sixteen.stats.maxDeltaE, 6.0);
        EXPECT_LE(sixteen.stats.maxDeltaE, 15.0);
        ASSERT_EQ(sixteen.lost.size(), 1u);
        const LostGroup& group = sixteen.lost.front();
        EXPECT_EQ(group.count, 16);
        EXPECT_EQ(group.x, 2);
        EXPECT_EQ(group.y, 2);
        EXPECT_EQ(group.w, 4);
        EXPECT_EQ(group.h, 4);
        EXPECT_EQ(group.mapped, kGrey99);
        EXPECT_NEAR(group.original.r, 122, 1);
        EXPECT_NEAR(group.original.g, 122, 1);
        EXPECT_NEAR(group.original.b, 122, 1);
        EXPECT_EQ(sixteen.usage[0].count, 64);
        EXPECT_EQ(sixteen.usage[1].count, 0);

        const ApplyResult fifteen = applyWith(withBlock(1, 1, 3, 5), palette);
        EXPECT_EQ(fifteen.verdict, Verdict::Fixable);
        ASSERT_EQ(fifteen.lost.size(), 1u);
        EXPECT_EQ(fifteen.lost.front().count, 15);

        // two blocks that do not touch are two groups, the larger first
        RgbaImage apart = withBlock(0, 0, 2, 2);
        for (int y = 4; y < 7; y++) {
            for (int x = 4; x < 7; x++) apart.set(x, y, kGrey122);
        }
        const ApplyResult two = applyWith(apart, palette);
        ASSERT_EQ(two.lost.size(), 2u);
        EXPECT_EQ(two.lost[0].count, 9);
        EXPECT_EQ(two.lost[0].x, 4);
        EXPECT_EQ(two.lost[1].count, 4);
        EXPECT_EQ(two.lost[1].x, 0);

        // a lone lost pixel is noise: not listed
        const ApplyResult lone = applyWith(withBlock(3, 3, 1, 1), palette);
        EXPECT_TRUE(lone.lost.empty());
        EXPECT_EQ(lone.verdict, Verdict::Fixable);
    }

    TEST(ApplyPalette, ReduceSixteenKeepsLockedColoursAndIsDeterministic) {
        RgbaImage image(48, 48);
        for (int y = 0; y < 48; y++) {
            for (int x = 0; x < 48; x++) image.set(x, y, Rgba8{(uint8_t)(x * 5), (uint8_t)(y * 5), (uint8_t)((x * y) % 256), 255});
        }
        ApplyRequest request;
        request.palette = {{255, 0, 255, 255}, {18, 52, 86, 255}};
        request.locked = {0};
        request.reduce = 16;
        const ApplyResult first = applyPalette(image, request);
        const ApplyResult second = applyPalette(image, request);
        EXPECT_TRUE(first.image == second.image);
        EXPECT_EQ(first.palette, second.palette);
        ASSERT_FALSE(first.palette.empty());
        EXPECT_LE(first.palette.size(), 16u);
        EXPECT_EQ(first.palette[0], (Rgba8{255, 0, 255, 255}));
        EXPECT_LE(first.stats.colorsAfter, 16);
        for (const Rgba8& pixel : first.image.pixels) EXPECT_TRUE(inPalette(pixel, first.palette));
        EXPECT_EQ(first.usage.size(), first.palette.size());

        // out of range clamps to 2
        request.reduce = 1;
        request.locked.clear();
        EXPECT_LE(applyPalette(image, request).palette.size(), 2u);
    }

    TEST(ApplyPalette, ReduceToMoreColoursThanTheImageHasIsExact) {
        const std::vector<Rgba8> five = {kBlack, kWhite, {200, 30, 30, 255}, {30, 200, 30, 255}, {30, 30, 200, 255}};
        const RgbaImage image = patternImage(10, 10, five);
        ApplyRequest request;
        request.reduce = 16;
        const ApplyResult result = applyPalette(image, request);
        EXPECT_EQ(result.palette.size(), 5u);
        for (const Rgba8& color : five) EXPECT_TRUE(inPalette(color, result.palette));
        EXPECT_TRUE(result.image == image);
        EXPECT_EQ(result.verdict, Verdict::Clean);
    }

    TEST(ApplyPalette, BayerOnHalfGreyShowsHalfTheCellsWhite) {
        const std::vector<Rgba8> palette = {kBlack, kWhite};
        const px::PaletteMatcher matcher(palette, px::ColorMetric::OkLab);
        // the grey of linear 0.5 (sRGB 188): a dither mixes in linear light, so half the cells white
        const Rgba8 half{188, 188, 188, 255};
        const px::PaletteMatcher::Mix mix = matcher.ditherPair(half, 1.0f);
        ASSERT_EQ(mix.ratio16, 8);

        const RgbaImage image(16, 16, half);
        const struct { Dither dither; int size; } cases[] = {{Dither::Bayer2, 2}, {Dither::Bayer4, 4}, {Dither::Bayer8, 8}};
        for (const auto& c : cases) {
            SCOPED_TRACE(ditherName(c.dither));
            // the ratio in steps of the matrix's cells: half of them at every size
            const px::PaletteMatcher::Mix sized = matcher.ditherPair(half, 1.0f, c.size * c.size);
            ASSERT_EQ(sized.ratio16, c.size * c.size / 2);
            const ApplyResult result = applyWith(image, palette, c.dither);
            for (int by = 0; by < 16; by += c.size) {
                for (int bx = 0; bx < 16; bx += c.size) {
                    int white = 0;
                    for (int y = by; y < by + c.size; y++) {
                        for (int x = bx; x < bx + c.size; x++) {
                            const Rgba8 pixel = result.image.at(x, y);
                            EXPECT_TRUE(pixel == kWhite || pixel == kBlack);
                            if (pixel == kWhite) white++;
                            const bool showsB = px::bayerThreshold(c.size, x, y) < sized.ratio16 / (float)(c.size * c.size);
                            EXPECT_EQ(pixel, palette[showsB ? sized.b : sized.a]) << x << "," << y;
                        }
                    }
                    EXPECT_EQ(white, c.size * c.size / 2) << "block " << bx << "," << by;
                }
            }
        }

        // no strength, no dither: the nearest everywhere
        ApplyRequest request;
        request.palette = palette;
        request.dither = Dither::Bayer4;
        request.ditherStrength = 0.0f;
        const ApplyResult flat = applyPalette(image, request);
        const Rgba8 nearest = palette[(size_t)matcher.nearestUncached(half).first];
        for (const Rgba8& pixel : flat.image.pixels) EXPECT_EQ(pixel, nearest);
    }

    TEST(ApplyPalette, FloydSteinbergIsDeterministic) {
        RgbaImage image(32, 16);
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 32; x++) image.set(x, y, Rgba8{(uint8_t)(x * 8), (uint8_t)(x * 8), (uint8_t)(x * 8 + y / 4), 255});
        }
        const std::vector<Rgba8> palette = {kBlack, kWhite, {128, 128, 128, 255}};
        const ApplyResult first = applyWith(image, palette, Dither::FloydSteinberg);
        const ApplyResult second = applyWith(image, palette, Dither::FloydSteinberg);
        EXPECT_TRUE(first.image == second.image);
        for (const Rgba8& pixel : first.image.pixels) EXPECT_TRUE(inPalette(pixel, palette));

        const ApplyResult plain = applyWith(image, palette, Dither::None);
        EXPECT_FALSE(first.image == plain.image);

        ApplyRequest request;
        request.palette = palette;
        request.dither = Dither::FloydSteinberg;
        request.ditherStrength = 0.0f;
        EXPECT_TRUE(applyPalette(image, request).image == plain.image);
    }

    TEST(ApplyPalette, AlphaBinaryAtThreshold) {
        RgbaImage image(6, 1);
        const uint8_t alphas[6] = {0, 1, 127, 128, 200, 255};
        for (int x = 0; x < 6; x++) image.set(x, 0, Rgba8{255, 255, 255, alphas[x]});
        image.set(0, 0, Rgba8{10, 20, 30, 0});
        const std::vector<Rgba8> palette = {kBlack, kWhite};

        ApplyRequest request;
        request.palette = palette;
        request.alphaThreshold = 128;
        const ApplyResult binary = applyPalette(image, request);
        EXPECT_EQ(binary.image.at(0, 0), (Rgba8{10, 20, 30, 0}));
        EXPECT_EQ(binary.image.at(1, 0), (Rgba8{0, 0, 0, 0}));
        EXPECT_EQ(binary.image.at(2, 0), (Rgba8{0, 0, 0, 0}));
        EXPECT_EQ(binary.image.at(3, 0), kWhite);
        EXPECT_EQ(binary.image.at(4, 0), kWhite);
        EXPECT_EQ(binary.image.at(5, 0), kWhite);
        EXPECT_EQ(binary.stats.pixelsMapped, 3);
        EXPECT_EQ(binary.stats.pixelsChanged, 4);
        EXPECT_EQ(binary.stats.maxDeltaE, 0.0);
        EXPECT_EQ(binary.verdict, Verdict::Fixable);

        request.alphaThreshold = 0;
        const ApplyResult keep = applyPalette(image, request);
        EXPECT_TRUE(keep.image == image);
        EXPECT_EQ(keep.stats.pixelsMapped, 5);
        EXPECT_EQ(keep.verdict, Verdict::Clean);
    }

    TEST(ApplyPalette, RemapIndicesExactMatchesOnly) {
        const Rgba8 a{10, 20, 30, 255}, b{40, 50, 60, 0}, c{70, 80, 90, 255};
        const std::vector<Rgba8> from = {a, b, c, a};
        const std::vector<Rgba8> to = {{200, 0, 0, 255}, {0, 200, 0, 255}, {0, 0, 200, 128}, {9, 9, 9, 255}};
        RgbaImage image(7, 1);
        image.set(0, 0, a);
        image.set(1, 0, Rgba8{40, 50, 60, 200});    // a fully transparent source slot never matches
        image.set(2, 0, c);
        image.set(3, 0, Rgba8{70, 80, 90, 101});
        image.set(4, 0, Rgba8{11, 20, 30, 255});    // one off: untouched
        image.set(5, 0, Rgba8{10, 20, 30, 0});      // transparent still swaps, to alpha 0
        image.set(6, 0, Rgba8{1, 2, 3, 77});
        const RgbaImage out = remapIndices(image, from, to);
        EXPECT_EQ(out.at(0, 0), to[0]);             // the first of the duplicates wins
        EXPECT_EQ(out.at(1, 0), image.at(1, 0));
        EXPECT_EQ(out.at(2, 0), (Rgba8{0, 0, 200, 128}));
        EXPECT_EQ(out.at(3, 0), (Rgba8{0, 0, 200, 51}));    // round(101 x 128 / 255) = round(50.7)
        EXPECT_EQ(out.at(4, 0), image.at(4, 0));
        EXPECT_EQ(out.at(5, 0), (Rgba8{200, 0, 0, 0}));
        EXPECT_EQ(out.at(6, 0), image.at(6, 0));

        // an index past the end of the target keeps the source
        const std::vector<Rgba8> shortTo = {{200, 0, 0, 255}};
        const RgbaImage partial = remapIndices(image, from, shortTo);
        EXPECT_EQ(partial.at(0, 0), shortTo[0]);
        EXPECT_EQ(partial.at(2, 0), image.at(2, 0));

        // the alpha rule over every pair, against the rounded real product
        RgbaImage ramp(256, 1);
        for (int x = 0; x < 256; x++) ramp.set(x, 0, Rgba8{a.r, a.g, a.b, (uint8_t)x});
        for (int target = 0; target < 256; target += 17) {
            const std::vector<Rgba8> one = {Rgba8{1, 1, 1, (uint8_t)target}};
            const RgbaImage swapped = remapIndices(ramp, std::vector<Rgba8>{a}, one);
            for (int x = 0; x < 256; x++) {
                EXPECT_EQ(swapped.at(x, 0).a, (uint8_t)std::lround(x * target / 255.0)) << x << " x " << target;
            }
        }
    }

    TEST(ApplyPalette, Names) {
        for (Dither dither : {Dither::None, Dither::Bayer2, Dither::Bayer4, Dither::Bayer8, Dither::FloydSteinberg}) {
            Dither parsed = Dither::None;
            ASSERT_TRUE(parseDither(ditherName(dither), parsed));
            EXPECT_EQ(parsed, dither);
        }
        EXPECT_STREQ(ditherName(Dither::FloydSteinberg), "floyd_steinberg");
        Dither unchanged = Dither::Bayer2;
        EXPECT_FALSE(parseDither("bayer16", unchanged));
        EXPECT_EQ(unchanged, Dither::Bayer2);
        EXPECT_STREQ(verdictName(Verdict::Clean), "clean");
        EXPECT_STREQ(verdictName(Verdict::Fixable), "fixable");
        EXPECT_STREQ(verdictName(Verdict::NeedsPerson), "needs_person");
        EXPECT_EQ(verdictExitCode(Verdict::Clean), 0);
        EXPECT_EQ(verdictExitCode(Verdict::Fixable), 3);
        EXPECT_EQ(verdictExitCode(Verdict::NeedsPerson), 4);
    }

    TEST(ApplyPalette, EdgeCases) {
        const std::vector<Rgba8> palette = {kBlack, kWhite};

        const ApplyResult empty = applyWith(RgbaImage(), palette);
        EXPECT_TRUE(empty.image.empty());
        EXPECT_EQ(empty.verdict, Verdict::Clean);
        EXPECT_EQ(empty.usage.size(), 2u);

        const ApplyResult single = applyWith(RgbaImage(1, 1, Rgba8{250, 240, 245, 255}), palette, Dither::FloydSteinberg);
        EXPECT_EQ(single.image.at(0, 0), kWhite);
        EXPECT_EQ(single.stats.pixelsChanged, 1);
        EXPECT_EQ(single.verdict, Verdict::Fixable);

        const RgbaImage clear(4, 4, Rgba8{9, 9, 9, 0});
        const ApplyResult transparent = applyWith(clear, palette, Dither::Bayer4);
        EXPECT_TRUE(transparent.image == clear);
        EXPECT_EQ(transparent.stats.pixelsMapped, 0);
        EXPECT_EQ(transparent.stats.colorsBefore, 0);
        EXPECT_EQ(transparent.verdict, Verdict::Clean);

        const RgbaImage colours = patternImage(5, 5, eightColors());
        const ApplyResult noPalette = applyWith(colours, {});
        EXPECT_TRUE(noPalette.image == colours);
        EXPECT_EQ(noPalette.stats.pixelsMapped, 0);
        EXPECT_TRUE(noPalette.usage.empty());

        // a transparent slot is never matched, even by the colour it holds
        const std::vector<Rgba8> slotted = {{0, 0, 0, 0}, kWhite, {40, 40, 40, 255}};
        const ApplyResult slot = applyWith(RgbaImage(2, 2, kBlack), slotted);
        EXPECT_EQ(slot.image.at(0, 0), (Rgba8{40, 40, 40, 255}));
        EXPECT_EQ(slot.usage[0].count, 0);
        EXPECT_EQ(slot.usage[2].count, 4);

        // a duplicate colour: its first index takes the pixels
        const ApplyResult duplicate = applyWith(RgbaImage(3, 3, kWhite), {kWhite, kBlack, kWhite});
        EXPECT_EQ(duplicate.usage[0].count, 9);
        EXPECT_EQ(duplicate.usage[2].count, 0);
        EXPECT_EQ(duplicate.verdict, Verdict::Clean);

        // a buffer that does not match its size is refused, untouched
        RgbaImage broken(2, 2, kBlack);
        broken.pixels.pop_back();
        const ApplyResult refused = applyWith(broken, palette);
        EXPECT_TRUE(refused.image == broken);
        EXPECT_EQ(refused.verdict, Verdict::NeedsPerson);
    }

    TEST(RgbaImage, PngRoundTrip) {
        RgbaImage image(3, 2);
        image.set(0, 0, Rgba8{255, 0, 0, 255});
        image.set(1, 0, Rgba8{0, 255, 0, 128});
        image.set(2, 0, Rgba8{0, 0, 255, 1});
        image.set(0, 1, Rgba8{12, 34, 56, 0});
        image.set(1, 1, Rgba8{255, 255, 255, 255});
        image.set(2, 1, Rgba8{7, 8, 9, 200});

        const std::string png = encodeRgbaPng(image);
        ASSERT_FALSE(png.empty());
        std::string error;
        const std::optional<RgbaImage> decoded = decodeRgbaImage(png.data(), png.size(), &error);
        ASSERT_TRUE(decoded.has_value()) << error;
        EXPECT_TRUE(*decoded == image);

        const std::filesystem::path path = std::filesystem::temp_directory_path() / "je-applypalette-roundtrip.png";
        ASSERT_TRUE(saveRgbaPng(image, path));
        const std::optional<RgbaImage> loaded = loadRgbaImage(path, &error);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        ASSERT_TRUE(loaded.has_value()) << error;
        EXPECT_TRUE(*loaded == image);

        EXPECT_FALSE(loadRgbaImage(path, &error).has_value());
        EXPECT_FALSE(error.empty());
        const char garbage[] = "not an image at all";
        EXPECT_FALSE(decodeRgbaImage(garbage, sizeof(garbage), &error).has_value());
        EXPECT_TRUE(encodeRgbaPng(RgbaImage()).empty());
    }

    TEST(ApplyPalette, Benchmark1024With64Colours) {
        // photo-like: smooth, about 100,000 distinct colours
        RgbaImage image(1024, 1024);
        for (int y = 0; y < 1024; y++) {
            for (int x = 0; x < 1024; x++) {
                image.set(x, y, Rgba8{(uint8_t)(x >> 2), (uint8_t)(y >> 2), (uint8_t)((x + y) >> 3), 255});
            }
        }
        std::vector<Rgba8> palette;
        for (int r = 0; r < 4; r++) {
            for (int g = 0; g < 4; g++) {
                for (int b = 0; b < 4; b++) palette.push_back(Rgba8{(uint8_t)(r * 85), (uint8_t)(g * 85), (uint8_t)(b * 85), 255});
            }
        }
        const auto start = std::chrono::steady_clock::now();
        const ApplyResult result = applyWith(image, palette);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("[bench] apply_palette 1024x1024, 64 colours, oklab, %d distinct colours: %.1f ms (target <= 150 ms, R31)\n",
                    result.stats.colorsBefore, ms);
        EXPECT_EQ(result.stats.pixelsMapped, 1024 * 1024);
        EXPECT_LE(result.stats.colorsAfter, 64);
    }

}
