// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/FontFace.h"

// The fixtures are a fontTools 4.61.1 subset of fonts/Roboto-Regular-subset-post.ttf,
// packaged as TTF, WOFF and WOFF2 (Brotli 1.2.0); their font licence is beside them.
// The decoding itself is the editor's (editor/titleart/fonts/WebFontDecode, its own tests): here
// the engine's side, with a stand-in decoder that returns the TTF.

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

using namespace jasperengine;

namespace {

    std::vector<unsigned char> webFontFixture(const std::string& extension){
        std::ifstream input(std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/fixtures/fonts/webfonts/webfont." + extension,
                            std::ios::binary);
        return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    std::shared_ptr<FontFace> webFontLoad(const std::vector<unsigned char>& bytes, std::string* error = nullptr){
        return FontFace::loadMemory(bytes.data(), bytes.size(), 0, false, "web-font regression", error);
    }

    // players run without a decoder: every test leaves it that way
    struct WebFontDecoderScope {
        explicit WebFontDecoderScope(FontFace::WebFontDecoder decoder){ FontFace::setWebFontDecoder(std::move(decoder)); }
        ~WebFontDecoderScope(){ FontFace::setWebFontDecoder(nullptr); }
    };

}

TEST(WebFonts, DecodedBytesBelongToTheFaceAndKeepSfntMetricsAndGlyphPixels){
    const auto sfntBytes = webFontFixture("ttf");
    ASSERT_FALSE(sfntBytes.empty());
    const auto sfnt = webFontLoad(sfntBytes);
    ASSERT_NE(sfnt, nullptr);
    std::vector<size_t> decodedLengths;
    WebFontDecoderScope scope([&](const unsigned char*, size_t length, std::vector<unsigned char>& out, std::string&){
        decodedLengths.push_back(length);
        out = sfntBytes;
        return true;
    });
    for (const std::string extension : {"woff", "woff2"}){
        SCOPED_TRACE(extension);
        auto compressed = webFontFixture(extension);
        ASSERT_FALSE(compressed.empty());
        std::string error;
        auto font = webFontLoad(compressed, &error);
        ASSERT_NE(font, nullptr) << error;
        ASSERT_FALSE(decodedLengths.empty());
        EXPECT_EQ(decodedLengths.back(), compressed.size());
        // Decoded bytes belong to the face, including loadMemory(copy = false).
        std::fill(compressed.begin(), compressed.end(), 0);
        EXPECT_EQ(font->getFamilyName(), sfnt->getFamilyName());
        EXPECT_EQ(font->getWeight(), sfnt->getWeight());
        EXPECT_EQ(font->getUnitsPerEm(), sfnt->getUnitsPerEm());
        EXPECT_EQ(font->getAscender(), sfnt->getAscender());
        for (uint32_t codepoint : {0x41u, 0x61u, 0x7Au, 0xE9u, 0xF1u, 0x303u}){
            SCOPED_TRACE(codepoint);
            const auto glyph = font->getGlyphIndex(codepoint);
            ASSERT_NE(glyph, 0u);
            EXPECT_EQ(glyph, sfnt->getGlyphIndex(codepoint));
            const float scale = sfnt->scaleForEmSize(48);
            std::vector<unsigned char> expected, actual;
            int ew = 0, eh = 0, ex = 0, ey = 0, aw = 0, ah = 0, ax = 0, ay = 0;
            ASSERT_TRUE(sfnt->rasterizeGlyph(glyph, scale, expected, ew, eh, ex, ey));
            ASSERT_TRUE(font->rasterizeGlyph(glyph, scale, actual, aw, ah, ax, ay));
            EXPECT_EQ(aw, ew);
            EXPECT_EQ(ah, eh);
            EXPECT_EQ(actual, expected);
        }
    }
    // a plain sfnt never reaches the decoder
    decodedLengths.clear();
    EXPECT_NE(webFontLoad(sfntBytes), nullptr);
    EXPECT_TRUE(decodedLengths.empty());
}

TEST(WebFonts, WithoutADecoderAWebFontIsRefusedNamingTheExport){
    for (const std::string extension : {"woff", "woff2"}){
        SCOPED_TRACE(extension);
        std::string error;
        EXPECT_EQ(webFontLoad(webFontFixture(extension), &error), nullptr);
        EXPECT_NE(error.find(extension == "woff" ? "is WOFF," : "is WOFF2,"), std::string::npos) << error;
        EXPECT_NE(error.find("export again"), std::string::npos) << error;
    }
}

TEST(WebFonts, ADecoderRefusalNamesTheFormatAndItsReason){
    WebFontDecoderScope scope([](const unsigned char*, size_t, std::vector<unsigned char>&, std::string& why){
        why = "it is truncated";
        return false;
    });
    for (const std::string extension : {"woff", "woff2"}){
        SCOPED_TRACE(extension);
        std::string error;
        EXPECT_EQ(webFontLoad(webFontFixture(extension), &error), nullptr);
        EXPECT_NE(error.find(std::string("Failed to decode ") + (extension == "woff" ? "WOFF" : "WOFF2") + " font"), std::string::npos) << error;
        EXPECT_NE(error.find("it is truncated"), std::string::npos) << error;
    }
}
