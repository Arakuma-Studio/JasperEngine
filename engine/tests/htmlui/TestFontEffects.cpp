// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RmlFontEngine phase 1 (spec §3.13): font effects (RmlFontEffects), synthetic bold and
// italic, the per-context raster scale. CPU only, on the RecordingRenderInterface.

#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "RmlFile.h"
#include "RmlFontEffects.h"
#include "RmlFontEngine.h"
#include "RmlSystem.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"
#include "util/TextShaper.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/FontEffect.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct Generated {
        int width = 0;
        Rml::TexturedMeshList meshes;

        size_t quads(size_t mesh) const{ return meshes[mesh].mesh.indices.size() / 6; }
        size_t quads() const{
            size_t count = 0;
            for (size_t i = 0; i < meshes.size(); i++)
                count += quads(i);
            return count;
        }
        const Rml::Vertex& vertex(size_t mesh, size_t quad, int corner) const{ return meshes[mesh].mesh.vertices[quad * 4 + corner]; }
        Rml::Vector2f size(size_t mesh, size_t quad) const{ return vertex(mesh, quad, 2).position - vertex(mesh, quad, 0).position; }
        // the quad's region of its page, in page pixels
        Rml::Vector2f texels(size_t mesh, size_t quad) const{
            return (vertex(mesh, quad, 2).tex_coord - vertex(mesh, quad, 0).tex_coord) * (float)RmlFontEngine::pageSize;
        }
    };

    // a front-layer effect with its own texture, made without RmlUi's parser (fingerprint 0)
    class FrontEffect : public Rml::FontEffect {
    public:
        FrontEffect(){
            SetLayer(Layer::Front);
            SetColour(Rml::Colourb(0, 255, 0, 255));
        }
        bool HasUniqueTexture() const override{ return true; }
        bool GetGlyphMetrics(Rml::Vector2i& origin, Rml::Vector2i& dimensions, const Rml::FontGlyph&) const override{
            if (dimensions.x * dimensions.y <= 0)
                return false;
            origin -= Rml::Vector2i(1, 1);
            dimensions += Rml::Vector2i(2, 2);
            return true;
        }
        void GenerateGlyphTexture(Rml::byte* destination, Rml::Vector2i dimensions, int stride, const Rml::FontGlyph&) const override{
            for (int y = 0; y < dimensions.y; y++)
                for (int x = 0; x < dimensions.x * 4; x++)
                    destination[y * stride + x] = 255;
        }
    };

    class RmlFontEffectsTest : public ::testing::Test {
    protected:
        // declared first: render managers release through them during Rml::Shutdown
        RecordingRenderInterface recording;
        RecordingRenderInterface secondRecording;
        RmlFontEngine engine;
        Rml::Context* context = nullptr;
        Rml::Context* second = nullptr;
        Rml::String language;

        void SetUp() override{
            ASSERT_FALSE(HtmlUI::isInitialized());
            FontRegistry::clear();

            Rml::SetSystemInterface(&HtmlUI::getSystemInterface());
            Rml::SetFileInterface(&HtmlUI::getFileInterface());
            Rml::SetFontEngineInterface(&engine);
            ASSERT_TRUE(Rml::Initialise());

            context = Rml::CreateContext("font-effects", Rml::Vector2i(640, 360), &recording);
            ASSERT_NE(context, nullptr);
            // its own render manager, as a texture-mode document's context has
            second = Rml::CreateContext("font-effects-2", Rml::Vector2i(320, 180), &secondRecording);
            ASSERT_NE(second, nullptr);
            ASSERT_NE(&context->GetRenderManager(), &second->GetRenderManager());

            HtmlUI::getSystemInterface().resetCounters();
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            Rml::RemoveContext("font-effects");
            Rml::RemoveContext("font-effects-2");
            Rml::Shutdown();
            FontRegistry::clear();

            EXPECT_EQ(recording.getLiveTextureCount(), 0u);
            EXPECT_EQ(recording.getLiveGeometryCount(), 0u);
            EXPECT_EQ(secondRecording.getLiveTextureCount(), 0u);
            EXPECT_EQ(HtmlUI::getSystemInterface().getErrorCount(), 0u);
        }

        Rml::FontFaceHandle handle(const std::string& family, int size, Rml::Style::FontWeight weight = Rml::Style::FontWeight::Normal,
                                   Rml::Style::FontStyle style = Rml::Style::FontStyle::Normal){
            return engine.GetFontFaceHandle(family, style, weight, size);
        }

        // effects as RmlUi's style sheet parser instances them (a new instance per call)
        static Rml::FontEffectList parse(const std::string& value){
            Rml::PropertyDictionary properties;
            if (!Rml::StyleSheetSpecification::ParsePropertyDeclaration(properties, "font-effect", value))
                return {};
            const Rml::Property* property = properties.GetProperty(Rml::PropertyId::FontEffect);
            if (!property)
                return {};
            Rml::FontEffectsPtr effects = property->Get<Rml::FontEffectsPtr>();
            return effects ? effects->list : Rml::FontEffectList();
        }

        Rml::FontEffectsHandle prepare(Rml::FontFaceHandle face, const Rml::FontEffectList& effects){
            return engine.PrepareFontEffects(face, effects);
        }

        Generated generate(Rml::FontFaceHandle face, Rml::FontEffectsHandle effects, const std::string& text,
                           float opacity = 1.0f, Rml::Context* target = nullptr){
            Generated out;
            Rml::TextShapingContext shaping{language};
            out.width = engine.GenerateString((target ? target : context)->GetRenderManager(), face, effects, text, Rml::Vector2f(10, 50),
                                              Rml::ColourbPremultiplied(255, 255, 255, 255), opacity, shaping, out.meshes);
            return out;
        }

        int width(Rml::FontFaceHandle face, const std::string& text){
            Rml::TextShapingContext shaping{language};
            return engine.GetStringWidth(face, text, shaping);
        }

        // coverage size of one glyph of the built-in Roboto at a font size
        static Rml::Vector2i coverageSize(uint32_t codepoint, int size){
            std::shared_ptr<FontFace> roboto = FontRegistry::getBuiltInLatin();
            std::vector<unsigned char> pixels;
            int w = 0, h = 0, xoff = 0, yoff = 0;
            roboto->rasterizeGlyph(roboto->getGlyphIndex(codepoint), roboto->scaleForEmSize((float)size), pixels, w, h, xoff, yoff);
            return Rml::Vector2i(w, h);
        }
    };

}

