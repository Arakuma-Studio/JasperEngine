// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// PoseImage (spec: pose animation phase 3, section 3): drawing, the bitmap font, the contact
// sheet layout and the PNG round trip.

#include "editor/pose/PoseImage.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace jasperengine::editor;

namespace {

    const PoseColor BLACK(0, 0, 0, 255);
    const PoseColor WHITE(255, 255, 255, 255);
    const PoseColor RED(255, 0, 0, 255);

    bool glyphBit(char c, int col, int row){
        return (PoseImage::glyphRows(c)[row] & (0x20 >> col)) != 0;
    }

}

TEST(PoseImage, CreateFillAndPixels){
    PoseImage image(4, 3, RED);
    EXPECT_EQ(image.getWidth(), 4);
    EXPECT_EQ(image.getHeight(), 3);
    EXPECT_EQ(image.getPixels().size(), 4u * 3u * 4u);
    EXPECT_EQ(image.getPixel(3, 2), RED);
    // outside reads as transparent and writes nothing
    EXPECT_EQ(image.getPixel(4, 0), PoseColor(0, 0, 0, 0));
    image.setPixel(-1, 0, WHITE);
    image.setPixel(1, 1, WHITE);
    EXPECT_EQ(image.getPixel(1, 1), WHITE);
    image.fill(BLACK);
    EXPECT_EQ(image.getPixel(1, 1), BLACK);

    PoseImage empty(0, 5);
    EXPECT_TRUE(empty.isEmpty());
    EXPECT_EQ(PoseColor::hex(0xFF7A45), PoseColor(0xFF, 0x7A, 0x45, 255));
}

TEST(PoseImage, BlendSourceOver){
    PoseImage image(1, 1, PoseColor(0, 0, 255, 255));
    image.blendPixel(0, 0, PoseColor(255, 0, 0, 128));
    const PoseColor c = image.getPixel(0, 0);
    EXPECT_NEAR(c.r, 128, 1);
    EXPECT_EQ(c.g, 0);
    EXPECT_NEAR(c.b, 127, 1);
    EXPECT_EQ(c.a, 255);

    // onto a transparent pixel the colour is kept and the alpha is the source's
    PoseImage clear(1, 1);
    clear.blendPixel(0, 0, PoseColor(10, 20, 30, 128));
    EXPECT_EQ(clear.getPixel(0, 0), PoseColor(10, 20, 30, 128));
}

TEST(PoseImage, HorizontalLineCoversItsWidth){
    PoseImage image(20, 12, BLACK);
    image.drawLine(2.0f, 5.0f, 14.0f, 5.0f, 2.0f, WHITE);
    // a 2 px line on y = 5 covers rows 4 and 5 fully, nothing above or below
    for (int x = 3; x <= 13; x++){
        EXPECT_EQ(image.getPixel(x, 4), WHITE) << x;
        EXPECT_EQ(image.getPixel(x, 5), WHITE) << x;
        EXPECT_EQ(image.getPixel(x, 3), BLACK) << x;
        EXPECT_EQ(image.getPixel(x, 6), BLACK) << x;
    }
    // round caps end about a radius past the end points
    EXPECT_EQ(image.getPixel(17, 5), BLACK);
    EXPECT_EQ(image.getPixel(0, 5), BLACK);
}

TEST(PoseImage, DiagonalLineIsAntiAliased){
    PoseImage image(20, 20, BLACK);
    image.drawLine(2.0f, 3.0f, 17.0f, 12.0f, 1.5f, WHITE);
    int full = 0;
    int partial = 0;
    for (int y = 0; y < 20; y++){
        for (int x = 0; x < 20; x++){
            const PoseColor c = image.getPixel(x, y);
            EXPECT_EQ(c.r, c.g);
            EXPECT_EQ(c.a, 255);
            if (c.r == 255) full++;
            else if (c.r > 0) partial++;
        }
    }
    EXPECT_GT(full, 0);
    EXPECT_GT(partial, 0);
    // far from the segment nothing is drawn
    EXPECT_EQ(image.getPixel(17, 2), BLACK);
    EXPECT_EQ(image.getPixel(2, 17), BLACK);
}

