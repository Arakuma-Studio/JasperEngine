// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RmlFontEngine, RmlUi's font engine on the engine text stack. CPU only: RmlUi runs on the
// RecordingRenderInterface, which keeps the generated page textures and the draws.

#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "RmlFile.h"
#include "RmlFontEngine.h"
#include "RmlSystem.h"
#include "io/Data.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"
#include "util/TextShaper.h"

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct Generated {
        int width = 0;
        Rml::TexturedMeshList meshes;

        size_t quads() const{
            size_t count = 0;
            for (const Rml::TexturedMesh& mesh : meshes)
                count += mesh.mesh.indices.size() / 6;
            return count;
        }
        // corners of the n-th quad of the first mesh
        const Rml::Vertex& topLeft(size_t quad) const{ return meshes[0].mesh.vertices[quad * 4]; }
        const Rml::Vertex& bottomRight(size_t quad) const{ return meshes[0].mesh.vertices[quad * 4 + 2]; }
    };

    std::vector<unsigned char> readFile(const std::string& path){
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK)
            return {};
        return std::vector<unsigned char>(data.getMemPtr(), data.getMemPtr() + data.length());
    }

    class RmlFontEngineTest : public ::testing::Test {
    protected:
        // declared first: render managers release through it during Rml::Shutdown
        RecordingRenderInterface recording;
        RmlFontEngine engine;
        Rml::Context* context = nullptr;
        Rml::String language;

        void SetUp() override{
            ASSERT_FALSE(HtmlUI::isInitialized());
            FontRegistry::clear();

            Rml::SetSystemInterface(&HtmlUI::getSystemInterface());
            Rml::SetFileInterface(&HtmlUI::getFileInterface());
            Rml::SetFontEngineInterface(&engine);
            ASSERT_TRUE(Rml::Initialise());

            context = Rml::CreateContext("font-engine", Rml::Vector2i(640, 360), &recording);
            ASSERT_NE(context, nullptr);

            HtmlUI::getSystemInterface().resetCounters();
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            if (context)
                Rml::RemoveContext("font-engine");
            Rml::Shutdown();
            FontRegistry::clear();

            EXPECT_EQ(recording.getLiveTextureCount(), 0u);
            EXPECT_EQ(recording.getLiveGeometryCount(), 0u);
        }

        Rml::FontFaceHandle handle(const std::string& family, int size, Rml::Style::FontWeight weight = Rml::Style::FontWeight::Normal,
                                   Rml::Style::FontStyle style = Rml::Style::FontStyle::Normal){
            return engine.GetFontFaceHandle(family, style, weight, size);
        }

        Rml::TextShapingContext shaping(float letterSpacing = 0.0f, Rml::Style::FontKerning kerning = Rml::Style::FontKerning::Auto){
            Rml::TextShapingContext context{language};
            context.letter_spacing = letterSpacing;
            context.font_kerning = kerning;
            return context;
        }

        int width(Rml::FontFaceHandle face, const std::string& text, float letterSpacing = 0.0f,
                  Rml::Character prior = Rml::Character::Null){
            return engine.GetStringWidth(face, text, shaping(letterSpacing), prior);
        }

        Generated generate(Rml::FontFaceHandle face, const std::string& text, Rml::Vector2f position = Rml::Vector2f(10, 50),
                           Rml::ColourbPremultiplied colour = Rml::ColourbPremultiplied(255, 255, 255, 255), float opacity = 1.0f,
                           float letterSpacing = 0.0f){
            Generated out;
            out.width = engine.GenerateString(context->GetRenderManager(), face, 0, text, position, colour, opacity,
                                              shaping(letterSpacing), out.meshes);
            return out;
        }
    };

    const char* textDocument = R"(
        <rml>
        <head>
            <style>
                body { display: block; width: 600px; height: 300px; font-family: sans-serif; font-size: 24px; color: #ffffffff; }
                p { display: block; }
                /* RmlUi 6.3: a family list is one string, quotes are not RCSS (phase 2 rewrites both) */
                #b { font-family: No Such Font, sans-serif; font-weight: bold; }
                #c { -rmlui-direction: rtl; }
            </style>
        </head>
        <body>
            <p id="a">Hello text</p>
            <p id="b">Bold fallback</p>
            <p id="c">&#x645;&#x631;&#x62D;&#x628;&#x627; abc</p>
        </body>
        </rml>
    )";

}