TEST_F(RmlFontEffectsTest, PrepareCachesByFingerprint){
    Rml::FontFaceHandle face = handle("sans-serif", 24);
    EXPECT_EQ(prepare(face, {}), 0u);

    // two parses of the same declaration: other instances, the same fingerprints, one set
    Rml::FontEffectList a = parse("outline(2px red), shadow(2px 3px black)");
    Rml::FontEffectList b = parse("outline(2px red), shadow(2px 3px black)");
    ASSERT_EQ(a.size(), 2u);
    ASSERT_EQ(b.size(), 2u);
    ASSERT_NE(a[0].get(), b[0].get());
    const Rml::FontEffectsHandle handleA = prepare(face, a);
    EXPECT_NE(handleA, 0u);
    EXPECT_EQ(prepare(face, b), handleA);
    // any face
    EXPECT_EQ(prepare(handle("sans-serif", 40), b), handleA);
    EXPECT_EQ(engine.getEffectSetCount(), 1u);

    // a different colour, width or list is another set
    EXPECT_NE(prepare(face, parse("outline(2px blue), shadow(2px 3px black)")), handleA);
    EXPECT_NE(prepare(face, parse("outline(3px red), shadow(2px 3px black)")), handleA);
    EXPECT_NE(prepare(face, parse("outline(2px red)")), handleA);
    EXPECT_EQ(engine.getEffectSetCount(), 4u);

    // effects without a fingerprint are told apart by instance
    Rml::FontEffectList custom1{std::make_shared<FrontEffect>()};
    Rml::FontEffectList custom2{std::make_shared<FrontEffect>()};
    EXPECT_NE(prepare(face, custom1), prepare(face, custom2));
    EXPECT_EQ(prepare(face, custom1), prepare(face, custom1));

    // the sets outlive font resource releases (RmlUi keeps the handles), not Shutdown
    engine.ReleaseFontResources();
    EXPECT_EQ(engine.getEffectSetCount(), 6u);
    EXPECT_EQ(prepare(face, a), handleA);
}

