// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Transform-aware text (S16 R14-R16, AC10, AC11, AC17): text under scale() and at a fractional
// pixel ratio is rasterized at its on-screen scale, as sharp as the same text at that font size;
// while the scale animates it draws distance glyphs (or the bitmap stretched until they exist),
// and once stable for six frames it is made again exactly once as a bitmap, the context drawing
// the settle frame without input. text-rendering: optimizeSpeed keeps the stretched bitmap,
// geometricPrecision draws distance glyphs at rest.

#include "TestGoldenCommon.h"
#include "TestRenderCommon.h"

#include "HtmlUIClock.h"
#include "RmlFontEngine.h"

#include <RmlUi/Core/ElementText.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const int width = 640;
    const int height = 240;

    const char* const label = "Sharp text 42";

    std::string documentOf(const std::string& css, const std::string& body){
        return "<rml><head><style>body { font-family: sans-serif; color: #ffffff; } "
               ".label { position: absolute; left: 20px; top: 20px; font-size: 16px; transform-origin: 0 0; } " +
               css + "</style></head><body>" + body + "</body></rml>";
    }

    struct TransformAwareText {
        bool previous;
        explicit TransformAwareText(bool on): previous(RmlFontEngine::isTransformAwareText()){
            RmlFontEngine::setTransformAwareTextForTesting(on);
        }
        ~TransformAwareText(){
            RmlFontEngine::setTransformAwareTextForTesting(previous);
        }
    };

    // the ink of a read-back screen against the background
    InkBox inkOf(const std::vector<uint8_t>& rgba, int w, int h){
        InkBox box;
        for (int y = 0; y < h; y++){
            for (int x = 0; x < w; x++){
                const uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
                const int d = std::max(std::abs(p[0] - goldenBackground), std::max(std::abs(p[1] - goldenBackground), std::abs(p[2] - goldenBackground)));
                if (d <= 40)
                    continue;
                if (box.count == 0){
                    box.x0 = box.x1 = x;
                    box.y0 = box.y1 = y;
                }else{
                    box.x0 = std::min(box.x0, x);
                    box.x1 = std::max(box.x1, x);
                    box.y0 = std::min(box.y0, y);
                    box.y1 = std::max(box.y1, y);
                }
                box.count++;
            }
        }
        return box;
    }

    // a document drawn until it settled (frames without a request for another one)
    double acutanceOf(int targetWidth, int targetHeight, int contextWidth, int contextHeight, const std::string& rml, float rasterScale = 0.0f,
                      InkBox* inkOut = nullptr){
        RenderTestContext test(targetWidth, targetHeight, contextWidth, contextHeight);
        if (!test.isValid())
            return 0.0;
        // what HtmlUIContext sets from its view: the target's pixels per document pixel
        if (rasterScale > 0.0f)
            HtmlUI::getFontEngine().setRasterScale(test.context->GetRenderManager(), rasterScale);
        if (!test.loadFromMemory(rml, "render/sharpness.rml") || !test.renderUntilDrawn())
            return 0.0;
        for (int i = 0; i < 12 && test.renderRequested; i++)
            test.renderFrame();
        std::vector<uint8_t> screen;
        if (!test.readScreen(screen))
            return 0.0;
        const InkBox ink = inkOf(screen, targetWidth, targetHeight);
        if (inkOut)
            *inkOut = ink;
        return acutance(screen, targetWidth, targetHeight, ink);
    }

    Rml::ElementText* textOf(Rml::Element* element){
        for (int i = 0; element && i < element->GetNumChildren(); i++){
            if (Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(element->GetChild(i)))
                return text;
        }
        return nullptr;
    }

}

