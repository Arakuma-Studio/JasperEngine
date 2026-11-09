// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S51 AC21: the input of S42's font_antialiasing rule.

#include "pixelart/BitmapFontPool.h"
#include "pixelart/FontPixelReport.h"
#include "TempDir.h"
#include "TestPaths.h"

#include <gtest/gtest.h>

#include <fstream>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    std::string fprFont(test::TempDir& dir, const std::string& name, const std::string& image, const std::string& extra){
        const std::string path = dir.str() + "/" + name;
        std::ofstream out(path);
        out << "{\"format\": \"jasperengine.bitmapfont\", \"source\": {\"type\": \"grid\", \"image\": \"" << test::testPaths().assetDir << "/fonts/"
            << image << "\"}" << extra << "}";
        return path;
    }

}

TEST(FontPixelReport, VectorFontIsAntialiased){
    const FontPixelReport roboto = analyzeFontForPixelArt("");   // the built-in vector font
    EXPECT_EQ(roboto.kind, FontKind::VECTOR);
    EXPECT_TRUE(roboto.antialiased);
    const FontPixelReport ttf = analyzeFontForPixelArt(test::testPaths().assetDir + "/fonts/pixel8.ttf");
    EXPECT_EQ(ttf.kind, FontKind::VECTOR);
    EXPECT_TRUE(ttf.antialiased);
    EXPECT_TRUE(ttf.loads);
    // a pixel TTF: its grid is detected, the fix is a .bitmapfont over it
    EXPECT_TRUE(ttf.pixelGridDetected);
    EXPECT_EQ(ttf.nativeSize, 8);
}

TEST(FontPixelReport, CleanGridFontAndPartialAlpha){
    BitmapFontPool::clear();
    test::TempDir dir;
    const FontPixelReport clean = analyzeFontForPixelArt(fprFont(dir, "clean.bitmapfont", "grid@6x9.png", ""));
    EXPECT_EQ(clean.kind, FontKind::BITMAP);
    EXPECT_TRUE(clean.loads);
    EXPECT_FALSE(clean.antialiased);
    EXPECT_EQ(clean.partialAlphaShare, 0.0f);
    EXPECT_EQ(clean.nativeSize, 9);

    const FontPixelReport sloppy = analyzeFontForPixelArt(fprFont(dir, "sloppy.bitmapfont", "partial@6x9.png", ""));
    EXPECT_TRUE(sloppy.antialiased);
    EXPECT_GT(sloppy.partialAlphaShare, 0.0f);
    EXPECT_LT(sloppy.partialAlphaShare, 1.0f);

    const FontPixelReport cut = analyzeFontForPixelArt(fprFont(dir, "cut.bitmapfont", "partial@6x9.png", ", \"alpha_cutoff\": 128"));
    EXPECT_FALSE(cut.antialiased);
    EXPECT_EQ(cut.partialAlphaShare, 0.0f);

    const FontPixelReport broken = analyzeFontForPixelArt(dir.str() + "/missing.bitmapfont");
    EXPECT_FALSE(broken.loads);
    ASSERT_FALSE(broken.errors.empty());
    EXPECT_EQ(broken.errors[0].kind, "font_file_missing");
    BitmapFontPool::clear();
}

TEST(FontPixelReport, TextSizeMultiplesAndVectorFallbacks){
    BitmapFontPool::clear();
    const TextPixelReport ok = analyzeTextForPixelArt({kBuiltinPixelFont}, 18);
    EXPECT_EQ(ok.pixelScale, 2);
    EXPECT_EQ(ok.effectiveFontSize, 18);
    EXPECT_TRUE(ok.sizeIsMultiple);
    const TextPixelReport off = analyzeTextForPixelArt({kBuiltinPixelFont, "fonts/Smooth.ttf"}, 16);
    EXPECT_EQ(off.pixelScale, 2);
    EXPECT_FALSE(off.sizeIsMultiple);
    ASSERT_EQ(off.vectorFallbacks.size(), 1u);
    EXPECT_EQ(off.vectorFallbacks[0], "fonts/Smooth.ttf");
    const TextPixelReport vector = analyzeTextForPixelArt({""}, 16);
    EXPECT_EQ(vector.pixelScale, 0);
    EXPECT_EQ(vector.font.kind, FontKind::VECTOR);
    BitmapFontPool::clear();
}
