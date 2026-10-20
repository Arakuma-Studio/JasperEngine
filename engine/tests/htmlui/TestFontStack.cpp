// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The shared text stack under STBText and the HTML UI font engine: FontFace, FontRegistry,
// TextShaper, GlyphAtlas. CPU only.

#include "harness/TestSystem.h"

#include "DefaultFont.h"
#include "io/Data.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"
#include "util/GlyphAtlas.h"
#include "util/TextShaper.h"

#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::vector<unsigned char> readFile(const std::string& path){
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK)
            return {};
        return std::vector<unsigned char>(data.getMemPtr(), data.getMemPtr() + data.length());
    }

    void writeU32(unsigned char* p, uint32_t value){
        p[0] = (unsigned char)(value >> 24); p[1] = (unsigned char)(value >> 16);
        p[2] = (unsigned char)(value >> 8); p[3] = (unsigned char)value;
    }

    uint32_t readU32(const unsigned char* p){
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }

    // a TrueType collection holding the same font `count` times (table offsets moved past the header)
    std::vector<unsigned char> makeCollection(const std::vector<unsigned char>& font, uint32_t count){
        const uint32_t header = 12 + 4 * count;
        std::vector<unsigned char> out(header, 0);
        std::memcpy(out.data(), "ttcf", 4);
        writeU32(&out[4], 0x00010000);
        writeU32(&out[8], count);
        for (uint32_t i = 0; i < count; i++)
            writeU32(&out[12 + 4 * i], header);
        out.insert(out.end(), font.begin(), font.end());

        unsigned char* sfnt = &out[header];
        uint32_t numTables = (uint32_t)((sfnt[4] << 8) | sfnt[5]);
        for (uint32_t i = 0; i < numTables; i++){
            unsigned char* record = sfnt + 12 + 16 * i;
            writeU32(record + 8, readU32(record + 8) + header);
        }
        return out;
    }

    std::shared_ptr<FontFace> robotoCopy(const std::string& label){
        return FontFace::loadMemory(roboto_regular_ttf, roboto_regular_ttf_len, 0, false, label);
    }

    struct Chain {
        std::vector<std::shared_ptr<FontFace>> owned;
        std::vector<const FontFace*> faces;
        std::vector<float> scales;

        void add(const std::shared_ptr<FontFace>& face, float scale){
            owned.push_back(face);
            faces.push_back(face.get());
            scales.push_back(scale);
        }
    };

    // the chain STBText builds without a project font, at its pixel-height scale
    Chain builtInChain(float pixelHeight){
        Chain chain;
        std::shared_ptr<FontFace> latin = FontRegistry::getBuiltInLatin();
        std::shared_ptr<FontFace> arabic = FontRegistry::getBuiltInArabic();
        chain.add(latin, latin->scaleForPixelHeight(pixelHeight));
        chain.add(arabic, arabic->scaleForPixelHeight(pixelHeight));
        return chain;
    }

    std::vector<uint32_t> decode(const std::string& text){
        std::vector<uint32_t> codepoints;
        TextShaper::decodeUtf8(text.data(), text.size(), codepoints);
        return codepoints;
    }

    std::vector<ShapedGlyph> shape(const Chain& chain, const std::string& text, const ShapeOptions& options = ShapeOptions()){
        std::vector<uint32_t> codepoints = decode(text);
        std::vector<ShapedGlyph> out;
        TextShaper::shapeRange(chain.faces, chain.scales, codepoints, 0, codepoints.size(), 0, options, out);
        return out;
    }

    float measure(const Chain& chain, const std::string& text, const ShapeOptions& options = ShapeOptions()){
        std::vector<uint32_t> codepoints = decode(text);
        return TextShaper::measure(chain.faces, chain.scales, codepoints, 0, codepoints.size(), options);
    }

    const char* arabicWord = "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7";   // مرحبا
    const char* lamAlef = "\xd9\x84\xd8\xa7";                             // لا

    class FontRegistryTest : public ::testing::Test {
    protected:
        void SetUp() override{ FontRegistry::clear(); }
        void TearDown() override{ FontRegistry::clear(); }
    };

}

