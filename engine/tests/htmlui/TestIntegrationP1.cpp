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

namespace {

    // counts the events of one type on an element
    class CountingListener : public Rml::EventListener {
    public:
        int count = 0;
        void ProcessEvent(Rml::Event&) override{ count++; }
    };

    // the canvas point a world point projects to through the scene camera
    Vector2 project(Scene& scene, const Vector3& world){
        const CameraComponent& camera = scene.getComponent<CameraComponent>(scene.getCamera());
        Vector4 clip = camera.viewProjectionMatrix * Vector4(world.x, world.y, world.z, 1.0f);
        const float x = clip.x / clip.w, y = clip.y / clip.w;
        return Vector2((x + 1.0f) * 0.5f * Engine::getCanvasWidth(), (1.0f - y) * 0.5f * Engine::getCanvasHeight());
    }

    // One 3D scene with the three kinds of UI: a screen document (ecs/input.html: #button at
    // (100, 100) 160x48 on a transparent body), component UI buttons, and a texture-mode
    // document (p1/texture_input.html: #tl and #br quadrants on a transparent body) on a
    // wall, all fed through the engine's Engine::system* entry points
    class InputRoutingTest : public IntegrationTest {
    protected:
        void SetUp() override{
            IntegrationTest::SetUp();
            Engine::onMouseDown.add(callbackTag, std::function<void(int, float, float, int)>([this](int, float, float, int){ gameDowns++; }));
            Engine::onMouseUp.add(callbackTag, std::function<void(int, float, float, int)>([this](int, float, float, int){ gameUps++; }));
            Engine::onTouchStart.add(callbackTag, std::function<void(int, float, float)>([this](int, float, float){ gameTouches++; }));
        }

        void TearDown() override{
            Engine::onMouseDown.remove(callbackTag);
            Engine::onMouseUp.remove(callbackTag);
            Engine::onTouchStart.remove(callbackTag);
            Engine::systemTouchCancel();
            IntegrationTest::TearDown();
        }

        void createRoutingScene(){
            fixture.reset(new SceneFixture());
            Camera& camera = fixture->add<Camera>();
            camera.setPosition(0, 0, 5);
            camera.setTarget(0, 0, 0);
            camera.activate();
            // a 3D scene takes component UI events when asked to (a UI camera's scene does by default)
            fixture->getScene().setEnableUIEvents(true);

            Primitive& wall = fixture->add<Primitive>();
            wall.createWall(2, 2);
            wall.setPosition(1.5f, 0, 0);

            screenDocument.reset(new UIDocument(fixture->addDocument("ecs/input.html")));
            textureDocument.reset(new UIDocument(fixture->addDocument("p1/texture_input.html")));
            UIDocumentComponent& component = texture().getComponent<UIDocumentComponent>();
            component.renderMode = UIDocumentRenderMode::TEXTURE;
            component.textureWidth = 256;
            component.textureHeight = 256;
            component.inputSurface = wall.getEntity();
            ASSERT_TRUE(fixture->waitUntilLoaded());

            // a component button over the world document's #tl, one where nothing else is
            tl = project(fixture->getScene(), Vector3(1.0f, 0.5f, 0));
            br = project(fixture->getScene(), Vector3(2.0f, -0.5f, 0));
            worldBody = project(fixture->getScene(), Vector3(2.0f, 0.5f, 0));
            overWorld = &fixture->add<Button>();
            overWorld->setPosition(tl.x - 15, tl.y - 15);
            overWorld->setSize(30, 30);
            alone = &fixture->add<Button>();
            alone->setPosition(20, 280);
            alone->setSize(60, 40);
            overWorld->getComponent<ButtonComponent>().onPress.add(callbackTag, std::function<void()>([this](){ overWorldPresses++; }));
            alone->getComponent<ButtonComponent>().onPress.add(callbackTag, std::function<void()>([this](){ alonePresses++; }));
            GpuEnvironment::drawFrames(2);

            Rml::ElementDocument* world = system().getDocument(texture().getEntity());
            ASSERT_NE(world, nullptr);
            worldTl = world->GetElementById("tl");
            worldBr = world->GetElementById("br");
            screenButton = system().getDocument(screen().getEntity())->GetElementById("button");
            ASSERT_TRUE(worldTl && worldBr && screenButton);
            worldTl->AddEventListener(Rml::EventId::Click, &worldTlClicks);
            worldBr->AddEventListener(Rml::EventId::Click, &worldBrClicks);
            screenButton->AddEventListener(Rml::EventId::Click, &screenClicks);
        }

