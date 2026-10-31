// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Native Text: Text Effect (ui-scaling-text spec 7.3, 14) through UISystem, the shared formulas
// (util/TextEffects.h, TMP Effect's), and the SDF atlas of outlines and shadows (7.2): only
// when one is on, plain texts as before, visibleCharacters.

#include <gtest/gtest.h>

#include "Engine.h"
#include "Scene.h"
#include "component/TextComponent.h"
#include "component/TextEffectComponent.h"
#include "component/UIComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Transform.h"
#include "component/CameraComponent.h"
#include "subsystem/UISystem.h"
#include "pool/ShaderPool.h"
#include "util/STBText.h"
#include "util/TextEffects.h"
#include "object/ui/TextEffect.h"
#include "Stream.h"

#include <cmath>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr float PI = 3.14159265358979323846f;

    TextEffectLayer layerOf(TextEffectType type){
        TextEffectLayer layer;
        layer.type = type;
        return layer;
    }

    // a Text (and optionally a Text Effect) laid out by UISystem without a GPU
    struct TextScene {
        Scene scene;
        Entity entity;

        explicit TextScene(const std::string& text, bool effect = true, unsigned int fontSize = 20){
            // a UI camera, as a loaded UI scene has
            Entity camera = scene.createEntity();
            scene.addComponent<Transform>(camera, Transform());
            CameraComponent cameraComponent;
            cameraComponent.type = CameraType::CAMERA_UI;
            scene.addComponent<CameraComponent>(camera, cameraComponent);
            scene.setCamera(camera);

            entity = scene.createEntity();
            scene.addComponent<Transform>(entity, Transform());
            scene.addComponent<RectTransformComponent>(entity, RectTransformComponent());
            scene.addComponent<UIComponent>(entity, UIComponent());
            TextComponent component;
            component.text = text;
            component.fontSize = fontSize;
            scene.addComponent<TextComponent>(entity, component);
            if (effect)
                scene.addComponent<TextEffectComponent>(entity, TextEffectComponent());
        }

        UISystem& ui() { return *scene.getSystem<UISystem>(); }
        TextComponent& text() { return scene.getComponent<TextComponent>(entity); }
        UIComponent& uiComponent() { return scene.getComponent<UIComponent>(entity); }
        TextEffectComponent& effect() { return scene.getComponent<TextEffectComponent>(entity); }

        void update(double dt){ ui().update(dt); }

        std::vector<Vector3> positions(){
            Buffer& buffer = uiComponent().buffer;
            Attribute* attribute = buffer.getAttribute(AttributeType::POSITION);
            std::vector<Vector3> out;
            for (unsigned int i = 0; attribute && i < attribute->getCount(); i++)
                out.push_back(buffer.getVector3(attribute, i));
            return out;
        }

        std::vector<Vector4> colors(){
            Buffer& buffer = uiComponent().buffer;
            Attribute* attribute = buffer.getAttribute(AttributeType::COLOR);
            std::vector<Vector4> out;
            for (unsigned int i = 0; attribute && i < attribute->getCount(); i++)
                out.push_back(buffer.getVector4(attribute, i));
            return out;
        }
    };

}

// ---- the formulas -----------------------------------------------------------------------------

TEST(TextEffectFormulas, WaveAtZeroAndAQuarterPeriod){
    TextEffectLayer wave = layerOf(TextEffectType::Wave);
    wave.amplitude = 0.2f;
    wave.duration = 2.0f;
    wave.phaseStep = 0.3f;
    for (int i = 0; i < 5; i++){
        for (float t : {0.0f, 0.5f}){
            TextEffectGlyphState glyph;
            for (Vector2& c : glyph.corners) c = Vector2(10.0f, 10.0f);
            TextEffects::applyLayer(wave, i, 5, t, 30.0f, false, glyph);
            // y down: + A em cos(2 pi t/T - 0.3 i)
            const float expected = 0.2f * 30.0f * std::cos(2.0f * PI * t / 2.0f - 0.3f * (float)i);
            EXPECT_NEAR(glyph.corners[0].y - 10.0f, expected, 1e-4f);
            EXPECT_NEAR(glyph.corners[2].x, 10.0f, 1e-5f);
        }
    }
}

