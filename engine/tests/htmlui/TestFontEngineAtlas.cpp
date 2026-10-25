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

    // each document drawn alone with room for everything: what its frames must show
    std::vector<std::vector<uint8_t>> references(4);
    {
        AtlasBudget roomy(64, 8);
        for (int i = 0; i < 4; i++){
            RenderTestContext reference(width, height);
            ASSERT_TRUE(reference.isValid());
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
    std::vector<Rml::ElementDocument*> documents;
    for (int i = 0; i < 4; i++){
        Rml::ElementDocument* document = test.loadFromMemory(documentOf(sizes[i]), "render/atlas.rml");
        ASSERT_NE(document, nullptr);
        document->Hide();
        documents.push_back(document);
    }
    const uint64_t evictionsBefore = fonts.getEvictionCount();

    // the documents one after the other, three frames each, twice
    for (int round = 0; round < 2; round++){
        for (int i = 0; i < 4; i++){
            for (Rml::ElementDocument* document : documents)
                document->Hide();
            documents[(size_t)i]->Show();
            // the handle of the document drawn two documents ago keeps its version until its page goes
            for (int frame = 0; frame < 3; frame++){
                ASSERT_TRUE(test.renderFrame());
                EXPECT_LE(fonts.getAtlasBytes(), RmlFontEngine::getBaseBudgetBytes() + RmlFontEngine::getDistanceBudgetBytes());
            }
            std::vector<uint8_t> screen;
            ASSERT_TRUE(test.readScreen(screen));
            EXPECT_LE(largestDifference(screen, references[(size_t)i]), 1) << "document " << i << " in round " << round;
        }
    }
    EXPECT_GT(fonts.getEvictionCount(), evictionsBefore);
    EXPECT_LE(fonts.getPageCount(), 2u);

    // only the users of a cleared page get a new version: the document shown now keeps its own
    const Rml::FontFaceHandle shown = fonts.GetFontFaceHandle("sans-serif", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal, sizes[3]);
    const Rml::FontFaceHandle cleared = fonts.GetFontFaceHandle("sans-serif", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal, sizes[1]);
    const int shownVersion = fonts.GetVersion(shown);
    const int clearedVersion = fonts.GetVersion(cleared);
    // the first document again: it needs a page, the least recently used one (the second's, drawn three documents ago) goes
    for (Rml::ElementDocument* document : documents)
        document->Hide();
    documents[0]->Show();
    for (int frame = 0; frame < 3; frame++)
        ASSERT_TRUE(test.renderFrame());
    EXPECT_EQ(fonts.GetVersion(shown), shownVersion);
    EXPECT_GT(fonts.GetVersion(cleared), clearedVersion);

    // more than four evictions within 120 frames: once per context
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "HTMLUI-GEN-0007"), 1u);
}
