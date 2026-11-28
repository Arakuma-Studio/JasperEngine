// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A view loss keeps the documents (S95 REQ-15, AC-16): Android drops the EGL context in the
// background (LOW_MEMORY without a window, EGL_CONTEXT_LOST) and a browser the WebGL context.
// Only the GPU objects go: the context, the DOM a script changed, its element handles, listeners
// and scroll stay, no onLoad or onUnload fires, and the frame after the return is the one before.
// Both routes: the platforms' Engine::systemViewDestroyed / systemViewLoaded, and the scenes-only
// release the editor's send_input app view_lost / view_restored runs.

#include "TestGoldenCommon.h"
#include "TestScriptCommon.h"

#include "Input.h"
#include "component/UIDocumentComponent.h"
#include "texture/Framebuffer.h"

#include <RmlUi/Core/Element.h>

#include <cstdlib>
#include <functional>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class ViewLossTest : public ScriptTest {
    protected:
        int loads = 0;
        int unloads = 0;
        int loadsBefore = 0;

        void SetUp() override{
            ScriptTest::SetUp();
            UIDocumentComponent& component = document->getComponent<UIDocumentComponent>();
            component.onLoad.add("view_loss_load", std::function<void()>([this](){ loads++; }));
            component.onUnload.add("view_loss_unload", std::function<void()>([this](){ unloads++; }));
        }

        // the scene and its document drawn (the fixture's own wait does not know the document
        // ScriptTest loaded)
        bool drawn(){
            return fixture->waitUntilLoaded() && GpuEnvironment::runFrames([this](){ return documentSystem(scene()).isAllLoaded(); });
        }

        // what a game's script did to its document before the app went to the background
        void changeTheDocument(){
            ASSERT_TRUE(drawn());
            // (UIDocument::load in SetUp fired its onLoad before the listener above)
            loadsBefore = loads;
            EXPECT_LUA(R"(
                clicks = 0
                el = doc:getElementById("label")
                el.textContent = "Kept"
                local btn = doc:getElementById("btn")
                btn:setClass("on", true)
                btn:addEventListener("click", function() clicks = clicks + 1 end)
                doc:getElementById("scroller").scrollTop = 30
            )");
            ASSERT_TRUE(drawn());
        }

        // after the return: the same elements, as the script left them, and a click reaches the listener
        void expectTheDocumentKept(Rml::Element* labelBefore){
            Rml::ElementDocument* rmlDocument = documentSystem(scene()).getDocument(document->getEntity());
            ASSERT_NE(rmlDocument, nullptr);
            EXPECT_EQ(rmlDocument->GetElementById("label"), labelBefore);
            EXPECT_LUA(R"(
                assert(el == doc:getElementById("label"), "a new element")
                assert(el.textContent == "Kept", "text " .. tostring(el.textContent))
                assert(doc:getElementById("btn"):hasClass("on"), "class lost")
                local scroll = doc:getElementById("scroller").scrollTop
                assert(scroll == 30, "scrollTop " .. tostring(scroll))
            )");
            UIDocumentSystem& system = documentSystem(scene());
            EXPECT_TRUE(system.eventOnPointerDown(0, D_MOUSE_BUTTON_LEFT, 350.0f, 40.0f, 0, false));
            EXPECT_TRUE(system.eventOnPointerUp(0, D_MOUSE_BUTTON_LEFT, 350.0f, 40.0f, 0, false));
            EXPECT_EQ(luaString("clicks"), "1");
            EXPECT_EQ(system.getLoadCount(document->getEntity()), 1u);
            EXPECT_EQ(loads, loadsBefore);
            EXPECT_EQ(unloads, 0);
            lua("clicks = nil");
        }

        static size_t differingPixels(const Screen& a, const Screen& b){
            if (a.width != b.width || a.height != b.height)
                return (size_t)-1;
            size_t count = 0;
            for (size_t i = 0; i + 3 < a.rgba.size(); i += 4){
                for (int c = 0; c < 3; c++){
                    if (std::abs((int)a.rgba[i + c] - (int)b.rgba[i + c]) > 2){
                        count++;
                        break;
                    }
                }
            }
            return count;
        }

        static ::testing::AssertionResult capture(Screen& screen, int frames = 2){
            GpuEnvironment::drawFrames(frames);
            if (!screen.read())
                return ::testing::AssertionFailure() << "readback of the target failed";
            return ::testing::AssertionSuccess();
        }
    };

}

