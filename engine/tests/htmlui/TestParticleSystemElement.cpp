// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// <particle-system> (ui-scaling-text spec 7.5, 14) on S20's Particle System: the prefab made on a
// hidden runtime GameObject of the document's scene, drawn in document order inside the element's
// clip; its whole hierarchy goes with the element (removed, its document unloaded, its src
// changed); play/pause/stop before the prefab exists are queued; no system yet reads as nil in
// Lua; a bad src is HTMLUI-HTML-0029 in the document's diagnostics; texture-mode documents work
// too; time="ui"/"game" and loop= reach the system (it plays at Engine.timeScale 0 with "ui");
// clear, pause, stop and the `complete` of a system that does not loop; an old prefab of the
// legacy two-GameObject layout plays as well.

#include "InspectorTestCommon.h"
#include "TestGoldenCommon.h"

#include "Engine.h"
#include "LuaBinding.h"
#include "object/ParticleSystem.h"
#include "component/ActionComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/Transform.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/compat/DiagnosticCodes.h"
#include "manager/PrefabManager.h"
#include "object/ui/UIDocument.h"
#include "object/ui/UIElement.h"
#include "object/ui/UIElementInternal.h"
#include "object/ui/UIEvent.h"
#include "htmlui/compat/Diagnostics.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
#include "subsystem/ParticleSubsystem.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include "lua.hpp"
#include "LuaBridge.h"