// ---------------------------------------------------------------------------------------
// FontFace

TEST(FontFaceTest, RobotoMetadata){
    std::shared_ptr<FontFace> roboto = FontRegistry::getBuiltInLatin();
    ASSERT_NE(roboto, nullptr);
    EXPECT_EQ(roboto->getFamilyName(), "Roboto");
    EXPECT_EQ(roboto->getWeight(), 400);
    EXPECT_FALSE(roboto->isItalic());
    EXPECT_EQ(roboto->getUnitsPerEm(), 2048);
    EXPECT_FALSE(roboto->hasPostTable());
    EXPECT_EQ(roboto->getUnderlinePosition(), 0);
    EXPECT_EQ(roboto->getUnderlineThickness(), 0);
    EXPECT_GT(roboto->getAscender(), 0);
    EXPECT_LT(roboto->getDescender(), 0);
    EXPECT_GT(roboto->getXHeight(), 0);
    EXPECT_LT(roboto->getXHeight(), roboto->getAscender());
    EXPECT_EQ(roboto->getLabel(), "built-in Roboto");
    EXPECT_NE(roboto->getHbFont(), nullptr);

    // stb_truetype semantics: pixel height = ascent - descent, em size = upem
    EXPECT_FLOAT_EQ(roboto->scaleForPixelHeight(100.0f), 100.0f / (roboto->getAscender() - roboto->getDescender()));
    EXPECT_FLOAT_EQ(roboto->scaleForEmSize(100.0f), 100.0f / 2048.0f);

    std::shared_ptr<FontFace> bold = FontFace::loadFile("fonts/Roboto-Bold.ttf");
    ASSERT_NE(bold, nullptr);
    EXPECT_EQ(bold->getFamilyName(), "Roboto");
    EXPECT_EQ(bold->getWeight(), 700);
    EXPECT_FALSE(bold->isItalic());
    EXPECT_TRUE(bold->hasPostTable());
    EXPECT_LT(bold->getUnderlinePosition(), 0);
    EXPECT_GT(bold->getUnderlineThickness(), 0);
    EXPECT_EQ(bold->getLabel(), "fonts/Roboto-Bold.ttf");

    std::shared_ptr<FontFace> italic = FontFace::loadFile("fonts/Roboto-Italic.ttf");
    ASSERT_NE(italic, nullptr);
    EXPECT_EQ(italic->getFamilyName(), "Roboto");
    EXPECT_EQ(italic->getWeight(), 400);
    EXPECT_TRUE(italic->isItalic());
}

TEST(FontFaceTest, ArabicCoverage){
    std::shared_ptr<FontFace> arabic = FontRegistry::getBuiltInArabic();
    std::shared_ptr<FontFace> latin = FontRegistry::getBuiltInLatin();
    ASSERT_NE(arabic, nullptr);
    EXPECT_EQ(arabic->getFamilyName(), "Noto Sans Arabic");

    for (uint32_t c : {0x0627u, 0x0644u, 0x0645u, 0x0631u}){
        EXPECT_TRUE(arabic->hasCodepoint(c)) << c;
        EXPECT_FALSE(latin->hasCodepoint(c)) << c;
    }
    EXPECT_TRUE(latin->hasCodepoint('A'));
    EXPECT_TRUE(latin->hasCodepoint(0x0416));   // Cyrillic Zhe
    EXPECT_TRUE(latin->hasCodepoint(0x03A9));   // Greek Omega
    EXPECT_FALSE(latin->hasCodepoint(0x4E2D));
    EXPECT_EQ(latin->getGlyphIndex(0x4E2D), 0u);

    // a glyph with ink rasterizes, a space does not
    std::vector<unsigned char> pixels;
    int w, h, x, y;
    const float scale = arabic->scaleForEmSize(32.0f);
    EXPECT_TRUE(arabic->rasterizeGlyph(arabic->getGlyphIndex(0x0645), scale, pixels, w, h, x, y));
    EXPECT_GT(w, 0);
    EXPECT_GT(h, 0);
    EXPECT_EQ(pixels.size(), (size_t)(w * h));
    bool inked = false;
    for (unsigned char p : pixels) inked |= p > 0;
    EXPECT_TRUE(inked);

    EXPECT_FALSE(latin->rasterizeGlyph(latin->getGlyphIndex(' '), scale, pixels, w, h, x, y));
    EXPECT_EQ(w, 0);
    EXPECT_TRUE(pixels.empty());
    EXPECT_GT(latin->getGlyphAdvance(latin->getGlyphIndex(' '), scale), 0.0f);
}