        void removeListeners(){
            worldTl->RemoveEventListener(Rml::EventId::Click, &worldTlClicks);
            worldBr->RemoveEventListener(Rml::EventId::Click, &worldBrClicks);
            screenButton->RemoveEventListener(Rml::EventId::Click, &screenClicks);
        }

        void click(Vector2 point){
            Engine::systemMouseMove(point.x, point.y, 0);
            Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, point.x, point.y, 0);
            Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, point.x, point.y, 0);
        }

        void tap(int pointer, Vector2 point){
            Engine::systemTouchStart(pointer, point.x, point.y);
            Engine::systemTouchEnd(pointer, point.x, point.y);
        }

        Vector2 tl, br, worldBody;
        Button* overWorld = nullptr;
        Button* alone = nullptr;
        Rml::Element* worldTl = nullptr;
        Rml::Element* worldBr = nullptr;
        Rml::Element* screenButton = nullptr;
        CountingListener worldTlClicks, worldBrClicks, screenClicks;
        int gameDowns = 0, gameUps = 0, gameTouches = 0, overWorldPresses = 0, alonePresses = 0;
    };

}

TEST_F(InputRoutingTest, ScreenDocumentsThenComponentUIThenWorldThenGame){
    createRoutingScene();
    ASSERT_TRUE(Engine::isIgnoreEventsHandledByUI());

    // the screen document's button: the document's, nobody else's
    click(Vector2(180, 124));
    EXPECT_EQ(screenClicks.count, 1);
    EXPECT_EQ(gameDowns, 0);

    // a component button where nothing else is: the component UI's
    click(Vector2(50, 300));
    EXPECT_EQ(alonePresses, 1);
    EXPECT_EQ(gameDowns, 0);

    // the world document's #br: the world document's
    click(br);
    EXPECT_EQ(worldBrClicks.count, 1);
    EXPECT_EQ(system().getTextureFocus(), texture().getEntity());
    EXPECT_EQ(gameDowns, 0);

    // a component button drawn over the world document's #tl: the component UI is in front
    // of the 3D world, so the button gets the press and the document under it nothing; the
    // press elsewhere takes the keys away from the world document
    click(tl);
    EXPECT_EQ(overWorldPresses, 1);
    EXPECT_EQ(worldTlClicks.count, 0);
    EXPECT_FALSE(worldTl->IsPseudoClassSet("hover"));
    EXPECT_EQ(system().getTextureFocus(), NULL_ENTITY);
    EXPECT_EQ(gameDowns, 0);

    // hovering the component button does not hover the world element under it
    Engine::systemMouseMove(br.x, br.y, 0);
    EXPECT_TRUE(worldBr->IsPseudoClassSet("hover"));
    Engine::systemMouseMove(tl.x, tl.y, 0);
    EXPECT_FALSE(worldTl->IsPseudoClassSet("hover"));
    EXPECT_FALSE(worldBr->IsPseudoClassSet("hover"));

    // a press taken by the world document keeps the pointer over the component button
    // (capture): the release is the document's, the button is not pressed
    Engine::systemMouseMove(br.x, br.y, 0);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, br.x, br.y, 0);
    EXPECT_TRUE(worldBr->IsPseudoClassSet("active"));
    Engine::systemMouseMove(tl.x, tl.y, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, tl.x, tl.y, 0);
    EXPECT_FALSE(worldBr->IsPseudoClassSet("active"));
    EXPECT_FALSE(system().hasPointerCapture(0, D_MOUSE_BUTTON_LEFT, false));
    EXPECT_EQ(overWorldPresses, 1);
    EXPECT_EQ(gameDowns, 0);
    EXPECT_EQ(gameUps, 0);

    // the world document's transparent body and empty space: the game's
    click(worldBody);
    EXPECT_EQ(gameDowns, 1);
    click(Vector2(600, 330));
    EXPECT_EQ(gameDowns, 2);
    EXPECT_EQ(worldTlClicks.count, 0);
    EXPECT_EQ(worldBrClicks.count, 1);
    EXPECT_EQ(screenClicks.count, 1);

    // touch follows the same order
    tap(1, tl);
    EXPECT_EQ(overWorldPresses, 2);
    EXPECT_EQ(worldTlClicks.count, 0);
    tap(2, br);
    EXPECT_EQ(worldBrClicks.count, 2);
    tap(3, Vector2(180, 124));
    EXPECT_EQ(screenClicks.count, 2);
    EXPECT_EQ(gameTouches, 0);
    tap(4, worldBody);
    EXPECT_EQ(gameTouches, 1);

    removeListeners();
}

