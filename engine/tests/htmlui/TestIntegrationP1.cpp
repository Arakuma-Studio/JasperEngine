// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Phase-1 integration (p1-integrate): what crosses the work packages. A scene that uses
// every phase-1 feature at once (screen and texture-mode documents, Lua listeners, data
// models, anime, Lottie, SVG, filters, stencil clips, text effects): play stop releases
// what the scripts made in every context, repeated play/stop cycles leak nothing, hot reload
// of a playing document keeps its model, listeners and Lottie, texture-mode documents draw
// clips, layers and filters, input goes to screen documents, then the component UI, then
// documents on meshes, then the game, and the render statistics add up.

#include "TestGoldenCommon.h"
#include "TestDocumentCommon.h"
#include "TestRenderCommon.h"

#include "HtmlUIClock.h"
#include "HtmlUIContext.h"
#include "RmlRenderer.h"
#include "Input.h"
#include "LuaBinding.h"
#include "component/ButtonComponent.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/anim/AnimEngine.h"
#include "htmlui/anim/AnimLua.h"
#include "object/Camera.h"
#include "object/Primitive.h"
#include "object/ui/Button.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "render/SystemRender.h"
#include "subsystem/UIDocumentSystem.h"
#include "texture/Framebuffer.h"

#include "lua.hpp"
#include "LuaBridge.h"
#include "sokol_gfx.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::test;

namespace fs = std::filesystem;

namespace {

    const char* callbackTag = "htmlui-tests-integration";

    // runs a Lua chunk; "" when it ran, else the error
    std::string lua(const std::string& code){
        lua_State* L = LuaBinding::getLuaState();
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || LuaBinding::pcallWithTraceback(L, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    std::string luaText(const std::string& expression){
        lua_State* L = LuaBinding::getLuaState();
        const std::string code = "return tostring(" + expression + ")";
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || LuaBinding::pcallWithTraceback(L, 0, 1) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return "error: " + error;
        }
        std::string value = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        return value;
    }

    double luaNumber(const std::string& expression){
        const std::string text = luaText(expression);
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        return (end && *end == '\0' && !text.empty()) ? value : std::nan("");
    }

    void setGlobal(const char* name, UIDocument& document){
        lua_State* L = LuaBinding::getLuaState();
        if (!luabridge::push(L, document))
            lua_pushnil(L);
        lua_setglobal(L, name);
    }

    void clearGlobals(){
        lua("screenDoc, textureDoc, model, textureModel, anim, spin, clicks, loops, updates, changes, buys = "
            "nil, nil, nil, nil, nil, nil, nil, nil, nil, nil, nil; collectgarbage()");
    }

    // draws frames with the UI clock pinned and moved forward by step seconds each
    void drawFramesAt(double& time, int frames, double step){
        for (int i = 0; i < frames; i++){
            time += step;
            HtmlUIClock::setFixedTime(time);
            GpuEnvironment::drawFrames(1);
        }
    }

    // the scripts of a play session, as a game's would be: listeners, data models, anime with
    // callbacks and Lottie events in the screen document and in a texture-mode document
    const char* playScripts = R"(
        clicks, loops, updates, changes, buys = 0, 0, 0, 0, 0
        model = screenDoc:createDataModel("hud", { gold = 10, buy = function(event) buys = buys + 1 end })
        model:onChange("gold", function(value) changes = changes + 1 end)
        screenDoc:getElementById("buy"):addEventListener("click", function(event) clicks = clicks + 1 end)
        screenDoc:getElementById("spin"):addEventListener("loop", function(event) loops = loops + 1 end)
        screenDoc:addEventListener("keydown", function(event) end)
        textureModel = textureDoc:createDataModel("form", { label = "Bound" })
        textureDoc:getElementById("name"):addEventListener("click", function(event) clicks = clicks + 1 end)
        textureDoc:getElementById("name"):addEventListener("input", function(event) end, { once = true })
        anim = anime.animate("#box", { translateX = 100, duration = 400, loop = true,
                                       onUpdate = function(a) updates = updates + 1 end })
    )";

    // the GPU objects a context's renderer owns
    struct GpuHandles {
        std::vector<uint32_t> buffers, images;
        void add(HtmlUIContext* context){
            if (context)
                context->getRenderer().getGpuHandles(buffers, images);
        }
    };

