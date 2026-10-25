// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "harness/RecordingRenderInterface.h"

#include "HtmlUI.h"
#include "RmlSystem.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {
    const char* emptyDocument = "<rml><head></head><body style=\"display: block; width: 100px; height: 50px;\"></body></rml>";

    // a context on the CPU-only renderer proves RmlUi is usable
    bool loadDocumentInContext(RecordingRenderInterface& recording, const char* name){
        Rml::Context* context = Rml::CreateContext(name, Rml::Vector2i(320, 200), &recording);
        if (!context)
            return false;
        Rml::ElementDocument* document = context->LoadDocumentFromMemory(emptyDocument, name);
        const bool loaded = document != nullptr;
        if (document){
            document->Show();
            context->Update();
            context->Render();
        }
        Rml::RemoveContext(name);
        return loaded;
    }
}


TEST(HtmlUILifecycleTest, ReinitialiseAfterRelease){
    RecordingRenderInterface recording;

    ASSERT_TRUE(HtmlUI::acquire());
    EXPECT_TRUE(loadDocumentInContext(recording, "first"));
    HtmlUI::release();
    ASSERT_FALSE(HtmlUI::isInitialized());

    HtmlUI::getSystemInterface().resetCounters();
    ASSERT_TRUE(HtmlUI::acquire());
    EXPECT_TRUE(loadDocumentInContext(recording, "second"));
    EXPECT_EQ(HtmlUI::getSystemInterface().getErrorCount(), 0u);
    HtmlUI::release();
    ASSERT_FALSE(HtmlUI::isInitialized());

    EXPECT_EQ(recording.getLiveGeometryCount(), 0u);
    EXPECT_EQ(recording.getLiveTextureCount(), 0u);
}