TEST(TextEffectFormulas, OneShotProgressAsTMPEffect){
    // TMP Effect: start = lerp(i/N, 0, r), length = lerp(1/N, 1, r), q = clamp01((p - start) / length)
    const float r = 0.531f;
    const int n = 5;
    for (int i = 0; i < n; i++){
        for (float p : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}){
            const float start = (float)i / n * (1.0f - r);
            const float length = 1.0f / n + (1.0f - 1.0f / n) * r;
            const float expected = std::clamp((p - start) / length, 0.0f, 1.0f);
            EXPECT_NEAR(TextEffects::oneShotProgress(i, n, p, r), expected, 1e-6f) << i << " " << p;
        }
    }
    EXPECT_FLOAT_EQ(TextEffects::oneShotProgress(4, 5, 1.0f, r), 1.0f);
    EXPECT_FLOAT_EQ(TextEffects::oneShotProgress(4, 5, 0.0f, r), 0.0f);
}

TEST(TextEffectFormulas, GrowFromTheBottomAndUnfoldFromTheMiddle){
    auto box = [](){
        TextEffectGlyphState g;
        g.corners[0] = Vector2(0, 0); g.corners[1] = Vector2(10, 0); g.corners[2] = Vector2(10, 20); g.corners[3] = Vector2(0, 20);
        return g;
    };
    TextEffectLayer grow = layerOf(TextEffectType::Grow);
    grow.duration = 1.0f;
    grow.spread = 1.0f;   // all at once: q = p
    TextEffectGlyphState g = box();
    TextEffects::applyLayer(grow, 0, 1, 0.25f, 20.0f, false, g);
    EXPECT_NEAR(g.corners[0].y, 15.0f, 1e-4f);   // the top a quarter up from the bottom
    EXPECT_NEAR(g.corners[3].y, 20.0f, 1e-4f);

    TextEffectLayer unfold = layerOf(TextEffectType::Unfold);
    unfold.duration = 1.0f;
    unfold.spread = 1.0f;
    g = box();
    TextEffects::applyLayer(unfold, 0, 1, 0.5f, 20.0f, false, g);
    EXPECT_NEAR(g.corners[0].y, 5.0f, 1e-4f);
    EXPECT_NEAR(g.corners[3].y, 15.0f, 1e-4f);

    // reduced motion: neither moves
    g = box();
    TextEffects::applyLayer(unfold, 0, 1, 0.5f, 20.0f, true, g);
    EXPECT_NEAR(g.corners[0].y, 0.0f, 1e-6f);
}

TEST(TextEffectFormulas, ColoursCompose){
    TextEffectLayer gradient = layerOf(TextEffectType::Gradient);
    gradient.spread = 1.0f;
    gradient.duration = 1.0f;
    gradient.mix = TextEffectMix::Add;
    gradient.gradient = {{Vector4(0.2f, 0.0f, 0.0f, 0.5f), 0.0f}, {Vector4(0.2f, 0.0f, 0.0f, 0.5f), 1.0f}};
    TextEffectGlyphState g;
    g.color = Vector4(0.5f, 0.5f, 0.5f, 1.0f);
    TextEffects::applyLayer(gradient, 0, 1, 0.3f, 20.0f, false, g);
    // TMP Add: rgb added, alpha multiplied
    EXPECT_NEAR(g.color.x, 0.7f, 1e-5f);
    EXPECT_NEAR(g.color.y, 0.5f, 1e-5f);
    EXPECT_NEAR(g.color.w, 0.5f, 1e-5f);

    EXPECT_NEAR(TextEffects::hsl(0.0f, 1.0f, 0.5f).x, 1.0f, 1e-5f);
    EXPECT_NEAR(TextEffects::hsl(1.0f / 3.0f, 1.0f, 0.5f).y, 1.0f, 1e-5f);
}