TEST_F(RmlFontEngineTest, HandleCaching){
    Rml::FontFaceHandle a = handle("Roboto", 16);
    ASSERT_NE(a, 0u);
    EXPECT_EQ(handle("Roboto", 16), a);
    // the family list is normalized before it is a key
    EXPECT_EQ(handle(" 'roboto' ", 16), a);
    EXPECT_NE(handle("Roboto", 17), a);
    EXPECT_NE(handle("Roboto", 16, Rml::Style::FontWeight::Bold), a);
    EXPECT_NE(handle("Roboto", 16, Rml::Style::FontWeight::Normal, Rml::Style::FontStyle::Italic), a);
    // Auto weight is normal
    EXPECT_EQ(handle("Roboto", 16, Rml::Style::FontWeight::Auto), a);
    // no size, no handle
    EXPECT_EQ(handle("Roboto", 0), 0u);
    EXPECT_EQ(handle("Roboto", -3), 0u);

    // the chain: Roboto, then Noto Sans Arabic, each once
    std::vector<const FontFace*> chain = engine.getChain(a);
    ASSERT_EQ(chain.size(), 2u);
    EXPECT_EQ(chain[0], FontRegistry::getBuiltInLatin().get());
    EXPECT_EQ(chain[1], FontRegistry::getBuiltInArabic().get());
}

TEST_F(RmlFontEngineTest, FamilyListSplitAndFallback){
    EXPECT_EQ(RmlFontEngine::splitFamilyList("\"A, B\", 'C' , d,,  "), (std::vector<std::string>{"a, b", "c", "d"}));
    EXPECT_EQ(RmlFontEngine::splitFamilyList("Missing, sans-serif"), (std::vector<std::string>{"missing", "sans-serif"}));
    EXPECT_TRUE(RmlFontEngine::splitFamilyList("").empty());

    Rml::FontFaceHandle face = handle("Missing Family X, sans-serif", 16);
    ASSERT_NE(face, 0u);
    EXPECT_EQ(engine.getChain(face)[0], FontRegistry::getBuiltInLatin().get());
    // warned once per name, not per lookup
    EXPECT_NE(handle("Missing Family X, sans-serif", 20), 0u);
    EXPECT_NE(handle("Missing Family X", 20), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI] font-family 'missing family x' is not loaded"), 1u);

    // a loaded family comes first, the rest of the list after it
    ASSERT_TRUE(engine.LoadFontFace("fonts/Roboto-Bold.ttf", 0, "Heavy", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Bold, false));
    Rml::FontFaceHandle heavy = handle("Heavy, Missing Family X, 'Noto Sans Arabic'", 16, Rml::Style::FontWeight::Bold);
    std::vector<const FontFace*> chain = engine.getChain(heavy);
    ASSERT_EQ(chain.size(), 3u);
    EXPECT_EQ(chain[0]->getLabel(), "fonts/Roboto-Bold.ttf");
    EXPECT_EQ(chain[1], FontRegistry::getBuiltInArabic().get());
    EXPECT_EQ(chain[2], FontRegistry::getBuiltInLatin().get());

    // a fallback face is in every chain, after the families asked for
    ASSERT_TRUE(engine.LoadFontFace("fonts/Roboto-Italic.ttf", 0, true, Rml::Style::FontWeight::Auto));
    chain = engine.getChain(handle("Heavy", 16, Rml::Style::FontWeight::Bold));
    ASSERT_EQ(chain.size(), 4u);
    EXPECT_EQ(chain[0]->getLabel(), "fonts/Roboto-Bold.ttf");
    EXPECT_EQ(chain[1]->getLabel(), "fonts/Roboto-Italic.ttf");
}

TEST_F(RmlFontEngineTest, EmptyFamilyUsesDefault){
    // a fallback face registered: still the default font first
    ASSERT_TRUE(engine.LoadFontFace("fonts/Roboto-Bold.ttf", 0, "Some Fallback", Rml::Style::FontStyle::Normal,
                                    Rml::Style::FontWeight::Bold, true));
    for (const char* family : {"", "   ", "Nothing Loaded"}){
        Rml::FontFaceHandle face = handle(family, 16);
        ASSERT_NE(face, 0u) << family;
        std::vector<const FontFace*> chain = engine.getChain(face);
        ASSERT_EQ(chain.size(), 3u);
        EXPECT_EQ(chain[0], FontRegistry::getBuiltInLatin().get());
        EXPECT_EQ(chain[1]->getLabel(), "fonts/Roboto-Bold.ttf");
        EXPECT_EQ(chain[2], FontRegistry::getBuiltInArabic().get());
    }
}