TEST(FontFaceTest, CollectionIndex){
    std::vector<unsigned char> font = readFile("fonts/Roboto-Bold.ttf");
    ASSERT_FALSE(font.empty());

    // a plain font only has index 0
    std::string error;
    EXPECT_EQ(FontFace::loadFile("fonts/Roboto-Bold.ttf", 1, &error), nullptr);
    EXPECT_EQ(error, "Failed to initialize font: fonts/Roboto-Bold.ttf");
    EXPECT_EQ(FontFace::loadFile("fonts/Roboto-Bold.ttf", -1), nullptr);

    std::vector<unsigned char> collection = makeCollection(font, 2);
    for (int index : {0, 1}){
        std::shared_ptr<FontFace> member = FontFace::loadMemory(collection.data(), collection.size(), index, true, "collection");
        ASSERT_NE(member, nullptr) << index;
        EXPECT_EQ(member->getFaceIndex(), index);
        EXPECT_EQ(member->getFamilyName(), "Roboto");
        EXPECT_EQ(member->getWeight(), 700);
        EXPECT_TRUE(member->hasCodepoint('A'));
    }
    EXPECT_EQ(FontFace::loadMemory(collection.data(), collection.size(), 2, true, "collection", &error), nullptr);
    EXPECT_EQ(error, "Failed to initialize font: collection");
}

TEST(FontFaceTest, InvalidDataFails){
    std::string error;
    EXPECT_EQ(FontFace::loadFile("fonts/does-not-exist.ttf", 0, &error), nullptr);
    EXPECT_EQ(error, "Font file not found: fonts/does-not-exist.ttf");
    EXPECT_EQ(FontFace::loadFile("", 0, &error), nullptr);

    EXPECT_EQ(FontFace::loadMemory(nullptr, 100, 0, false, "null", &error), nullptr);
    EXPECT_EQ(error, "Failed to initialize font: null");

    const char text[] = "this is not a font file at all, only some text that is long enough";
    EXPECT_EQ(FontFace::loadMemory((const unsigned char*)text, sizeof(text), 0, true, "text", &error), nullptr);

    // a real header whose tables are cut off: rejected before stb_truetype reads past the end
    std::vector<unsigned char> font = readFile("fonts/Roboto-Bold.ttf");
    ASSERT_GT(font.size(), 4096u);
    EXPECT_EQ(FontFace::loadMemory(font.data(), 4096, 0, true, "truncated", &error), nullptr);
    EXPECT_EQ(FontFace::loadMemory(font.data(), 11, 0, true, "tiny", &error), nullptr);

    // a collection header pointing past the end
    std::vector<unsigned char> collection = makeCollection(font, 1);
    writeU32(&collection[12], (uint32_t)collection.size());
    EXPECT_EQ(FontFace::loadMemory(collection.data(), collection.size(), 0, true, "bad collection", &error), nullptr);

    // nothing is logged by FontFace itself
    TestSystem::get().clearLogs();
    EXPECT_EQ(FontFace::loadFile("fonts/does-not-exist.ttf"), nullptr);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}

// ---------------------------------------------------------------------------------------
// FontRegistry