TEST_F(RmlFontEffectsTest, RegionSizeEqualsGlyphMetrics){
    Rml::FontFaceHandle face = handle("sans-serif", 32);
    const Rml::Vector2i coverage = coverageSize('H', 32);
    ASSERT_GT(coverage.x, 0);

    struct Case { const char* value; int grow; };
    // outline/blur: width on each side; glow: outline + blur widths
    for (Case c : {Case{"outline(2px red)", 2}, Case{"blur(3px red)", 3}, Case{"glow(1px 2px 0px 0px red)", 3}}){
        Rml::FontEffectList effects = parse(c.value);
        ASSERT_EQ(effects.size(), 1u) << c.value;
        ASSERT_TRUE(effects[0]->HasUniqueTexture());

        // what the effect asks for a glyph of that coverage
        Rml::Vector2i origin(0, 0), dimensions = coverage;
        Rml::FontGlyph glyph = RmlFontEffects::wrapCoverage(nullptr, coverage.x, coverage.y, 0, 0, 0);
        ASSERT_TRUE(effects[0]->GetGlyphMetrics(origin, dimensions, glyph));
        EXPECT_EQ(dimensions, coverage + Rml::Vector2i(2 * c.grow, 2 * c.grow)) << c.value;

        Generated text = generate(face, prepare(face, effects), "H");
        // back-layer effect first, then the text; one quad each
        ASSERT_EQ(text.meshes.size(), 2u) << c.value;
        ASSERT_EQ(text.quads(0), 1u);
        ASSERT_EQ(text.quads(1), 1u);
        // the effect's atlas region and its quad have exactly that size (document px at raster scale 1)
        EXPECT_EQ(text.texels(0, 0), Rml::Vector2f(dimensions)) << c.value;
        EXPECT_EQ(text.size(0, 0), Rml::Vector2f(dimensions)) << c.value;
        // centred on the glyph: grown by `grow` on each side
        EXPECT_FLOAT_EQ(text.vertex(0, 0, 0).position.x, text.vertex(1, 0, 0).position.x - c.grow) << c.value;
        EXPECT_FLOAT_EQ(text.vertex(0, 0, 0).position.y, text.vertex(1, 0, 0).position.y - c.grow) << c.value;
        // its own RGBA8 page, not the coverage page
        EXPECT_FALSE(text.meshes[0].texture == text.meshes[1].texture);
    }
    EXPECT_EQ(engine.getPageCount(), 1u);
    EXPECT_EQ(engine.getEffectPageCount(), 1u);
}

TEST_F(RmlFontEffectsTest, EffectPagePixels){
    Rml::FontFaceHandle face = handle("sans-serif", 32);
    Generated text = generate(face, prepare(face, parse("outline(2px red)")), "H");
    ASSERT_EQ(text.meshes.size(), 2u);

    // the effect page through RmlUi: premultiplied white (r = g = b = a), the outline inked
    // around the glyph, the vertex colour tints it
    Rml::Mesh copy = text.meshes[0].mesh;
    Rml::Geometry geometry = context->GetRenderManager().MakeGeometry(std::move(copy));
    recording.beginFrame();
    geometry.Render(Rml::Vector2f(0, 0), text.meshes[0].texture);
    ASSERT_EQ(recording.getDraws().size(), 1u);
    const RecordingRenderInterface::Texture* page = recording.getTexture(recording.getDraws()[0].texture);
    ASSERT_NE(page, nullptr);
    ASSERT_EQ(page->dimensions, Rml::Vector2i(RmlFontEngine::pageSize, RmlFontEngine::pageSize));
    size_t inked = 0;
    for (size_t i = 0; i < page->pixels.size(); i += 4){
        const uint8_t a = page->pixels[i + 3];
        ASSERT_EQ(page->pixels[i], a);
        ASSERT_EQ(page->pixels[i + 1], a);
        ASSERT_EQ(page->pixels[i + 2], a);
        inked += a > 0;
    }
    // more ink than the glyph: the outline surrounds it
    const Rml::Vector2i coverage = coverageSize('H', 32);
    EXPECT_GT(inked, (size_t)(coverage.x * coverage.y) / 2);

    for (const Rml::Vertex& vertex : text.meshes[0].mesh.vertices){
        EXPECT_EQ(vertex.colour.red, 255);
        EXPECT_EQ(vertex.colour.green, 0);
        EXPECT_EQ(vertex.colour.alpha, 255);
    }
}

