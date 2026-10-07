// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"

#include "Engine.h"
#include "HtmlUI.h"
#include "HtmlUIContext.h"
#include "RmlFile.h"
#include "RmlFontEngine.h"
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

TEST(HtmlUILifecycleTest, RefCount){
    ASSERT_EQ(HtmlUI::getReferenceCount(), 0);
    ASSERT_FALSE(HtmlUI::isInitialized());

    ASSERT_TRUE(HtmlUI::acquire());
    EXPECT_TRUE(HtmlUI::isInitialized());
    EXPECT_EQ(HtmlUI::getReferenceCount(), 1);
    // the engine's interfaces are the ones RmlUi uses
    EXPECT_EQ(Rml::GetSystemInterface(), &HtmlUI::getSystemInterface());
    EXPECT_EQ(Rml::GetFileInterface(), &HtmlUI::getFileInterface());
    EXPECT_EQ(Rml::GetFontEngineInterface(), &HtmlUI::getFontEngine());

    ASSERT_TRUE(HtmlUI::acquire());
    EXPECT_EQ(HtmlUI::getReferenceCount(), 2);

    HtmlUI::release();
    EXPECT_TRUE(HtmlUI::isInitialized());
    EXPECT_EQ(HtmlUI::getReferenceCount(), 1);
    EXPECT_EQ(Rml::GetSystemInterface(), &HtmlUI::getSystemInterface());

    HtmlUI::release();
    EXPECT_FALSE(HtmlUI::isInitialized());
    EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
    // Rml::Shutdown clears the interfaces
    EXPECT_EQ(Rml::GetSystemInterface(), nullptr);
}

TEST(HtmlUILifecycleTest, ReleaseWithoutAcquireWarns){
    TestSystem::get().clearLogs();
    HtmlUI::release();
    EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "without a matching acquire"), 1u);
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

TEST(HtmlUILifecycleTest, LoadFontFaceMissingFileFails){
    EXPECT_FALSE(HtmlUI::loadFontFace("fonts/does-not-exist.ttf"));
    EXPECT_FALSE(HtmlUI::loadFontFace("fonts/does-not-exist.ttf", true, "Missing", 700, true));

    TestSystem::get().clearLogs();
    EXPECT_FALSE(HtmlUI::loadFontFace(""));
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "empty font path"), 1u);
}

TEST(HtmlUILifecycleTest, ContextRegistry){
    HtmlUIContext first("registry-first");
    HtmlUIContext second("registry-second");
    const size_t before = HtmlUI::getContextCount();

    HtmlUI::registerContext(&first);
    HtmlUI::registerContext(&first);   // once only
    HtmlUI::registerContext(&second);
    HtmlUI::registerContext(nullptr);
    EXPECT_EQ(HtmlUI::getContextCount(), before + 2);

    HtmlUI::unregisterContext(&first);
    EXPECT_EQ(HtmlUI::getContextCount(), before + 1);
    HtmlUI::unregisterContext(&first);   // unknown: nothing
    HtmlUI::unregisterContext(&second);
    EXPECT_EQ(HtmlUI::getContextCount(), before);
}

TEST(HtmlUILifecycleTest, ViewDestroyedSubscription){
    const std::string tag = "jasperengine::HtmlUI_onViewDestroyed";

    ASSERT_TRUE(HtmlUI::acquire());
    // no GPU objects exist: losing the view is harmless
    HtmlUI::getSystemInterface().resetCounters();
    Engine::onViewDestroyed.call();
    EXPECT_EQ(HtmlUI::getSystemInterface().getErrorCount(), 0u);
    EXPECT_TRUE(Engine::onViewDestroyed.remove(tag));

    // Engine::clearAllSubscriptions (the editor stopping play mode) drops it the same way:
    // the next acquire subscribes again
    ASSERT_TRUE(HtmlUI::acquire());
    HtmlUI::release();
    EXPECT_TRUE(Engine::onViewDestroyed.remove(tag));
    ASSERT_TRUE(HtmlUI::acquire());

    // the last release unsubscribes
    HtmlUI::release();
    HtmlUI::release();
    EXPECT_FALSE(HtmlUI::isInitialized());
    EXPECT_FALSE(Engine::onViewDestroyed.remove(tag));
}

TEST(HtmlUILifecycleTest, DebuggerNeedsAContext){
    ASSERT_TRUE(HtmlUI::acquire());
    // a context without an RmlUi context (not created) cannot host the debugger
    HtmlUIContext context("debugger-host");
    TestSystem::get().clearLogs();
    HtmlUI::setDebuggerContext(&context, true);
    EXPECT_EQ(HtmlUI::getDebuggerContext(), nullptr);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "debugger needs a created"), 1u);

    HtmlUI::setDebuggerContext(nullptr, false);
    EXPECT_EQ(HtmlUI::getDebuggerContext(), nullptr);
    HtmlUI::release();
}