TEST_F(FontRegistryTest, NearestWeightRules){
    std::shared_ptr<FontFace> w300 = robotoCopy("w300");
    std::shared_ptr<FontFace> w400 = robotoCopy("w400");
    std::shared_ptr<FontFace> w700 = robotoCopy("w700");
    FontRegistry::registerFace(w300, "Weights", 300);
    FontRegistry::registerFace(w400, "Weights", 400);
    FontRegistry::registerFace(w700, "Weights", 700);

    auto find = [](const char* family, int weight){
        std::shared_ptr<FontFace> face = FontRegistry::find(family, weight, false);
        return face ? face->getLabel() : std::string("none");
    };

    // 400: 400, else 500, else lighter descending, else heavier ascending
    EXPECT_EQ(find("Weights", 400), "w400");
    // 400..500: desired up to 500 ascending, then lighter descending
    EXPECT_EQ(find("Weights", 450), "w400");
    EXPECT_EQ(find("Weights", 500), "w400");
    // < 400: lighter descending, then heavier ascending
    EXPECT_EQ(find("Weights", 300), "w300");
    EXPECT_EQ(find("Weights", 350), "w300");
    EXPECT_EQ(find("Weights", 100), "w300");
    // > 500: heavier ascending, then lighter descending
    EXPECT_EQ(find("Weights", 600), "w700");
    EXPECT_EQ(find("Weights", 700), "w700");
    EXPECT_EQ(find("Weights", 900), "w700");
    // 0 is normal
    EXPECT_EQ(find("Weights", 0), "w400");

    // with a 500 face, 400..500 goes up to it before going lighter
    std::shared_ptr<FontFace> w500 = robotoCopy("w500");
    FontRegistry::registerFace(w500, "Weights", 500);
    EXPECT_EQ(find("Weights", 400), "w400");
    EXPECT_EQ(find("Weights", 450), "w500");
    EXPECT_EQ(find("Weights", 500), "w500");
    EXPECT_EQ(find("Weights", 550), "w700");

    // without 400 and 500, 400 goes lighter first
    std::shared_ptr<FontFace> a300 = robotoCopy("a300");
    std::shared_ptr<FontFace> a700 = robotoCopy("a700");
    FontRegistry::registerFace(a300, "Apart", 300);
    FontRegistry::registerFace(a700, "Apart", 700);
    EXPECT_EQ(find("Apart", 400), "a300");
    EXPECT_EQ(find("Apart", 500), "a300");
    EXPECT_EQ(find("Apart", 501), "a700");
    EXPECT_EQ(find("Apart", 200), "a300");

    // registering the same family, weight and style again replaces the face
    std::shared_ptr<FontFace> again = robotoCopy("w700b");
    FontRegistry::registerFace(again, "Weights", 700);
    EXPECT_EQ(find("Weights", 700), "w700b");

    EXPECT_EQ(find("Unknown family", 400), "none");
}

TEST_F(FontRegistryTest, ItalicPreference){
    std::shared_ptr<FontFace> regular = robotoCopy("regular");
    std::shared_ptr<FontFace> boldItalic = robotoCopy("bold italic");
    FontRegistry::registerFace(regular, "Styled", 400, 0);
    FontRegistry::registerFace(boldItalic, "Styled", 700, 1);

    // the style wins over the weight
    EXPECT_EQ(FontRegistry::find("Styled", 400, true)->getLabel(), "bold italic");
    EXPECT_EQ(FontRegistry::find("Styled", 700, false)->getLabel(), "regular");

    // no italic face at all: the normal one (synthetic italic is phase 1)
    std::shared_ptr<FontFace> upright = robotoCopy("upright");
    FontRegistry::registerFace(upright, "Upright");
    EXPECT_EQ(FontRegistry::find("Upright", 400, true)->getLabel(), "upright");

    // italic -1 reads it from the face
    std::shared_ptr<FontFace> italicFile = FontFace::loadFile("fonts/Roboto-Italic.ttf");
    ASSERT_NE(italicFile, nullptr);
    FontRegistry::registerFace(italicFile, "FromFile");
    EXPECT_EQ(FontRegistry::find("FromFile", 400, true), italicFile);
    EXPECT_EQ(FontRegistry::find("FromFile", 400, false), italicFile);
}

