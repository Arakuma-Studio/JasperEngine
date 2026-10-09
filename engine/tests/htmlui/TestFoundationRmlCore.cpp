// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RmlUi as vendored builds, initialises with the engine's system and file interfaces,
// lays out and draws a document, and shuts down without leaking render resources.

#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "RmlFile.h"
#include "RmlSystem.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // A font engine with fixed metrics and one untextured quad per non-space character, so
    // RmlUi lays out and draws text without the engine's font stack
    class FakeFontEngine : public Rml::FontEngineInterface {
    public:
        Rml::FontFaceHandle GetFontFaceHandle(const Rml::String&, Rml::Style::FontStyle, Rml::Style::FontWeight, int size) override{
            if (size <= 0)
                return 0;
            std::unique_ptr<Rml::FontMetrics>& metrics = faces[size];
            if (!metrics){
                metrics.reset(new Rml::FontMetrics());
                metrics->size = size;
                metrics->ascent = size * 0.8f;
                metrics->descent = size * 0.2f;
                metrics->line_spacing = size * 1.25f;
                metrics->x_height = size * 0.5f;
                metrics->underline_position = size * 0.1f;
                metrics->underline_thickness = 1.0f;
                metrics->has_ellipsis = false;
            }
            return reinterpret_cast<Rml::FontFaceHandle>(metrics.get());
        }

        const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override{
            return *reinterpret_cast<const Rml::FontMetrics*>(handle);
        }

        int GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext&, Rml::Character) override{
            return int(string.size()) * advance(handle);
        }

        int GenerateString(Rml::RenderManager&, Rml::FontFaceHandle handle, Rml::FontEffectsHandle, Rml::StringView string,
                           Rml::Vector2f position, Rml::ColourbPremultiplied colour, float, const Rml::TextShapingContext&,
                           Rml::TexturedMeshList& meshList) override{
            const Rml::FontMetrics& metrics = GetFontMetrics(handle);
            meshList.resize(1);
            Rml::Mesh& mesh = meshList[0].mesh;
            int x = 0;
            for (const char* c = string.begin(); c != string.end(); ++c){
                if (*c != ' '){
                    Rml::MeshUtilities::GenerateQuad(mesh, position + Rml::Vector2f(float(x), -metrics.ascent),
                                                     Rml::Vector2f(float(advance(handle) - 1), metrics.ascent), colour);
                }
                x += advance(handle);
            }
            generatedStrings++;
            return x;
        }

        int GetVersion(Rml::FontFaceHandle) override{
            return 1;
        }

        int generatedStrings = 0;

    private:
        static int advance(Rml::FontFaceHandle handle){
            return std::max(1, reinterpret_cast<const Rml::FontMetrics*>(handle)->size / 2);
        }

        std::map<int, std::unique_ptr<Rml::FontMetrics>> faces;
    };

    const char* smokeDocument = R"(
        <rml>
        <head>
            <style>
                body { display: block; width: 400px; height: 200px; background-color: #202020ff; font-family: test; font-size: 20px; }
                div { display: block; }
                #box { width: 120px; height: 40px; margin: 10px; background-color: #ff0000ff; }
                #label { color: #ffffffff; }
            </style>
        </head>
        <body>
            <div id="box"></div>
            <div id="label">Hello RmlUi</div>
        </body>
        </rml>
    )";

}

TEST(RmlCoreSmokeTest, InitialiseLoadRenderShutdown){
    ASSERT_FALSE(HtmlUI::isInitialized());

    // declared first: render managers release through it during Rml::Shutdown
    RecordingRenderInterface recording;
    FakeFontEngine fontEngine;
    RmlSystem& system = HtmlUI::getSystemInterface();
    system.resetCounters();

    Rml::SetSystemInterface(&system);
    Rml::SetFileInterface(&HtmlUI::getFileInterface());
    Rml::SetFontEngineInterface(&fontEngine);
    ASSERT_TRUE(Rml::Initialise());

    Rml::Context* context = Rml::CreateContext("smoke", Rml::Vector2i(640, 360), &recording);
    ASSERT_NE(context, nullptr);

    Rml::ElementDocument* document = context->LoadDocumentFromMemory(smokeDocument, "[smoke]");
    ASSERT_NE(document, nullptr);
    document->Show();

    context->Update();
    recording.beginFrame();
    context->Render();

    // layout
    Rml::Element* box = document->GetElementById("box");
    ASSERT_NE(box, nullptr);
    EXPECT_FLOAT_EQ(box->GetClientWidth(), 120.0f);
    EXPECT_FLOAT_EQ(box->GetClientHeight(), 40.0f);
    EXPECT_FLOAT_EQ(box->GetAbsoluteLeft(), 10.0f);
    EXPECT_FLOAT_EQ(box->GetAbsoluteTop(), 10.0f);

    Rml::Element* label = document->GetElementById("label");
    ASSERT_NE(label, nullptr);
    // 11 characters of 10 px (fake font at 20 px)
    EXPECT_EQ(label->GetInnerRML(), "Hello RmlUi");
    EXPECT_GT(fontEngine.generatedStrings, 0);

    // geometry: body background, the box background and the text quads were drawn
    EXPECT_GT(recording.getCompiledGeometryTotal(), 0u);
    ASSERT_GE(recording.getDraws().size(), 3u);
    size_t textQuads = 0;
    for (const RecordingRenderInterface::Draw& draw : recording.getDraws()){
        EXPECT_NE(recording.getGeometry(draw.geometry), nullptr);
        textQuads += (draw.vertexCount == 40) ? 1 : 0;   // 10 inked characters x 4 vertices
    }
    EXPECT_EQ(textQuads, 1u);

    EXPECT_EQ(system.getErrorCount(), 0u);

    // Shutdown releases everything the context compiled
    Rml::RemoveContext("smoke");
    Rml::Shutdown();
    EXPECT_EQ(recording.getLiveGeometryCount(), 0u);
    EXPECT_EQ(recording.getLiveTextureCount(), 0u);
    EXPECT_EQ(system.getErrorCount(), 0u);
}