TEST_F(RmlFontEngineTest, MetricsSane){
    for (int size : {8, 16, 37}){
        const Rml::FontMetrics& metrics = engine.GetFontMetrics(handle("sans-serif", size));
        EXPECT_EQ(metrics.size, size);
        EXPECT_GT(metrics.ascent, 0.0f);
        EXPECT_GT(metrics.descent, 0.0f);
        EXPECT_GE(metrics.line_spacing, metrics.ascent + metrics.descent);
        EXPECT_GT(metrics.x_height, 0.0f);
        EXPECT_LT(metrics.x_height, metrics.ascent);
        EXPECT_GE(metrics.underline_thickness, 1.0f);
        EXPECT_GT(metrics.underline_position, 0.0f);   // below the baseline
        EXPECT_TRUE(metrics.has_ellipsis);
        // the built-in Roboto has no post table
        EXPECT_FLOAT_EQ(metrics.underline_position, size / 12.0f);
        EXPECT_FLOAT_EQ(metrics.underline_thickness, std::max(1.0f, size / 16.0f));
    }

    // a face with a post table: its own underline, positive below the baseline
    ASSERT_TRUE(engine.LoadFontFace("fonts/Roboto-Bold.ttf", 0, false, Rml::Style::FontWeight::Auto));
    std::shared_ptr<FontFace> bold = FontRegistry::find("Roboto", 700, false);
    ASSERT_TRUE(bold && bold->hasPostTable());
    const Rml::FontMetrics& metrics = engine.GetFontMetrics(handle("Roboto", 64, Rml::Style::FontWeight::Bold));
    const float s = 64.0f / bold->getUnitsPerEm();
    EXPECT_FLOAT_EQ(metrics.underline_position, -bold->getUnderlinePosition() * s);
    EXPECT_FLOAT_EQ(metrics.underline_thickness, std::max(1.0f, bold->getUnderlineThickness() * s));
    EXPECT_FLOAT_EQ(metrics.x_height, bold->getXHeight() * s);

    // an invalid handle has zero metrics, not a crash
    EXPECT_EQ(engine.GetFontMetrics(0).size, 0);
}

TEST_F(RmlFontEngineTest, EmSizeNotPixelHeight){
    // CSS font-size is the em size: Roboto's ascent + descent is 2400/2048 em, not the size
    const Rml::FontMetrics& metrics = engine.GetFontMetrics(handle("Roboto", 100));
    std::shared_ptr<FontFace> roboto = FontRegistry::getBuiltInLatin();
    const float em = (roboto->getAscender() - roboto->getDescender()) * 100.0f / roboto->getUnitsPerEm();
    EXPECT_NEAR(metrics.ascent + metrics.descent, em, 0.01f);
    EXPECT_GT(std::fabs(metrics.ascent + metrics.descent - 100.0f), 5.0f);
    EXPECT_NEAR(metrics.ascent, roboto->getAscender() * 100.0f / 2048.0f, 0.01f);
}

TEST_F(RmlFontEngineTest, StringWidthMatchesShaper){
    Rml::FontFaceHandle face = handle("sans-serif", 24);
    std::shared_ptr<FontFace> latin = FontRegistry::getBuiltInLatin();
    std::shared_ptr<FontFace> arabic = FontRegistry::getBuiltInArabic();
    std::vector<const FontFace*> faces = {latin.get(), arabic.get()};
    std::vector<float> scales = {latin->scaleForEmSize(24.0f), arabic->scaleForEmSize(24.0f)};

    for (const std::string text : {"Hello, World!", "AV Ta", "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 abc", "x"}){
        std::vector<uint32_t> codepoints;
        TextShaper::decodeUtf8(text.data(), text.size(), codepoints);
        const float expected = TextShaper::measure(faces, scales, codepoints, 0, codepoints.size(), ShapeOptions());
        EXPECT_EQ(width(face, text), (int)std::lround(expected)) << text;
        // cached: the same again
        EXPECT_EQ(width(face, text), (int)std::lround(expected)) << text;
    }
    EXPECT_EQ(width(face, ""), 0);
    EXPECT_EQ(width(0, "abc"), 0);

    // kerning off is wider for "AV"
    EXPECT_GT(engine.GetStringWidth(face, "AVAVAV", shaping(0.0f, Rml::Style::FontKerning::None)), width(face, "AVAVAV"));
}