namespace {

    // a playing document in a scratch directory (absolute paths), so the test can change it
    class PlayReloadTest : public IntegrationTest {
    protected:
        fs::path dir;

        void SetUp() override{
            IntegrationTest::SetUp();
            dir = fs::path(JASPER_HTMLUI_TEST_BINARY_DIR) / "integration" / "play-reload";
            fs::remove_all(dir);
            fs::create_directories(dir);
            fs::copy_file(fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / "fixtures/p1/lottie/shapes.json", dir / "shapes.json");
            write("play.css", css("#ffff00"));
            write("play.html", html(""));
        }

        std::string path(const std::string& name) const{
            return (dir / name).generic_string();
        }

        static std::string css(const std::string& boxColor){
            return "body { display: block; position: absolute; left: 0; top: 0; width: 100%; height: 100%; margin: 0; "
                   "font-family: sans-serif; font-size: 20px; color: #ffffff; }\n"
                   "div, p { display: block; }\n"
                   "#spin { position: absolute; left: 16px; top: 16px; width: 100px; height: 100px; }\n"
                   "#hud { position: absolute; left: 160px; top: 16px; }\n"
                   "#buy { position: absolute; left: 160px; top: 60px; width: 100px; height: 40px; background-color: #808080; }\n"
                   "#name { position: absolute; left: 300px; top: 60px; width: 160px; height: 30px; background-color: #ffffff; color: #000000; display: block; }\n"
                   "#box { position: absolute; left: 480px; top: 140px; width: 40px; height: 40px; background-color: " + boxColor + "; }\n";
        }

        static std::string html(const std::string& extra){
            return "<html>\n<head>\n    <title>play</title>\n    <link type=\"text/css\" href=\"play.css\"/>\n</head>\n<body>\n"
                   "    <lottie id=\"spin\" src=\"shapes.json\" autoplay=\"autoplay\" loop=\"loop\"/>\n"
                   "    <div id=\"hud\" data-model=\"hud\">Gold: <span id=\"gold\">{{ gold }}</span></div>\n"
                   "    <div id=\"buy\">Buy</div>\n"
                   "    <input id=\"name\" type=\"text\" value=\"\"/>\n"
                   "    <div id=\"box\"></div>\n" + extra +
                   "</body>\n</html>\n";
        }

        // a new content and a later modification time, whatever the file system's resolution
        void write(const std::string& name, const std::string& content){
            const fs::path file = dir / name;
            std::error_code error;
            const bool existed = fs::exists(file, error);
            const fs::file_time_type before = existed ? fs::last_write_time(file, error) : fs::file_time_type();
            {
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out << content;
            }
            if (existed)
                fs::last_write_time(file, before + std::chrono::seconds(2), error);
        }
    };

}

