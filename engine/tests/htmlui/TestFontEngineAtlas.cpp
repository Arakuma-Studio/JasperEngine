// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The glyph atlas on the GPU (S16 R12, R13, AC12): base pages are R8 (1 MiB each) with a white
// block that untextured geometry batched with them samples; the atlas stays within its budget:
// with 2 MiB and four pages' worth of glyphs cycled across documents, the least recently used
// page is cleared, every drawn frame still has all its glyphs, only the handles that used the
// cleared page get a new version, and the "budget too small" diagnostic is logged once.

#include "TestRenderCommon.h"

#include "HtmlUITexture.h"
#include "RmlFontEngine.h"

#include <RmlUi/Core/ElementDocument.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const int width = 640;
    const int height = 360;

    struct AtlasBudget {
        explicit AtlasBudget(size_t baseMiB, size_t distanceMiB){
            RmlFontEngine::setAtlasBudget(baseMiB, distanceMiB);
        }
        ~AtlasBudget(){
            RmlFontEngine::setAtlasBudget(0, 0);
        }
    };

    // 62 ASCII letters and digits and the 64 letters of Latin-1 (U+00C0-U+00FF)
    std::string glyphSet(){
        std::string text = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        for (unsigned int c = 0xC0; c <= 0xFF; c++){
            text += (char)(0xC0 | (c >> 6));
            text += (char)(0x80 | (c & 0x3F));
        }
        return text;
    }

    std::string documentOf(int fontSize){
        return "<rml><head><style>body { margin: 0; font-family: sans-serif; color: #ffffff; } p { font-size: " + std::to_string(fontSize) +
               "px; line-height: 1; margin: 0; }</style></head><body><p>" + glyphSet() + "</p></body></rml>";
    }

    int largestDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        int largest = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); i++)
            largest = std::max(largest, std::abs((int)a[i] - (int)b[i]));
        return a.size() == b.size() ? largest : 255;
    }

    // bitmap sizes (under the 128 device px that draw from distance glyphs), about a page each
    const int sizes[4] = {96, 104, 112, 120};

}

TEST(TestFontEngine, AtlasBudgetPagesAreR8){
    JASPER_REQUIRE_GPU();
    RmlFontEngine& fonts = HtmlUI::getFontEngine();
    RenderTestContext test(width, height);
    ASSERT_TRUE(test.isValid());
    fonts.ReleaseFontResources();
    ASSERT_NE(test.loadFromMemory(documentOf(24), "render/atlas.rml"), nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    // one R8 page: 1 MiB, where RGBA8 was 4
    EXPECT_EQ(fonts.getPageCount(), 1u);
    EXPECT_EQ(fonts.getAtlasBytes(), fonts.getPageCount() * 1024u * 1024u);
    EXPECT_EQ(test.renderer.getStats().atlasBytes, fonts.getAtlasBytes());

    // the page's texture: R8 coverage with its white block
    const std::vector<const HtmlUITexture*> textures = test.renderer.getAtlasTextures();
    ASSERT_EQ(textures.size(), 1u);
    EXPECT_EQ(textures[0]->sample, HtmlUITexture::Sample::COVERAGE);
    EXPECT_TRUE(textures[0]->hasWhiteTexel);
    EXPECT_EQ(textures[0]->uploadedGeneration, *textures[0]->atlasGeneration);
}

TEST(TestFontEngine, AtlasBudgetEvictsLeastRecentlyUsed){
    JASPER_REQUIRE_GPU();
    RmlFontEngine& fonts = HtmlUI::getFontEngine();
    // a label drawn in every frame, under the documents: its page is never unused, so it is never
    // cleared and its handle keeps its version (only the users of a cleared page get a new one)
    const std::string label = "<rml><head><style>body { margin: 0; font-family: sans-serif; color: #80ff80; } p { position: absolute; "
                              "top: 330px; left: 0; font-size: 20px; margin: 0; }</style></head><body><p>Budget 2 MiB</p></body></rml>";

    // each document drawn alone (with the label) with room for everything: what its frames must show
    std::vector<std::vector<uint8_t>> references(4);
    {
        AtlasBudget roomy(64, 8);
        for (int i = 0; i < 4; i++){
            RenderTestContext reference(width, height);
            ASSERT_TRUE(reference.isValid());
            ASSERT_NE(reference.loadFromMemory(label, "render/atlas-label.rml"), nullptr);
            ASSERT_NE(reference.loadFromMemory(documentOf(sizes[i]), "render/atlas.rml"), nullptr);
            ASSERT_TRUE(reference.renderUntilDrawn());
            ASSERT_TRUE(reference.readScreen(references[(size_t)i]));
        }
    }

    AtlasBudget tight(2, 4);
    RenderTestContext test(width, height);
    ASSERT_TRUE(test.isValid());
    fonts.ReleaseFontResources();
    TestSystem::get().clearLogs();
    ASSERT_NE(test.loadFromMemory(label, "render/atlas-label.rml"), nullptr);
    std::vector<Rml::ElementDocument*> documents;
    for (int i = 0; i < 4; i++){
        Rml::ElementDocument* document = test.loadFromMemory(documentOf(sizes[i]), "render/atlas.rml");
        ASSERT_NE(document, nullptr);
        document->Hide();
        documents.push_back(document);
    }
    ASSERT_TRUE(test.renderFrame());
    const Rml::FontFaceHandle labelHandle = fonts.GetFontFaceHandle("sans-serif", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal, 20);
    const int labelVersion = fonts.GetVersion(labelHandle);
    auto handleOf = [&](int i){
        return fonts.GetFontFaceHandle("sans-serif", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal, sizes[i]);
    };
    const uint64_t evictionsBefore = fonts.getEvictionCount();
    int versions[4] = {0, 0, 0, 0};

    // the documents one after the other, three frames each, three times: a document's glyphs that
    // find no room wait a frame for the page of the one shown before to be unused
    for (int round = 0; round < 3; round++){
        for (int i = 0; i < 4; i++){
            for (Rml::ElementDocument* document : documents)
                document->Hide();
            documents[(size_t)i]->Show();
            for (int frame = 0; frame < 3; frame++){
                ASSERT_TRUE(test.renderFrame());
                EXPECT_LE(fonts.getAtlasBytes(), RmlFontEngine::getBaseBudgetBytes() + RmlFontEngine::getDistanceBudgetBytes());
            }
            std::vector<uint8_t> screen;
            ASSERT_TRUE(test.readScreen(screen));
            EXPECT_LE(largestDifference(screen, references[(size_t)i]), 1) << "document " << i << " in round " << round;
            if (round == 0)
                versions[i] = fonts.GetVersion(handleOf(i));
        }
    }
    EXPECT_GT(fonts.getEvictionCount(), evictionsBefore);
    EXPECT_LE(fonts.getPageCount(), 2u);
    // the documents' pages were cleared (their handles have new versions), the label's never
    int bumped = 0;
    for (int i = 0; i < 4; i++)
        bumped += fonts.GetVersion(handleOf(i)) > versions[i] ? 1 : 0;
    EXPECT_GE(bumped, 1);
    EXPECT_EQ(fonts.GetVersion(labelHandle), labelVersion);

    // more than four evictions within 120 frames: once per context
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "HTMLUI-GEN-0007"), 1u);
}