TEST_F(RmlFontEngineTest, PriorCharacterKerning){
    Rml::FontFaceHandle face = handle("sans-serif", 48);
    // "V" after "A" is kerned closer than "V" alone; the difference belongs to the string
    EXPECT_LT(width(face, "V", 0.0f, Rml::Character('A')), width(face, "V"));
    EXPECT_EQ(width(face, "AV"), width(face, "A") + width(face, "V", 0.0f, Rml::Character('A')));
    // no pair, no change
    EXPECT_EQ(width(face, "o", 0.0f, Rml::Character(' ')), width(face, "o"));
    // never negative
    EXPECT_GE(width(face, ".", 0.0f, Rml::Character('V')), 0);
}

TEST_F(RmlFontEngineTest, LetterSpacing){
    Rml::FontFaceHandle face = handle("sans-serif", 20);
    EXPECT_NEAR(width(face, "Hello", 2.0f), width(face, "Hello") + 10, 1);
    EXPECT_NEAR(width(face, "Hello", -1.0f), width(face, "Hello") - 5, 1);

    Generated plain = generate(face, "Hello");
    Generated spaced = generate(face, "Hello", Rml::Vector2f(10, 50), Rml::ColourbPremultiplied(255, 255, 255, 255), 1.0f, 4.0f);
    EXPECT_NEAR(spaced.width, plain.width + 20, 1);
    ASSERT_EQ(spaced.quads(), 5u);
    // the last letter moved by four spacings
    EXPECT_NEAR(spaced.topLeft(4).position.x - plain.topLeft(4).position.x, 16.0f, 1.0f);
}

TEST_F(RmlFontEngineTest, GenerateStringQuads){
    Rml::FontFaceHandle face = handle("sans-serif", 24);
    const Rml::ColourbPremultiplied colour(200, 100, 50, 255);

    Generated text = generate(face, "Hi, world", Rml::Vector2f(10.3f, 50.6f), colour, 0.5f);
    // one quad per inked glyph (the space has none), one mesh per page used
    EXPECT_EQ(text.quads(), 8u);
    ASSERT_EQ(text.meshes.size(), 1u);
    EXPECT_EQ(text.width, width(face, "Hi, world"));

    const Rml::TexturedMesh& mesh = text.meshes[0];
    ASSERT_TRUE((bool)mesh.texture);
    for (const Rml::Vertex& vertex : mesh.mesh.vertices){
        // premultiplied colour times opacity, on every channel
        EXPECT_EQ(vertex.colour.red, 100);
        EXPECT_EQ(vertex.colour.green, 50);
        EXPECT_EQ(vertex.colour.blue, 25);
        EXPECT_EQ(vertex.colour.alpha, 127);
        // snapped to whole pixels at raster scale 1
        EXPECT_FLOAT_EQ(vertex.position.x, std::round(vertex.position.x));
        EXPECT_FLOAT_EQ(vertex.position.y, std::round(vertex.position.y));
        EXPECT_GE(vertex.tex_coord.x, 0.0f);
        EXPECT_LE(vertex.tex_coord.x, 1.0f);
    }

    // "H" sits on the baseline: its top is about the cap height above it
    const Rml::Vertex& h = text.topLeft(0);
    EXPECT_NEAR(h.position.x, 10.0f + 2.0f, 2.0f);
    EXPECT_LT(h.position.y, 50.6f - 12.0f);
    EXPECT_NEAR(text.bottomRight(0).position.y, 51.0f, 1.0f);
    // bitmap size = UV size in page pixels at raster scale 1
    EXPECT_NEAR((text.bottomRight(0).tex_coord.x - h.tex_coord.x) * RmlFontEngine::pageSize,
                text.bottomRight(0).position.x - h.position.x, 1e-3f);

    // the page texture is premultiplied white coverage (a, a, a, a)
    EXPECT_EQ(mesh.texture.GetDimensions(), Rml::Vector2i(RmlFontEngine::pageSize, RmlFontEngine::pageSize));
    Rml::Mesh copy = mesh.mesh;
    Rml::Geometry geometry = context->GetRenderManager().MakeGeometry(std::move(copy));
    recording.beginFrame();
    geometry.Render(Rml::Vector2f(0, 0), mesh.texture);
    ASSERT_EQ(recording.getDraws().size(), 1u);
    const RecordingRenderInterface::Texture* page = recording.getTexture(recording.getDraws()[0].texture);
    ASSERT_NE(page, nullptr);
    ASSERT_EQ(page->pixels.size(), (size_t)RmlFontEngine::pageSize * RmlFontEngine::pageSize * 4);
    size_t inked = 0;
    for (size_t i = 0; i < page->pixels.size(); i += 4){
        const uint8_t a = page->pixels[i + 3];
        ASSERT_EQ(page->pixels[i], a);
        ASSERT_EQ(page->pixels[i + 1], a);
        ASSERT_EQ(page->pixels[i + 2], a);
        inked += a > 0;
    }
    EXPECT_GT(inked, 100u);

    EXPECT_EQ(generate(0, "abc").width, 0);
}