TEST_F(FontRegistryTest, GenericFamilies){
    for (const char* generic : {"sans-serif", "serif", "monospace", "system-ui", "ui-sans-serif",
                                "ui-serif", "ui-monospace", "cursive", "fantasy", " Sans-Serif ", "'MONOSPACE'"}){
        EXPECT_TRUE(FontRegistry::isGenericFamily(generic)) << generic;
        EXPECT_TRUE(FontRegistry::hasFamily(generic)) << generic;
        EXPECT_EQ(FontRegistry::find(generic, 400, false), FontRegistry::getBuiltInLatin()) << generic;
    }
    EXPECT_FALSE(FontRegistry::isGenericFamily("Roboto"));
    EXPECT_FALSE(FontRegistry::isGenericFamily("sans"));
    EXPECT_FALSE(FontRegistry::isGenericFamily(""));

    // the built-ins are there without any registration
    EXPECT_TRUE(FontRegistry::hasFamily("Roboto"));
    EXPECT_TRUE(FontRegistry::hasFamily("noto sans arabic"));
    EXPECT_EQ(FontRegistry::find("Noto Sans Arabic", 400, false), FontRegistry::getBuiltInArabic());

    // a generic family is looked up as Roboto, so a project's Roboto Bold serves it
    std::shared_ptr<FontFace> bold = FontFace::loadFile("fonts/Roboto-Bold.ttf");
    ASSERT_NE(bold, nullptr);
    FontRegistry::registerFace(bold);
    EXPECT_EQ(FontRegistry::find("sans-serif", 700, false), bold);
    EXPECT_EQ(FontRegistry::find("serif", 400, false), FontRegistry::getBuiltInLatin());
}

TEST_F(FontRegistryTest, CaseAndQuotes){
    std::shared_ptr<FontFace> face = robotoCopy("mine");
    FontRegistry::registerFace(face, "  \"My Font\" ");
    EXPECT_EQ(FontRegistry::find("my font", 400, false), face);
    EXPECT_EQ(FontRegistry::find("'MY FONT'", 400, false), face);
    EXPECT_EQ(FontRegistry::find(" \"My Font\"", 400, false), face);
    EXPECT_TRUE(FontRegistry::hasFamily("MY FONT"));
    EXPECT_FALSE(FontRegistry::hasFamily("My Fonts"));
    EXPECT_EQ(FontRegistry::normalizeFamily("  'Open Sans' "), "open sans");
}

TEST_F(FontRegistryTest, FallbackFacesAndFileCache){
    EXPECT_TRUE(FontRegistry::getFallbackFaces().empty());

    std::shared_ptr<FontFace> fallback = robotoCopy("fallback");
    FontRegistry::registerFace(fallback, "Fallback Family", 0, -1, true);
    FontRegistry::registerFace(fallback, "Fallback Family", 0, -1, true);   // once
    ASSERT_EQ(FontRegistry::getFallbackFaces().size(), 1u);
    EXPECT_EQ(FontRegistry::getFallbackFaces()[0], fallback);
    EXPECT_EQ(FontRegistry::find("Fallback Family", 400, false), fallback);

    // the same file is parsed once while it is held
    std::shared_ptr<FontFace> first = FontRegistry::getOrLoadFile("fonts/Roboto-Bold.ttf");
    std::shared_ptr<FontFace> second = FontRegistry::getOrLoadFile("fonts/Roboto-Bold.ttf");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, second);
    EXPECT_NE(FontRegistry::getOrLoadFile("fonts/Roboto-Bold.ttf", 1), first);   // other index: fails

    std::string error;
    EXPECT_EQ(FontRegistry::getOrLoadFile("fonts/does-not-exist.ttf", 0, &error), nullptr);
    EXPECT_EQ(error, "Font file not found: fonts/does-not-exist.ttf");

    // clear drops every registration; the built-ins come back on the next use
    FontRegistry::clear();
    EXPECT_TRUE(FontRegistry::getFallbackFaces().empty());
    EXPECT_EQ(FontRegistry::find("Fallback Family", 400, false), nullptr);
    EXPECT_NE(FontRegistry::getBuiltInLatin(), nullptr);
}