TEST(TextEffectFormulas, TMPEffectImport){
    const std::vector<TextEffectGradientStop> gold = {{Vector4(1.0f, 0.8f, 0.2f, 1.0f), 0.0f}, {Vector4(1.0f, 1.0f, 1.0f, 1.0f), 1.0f}};
    std::vector<TextEffectLayer> layers = TextEffects::fromTMPEffect("Waves", 1.5f, 0.4f, 0.531f, gold, true);
    ASSERT_EQ(layers.size(), 2u);
    EXPECT_EQ(layers[0].type, TextEffectType::Wave);
    EXPECT_FLOAT_EQ(layers[0].amplitude, 0.12f);   // 0.3 x 0.4
    EXPECT_FLOAT_EQ(layers[0].duration, 1.5f);
    EXPECT_EQ(layers[1].type, TextEffectType::Gradient);
    EXPECT_EQ(layers[1].mix, TextEffectMix::Add);
    EXPECT_FLOAT_EQ(layers[1].spread, 0.531f);
    // a white gradient colours nothing
    EXPECT_EQ(TextEffects::fromTMPEffect("Grow", 1.0f, 1.0f, 0.5f, {{Vector4(1, 1, 1, 1), 0.0f}}, false).size(), 1u);
}

// ---- the component through UISystem -------------------------------------------------------

TEST(TextEffectComponentTest, WaveMovesTheGlyphsAndStopRestoresThem){
    TextScene t("Hello", false);
    t.update(0.0);
    const std::vector<Vector3> plain = t.positions();
    ASSERT_EQ(plain.size(), 20u);
    EXPECT_FALSE(t.uiComponent().buffer.getAttribute(AttributeType::COLOR));

    // the effect: a colour attribute, the glyphs from the same base
    t.scene.addComponent<TextEffectComponent>(t.entity, TextEffectComponent());
    TextEffectComponent& effect = t.effect();
    effect.layers[0] = layerOf(TextEffectType::Wave);
    effect.layers[0].amplitude = 0.2f;
    effect.layers[0].duration = 2.0f;
    effect.useUnscaledTime = false;
    t.update(0.0);
    t.update(0.0);
    ASSERT_TRUE(t.uiComponent().buffer.getAttribute(AttributeType::COLOR));
    EXPECT_TRUE(t.uiComponent().vertexColorAbsolute);
    std::vector<Vector3> moved = t.positions();
    ASSERT_EQ(moved.size(), plain.size());
    for (size_t q = 0; q < 5; q++){
        const float expected = 0.2f * 20.0f * std::cos(-0.3f * (float)q);
        EXPECT_NEAR(moved[q * 4].y - plain[q * 4].y, expected, 1e-3f) << q;
        EXPECT_NEAR(moved[q * 4].x, plain[q * 4].x, 1e-4f);
    }

    // half a period later: cos(pi - 0.3 i)
    t.update(1.0);
    moved = t.positions();
    EXPECT_NEAR(moved[0].y - plain[0].y, 0.2f * 20.0f * std::cos(PI), 1e-3f);

    // stop: the plain text again
    TextEffect(&t.scene, t.entity).stop();
    t.update(0.0);
    moved = t.positions();
    for (size_t v = 0; v < plain.size(); v++)
        EXPECT_NEAR(moved[v].y, plain[v].y, 1e-4f);
}