// S16: effect glyphs go into their pages in place (the renderer uploads the page again): no text is
// made again for a new effect glyph, the handle's version stays
TEST_F(RmlFontEffectsTest, EffectGlyphsKeepTheVersion){
    Rml::FontFaceHandle face = handle("sans-serif", 24);
    const Rml::FontEffectsHandle outline = prepare(face, parse("outline(1px black)"));

    generate(face, 0, "a");
    const int start = engine.GetVersion(face);
    EXPECT_EQ(engine.getEffectPageCount(), 0u);

    // the base glyph exists, the effect glyph is new: it gets a page
    generate(face, outline, "a");
    EXPECT_EQ(engine.getEffectPageCount(), 1u);
    generate(face, outline, "aaa");
    generate(face, 0, "a");
    generate(face, outline, "b");
    generate(face, prepare(face, parse("outline(2px black)")), "b");
    // a shadow makes no glyphs of its own
    generate(face, prepare(face, parse("shadow(1px 1px black)")), "ab");
    EXPECT_EQ(engine.GetVersion(face), start);
    EXPECT_EQ(engine.getEffectPageCount(), 1u);

    // a space has no ink: no effect quad, no failure
    Generated space = generate(face, outline, " ");
    EXPECT_EQ(space.quads(), 0u);
    EXPECT_GT(space.width, 0);
}

TEST_F(RmlFontEffectsTest, ShadowSharesBaseGlyphs){
    Rml::FontFaceHandle face = handle("sans-serif", 24);
    Generated plain = generate(face, 0, "Hi");
    Generated text = generate(face, prepare(face, parse("shadow(2px 3px #000000)")), "Hi", 0.5f);
    ASSERT_EQ(text.meshes.size(), 2u);
    EXPECT_EQ(text.width, plain.width);
    // the shadow is the base glyphs, offset, from the same page
    EXPECT_TRUE(text.meshes[0].texture == text.meshes[1].texture);
    ASSERT_EQ(text.quads(0), 2u);
    for (size_t quad = 0; quad < 2; quad++){
        for (int corner = 0; corner < 4; corner++){
            EXPECT_EQ(text.vertex(0, quad, corner).position, text.vertex(1, quad, corner).position + Rml::Vector2f(2, 3));
            EXPECT_EQ(text.vertex(0, quad, corner).tex_coord, text.vertex(1, quad, corner).tex_coord);
            EXPECT_EQ(text.vertex(1, quad, corner).position, plain.vertex(0, quad, corner).position);
        }
    }
    // the effect colour, premultiplied with the opacity; the text keeps its own
    EXPECT_EQ(text.vertex(0, 0, 0).colour.alpha, 127);
    EXPECT_EQ(text.vertex(0, 0, 0).colour.red, 0);
    EXPECT_EQ(text.vertex(1, 0, 0).colour.red, 127);
}

TEST_F(RmlFontEffectsTest, LayerOrder){
    Rml::FontFaceHandle face = handle("sans-serif", 24);
    // RmlUi's parser puts back effects first; a front effect (made in code) draws after the text
    Rml::FontEffectList list = parse("shadow(1px 1px blue), outline(1px red)");
    ASSERT_EQ(list.size(), 2u);
    list.insert(list.begin(), std::make_shared<FrontEffect>());

    Generated text = generate(face, prepare(face, list), "H");
    ASSERT_EQ(text.meshes.size(), 4u);
    // shadow (blue), outline (red), the text (white), the front effect (green)
    EXPECT_EQ(text.vertex(0, 0, 0).colour, Rml::ColourbPremultiplied(0, 0, 255, 255));
    EXPECT_EQ(text.vertex(1, 0, 0).colour, Rml::ColourbPremultiplied(255, 0, 0, 255));
    EXPECT_EQ(text.vertex(2, 0, 0).colour, Rml::ColourbPremultiplied(255, 255, 255, 255));
    EXPECT_EQ(text.vertex(3, 0, 0).colour, Rml::ColourbPremultiplied(0, 255, 0, 255));
    EXPECT_TRUE(text.meshes[0].texture == text.meshes[2].texture);
    EXPECT_TRUE(text.meshes[1].texture == text.meshes[3].texture);
    EXPECT_EQ(text.size(3, 0), text.size(2, 0) + Rml::Vector2f(2, 2));
}