// ---------------------------------------------------------------------------------------
// TextShaper

TEST(TextShaperTest, KerningToggle){
    Chain chain = builtInChain(100.0f);
    ShapeOptions noKerning;
    noKerning.kerning = false;

    const float kerned = measure(chain, "AV");
    const float plain = measure(chain, "AV", noKerning);
    EXPECT_LT(kerned, plain - 1.0f);

    // the advances alone, glyph by glyph
    const float separate = measure(chain, "A") + measure(chain, "V");
    EXPECT_NEAR(plain, separate, 1e-3f);
}

TEST(TextShaperTest, LetterSpacing){
    Chain chain = builtInChain(40.0f);
    ShapeOptions spaced;
    spaced.letterSpacing = 3.0f;

    const float base = measure(chain, "Hello");
    EXPECT_NEAR(measure(chain, "Hello", spaced), base + 5 * 3.0f, 1e-3f);

    // letter spacing turns ligatures off: "fi" is one glyph without, two with
    EXPECT_EQ(shape(chain, "fi").size(), 1u);
    std::vector<ShapedGlyph> separated = shape(chain, "fi", spaced);
    ASSERT_EQ(separated.size(), 2u);
    EXPECT_EQ(separated[0].cluster, 0u);
    EXPECT_EQ(separated[1].cluster, 1u);

    // negative spacing tightens
    spaced.letterSpacing = -2.0f;
    EXPECT_NEAR(measure(chain, "Hello", spaced), base - 10.0f, 1e-3f);
}

TEST(TextShaperTest, RtlOrder){
    Chain chain = builtInChain(32.0f);

    // a right-to-left word comes out in visual order: last letter first
    std::vector<ShapedGlyph> word = shape(chain, arabicWord);
    ASSERT_FALSE(word.empty());
    for (const ShapedGlyph& glyph : word){
        EXPECT_TRUE(glyph.rtl);
        EXPECT_EQ(glyph.face, 1u);   // the Arabic face of the chain
    }
    EXPECT_GT(word.front().cluster, word.back().cluster);

    // mixed text: auto takes the first strong character (Latin, LTR paragraph), so "abc"
    // is leftmost; a forced RTL base puts the Arabic word first
    std::string mixed = std::string("abc ") + arabicWord;
    std::vector<ShapedGlyph> automatic = shape(chain, mixed);
    ASSERT_FALSE(automatic.empty());
    EXPECT_EQ(automatic.front().cluster, 0u);
    EXPECT_FALSE(automatic.front().rtl);

    ShapeOptions rtl;
    rtl.baseDirection = 2;
    std::vector<ShapedGlyph> rightToLeft = shape(chain, mixed, rtl);
    ASSERT_EQ(rightToLeft.size(), automatic.size());
    EXPECT_TRUE(rightToLeft.front().rtl);
    EXPECT_GE(rightToLeft.front().cluster, 4u);

    // and a forced LTR base keeps an Arabic-first string left to right between runs
    std::string arabicFirst = std::string(arabicWord) + " abc";
    ShapeOptions ltr;
    ltr.baseDirection = 1;
    std::vector<ShapedGlyph> leftToRight = shape(chain, arabicFirst, ltr);
    ASSERT_FALSE(leftToRight.empty());
    EXPECT_TRUE(leftToRight.front().rtl);
    EXPECT_FALSE(leftToRight.back().rtl);

    // auto: an Arabic-first string is a right-to-left paragraph, so "abc" ends up left
    std::vector<ShapedGlyph> autoArabicFirst = shape(chain, arabicFirst);
    ASSERT_FALSE(autoArabicFirst.empty());
    EXPECT_FALSE(autoArabicFirst.front().rtl);
    EXPECT_TRUE(autoArabicFirst.back().rtl);
}

