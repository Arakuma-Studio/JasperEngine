// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S47 AC1 (R6-R8): the grid alphabet and encodings. Red control: tenten's alphabet, index 1 written
// as '0' (.0123...), fails IndexIsItsOwnDigit's first assertion.

#include "SpriteDrawTestSupport.h"

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::draw;
using jasperengine::editor::pixelart::testing::json;

namespace {

    std::vector<uint8_t> paletteOf(int n) {
        std::vector<uint8_t> out(static_cast<size_t>(n) * 4);
        for (int i = 0; i < n; i++) {
            out[static_cast<size_t>(i) * 4] = static_cast<uint8_t>(i);
            out[static_cast<size_t>(i) * 4 + 3] = 255;
        }
        return out;
    }

    PixelImage strip(const std::vector<uint8_t>& indices, int paletteSize) {
        return PixelImage::fromIndices(static_cast<int>(indices.size()), 1, indices, paletteOf(paletteSize), 0);
    }

}

TEST(GridCodec, IndexIsItsOwnDigit) {
    EXPECT_EQ(alphabetChar(1), '1');
    EXPECT_EQ(alphabetChar(9), '9');
    EXPECT_EQ(alphabetChar(10), 'a');
    EXPECT_EQ(alphabetChar(35), 'z');
    EXPECT_EQ(alphabetChar(36), 'A');
    EXPECT_EQ(alphabetChar(61), 'Z');
    EXPECT_EQ(alphabetChar(0), '.');
    for (int i = 0; i <= 61; i++) EXPECT_EQ(alphabetIndex(alphabetChar(i)), i) << i;
}

TEST(GridCodec, ZeroAndDotBothDecodeToZero) {
    EXPECT_EQ(alphabetIndex('.'), 0);
    EXPECT_EQ(alphabetIndex('0'), 0);
    PixelPatch patch;
    PixelProblem problem;
    const std::vector<std::string> rows = {".01"};
    ASSERT_TRUE(decodeRows(rows, GridEncoding::Chars, nullptr, true, patch, problem));
    EXPECT_EQ(patch.index[0], 0);
    EXPECT_EQ(patch.index[1], 0);
    EXPECT_EQ(patch.index[2], 1);
    EXPECT_EQ(patch.keep[0], 0);
    EXPECT_EQ(patch.keep[1], 0);
}

TEST(GridCodec, Index62ForcesHex2) {
    const PixelImage img = strip({0, 61}, 64);
    const EncodedGrid chars = encodeRegion(img, img.bounds(), GridEncoding::Auto, paletteOf(64), 0);
    EXPECT_EQ(chars.encoding, GridEncoding::Chars);
    EXPECT_EQ(chars.rows[0], ".Z");
    const PixelImage wide = strip({0, 62}, 64);
    const EncodedGrid hex = encodeRegion(wide, wide.bounds(), GridEncoding::Auto, paletteOf(64), 0);
    EXPECT_EQ(hex.encoding, GridEncoding::Hex2);
    EXPECT_EQ(hex.rows[0], "003e");
}

TEST(GridCodec, DashAndSpaceKeepAndShortRowsKeepTheRest) {
    PixelPatch patch;
    PixelProblem problem;
    const std::vector<std::string> rows = {"1-2", " 3"};
    ASSERT_TRUE(decodeRows(rows, GridEncoding::Chars, nullptr, true, patch, problem));
    ASSERT_EQ(patch.w, 3);
    ASSERT_EQ(patch.h, 2);
    EXPECT_EQ(patch.keep, (std::vector<uint8_t>{0, 1, 0, 1, 0, 1}));
    EXPECT_EQ(patch.index[2], 2);
    EXPECT_EQ(patch.index[4], 3);
}

TEST(GridCodec, Hex2KeepsWithDoubleDash) {
    PixelPatch patch;
    PixelProblem problem;
    const std::vector<std::string> rows = {"--ff0a"};
    ASSERT_TRUE(decodeRows(rows, GridEncoding::Hex2, nullptr, true, patch, problem));
    EXPECT_EQ(patch.keep, (std::vector<uint8_t>{1, 0, 0}));
    EXPECT_EQ(patch.index[1], 255);
    EXPECT_EQ(patch.index[2], 10);
}

TEST(GridCodec, PasteOverKeepsIndexZero) {
    draw::SpriteDoc sprite = testing::makeSprite(2, 1);
    S47_APPLY(sprite, json::array({{{"op", "pixel"}, {"points", {{0, 0}, {1, 0}}}, {"color", 2}}}));
    S47_APPLY(sprite, json::array({{{"op", "paste"}, {"rows", {".3"}}, {"blend", "over"}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], "23");
    S47_APPLY(sprite, json::array({{{"op", "paste"}, {"rows", {".-"}}}}));
    EXPECT_EQ(testing::canvasRows(sprite)[0], ".3");
}

TEST(GridCodec, LegendAssignsCharactersInFirstAppearanceOrder) {
    PixelImage image(3, 2);
    image.set(0, 0, {10, 20, 30, 255});
    image.set(1, 0, {1, 2, 3, 255});
    image.set(2, 0, {10, 20, 30, 255});
    image.set(1, 1, {9, 8, 7, 128});
    const EncodedGrid grid = encodeRegion(image, image.bounds(), GridEncoding::Auto, {}, -1);
    EXPECT_EQ(grid.encoding, GridEncoding::Legend);
    EXPECT_EQ(grid.rows[0], "121");
    EXPECT_EQ(grid.rows[1], ".3.");
    EXPECT_EQ(grid.colors["1"], "#0a141eff");
    EXPECT_EQ(grid.colors["2"], "#010203ff");
    EXPECT_EQ(grid.colors["3"], "#09080780");
}

TEST(GridCodec, LegendSwitchesToHex8At62Colors) {
    PixelImage few(61, 1), many(62, 1);
    for (int i = 0; i < 61; i++) few.set(i, 0, {static_cast<uint8_t>(i + 1), 0, 0, 255});
    for (int i = 0; i < 62; i++) many.set(i, 0, {static_cast<uint8_t>(i + 1), 0, 0, 255});
    EXPECT_EQ(encodeRegion(few, few.bounds(), GridEncoding::Auto, {}, -1).encoding, GridEncoding::Legend);
    const EncodedGrid grid = encodeRegion(many, many.bounds(), GridEncoding::Auto, {}, -1);
    EXPECT_EQ(grid.encoding, GridEncoding::Hex8);
    EXPECT_EQ(grid.rows[0].size(), 62u * 8u);
    EXPECT_EQ(grid.rows[0].substr(0, 8), "010000ff");
}

TEST(GridCodec, BadCharacterReportsRowColumnAndValidSet) {
    PixelPatch patch;
    PixelProblem problem;
    const std::vector<std::string> rows = {"11", "1!"};
    ASSERT_FALSE(decodeRows(rows, GridEncoding::Chars, nullptr, true, patch, problem));
    EXPECT_EQ(problem.kind, "bad_grid_char");
    EXPECT_EQ(problem.field, "rows[1]");
    EXPECT_NE(problem.message.find("column 1"), std::string::npos) << problem.message;
    EXPECT_NE(problem.fix.find("a-z"), std::string::npos) << problem.fix;
}

TEST(GridCodec, UsedListsOnlyTheRegionsIndices) {
    const PixelImage img = strip({0, 3, 3, 5}, 6);
    const EncodedGrid grid = encodeRegion(img, {1, 0, 2, 1}, GridEncoding::Auto, paletteOf(6), 0);
    EXPECT_EQ(grid.colors.size(), 1u);
    EXPECT_TRUE(grid.colors.contains("3"));
}