TEST(TextSharpness, ScaledLabelMatchesItsFontSize){
    JASPER_REQUIRE_GPU();
    const std::string body = std::string("<div class=\"label\">") + label + "</div>";
    InkBox scaledInk, referenceInk;
    const double reference = acutanceOf(width, height, 0, 0, documentOf(".label { font-size: 32px; }", body), 0.0f, &referenceInk);
    const double scaled = acutanceOf(width, height, 0, 0, documentOf(".label { transform: scale(2); }", body), 0.0f, &scaledInk);
    ASSERT_GT(reference, 0.0);
    // the same text at the same size on screen
    EXPECT_NEAR(scaledInk.width(), referenceInk.width(), referenceInk.width() * 0.05);
    EXPECT_GE(scaled, 0.95 * reference) << "scale(2) " << scaled << ", font-size x2 " << reference;

    // red control: without transform-aware text a 16 px bitmap stretched twice
    TransformAwareText off(false);
    const double stretched = acutanceOf(width, height, 0, 0, documentOf(".label { transform: scale(2); }", body));
    EXPECT_LE(stretched, 0.60 * reference) << "stretched " << stretched << ", font-size x2 " << reference;
}

TEST(TextSharpness, FractionalPixelRatioIsSharp){
    JASPER_REQUIRE_GPU();
    const std::string rml = documentOf("", std::string("<div class=\"label\">") + label + "</div>");
    // the same document at 1.37 target pixels per document pixel, and at 1
    const double one = acutanceOf(400, 160, 400, 160, rml, 1.0f);
    const double ratio = acutanceOf(548, 219, 400, 160, rml, 1.37f);
    ASSERT_GT(one, 0.0);
    EXPECT_GE(ratio, 0.95 * one) << "pixel ratio 1.37 " << ratio << ", 1.0 " << one;
}

TEST(TextSharpness, AnimatingScaleDrawsDistanceGlyphs){
    JASPER_REQUIRE_GPU();
    const std::string body = std::string("<div id=\"a\" class=\"label\" style=\"transform: scale(0.5)\">") + label + "</div>";
    RenderTestContext test(width, height);
    ASSERT_TRUE(test.isValid());
    Rml::ElementDocument* document = test.loadFromMemory(documentOf("", body), "render/sharpness.rml");
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    Rml::Element* element = document->GetElementById("a");
    Rml::ElementText* text = textOf(element);
    ASSERT_NE(text, nullptr);
    float scale = 0.0f;
    EXPECT_EQ(text->GetRasterMode(scale), Rml::ElementText::RasterMode::Bitmap);
    EXPECT_NEAR(scale, 0.5f, 0.01f);

    // the scale moves every frame: the bitmap stretched until the distance glyphs exist, then them
    auto scaleAt = [](int frame){ return 0.5f + 0.5f * (float)frame / 18.0f; };
    element->SetProperty("transform", "scale(" + std::to_string(scaleAt(1)) + ")");
    test.renderFrame();
    EXPECT_TRUE(test.renderRequested);
    HtmlUI::getFontEngine().finishDistanceGlyphsForTesting();
    std::vector<uint8_t> mid;
    for (int frame = 2; frame <= 9; frame++){
        element->SetProperty("transform", "scale(" + std::to_string(scaleAt(frame)) + ")");
        ASSERT_TRUE(test.renderFrame());
        EXPECT_TRUE(test.renderRequested) << "frame " << frame;
    }
    EXPECT_EQ(text->GetRasterMode(scale), Rml::ElementText::RasterMode::DistanceField);
    ASSERT_TRUE(test.readScreen(mid));
    const double animating = acutance(mid, width, height, inkOf(mid, width, height));

    // the same scale at rest, a bitmap at its exact size
    const float midScale = scaleAt(9);
    const double rest = acutanceOf(width, height, 0, 0,
                                   documentOf("", std::string("<div class=\"label\" style=\"transform: scale(") + std::to_string(midScale) + ")\">" +
                                                  label + "</div>"));
    ASSERT_GT(rest, 0.0);
    EXPECT_GE(animating, 0.85 * rest) << "animating " << animating << ", at rest " << rest;
}