TEST(TextShaperTest, ArabicJoins){
    Chain chain = builtInChain(32.0f);
    std::shared_ptr<FontFace> arabic = FontRegistry::getBuiltInArabic();

    // joined letters take their contextual forms: in "lm" (lam meem) the lam is initial and
    // the meem final, neither is the isolated glyph. (The built-in Noto Sans Arabic subset
    // has no lam-alef ligature, so a ligature never lowers the glyph count with it.)
    std::vector<ShapedGlyph> glyphs = shape(chain, "\xd9\x84\xd9\x85");
    ASSERT_EQ(glyphs.size(), 2u);
    // visual order: meem (cluster 1) left, lam (cluster 0) right
    EXPECT_EQ(glyphs[0].cluster, 1u);
    EXPECT_EQ(glyphs[1].cluster, 0u);
    EXPECT_NE(glyphs[0].glyphIndex, arabic->getGlyphIndex(0x0645));
    EXPECT_NE(glyphs[1].glyphIndex, arabic->getGlyphIndex(0x0644));

    // lam + alef also joins
    std::vector<ShapedGlyph> lamAlefGlyphs = shape(chain, lamAlef);
    ASSERT_EQ(lamAlefGlyphs.size(), 2u);
    EXPECT_NE(lamAlefGlyphs[1].glyphIndex, arabic->getGlyphIndex(0x0644));

    // the same letters shaped one by one stay isolated
    std::vector<uint32_t> lam(1, 0x0644);
    std::vector<ShapedGlyph> alone;
    TextShaper::shapeRange(chain.faces, chain.scales, lam, 0, 1, 0, ShapeOptions(), alone);
    ASSERT_EQ(alone.size(), 1u);
    EXPECT_EQ(alone[0].glyphIndex, arabic->getGlyphIndex(0x0644));

    // a ligature does lower the count: Roboto's "fi"
    EXPECT_LT(shape(chain, "fi").size(), decode("fi").size());

    // a language tag is accepted
    ShapeOptions options;
    options.language = "ar";
    EXPECT_EQ(shape(chain, arabicWord, options).size(), shape(chain, arabicWord).size());
}

TEST(TextShaperTest, MatchesSTBTextShaping){
    // STBText's chain at 24 px: the characterization test recorded the last caret of this
    // line at 239.579987 before shaping moved here
    Chain chain = builtInChain(24.0f);
    EXPECT_FLOAT_EQ(measure(chain, "Hello, World! AV Ta fi 0123"), 239.579987f);

    // the face picked per codepoint: the first that has it, after a fallback the primary again
    EXPECT_EQ(TextShaper::selectFace(chain.faces, 'A', 0), 0u);
    EXPECT_EQ(TextShaper::selectFace(chain.faces, 0x0645, 0), 1u);
    EXPECT_EQ(TextShaper::selectFace(chain.faces, ' ', 1), 0u);
    EXPECT_EQ(TextShaper::selectFace(chain.faces, 0x4E2D, 1), 0u);   // nobody: the primary's .notdef

    // offsets, lines and ranges
    std::vector<uint32_t> codepoints = decode("one two");
    std::vector<ShapedGlyph> out;
    TextShaper::shapeRange(chain.faces, chain.scales, codepoints, 4, 3, 7, ShapeOptions(), out);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].cluster, 4u);
    EXPECT_EQ(out[2].line, 7u);
    out.clear();
    TextShaper::shapeRange(chain.faces, chain.scales, codepoints, 5, 10, 0, ShapeOptions(), out);   // out of range
    EXPECT_TRUE(out.empty());

    // UTF-8 decoding, invalid bytes become U+FFFD
    std::vector<uint32_t> decoded;
    const char text[] = "a\xc3\xa9\xf0\x9f\x98\x80\xff";
    TextShaper::decodeUtf8(text, sizeof(text) - 1, decoded);
    EXPECT_EQ(decoded, (std::vector<uint32_t>{'a', 0xE9, 0x1F600, 0xFFFD}));
    TextShaper::decodeUtf8(nullptr, 0, decoded);
    EXPECT_TRUE(decoded.empty());
}