TEST(TextEffectComponentTest, TypewriterRevealsAndCompletes){
    TextScene t("Hi there");
    TextEffectComponent& effect = t.effect();
    effect.layers[0] = layerOf(TextEffectType::Typewriter);
    effect.layers[0].charactersPerSecond = 10.0f;
    effect.loop = false;
    effect.useUnscaledTime = false;
    std::vector<int> revealed;
    int completed = 0;
    effect.onCharacterRevealed.add("test", [&](int index){ revealed.push_back(index); });
    effect.onComplete.add("test", [&](){ completed++; });

    t.update(0.0);
    t.update(0.25);
    // 0.25 s at 10 a second: two characters
    EXPECT_EQ(revealed, (std::vector<int>{0, 1}));
    std::vector<Vector4> colors = t.colors();
    ASSERT_EQ(colors.size(), 32u);   // 8 glyph quads, the space one without ink
    EXPECT_FLOAT_EQ(colors[0].w, 1.0f);
    EXPECT_FLOAT_EQ(colors[4].w, 1.0f);
    EXPECT_FLOAT_EQ(colors[12].w, 0.0f);   // "t" (the space's quad is the third)
    EXPECT_EQ(completed, 0);

    // seven visible characters: done at 0.7 s, once
    t.update(0.5);
    EXPECT_EQ(revealed.size(), 7u);
    EXPECT_EQ(completed, 1);
    EXPECT_TRUE(t.effect().finished);
    t.update(1.0);
    EXPECT_EQ(completed, 1);

    // a new text starts it again
    t.text().text = "Bye";
    t.text().needUpdateText = true;
    revealed.clear();
    t.update(0.0);
    t.update(0.15);
    EXPECT_EQ(revealed, (std::vector<int>{0}));
}

TEST(TextEffectComponentTest, RangeAndWordsSelectTheCharacters){
    TextScene t("one two three");
    TextEffectComponent& effect = t.effect();
    effect.layers[0] = layerOf(TextEffectType::Wave);
    effect.layers[0].amplitude = 0.5f;
    effect.layers[0].phaseStep = 0.0f;
    effect.useUnscaledTime = false;
    effect.startIndex = 4;
    effect.length = 3;   // "two"
    t.update(0.0);
    t.update(0.0);
    const std::vector<Vector3>& base = t.effect().basePositions;
    std::vector<Vector3> moved = t.positions();
    auto shifted = [&](size_t glyph){ return std::fabs(moved[glyph * 4].y - base[glyph * 4].y) > 1e-3f; };
    for (size_t g : {0u, 1u, 2u, 8u, 9u}) EXPECT_FALSE(shifted(g)) << g;
    for (size_t g : {4u, 5u, 6u}) EXPECT_TRUE(shifted(g)) << g;

    // forWords overrides the range: whole words only ("on" is not "one")
    effect.forWords = "three on";
    effect.needApply = true;
    t.update(0.0);
    moved = t.positions();
    for (size_t g : {0u, 4u, 6u}) EXPECT_FALSE(shifted(g)) << g;
    for (size_t g : {8u, 12u}) EXPECT_TRUE(shifted(g)) << g;

    // past the end: clamped, nothing breaks
    effect.forWords.clear();
    effect.startIndex = 50;
    effect.needApply = true;
    t.update(0.0);
    moved = t.positions();
    for (size_t g = 0; g < 13; g++) EXPECT_FALSE(shifted(g)) << g;
}

TEST(TextEffectComponentTest, LoopRestartsAndTimeScales){
    TextScene t("Loop");
    TextEffectComponent& effect = t.effect();
    effect.layers[0] = layerOf(TextEffectType::FadeIn);
    effect.layers[0].duration = 0.5f;
    effect.loop = true;
    effect.useUnscaledTime = true;
    int completed = 0;
    effect.onComplete.add("test", [&](){ completed++; });

    Engine::setTimeScale(0.5f);
    t.update(0.0);
    // scaled 0.15 a frame is 0.3 unscaled: complete after two frames, again after two more
    t.update(0.15);
    t.update(0.15);
    EXPECT_EQ(completed, 1);
    EXPECT_FALSE(t.effect().finished);   // looping: started again
    t.update(0.15);
    t.update(0.15);
    EXPECT_EQ(completed, 2);

    // scaled time: half as fast
    TextScene s("Loop");
    s.effect().layers[0] = effect.layers[0];
    s.effect().useUnscaledTime = false;
    int scaledCompleted = 0;
    s.effect().onComplete.add("test", [&](){ scaledCompleted++; });
    s.update(0.0);
    for (int i = 0; i < 3; i++) s.update(0.15);
    EXPECT_EQ(scaledCompleted, 0);
    s.update(0.15);
    EXPECT_EQ(scaledCompleted, 1);
    Engine::setTimeScale(1.0f);
}

