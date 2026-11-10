// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The rotation and scaler ports of edit_sprite (docs/specs/S47-agent-sprite-drawing.md R21, R22,
// AC5): 90-degree steps are exact permutations, the pixel-art rotations select only source
// pixels, their output matches goldens reviewed by eye once, and AdvMAME2x/3x follow the published
// rules.
//
// Goldens: editor/tests/pixelart/golden/rotate_<method>_<angle>.png, rewritten with
// JASPER_TEST_UPDATE_GOLDEN=1 (look at every changed image before committing it).

#include "pixelart/draw/SpriteRotate.h"
#include "pixelart/PixelImage.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::draw;

namespace {

    const RotateMethod kMethods[] = {RotateMethod::CleanEdge, RotateMethod::Rotxel, RotateMethod::OmniScale, RotateMethod::Nearest};

    const char* methodName(RotateMethod method) {
        switch (method) {
            case RotateMethod::CleanEdge: return "cleanedge";
            case RotateMethod::Rotxel: return "rotxel";
            case RotateMethod::OmniScale: return "omniscale";
            case RotateMethod::Nearest: return "nearest";
        }
        return "?";
    }

    // A 24 x 24 sword on a transparent canvas: outline, blade, edge, guard and grip (5 colours)
    PixelImage swordSprite() {
        static const char* rows[] = {
            "..............oo",
            ".............obo",
            "............obgo",
            "...........obgo.",
            "..........obgo..",
            ".........obgo...",
            "........obgo....",
            "...oo..obgo.....",
            "...oyoobgo......",
            "....oyygo.......",
            ".....oyyo.......",
            "....ohoyyo......",
            "...oho.oyyo.....",
            "..oho...oo......",
            ".oho............",
            ".oo.............",
        };
        PixelImage image(24, 24);
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                PixelRgba c{0, 0, 0, 0};
                switch (rows[y][x]) {
                    case 'o': c = {26, 28, 44, 255}; break;
                    case 'b': c = {244, 244, 244, 255}; break;
                    case 'g': c = {148, 176, 194, 255}; break;
                    case 'y': c = {255, 205, 117, 255}; break;
                    case 'h': c = {177, 62, 83, 255}; break;
                    default: break;
                }
                image.set(x + 4, y + 4, c);
            }
        }
        return image;
    }

    // A w x h image of a few colours placed by a fixed hash (every pixel tells its place apart)
    PixelImage patterned(int w, int h) {
        PixelImage image(w, h);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const uint32_t v = static_cast<uint32_t>(x * 73856093u) ^ static_cast<uint32_t>(y * 19349663u);
                image.set(x, y, PixelRgba{static_cast<uint8_t>(v % 5 * 60), static_cast<uint8_t>(v / 5 % 3 * 100), 40, 255});
            }
        }
        return image;
    }

    std::vector<int32_t> rotate(const PixelImage& image, double degrees, RotateMethod method) {
        return rotateMap(image.width(), image.height(), image.pixels(), degrees, (image.width() - 1) / 2.0, (image.height() - 1) / 2.0, method);
    }

    PixelImage apply(const PixelImage& source, const std::vector<int32_t>& map, int w, int h) {
        PixelImage out(w, h);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const int32_t i = map[static_cast<size_t>(y) * w + x];
                if (i >= 0) out.set(x, y, source.at(i % source.width(), i / source.width()));
            }
        }
        return out;
    }

    // map applied to a map: the composition of two turns
    std::vector<int32_t> compose(const std::vector<int32_t>& first, const std::vector<int32_t>& second) {
        std::vector<int32_t> out(second.size(), -1);
        for (size_t i = 0; i < second.size(); i++) {
            if (second[i] >= 0) out[i] = first[static_cast<size_t>(second[i])];
        }
        return out;
    }

    std::vector<int32_t> identity(size_t n) {
        std::vector<int32_t> out(n);
        for (size_t i = 0; i < n; i++) out[i] = static_cast<int32_t>(i);
        return out;
    }

    std::filesystem::path goldenDir() { return std::filesystem::path(JASPER_EDITOR_TEST_FIXTURES) / ".." / "pixelart" / "golden"; }

    bool updateGoldens() {
        const char* update = std::getenv("JASPER_TEST_UPDATE_GOLDEN");
        return update && std::string(update) == "1";
    }

    PixelEqual exact() {
        return [](int32_t a, int32_t b) { return a == b; };
    }

    // a w x h scaler input given as letters (one letter = one colour); equality compares letters
    struct LetterImage {
        int w = 0, h = 0;
        std::string cells;
        PixelEqual equal() const {
            return [this](int32_t a, int32_t b) { return cells[static_cast<size_t>(a)] == cells[static_cast<size_t>(b)]; };
        }
        // the scaled map as letters, one row per string
        std::vector<std::string> letters(const std::vector<int32_t>& map, int factor) const {
            std::vector<std::string> out;
            const int ow = w * factor;
            for (int y = 0; y < h * factor; y++) {
                std::string row;
                for (int x = 0; x < ow; x++) row += cells[static_cast<size_t>(map[static_cast<size_t>(y) * ow + x])];
                out.push_back(row);
            }
            return out;
        }
    };

}