// ---------------------------------------------------------------------------------------
// GlyphAtlas

TEST(GlyphAtlasTest, PackNoOverlap){
    GlyphAtlas atlas(256, 256, 1);
    EXPECT_EQ(atlas.getWidth(), 256u);
    EXPECT_EQ(atlas.getHeight(), 256u);
    EXPECT_EQ(atlas.getPixels().size(), 256u * 256u);

    struct Rect { int x, y, w, h; };
    std::vector<Rect> placed;
    std::mt19937 random(1234);
    std::uniform_int_distribution<int> size(1, 24);

    for (int i = 0; i < 400; i++){
        int w = size(random);
        int h = size(random);
        int x, y;
        if (!atlas.pack(w, h, x, y))
            break;
        EXPECT_GE(x, 0);
        EXPECT_GE(y, 0);
        EXPECT_LE(x + w, 256);
        EXPECT_LE(y + h, 256);
        placed.push_back({x, y, w, h});
    }
    EXPECT_GT(placed.size(), 100u);

    // no two rects (with their padding) overlap
    for (size_t i = 0; i < placed.size(); i++){
        for (size_t j = i + 1; j < placed.size(); j++){
            const Rect& a = placed[i];
            const Rect& b = placed[j];
            bool overlap = a.x < b.x + b.w + 1 && b.x < a.x + a.w + 1 && a.y < b.y + b.h + 1 && b.y < a.y + a.h + 1;
            EXPECT_FALSE(overlap) << i << " and " << j;
        }
    }

    // blit writes exactly the rect
    std::vector<unsigned char> square(4 * 3, 200);
    int x, y;
    GlyphAtlas small(16, 16, 1);
    ASSERT_TRUE(small.pack(4, 3, x, y));
    small.blit(square.data(), 4, 3, x, y);
    size_t written = 0;
    for (unsigned char p : small.getPixels()) written += (p == 200);
    EXPECT_EQ(written, 12u);
    EXPECT_EQ(small.getPixels()[(size_t)y * 16 + x], 200);

    // clipped at the edge, ignored outside
    small.blit(square.data(), 4, 3, 14, 15);
    small.blit(square.data(), 4, 3, 16, 0);
    small.blit(square.data(), 4, 3, -1, 0);
    written = 0;
    for (unsigned char p : small.getPixels()) written += (p == 200);
    EXPECT_EQ(written, 14u);

    // reset empties it
    small.reset(32, 8);
    EXPECT_EQ(small.getWidth(), 32u);
    EXPECT_EQ(small.getPixels().size(), 256u);
    for (unsigned char p : small.getPixels()) EXPECT_EQ(p, 0);
}

TEST(GlyphAtlasTest, FullReturnsFalse){
    GlyphAtlas atlas(64, 64, 1);
    int x, y;

    // larger than the atlas (with padding)
    EXPECT_FALSE(atlas.pack(64, 10, x, y));
    EXPECT_FALSE(atlas.pack(10, 64, x, y));
    // empty rects always fit and take no space
    EXPECT_TRUE(atlas.pack(0, 10, x, y));
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);

    // 16 cells of 15x15 (+1 padding) fill it exactly
    int packed = 0;
    while (atlas.pack(15, 15, x, y)) packed++;
    EXPECT_EQ(packed, 16);
    EXPECT_FALSE(atlas.pack(1, 1, x, y));

    // a reset atlas packs from the top-left again
    atlas.reset(64, 64);
    ASSERT_TRUE(atlas.pack(15, 15, x, y));
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);
}