TEST(PoseImage, NonFiniteInputsDrawNothing){
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    PoseImage image(16, 16, BLACK);
    const std::vector<uint8_t> before = image.getPixels();
    image.drawLine(nan, 2.0f, 10.0f, 10.0f, 2.0f, WHITE);
    image.drawLine(2.0f, 2.0f, inf, 10.0f, 2.0f, WHITE);
    image.drawLine(2.0f, 2.0f, 10.0f, -inf, 2.0f, WHITE);
    image.drawLine(2.0f, 2.0f, 10.0f, 10.0f, nan, WHITE);
    image.drawLine(2.0f, 2.0f, 10.0f, 10.0f, inf, WHITE);
    image.fillCircle(nan, 8.0f, 4.0f, WHITE);
    image.fillCircle(8.0f, inf, 4.0f, WHITE);
    image.fillCircle(8.0f, 8.0f, inf, WHITE);
    image.fillCircle(8.0f, 8.0f, nan, WHITE);
    EXPECT_EQ(image.getPixels(), before);
    // huge but finite coordinates clip safely (no int overflow in the bounding box)
    image.drawLine(-1e30f, 8.0f, 1e30f, 8.0f, 2.0f, WHITE);
    image.fillCircle(1e30f, -1e30f, 1e30f, WHITE);
    image.drawLine(-1e4f, 8.0f, 1e4f, 8.0f, 2.0f, WHITE);
    EXPECT_EQ(image.getPixel(8, 8), WHITE);
}

TEST(PoseImage, FilledCircle){
    PoseImage image(20, 20, BLACK);
    image.fillCircle(10.0f, 10.0f, 6.0f, RED);
    EXPECT_EQ(image.getPixel(9, 9), RED);
    EXPECT_EQ(image.getPixel(10, 10), RED);
    EXPECT_EQ(image.getPixel(8, 10), RED);    // centre (8.5, 10.5): 1.6 from the centre, radius 3
    EXPECT_EQ(image.getPixel(14, 10), BLACK); // centre 14.5: 4.5 away
    EXPECT_EQ(image.getPixel(10, 14), BLACK);
    // the rim is blended
    const PoseColor rim = image.getPixel(12, 8);   // centre (12.5, 8.5): 2.9 away
    EXPECT_GT(rim.r, 0);
    EXPECT_LT(rim.r, 255);
}

TEST(PoseImage, FillAndStrokeRect){
    PoseImage image(10, 10, BLACK);
    image.fillRect(-2, -2, 5, 4, RED);
    EXPECT_EQ(image.getPixel(0, 0), RED);
    EXPECT_EQ(image.getPixel(2, 1), RED);
    EXPECT_EQ(image.getPixel(3, 1), BLACK);
    EXPECT_EQ(image.getPixel(2, 2), BLACK);

    image.strokeRect(4, 4, 4, 4, WHITE);
    EXPECT_EQ(image.getPixel(4, 4), WHITE);
    EXPECT_EQ(image.getPixel(7, 7), WHITE);
    EXPECT_EQ(image.getPixel(5, 5), BLACK);
}

TEST(PoseImage, BlitClipsAndCrops){
    PoseImage src(4, 4, RED);
    src.setPixel(3, 3, WHITE);
    PoseImage dst(6, 6, BLACK);
    dst.blit(src, -2, -2);
    EXPECT_EQ(dst.getPixel(0, 0), RED);
    EXPECT_EQ(dst.getPixel(1, 1), WHITE);
    EXPECT_EQ(dst.getPixel(2, 2), BLACK);

    dst.blit(src, 4, 4);
    EXPECT_EQ(dst.getPixel(5, 5), RED);

    // a source rectangle
    PoseImage part(6, 6, BLACK);
    part.blit(src, 2, 2, 2, 2, 0, 0);
    EXPECT_EQ(part.getPixel(0, 0), RED);
    EXPECT_EQ(part.getPixel(1, 1), WHITE);
    EXPECT_EQ(part.getPixel(2, 2), BLACK);

    PoseImage cropped = src.crop(2, 2, 4, 4);
    EXPECT_EQ(cropped.getWidth(), 4);
    EXPECT_EQ(cropped.getPixel(1, 1), WHITE);
    EXPECT_EQ(cropped.getPixel(2, 2), PoseColor(0, 0, 0, 0));
}

TEST(PoseImage, GlyphTableShapes){
    // 'A': rounded top on row 1, crossbar on row 4, legs down to row 7
    EXPECT_FALSE(glyphBit('A', 0, 1));
    EXPECT_TRUE(glyphBit('A', 1, 1));
    EXPECT_TRUE(glyphBit('A', 3, 1));
    for (int col = 0; col < 5; col++){
        EXPECT_TRUE(glyphBit('A', col, 4)) << col;
    }
    EXPECT_TRUE(glyphBit('A', 0, 7));
    EXPECT_TRUE(glyphBit('A', 4, 7));
    // every glyph leaves the gap column and row 0 empty, and the space is blank
    for (int c = 32; c <= 126; c++){
        const uint8_t* rows = PoseImage::glyphRows((char)c);
        EXPECT_EQ(rows[0], 0) << (char)c;
        for (int row = 0; row < PoseImage::GLYPH_HEIGHT; row++){
            EXPECT_EQ(rows[row] & 0x01, 0) << (char)c;
            if (c == ' '){
                EXPECT_EQ(rows[row], 0);
            }
        }
    }
    // printable glyphs other than the space all draw something
    for (int c = 33; c <= 126; c++){
        const uint8_t* rows = PoseImage::glyphRows((char)c);
        int bits = 0;
        for (int row = 0; row < PoseImage::GLYPH_HEIGHT; row++) bits |= rows[row];
        EXPECT_NE(bits, 0) << (char)c;
    }
    // descenders reach rows 8-9, unprintable bytes draw as '?'
    EXPECT_NE(PoseImage::glyphRows('g')[9], 0);
    EXPECT_EQ(PoseImage::glyphRows('\x01'), PoseImage::glyphRows('?'));
    EXPECT_EQ(PoseImage::glyphRows('\n'), PoseImage::glyphRows('?'));
}

