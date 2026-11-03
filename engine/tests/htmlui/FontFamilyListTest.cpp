// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The font-family contract between the RmlUi fork and the engine's font engine (phase-3 spec 3.3.4): the fork's
// font_family parser stores a CSS family list as one canonical string (quotes removed, white space collapsed,
// lowercase, joined by commas without spaces), and RmlFontEngine resolves that list from left to right, generic
// families (serif, sans-serif, monospace, system-ui, ui-*, cursive, fantasy, emoji, math) on the built-in faces.

#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "RmlFile.h"
#include "RmlFontEngine.h"
#include "RmlSystem.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class FontFamilyListTest : public ::testing::Test {
    protected:
        RecordingRenderInterface recording;
        RmlFontEngine engine;
        Rml::Context* context = nullptr;

        void SetUp() override{
            ASSERT_FALSE(HtmlUI::isInitialized());
            FontRegistry::clear();
            Rml::SetSystemInterface(&HtmlUI::getSystemInterface());
            Rml::SetFileInterface(&HtmlUI::getFileInterface());
            Rml::SetFontEngineInterface(&engine);
            ASSERT_TRUE(Rml::Initialise());
            context = Rml::CreateContext("font-family-list", Rml::Vector2i(320, 200), &recording);
            ASSERT_NE(context, nullptr);
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            if (context)
                Rml::RemoveContext("font-family-list");
            Rml::Shutdown();
            FontRegistry::clear();
        }

        // what the fork computes for a declared font-family
        std::string computed(const std::string& declared){
            Rml::ElementDocument* document = context->LoadDocumentFromMemory("<rml><body><p id='p'>x</p></body></rml>");
            if (!document)
                return "<no document>";
            Rml::Element* p = document->GetElementById("p");
            std::string result = "<invalid>";
            if (p->SetProperty("font-family", declared)){
                context->Update();
                result = p->GetComputedValues().font_family();
            }
            document->Close();
            context->Update();
            return result;
        }
    };

}

TEST_F(FontFamilyListTest, TheForkWritesTheCanonicalList){
    EXPECT_EQ(computed("\"Inter\",  'Segoe  UI' , Sans-Serif"), "inter,segoe ui,sans-serif");
    EXPECT_EQ(computed("Open   Sans, serif"), "open sans,serif");
    EXPECT_EQ(computed("\"Noto Sans Arabic\""), "noto sans arabic");
    EXPECT_EQ(computed("a,,b"), "<invalid>");
}


TEST_F(FontFamilyListTest, FamiliesResolveLeftToRight){
    ASSERT_TRUE(engine.LoadFontFace("fonts/Roboto-Bold.ttf", 0, "Heavy", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Bold, false));

    // an unknown family falls through to the next one; the generic families are the built-in faces
    Rml::FontFaceHandle face = engine.GetFontFaceHandle(computed("'Missing Family Y', Heavy, sans-serif"), Rml::Style::FontStyle::Normal,
                                                        Rml::Style::FontWeight::Bold, 100);
    ASSERT_NE(face, 0u);
    const Rml::String language;
    const Rml::TextShapingContext shaping{language};
    EXPECT_EQ(engine.GetStringWidth(face, "M", shaping), 88);

    for (const char* generic : {"serif", "sans-serif", "monospace", "system-ui", "ui-rounded", "cursive", "fantasy", "emoji", "math"}){
        EXPECT_TRUE(FontRegistry::isGenericFamily(generic)) << generic;
        Rml::FontFaceHandle handle = engine.GetFontFaceHandle(computed(std::string("'Missing Family Y', ") + generic), Rml::Style::FontStyle::Normal,
                                                              Rml::Style::FontWeight::Normal, 100);
        ASSERT_NE(handle, 0u) << generic;
        EXPECT_EQ(engine.GetStringWidth(handle, "M", shaping), 87) << generic;
    }
    // a generic family is never reported as not loaded
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI] font-family 'emoji' is not loaded"), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI] font-family 'missing family y' is not loaded"), 1u);
}
