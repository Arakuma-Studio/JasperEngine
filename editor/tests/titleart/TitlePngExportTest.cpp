// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The PNG side of Title Art's exports (docs/specs/S87-title-localisation-interchange.md R37, D7,
// D8, AC19): the band encoder (Sub rows, parallel bands, combined Adler-32: the same bytes for any
// thread count; the decode equals the pixels, 8- and 16-bit), the sRGB chunks in order and no
// iCCP, pHYs from the dpi, the recipe; colour bleed under transparent pixels against the dark
// fringe of zeroed RGB (its red control), and premultiplied alpha. The renders of an export
// (trim, parts, languages, ZIP) are tools/mcp/tests/test_title_export_png.py's.

#include <gtest/gtest.h>

#include "texsynth/SynthImage.h"
#include "titleart/TitleArtBake.h"
#include "titleart/interchange/TitleExport.h"
#include "titleart/interchange/png/PngBandEncoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    texsynth::Image gradient(int w, int h, int bitDepth) {
        texsynth::Image image = texsynth::Image::make(w, h, 4, bitDepth);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint8_t* p = image.pixel(x, y);
                const uint32_t v[4] = {(uint32_t)(x * 7 + y), (uint32_t)(y * 3), (uint32_t)((x ^ y) & 255), (uint32_t)((x + y) % 256)};
                for (int c = 0; c < 4; c++) {
                    if (bitDepth == 16) reinterpret_cast<uint16_t*>(p)[c] = (uint16_t)(v[c] * 257 + (c == 0 ? x : 0));
                    else p[c] = (uint8_t)v[c];
                }
            }
        }
        return image;
    }

    // the chunk types of a PNG in order
    std::vector<std::string> chunks(const std::vector<uint8_t>& png) {
        std::vector<std::string> out;
        size_t at = 8;
        while (at + 8 <= png.size()) {
            const uint32_t length = (uint32_t)png[at] << 24 | (uint32_t)png[at + 1] << 16 | (uint32_t)png[at + 2] << 8 | png[at + 3];
            out.emplace_back(reinterpret_cast<const char*>(png.data() + at + 4), 4);
            at += 12 + length;
        }
        return out;
    }

    // a white disc on transparent, the RGB under alpha 0 as `bleed` leaves it
    texsynth::Image disc(int size, bool bleed) {
        texsynth::Image image = texsynth::Image::make(size, size, 4, 8);
        const double r = size * 0.3, c = size * 0.5;
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                const double d = std::hypot(x + 0.5 - c, y + 0.5 - c);
                const double a = std::clamp(r + 0.5 - d, 0.0, 1.0);
                uint8_t* p = image.pixel(x, y);
                p[3] = (uint8_t)std::lround(a * 255);
                const uint8_t v = p[3] ? 255 : 0;
                p[0] = p[1] = p[2] = v;
            }
        }
        if (bleed) bleedAlpha(image, 8);
        return image;
    }

    // 4x box-downsampled with straight alpha (what bilinear mips of a straight texture do), then
    // over mid grey: how far the edge falls below the correct (premultiplied) composite
    double edgeDip(const texsynth::Image& image) {
        const int w = image.width / 4;
        double worst = 0;
        for (int y = 0; y < w; y++) {
            for (int x = 0; x < w; x++) {
                double rgb[3] = {0, 0, 0}, premultiplied[3] = {0, 0, 0}, a = 0;
                for (int j = 0; j < 4; j++) {
                    for (int i = 0; i < 4; i++) {
                        const uint8_t* p = image.pixel(x * 4 + i, y * 4 + j);
                        for (int c = 0; c < 3; c++) {
                            rgb[c] += p[c];
                            premultiplied[c] += p[c] * (p[3] / 255.0);
                        }
                        a += p[3];
                    }
                }
                a /= 16 * 255.0;
                if (a <= 0.02 || a >= 0.98) continue;
                const double straight = rgb[0] / 16 * a + 128 * (1 - a);
                const double correct = premultiplied[0] / 16 + 128 * (1 - a);
                worst = std::max(worst, correct - straight);
            }
        }
        return worst;
    }

}

TEST(TitlePngExport, BandsGiveTheSameBytesForAnyThreadCount) {
    for (int depth : {8, 16}) {
        const texsynth::Image image = gradient(300, 700, depth);
        TitlePngOptions serial, parallel;
        serial.parallel = false;
        std::vector<uint8_t> a, b;
        std::string error;
        ASSERT_TRUE(encodeTitlePng(image, serial, a, error)) << error;
        ASSERT_TRUE(encodeTitlePng(image, parallel, b, error)) << error;
        EXPECT_EQ(a, b) << depth << "-bit";
        std::vector<uint8_t> again;
        ASSERT_TRUE(encodeTitlePng(image, parallel, again, error));
        EXPECT_EQ(a, again) << "two exports byte-identical";
        // the decode equals the pixels
        texsynth::Image decoded;
        ASSERT_TRUE(texsynth::loadImageFromMemory(a.data(), a.size(), decoded, error)) << error;
        EXPECT_EQ(decoded.bitDepth, depth);
        EXPECT_EQ(decoded.width, 300);
        EXPECT_EQ(decoded.data, image.data) << depth << "-bit";
    }
}