TEST(PoseImage, DrawTextPixels){
    PoseImage image(40, 12, BLACK);
    const int end = image.drawText(2, 1, "AL", WHITE);
    EXPECT_EQ(end, 2 + 2 * PoseImage::GLYPH_WIDTH);
    // 'A' at (2, 1): every cell pixel matches the glyph table
    for (int row = 0; row < PoseImage::GLYPH_HEIGHT; row++){
        for (int col = 0; col < PoseImage::GLYPH_WIDTH; col++){
            const PoseColor expected = glyphBit('A', col, row) ? WHITE : BLACK;
            EXPECT_EQ(image.getPixel(2 + col, 1 + row), expected) << col << "," << row;
        }
    }
    // 'L' next to it: the vertical stroke in column 0, the foot on row 7
    EXPECT_EQ(image.getPixel(8, 2), WHITE);
    EXPECT_EQ(image.getPixel(9, 2), BLACK);
    for (int col = 0; col < 5; col++){
        EXPECT_EQ(image.getPixel(8 + col, 8), WHITE) << col;
    }
    // clipped text writes nothing outside
    image.drawText(36, 1, "WWW", WHITE);
    EXPECT_EQ(image.getPixel(36, 2), WHITE);
    EXPECT_EQ(image.getPixel(39, 2), BLACK);
}

TEST(PoseImage, TextWidthAndFit){
    EXPECT_EQ(PoseImage::textWidth(""), 0);
    EXPECT_EQ(PoseImage::textWidth("abc"), 18);
    EXPECT_EQ(PoseImage::fitText("abcdef", 36), "abcdef");
    EXPECT_EQ(PoseImage::fitText("abcdefghij", 36), "abcd..");
    EXPECT_EQ(PoseImage::fitText("abcdefghij", 40), "abcd..");
    EXPECT_EQ(PoseImage::fitText("abcdefghij", 12), "..");
    EXPECT_LE(PoseImage::textWidth(PoseImage::fitText("a long pose name here", 50)), 50);
}

TEST(PoseImage, SheetLayoutSizes){
    PoseSheetLayout layout;
    layout.columns = 3;
    layout.rows = 2;
    layout.cellWidth = 160;
    layout.cellHeight = 200;
    EXPECT_EQ(layout.labelHeight, 14);
    EXPECT_EQ(layout.getWidth(), 3 * 160 + 2);
    EXPECT_EQ(layout.getHeight(), 2 * (200 + 14) + 1);
    EXPECT_EQ(layout.cellX(0), 0);
    EXPECT_EQ(layout.cellX(2), 322);
    EXPECT_EQ(layout.cellY(1), 215);
    EXPECT_EQ(layout.labelY(0), 200);

    layout.titleHeight = 14;
    EXPECT_EQ(layout.getHeight(), 15 + 2 * (200 + 14) + 1);
    EXPECT_EQ(layout.cellY(0), 15);

    PoseSheetLayout single;
    single.cellWidth = 64;
    single.cellHeight = 80;
    single.labelHeight = 0;
    EXPECT_EQ(single.getWidth(), 64);
    EXPECT_EQ(single.getHeight(), 80);
}