TEST_F(RmlFontEffectsTest, EmboldenBitmap){
    // one inked pixel spreads right and up
    std::vector<unsigned char> pixels{200};
    int w = 1, h = 1, yoff = -5;
    RmlFontEffects::embolden(pixels, w, h, yoff, 2);
    EXPECT_EQ(w, 3);
    EXPECT_EQ(h, 3);
    EXPECT_EQ(yoff, -7);
    ASSERT_EQ(pixels.size(), 9u);
    for (unsigned char p : pixels)
        EXPECT_EQ(p, 200);

    // a column of two: the bottom stays on the same row (yoff + height unchanged)
    std::vector<unsigned char> column{10, 0, 20, 0};   // 2x2, left column inked
    w = 2; h = 2; yoff = -2;
    RmlFontEffects::embolden(column, w, h, yoff, 1);
    EXPECT_EQ(w, 3);
    EXPECT_EQ(h, 3);
    EXPECT_EQ(yoff + h, -2 + 2);
    const std::vector<unsigned char> expected{10, 10, 0,
                                              20, 20, 0,
                                              20, 20, 0};
    EXPECT_EQ(column, expected);

    // nothing to do
    std::vector<unsigned char> none;
    w = 0; h = 0; yoff = 0;
    RmlFontEffects::embolden(none, w, h, yoff, 3);
    EXPECT_EQ(w, 0);

    EXPECT_FLOAT_EQ(RmlFontEffects::boldStrength(12), 1.0f);
    EXPECT_FLOAT_EQ(RmlFontEffects::boldStrength(48), 2.0f);
}

TEST_F(RmlFontEffectsTest, SyntheticBold){
    // only the built-in Roboto Regular: bold is synthesized
    Rml::FontFaceHandle regular = handle("Roboto", 48);
    Rml::FontFaceHandle bold = handle("Roboto", 48, Rml::Style::FontWeight::Bold);
    ASSERT_EQ(engine.getChain(bold)[0], engine.getChain(regular)[0]);

    // the advances stay the regular face's (Chromium's fake bold: the outline grows, the advance does not)
    EXPECT_EQ(width(bold, "Hello"), width(regular, "Hello"));
    Generated r = generate(regular, 0, "Hello");
    Generated b = generate(bold, 0, "Hello");
    EXPECT_EQ(b.width, width(bold, "Hello"));
    ASSERT_EQ(b.quads(), 5u);
    // a bitmap 2 px wider and taller, on the same baseline
    EXPECT_EQ(b.size(0, 0), r.size(0, 0) + Rml::Vector2f(2, 2));
    EXPECT_EQ(b.vertex(0, 0, 2).position.y, r.vertex(0, 0, 2).position.y);
    // later glyphs at the regular face's positions
    EXPECT_NEAR(b.vertex(0, 4, 0).position.x - r.vertex(0, 4, 0).position.x, 0.0f, 1.0f);

    // a weight of 600 on a 400 face: < 300 lighter, no synthesis
    EXPECT_EQ(width(handle("Roboto", 48, Rml::Style::FontWeight(600)), "Hello"), width(regular, "Hello"));

    // with the real bold face: no synthesis
    ASSERT_TRUE(engine.LoadFontFace("fonts/Roboto-Bold.ttf", 0, false, Rml::Style::FontWeight::Auto));
    Rml::FontFaceHandle real = handle("Roboto", 48, Rml::Style::FontWeight::Bold);
    std::vector<const FontFace*> chain = engine.getChain(real);
    ASSERT_EQ(chain[0]->getWeight(), 700);
    std::vector<uint32_t> codepoints;
    TextShaper::decodeUtf8("Hello", 5, codepoints);
    std::vector<float> scales;
    for (const FontFace* face : chain)
        scales.push_back(face->scaleForEmSize(48.0f));
    EXPECT_EQ(width(real, "Hello"), (int)std::lround(TextShaper::measure(chain, scales, codepoints, 0, codepoints.size(), ShapeOptions())));
}

