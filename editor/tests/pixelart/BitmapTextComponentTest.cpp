// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S51 through the Text component and UISystem (no GPU): the pixel scale (AC7), NEAREST and no
// signed-distance atlas (AC9), the styled layers and a colour change that keeps the cache (AC10),
// a Text Effect moving both layers of a glyph (AC12), and the outlineNeighbors key (R32).

#include <gtest/gtest.h>

#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/RectTransformComponent.h"
#include "component/TextComponent.h"
#include "component/TextEffectComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "subsystem/UISystem.h"
#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapText.h"
#include "pixelart/BitmapTextCache.h"
#include "pool/FontPool.h"
#include "reflect/Reflect.h"
#include "util/TextEffects.h"
#include "Catalog.h"

#include <cmath>
#include <vector>

using namespace jasperengine;

namespace {

    struct BitmapTextScene {
        Scene scene;
        Entity entity;

        explicit BitmapTextScene(const std::string& text, unsigned int fontSize = 9){
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
            component.font[0] = ::jasperengine::pixelart::kBuiltinPixelFont;
            component.text = text;
            component.fontSize = fontSize;
            scene.addComponent<TextComponent>(entity, component);
        }

        UISystem& ui() { return *scene.getSystem<UISystem>(); }
        TextComponent& text() { return scene.getComponent<TextComponent>(entity); }
        UIComponent& uiComponent() { return scene.getComponent<UIComponent>(entity); }
        void update(double dt = 0.0){ ui().update(dt); }

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

TEST(BitmapText, ScaleThroughTheTextComponent){
    // R7: 18 on a 9 px font is x2, 16 is x2, 4 is x1
    for (const auto& [size, k] : std::vector<std::pair<unsigned int, int>>{{18, 2}, {16, 2}, {4, 1}, {27, 3}}){
        BitmapTextScene t("Hp 99", size);
        t.update();
        TextComponent& text = t.text();
        ASSERT_TRUE(text.bitmapText) << size;
        EXPECT_FALSE(text.stbtext);
        EXPECT_EQ(text.pixelScale, k) << size;
        EXPECT_FLOAT_EQ(text.pixelUnit, (float)k);
        // the layout height is k times the line height, every corner on the k grid
        EXPECT_EQ(t.scene.getComponent<RectTransformComponent>(t.entity).height, (unsigned int)(10 * k));
        for (const Vector3& p : t.positions()){
            EXPECT_NEAR(std::fmod(std::fabs(p.x), (float)k), 0.0f, 1e-4f) << size;
            EXPECT_NEAR(std::fmod(std::fabs(p.y), (float)k), 0.0f, 1e-4f) << size;
        }
    }
}

TEST(BitmapText, NoSdf){
    BitmapTextScene t("Outlined", 18);
    t.text().outlineWidth = 1.0f;
    t.text().shadowEnabled = true;
    t.text().shadowOffset = Vector2(1.0f, 1.0f);
    t.update();
    t.update();
    TextComponent& text = t.text();
    UIComponent& ui = t.uiComponent();
    ASSERT_TRUE(text.bitmapText);
    // no distance field: no STBText, no pool entry, no SDF shader path
    EXPECT_FALSE(text.stbtext);
    EXPECT_EQ(text.sdfPadding, 0);
    EXPECT_FALSE(ui.textSdf);
    EXPECT_FALSE(FontPool::contains(t.ui().getFontId(text)));
    EXPECT_EQ(t.ui().getFontId(text).find("~sdf"), std::string::npos);
    // NEAREST, from the cache's RGBA page, drawn as a texture (R9)
    EXPECT_EQ(ui.texture.getMinFilter(), TextureFilter::NEAREST);
    EXPECT_EQ(ui.texture.getMagFilter(), TextureFilter::NEAREST);
    EXPECT_TRUE(ui.textRgba);
    EXPECT_TRUE(ui.pixelOriginSnap);
    EXPECT_TRUE(text.bitmapText->usesCache());

    // without a style: the font's own R8 atlas, coverage sampled
    text.outlineWidth = 0.0f;
    text.shadowEnabled = false;
    t.update();
    t.update();
    EXPECT_FALSE(t.text().bitmapText->usesCache());
    EXPECT_FALSE(t.uiComponent().textRgba);
    EXPECT_EQ(t.uiComponent().texture.getMinFilter(), TextureFilter::NEAREST);

    // back to a vector font: filtered coverage again
    t.text().font[0] = "";
    t.text().needReloadAtlas = true;
    t.update();
    t.update();
    EXPECT_TRUE(t.text().stbtext);
    EXPECT_FALSE(t.text().bitmapText);
    EXPECT_EQ(t.uiComponent().texture.getMinFilter(), TextureFilter::LINEAR);
    EXPECT_FALSE(t.uiComponent().pixelOriginSnap);
}

TEST(BitmapTextCache, ColourChangeNoRebuild){
    BitmapTextScene t("Score 120", 27);
    t.text().outlineWidth = 1.0f;
    t.text().outlineColor = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
    t.update();
    t.update();
    ASSERT_TRUE(t.text().bitmapText && t.text().bitmapText->usesCache());
    const int backQuads = t.text().bitmapText->getBackQuads();
    ASSERT_EQ(backQuads, 9);
    const unsigned long builds = t.text().buildCount;
    const ::jasperengine::pixelart::BitmapTextCache::Stats before = ::jasperengine::pixelart::BitmapTextCache::get().getStats();
    const std::vector<Vector3> positions = t.positions();

    t.uiComponent().color = Vector4(0.2f, 0.4f, 0.6f, 0.5f);
    t.update();
    const ::jasperengine::pixelart::BitmapTextCache::Stats after = ::jasperengine::pixelart::BitmapTextCache::get().getStats();
    EXPECT_EQ(after.misses, before.misses);
    EXPECT_EQ(after.hits, before.hits);
    EXPECT_EQ(t.text().buildCount, builds);
    EXPECT_EQ(t.positions().size(), positions.size());
    const std::vector<Vector4> colors = t.colors();
    ASSERT_EQ(colors.size(), positions.size());
    for (size_t v = 0; v < colors.size(); v++){
        if (v < (size_t)backQuads * 4) {
            EXPECT_EQ(colors[v], Vector4(1.0f, 1.0f, 1.0f, 1.0f)) << v;
        } else {
            EXPECT_EQ(colors[v], Vector4(0.2f, 0.4f, 0.6f, 1.0f)) << v;
        }
    }
    // the draw takes only the component's alpha (D5)
    EXPECT_TRUE(t.uiComponent().vertexColorAbsolute);

    // the outline colour is the style: a new cache entry
    t.text().outlineColor = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
    t.update();
    t.update();
    EXPECT_GT(::jasperengine::pixelart::BitmapTextCache::get().getStats().misses, after.misses);
}

TEST(BitmapText, TextEffectMovesBothLayers){
    BitmapTextScene t("Wave", 18);
    t.text().outlineWidth = 1.0f;
    t.update();
    const std::vector<Vector3> plain = t.positions();
    t.scene.addComponent<TextEffectComponent>(t.entity, TextEffectComponent());
    TextEffectComponent& effect = t.scene.getComponent<TextEffectComponent>(t.entity);
    TextEffects::setType(effect.layers[0], TextEffectType::Wave);
    effect.layers[0].amplitude = 0.2f;
    effect.layers[0].duration = 2.0f;
    effect.useUnscaledTime = false;
    t.update();
    t.update();
    t.update(0.37);
    const std::vector<Vector3> moved = t.positions();
    ASSERT_EQ(moved.size(), plain.size());
    const size_t glyphs = plain.size() / 8;   // back quads, then face quads
    bool anyMoved = false;
    for (size_t g = 0; g < glyphs; g++){
        const float back = moved[g * 4].y - plain[g * 4].y;
        const float face = moved[(g + glyphs) * 4].y - plain[(g + glyphs) * 4].y;
        EXPECT_NEAR(back, face, 1e-3f) << g;
        anyMoved = anyMoved || std::fabs(face) > 1e-3f;
    }
    EXPECT_TRUE(anyMoved);
    // the back quads keep their texture's colours (white), the faces the text's
    const std::vector<Vector4> colors = t.colors();
    EXPECT_EQ(colors[0].x, 1.0f);
}

TEST(BitmapText, VisibleCharactersHideBothLayers){
    BitmapTextScene t("abcd", 9);
    t.text().outlineWidth = 1.0f;
    t.text().visibleCharacters = 2;
    t.update();
    const std::vector<Vector3> p = t.positions();
    const size_t glyphs = p.size() / 8;
    ASSERT_EQ(glyphs, 4u);
    for (size_t g : {(size_t)2, (size_t)3, (size_t)6, (size_t)7}){
        // collapsed to a point
        EXPECT_EQ(p[g * 4].x, p[g * 4 + 2].x) << g;
        EXPECT_EQ(p[g * 4].y, p[g * 4 + 2].y) << g;
    }
    EXPECT_NE(p[0].x, p[2].x);
}

TEST(BitmapText, OutlineNeighborsStreamOmittedWhenDefault){
    const editor::reflect::ComponentDesc& desc = *editor::reflect::find(editor::ComponentType::TextComponent);
    TextComponent plain;
    const YAML::Node encoded = editor::reflect::encode(desc, &plain, editor::reflect::EncodeContext{});
    EXPECT_FALSE(encoded["outlineNeighbors"]);
    TextComponent four;
    four.outlineNeighbors = 4;
    const YAML::Node node = editor::reflect::encode(desc, &four, editor::reflect::EncodeContext{});
    ASSERT_TRUE(node["outlineNeighbors"]);
    EXPECT_EQ(node["outlineNeighbors"].as<int>(), 4);
    TextComponent back;
    editor::reflect::DecodeReport report;
    editor::reflect::decode(desc, YAML::Load(YAML::Dump(node)), &back, editor::reflect::DecodeContext{}, report);
    EXPECT_EQ(back.outlineNeighbors, 4);
}