#include <chrono>
#include <functional>
#include <string>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr uint32_t PREFAB_ID = 9100;
    constexpr uint32_t OTHER_PREFAB_ID = 9101;
    constexpr uint32_t STREAM_PREFAB_ID = 9102;
    constexpr uint32_t SHORT_PREFAB_ID = 9103;
    constexpr uint32_t LEGACY_PREFAB_ID = 9104;
    constexpr uint32_t PARENT_PREFAB_ID = 9105;

    // one particle of 20 units at the emitter, alive for a long time, drawn as a square (the old
    // points' look the golden has): an effect that does not loop
    std::string sparkleEffect(const std::string& color){
        return R"({"version": 1, "duration": 1, "looping": false, "maxParticles": 1, "modules": [
            {"type": "main", "startLifetime": 1000, "startSpeed": 0, "startSize": 20, "startColor": )" + color + R"(},
            {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]},
            {"type": "renderer", "untexturedShape": "square"}]})";
    }

    // a root GameObject and its "Particle System" child (S20: the system renders itself)
    bool makeSystem(Scene* scene, Entity root, const std::string& effect){
        scene->addComponent<Transform>(root);
        // (a handle made with ParticleSystem(scene) owns its entity: a bare one, as generated prefabs do)
        const Entity system = scene->createEntity();
        scene->addComponent<Transform>(system);
        scene->addComponent<ParticleSystemComponent>(system);
        ParticleSystem particles(scene, system);
        if (!particles.setInlineEffect(effect))
            return false;
        scene->addEntityChild(root, system, false);
        return true;
    }

    bool makeSparkle(Scene* scene, Entity root, const std::string& color){
        return makeSystem(scene, root, sparkleEffect(color));
    }

    // the legacy two-GameObject layout of an older generated prefab: a Points emitter (the root)
    // and its "Particle System" child with the action, the old setters (the system moves onto
    // the root, S20 D1)
    bool makeLegacySparkle(Scene* scene, Entity root){
        scene->addComponent<Transform>(root);
        scene->addComponent<PointsComponent>(root);
        const Entity system = scene->createEntity();
        scene->addComponent<Transform>(system);
        scene->addComponent<ActionComponent>(system);
        scene->addComponent<ParticleSystemComponent>(system);
        ParticleSystem particles(scene, system);
        particles.setTarget(root);
        particles.setMaxParticles(1);
        particles.setRate(0);
        particles.addBurst(0.0f, 1);
        particles.setLoop(false);
        particles.setLifeInitializer(1000.0f);
        particles.setSizeInitializer(20.0f);
        particles.setColorInitializer(Vector3(0.0f, 1.0f, 0.0f));
        particles.setPositionInitializer(Vector3(0.0f, 0.0f, 0.0f));
        particles.setVelocityInitializer(Vector3(0.0f, 0.0f, 0.0f));
        scene->addEntityChild(root, system, false);
        return true;
    }

    class ParticleSystemElementTest : public InspectorTestBase {
    protected:
        void SetUp() override{
            InspectorTestBase::SetUp();
            PrefabManager::registerPrefab(PREFAB_ID, "fx/sparkle.prefab", [](Scene* scene, Entity root){
                return makeSparkle(scene, root, "[0, 1, 0, 1]");
            });
            PrefabManager::registerPrefab(OTHER_PREFAB_ID, "fx/red.prefab", [](Scene* scene, Entity root){
                return makeSparkle(scene, root, "[1, 0, 0, 1]");
            });
            // a looping stream: 60 particles a second, each alive for 10 seconds
            PrefabManager::registerPrefab(STREAM_PREFAB_ID, "fx/stream.prefab", [](Scene* scene, Entity root){
                return makeSystem(scene, root, R"({"version": 1, "duration": 1, "looping": true, "maxParticles": 1000, "modules": [
                    {"type": "main", "startLifetime": 10, "startSpeed": 0, "startSize": 4, "startColor": [1, 1, 1, 1]},
                    {"type": "emission", "rateOverTime": 60},
                    {"type": "renderer"}]})");
            });
            // one particle that lives half a second: done soon after it plays
            PrefabManager::registerPrefab(SHORT_PREFAB_ID, "fx/short.prefab", [](Scene* scene, Entity root){
                return makeSystem(scene, root, R"({"version": 1, "duration": 0.2, "looping": false, "maxParticles": 1, "modules": [
                    {"type": "main", "startLifetime": 0.5, "startSpeed": 0, "startSize": 8, "startColor": [1, 1, 1, 1]},
                    {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]},
                    {"type": "renderer"}]})");
            });
            PrefabManager::registerPrefab(LEGACY_PREFAB_ID, "fx/legacy.prefab", [](Scene* scene, Entity root){
                return makeLegacySparkle(scene, root);
            });
            // a parent that draws nothing and spawns one sparkle at birth, from the child's own Renderer
            PrefabManager::registerPrefab(PARENT_PREFAB_ID, "fx/parent.prefab", [](Scene* scene, Entity root){
                return makeSystem(scene, root, R"({"version": 1, "duration": 1, "looping": false, "maxParticles": 1,
                    "subEmitters": [{"trigger": "birth", "effect": "fx/ui-child.particles", "emitCount": 1}],
                    "modules": [
                        {"type": "main", "startLifetime": 1000, "startSpeed": 0, "startSize": 20},
                        {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 1}]},
                        {"type": "subEmitters"},
                        {"type": "renderer", "renderMode": "none"}]})");
            });
        }

        void TearDown() override{
            Engine::setTimeScale(1.0f);
            PrefabManager::clearAll();
            lua("doc = nil; collectgarbage()");
            InspectorTestBase::TearDown();
        }

        static std::string lua(const std::string& code){
            lua_State* L = LuaBinding::getLuaState();
            if (luaL_loadstring(L, code.c_str()) != LUA_OK || LuaBinding::pcallWithTraceback(L, 0, 1) != LUA_OK){
                std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
                lua_pop(L, 1);
                return "error: " + error;
            }
            std::string value = luaL_tolstring(L, -1, nullptr);
            lua_pop(L, 2);
            return value;
        }

        static void setDocument(UIDocument& document){
            lua_State* L = LuaBinding::getLuaState();
            if (!luabridge::push(L, document))
                lua_pushnil(L);
            lua_setglobal(L, "doc");
        }

        // the prefab's GameObjects of a <particle-system> (its system, and the root above it)
        static std::vector<Entity> madeEntities(Scene* scene, const UIElement& element){
            std::vector<Entity> entities;
            if (!element.hasParticleSystem())
                return entities;
            const Entity system = element.getParticleSystem().getEntity();
            entities.push_back(system);
            if (Transform* transform = scene->findComponent<Transform>(system))
                entities.push_back(transform->parent);
            return entities;
        }

        static size_t aliveCount(Scene* scene, const std::vector<Entity>& entities){
            size_t alive = 0;
            for (Entity entity : entities){
                if (entity != NULL_ENTITY && scene->isEntityCreated(entity))
                    alive++;
            }
            return alive;
        }

        // frames (wall-clock time: at least `seconds`) until `done` holds; false if it never did
        static bool drawUntil(const std::function<bool()>& done, double seconds = 10.0){
            const auto start = std::chrono::steady_clock::now();
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds){
                GpuEnvironment::drawFrames(1);
                if (done())
                    return true;
            }
            return false;
        }

        // at least `seconds` of frames
        static void drawFor(double seconds){
            const auto start = std::chrono::steady_clock::now();
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds)
                GpuEnvironment::drawFrames(1);
        }
    };

    const char* const panelCss =
        "* { margin: 0; }\n"
        "#panel { position: absolute; left: 100px; top: 100px; width: 200px; height: 120px; background: #000080; overflow: hidden; }\n"
        "particle-system { position: absolute; }\n";

}