TEST(SpriteRotate, QuarterTurnsArePermutations) {
    for (int side : {7, 8, 15, 16}) {
        const PixelImage image = patterned(side, side);
        for (RotateMethod method : kMethods) {
            for (double degrees : {90.0, 180.0, 270.0, -90.0, 450.0}) {
                const std::vector<int32_t> map = rotate(image, degrees, method);
                ASSERT_EQ(map.size(), static_cast<size_t>(side * side));
                std::vector<int> seen(map.size(), 0);
                for (int32_t i : map) {
                    ASSERT_GE(i, 0) << methodName(method) << " " << degrees << " on " << side;
                    seen[static_cast<size_t>(i)]++;
                }
                for (int count : seen) EXPECT_EQ(count, 1) << methodName(method) << " " << degrees << " on " << side;
            }
            // a clockwise quarter turn: the top-left pixel goes to the top-right corner
            const std::vector<int32_t> quarter = rotate(image, 90.0, method);
            EXPECT_EQ(quarter[static_cast<size_t>(side - 1)], 0) << methodName(method);
        }
    }
}

TEST(SpriteRotate, FourQuarterTurnsAreTheIdentity) {
    for (int side : {5, 6, 9, 32}) {
        const PixelImage image = patterned(side, side);
        for (RotateMethod method : kMethods) {
            const std::vector<int32_t> quarter = rotate(image, 90.0, method);
            std::vector<int32_t> total = identity(quarter.size());
            for (int i = 0; i < 4; i++) total = compose(total, quarter);
            EXPECT_EQ(total, identity(quarter.size())) << methodName(method) << " on " << side;
        }
    }
}

TEST(SpriteRotate, TwoHalfTurnsOfARectangleAreTheIdentity) {
    for (auto [w, h] : {std::pair{12, 5}, std::pair{7, 10}}) {
        const PixelImage image = patterned(w, h);
        for (RotateMethod method : kMethods) {
            const std::vector<int32_t> half = rotate(image, 180.0, method);
            EXPECT_EQ(compose(half, half), identity(half.size())) << methodName(method) << " on " << w << "x" << h;
        }
    }
}

TEST(SpriteRotate, ZeroDegreesIsTheIdentity) {
    const PixelImage image = swordSprite();
    for (RotateMethod method : kMethods) {
        EXPECT_EQ(rotate(image, 0.0, method), identity(24 * 24)) << methodName(method);
        EXPECT_EQ(rotate(image, 360.0, method), identity(24 * 24)) << methodName(method);
    }
}

TEST(SpriteRotate, ArbitraryAnglesSelectOnlySourcePixels) {
    const PixelImage image = swordSprite();
    for (RotateMethod method : kMethods) {
        for (double degrees : {30.0, 45.0, -17.5, 200.0}) {
            const std::vector<int32_t> map = rotate(image, degrees, method);
            ASSERT_EQ(map.size(), static_cast<size_t>(24 * 24));
            int drawn = 0;
            for (int32_t i : map) {
                ASSERT_GE(i, -1) << methodName(method) << " " << degrees;
                ASSERT_LT(i, 24 * 24) << methodName(method) << " " << degrees;
                if (i >= 0 && image.at(i % 24, i / 24)[3] != 0) drawn++;
            }
            // the sword stays about its size (no method loses or doubles the shape)
            EXPECT_GT(drawn, 60) << methodName(method) << " " << degrees;
            EXPECT_LT(drawn, 140) << methodName(method) << " " << degrees;
        }
    }
}

TEST(SpriteRotate, IsDeterministic) {
    const PixelImage image = swordSprite();
    for (RotateMethod method : kMethods) {
        EXPECT_EQ(rotate(image, 33.0, method), rotate(image, 33.0, method)) << methodName(method);
    }
}

TEST(SpriteRotate, MatchesTheGoldens) {
    const PixelImage image = swordSprite();
    for (RotateMethod method : kMethods) {
        for (int degrees : {30, 45}) {
            const PixelImage rotated = apply(image, rotate(image, degrees, method), 24, 24);
            const std::filesystem::path golden = goldenDir() / ("rotate_" + std::string(methodName(method)) + "_" + std::to_string(degrees) + ".png");
            if (updateGoldens()) {
                ASSERT_TRUE(rotated.savePng(golden)) << golden;
                continue;
            }
            std::string error;
            std::optional<PixelImage> expected = PixelImage::loadPng(golden, &error);
            ASSERT_TRUE(expected.has_value()) << golden << " is missing (" << error << "): run with JASPER_TEST_UPDATE_GOLDEN=1 and look at it";
            EXPECT_TRUE(rotated == *expected) << golden << " differs";
        }
    }
}

