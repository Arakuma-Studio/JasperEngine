// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S48 R27: the contact sheet's layout, backdrop bands, labels and integer scale

#include "pixelart/fx/ContactSheet.h"

#include <gtest/gtest.h>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::fx;

namespace {

    std::vector<PixelImage> dots(int n, int w, int h) {
        std::vector<PixelImage> frames;
        for (int i = 0; i < n; i++) {
            PixelImage f(w, h);
            f.set(i % w, h / 2, PixelRgba{255, 0, 0, 255});
            frames.push_back(f);
        }
        return frames;
    }

    std::vector<const PixelImage*> view(const std::vector<PixelImage>& frames) {
        std::vector<const PixelImage*> v;
        for (const PixelImage& f : frames) v.push_back(&f);
        return v;
    }

}

TEST(ContactSheet, TimeLabels) {
    EXPECT_EQ(timeLabel(0.25), "t=0.25s");
    EXPECT_EQ(timeLabel(0.375), "t=0.375s");
    EXPECT_EQ(timeLabel(1.0), "t=1s");
    EXPECT_EQ(timeLabel(0.0), "t=0s");
}

TEST(ContactSheet, ScaleIsTheLargestIntegerThatFits) {
    SheetSettings s;
    s.maxWidth = 1024;
    // 8 frames of 16 px: 8 * 16 * k + 7 <= 1024 -> k = 7
    SheetLayout l = layoutSheet(16, 16, 8, s);
    EXPECT_EQ(l.columns, 8);
    EXPECT_EQ(l.scale, 7);
    EXPECT_EQ(l.width, 8 * 16 * 7 + 7);
    // a given scale is kept; columns shrink to fit
    s.scale = 10;
    l = layoutSheet(16, 16, 8, s);
    EXPECT_EQ(l.scale, 10);
    EXPECT_EQ(l.columns, 6);   // 6 * 160 + 5 = 965
    EXPECT_EQ(l.rows, 2);
    // the cap is 16
    s.scale = 0;
    s.maxWidth = 100000;
    EXPECT_EQ(layoutSheet(16, 16, 4, s).scale, 16);
    // a frame wider than max_width stays at scale 1 in one column
    s.maxWidth = 100;
    l = layoutSheet(300, 20, 4, s);
    EXPECT_EQ(l.scale, 1);
    EXPECT_EQ(l.columns, 1);
}

TEST(ContactSheet, BandsPerBackdropWithLabelsAndSeparators) {
    const std::vector<PixelImage> frames = dots(5, 12, 10);
    SheetSettings s;
    s.backdrops = {Backdrop::Dark, Backdrop::Light, Backdrop::Black};
    s.columns = 3;
    s.scale = 4;
    SheetLayout l;
    const PixelImage sheet = composeSheet(view(frames), {0, 0.25, 0.5, 0.75, 1.0}, s, &l);
    EXPECT_EQ(l.bands, 3);
    EXPECT_EQ(l.rows, 2);
    EXPECT_EQ(l.cellHeight, 10 * 4 + SheetLayout::kLabelHeight);
    EXPECT_EQ(sheet.width(), 3 * 48 + 2);
    EXPECT_EQ(sheet.height(), 3 * l.bandHeight + 2);
    // each band's backdrop under an empty pixel: dark #15131d, light #ebe8f0, black
    const PixelRgba expected[3] = {{0x15, 0x13, 0x1d, 255}, {0xeb, 0xe8, 0xf0, 255}, {0, 0, 0, 255}};
    for (int b = 0; b < 3; b++) {
        const IntPoint o = l.frameOrigin(b, 0);
        EXPECT_EQ(sheet.at(o.x + 47, o.y), expected[b]) << b;
        // the red dot of frame 0 at (0, 5), 4x4 sheet pixels, every one the same colour
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) EXPECT_EQ(sheet.at(o.x + x, o.y + 20 + y), (PixelRgba{255, 0, 0, 255}));
        // the separator column right of cell 0
        EXPECT_EQ(sheet.at(o.x + 48, o.y), (PixelRgba{0, 0, 0, 255}));
    }
    // the label strip under frame 1 has text pixels
    const IntPoint o1 = l.frameOrigin(0, 1);
    int text = 0;
    for (int y = 0; y < SheetLayout::kLabelHeight; y++)
        for (int x = 0; x < l.cellWidth; x++) text += sheet.at(o1.x + x, o1.y + 40 + y) == PixelRgba{255, 255, 255, 255} ? 1 : 0;
    EXPECT_GT(text, 20);
    // deterministic
    EXPECT_EQ(sheet.hash(), composeSheet(view(frames), {0, 0.25, 0.5, 0.75, 1.0}, s).hash());
    // labels off: no strip
    s.labels = false;
    EXPECT_EQ(layoutSheet(12, 10, 5, s).cellHeight, 40);
}

TEST(ContactSheet, PremultipliedFramesComposeOverTheBackdrop) {
    PixelImage f(2, 1);
    f.set(0, 0, PixelRgba{100, 0, 0, 128});   // half-covered red, premultiplied
    SheetSettings s;
    s.backdrops = {Backdrop::Black};
    s.scale = 1;
    s.labels = false;
    SheetLayout l;
    const PixelImage sheet = composeSheet({&f}, {0}, s, &l);
    const IntPoint o = l.frameOrigin(0, 0);
    EXPECT_EQ(sheet.at(o.x, o.y), (PixelRgba{100, 0, 0, 255}));
    s.backdrops = {Backdrop::Light};
    const PixelImage light = composeSheet({&f}, {0}, s, &l);
    EXPECT_EQ(light.at(o.x, o.y)[0], 100 + (0xeb * 127 + 127) / 255);
    // scene mode: the frames are the view, one band
    s.opaqueFrames = true;
    s.backdrops = {Backdrop::Dark, Backdrop::Light};
    EXPECT_EQ(layoutSheet(2, 1, 1, s).bands, 1);
}

TEST(ContactSheet, BackdropsAreDeterministic) {
    for (Backdrop b : {Backdrop::Checker, Backdrop::Dark, Backdrop::Light, Backdrop::Sky, Backdrop::Ground, Backdrop::Black, Backdrop::Scene}) {
        Backdrop parsed;
        ASSERT_TRUE(parseBackdrop(backdropName(b), parsed));
        EXPECT_EQ(parsed, b);
        EXPECT_EQ(backdropImage(b, 40, 30, 20, {1, 2, 3, 255}).hash(), backdropImage(b, 40, 30, 20, {1, 2, 3, 255}).hash());
    }
    Backdrop none;
    EXPECT_FALSE(parseBackdrop("grass", none));
    // the ground's first row is the floor's edge colour
    EXPECT_EQ(backdropImage(Backdrop::Ground, 40, 30, 20, {}).at(5, 20), (PixelRgba{0x4a, 0x42, 0x63, 255}));
    EXPECT_EQ(backdropImage(Backdrop::Ground, 40, 30, 20, {}).at(5, 19), (PixelRgba{0x15, 0x13, 0x1d, 255}));
}

TEST(ContactSheet, SheetTimes) {
    EXPECT_EQ(sheetTimes(5, 1.0, false), (std::vector<double>{0, 0.25, 0.5, 0.75, 1.0}));
    EXPECT_EQ(sheetTimes(4, 1.0, true), (std::vector<double>{0, 0.25, 0.5, 0.75}));
    EXPECT_EQ(sheetTimes(1, 2.0, false), (std::vector<double>{0}));
}