// drawn at the element (one particle of 20 units at scale 1), clipped by the panel, with a golden;
// removing the element destroys the prefab's whole hierarchy
TEST_F(ParticleSystemElementTest, DrawsClippedAndRemovalDestroysTheHierarchy){
    page("p", panelCss,
         "<div id=\"panel\">"
         "<particle-system id=\"inside\" src=\"fx/sparkle.prefab\" autoplay style=\"left: 60px; top: 60px\"></particle-system>"
         "<particle-system id=\"edge\" src=\"fx/sparkle.prefab\" autoplay style=\"left: 200px; top: 60px\"></particle-system>"
         "</div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/p.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    GpuEnvironment::drawFrames(4);
    Scene* scene = &fixture.getScene();

    UIElement inside = document.getElementById("inside");
    UIElement edge = document.getElementById("edge");
    ASSERT_TRUE(inside.hasParticleSystem());
    ASSERT_TRUE(edge.hasParticleSystem());
    const std::vector<Entity> made = madeEntities(scene, inside);
    ASSERT_EQ(made.size(), 2u);
    EXPECT_EQ(aliveCount(scene, made), 2u);
    // the prefab draws only through the element: none of its renderers is visible
    for (Entity entity : made)
        EXPECT_FALSE(scene->getComponent<Transform>(entity).visible);

    Screen screen;
    GpuEnvironment::drawFrames(1);
    ASSERT_TRUE(screen.read());
    // inside: 20 x 20 green around (160, 160); the edge one: only its half left of the panel's edge
    EXPECT_NEAR((double)screen.countColor({150, 150, 170, 170}, 0, 255, 0, 40), 400.0, 60.0);
    EXPECT_GT(screen.countColor({290, 150, 300, 170}, 0, 255, 0, 40), 150u);
    EXPECT_EQ(screen.countColor({301, 150, 312, 170}, 0, 255, 0, 40), 0u);
    EXPECT_SCREEN_GOLDEN(screen, "ui_particle_system");

    inside.remove();
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(aliveCount(scene, made), 0u) << "the root and its Particle System child went with the element";
    GpuEnvironment::drawFrames(1);
    ASSERT_TRUE(screen.read());
    EXPECT_EQ(screen.countColor({150, 150, 170, 170}, 0, 255, 0, 40), 0u);

    // an ancestor removed takes the other one's prefab too; attached again, it makes a new one
    const std::vector<Entity> edgeMade = madeEntities(scene, edge);
    ASSERT_EQ(aliveCount(scene, edgeMade), 2u);
    UIElement panel = document.getElementById("panel");
    panel.remove();
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(aliveCount(scene, edgeMade), 0u);
    document.getBody().appendChild(panel);
    GpuEnvironment::drawFrames(2);
    EXPECT_TRUE(edge.hasParticleSystem()) << "made again once attached";
}

// a new src replaces the whole hierarchy; the document's unload destroys it too
TEST_F(ParticleSystemElementTest, SrcChangeAndUnloadDestroyTheHierarchy){
    page("p", panelCss, "<div id=\"panel\"><particle-system id=\"fx\" src=\"fx/sparkle.prefab\" autoplay></particle-system></div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/p.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    GpuEnvironment::drawFrames(2);
    Scene* scene = &fixture.getScene();

    UIElement element = document.getElementById("fx");
    const std::vector<Entity> first = madeEntities(scene, element);
    ASSERT_EQ(aliveCount(scene, first), 2u);

    element.setAttribute("src", "fx/red.prefab");
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(aliveCount(scene, first), 0u) << "the old prefab's root and child are gone";
    const std::vector<Entity> second = madeEntities(scene, element);
    ASSERT_EQ(aliveCount(scene, second), 2u);

    // the document goes: its element, and with it the prefab
    scene->destroyEntity(document.getEntity());
    GpuEnvironment::drawFrames(3);
    EXPECT_EQ(aliveCount(scene, second), 0u);
}

// before the prefab exists (a detached element: no update has made it yet): getParticleSystem()
// is nil in Lua (no crash) and play/pause/stop are queued (the last one wins over autoplay); a bad
// src is HTMLUI-HTML-0029 in the document's diagnostics and stays nil
TEST_F(ParticleSystemElementTest, NoSystemYetIsNilAndCommandsAreQueued){
    page("p", panelCss, "<div id=\"panel\"></div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/p.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    setDocument(document);

    // made by a script: detached, not updated yet
    EXPECT_EQ(lua("queued = doc:createElement('particle-system'); queued:setAttribute('src', 'fx/sparkle.prefab'); "
                  "return queued:getParticleSystem()"), "nil");
    EXPECT_EQ(lua("queued:play(); doc:getElementById('panel'):appendChild(queued); return queued:isPlaying()"), "false");
    UIElement stopped = document.createElement("particle-system");
    stopped.setAttribute("src", "fx/sparkle.prefab");
    stopped.setAttribute("autoplay", "");
    stopped.play();
    stopped.stop();
    EXPECT_FALSE(stopped.hasParticleSystem());
    document.getElementById("panel").appendChild(stopped);
    UIElement bad = document.createElement("particle-system");
    bad.setAttribute("src", "fx/missing.prefab");
    document.getElementById("panel").appendChild(bad);
    GpuEnvironment::drawFrames(3);

    EXPECT_EQ(lua("return queued:getParticleSystem() ~= nil"), "true");
    EXPECT_EQ(lua("return queued:isPlaying()"), "true") << "play() before the prefab existed started it (no autoplay)";
    EXPECT_TRUE(stopped.hasParticleSystem());
    EXPECT_FALSE(stopped.isPlaying()) << "the last queued command (stop) won over autoplay";

    EXPECT_FALSE(bad.hasParticleSystem());
    bool reported = false;
    Rml::Element* badElement = UIElementAccess::get(bad);
    ASSERT_NE(badElement, nullptr);
    for (const htmlui::compat::Diagnostic& diagnostic : htmlui::compat::DocumentLoader::diagnosticsOf(badElement->GetOwnerDocument())){
        if (diagnostic.code == htmlui::compat::codes::ParticleSystemSource)
            reported = true;
    }
    EXPECT_TRUE(reported) << "HTMLUI-HTML-0029 in the document's diagnostics";
    lua("queued = nil");
}

// a texture-mode (world) document makes its prefab in its scene as well
TEST_F(ParticleSystemElementTest, TextureModeDocument){
    page("tex", panelCss, "<div id=\"panel\"><particle-system id=\"fx\" src=\"fx/sparkle.prefab\" autoplay></particle-system></div>");
    SceneFixture fixture;
    UIDocument texture = fixture.addDocument("ui/tex.html");
    UIDocumentComponent& component = texture.getComponent<UIDocumentComponent>();
    component.renderMode = UIDocumentRenderMode::TEXTURE;
    component.textureWidth = 512;
    component.textureHeight = 256;
    component.pixelRatio = 1.0f;
    ASSERT_TRUE(fixture.waitUntilLoaded());
    GpuEnvironment::drawFrames(3);

    UIElement element = texture.getElementById("fx");
    ASSERT_NE(UIElementAccess::get(element), nullptr);
    EXPECT_TRUE(element.hasParticleSystem());
    EXPECT_TRUE(element.isPlaying());
}

// time="ui" (the default) is the system's Unscaled Time: it plays at Engine.timeScale 0 (a pause
// menu); time="game" follows the time scale, the effect's own Unscaled Time turned off too; loop=
// sets the effect's Looping, a later change of the attribute too
TEST_F(ParticleSystemElementTest, TimeAndLoopAttributes){
    page("p", panelCss,
         "<div id=\"panel\">"
         "<particle-system id=\"ui\" src=\"fx/stream.prefab\" autoplay></particle-system>"
         "<particle-system id=\"game\" src=\"fx/stream.prefab\" time=\"game\" autoplay></particle-system>"
         "<particle-system id=\"once\" src=\"fx/stream.prefab\" loop=\"false\" autoplay></particle-system>"
         "<particle-system id=\"again\" src=\"fx/sparkle.prefab\" loop></particle-system>"
         "</div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/p.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    GpuEnvironment::drawFrames(2);

    UIElement ui = document.getElementById("ui");
    UIElement game = document.getElementById("game");
    UIElement once = document.getElementById("once");
    UIElement again = document.getElementById("again");
    ASSERT_TRUE(ui.hasParticleSystem());
    ASSERT_TRUE(game.hasParticleSystem());
    EXPECT_TRUE(ui.getParticleSystem().isUseUnscaledTime());
    EXPECT_FALSE(game.getParticleSystem().isUseUnscaledTime());
    EXPECT_FALSE(game.getParticleSystem().getEffectDocument().useUnscaledTime);
    EXPECT_FALSE(once.getParticleSystem().getEffectDocument().looping) << "loop=\"false\" plays once";
    EXPECT_TRUE(again.getParticleSystem().getEffectDocument().looping) << "a bare loop attribute loops";
    EXPECT_TRUE(ui.getParticleSystem().getEffectDocument().looping) << "the prefab's own setting without the attribute";

    // the game paused: the "ui" system goes on spawning, the "game" one is held
    Engine::setTimeScale(0.0f);
    GpuEnvironment::drawFrames(1);
    const unsigned int uiBefore = ui.getParticleSystem().getParticleCount();
    const unsigned int gameBefore = game.getParticleSystem().getParticleCount();
    EXPECT_TRUE(drawUntil([&](){ return ui.getParticleSystem().getParticleCount() >= uiBefore + 5; }))
        << "time=\"ui\" plays at Engine.timeScale 0 (" << uiBefore << " -> " << ui.getParticleSystem().getParticleCount() << ")";
    EXPECT_EQ(game.getParticleSystem().getParticleCount(), gameBefore) << "time=\"game\" is held at time scale 0";
    Engine::setTimeScale(1.0f);
    EXPECT_TRUE(drawUntil([&](){ return game.getParticleSystem().getParticleCount() > gameBefore; })) << "and goes on at time scale 1";

    // the attributes changed later reach the system too
    ui.setAttribute("time", "game");
    ui.setAttribute("loop", "false");
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(ui.getParticleSystem().isUseUnscaledTime());
    EXPECT_FALSE(ui.getParticleSystem().getEffectDocument().looping);
}

// clear() removes the particles and the system plays on; pause() holds it (not playing); play()
// resumes it; stop() stops a looping one (no `complete`); a system that does not loop sends
// `complete` once its last particle died, and again after it is played once more
TEST_F(ParticleSystemElementTest, ClearPauseStopAndComplete){
    page("p", panelCss,
         "<div id=\"panel\">"
         "<particle-system id=\"stream\" src=\"fx/stream.prefab\" autoplay></particle-system>"
         "<particle-system id=\"short\" src=\"fx/short.prefab\"></particle-system>"
         "</div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/p.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    GpuEnvironment::drawFrames(2);

    UIElement stream = document.getElementById("stream");
    UIElement shortLived = document.getElementById("short");
    ASSERT_TRUE(stream.hasParticleSystem());
    ASSERT_TRUE(shortLived.hasParticleSystem());
    int streamCompletes = 0, shortCompletes = 0;
    stream.addEventListener("complete", [&](UIEvent&){ streamCompletes++; });
    shortLived.addEventListener("complete", [&](UIEvent&){ shortCompletes++; });

    ASSERT_TRUE(drawUntil([&](){ return stream.getParticleSystem().getParticleCount() >= 5; }));
    EXPECT_TRUE(stream.isPlaying());
    stream.clear();
    EXPECT_EQ(stream.getParticleSystem().getParticleCount(), 0u) << "clear() removes the live particles";
    EXPECT_TRUE(stream.isPlaying()) << "and the system plays on";
    EXPECT_TRUE(drawUntil([&](){ return stream.getParticleSystem().getParticleCount() > 0; })) << "spawning again";

    stream.pause();
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(stream.isPlaying()) << "paused is not playing";
    EXPECT_TRUE(stream.getParticleSystem().isPaused());
    const unsigned int held = stream.getParticleSystem().getParticleCount();
    drawFor(0.2);
    EXPECT_EQ(stream.getParticleSystem().getParticleCount(), held) << "a paused system spawns nothing";
    stream.play();
    EXPECT_TRUE(stream.isPlaying()) << "play() resumes it";
    // stop() stops emitting; what is alive dies out, the system playing until then (Unity's)
    stream.stop();
    GpuEnvironment::drawFrames(2);
    EXPECT_FALSE(stream.getParticleSystem().isEmitting());
    const unsigned int stopped = stream.getParticleSystem().getParticleCount();
    EXPECT_GT(stopped, 0u);
    drawFor(0.2);
    EXPECT_EQ(stream.getParticleSystem().getParticleCount(), stopped) << "a stopped system spawns nothing";
    stream.clear();
    EXPECT_TRUE(drawUntil([&](){ return !stream.isPlaying(); })) << "stopped and empty: not playing";
    EXPECT_EQ(streamCompletes, 0) << "a looping system sends no complete";

    // no autoplay: it waits for play(); then it plays until its particle died, and completes once
    EXPECT_FALSE(shortLived.isPlaying());
    shortLived.play();
    EXPECT_TRUE(shortLived.isPlaying());
    EXPECT_TRUE(drawUntil([&](){ return shortCompletes > 0; })) << "complete when the system that does not loop has finished";
    EXPECT_FALSE(shortLived.isPlaying());
    GpuEnvironment::drawFrames(3);
    EXPECT_EQ(shortCompletes, 1) << "once";
    shortLived.play();
    EXPECT_TRUE(drawUntil([&](){ return shortCompletes > 1; })) << "and again after another play";
}

// a prefab of the legacy two-GameObject layout (a Points root, its child's action, the old
// setters): its system lives on the root now, and the element finds and plays it
TEST_F(ParticleSystemElementTest, LegacyLayoutPrefabPlays){
    page("p", panelCss,
         "<div id=\"panel\"><particle-system id=\"fx\" src=\"fx/legacy.prefab\" autoplay style=\"left: 60px; top: 60px\"></particle-system></div>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/p.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    GpuEnvironment::drawFrames(4);

    UIElement element = document.getElementById("fx");
    ASSERT_TRUE(element.hasParticleSystem());
    Scene* scene = &fixture.getScene();
    const Entity system = element.getParticleSystem().getEntity();
    EXPECT_TRUE(scene->findComponent<PointsComponent>(system)) << "on the old emitter, the prefab's root";
    EXPECT_TRUE(element.isPlaying());
    EXPECT_EQ(element.getParticleSystem().getParticleCount(), 1u);
    // drawn: some green at the element (the old setters keep the effect's start speed: it moves)
    Screen screen;
    GpuEnvironment::drawFrames(1);
    ASSERT_TRUE(screen.read());
    EXPECT_GT(screen.countColor({0, 0, 640, 360}, 0, 255, 0, 40), 0u);
}

// BF23: a sub-emitter's particles are drawn through the element too, with the child's Renderer
TEST_F(ParticleSystemElementTest, DrawsTheChildrenOfItsSubEmitters){
    particles::ParticleEffectDocument child;
    std::vector<particles::Diagnostic> diagnostics;
    ASSERT_TRUE(particles::ParticleEffectCodec::read(R"({"version": 1, "duration": 1, "looping": false, "maxParticles": 4, "modules": [
        {"type": "main", "startLifetime": 1000, "startSpeed": 0, "startSize": 20, "startColor": [1, 0, 0, 1]},
        {"type": "emission", "rateOverTime": 0},
        {"type": "shape", "enabled": false},
        {"type": "renderer", "untexturedShape": "square"}]})", child, diagnostics));
    particles::ParticleEffectLibrary::set("fx/ui-child.particles", child);
    page("p", panelCss,
         "<div id=\"panel\">"
         "<particle-system id=\"inside\" src=\"fx/parent.prefab\" autoplay style=\"left: 60px; top: 60px\"></particle-system>"
         "</div>");
    SceneFixture fixture;
    fixture.addDocument("ui/p.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    GpuEnvironment::drawFrames(6);
    Screen screen;
    GpuEnvironment::drawFrames(1);
    ASSERT_TRUE(screen.read());
    // 20 x 20 red around (160, 160), on the panel's blue
    EXPECT_NEAR((double)screen.countColor({150, 150, 170, 170}, 255, 0, 0, 40), 400.0, 60.0);
    particles::ParticleEffectLibrary::clear();
}

TEST_F(ParticleSystemElementTest, AnUnboundSystemWhoseEffectIsGoneDrawsNothing){
    // BF23's re-review N1: the view went (Android, a lost context: the runtime unbound) and the
    // effect's file can no longer be read; the system keeps its last effect, and the element's
    // read of it draws nothing (it read through the unbound runtime's effect)
    particles::ParticleEffectDocument d;
    std::vector<particles::Diagnostic> diagnostics;
    ASSERT_TRUE(particles::ParticleEffectCodec::read(sparkleEffect("[0, 1, 0, 1]"), d, diagnostics));
    particles::ParticleEffectLibrary::set("fx/gone.particles", d);
    Scene scene;
    ParticleSystem system(&scene);
    system.setEffect("fx/gone.particles");
    ParticleSubsystem& subsystem = *scene.getSystem<ParticleSubsystem>();
    for (int i = 0; i < 3; i++) subsystem.update(1.0 / 30.0);
    ASSERT_EQ(system.getParticleCount(), 1u);
    subsystem.destroy();
    particles::ParticleEffectLibrary::clear();
    std::vector<UIParticleQuad> quads;
    EXPECT_TRUE(UIParticleSource::read(&scene, system.getEntity(), quads));
    EXPECT_TRUE(quads.empty());
}
