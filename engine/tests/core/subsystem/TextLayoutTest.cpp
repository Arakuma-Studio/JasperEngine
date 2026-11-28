// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S94 F24-F26 (AC24-AC26): a Text's Anchor places its box against its origin by the font's
// metrics (Top, Middle, Bottom on the box; Baseline, Capline, Midline on the first line) and
// leaves its lines where they are inside the box; its size is known right after setText (Text:width
// lays it out on demand, once); and the em font size scales by the em square as CSS does. No GPU:
// UISystem lays the glyphs out on the CPU (the built-in Roboto).

#include "Scene.h"
#include "component/RectTransformComponent.h"
#include "component/TextComponent.h"
#include "component/UIComponent.h"
#include "object/Camera.h"
#include "object/ui/Text.h"
#include "subsystem/UISystem.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"
#include "util/STBText.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <memory>
#include <string>
#include <vector>

namespace subsystem_TextLayoutTest_cpp {

using namespace jasperengine;

namespace {

    struct TextLayoutScene {
        Scene scene;
        Camera camera{&scene};
        std::unique_ptr<Text> text;

        TextLayoutScene(CameraType type, const std::string& content, unsigned int fontSize = 40){
            camera.setType(type);
            camera.activate();
            text = std::make_unique<Text>(&scene);
            text->setFontSize(fontSize);
            text->setText(content);
        }

        TextComponent& component(){ return scene.getComponent<TextComponent>(text->getEntity()); }
        RectTransformComponent& layout(){ return scene.getComponent<RectTransformComponent>(text->getEntity()); }

        // the glyph quads' corners after the alignment
        std::vector<Vector3> vertices(){
            Buffer& buffer = scene.getComponent<UIComponent>(text->getEntity()).buffer;
            Attribute* positions = buffer.getAttribute(AttributeType::POSITION);
            std::vector<Vector3> out;
            for (unsigned int i = 0; positions && i < positions->getCount(); i++)
                out.push_back(buffer.getVector3(positions, i));
            return out;
        }