TEST_F(ViewLossTest, PlatformViewLossKeepsTheDocument){
    changeTheDocument();
    Screen before;
    ASSERT_TRUE(capture(before));
    HtmlUIContext* context = documentSystem(scene()).getContext();
    ASSERT_NE(context, nullptr);
    Rml::Element* label = documentSystem(scene()).getDocument(document->getEntity())->GetElementById("label");

    // the platform drops the view (Android LOW_MEMORY without a window, a lost WebGL context)
    Engine::systemViewDestroyed();
    EXPECT_FALSE(Engine::isViewLoaded());
    EXPECT_EQ(documentSystem(scene()).getContext(), context);
    EXPECT_TRUE(document->isLoaded());
    EXPECT_FALSE(context->hasRenderedFrame());
    EXPECT_FALSE(document->getComponent<UIDocumentComponent>().needReload);

    // and brings it back: the textures are made again as the document draws
    Engine::systemViewLoaded();
    Engine::systemViewChanged();
    ASSERT_TRUE(drawn());
    EXPECT_EQ(documentSystem(scene()).getContext(), context);
    EXPECT_TRUE(context->hasRenderedFrame());

    Screen after;
    ASSERT_TRUE(capture(after));
    EXPECT_EQ(differingPixels(before, after), 0u);
    expectTheDocumentKept(label);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}

TEST_F(ViewLossTest, ScenesGpuReleaseKeepsTheDocument){
    changeTheDocument();
    Screen before;
    ASSERT_TRUE(capture(before));
    HtmlUIContext* context = documentSystem(scene()).getContext();
    ASSERT_NE(context, nullptr);
    Rml::Element* label = documentSystem(scene()).getDocument(document->getEntity())->GetElementById("label");

    // the editor's view_lost: this scene's GPU objects only, the view stays
    Engine::releaseScenesGpu({&scene()});
    EXPECT_TRUE(Engine::isViewLoaded());
    EXPECT_FALSE(context->hasRenderedFrame());
    EXPECT_EQ(documentSystem(scene()).getContext(), context);

    Engine::restoreScenesGpu({&scene()});
    ASSERT_TRUE(drawn());
    EXPECT_TRUE(context->hasRenderedFrame());

    Screen after;
    ASSERT_TRUE(capture(after));
    EXPECT_EQ(differingPixels(before, after), 0u);
    expectTheDocumentKept(label);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}

TEST_F(ViewLossTest, TextureDocumentTargetComesBack){
    UIDocument texture = fixture->addDocument("ecs/basic.rml", 1);
    UIDocumentComponent& component = texture.getComponent<UIDocumentComponent>();
    component.renderMode = UIDocumentRenderMode::TEXTURE;
    component.textureWidth = 128;
    component.textureHeight = 64;
    ASSERT_TRUE(fixture->waitUntilLoaded());
    UIDocumentSystem& system = documentSystem(scene());
    Framebuffer* target = system.getTargetTexture(texture.getEntity());
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->isCreated());
    HtmlUIContext* textureContext = system.getTextureContext(texture.getEntity());
    Rml::ElementDocument* rmlDocument = system.getDocument(texture.getEntity());
    std::vector<uint8_t> before;
    ASSERT_TRUE(readTexture(target->getRender().getColorTexture(), 128, 64, before));

    Engine::systemViewDestroyed();
    // the Framebuffer its consumers hold stays; its texture went with the view
    EXPECT_EQ(system.getTargetTexture(texture.getEntity()), target);
    EXPECT_FALSE(target->isCreated());
    Engine::systemViewLoaded();
    Engine::systemViewChanged();
    ASSERT_TRUE(fixture->waitUntilLoaded());

    EXPECT_EQ(system.getTargetTexture(texture.getEntity()), target);
    EXPECT_EQ(system.getTextureContext(texture.getEntity()), textureContext);
    EXPECT_EQ(system.getDocument(texture.getEntity()), rmlDocument);
    ASSERT_TRUE(target->isCreated());
    std::vector<uint8_t> after;
    ASSERT_TRUE(readTexture(target->getRender().getColorTexture(), 128, 64, after));
    EXPECT_EQ(before, after);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}
