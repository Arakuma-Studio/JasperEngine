// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/FontFace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>

using namespace jasperengine;

namespace {

    std::vector<unsigned char> webFontFixture(const std::string& extension){
        std::ifstream input(std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/fixtures/fonts/webfonts/webfont." + extension,
                            std::ios::binary);
        return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    uint32_t webFontU32(const unsigned char* p){
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }

    void webFontPut32(std::vector<unsigned char>& bytes, size_t at, uint32_t value){
        for (size_t i = 0; i < 4; i++)
            bytes[at + i] = (unsigned char)(value >> (24 - 8 * i));
    }

    std::shared_ptr<FontFace> webFontLoad(const std::vector<unsigned char>& bytes, std::string* error = nullptr){
        return FontFace::loadMemory(bytes.data(), bytes.size(), 0, false, "web-font regression", error);
    }

}

TEST(WebFonts, WoffAndWoff2KeepSfntMetricsAndGlyphPixels){
    const auto sfntBytes = webFontFixture("ttf");
    ASSERT_FALSE(sfntBytes.empty());
    const auto sfnt = webFontLoad(sfntBytes);
    ASSERT_NE(sfnt, nullptr);
    for (const std::string extension : {"woff", "woff2"}){
        SCOPED_TRACE(extension);
        auto compressed = webFontFixture(extension);
        ASSERT_FALSE(compressed.empty());
        std::string error;
        auto font = webFontLoad(compressed, &error);
        ASSERT_NE(font, nullptr) << error;
        // Decoded bytes belong to the face, including loadMemory(copy = false).
        std::fill(compressed.begin(), compressed.end(), 0);
        EXPECT_EQ(font->getFamilyName(), sfnt->getFamilyName());
        EXPECT_EQ(font->getWeight(), sfnt->getWeight());
        EXPECT_EQ(font->getUnitsPerEm(), sfnt->getUnitsPerEm());
        EXPECT_EQ(font->getAscender(), sfnt->getAscender());
        EXPECT_EQ(font->getDescender(), sfnt->getDescender());
        EXPECT_EQ(font->getLineGap(), sfnt->getLineGap());
        for (uint32_t codepoint : {0x41u, 0x61u, 0x7Au, 0xE9u, 0xF1u, 0x303u}){
            SCOPED_TRACE(codepoint);
            const auto glyph = font->getGlyphIndex(codepoint);
            ASSERT_NE(glyph, 0u);
            EXPECT_EQ(glyph, sfnt->getGlyphIndex(codepoint));
            const float scale = sfnt->scaleForEmSize(48);
            EXPECT_EQ(font->getGlyphAdvance(glyph, scale), sfnt->getGlyphAdvance(glyph, scale));
            std::vector<unsigned char> expected, actual;
            int ew = 0, eh = 0, ex = 0, ey = 0, aw = 0, ah = 0, ax = 0, ay = 0;
            ASSERT_TRUE(sfnt->rasterizeGlyph(glyph, scale, expected, ew, eh, ex, ey));
            ASSERT_TRUE(font->rasterizeGlyph(glyph, scale, actual, aw, ah, ax, ay));
            EXPECT_EQ(aw, ew);
            EXPECT_EQ(ah, eh);
            EXPECT_EQ(ax, ex);
            EXPECT_EQ(ay, ey);
            EXPECT_EQ(actual, expected);
        }
    }
}

TEST(WebFonts, RejectsTruncatedFilesAndReportsTheFormat){
    for (const std::string extension : {"woff", "woff2"}){
        const auto original = webFontFixture(extension);
        ASSERT_GT(original.size(), 48u);
        for (size_t length : {size_t(4), size_t(12), size_t(43), original.size() / 2, original.size() - 1}){
            SCOPED_TRACE(extension + " at " + std::to_string(length));
            const std::vector<unsigned char> bytes(original.begin(), original.begin() + length);
            std::string error;
            EXPECT_EQ(webFontLoad(bytes, &error), nullptr);
            EXPECT_NE(error.find(extension == "woff" ? "WOFF" : "WOFF2"), std::string::npos);
        }
    }
}

TEST(WebFonts, RejectsOversizedDecodedFontsBeforeAllocation){
    for (const std::string extension : {"woff", "woff2"}){
        auto bytes = webFontFixture(extension);
        ASSERT_GT(bytes.size(), 48u);
        webFontPut32(bytes, 16, 64 * 1024 * 1024 + 4);
        std::string error;
        EXPECT_EQ(webFontLoad(bytes, &error), nullptr);
        EXPECT_NE(error.find("64 MiB"), std::string::npos);
    }
}

TEST(WebFonts, RejectsInvalidWoffDirectoryAndCorruptTables){
    const auto original = webFontFixture("woff");
    ASSERT_GT(original.size(), 84u);
    auto rejected = [&](size_t at, uint32_t value){
        auto bytes = original;
        webFontPut32(bytes, at, value);
        EXPECT_EQ(webFontLoad(bytes), nullptr) << "field at " << at;
    };
    rejected(8, (uint32_t)original.size() + 1);             // total length
    rejected(12, 0x00010001);                             // reserved bits
    rejected(16, webFontU32(original.data() + 16) + 4);    // SFNT length
    rejected(48, 44);                                     // table overlaps directory
    rejected(52, webFontU32(original.data() + 56) + 1);    // compressed > original
    rejected(64, webFontU32(original.data() + 44));        // duplicate table tag
    rejected(68, webFontU32(original.data() + 48));        // overlapping tables
    rejected(60, webFontU32(original.data() + 60) ^ 1);    // table checksum
    const size_t tables = (size_t(original[12]) << 8) | original[13];
    bool compressedTable = false;
    for (size_t i = 0; i < tables; i++){
        const size_t entry = 44 + i * 20;
        const size_t packed = webFontU32(original.data() + entry + 8);
        if (packed >= webFontU32(original.data() + entry + 12))
            continue;
        auto bytes = original;
        const size_t at = webFontU32(original.data() + entry + 4);
        bytes[at + packed - 1] ^= 1;                      // zlib Adler32
        EXPECT_EQ(webFontLoad(bytes), nullptr);
        compressedTable = true;
        break;
    }
    EXPECT_TRUE(compressedTable);
}
