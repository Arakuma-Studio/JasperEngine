// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Default-ignorable characters (U+200B zero-width space, joiners, word joiner, soft hyphen,
// variation selectors, BOM) in the engine's text stack (phase-3 spec 3.10, p3-text): they draw
// nothing, take no space and never switch the text to a fallback face. Taffy's fixtures build
// their sizes from "HH&ZeroWidthSpace;HH" in Ahem, which has no U+200B glyph: the shaper took the
// next face that has one (the built-in Roboto) and kept it for the letters after it.

#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"
#include "FixtureRunner.h"

#include "HtmlUI.h"
#include "RmlFile.h"
#include "RmlFontEngine.h"
#include "RmlSystem.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"
#include "util/TextShaper.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::vector<unsigned char> ahemBytes(){
        std::ifstream file(htmluiTestSourceDirectory() + "/fonts/Ahem.ttf", std::ios::binary);
        return std::vector<unsigned char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    std::vector<uint32_t> decode(const std::string& text){
        std::vector<uint32_t> codepoints;
        TextShaper::decodeUtf8(text.data(), text.size(), codepoints);
        return codepoints;
    }

    // Ahem (no U+200B, no soft hyphen), then the built-in Roboto: the chain of a document whose
    // font-family is Ahem
    struct AhemChain {
        std::vector<unsigned char> bytes = ahemBytes();
        std::shared_ptr<FontFace> ahem;
        std::shared_ptr<FontFace> roboto = FontRegistry::getBuiltInLatin();
        std::vector<const FontFace*> faces;
        std::vector<float> scales;

        explicit AhemChain(float size){
            ahem = FontFace::loadMemory(bytes.data(), bytes.size(), 0, false, "Ahem");
            if (!ahem)
                return;
            faces = {ahem.get(), roboto.get()};
            scales = {ahem->scaleForEmSize(size), roboto->scaleForEmSize(size)};
        }

        std::vector<ShapedGlyph> shape(const std::string& text) const{
            std::vector<uint32_t> codepoints = decode(text);
            std::vector<ShapedGlyph> out;
            TextShaper::shapeRange(faces, scales, codepoints, 0, codepoints.size(), 0, ShapeOptions(), out);
            return out;
        }
    };

    // UTF-8 of the characters under test
    const char* zwsp = "\xe2\x80\x8b";   // U+200B
    const char* ignorables[] = {
        "\xe2\x80\x8b",   // U+200B zero-width space
        "\xe2\x80\x8c",   // U+200C zero-width non-joiner
        "\xe2\x80\x8d",   // U+200D zero-width joiner
        "\xe2\x81\xa0",   // U+2060 word joiner
        "\xc2\xad",       // U+00AD soft hyphen
        "\xef\xbb\xbf",   // U+FEFF zero-width no-break space (BOM)
        "\xef\xb8\x8f",   // U+FE0F variation selector 16
        "\xe2\x80\x8e",   // U+200E left-to-right mark
    };

}

TEST(TextZeroWidth, DefaultIgnorableCodepoints){
    for (uint32_t c : {0x00ADu, 0x034Fu, 0x061Cu, 0x180Eu, 0x200Bu, 0x200Cu, 0x200Du, 0x200Eu, 0x200Fu, 0x202Au, 0x202Eu,
                       0x2060u, 0x2064u, 0xFE00u, 0xFE0Fu, 0xFEFFu, 0x1D173u, 0xE0001u, 0xE0FFFu})
        EXPECT_TRUE(TextShaper::isDefaultIgnorable(c)) << std::hex << c;
    for (uint32_t c : {0x0020u, 0x00A0u, 0x0041u, 0x00ACu, 0x00AEu, 0x2010u, 0x2028u, 0x200Au, 0x2070u, 0x3000u, 0xFE10u, 0xFFFDu,
                       0x1F600u, 0xE1000u})
        EXPECT_FALSE(TextShaper::isDefaultIgnorable(c)) << std::hex << c;
}

TEST(TextZeroWidth, IgnorablesKeepTheCurrentFace){
    AhemChain chain(10.0f);
    ASSERT_TRUE(chain.ahem);
    ASSERT_FALSE(chain.ahem->hasCodepoint(0x200B));

    // the current face whatever covers it; at the start of a run, the primary
    EXPECT_EQ(TextShaper::selectFace(chain.faces, 0x200B, 0), 0u);
    EXPECT_EQ(TextShaper::selectFace(chain.faces, 0x200B, 1), 1u);
    EXPECT_EQ(TextShaper::selectFace(chain.faces, 0x00AD, 0), 0u);
    EXPECT_EQ(TextShaper::selectFace(chain.faces, 0x200B, 7), 0u);   // out of range: the primary
    // other codepoints as before: the current face while it covers, else the first that does
    EXPECT_EQ(TextShaper::selectFace(chain.faces, 'H', 0), 0u);
    EXPECT_EQ(TextShaper::selectFace(chain.faces, 'H', 1), 1u);
}

