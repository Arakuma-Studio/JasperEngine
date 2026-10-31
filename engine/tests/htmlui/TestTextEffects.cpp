// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The HTML UI's `text-effect` (ui-scaling-text spec 7.4, 14): the property's parser (every function,
// its defaults, invalid values), the per-glyph transform GenerateString applies with the shared
// formulas of util/TextEffects.h (the native Text Effect's), its index across lines and elements,
// and reduced motion.

#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"
#include "FixtureRunner.h"
#include "HtmlUI.h"
#include "htmlui/HtmlTextEffects.h"
#include "htmlui/HtmlUIClock.h"
#include "htmlui/compat/runtime/CompatProperties.h"
#include "RmlFile.h"
#include "RmlFontEngine.h"
#include "RmlSystem.h"
#include "util/FontRegistry.h"
#include "util/TextEffects.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/TextShapingContext.h>
#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::htmlui;
using namespace jasperengine::test;

namespace {

    constexpr float PI = 3.14159265358979323846f;

    HtmlTextEffect parsed(const std::string& value){
        HtmlTextEffect effect;
        std::string error;
        EXPECT_TRUE(parseTextEffect(value, effect, error)) << value << ": " << error;
        return effect;
    }

    std::string parseError(const std::string& value){
        HtmlTextEffect effect;
        std::string error;
        EXPECT_FALSE(parseTextEffect(value, effect, error)) << value;
        return error;
    }

}

TEST(TextEffectProperty, EveryFunctionAndItsDefaults){
    EXPECT_TRUE(parsed("none").layers.empty());
    EXPECT_TRUE(parsed("").layers.empty());

    HtmlTextEffect wave = parsed("wave()");
    ASSERT_EQ(wave.layers.size(), 1u);
    EXPECT_EQ(wave.layers[0].type, TextEffectType::Wave);
    EXPECT_FLOAT_EQ(wave.layers[0].amplitude, 0.15f);
    EXPECT_FLOAT_EQ(wave.layers[0].duration, 1.5f);
    EXPECT_NEAR(wave.layers[0].phaseStep, 0.3f, 1e-6f);

    wave = parsed("wave(0.12em 2s 20deg)");
    EXPECT_FLOAT_EQ(wave.layers[0].amplitude, 0.12f);
    EXPECT_FLOAT_EQ(wave.layers[0].duration, 2.0f);
    EXPECT_NEAR(wave.layers[0].phaseStep, 20.0f * PI / 180.0f, 1e-5f);
    EXPECT_FALSE(wave.amplitudeInPx[0]);
    wave = parsed("wave(4px 500ms)");
    EXPECT_FLOAT_EQ(wave.layers[0].amplitude, 4.0f);
    EXPECT_TRUE(wave.amplitudeInPx[0]);
    EXPECT_FLOAT_EQ(wave.layers[0].duration, 0.5f);

    HtmlTextEffect bounce = parsed("bounce()");
    EXPECT_EQ(bounce.layers[0].type, TextEffectType::Bounce);
    EXPECT_FLOAT_EQ(bounce.layers[0].amplitude, 0.3f);
    EXPECT_FLOAT_EQ(bounce.layers[0].duration, 1.0f);
    EXPECT_FLOAT_EQ(bounce.layers[0].spread, 0.5f);

    HtmlTextEffect shake = parsed("shake(0.1em 20)");
    EXPECT_EQ(shake.layers[0].type, TextEffectType::Shake);
    EXPECT_FLOAT_EQ(shake.layers[0].amplitude, 0.1f);
    EXPECT_FLOAT_EQ(shake.layers[0].frequency, 20.0f);
    EXPECT_FLOAT_EQ(parsed("shake()").layers[0].frequency, 12.0f);

    HtmlTextEffect pulse = parsed("pulse(0.2)");
    EXPECT_EQ(pulse.layers[0].type, TextEffectType::Pulse);
    EXPECT_FLOAT_EQ(pulse.layers[0].amplitude, 0.2f);
    EXPECT_FLOAT_EQ(pulse.layers[0].duration, 1.0f);

    HtmlTextEffect typewriter = parsed("typewriter(20 0.1s)");
    EXPECT_EQ(typewriter.layers[0].type, TextEffectType::Typewriter);
    EXPECT_FLOAT_EQ(typewriter.layers[0].charactersPerSecond, 20.0f);
    EXPECT_FLOAT_EQ(typewriter.layers[0].fadeTime, 0.1f);
    EXPECT_FLOAT_EQ(parsed("typewriter()").layers[0].charactersPerSecond, 30.0f);

    EXPECT_EQ(parsed("fade-in()").layers[0].type, TextEffectType::FadeIn);
    EXPECT_FLOAT_EQ(parsed("fade-in()").layers[0].duration, 0.5f);
    EXPECT_EQ(parsed("grow(1s 0.531)").layers[0].type, TextEffectType::Grow);
    EXPECT_FLOAT_EQ(parsed("grow(1s 0.531)").layers[0].spread, 0.531f);
    EXPECT_EQ(parsed("unfold()").layers[0].type, TextEffectType::Unfold);

    HtmlTextEffect rainbow = parsed("rainbow(3s 30deg 50% 40%)");
    EXPECT_EQ(rainbow.layers[0].type, TextEffectType::Rainbow);
    EXPECT_FLOAT_EQ(rainbow.layers[0].duration, 3.0f);
    EXPECT_NEAR(rainbow.layers[0].phaseStep, 30.0f * PI / 180.0f, 1e-5f);
    EXPECT_FLOAT_EQ(rainbow.layers[0].saturation, 0.5f);
    EXPECT_FLOAT_EQ(rainbow.layers[0].lightness, 0.4f);
    EXPECT_FLOAT_EQ(parsed("rainbow()").layers[0].saturation, 0.8f);
}