TEST(TextEffectComponentTest, LayersComposeAndEditModeHolds){
    TextScene t("AB");
    TextEffectComponent& effect = t.effect();
    effect.layers[0] = layerOf(TextEffectType::Gradient);
    effect.layers[0].mix = TextEffectMix::Replace;
    effect.layers[0].gradient = {{Vector4(1.0f, 0.0f, 0.0f, 1.0f), 0.0f}};
    effect.layers[1] = layerOf(TextEffectType::FadeIn);
    effect.layers[1].duration = 1.0f;
    effect.layers[1].spread = 1.0f;
    effect.useUnscaledTime = false;
    t.update(0.0);
    t.update(0.5);
    std::vector<Vector4> colors = t.colors();
    ASSERT_FALSE(colors.empty());
    EXPECT_NEAR(colors[0].x, 1.0f, 1e-4f);   // red (linear 1)
    EXPECT_NEAR(colors[0].y, 0.0f, 1e-4f);
    EXPECT_NEAR(colors[0].w, 0.5f, 1e-4f);   // half faded in

    // the editor's edit mode: held, the plain colours
    t.ui().setTextEffectsHeld(true);
    t.update(0.5);
    colors = t.colors();
    EXPECT_NEAR(colors[0].w, 1.0f, 1e-4f);
    EXPECT_NEAR(colors[0].y, t.uiComponent().color.y, 1e-4f);
    t.ui().setTextEffectsHeld(false);
}

// ---- outline, shadow, visible characters -----------------------------------------------------

TEST(TextSdfTest, PlainTextsKeepTheirAtlasAndBuffer){
    TextScene t("Plain", false);
    t.update(0.0);
    TextComponent& text = t.text();
    ASSERT_TRUE(text.stbtext);
    EXPECT_EQ(text.stbtext->getOptions().sdfPadding, 0);
    EXPECT_FLOAT_EQ(text.stbtext->getOptions().rasterScale, 1.0f);
    EXPECT_EQ(text.sdfPadding, 0);
    EXPECT_FALSE(t.uiComponent().textSdf);
    // the two attributes texts always had, no colour
    EXPECT_TRUE(t.uiComponent().buffer.getAttribute(AttributeType::POSITION));
    EXPECT_TRUE(t.uiComponent().buffer.getAttribute(AttributeType::TEXCOORD1));
    EXPECT_FALSE(t.uiComponent().buffer.getAttribute(AttributeType::COLOR));
    // the same font id as before (the pool shares the atlas with older texts)
    EXPECT_EQ(t.ui().getFontId(text), std::string("font|20"));
    // the plain variant
    EXPECT_EQ(ShaderPool::getUIProperties(false, true, false, false), ShaderPool::getUIProperties(false, true, false, false, false));
}