TEST(PoseImage, ContactSheetComposition){
    PoseSheetLayout layout;
    layout.columns = 2;
    layout.rows = 2;
    layout.cellWidth = 40;
    layout.cellHeight = 30;
    PoseContactSheet sheet(layout);
    const PoseImage& image = sheet.getImage();
    ASSERT_EQ(image.getWidth(), 81);
    ASSERT_EQ(image.getHeight(), 2 * 44 + 1);

    // a larger cell is clipped to its slot: the separators stay black
    sheet.setCell(0, 0, PoseImage(50, 50, RED));
    sheet.setCell(1, 1, PoseImage(40, 30, WHITE));
    EXPECT_EQ(image.getPixel(0, 0), RED);
    EXPECT_EQ(image.getPixel(39, 29), RED);
    EXPECT_EQ(image.getPixel(40, 0), PoseContactSheet::SEPARATOR_COLOR);
    EXPECT_EQ(image.getPixel(41, 45), WHITE);
    EXPECT_EQ(image.getPixel(10, 44), PoseContactSheet::SEPARATOR_COLOR);   // between the rows
    EXPECT_EQ(image.getPixel(1, 30), PoseContactSheet::LABEL_BACKGROUND);   // label strip
    EXPECT_EQ(image.getPixel(1, 43), PoseContactSheet::LABEL_BACKGROUND);

    // label text: white glyphs from (3, labelY + 2)
    sheet.setLabel(0, 0, "A");
    const int textX = PoseContactSheet::LABEL_PADDING;
    const int textY = layout.labelY(0) + 2;
    EXPECT_EQ(image.getPixel(textX + 1, textY + 1), PoseContactSheet::LABEL_TEXT);
    EXPECT_EQ(image.getPixel(textX + 0, textY + 1), PoseContactSheet::LABEL_BACKGROUND);
    EXPECT_EQ(image.getPixel(textX + 0, textY + 4), PoseContactSheet::LABEL_TEXT);

    // a long label stays inside its strip
    sheet.setLabel(0, 1, "a very long label that does not fit");
    for (int y = layout.labelY(1); y < layout.labelY(1) + 14; y++){
        EXPECT_EQ(image.getPixel(40, y), PoseContactSheet::SEPARATOR_COLOR);
    }

    PoseSheetLayout titled = layout;
    titled.titleHeight = 14;
    PoseContactSheet withTitle(titled, "Title");
    EXPECT_EQ(withTitle.getImage().getHeight(), image.getHeight() + 15);
    EXPECT_EQ(withTitle.getImage().getPixel(60, 0), PoseContactSheet::LABEL_BACKGROUND);
    EXPECT_EQ(withTitle.getImage().getPixel(60, 14), PoseContactSheet::SEPARATOR_COLOR);
}

TEST(PoseImage, LabelTexts){
    EXPECT_EQ(PoseContactSheet::poseLabel(3, "crouch"), "3 crouch");
    EXPECT_EQ(PoseContactSheet::poseLabel(0, ""), "0");
    EXPECT_EQ(PoseContactSheet::timeLabel(0.5f), "t=0.50");
    EXPECT_EQ(PoseContactSheet::timeLabel(-0.001f), "t=0.00");
    EXPECT_EQ(PoseContactSheet::timeLabel(1.234f, true, false), "t=1.23 [L]");
    EXPECT_EQ(PoseContactSheet::timeLabel(2.0f, false, true), "t=2.00 [R]");
    EXPECT_EQ(PoseContactSheet::timeLabel(0.25f, true, true), "t=0.25 [LR]");
}

TEST(PoseImage, PngRoundTrip){
    PoseImage image(23, 17);
    for (int y = 0; y < 17; y++){
        for (int x = 0; x < 23; x++){
            image.setPixel(x, y, PoseColor((uint8_t)(x * 11), (uint8_t)(y * 15), (uint8_t)((x * y) & 0xFF), (uint8_t)(255 - x * 3)));
        }
    }
    image.drawText(1, 1, "Pose", WHITE);

    std::vector<uint8_t> png;
    ASSERT_TRUE(image.encodePng(png));
    ASSERT_GT(png.size(), 8u);
    EXPECT_EQ(png[1], 'P');
    EXPECT_EQ(png[2], 'N');
    EXPECT_EQ(png[3], 'G');

    // same pixels, same bytes
    std::vector<uint8_t> again;
    ASSERT_TRUE(image.encodePng(again));
    EXPECT_EQ(png, again);

    PoseImage decoded;
    ASSERT_TRUE(PoseImage::decodePng(png.data(), png.size(), decoded));
    EXPECT_EQ(decoded.getWidth(), 23);
    EXPECT_EQ(decoded.getHeight(), 17);
    EXPECT_EQ(decoded.getPixels(), image.getPixels());

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "jasper_pose_image_test.png";
    ASSERT_TRUE(image.writePng(path.string()));
    PoseImage fromFile;
    ASSERT_TRUE(PoseImage::readPng(path.string(), fromFile));
    EXPECT_EQ(fromFile.getPixels(), image.getPixels());
    std::filesystem::remove(path);

    PoseImage empty;
    EXPECT_FALSE(empty.encodePng(png));
    const uint8_t garbage[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    EXPECT_FALSE(PoseImage::decodePng(garbage, sizeof(garbage), decoded));
    EXPECT_TRUE(decoded.isEmpty());
    EXPECT_FALSE(PoseImage::readPng("/nonexistent/dir/none.png", decoded));
}