TEST(TextEffectProperty, InvalidValuesAreReported){
    EXPECT_NE(parseError("wiggle()").find("unknown function"), std::string::npos);
    EXPECT_NE(parseError("wave(1s)").find("length"), std::string::npos);
    EXPECT_NE(parseError("wave(0.1em 2px)").find("time"), std::string::npos);
    EXPECT_NE(parseError("wave(0.1em 1s 1s)").find("angle"), std::string::npos);
    EXPECT_NE(parseError("shake(1em 2 3)").find("at most"), std::string::npos);
    EXPECT_NE(parseError("wave").find("not a function"), std::string::npos);

    int iterations = 0;
    EXPECT_TRUE(parseTextEffectIteration("infinite", iterations));
    EXPECT_EQ(iterations, -1);
    EXPECT_TRUE(parseTextEffectIteration("3", iterations));
    EXPECT_EQ(iterations, 3);
    EXPECT_FALSE(parseTextEffectIteration("0", iterations));
    EXPECT_FALSE(parseTextEffectIteration("2.5", iterations));
}

namespace {

    std::vector<unsigned char> ahemBytes(){
        std::ifstream file(htmluiTestSourceDirectory() + "/fonts/Ahem.ttf", std::ios::binary);
        return std::vector<unsigned char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // GenerateString with an effect state, on Ahem (each glyph a 1em square)
    class TextEffectGlyphsTest : public ::testing::Test {
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
            context = Rml::CreateContext("text-effects", Rml::Vector2i(320, 200), &recording);
            ASSERT_NE(context, nullptr);
            ASSERT_FALSE(ahem.empty());
            ASSERT_TRUE(engine.LoadFontFace(Rml::Span<const Rml::byte>(ahem.data(), ahem.size()), 0, "Ahem", Rml::Style::FontStyle::Normal,
                                            Rml::Style::FontWeight::Normal, false));
        }

        void TearDown() override{
            if (context)
                Rml::RemoveContext("text-effects");
            Rml::Shutdown();
            FontRegistry::clear();
        }

        // the quads' top-left corners of "HH HH" at y = 20 (the baseline), with an effect state
        std::vector<Rml::Vertex> generate(const HtmlTextEffectGlyphs* fx, int base, const std::string& text = "HH HH"){
            const Rml::FontFaceHandle face = engine.GetFontFaceHandle("ahem", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal, 10);
            EXPECT_NE(face, 0u);
            Rml::TextShapingContext shaping{language};
            shaping.text_effect = fx;
            shaping.glyph_index_base = base;
            Rml::TexturedMeshList meshes;
            engine.GenerateString(context->GetRenderManager(), face, 0, text, Rml::Vector2f(0, 20), Rml::ColourbPremultiplied(255, 255, 255, 255),
                                  1.0f, shaping, meshes);
            std::vector<Rml::Vertex> vertices;
            for (const Rml::TexturedMesh& mesh : meshes)
                vertices.insert(vertices.end(), mesh.mesh.vertices.begin(), mesh.mesh.vertices.end());
            return vertices;
        }
    };

}

