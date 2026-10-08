// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_TEST_COMMON_H
#define COMPAT_RUNTIME_TEST_COMMON_H

// A live RmlUi context for the compat runtime tests (p2-runtime): HtmlUI acquired (so
// registerRuntime ran), a context drawn by a RecordingRenderInterface (CPU only), helpers to
// load an RML body with a style sheet, run frames and send pointer and key input.

#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "htmlui/compat/runtime/RuntimeRegistry.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <string>

namespace jasperengine::test::compat {

    class RuntimeContextTest : public ::testing::Test {
    protected:
        static constexpr int width = 800;
        static constexpr int height = 600;

        void SetUp() override{
            TestSystem::get().clearLogs();
            ASSERT_TRUE(HtmlUI::acquire());
            static int counter = 0;
            name = "compat-runtime-" + std::to_string(++counter);
            context = Rml::CreateContext(name, Rml::Vector2i(width, height), &recording);
            ASSERT_NE(context, nullptr);
        }

        void TearDown() override{
            if (context){
                context->UnloadAllDocuments();
                context->Update();
                Rml::RemoveContext(name);
                context = nullptr;
            }
            Rml::ReleaseTextures(&recording);
            Rml::ReleaseRenderManagers();
            HtmlUI::release();
            EXPECT_EQ(recording.getLiveTextureCount(), 0u);
            EXPECT_EQ(recording.getLiveGeometryCount(), 0u);
        }

        // An RML document: `style` goes into the head, `body` into <body>. The body fills the
        // context and every element is a block unless the style says otherwise.
        Rml::ElementDocument* show(const std::string& body, const std::string& style = "",
                                   const std::string& url = "compat/runtime/test.rml"){
            // one document at a time
            if (document){
                context->UnloadAllDocuments();
                context->Update();
                document = nullptr;
            }
            const std::string rml = "<rml><head><style>body { display: block; width: " + std::to_string(width) +
                                    "px; height: " + std::to_string(height) + "px; font-family: sans-serif; font-size: 16px; "
                                    "color: #000000; } div, p, ul, ol, li, summary, details, dialog, form, button { display: block; } " +
                                    style + "</style></head><body>" + body + "</body></rml>";
            document = context->LoadDocumentFromMemory(rml, url);
            if (document)
                document->Show();
            frame();
            return document;
        }

        // Context::Update (after the compat runtime, as HtmlUIContext::update does) and Render
        void frame(){
            htmlui::compat::updateRuntime(context);
            context->Update();
            recording.beginFrame();
            context->Render();
        }

        Rml::Element* byId(const std::string& id){
            return document ? document->GetElementById(id) : nullptr;
        }

        Rml::Vector2f centre(Rml::Element* element){
            return element->GetAbsoluteOffset(Rml::BoxArea::Border) + element->GetBox().GetSize(Rml::BoxArea::Border) * 0.5f;
        }

        void click(Rml::Element* element, int modifiers = 0){
            const Rml::Vector2f at = centre(element);
            context->ProcessMouseMove((int)at.x, (int)at.y, modifiers);
            context->ProcessMouseButtonDown(0, modifiers);
            context->ProcessMouseButtonUp(0, modifiers);
            frame();
        }

        void key(Rml::Input::KeyIdentifier identifier, int modifiers = 0){
            context->ProcessKeyDown(identifier, modifiers);
            context->ProcessKeyUp(identifier, modifiers);
            frame();
        }

        void text(const std::string& characters){
            context->ProcessTextInput(characters);
            frame();
        }

        RecordingRenderInterface recording;
        Rml::Context* context = nullptr;
        Rml::ElementDocument* document = nullptr;
        std::string name;
    };

}

#endif //COMPAT_RUNTIME_TEST_COMMON_H