// RmlUi hands the font engine the line feed that ends a `white-space: pre` line (ElementText's
// BuildToken keeps it in the token): control characters take no space and draw nothing, as in
// RmlUi's own font engine. They drew the missing-glyph box at the end of every <pre> line.
TEST_F(RmlFontEngineTest, ControlCharactersTakeNoSpaceAndDrawNothing){
    Rml::FontFaceHandle face = handle("sans-serif", 24);

    EXPECT_EQ(width(face, "text\n"), width(face, "text"));
    EXPECT_EQ(width(face, "\n"), 0);
    EXPECT_EQ(width(face, "a\x01" "b\x1f" "c\r\n"), width(face, "abc"));
    // a kerning pair is looked up across a control character's place, not with it
    EXPECT_EQ(width(face, "\n", 0.0f, Rml::Character('A')), 0);

    Generated plain = generate(face, "text");
    Generated withFeed = generate(face, "text\n");
    EXPECT_EQ(withFeed.quads(), plain.quads());
    EXPECT_EQ(withFeed.width, plain.width);
    EXPECT_EQ(generate(face, "\n").quads(), 0u);
    EXPECT_EQ(generate(face, "\n").width, 0);
    // letter spacing is not added for them either
    EXPECT_EQ(generate(face, "text\n", Rml::Vector2f(10, 50), Rml::ColourbPremultiplied(255, 255, 255, 255), 1.0f, 4.0f).width,
              generate(face, "text", Rml::Vector2f(10, 50), Rml::ColourbPremultiplied(255, 255, 255, 255), 1.0f, 4.0f).width);
}

// S16 R13: versions are per handle, and a page that gets new glyphs is the same texture (its
// texels are uploaded again): no text is made again for it. Only what clears a page or resets
// everything (the epoch) changes a version.
TEST_F(RmlFontEngineTest, VersionIsPerHandle){
    Rml::FontFaceHandle face = handle("sans-serif", 24);
    Rml::FontFaceHandle other = handle("sans-serif", 30);
    const int start = engine.GetVersion(face);
    const int otherStart = engine.GetVersion(other);

    generate(face, "abc");
    generate(face, "abcd");
    generate(other, "xyz");
    EXPECT_EQ(engine.GetVersion(face), start);
    EXPECT_EQ(engine.GetVersion(other), otherStart);
    // widths never touch the atlas
    width(face, "xyz");
    EXPECT_EQ(engine.GetVersion(face), start);

    // the epoch (a scale reset) moves every handle once
    engine.setRasterScale(2.0f);
    EXPECT_EQ(engine.GetVersion(face), start + 1);
    EXPECT_EQ(engine.GetVersion(other), otherStart + 1);
    EXPECT_EQ(engine.GetVersion(face), start + 1);
    engine.setRasterScale(1.0f);
}

TEST_F(RmlFontEngineTest, RasterScaleChange){
    Rml::FontFaceHandle face = handle("sans-serif", 32);
    EXPECT_FLOAT_EQ(engine.getRasterScale(), 1.0f);

    Generated one = generate(face, "H");
    ASSERT_EQ(one.quads(), 1u);
    EXPECT_EQ(engine.getPageCount(), 1u);

    const int before = engine.GetVersion(face);
    engine.setRasterScale(2.0f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(), 2.0f);
    EXPECT_EQ(engine.GetVersion(face), before + 1);
    EXPECT_EQ(engine.getPageCount(), 0u);
    // same scale again: nothing
    engine.setRasterScale(2.0f);
    EXPECT_EQ(engine.GetVersion(face), before + 1);
    // invalid scales are ignored
    engine.setRasterScale(0.0f);
    engine.setRasterScale(-1.0f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(), 2.0f);

    Generated two = generate(face, "H");
    ASSERT_EQ(two.quads(), 1u);

    // the same document size, a bitmap twice as large
    const float w1 = one.bottomRight(0).position.x - one.topLeft(0).position.x;
    const float h1 = one.bottomRight(0).position.y - one.topLeft(0).position.y;
    const float w2 = two.bottomRight(0).position.x - two.topLeft(0).position.x;
    const float h2 = two.bottomRight(0).position.y - two.topLeft(0).position.y;
    EXPECT_NEAR(w2, w1, 1.0f);
    EXPECT_NEAR(h2, h1, 1.0f);
    const float bitmap1 = (one.bottomRight(0).tex_coord.y - one.topLeft(0).tex_coord.y) * RmlFontEngine::pageSize;
    const float bitmap2 = (two.bottomRight(0).tex_coord.y - two.topLeft(0).tex_coord.y) * RmlFontEngine::pageSize;
    EXPECT_NEAR(bitmap2, bitmap1 * 2.0f, 2.0f);
    // positions snap to half document pixels (whole physical pixels)
    EXPECT_FLOAT_EQ(two.topLeft(0).position.x * 2.0f, std::round(two.topLeft(0).position.x * 2.0f));
    // layout does not change with the raster scale
    EXPECT_EQ(two.width, one.width);

    engine.setRasterScale(1.0f);
}