        float minY(){
            float y = FLT_MAX;
            for (const Vector3& v : vertices()) y = std::min(y, v.y);
            return y;
        }
    };

}

// AC24: the box of each vertical mode against the font's metrics (world camera: y up); the glyphs
// move with it
TEST(TextLayoutTest, AnchorVerticals){
    TextLayoutScene t(CameraType::CAMERA_PERSPECTIVE, "Hg");
    ASSERT_TRUE(t.text->updateLayout());
    TextComponent& text = t.component();
    ASSERT_TRUE(text.stbtext);
    const float height = (float)t.layout().height;
    const float ascent = text.stbtext->getAscent();
    const float cap = text.stbtext->getCapHeight();
    const float x = text.stbtext->getXHeight();
    ASSERT_GT(height, 0.0f);

    // the metrics are the primary face's (OS/2 cap and x heights) at the font size
    std::shared_ptr<FontFace> face = FontRegistry::getBuiltInLatin();
    const float scale = face->scaleForPixelHeight(40.0f);
    EXPECT_NEAR(cap, face->getCapHeight() * scale, 1e-3f);
    EXPECT_NEAR(x, face->getXHeight() * scale, 1e-3f);
    EXPECT_GT(cap, x);

    // Bottom is the anchor of before: the box above the origin
    EXPECT_FLOAT_EQ(text.boxMin.x, 0.0f);
    EXPECT_FLOAT_EQ(text.boxMin.y, 0.0f);
    const float bottomGlyphs = t.minY();

    struct Mode { TextAnchorVertical alignment; float boxMinY; };
    const Mode modes[] = {
        {TextAnchorVertical::TOP, -height},
        {TextAnchorVertical::MIDDLE, -height / 2.0f},
        {TextAnchorVertical::BOTTOM, 0.0f},
        {TextAnchorVertical::BASELINE, ascent - height},
        {TextAnchorVertical::CAPLINE, ascent - height - cap / 2.0f},
        {TextAnchorVertical::MIDLINE, ascent - height - x / 2.0f},
    };
    for (const Mode& mode : modes){
        SCOPED_TRACE((int)mode.alignment);
        t.text->setAnchorVertical(mode.alignment);
        ASSERT_TRUE(t.text->updateLayout());
        EXPECT_NEAR(text.boxMin.y, mode.boxMinY, 0.5f);
        EXPECT_NEAR(t.minY() - bottomGlyphs, mode.boxMinY, 0.5f);
    }

    // Capline centres the capitals on the origin (an H's ink, within its rasterized pixel)
    TextLayoutScene h(CameraType::CAMERA_PERSPECTIVE, "H");
    h.text->setAnchorVertical(TextAnchorVertical::CAPLINE);
    ASSERT_TRUE(h.text->updateLayout());
    float low = FLT_MAX, high = -FLT_MAX;
    for (const Vector3& v : h.vertices()){
        low = std::min(low, v.y);
        high = std::max(high, v.y);
    }
    EXPECT_NEAR((low + high) / 2.0f, 0.0f, 1.0f);
}

// a UI camera (y down) keeps the anchor of before: the box below the origin, Baseline on the line
TEST(TextLayoutTest, UiCameraKeepsTheOldAnchor){
    TextLayoutScene t(CameraType::CAMERA_UI, "Hg");
    ASSERT_TRUE(t.text->updateLayout());
    TextComponent& text = t.component();
    EXPECT_FLOAT_EQ(text.boxMin.y, 0.0f);
    t.text->setAnchorVertical(TextAnchorVertical::BASELINE);
    ASSERT_TRUE(t.text->updateLayout());
    EXPECT_NEAR(text.boxMin.y, -text.stbtext->getAscent(), 0.5f);
    EXPECT_TRUE(t.text->isPivotBaseline());
}

// Center and Right: the box against the origin (Unity TextMesh's Anchor); the lines keep their
// place inside the box (how they sit there is the Text's alignment, not the anchor)
TEST(TextLayoutTest, AnchorHorizontals){
    TextLayoutScene t(CameraType::CAMERA_PERSPECTIVE, "WWWW\nW");
    ASSERT_TRUE(t.text->updateLayout());
    TextComponent& text = t.component();
    const float width = (float)t.layout().width;
    ASSERT_EQ(text.charExtents.size(), 5u);
    EXPECT_NEAR(text.charExtents[4].left, 0.0f, 0.5f);

    t.text->setAnchorHorizontal(TextAnchorHorizontal::CENTER);
    ASSERT_TRUE(t.text->updateLayout());
    EXPECT_FLOAT_EQ(text.boxMin.x, -width / 2.0f);
    EXPECT_NEAR(text.charExtents[4].left, 0.0f, 0.5f) << "the short line stays at the box's left";
    EXPECT_NEAR(text.charExtents[0].left, 0.0f, 0.5f);
    EXPECT_TRUE(t.text->isPivotCentered());

    t.text->setAnchorHorizontal(TextAnchorHorizontal::RIGHT);
    ASSERT_TRUE(t.text->updateLayout());
    EXPECT_FLOAT_EQ(text.boxMin.x, -width);
    EXPECT_NEAR(text.charExtents[4].left, 0.0f, 0.5f);

    // a value of the other enum is refused
    t.text->setAnchorHorizontal(static_cast<TextAnchorHorizontal>(TextAnchorVertical::MIDDLE));
    EXPECT_EQ(t.text->getAnchorHorizontal(), TextAnchorHorizontal::RIGHT);
}

// AC25 (R42): the size right after setText, before any update drew the text; a clean text is not
// laid out again (F-PF7)
TEST(TextLayoutTest, SizeKnownBeforeFirstDraw){
    TextLayoutScene t(CameraType::CAMERA_PERSPECTIVE, "Door 12");
    const unsigned int width = t.text->getWidth();
    EXPECT_GT(width, 0u);
    EXPECT_GT(t.text->getHeight(), 0u);
    const unsigned long builds = t.component().buildCount;
    EXPECT_EQ(t.text->getWidth(), width);
    EXPECT_EQ(t.component().buildCount, builds);

    t.text->setText("Door 12 and more");
    EXPECT_GT(t.text->getWidth(), width);
    EXPECT_EQ(t.component().buildCount, builds + 1);
}

// AC26 (R43): Em scales the em square to the font size (CSS, TextMesh Pro): the capitals are
// capHeight x size / unitsPerEm; Pixel Height scales ascender - descender to it
TEST(TextLayoutTest, EmSizeMatchesCss){
    std::shared_ptr<FontFace> face = FontRegistry::getBuiltInLatin();
    const float upem = (float)face->getUnitsPerEm();
    const float span = (float)(face->getAscender() - face->getDescender());

    TextLayoutScene em(CameraType::CAMERA_PERSPECTIVE, "H", 100);
    em.text->setFontSizeUnit(TextFontSizeUnit::EM);
    ASSERT_TRUE(em.text->updateLayout());
    TextComponent& emText = em.component();
    EXPECT_NEAR(emText.stbtext->getCapHeight(), face->getCapHeight() * 100.0f / upem, 0.5f);
    float low = FLT_MAX, high = -FLT_MAX;
    for (const Vector3& v : em.vertices()){
        low = std::min(low, v.y);
        high = std::max(high, v.y);
    }
    // the H's ink, within the pixel its rasterization rounds to on each side
    EXPECT_NEAR(high - low, face->getCapHeight() * 100.0f / upem, 2.0f);

    TextLayoutScene pixel(CameraType::CAMERA_PERSPECTIVE, "H", 100);
    ASSERT_TRUE(pixel.text->updateLayout());
    const float ratio = pixel.component().stbtext->getCapHeight() / emText.stbtext->getCapHeight();
    EXPECT_NEAR(ratio, upem / span, upem / span * 0.01f);
    // two scales, two atlases
    EXPECT_NE(emText.stbtext, pixel.component().stbtext);
}

}  // namespace subsystem_TextLayoutTest_cpp