TEST(TitlePngExport, ChunksTagSrgb) {
    TitlePngOptions o;
    o.dpi = 300;
    o.recipeKeyword = "jasper.titleart.export";
    o.recipeText = "{\"language\":\"de\"}";
    std::vector<uint8_t> png;
    std::string error;
    ASSERT_TRUE(encodeTitlePng(gradient(16, 16, 8), o, png, error));
    EXPECT_EQ(chunks(png), std::vector<std::string>({"IHDR", "sRGB", "gAMA", "cHRM", "pHYs", "iTXt", "IDAT", "IEND"}));
    const std::string bytes(png.begin(), png.end());
    EXPECT_EQ(bytes.find("iCCP"), std::string::npos) << "never iCCP beside sRGB";
    // gAMA 45455 and pHYs at 300 dpi (11811 px/m)
    const size_t gama = bytes.find("gAMA");
    EXPECT_EQ(((uint8_t)bytes[gama + 4] << 24 | (uint8_t)bytes[gama + 5] << 16 | (uint8_t)bytes[gama + 6] << 8 | (uint8_t)bytes[gama + 7]), 45455);
    const size_t phys = bytes.find("pHYs");
    EXPECT_EQ(((uint8_t)bytes[phys + 4] << 24 | (uint8_t)bytes[phys + 5] << 16 | (uint8_t)bytes[phys + 6] << 8 | (uint8_t)bytes[phys + 7]), 11811);
    EXPECT_NE(bytes.find("{\"language\":\"de\"}"), std::string::npos);
}

TEST(TitlePngExport, AdlerCombines) {
    const std::string a = "the quick brown fox", b = " jumps over the lazy dog";
    const std::string ab = a + b;
    const uint32_t whole = titleAdler32(reinterpret_cast<const uint8_t*>(ab.data()), ab.size());
    const uint32_t first = titleAdler32(reinterpret_cast<const uint8_t*>(a.data()), a.size());
    const uint32_t second = titleAdler32(reinterpret_cast<const uint8_t*>(b.data()), b.size());
    EXPECT_EQ(titleAdler32Combine(first, second, b.size()), whole);
    EXPECT_EQ(titleAdler32(nullptr, 0), 1u);
}

// D8: bled colour leaves no dark fringe once the image is filtered; zeroed RGB (Pachislop's
// output, alpha_bleed false) is the red control
TEST(TitlePngExport, BleedRemovesTheDarkFringe) {
    const texsynth::Image bled = disc(128, true);
    const texsynth::Image zeroed = disc(128, false);
    EXPECT_LE(edgeDip(bled), 1.0);
    EXPECT_GE(edgeDip(zeroed), 40.0);
    // every alpha-0 pixel near the disc holds the colour of its nearest visible pixel
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128; x++) {
            const uint8_t* p = bled.pixel(x, y);
            const double d = std::hypot(x + 0.5 - 64, y + 0.5 - 64) - 128 * 0.3;
            if (p[3] == 0 && d < 7) EXPECT_EQ(p[0], 255) << x << "," << y;
        }
    }
}

TEST(TitlePngExport, PremultipliedInTheEncodedDomain) {
    TitleExportContext ctx;
    ctx.image.alpha = "premultiplied";
    texsynth::Image image = texsynth::Image::make(2, 1, 4, 8);
    const uint8_t pixels[8] = {200, 100, 50, 128, 255, 255, 255, 0};
    std::memcpy(image.data.data(), pixels, 8);
    ctx.finishPixels(image);
    EXPECT_EQ(image.pixel(0, 0)[0], (200 * 128 + 127) / 255);
    EXPECT_EQ(image.pixel(0, 0)[3], 128);
    EXPECT_EQ(image.pixel(1, 0)[0], 0) << "colour x 0";
    // alpha_bleed false writes zeros under alpha 0
    TitleExportContext zeros;
    zeros.image.alphaBleed = false;
    texsynth::Image other = texsynth::Image::make(2, 1, 4, 8);
    std::memcpy(other.data.data(), pixels, 8);
    zeros.finishPixels(other);
    EXPECT_EQ(other.pixel(1, 0)[0], 0);
    EXPECT_EQ(other.pixel(0, 0)[0], 200);
}