TEST_F(RmlFontEngineTest, PagesAppendOnly){
    Rml::FontFaceHandle face = handle("sans-serif", 40);
    Generated first = generate(face, "abc");
    ASSERT_EQ(first.quads(), 3u);
    const Rml::Vector2f uvA = first.topLeft(0).tex_coord;
    const Rml::Vector2f uvC = first.bottomRight(2).tex_coord;

    // many new glyphs later, in other sizes too
    generate(face, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    generate(handle("sans-serif", 90), "abcdefghijklmnopqrstuvwxyz");

    Generated again = generate(face, "abc");
    ASSERT_EQ(again.quads(), 3u);
    EXPECT_EQ(again.topLeft(0).tex_coord, uvA);
    EXPECT_EQ(again.bottomRight(2).tex_coord, uvC);
    EXPECT_EQ(again.topLeft(0).position, first.topLeft(0).position);
}

TEST_F(RmlFontEngineTest, PageOverflowNewPage){
    // glyphs of about 200 px: a 1024 page holds a few dozen
    Rml::FontFaceHandle big = handle("sans-serif", 280);
    Generated first = generate(big, "W");
    ASSERT_EQ(first.quads(), 1u);
    const Rml::Vector2f uvW = first.topLeft(0).tex_coord;

    generate(big, "ABCDEFGHIJKLMNOPQRSTUVXYZabcdefghijklmnopqrstuvwxyz0123456789");
    EXPECT_GE(engine.getPageCount(), 2u);
    // within the atlas budget (R13): 1 MiB per R8 page
    EXPECT_LE(engine.getPageCount() * 1024u * 1024u, RmlFontEngine::getBaseBudgetBytes());
    EXPECT_EQ(engine.getAtlasBytes(), engine.getPageCount() * 1024u * 1024u);

    // a string spanning pages: one mesh per page, every glyph drawn
    Generated spanning = generate(big, "Wz9");
    EXPECT_EQ(spanning.quads(), 3u);
    EXPECT_GE(spanning.meshes.size(), 2u);
    // the first glyph never moved
    EXPECT_EQ(generate(big, "W").topLeft(0).tex_coord, uvW);

    // a glyph larger than a page is skipped with one error, layout unaffected
    TestSystem::get().clearLogs();
    Rml::FontFaceHandle huge = handle("sans-serif", 1400);
    Generated tooLarge = generate(huge, "WW");
    EXPECT_EQ(tooLarge.quads(), 0u);
    EXPECT_GT(tooLarge.width, 1000);
    generate(huge, "M");
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "does not fit in the font atlas"), 1u);
}