TEST_F(PlayReloadTest, DocumentChangeKeepsModelListenersLottieAndForm){
    fixture.reset(new SceneFixture());
    screenDocument.reset(new UIDocument(fixture->addDocument(path("play.html"))));
    ASSERT_TRUE(fixture->waitUntilLoaded());
    const Entity entity = screen().getEntity();
    setGlobal("screenDoc", screen());

    // what a game's init does: a model first (the document reloads to bind it), listeners on
    // elements with an id, an animation with a callback, the Lottie's loop event
    ASSERT_EQ(lua(R"(
        model = screenDoc:createDataModel("hud", { gold = 10 })
        clicks, loops, updates = 0, 0, 0
        screenDoc:getElementById("buy"):addEventListener("click", function(event) clicks = clicks + 1 end)
        screenDoc:getElementById("spin"):addEventListener("loop", function(event) loops = loops + 1 end)
        anim = anime.animate("#box", { translateX = 100, duration = 400, loop = true,
                                       onUpdate = function(a) updates = updates + 1 end })
    )"), "");
    // 2.5 s: the 2 s Lottie wraps once
    drawFramesAt(time, 75, 1.0 / 30.0);
    EXPECT_EQ(screen().getElementById("gold").getTextContent(), "10");
    EXPECT_GE(luaNumber("loops"), 1);
    EXPECT_GT(luaNumber("updates"), 0);
    EXPECT_TRUE(screen().getElementById("spin").isPlaying());
    // a value typed into the field
    screen().getElementById("name").focus();
    EXPECT_TRUE(system().eventOnTextInput("kept"));
    drawFramesAt(time, 1, 0.0);
    HtmlUIContext* context = system().getContext();
    ASSERT_NE(context, nullptr);
    const size_t textures = context->getRenderer().getLiveTextureCount();
    Rml::ElementDocument* before = system().getDocument(entity);

    // the .html changes while playing: a full reload that keeps the state
    write("play.html", html("    <div id=\"extra\">new</div>\n"));
    const std::vector<UIDocumentSystem::Reloaded> reloaded = system().reloadChangedFiles({path("play.html")});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_FALSE(reloaded[0].stylesheetOnly);
    drawFramesAt(time, 2, 1.0 / 30.0);
    ASSERT_NE(system().getDocument(entity), before);
    EXPECT_TRUE(screen().getElementById("extra").isValid());

    // the model stays bound and live
    EXPECT_EQ(screen().getElementById("gold").getTextContent(), "10");
    ASSERT_EQ(lua("model.gold = 25"), "");
    drawFramesAt(time, 1, 0.0);
    EXPECT_EQ(screen().getElementById("gold").getTextContent(), "25");
    // the listeners moved to the new elements with the same id
    const double clicksBefore = luaNumber("clicks");
    ASSERT_EQ(lua("screenDoc:getElementById('buy'):click()"), "");
    EXPECT_EQ(luaNumber("clicks"), clicksBefore + 1);
    // the form value and focus
    EXPECT_EQ(screen().getElementById("name").getValue(), "kept");
    // the new Lottie plays (one texture, the old one released) and its loop listener fires
    const double loopsBefore = luaNumber("loops");
    drawFramesAt(time, 75, 1.0 / 30.0);
    EXPECT_TRUE(screen().getElementById("spin").isPlaying());
    EXPECT_GT(luaNumber("loops"), loopsBefore);
    EXPECT_EQ(context->getRenderer().getLiveTextureCount(), textures);
    // the animation's element went with the old document: it is cancelled, not an error
    EXPECT_EQ(luaText("anim.cancelled"), "true");

    // a new animation on the new #box, then a stylesheet-only change: the DOM, the animation
    // and the Lottie's texture stay
    ASSERT_EQ(lua(R"(
        updates = 0
        anim = anime.animate("#box", { translateX = 100, duration = 400, loop = true,
                                       onUpdate = function(a) updates = updates + 1 end })
    )"), "");
    drawFramesAt(time, 5, 1.0 / 30.0);
    Rml::ElementDocument* current = system().getDocument(entity);
    write("play.css", css("#00ffff"));
    const std::vector<UIDocumentSystem::Reloaded> restyled = system().reloadChangedFiles({path("play.css")});
    ASSERT_EQ(restyled.size(), 1u);
    EXPECT_TRUE(restyled[0].stylesheetOnly);
    const double updatesBefore = luaNumber("updates");
    drawFramesAt(time, 5, 1.0 / 30.0);
    EXPECT_EQ(system().getDocument(entity), current);
    EXPECT_GT(luaNumber("updates"), updatesBefore);
    EXPECT_EQ(luaText("anim.cancelled"), "false");
    EXPECT_EQ(screen().getElementById("box").getStyle("background-color"), "#00ffff");
    EXPECT_EQ(context->getRenderer().getLiveTextureCount(), textures);

    // play stop after all that: nothing left
    stopScripts();
    EXPECT_EQ(HtmlUIScriptState::liveLuaRefs(), 0u);
    EXPECT_EQ(AnimLua::getLiveLuaRefs(), 0u);
}

namespace {

    struct Texels {
        std::vector<uint8_t> rgba;
        int width = 0, height = 0;
        const uint8_t* at(int x, int y) const{ return &rgba[(size_t(y) * width + x) * 4]; }
        ::testing::AssertionResult is(int x, int y, int r, int g, int b, int a, int tolerance = 2) const{
            const uint8_t* p = at(x, y);
            if (std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance &&
                std::abs(p[3] - a) <= tolerance)
                return ::testing::AssertionSuccess();
            return ::testing::AssertionFailure() << "texel (" << x << ", " << y << ") is (" << int(p[0]) << ", " << int(p[1]) << ", "
                                                 << int(p[2]) << ", " << int(p[3]) << "), expected (" << r << ", " << g << ", " << b
                                                 << ", " << a << ") +-" << tolerance;
        }
        bool read(Framebuffer& target){
            width = (int)target.getWidth();
            height = (int)target.getHeight();
            return readTexture(target.getRender().getColorTexture(), width, height, rgba);
        }
    };

}

TEST_F(IntegrationTest, TextureDocumentDrawsClipsLayersAndFilters){
    fixture.reset(new SceneFixture());
    textureDocument.reset(new UIDocument(fixture->addDocument("p1/integration/texture_effects.html")));
    UIDocumentComponent& component = texture().getComponent<UIDocumentComponent>();
    component.renderMode = UIDocumentRenderMode::TEXTURE;
    component.textureWidth = 256;
    component.textureHeight = 256;
    ASSERT_TRUE(fixture->waitUntilLoaded());

    Framebuffer* target = texture().getTargetTexture();
    ASSERT_NE(target, nullptr);
    Texels texels;
    ASSERT_TRUE(texels.read(*target));

    // the rounded clip (stencil) in the texture context: outside the circle nothing, inside
    // the two halves
    EXPECT_EQ(texels.at(20, 20)[3], 0);
    EXPECT_EQ(texels.at(108, 108)[3], 0);
    EXPECT_TRUE(texels.is(40, 64, 255, 0, 0, 255));
    EXPECT_TRUE(texels.is(90, 64, 0, 0, 255, 255));
    // the blur (layers, downscaled passes): solid in the middle, soft at the edge, nothing far out
    EXPECT_TRUE(texels.is(192, 64, 255, 255, 255, 255, 8));
    EXPECT_GT(texels.at(168, 64)[3], 60);
    EXPECT_LT(texels.at(168, 64)[3], 200);
    EXPECT_EQ(texels.at(146, 18)[3], 0);
    // the opacity group: straight colour at half alpha, the overlap the top box only
    EXPECT_TRUE(texels.is(30, 160, 255, 0, 0, 128));
    EXPECT_TRUE(texels.is(64, 192, 0, 0, 255, 128));
    EXPECT_TRUE(texels.is(100, 230, 0, 0, 255, 128));
    EXPECT_EQ(texels.at(100, 150)[3], 0);
    // the rotated, rounded, clipped panel with outlined text and its magenta drop shadow
    size_t magenta = 0, green = 0, white = 0;
    for (int y = 140; y < 240; y++){
        for (int x = 130; x < 250; x++){
            const uint8_t* p = texels.at(x, y);
            if (p[3] > 250 && p[0] > 200 && p[1] < 60 && p[2] > 200) magenta++;
            if (p[3] > 250 && p[0] < 60 && p[1] > 100 && p[1] < 160 && p[2] < 60) green++;
            if (p[3] > 250 && p[0] > 240 && p[1] > 240 && p[2] > 240) white++;
        }
    }
    EXPECT_GT(magenta, 40u);
    EXPECT_GT(green, 3000u);
    EXPECT_GT(white, 60u);

    // the texture context drew it with layers and filters
    const HtmlUIRenderStats& stats = system().getTextureContext(texture().getEntity())->getRenderer().getStats();
    EXPECT_GE(stats.filtersApplied, 3u);
    EXPECT_GE(stats.layersPushed, 3u);
    EXPECT_GT(stats.passes, 3u);

    CompareResult result = compareWithGolden("p1/texture_effects", texels.rgba, texels.width, texels.height);
    EXPECT_TRUE(result.passed) << result.message;
}

TEST_F(IntegrationTest, RenderStatsOfACombinedScene){
    createScene();
    ASSERT_TRUE(fixture->waitUntilLoaded());
    drawFramesAt(time, 3, 0.0);

    HtmlUIContext* screenContext = system().getContext();
    HtmlUIContext* textureContext = system().getTextureContext(texture().getEntity());
    ASSERT_TRUE(screenContext && textureContext);
    const HtmlUIRenderStats& total = system().getRenderStats();
    const HtmlUIRenderStats& a = screenContext->getRenderer().getStats();
    const HtmlUIRenderStats& b = textureContext->getRenderer().getStats();

    // both contexts drew this frame, and the scene's statistics are their sum
    const uint64_t frame = SystemRender::getFrameCount() - 1;
    EXPECT_TRUE(screenContext->getRenderer().hasFrame(frame));
    EXPECT_TRUE(textureContext->getRenderer().hasFrame(frame));
    EXPECT_EQ(total.passes, a.passes + b.passes);
    EXPECT_EQ(total.drawCalls, a.drawCalls + b.drawCalls);
    EXPECT_EQ(total.pipelineApplies, a.pipelineApplies + b.pipelineApplies);
    EXPECT_EQ(total.textureBinds, a.textureBinds + b.textureBinds);
    EXPECT_EQ(total.layersPushed, a.layersPushed + b.layersPushed);
    EXPECT_EQ(total.filtersApplied, a.filtersApplied + b.filtersApplied);
    EXPECT_EQ(total.geometryLive, a.geometryLive + b.geometryLive);
    EXPECT_EQ(total.geometryBytes, a.geometryBytes + b.geometryBytes);

    // sane values: the screen document's one drop-shadow filter, the texture document
    // without filters is one pass (its field's box: no script made its model, so the bound
    // text is empty); draws for every box, glyph run and image; every pipeline already made
    // (steady state), at most one switch per draw
    EXPECT_EQ(a.filtersApplied, 1u);
    EXPECT_EQ(a.layersPushed, 1u);
    EXPECT_GT(a.passes, 1u);
    EXPECT_EQ(b.passes, 1u);
    EXPECT_EQ(b.filtersApplied, 0u);
    EXPECT_GE(a.drawCalls, 10u);
    EXPECT_GE(b.drawCalls, 1u);
    EXPECT_EQ(total.pipelineCacheMisses, 0u);
    EXPECT_LE(total.pipelineApplies, total.drawCalls + total.passes * 2);
    EXPECT_GT(total.geometryLive, 0u);
    EXPECT_GT(total.geometryBytes, 0u);

    // a hidden texture document draws nothing: only the screen context counts
    texture().hide();
    drawFramesAt(time, 2, 0.0);
    EXPECT_EQ(system().getRenderStats().passes, screenContext->getRenderer().getStats().passes);
    EXPECT_EQ(system().getRenderStats().drawCalls, screenContext->getRenderer().getStats().drawCalls);
}

namespace {

    class TextEffectsInLayersTest : public ::testing::Test {
    protected:
        void SetUp() override{
            TestSystem::get().clearLogs();
            HtmlUIClock::setFixedTime(0.0);
        }
        void TearDown() override{
            HtmlUIClock::reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u);
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u);
        }
    };

    // pixels of a region whose largest channel difference from (r, g, b) is at most tolerance
    size_t countNear(const std::vector<uint8_t>& rgba, int width, PixelRect region, int r, int g, int b, int tolerance){
        size_t count = 0;
        for (int y = region.y0; y < region.y1; y++){
            for (int x = region.x0; x < region.x1; x++){
                const uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
                if (std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance)
                    count++;
            }
        }
        return count;
    }

    // pixels of a region that differ from the background by more than threshold
    size_t countInk(const std::vector<uint8_t>& rgba, int width, PixelRect region, int threshold = 40){
        size_t count = 0;
        for (int y = region.y0; y < region.y1; y++){
            for (int x = region.x0; x < region.x1; x++){
                const uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
                const int d = std::max(std::abs(p[0] - 0x1E), std::max(std::abs(p[1] - 0x1E), std::abs(p[2] - 0x1E)));
                if (d > threshold)
                    count++;
            }
        }
        return count;
    }

}