TEST(TextSharpness, SettleRegeneratesOnce){
    JASPER_REQUIRE_GPU();
    // 40 labels popping in from scale(0.5) to scale(1) over 300 ms (18 frames at 60 Hz)
    std::string body;
    for (int i = 0; i < 40; i++){
        body += "<div class=\"label\" style=\"left: " + std::to_string(10 + (i % 8) * 78) + "px; top: " + std::to_string(10 + (i / 8) * 44) +
                "px; transform: scale(0.5)\">Label " + std::to_string(i) + "</div>";
    }
    RenderTestContext test(width, height);
    ASSERT_TRUE(test.isValid());
    Rml::ElementDocument* document = test.loadFromMemory(documentOf("", body), "render/settle.rml");
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    Rml::ElementList labels;
    document->GetElementsByClassName(labels, "label");
    ASSERT_EQ(labels.size(), 40u);

    for (int frame = 1; frame <= 18; frame++){
        const float scale = 0.5f + 0.5f * (float)frame / 18.0f;
        for (Rml::Element* element : labels)
            element->SetProperty("transform", "scale(" + std::to_string(scale) + ")");
        ASSERT_TRUE(test.renderFrame());
        EXPECT_LE(test.renderer.getStats().glyphsRasterized, (uint32_t)RmlFontEngine::settleGlyphBudget) << "frame " << frame;
        if (frame == 2)
            HtmlUI::getFontEngine().finishDistanceGlyphsForTesting();
    }

    // stable from now on: no input, the context asks for its frames until the text settled,
    // and regenerates once per label
    uint32_t regenerations = 0;
    int frames = 0;
    while (test.renderRequested && frames < 20){
        ASSERT_TRUE(test.renderFrame());
        regenerations += test.renderer.getStats().textRegenerations;
        EXPECT_LE(test.renderer.getStats().glyphsRasterized, (uint32_t)RmlFontEngine::settleGlyphBudget);
        frames++;
    }
    EXPECT_FALSE(test.renderRequested) << "still settling after " << frames << " frames";
    EXPECT_GE(frames, 6);
    EXPECT_EQ(regenerations, 40u);
    for (Rml::Element* element : labels){
        float scale = 0.0f;
        Rml::ElementText* text = textOf(element);
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->GetRasterMode(scale), Rml::ElementText::RasterMode::Bitmap);
        EXPECT_NEAR(scale, 1.0f, 0.01f);
    }
    // settled: nothing more is asked for and nothing is made again
    ASSERT_TRUE(test.renderFrame());
    EXPECT_FALSE(test.renderRequested);
    EXPECT_EQ(test.renderer.getStats().textRegenerations, 0u);
}

TEST(TextSharpness, TextRenderingProperty){
    JASPER_REQUIRE_GPU();
    const std::string text = std::string(label) + "</div>";
    const double reference = acutanceOf(width, height, 0, 0, documentOf(".label { font-size: 32px; }", "<div class=\"label\">" + text));
    ASSERT_GT(reference, 0.0);

    // optimizeSpeed: the bitmap at the context's scale, stretched under the transform (the old look)
    const double speed = acutanceOf(width, height, 0, 0, documentOf(".label { transform: scale(2); text-rendering: optimizeSpeed; }",
                                                                    "<div class=\"label\">" + text));
    EXPECT_LE(speed, 0.60 * reference) << "optimizeSpeed " << speed << ", font-size x2 " << reference;
    // optimizeLegibility and auto: the hybrid, sharp at rest
    const double legibility = acutanceOf(width, height, 0, 0, documentOf(".label { transform: scale(2); text-rendering: optimizeLegibility; }",
                                                                         "<div class=\"label\">" + text));
    EXPECT_GE(legibility, 0.95 * reference);

    // geometricPrecision: distance glyphs at rest
    RenderTestContext test(width, height);
    ASSERT_TRUE(test.isValid());
    Rml::ElementDocument* document = test.loadFromMemory(documentOf(".label { text-rendering: geometricPrecision; }",
                                                                     "<div id=\"a\" class=\"label\">" + text), "render/sharpness.rml");
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    HtmlUI::getFontEngine().finishDistanceGlyphsForTesting();
    for (int i = 0; i < 4; i++)
        ASSERT_TRUE(test.renderFrame());
    float scale = 0.0f;
    Rml::ElementText* element = textOf(document->GetElementById("a"));
    ASSERT_NE(element, nullptr);
    EXPECT_EQ(element->GetRasterMode(scale), Rml::ElementText::RasterMode::DistanceField);
    EXPECT_FALSE(test.renderRequested);
    std::vector<uint8_t> screen;
    ASSERT_TRUE(test.readScreen(screen));
    EXPECT_GT(inkOf(screen, width, height).count, 100u);
}