TEST(SpriteRotate, AdvMame2xFollowsThePublishedRules) {
    // a staircase: its steps become diagonals (expected values from the published rules, worked by hand)
    LetterImage stairs{4, 4, "a..."
                             "aa.."
                             ".aa."
                             "..aa"};
    const std::vector<std::string> got = stairs.letters(scaleMap(4, 4, 2, stairs.equal()), 2);
    const std::vector<std::string> want = {
        "aa......",
        "aaa.....",
        "aaa.....",
        "aaaaa...",
        ".aaaa...",
        "...aaaa.",
        "...aaaaa",
        "....aaaa",
    };
    EXPECT_EQ(got, want);
    // a lone pixel keeps its 2x2 block
    LetterImage lone{3, 3, "....x...."};
    EXPECT_EQ(lone.letters(scaleMap(3, 3, 2, lone.equal()), 2),
              (std::vector<std::string>{"......", "......", "..xx..", "..xx..", "......", "......"}));
}

TEST(SpriteRotate, AdvMame3xFollowsThePublishedRules) {
    LetterImage stairs{3, 3, "a.."
                             "aa."
                             ".aa"};
    const std::vector<std::string> got = stairs.letters(scaleMap(3, 3, 3, stairs.equal()), 3);
    const std::vector<std::string> want = {
        "aaa......",
        "aaaa.....",
        "aaaa.....",
        "aaaaa....",
        "aaaaaa...",
        "aaaaaaaa.",
        ".aaaaaaaa",
        "..aaaaaaa",
        "...aaaaaa",
    };
    EXPECT_EQ(got, want);
}

TEST(SpriteRotate, AdvMame3xPlacesEachBlockOnItsOwnPixel) {
    // Red control: Pixelorama's scale_3x writes each 3x3 block one pixel up and left (E0 at
    // (3x - 1, 3y - 1), E8 at (3x + 1, 3y + 1)), so the lone pixel's block would cover rows and
    // columns 2-4 and this expectation fails on its first row.
    LetterImage lone{3, 3, "....x...."};
    EXPECT_EQ(lone.letters(scaleMap(3, 3, 3, lone.equal()), 3),
              (std::vector<std::string>{".........", ".........", ".........", "...xxx...", "...xxx...", "...xxx...", ".........", ".........", "........."}));
    // E8 (the bottom-right sub-pixel) takes F when H == F, D != H and B != F: the corner pixel of
    // an L-shaped notch is filled with the right neighbour's colour
    LetterImage notch{3, 3, "..."
                            ".ab"
                            ".bb"};
    const std::vector<std::string> got = notch.letters(scaleMap(3, 3, 3, notch.equal()), 3);
    EXPECT_EQ(got[5][5], 'b');   // E8 of the centre pixel 'a'
    EXPECT_EQ(got[4][4], 'a');   // E4 stays 'a'
}

TEST(SpriteRotate, AdvMame4xIsAdvMame2xTwice) {
    LetterImage stairs{3, 3, "a.."
                             "aa."
                             ".aa"};
    const std::vector<int32_t> four = scaleMap(3, 3, 4, stairs.equal());
    ASSERT_EQ(four.size(), static_cast<size_t>(12 * 12));
    const std::vector<int32_t> two = scaleMap(3, 3, 2, stairs.equal());
    LetterImage doubled{6, 6, stairs.letters(two, 2)[0] + stairs.letters(two, 2)[1] + stairs.letters(two, 2)[2] + stairs.letters(two, 2)[3] +
                                  stairs.letters(two, 2)[4] + stairs.letters(two, 2)[5]};
    EXPECT_EQ(stairs.letters(four, 4), doubled.letters(scaleMap(6, 6, 2, doubled.equal()), 2));
}

TEST(SpriteRotate, ScalersHonourTheEqualityTheyAreGiven) {
    // two shades the caller calls equal (a tolerance) round a corner as one colour
    LetterImage shades{2, 2, "ab"
                             "b."};
    const PixelEqual loose = [&](int32_t a, int32_t b) {
        auto ink = [](char c) { return c == 'a' || c == 'b'; };
        return ink(shades.cells[static_cast<size_t>(a)]) == ink(shades.cells[static_cast<size_t>(b)]);
    };
    const std::vector<int32_t> strict = scaleMap(2, 2, 2, exact());
    const std::vector<int32_t> tolerant = scaleMap(2, 2, 2, loose);
    // pixel (1, 1): D = 'b' (index 2) and B = 'b' (index 1) are equal only under the tolerance
    EXPECT_EQ(strict[static_cast<size_t>(2 * 4 + 2)], 3);
    EXPECT_NE(tolerant[static_cast<size_t>(2 * 4 + 2)], 3);
}