TEST_F(RmlFontEngineTest, LoadFontFaceFileAndMemory){
    // from the file: family, weight and style read from it
    ASSERT_TRUE(engine.LoadFontFace("fonts/Roboto-Bold.ttf", 0, false, Rml::Style::FontWeight::Auto));
    std::vector<const FontFace*> chain = engine.getChain(handle("Roboto", 16, Rml::Style::FontWeight::Bold));
    ASSERT_FALSE(chain.empty());
    EXPECT_EQ(chain[0]->getWeight(), 700);
    // normal weight still gets the regular built-in
    EXPECT_EQ(engine.getChain(handle("Roboto", 16))[0], FontRegistry::getBuiltInLatin().get());

    // from memory (copied: the buffer can go away), registered as given
    {
        std::vector<unsigned char> bytes = readFile("fonts/Roboto-Italic.ttf");
        ASSERT_FALSE(bytes.empty());
        ASSERT_TRUE(engine.LoadFontFace(Rml::Span<const Rml::byte>(bytes.data(), bytes.size()), 0, "Memory Italic",
                                        Rml::Style::FontStyle::Italic, Rml::Style::FontWeight::Normal, false));
    }
    chain = engine.getChain(handle("memory italic", 16, Rml::Style::FontWeight::Normal, Rml::Style::FontStyle::Italic));
    ASSERT_FALSE(chain.empty());
    EXPECT_TRUE(chain[0]->isItalic());
    EXPECT_GT(width(handle("memory italic", 16), "Italic text"), 0);
    EXPECT_GT(generate(handle("memory italic", 16), "Italic").quads(), 0u);

    // the family argument overrides the file's
    ASSERT_TRUE(engine.LoadFontFace("fonts/Roboto-Bold.ttf", 0, "Renamed", Rml::Style::FontStyle::Normal,
                                    Rml::Style::FontWeight::Auto, false));
    EXPECT_TRUE(FontRegistry::hasFamily("Renamed"));
    EXPECT_EQ(FontRegistry::find("Renamed", 400, false)->getWeight(), 700);

    // HtmlUI::loadFontFace goes through the same engine (the one RmlUi uses in the editor)
    EXPECT_TRUE(HtmlUI::loadFontFace("fonts/Roboto-Italic.ttf", false, "Api Family", 300, true));
    std::shared_ptr<FontFace> api = FontRegistry::find("Api Family", 300, true);
    ASSERT_NE(api, nullptr);
    EXPECT_EQ(api->getLabel(), "fonts/Roboto-Italic.ttf");
    EXPECT_EQ(HtmlUI::getSystemInterface().getErrorCount(), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}

TEST_F(RmlFontEngineTest, LoadFontFaceInvalidFails){
    EXPECT_FALSE(engine.LoadFontFace("fonts/nope.ttf", 0, false, Rml::Style::FontWeight::Auto));
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI] Font file not found: fonts/nope.ttf"), 1u);

    EXPECT_FALSE(engine.LoadFontFace("fonts/Roboto-Bold.ttf", 3, "Bad Index", Rml::Style::FontStyle::Normal,
                                     Rml::Style::FontWeight::Auto, false));
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI] Failed to initialize font: fonts/Roboto-Bold.ttf"), 1u);
    EXPECT_FALSE(FontRegistry::hasFamily("Bad Index"));

    const char garbage[] = "definitely not a font, just bytes";
    EXPECT_FALSE(engine.LoadFontFace(Rml::Span<const Rml::byte>((const Rml::byte*)garbage, sizeof(garbage)), 0, "Garbage",
                                     Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Auto, false));
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI] Failed to initialize font: memory font 'Garbage'"), 1u);
    EXPECT_FALSE(FontRegistry::hasFamily("Garbage"));

    // a failed load changes nothing
    EXPECT_NE(handle("sans-serif", 16), 0u);
}

TEST_F(RmlFontEngineTest, ReleaseFontResources){
    Rml::FontFaceHandle face = handle("sans-serif", 24);
    generate(face, "release me");
    EXPECT_EQ(engine.getPageCount(), 1u);
    const int before = engine.GetVersion(face);

    engine.ReleaseFontResources();
    EXPECT_EQ(engine.getPageCount(), 0u);
    EXPECT_GT(engine.GetVersion(0), before);

    // faces stay registered, handles and pages come back on demand
    ASSERT_TRUE(engine.LoadFontFace("fonts/Roboto-Bold.ttf", 0, "Kept", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Bold, false));
    engine.ReleaseFontResources();
    EXPECT_TRUE(FontRegistry::hasFamily("Kept"));
    Rml::FontFaceHandle again = handle("Kept", 24, Rml::Style::FontWeight::Bold);
    ASSERT_NE(again, 0u);
    EXPECT_EQ(generate(again, "back").quads(), 4u);
    EXPECT_EQ(engine.getPageCount(), 1u);

    // Rml::ReleaseFontResources (view loss) runs it too, with documents alive
    Rml::ElementDocument* document = context->LoadDocumentFromMemory(textDocument, "[release]");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Render();
    Rml::ReleaseFontResources();
    context->Update();
    recording.beginFrame();
    context->Render();
    EXPECT_GE(engine.getPageCount(), 1u);
    EXPECT_EQ(HtmlUI::getSystemInterface().getErrorCount(), 0u);
}