// colours parse through RmlUi (initialised by the fixture)
TEST_F(TextEffectGlyphsTest, GradientAndLayers){
    // as compat normalizes it (colours as hex) and as written
    HtmlTextEffect gradient = parsed("gradient(2s add #ffff00 0%, #ff8800 100%)");
    ASSERT_EQ(gradient.layers.size(), 1u);
    const TextEffectLayer& layer = gradient.layers[0];
    EXPECT_EQ(layer.type, TextEffectType::Gradient);
    EXPECT_FLOAT_EQ(layer.duration, 2.0f);
    EXPECT_EQ(layer.mix, TextEffectMix::Add);
    ASSERT_EQ(layer.gradient.size(), 2u);
    EXPECT_FLOAT_EQ(layer.gradient[0].color.y, 1.0f);
    EXPECT_FLOAT_EQ(layer.gradient[1].position, 1.0f);
    EXPECT_NEAR(layer.gradient[1].color.y, 0x88 / 255.0f, 1e-4f);

    // stops spread evenly without positions; the mix defaults to multiply
    HtmlTextEffect plain = parsed("gradient(red, lime, blue)");
    EXPECT_EQ(plain.layers[0].mix, TextEffectMix::Multiply);
    ASSERT_EQ(plain.layers[0].gradient.size(), 3u);
    EXPECT_FLOAT_EQ(plain.layers[0].gradient[1].position, 0.5f);

    // the menu title of the game: Waves + an added gradient, two layers in order
    HtmlTextEffect menu = parsed("wave(0.06em 1.5s), gradient(1.5s add #ffffff, #ffd040)");
    ASSERT_EQ(menu.layers.size(), 2u);
    EXPECT_EQ(menu.layers[0].type, TextEffectType::Wave);
    EXPECT_EQ(menu.layers[1].type, TextEffectType::Gradient);

    EXPECT_NE(parseError("gradient(2s)").find("colour"), std::string::npos);
    EXPECT_NE(parseError("gradient(2s add notacolour)").find("colour"), std::string::npos);
}

TEST_F(TextEffectGlyphsTest, WaveMovesEachGlyphByItsIndexAcrossStrings){
    const std::vector<Rml::Vertex> plain = generate(nullptr, 0);
    ASSERT_EQ(plain.size(), 16u);   // four inked squares, the space has none

    HtmlTextEffect effect = parsed("wave(0.2em 2s 0.5rad)");
    HtmlTextEffectGlyphs fx;
    fx.effect = &effect;
    fx.count = 10;
    fx.em = 10.0f;

    // t = 0: y_down += A em cos(-0.5 i); this string's glyphs are characters 5..8 of the effect
    fx.time = 0.0f;
    std::vector<Rml::Vertex> moved = generate(&fx, 5);
    ASSERT_EQ(moved.size(), plain.size());
    for (int q = 0; q < 4; q++){
        const float expected = 0.2f * 10.0f * std::cos(-0.5f * (float)(5 + q));
        for (int v = 0; v < 4; v++){
            EXPECT_NEAR(moved[q * 4 + v].position.y - plain[q * 4 + v].position.y, expected, 1e-3f) << "glyph " << q;
            EXPECT_NEAR(moved[q * 4 + v].position.x, plain[q * 4 + v].position.x, 1e-4f);
        }
    }

    // T/4 later: cos(pi/2 - 0.5 i)
    fx.time = 0.5f;
    moved = generate(&fx, 0);
    for (int q = 0; q < 4; q++){
        const float expected = 0.2f * 10.0f * std::cos(PI / 2.0f - 0.5f * (float)q);
        EXPECT_NEAR(moved[q * 4].position.y - plain[q * 4].position.y, expected, 1e-3f) << "glyph " << q;
    }
}

