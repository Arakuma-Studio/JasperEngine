// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// <particle-system> (ui-scaling-text spec 7.5, 14): the prefab made on a hidden runtime
// GameObject of the document's scene, drawn in document order inside the element's clip; its
// whole hierarchy goes with the element (removed, its document unloaded, its src changed);
// play/pause/stop before the prefab exists are queued; no system yet reads as nil in Lua; a bad
// src is HTMLUI-HTML-0029 in the document's diagnostics; texture-mode documents work too.

#include "InspectorTestCommon.h"
#include "TestGoldenCommon.h"

#include "LuaBinding.h"
#include "action/ParticleSystem.h"
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
#include "htmlui/compat/Diagnostics.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include "lua.hpp"
#include "LuaBridge.h"

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    constexpr uint32_t PREFAB_ID = 9100;
    constexpr uint32_t OTHER_PREFAB_ID = 9101;

    // the legacy two-GameObject layout: a Points emitter (the root) and its "Particle System"
    // child; one green particle of 20 units at the emitter, alive for a long time
    bool makeSparkle(Scene* scene, Entity root, Vector3 color){
        scene->addComponent<Transform>(root);
        scene->addComponent<PointsComponent>(root);
        // (a handle made with ParticleSystem(scene) owns its entity: a bare one, as generated prefabs do)
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
        particles.setColorInitializer(color);
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
                return makeSparkle(scene, root, Vector3(0.0f, 1.0f, 0.0f));
            });
            PrefabManager::registerPrefab(OTHER_PREFAB_ID, "fx/red.prefab", [](Scene* scene, Entity root){
                return makeSparkle(scene, root, Vector3(1.0f, 0.0f, 0.0f));
            });
        }

        void TearDown() override{
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