    bool isBufferValid(uint32_t id){
        return sg_query_buffer_state(sg_buffer{id}) == SG_RESOURCESTATE_VALID;
    }

    bool isImageValid(uint32_t id){
        return sg_query_image_state(sg_image{id}) == SG_RESOURCESTATE_VALID;
    }

    // a SceneFixture whose documents are a screen document using every phase-1 feature and a
    // texture-mode document with a form and a data model
    class IntegrationTest : public FullPathGoldenTest {
    protected:
        void SetUp() override{
            FullPathGoldenTest::SetUp();
            ASSERT_NE(LuaBinding::getLuaState(), nullptr);
            AnimEngine::instance().reset();
            time = 0.0;
            HtmlUIClock::setFixedTime(time);
        }

        void TearDown() override{
            clearGlobals();
            AnimEngine::instance().reset();
            screenDocument.reset();
            textureDocument.reset();
            fixture.reset();
            lua("collectgarbage()");
            HtmlUIClock::reset();
            EXPECT_EQ(HtmlUIScriptState::liveLuaRefs(), 0u);
            EXPECT_EQ(AnimLua::getLiveLuaRefs(), 0u);
            EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 0u);
            EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
            FullPathGoldenTest::TearDown();
        }

        void createScene(){
            fixture.reset(new SceneFixture());
            screenDocument.reset(new UIDocument(fixture->addDocument("p1/integration/combined.html")));
            textureDocument.reset(new UIDocument(fixture->addDocument("p1/texture_form.html", 1)));
            UIDocumentComponent& component = texture().getComponent<UIDocumentComponent>();
            component.renderMode = UIDocumentRenderMode::TEXTURE;
            component.textureWidth = 256;
            component.textureHeight = 256;
        }

        UIDocumentSystem& system(){
            return *fixture->getScene().getSystem<UIDocumentSystem>();
        }

        // what the editor's play stop does with the scene's scripts (Project::stop: anime
        // first, then LuaBinding::cleanupLuaScripts for each runtime scene)
        void stopScripts(){
            AnimEngine::instance().reset();
            LuaBinding::cleanupLuaScripts(&fixture->getScene());
        }

        UIDocument& screen(){ return *screenDocument; }
        UIDocument& texture(){ return *textureDocument; }

        std::unique_ptr<SceneFixture> fixture;
        std::unique_ptr<UIDocument> screenDocument, textureDocument;
        double time = 0.0;
    };

}

TEST_F(IntegrationTest, PlayStopReleasesScriptsOfEveryContext){
    createScene();
    ASSERT_TRUE(fixture->waitUntilLoaded());
    ASSERT_TRUE(screen().isLoaded());
    ASSERT_TRUE(texture().isLoaded());
    ASSERT_NE(system().getTextureContext(texture().getEntity()), nullptr);

    setGlobal("screenDoc", screen());
    setGlobal("textureDoc", texture());
    ASSERT_EQ(lua(playScripts), "");
    drawFramesAt(time, 30, 1.0 / 30.0);

    // everything runs: the animation, the Lottie, both models
    EXPECT_GT(luaNumber("updates"), 0);
    EXPECT_TRUE(screen().getElementById("spin").isPlaying());
    EXPECT_EQ(screen().getElementById("gold").getTextContent(), "10");
    EXPECT_EQ(texture().getElementById("bound").getTextContent(), "Bound");
    EXPECT_GT(HtmlUIScriptState::liveLuaRefs(), 0u);
    EXPECT_GT(AnimLua::getLiveLuaRefs(), 0u);

    // play stop: nothing the scripts made survives it, in the screen context nor in the
    // texture document's own context, at once (the code behind C++ listeners is unloaded
    // right after, so nothing may wait for the next update)
    stopScripts();
    EXPECT_EQ(HtmlUIScriptState::liveLuaRefs(), 0u);
    EXPECT_EQ(AnimLua::getLiveLuaRefs(), 0u);
    EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 0u);
    EXPECT_TRUE(system().getDataModelNames(screen().getEntity()).empty());
    EXPECT_TRUE(system().getDataModelNames(texture().getEntity()).empty());
    HtmlUIContext* textureContext = system().getTextureContext(texture().getEntity());
    ASSERT_NE(textureContext, nullptr);
    EXPECT_EQ(textureContext->getRmlContext()->GetNumDocuments(), 0);
    EXPECT_EQ(system().getContext()->getRmlContext()->GetNumDocuments(), 0);

    // the documents load again as they are on disk, with no error, and draw
    clearGlobals();
    ASSERT_TRUE(fixture->waitUntilLoaded());
    EXPECT_TRUE(screen().isLoaded());
    EXPECT_TRUE(texture().isLoaded());
    EXPECT_EQ(screen().getElementById("gold").getTextContent(), "");
    EXPECT_EQ(textureContext->getRmlContext()->GetNumDocuments(), 1);
    Screen frame;
    ASSERT_TRUE(capture(frame));
    EXPECT_TRUE(frame.is(500, 160, 255, 255, 0));   // #box, back where the file puts it
}