TEST_F(TextEffectGlyphsTest, TypewriterAndReducedMotion){
    const std::vector<Rml::Vertex> plain = generate(nullptr, 0);

    // 10 characters a second: at 0.25 s the first two (indices 0, 1) show
    HtmlTextEffect typewriter = parsed("typewriter(10)");
    HtmlTextEffectGlyphs fx;
    fx.effect = &typewriter;
    fx.count = 4;
    fx.em = 10.0f;
    fx.cycleTime = 0.25f;
    std::vector<Rml::Vertex> shown = generate(&fx, 0);
    ASSERT_EQ(shown.size(), 16u);
    EXPECT_EQ(shown[0].colour.alpha, 255);
    EXPECT_EQ(shown[4].colour.alpha, 255);
    EXPECT_EQ(shown[8].colour.alpha, 0);
    EXPECT_EQ(shown[12].colour.alpha, 0);

    // reduced motion holds a wave still and keeps the colour layers
    HtmlTextEffect both = parsed("wave(0.5em 1s), gradient(1s replace #ff0000, #ff0000)");
    fx.effect = &both;
    fx.time = 0.1f;
    fx.reducedMotion = true;
    std::vector<Rml::Vertex> still = generate(&fx, 0);
    ASSERT_EQ(still.size(), plain.size());
    for (size_t v = 0; v < still.size(); v++){
        EXPECT_NEAR(still[v].position.y, plain[v].position.y, 1e-4f);
        EXPECT_EQ(still[v].colour.green, 0);
        EXPECT_EQ(still[v].colour.red, 255);
    }
}

TEST(TextEffectsShared, MatchesTheTMPEffectWavesFormula){
    // TextMesh Pro Effect: y_up -= cos(-2 pi t/T + 0.3 i) * 0.3 * pointSize * Amplitude
    const std::vector<TextEffectLayer> layers = TextEffects::fromTMPEffect("Waves", 1.5f, 0.2f, 0.531f, {}, true);
    ASSERT_EQ(layers.size(), 1u);
    for (int i = 0; i < 6; i++){
        for (float t : {0.0f, 0.375f, 0.9f}){
            TextEffectGlyphState glyph;
            for (Vector2& corner : glyph.corners) corner = Vector2(0.0f, 0.0f);
            TextEffects::applyLayer(layers[0], i, 6, t, 48.0f, false, glyph);
            const float tmpUp = -std::cos(-2.0f * PI * t / 1.5f + 0.3f * (float)i) * 48.0f * 0.3f * 0.2f;
            EXPECT_NEAR(glyph.corners[0].y, -tmpUp, 1e-3f) << i << " " << t;
        }
    }
}

namespace {

    // An RmlUi document with the engine's text-effect properties and provider
    class TextEffectDocumentTest : public TextEffectGlyphsTest {
    protected:
        Rml::ElementDocument* document = nullptr;

        void SetUp() override{
            TextEffectGlyphsTest::SetUp();
            jasperengine::htmlui::compat::CompatProperties::registerAll();
            HtmlTextEffects::install();
            HtmlUIClock::setFixedTime(10.0);
        }

        void TearDown() override{
            HtmlUIClock::setFixedTime(std::nullopt);
            HtmlTextEffects::uninstall();
            if (document)
                document->Close();
            document = nullptr;
            jasperengine::htmlui::compat::CompatProperties::reset();
            TextEffectGlyphsTest::TearDown();
        }

        void load(const std::string& body){
            const std::string rml = "<rml><head><style>body { font-family: Ahem; font-size: 10px; } h1 { display: block; }</style></head><body>" +
                                    body + "</body></rml>";
            document = context->LoadDocumentFromMemory(rml);
            ASSERT_NE(document, nullptr);
            document->Show();
            frame();
        }