TEST_F(RmlFontEngineTest, NonBmpCodepoint){
    Rml::FontFaceHandle face = handle("sans-serif", 24);
    // an emoji and a CJK character nobody covers: the primary's .notdef box, no crash
    const std::string text = "a\xf0\x9f\x98\x80\xe4\xb8\xad" "b";
    EXPECT_GT(width(face, text), width(face, "ab"));
    Generated generated = generate(face, text);
    EXPECT_EQ(generated.quads(), 4u);
    EXPECT_GT(generated.width, 0);
    // invalid UTF-8 is U+FFFD, no crash either
    EXPECT_GT(width(face, "x\xff\xfey"), 0);
}

TEST_F(RmlFontEngineTest, EmptyString){
    Rml::FontFaceHandle face = handle("sans-serif", 24);
    const int version = engine.GetVersion(face);
    EXPECT_EQ(width(face, ""), 0);
    Generated empty = generate(face, "");
    EXPECT_EQ(empty.width, 0);
    EXPECT_TRUE(empty.meshes.empty());
    // only spaces: width, no quads
    Generated spaces = generate(face, "   ");
    EXPECT_GT(spaces.width, 0);
    EXPECT_EQ(spaces.quads(), 0u);
    EXPECT_EQ(engine.GetVersion(face), version);
    EXPECT_EQ(engine.getPageCount(), 0u);
}

TEST_F(RmlFontEngineTest, DebuggerFontLoads){
    ASSERT_TRUE(Rml::Debugger::Initialise(context));
    EXPECT_TRUE(FontRegistry::hasFamily("rmlui-debugger-font"));
    std::shared_ptr<FontFace> regular = FontRegistry::find("rmlui-debugger-font", 400, false);
    std::shared_ptr<FontFace> italic = FontRegistry::find("rmlui-debugger-font", 400, true);
    ASSERT_NE(regular, nullptr);
    ASSERT_NE(italic, nullptr);
    EXPECT_NE(regular, italic);
    EXPECT_TRUE(italic->isItalic());

    Rml::Debugger::SetVisible(true);
    context->Update();
    recording.beginFrame();
    context->Render();
    EXPECT_GT(recording.getDraws().size(), 0u);
    EXPECT_GE(engine.getPageCount(), 1u);
    EXPECT_EQ(HtmlUI::getSystemInterface().getErrorCount(), 0u);
    Rml::Debugger::Shutdown();
}

TEST_F(RmlFontEngineTest, DocumentTextRenders){
    Rml::ElementDocument* document = context->LoadDocumentFromMemory(textDocument, "[text]");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    recording.beginFrame();
    context->Render();

    // laid out with real metrics: one line of 24 px Roboto per paragraph
    Rml::Element* a = document->GetElementById("a");
    Rml::Element* b = document->GetElementById("b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    const Rml::FontMetrics& metrics = engine.GetFontMetrics(handle("sans-serif", 24));
    EXPECT_NEAR(a->GetClientHeight(), metrics.line_spacing, 1.0f);
    EXPECT_NEAR(b->GetAbsoluteTop() - a->GetAbsoluteTop(), a->GetClientHeight(), 0.5f);

    // a draw made before a later element added glyphs to its page references the page's
    // previous texture, which the recording interface releases at once (RmlRenderer defers
    // it to the end of the frame); the next frame regenerates against the current one
    context->Update();
    recording.beginFrame();
    context->Render();

    size_t textured = 0;
    for (const RecordingRenderInterface::Draw& draw : recording.getDraws()){
        if (draw.texture){
            const RecordingRenderInterface::Texture* texture = recording.getTexture(draw.texture);
            ASSERT_NE(texture, nullptr);
            EXPECT_EQ(texture->dimensions, Rml::Vector2i(RmlFontEngine::pageSize, RmlFontEngine::pageSize));
            textured++;
        }
    }
    EXPECT_GE(textured, 3u);

    // the missing family warns, nothing errors, and RmlUi never lacked a face
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "font-family 'no such font' is not loaded"), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "No font face defined"), 0u);
    EXPECT_EQ(HtmlUI::getSystemInterface().getErrorCount(), 0u);

    // a second frame adds no glyph: no version bump, no new texture
    const int version = engine.GetVersion(0);
    const size_t textures = recording.getLiveTextureCount();
    context->Update();
    recording.beginFrame();
    context->Render();
    EXPECT_EQ(engine.GetVersion(0), version);
    EXPECT_EQ(recording.getLiveTextureCount(), textures);
}