TEST(TextSdfTest, OutlineOrShadowSwitchToADistanceField){
    TextScene t("Outlined", false, 32);
    t.update(0.0);
    const std::vector<Vector3> plain = t.positions();

    t.text().outlineWidth = 3.0f;
    t.update(0.0);
    t.update(0.0);
    TextComponent& text = t.text();
    ASSERT_TRUE(text.stbtext);
    EXPECT_GT(text.sdfPadding, 0);
    EXPECT_EQ(text.stbtext->getOptions().sdfPadding, text.sdfPadding);
    EXPECT_TRUE(t.uiComponent().textSdf);
    EXPECT_NE(t.ui().getFontId(text).find("~sdf"), std::string::npos);
    // the outline width reaches the shader in atlas pixels, the distance scale too
    EXPECT_NEAR(t.uiComponent().textParams[2].x, 3.0f * text.rasterScale, 1e-4f);
    EXPECT_GT(t.uiComponent().textParams[2].w, 0.0f);
    // the quads grow by the padding around each glyph
    const std::vector<Vector3> padded = t.positions();
    ASSERT_EQ(padded.size(), plain.size());
    EXPECT_LT(padded[0].x, plain[0].x);

    // an inside texel of the distance field is above the edge value (128)
    TextureData* atlas = text.stbtext->getTextureData();
    ASSERT_NE(atlas, nullptr);
    const unsigned char* pixels = static_cast<const unsigned char*>(atlas->getData());
    unsigned char brightest = 0;
    for (int i = 0; i < atlas->getWidth() * atlas->getHeight(); i++) brightest = std::max(brightest, pixels[i]);
    EXPECT_GT(brightest, 128);

    // the shadow keeps it; with neither the plain atlas comes back
    text.outlineWidth = 0.0f;
    text.shadowEnabled = true;
    text.shadowOffset = Vector2(2.0f, 2.0f);
    text.shadowSoftness = 4.0f;
    text.needUpdateText = true;
    t.update(0.0);
    EXPECT_GT(t.text().sdfPadding, 0);
    EXPECT_GT(t.uiComponent().textParams[3].x, 0.0f);   // the shadow's uv offset
    t.text().shadowEnabled = false;
    t.text().needUpdateText = true;
    t.update(0.0);
    t.update(0.0);
    EXPECT_EQ(t.text().sdfPadding, 0);
    EXPECT_FALSE(t.uiComponent().textSdf);
    EXPECT_EQ(t.ui().getFontId(t.text()), std::string("font|32"));

    // the SDF variant is its own shader key
    EXPECT_NE(ShaderPool::getUIProperties(false, true, false, false, true), ShaderPool::getUIProperties(false, true, false, false));
    EXPECT_EQ(ShaderPool::getShaderStr(ShaderType::UI, ShaderPool::getUIProperties(false, true, false, false, true)), "ui_FtxSdf");
}

TEST(TextSdfTest, VisibleCharactersCollapseTheRest){
    TextScene t("abcde", false);
    t.text().visibleCharacters = 2;
    t.update(0.0);
    const std::vector<Vector3> positions = t.positions();
    ASSERT_EQ(positions.size(), 20u);
    // the first two keep their quads, the others are a point
    EXPECT_GT(positions[1].x - positions[0].x, 0.0f);
    EXPECT_GT(positions[5].x - positions[4].x, 0.0f);
    for (size_t q = 2; q < 5; q++){
        EXPECT_FLOAT_EQ(positions[q * 4 + 1].x, positions[q * 4].x);
        EXPECT_FLOAT_EQ(positions[q * 4 + 2].y, positions[q * 4].y);
    }
}

// fields written only when set read as their defaults over an old component (play restore, undo)
TEST(TextEffectStreamTest, AbsentFieldsResetOverAnOldComponent){
    TextComponent old;
    old.outlineWidth = 3.0f;
    old.shadowEnabled = true;
    old.visibleCharacters = 4;
    const TextComponent plain = editor::Stream::decodeTextComponent(editor::Stream::encodeTextComponent(TextComponent()), &old);
    EXPECT_EQ(plain.outlineWidth, TextComponent().outlineWidth);
    EXPECT_FALSE(plain.shadowEnabled);
    EXPECT_EQ(plain.visibleCharacters, TextComponent().visibleCharacters);

    TextEffectComponent oldEffect;
    oldEffect.layers[0].type = TextEffectType::Wave;
    oldEffect.loop = !oldEffect.loop;
    oldEffect.forWords = "gold";
    const TextEffectComponent fresh = editor::Stream::decodeTextEffectComponent(
        editor::Stream::encodeTextEffectComponent(TextEffectComponent()), &oldEffect);
    EXPECT_EQ(fresh.layers[0].type, TextEffectComponent().layers[0].type);
    EXPECT_EQ(fresh.loop, TextEffectComponent().loop);
    EXPECT_TRUE(fresh.forWords.empty());

    // and what is written comes back
    TextComponent outlined;
    outlined.outlineWidth = 2.0f;
    outlined.shadowEnabled = true;
    outlined.shadowOffset = Vector2(3.0f, -1.0f);
    const TextComponent back = editor::Stream::decodeTextComponent(editor::Stream::encodeTextComponent(outlined));
    EXPECT_EQ(back.outlineWidth, 2.0f);
    EXPECT_TRUE(back.shadowEnabled);
    EXPECT_EQ(back.shadowOffset, Vector2(3.0f, -1.0f));
}