        void frame(){
            HtmlTextEffects::get().update(context);
            context->Update();
            context->Render();
        }

        std::vector<Rml::ElementText*> texts(Rml::Element* root){
            std::vector<Rml::ElementText*> found;
            std::vector<Rml::Element*> stack{root};
            while (!stack.empty()){
                Rml::Element* e = stack.back();
                stack.pop_back();
                if (Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(e))
                    found.push_back(text);
                for (int i = e->GetNumChildren() - 1; i >= 0; i--)
                    stack.push_back(e->GetChild(i));
            }
            return found;
        }
    };

    struct EventCount : public Rml::EventListener {
        int count = 0;
        void ProcessEvent(Rml::Event&) override { count++; }
    };

}

TEST_F(TextEffectDocumentTest, IndicesRunOnAcrossElements){
    load("<h1 id='title' style='text-effect: wave(0.15em 1.5s)'>Hello <b>World</b></h1>");
    Rml::Element* title = document->GetElementById("title");
    ASSERT_NE(title, nullptr);
    std::vector<Rml::ElementText*> found = texts(title);
    ASSERT_EQ(found.size(), 2u);

    int first = -1;
    ASSERT_NE(HtmlTextEffects::get().GetTextEffect(*found[0], first), nullptr);
    EXPECT_EQ(first, 0);
    EXPECT_EQ(HtmlTextEffects::get().lastGlyphs()->count, 10);
    ASSERT_NE(HtmlTextEffects::get().GetTextEffect(*found[1], first), nullptr);
    // "World" goes on after the five letters of "Hello "
    EXPECT_EQ(first, 5);
    EXPECT_EQ(HtmlTextEffects::get().lastGlyphs()->count, 10);
    EXPECT_FLOAT_EQ(HtmlTextEffects::get().lastGlyphs()->em, 10.0f);

    // no effect: nothing
    load("<p id='plain'>Plain</p>");
    std::vector<Rml::ElementText*> plain = texts(document->GetElementById("plain"));
    ASSERT_EQ(plain.size(), 1u);
    EXPECT_EQ(HtmlTextEffects::get().GetTextEffect(*plain[0], first), nullptr);
}

TEST_F(TextEffectDocumentTest, TypewriterEndsWithTexteffectend){
    load("<p id='line' style='text-effect: typewriter(10); text-effect-iteration: 1'>Hi there</p>");
    Rml::Element* line = document->GetElementById("line");
    ASSERT_NE(line, nullptr);
    EventCount ended;
    line->AddEventListener("texteffectend", &ended);

    // seven characters at 10 a second: 0.7 s
    HtmlUIClock::setFixedTime(10.5);
    frame();
    EXPECT_EQ(ended.count, 0);
    HtmlUIClock::setFixedTime(10.8);
    frame();
    frame();
    EXPECT_EQ(ended.count, 1);
    // once
    HtmlUIClock::setFixedTime(12.0);
    frame();
    EXPECT_EQ(ended.count, 1);

    // restartTextEffect: from the start, and it ends again
    HtmlTextEffects::get().restart(line);
    frame();
    HtmlUIClock::setFixedTime(13.0);
    frame();
    frame();
    EXPECT_EQ(ended.count, 2);
    line->RemoveEventListener("texteffectend", &ended);
}

TEST_F(TextEffectDocumentTest, ReducedMotionReachesTheGlyphs){
    load("<p id='line' style='text-effect: bounce()'>Bounce</p>");
    std::vector<Rml::ElementText*> found = texts(document->GetElementById("line"));
    ASSERT_EQ(found.size(), 1u);
    int first = 0;
    HtmlTextEffects::get().setReducedMotion(context, true);
    ASSERT_NE(HtmlTextEffects::get().GetTextEffect(*found[0], first), nullptr);
    EXPECT_TRUE(HtmlTextEffects::get().lastGlyphs()->reducedMotion);
    HtmlTextEffects::get().setReducedMotion(context, false);
    HtmlTextEffects::get().GetTextEffect(*found[0], first);
    EXPECT_FALSE(HtmlTextEffects::get().lastGlyphs()->reducedMotion);
}