TEST_F(IntegrationTest, PlayStopCyclesLeakNothing){
    createScene();
    ASSERT_TRUE(fixture->waitUntilLoaded());

    size_t screenTextures = 0, screenGeometry = 0, textureTextures = 0, textureGeometry = 0, pipelines = 0;
    for (int cycle = 0; cycle < 12; cycle++){
        setGlobal("screenDoc", screen());
        setGlobal("textureDoc", texture());
        ASSERT_EQ(lua(playScripts), "") << "cycle " << cycle;
        drawFramesAt(time, 4, 0.25);
        stopScripts();
        clearGlobals();
        ASSERT_TRUE(fixture->waitUntilLoaded()) << "cycle " << cycle;
        drawFramesAt(time, 2, 0.0);

        HtmlUIContext* screenContext = system().getContext();
        HtmlUIContext* textureContext = system().getTextureContext(texture().getEntity());
        ASSERT_TRUE(screenContext && textureContext);
        // the Lottie's dynamic texture, the SVG, the image, font and effect pages: as many as
        // after the first play, and the pipeline cache does not grow
        if (cycle == 1){
            screenTextures = screenContext->getRenderer().getLiveTextureCount();
            screenGeometry = screenContext->getRenderer().getLiveGeometryCount();
            textureTextures = textureContext->getRenderer().getLiveTextureCount();
            textureGeometry = textureContext->getRenderer().getLiveGeometryCount();
            pipelines = SystemRender::getPipelineCacheSize();
            EXPECT_GE(screenTextures, 4u);
            EXPECT_GT(screenGeometry, 0u);
            EXPECT_GT(textureGeometry, 0u);
        }else if (cycle > 1){
            EXPECT_EQ(screenContext->getRenderer().getLiveTextureCount(), screenTextures) << "cycle " << cycle;
            EXPECT_EQ(screenContext->getRenderer().getLiveGeometryCount(), screenGeometry) << "cycle " << cycle;
            EXPECT_EQ(textureContext->getRenderer().getLiveTextureCount(), textureTextures) << "cycle " << cycle;
            EXPECT_EQ(textureContext->getRenderer().getLiveGeometryCount(), textureGeometry) << "cycle " << cycle;
            EXPECT_EQ(SystemRender::getPipelineCacheSize(), pipelines) << "cycle " << cycle;
        }
        EXPECT_EQ(HtmlUIScriptState::liveLuaRefs(), 0u) << "cycle " << cycle;
        if (::testing::Test::HasFailure())
            break;
    }

    // the scene goes (the editor's play copies, a scene reload): no GPU object of either
    // context stays
    GpuHandles handles;
    handles.add(system().getContext());
    handles.add(system().getTextureContext(texture().getEntity()));
    EXPECT_FALSE(handles.buffers.empty());
    EXPECT_FALSE(handles.images.empty());
    for (uint32_t id : handles.buffers) EXPECT_TRUE(isBufferValid(id));
    for (uint32_t id : handles.images) EXPECT_TRUE(isImageValid(id));
    ASSERT_NE(texture().getTargetTexture(), nullptr);
    screenDocument.reset();
    textureDocument.reset();
    fixture.reset();
    EXPECT_FALSE(HtmlUI::isInitialized());
    EXPECT_EQ(HtmlUI::getContextCount(), 0u);
    for (uint32_t id : handles.buffers) EXPECT_FALSE(isBufferValid(id)) << "buffer " << id;
    for (uint32_t id : handles.images) EXPECT_FALSE(isImageValid(id)) << "image " << id;
}