TEST_F(TextEffectsInLayersTest, FontEffectsInsideLayersAndFilters){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(512, 384, 0, 0, 4);
    ASSERT_TRUE(test.isValid());
    ASSERT_TRUE(test.load("p1/integration/text_effects_layers.html"));
    ASSERT_TRUE(test.renderUntilDrawn());
    std::vector<uint8_t> screen;
    ASSERT_TRUE(test.readScreen(screen));
    const int w = 512;

    // row 1, filter: opacity(.5) on white text with a red shadow: the group fades as one, so
    // no pixel is pure white or pure red, half-white ones are there
    const PixelRect row1{16, 8, 260, 48};
    EXPECT_EQ(countNear(screen, w, row1, 255, 255, 255, 30), 0u);
    EXPECT_EQ(countNear(screen, w, row1, 255, 0, 0, 30), 0u);
    EXPECT_GT(countNear(screen, w, row1, 142, 142, 142, 16), 40u);
    EXPECT_GT(countNear(screen, w, row1, 142, 15, 15, 20), 40u);
    // row 2, blur(1px) on outlined text: the blue outline is there, softened
    const PixelRect row2{16, 56, 260, 96};
    EXPECT_GT(countInk(screen, w, row2), 400u);
    EXPECT_EQ(countNear(screen, w, row2, 0, 128, 255, 4), 0u);
    // row 3, glow text in a backdrop-blurred panel over a gradient: green glow pixels
    size_t glow = 0;
    for (int y = 104; y < 160; y++){
        for (int x = 16; x < 316; x++){
            const uint8_t* p = &screen[(size_t(y) * w + x) * 4];
            if (p[1] > 160 && p[1] > p[0] + 60 && p[1] > p[2] + 60) glow++;
        }
    }
    EXPECT_GT(glow, 30u);
    // row 4, outlined text in a rounded overflow clip: cut at the box's right edge
    EXPECT_GT(countNear(screen, w, {16, 168, 166, 212}, 255, 0, 255, 40), 20u);
    EXPECT_EQ(countInk(screen, w, {170, 168, 400, 212}), 0u);
    // row 5, a horizontal mask: the white text fades to the right
    const size_t left = countNear(screen, w, {16, 224, 116, 264}, 255, 255, 255, 50);
    const size_t right = countNear(screen, w, {170, 224, 270, 264}, 255, 255, 255, 50);
    EXPECT_GT(left, 100u);
    EXPECT_LT(right * 4, left);
    // row 6, rotated text with a red blur effect and a yellow drop shadow (blurred)
    size_t yellow = 0, red = 0;
    for (int y = 250; y < 340; y++){
        for (int x = 230; x < 420; x++){
            const uint8_t* p = &screen[(size_t(y) * w + x) * 4];
            if (p[0] > 150 && p[1] > 150 && p[2] < 100) yellow++;
            if (p[0] > 100 && p[1] < 80 && p[2] < 80) red++;
        }
    }
    EXPECT_GT(yellow, 100u);
    EXPECT_GT(red, 40u);

    CompareResult result = test.compareScreen("p1/text_effects_layers");
    EXPECT_TRUE(result.passed) << result.message;
}