TEST(TextZeroWidth, IgnorablesAreInvisibleAndZeroWidth){
    AhemChain chain(10.0f);
    ASSERT_TRUE(chain.ahem);

    for (const char* ignorable : ignorables){
        SCOPED_TRACE(ignorable);
        const std::vector<ShapedGlyph> glyphs = chain.shape(std::string("HH") + ignorable + "HH");
        float total = 0.0f;
        for (const ShapedGlyph& glyph : glyphs){
            // every glyph in Ahem: the letters after it did not move to Roboto
            EXPECT_EQ(glyph.face, 0u);
            total += glyph.xAdvance;
        }
        // four 10px Ahem squares, nothing for the ignorable
        EXPECT_FLOAT_EQ(total, 40.0f);
        for (const ShapedGlyph& glyph : glyphs){
            if (glyph.cluster == 2)
                EXPECT_FLOAT_EQ(glyph.xAdvance, 0.0f);
        }
    }

    // the same at the start and at the end of a run
    float total = 0.0f;
    for (const ShapedGlyph& glyph : chain.shape(std::string(zwsp) + "H" + zwsp)){
        EXPECT_EQ(glyph.face, 0u);
        total += glyph.xAdvance;
    }
    EXPECT_FLOAT_EQ(total, 10.0f);
}

namespace {

    class TextZeroWidthEngine : public ::testing::Test {
    protected:
        RecordingRenderInterface recording;
        RmlFontEngine engine;
        Rml::Context* context = nullptr;
        Rml::String language;
        std::vector<unsigned char> ahem = ahemBytes();

        void SetUp() override{
            ASSERT_FALSE(HtmlUI::isInitialized());
            FontRegistry::clear();
            Rml::SetSystemInterface(&HtmlUI::getSystemInterface());
            Rml::SetFileInterface(&HtmlUI::getFileInterface());
            Rml::SetFontEngineInterface(&engine);
            ASSERT_TRUE(Rml::Initialise());
            context = Rml::CreateContext("zero-width", Rml::Vector2i(320, 200), &recording);
            ASSERT_NE(context, nullptr);
            ASSERT_FALSE(ahem.empty());
            ASSERT_TRUE(engine.LoadFontFace(Rml::Span<const Rml::byte>(ahem.data(), ahem.size()), 0, "Ahem", Rml::Style::FontStyle::Normal,
                                            Rml::Style::FontWeight::Normal, false));
        }

        void TearDown() override{
            if (context)
                Rml::RemoveContext("zero-width");
            Rml::Shutdown();
            FontRegistry::clear();
        }
    };

}

TEST_F(TextZeroWidthEngine, MeasuredAndDrawnLikeNothing){
    const Rml::FontFaceHandle face = engine.GetFontFaceHandle("ahem", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal, 10);
    ASSERT_NE(face, 0u);
    const Rml::TextShapingContext shaping{language};

    for (const char* ignorable : ignorables){
        SCOPED_TRACE(ignorable);
        EXPECT_EQ(engine.GetStringWidth(face, std::string("HH") + ignorable + "HH", shaping), 40);
        EXPECT_EQ(engine.GetStringWidth(face, std::string(ignorable), shaping), 0);

        Rml::TexturedMeshList meshes;
        const int width = engine.GenerateString(context->GetRenderManager(), face, 0, std::string("HH") + ignorable + "HH", Rml::Vector2f(0, 20),
                                                Rml::ColourbPremultiplied(255, 255, 255, 255), 1.0f, shaping, meshes);
        EXPECT_EQ(width, 40);
        size_t quads = 0;
        for (const Rml::TexturedMesh& mesh : meshes)
            quads += mesh.mesh.indices.size() / 6;
        // four inked squares, each 10px wide: the letters are Ahem's, not Roboto's "H"
        EXPECT_EQ(quads, 4u);
        if (!meshes.empty() && meshes[0].mesh.vertices.size() >= 16){
            const Rml::Vertex* v = meshes[0].mesh.vertices.data();
            for (size_t q = 0; q < 4; q++)
                EXPECT_NEAR(v[q * 4 + 2].position.x - v[q * 4].position.x, 10.0f, 1.0f);
            EXPECT_NEAR(v[3 * 4].position.x, 30.0f, 1.0f);
        }
    }
}