TEST(RmlCoreSmokeTest, LoadDocumentFromFileJoinsPaths){
    ASSERT_FALSE(HtmlUI::isInitialized());

    RecordingRenderInterface recording;
    FakeFontEngine fontEngine;
    RmlSystem& system = HtmlUI::getSystemInterface();
    system.resetCounters();

    Rml::SetSystemInterface(&system);
    Rml::SetFileInterface(&HtmlUI::getFileInterface());
    Rml::SetFontEngineInterface(&fontEngine);
    ASSERT_TRUE(Rml::Initialise());

    Rml::Context* context = Rml::CreateContext("file", Rml::Vector2i(640, 360), &recording);
    ASSERT_NE(context, nullptr);

    // RmlFile reads it relative to the asset root (the fixtures); the linked base.css and
    // the image resolve relative to the document through RmlSystem::JoinPath
    Rml::ElementDocument* document = context->LoadDocument("common/foundation.rml");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    recording.beginFrame();
    context->Render();

    Rml::Element* box = document->GetElementById("box");
    ASSERT_NE(box, nullptr);
    // display: block from base.css, so the box takes its own line at its margin
    EXPECT_EQ(box->GetComputedValues().display(), Rml::Style::Display::Block);
    EXPECT_FLOAT_EQ(box->GetAbsoluteTop(), 10.0f);
    EXPECT_FLOAT_EQ(box->GetClientWidth(), 120.0f);

    const std::vector<std::string>& requests = recording.getLoadRequests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0], "common/checker.png");
    Rml::Element* checker = document->GetElementById("checker");
    ASSERT_NE(checker, nullptr);
    EXPECT_FLOAT_EQ(checker->GetClientWidth(), 64.0f);

    EXPECT_EQ(system.getErrorCount(), 0u);
    EXPECT_EQ(system.getWarningCount(), 0u);

    Rml::RemoveContext("file");
    Rml::Shutdown();
    EXPECT_EQ(recording.getLiveGeometryCount(), 0u);
    EXPECT_EQ(recording.getLiveTextureCount(), 0u);
}

TEST(RmlCoreSmokeTest, MissingDocumentIsReported){
    ASSERT_FALSE(HtmlUI::isInitialized());

    RecordingRenderInterface recording;
    FakeFontEngine fontEngine;
    RmlSystem& system = HtmlUI::getSystemInterface();
    system.resetCounters();

    Rml::SetSystemInterface(&system);
    Rml::SetFileInterface(&HtmlUI::getFileInterface());
    Rml::SetFontEngineInterface(&fontEngine);
    ASSERT_TRUE(Rml::Initialise());

    Rml::Context* context = Rml::CreateContext("missing", Rml::Vector2i(640, 360), &recording);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(context->LoadDocument("common/does-not-exist.html"), nullptr);
    EXPECT_GT(system.getErrorCount() + system.getWarningCount(), 0u);

    Rml::RemoveContext("missing");
    Rml::Shutdown();
}

TEST(RmlCoreSmokeTest, InitialiseFailsWithoutFontEngineIsReported){
    ASSERT_FALSE(HtmlUI::isInitialized());

    // a failed Rml::Initialise leaves RmlUi half set up, so it runs in a child process
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EXIT({
        RmlSystem& system = HtmlUI::getSystemInterface();
        system.resetCounters();
        std::string lastError;
        system.setLogListener([&lastError](Rml::Log::Type type, const std::string& message){
            if (type == Rml::Log::LT_ERROR)
                lastError = message;
        });

        Rml::SetSystemInterface(&system);
        Rml::SetFileInterface(&HtmlUI::getFileInterface());
        Rml::SetFontEngineInterface(nullptr);
        const bool initialised = Rml::Initialise();

        const bool reported = !initialised && system.getErrorCount() == 1 &&
            lastError.find("font engine") != std::string::npos &&
            TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI] No font engine interface set!") == 1;
        std::_Exit(reported ? 0 : 1);
    }, ::testing::ExitedWithCode(0), "");
}