TEST_F(RmlFontEffectsTest, SyntheticItalic){
    Rml::FontFaceHandle regular = handle("Roboto", 48);
    Rml::FontFaceHandle italic = handle("Roboto", 48, Rml::Style::FontWeight::Normal, Rml::Style::FontStyle::Italic);
    ASSERT_FALSE(engine.getChain(italic)[0]->isItalic());

    // layout unchanged, quads sheared by tan 12deg around the baseline
    EXPECT_EQ(width(italic, "Hello"), width(regular, "Hello"));
    Generated r = generate(regular, 0, "H");
    Generated i = generate(italic, 0, "H");
    ASSERT_EQ(i.quads(), 1u);
    const float top = i.vertex(0, 0, 0).position.y;
    const float bottom = i.vertex(0, 0, 2).position.y;
    EXPECT_FLOAT_EQ(i.vertex(0, 0, 0).position.x, r.vertex(0, 0, 0).position.x + (50.0f - top) * RmlFontEffects::italicShear);
    EXPECT_FLOAT_EQ(i.vertex(0, 0, 3).position.x, r.vertex(0, 0, 3).position.x + (50.0f - bottom) * RmlFontEffects::italicShear);
    EXPECT_GT(i.vertex(0, 0, 0).position.x - i.vertex(0, 0, 3).position.x, 6.0f);
    // the same bitmap as the upright text
    EXPECT_EQ(i.texels(0, 0), r.texels(0, 0));

    // effects follow the shear
    Generated outlined = generate(italic, prepare(italic, parse("outline(2px red)")), "H");
    ASSERT_EQ(outlined.meshes.size(), 2u);
    EXPECT_GT(outlined.vertex(0, 0, 0).position.x, outlined.vertex(0, 0, 3).position.x);

    // with a real italic face: no shear
    ASSERT_TRUE(engine.LoadFontFace("fonts/Roboto-Italic.ttf", 0, false, Rml::Style::FontWeight::Auto));
    Rml::FontFaceHandle real = handle("Roboto", 48, Rml::Style::FontWeight::Normal, Rml::Style::FontStyle::Italic);
    ASSERT_TRUE(engine.getChain(real)[0]->isItalic());
    Generated realText = generate(real, 0, "l");
    ASSERT_EQ(realText.quads(), 1u);
    EXPECT_FLOAT_EQ(realText.vertex(0, 0, 0).position.x, realText.vertex(0, 0, 3).position.x);
}

