// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The RmlUi font protocol: independent font-table metrics, width arguments and input ownership.
// Actual glyph pixels, synthesis, opacity and scaling belong to the GPU text owners.

#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "RmlFontEngine.h"
#include "io/Data.h"
#include "util/FontRegistry.h"

#include <RmlUi/Core/TextShapingContext.h>
#include <RmlUi/Core/StringUtilities.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class RmlFontEngineTest : public ::testing::Test {
    protected:
        RmlFontEngine engine;
        const Rml::String language;

        void SetUp() override{
            ASSERT_FALSE(HtmlUI::isInitialized());
            FontRegistry::clear();
            engine.Initialize();
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            engine.Shutdown();
            FontRegistry::clear();
        }

        Rml::FontFaceHandle handle(const std::string& family, int size,
                                   Rml::Style::FontStyle style = Rml::Style::FontStyle::Normal){
            return engine.GetFontFaceHandle(family, style, Rml::Style::FontWeight::Normal, size);
        }

        int width(Rml::FontFaceHandle face, const std::string& text, Rml::Character prior = Rml::Character::Null,
                  Rml::Style::FontKerning kerning = Rml::Style::FontKerning::Auto){
            Rml::TextShapingContext shaping{language};
            shaping.font_kerning = kerning;
            return engine.GetStringWidth(face, text, shaping, prior);
        }
    };

}

TEST_F(RmlFontEngineTest, EmptyFamilyUsesDefault){
    ASSERT_TRUE(engine.LoadFontFace("fonts/Roboto-Bold.ttf", 0, "Some Fallback", Rml::Style::FontStyle::Normal,
                                    Rml::Style::FontWeight::Bold, true));
    // A registered fallback must not displace the default primary face. Roboto Regular's
    // hmtx M advance is 1788/2048 em, independently read from the bundled font.
    for (const char* family : {"", "Nothing Loaded"})
        EXPECT_EQ(width(handle(family, 100), "M"), 87) << family;
}

TEST_F(RmlFontEngineTest, EmSizeNotPixelHeight){
    // Roboto's hhea ascender=1900, descender=-500, unitsPerEm=2048.
    const Rml::FontMetrics& metrics = engine.GetFontMetrics(handle("Roboto", 100));
    EXPECT_NEAR(metrics.ascent, 92.7734375f, 0.01f);
    EXPECT_NEAR(metrics.ascent + metrics.descent, 117.1875f, 0.01f);
}

TEST_F(RmlFontEngineTest, PriorCharacterKerning){
    Rml::FontFaceHandle face = handle("sans-serif", 48);
    // Independently read Roboto tables: hmtx V=1304, space=508; GPOS A/V XAdvance=-87,
    // unitsPerEm=2048. At 48 px V rounds to 31, or 29 after A; three spaces round to 36.
    EXPECT_EQ(width(face, "V"), 31);
    EXPECT_EQ(width(face, "V", Rml::Character('A')), 29);
    EXPECT_EQ(width(face, "V", Rml::Character('A'), Rml::Style::FontKerning::None), 31);
    EXPECT_EQ(width(face, ""), 0);
    EXPECT_EQ(width(face, "   "), 36);
    // RmlUi keeps the line feed of a preformatted line; it contributes no advance.
    EXPECT_EQ(width(face, "\n"), 0);
}

TEST_F(RmlFontEngineTest, LoadFontFaceFileAndMemory){
    Data data;
    ASSERT_EQ(data.open("fonts/Roboto-Italic.ttf"), FileErrors::FILEDATA_OK);
    std::vector<unsigned char> bytes(data.getMemPtr(), data.getMemPtr() + data.length());
    ASSERT_FALSE(bytes.empty());
    ASSERT_TRUE(engine.LoadFontFace(Rml::Span<const Rml::byte>(bytes.data(), bytes.size()), 0, "Memory Italic",
                                    Rml::Style::FontStyle::Italic, Rml::Style::FontWeight::Normal, false));
    // The API copies its input: deterministic corruption exposes borrowing even if the
    // caller's freed allocation would otherwise retain the original font bytes.
    std::fill(bytes.begin(), bytes.end(), 0);
    EXPECT_EQ(width(handle("Memory Italic", 100, Rml::Style::FontStyle::Italic), "M"), 85);
    // Collection of generated font resources must retain registered faces. Italic M's
    // independent hmtx advance is 1734/2048 em; fallback Regular M would measure 87 instead.
    engine.ReleaseFontResources();
    EXPECT_EQ(width(handle("Memory Italic", 100, Rml::Style::FontStyle::Italic), "M"), 85);
}

TEST_F(RmlFontEngineTest, LoadFontFaceInvalidFails){
    EXPECT_FALSE(engine.LoadFontFace("fonts/nope.ttf", 0, false, Rml::Style::FontWeight::Auto));
    // Prove the file exists and is valid, so an unrelated file error cannot satisfy the
    // invalid collection-index check. The bundled TTF has only face 0.
    ASSERT_TRUE(engine.LoadFontFace("fonts/Roboto-Bold.ttf", 0, false, Rml::Style::FontWeight::Auto));
    EXPECT_FALSE(engine.LoadFontFace("fonts/Roboto-Bold.ttf", 3, "Bad Index", Rml::Style::FontStyle::Normal,
                                     Rml::Style::FontWeight::Auto, false));
    EXPECT_FALSE(FontRegistry::hasFamily("Bad Index"));
    const char garbage[] = "definitely not a font, just bytes";
    EXPECT_FALSE(engine.LoadFontFace(Rml::Span<const Rml::byte>((const Rml::byte*)garbage, sizeof(garbage)), 0, "Garbage",
                                     Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Auto, false));
    EXPECT_FALSE(FontRegistry::hasFamily("Garbage"));
}