TEST_F(RmlFontEffectsTest, PerContextRasterScale){
    Rml::RenderManager& first = context->GetRenderManager();
    Rml::RenderManager& other = second->GetRenderManager();
    Rml::FontFaceHandle face = handle("sans-serif", 32);
    const Rml::FontEffectsHandle outline = prepare(face, parse("outline(2px red)"));

    // the default scale (phase 0's global setter) is what managers without their own use
    EXPECT_FLOAT_EQ(engine.getRasterScale(first), 1.0f);
    engine.setRasterScale(1.5f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(first), 1.5f);
    engine.setRasterScale(1.0f);

    Generated one = generate(face, outline, "H");
    const int version = engine.GetVersion(face);

    // one context at 2x: a bump for its text, nothing dropped
    engine.setRasterScale(first, 2.0f);
    EXPECT_EQ(engine.GetVersion(face), version + 1);
    EXPECT_FLOAT_EQ(engine.getRasterScale(first), 2.0f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(other), 1.0f);
    EXPECT_EQ(engine.getPageCount(), 1u);
    // the same again, or the default for the other: no bump
    engine.setRasterScale(first, 2.0f);
    engine.setRasterScale(other, 1.0f);
    EXPECT_EQ(engine.GetVersion(face), version + 1);
    // invalid scales are ignored
    engine.setRasterScale(first, 0.0f);
    engine.setRasterScale(first, -2.0f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(first), 2.0f);

    // both contexts draw the same frame: each its own glyphs, from shared pages
    Generated two = generate(face, outline, "H");
    Generated otherOne = generate(face, outline, "H", 1.0f, second);
    ASSERT_EQ(two.meshes.size(), 2u);
    ASSERT_EQ(otherOne.meshes.size(), 2u);
    // the base glyph: twice the bitmap, the same document size
    EXPECT_NEAR(two.texels(1, 0).y, one.texels(1, 0).y * 2.0f, 2.0f);
    EXPECT_NEAR(two.size(1, 0).y, one.size(1, 0).y, 1.0f);
    EXPECT_EQ(otherOne.texels(1, 0), one.texels(1, 0));
    // effect glyphs are document resolution at any scale: one glyph serves both contexts
    EXPECT_EQ(two.texels(0, 0), one.texels(0, 0));
    EXPECT_EQ(two.size(0, 0), one.size(0, 0));
    EXPECT_EQ(two.vertex(0, 0, 0).tex_coord, otherOne.vertex(0, 0, 0).tex_coord);
    // layout does not change with the raster scale
    EXPECT_EQ(two.width, one.width);
    EXPECT_EQ(engine.getPageCount(), 1u);
    EXPECT_EQ(engine.getEffectPageCount(), 1u);

    // a context's own scale wins over the default
    engine.setRasterScale(1.5f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(other), 1.0f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(first), 2.0f);
    engine.setRasterScale(1.0f);
}

TEST_F(RmlFontEffectsTest, DocumentWithEffectsRenders){
    Rml::ElementDocument* document = context->LoadDocumentFromMemory(R"(
        <rml><head><style>
            body { display: block; font-family: sans-serif; font-size: 20px; color: #ffffff;
                   font-effect: shadow(2px 2px black), outline(1px #ff0000); }
            span { font-weight: bold; font-style: italic; font-effect: glow(2px 4px 0px 0px #00ff00); }
        </style></head>
        <body>effects <span>bold glow</span></body></rml>)", "[effects]");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    recording.beginFrame();
    context->Render();

    // every draw has a texture: the base page or the effect page; both were used
    std::set<Rml::TextureHandle> textures;
    for (const RecordingRenderInterface::Draw& draw : recording.getDraws()){
        EXPECT_NE(draw.texture, 0u);
        textures.insert(draw.texture);
    }
    EXPECT_GE(textures.size(), 2u);
    EXPECT_EQ(engine.getEffectPageCount(), 1u);
    // effects are supported: no warning
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u);
    EXPECT_EQ(HtmlUI::getSystemInterface().getWarningCount(), 0u);

    document->Close();
    context->Update();
}

// (upstream) per-context raster density, read by the production shape decorators too: independent
// render managers keep their own scale
TEST(RmlFontEffectsRasterScale, PerContextRasterScale){
    ASSERT_FALSE(HtmlUI::isInitialized());
    FontRegistry::clear();
    RecordingRenderInterface firstRenderer, otherRenderer;
    RmlFontEngine engine;
    Rml::SetSystemInterface(&HtmlUI::getSystemInterface());
    Rml::SetFileInterface(&HtmlUI::getFileInterface());
    Rml::SetFontEngineInterface(&engine);
    ASSERT_TRUE(Rml::Initialise());
    Rml::Context* first = Rml::CreateContext("font-scale-first", Rml::Vector2i(640, 360), &firstRenderer);
    Rml::Context* other = Rml::CreateContext("font-scale-other", Rml::Vector2i(320, 180), &otherRenderer);
    ASSERT_TRUE(first && other);
    engine.setRasterScale(first->GetRenderManager(), 2.0f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(first->GetRenderManager()), 2.0f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(other->GetRenderManager()), 1.0f);
    engine.setRasterScale(other->GetRenderManager(), 1.5f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(first->GetRenderManager()), 2.0f);
    EXPECT_FLOAT_EQ(engine.getRasterScale(other->GetRenderManager()), 1.5f);
    Rml::RemoveContext("font-scale-first");
    Rml::RemoveContext("font-scale-other");
    Rml::Shutdown();
    FontRegistry::clear();
}
