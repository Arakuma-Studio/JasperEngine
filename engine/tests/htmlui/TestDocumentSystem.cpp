// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem: lazy RmlUi context per scene, loading and unloading with the
// component, visibility, sort order, sizes, scenes and view lifetime; the UIDocument
// GameObject and cloning. Driven by UIDocumentSystem::update only (no rendering).

#include "TestDocumentCommon.h"

#include "Engine.h"
#include "HtmlUIImageCache.h"
#include "HtmlUITexture.h"
#include "RmlRenderer.h"
#include "RmlFontEngine.h"
#include "compat/PerfFixtures.h"
#include "object/GameObject.h"
#include "registry/EntityCloner.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Property.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    float zIndexOf(Rml::ElementDocument* document){
        const Rml::Property* property = document->GetProperty(Rml::PropertyId::ZIndex);
        return property ? property->Get<float>() : -1.0f;
    }

    // the id of the element hit at a document point
    std::string idAt(Scene& scene, float x, float y){
        Rml::Element* element = rmlContext(scene)->GetElementAtPoint(Rml::Vector2f(x, y));
        return element ? element->GetId() : std::string();
    }

    class UIDocumentSystemTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            ASSERT_EQ(HtmlUI::getReferenceCount(), 0) << "an earlier test left RmlUi initialised";
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            // every test's scenes are gone: RmlUi is released with the last context
            EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
            EXPECT_EQ(HtmlUI::getContextCount(), 0u);
        }

        size_t errors(const std::string& text = ""){
            return TestSystem::get().countLogs(D_LOG_ERROR, text);
        }

        // the debugger loads its own font: it cannot initialise without a font engine that
        // has fonts
        bool fontEngineHasFonts(){
            return HtmlUI::getFontEngine().GetFontFaceHandle("sans-serif", Rml::Style::FontStyle::Normal,
                                                             Rml::Style::FontWeight::Normal, 16) != 0;
        }
    };

    using UIDocumentTest = UIDocumentSystemTest;
    using UIDocumentClonerTest = UIDocumentSystemTest;

}

TEST_F(UIDocumentSystemTest, LoadsOnUpdate){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument document = fixture.addDocument("ecs/basic.rml");
    EXPECT_TRUE(documentSystem(scene).hasDocuments());
    EXPECT_FALSE(document.isLoaded());
    EXPECT_FALSE(documentSystem(scene).isAllLoaded());

    updateDocuments(scene);

    EXPECT_TRUE(document.isLoaded());
    EXPECT_TRUE(documentSystem(scene).isDocumentLoaded(document.getEntity()));
    ASSERT_NE(documentSystem(scene).getContext(), nullptr);
    EXPECT_EQ(HtmlUI::getReferenceCount(), 1);
    EXPECT_EQ(HtmlUI::getContextCount(), 1u);
    EXPECT_EQ(documentCount(scene), 1);

    Rml::ElementDocument* rml = findDocument(scene, "ecs basic");
    ASSERT_NE(rml, nullptr);
    EXPECT_TRUE(rml->IsVisible());

    // laid out at the canvas size
    const Rml::Vector2i dimensions = documentSystem(scene).getContext()->getDimensions();
    EXPECT_EQ(dimensions.x, Engine::getCanvasWidth());
    EXPECT_EQ(dimensions.y, Engine::getCanvasHeight());

    UIElement box = document.getElementById("box");
    ASSERT_TRUE(box.isValid());
    EXPECT_EQ(box.getTagName(), "div");

    // a second update keeps the same document
    updateDocuments(scene);
    EXPECT_EQ(findDocument(scene, "ecs basic"), rml);
    EXPECT_TRUE(box.isValid());

    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentSystemTest, MissingFileFailsWithError){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument document = fixture.addDocument("ecs/missing.html");
    updateDocuments(scene);

    UIDocumentComponent& component = document.getComponent<UIDocumentComponent>();
    EXPECT_FALSE(component.loaded);
    EXPECT_TRUE(component.loadFailed);
    EXPECT_FALSE(component.needReload);
    EXPECT_EQ(errors("could not load document 'ecs/missing.html'"), 1u);
    EXPECT_EQ(documentCount(scene), 0);
    // a failed document counts as loaded: there is nothing to wait for
    EXPECT_TRUE(documentSystem(scene).isAllLoaded());

    // one error per attempt, not one per frame
    updateDocuments(scene);
    updateDocuments(scene);
    EXPECT_EQ(errors("could not load document 'ecs/missing.html'"), 1u);

    // a fixed filename loads
    document.setFilename("ecs/basic.rml");
    updateDocuments(scene);
    EXPECT_TRUE(component.loaded);
    EXPECT_FALSE(component.loadFailed);
}

TEST_F(UIDocumentSystemTest, FilenameChangeReloads){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument document = fixture.addDocument("ecs/basic.rml");
    updateDocuments(scene);
    UIElement box = document.getElementById("box");
    ASSERT_TRUE(box.isValid());

    document.setFilename("ecs/second.rml");
    EXPECT_TRUE(document.getComponent<UIDocumentComponent>().needReload);
    updateDocuments(scene);

    EXPECT_TRUE(document.isLoaded());
    EXPECT_EQ(documentCount(scene), 1);
    EXPECT_NE(findDocument(scene, "ecs second"), nullptr);
    EXPECT_EQ(findDocument(scene, "ecs basic"), nullptr);
    EXPECT_FALSE(box.isValid());
    EXPECT_TRUE(document.getElementById("second").isValid());
    EXPECT_FALSE(document.getElementById("box").isValid());

    // the same filename again does not reload
    Rml::ElementDocument* second = findDocument(scene, "ecs second");
    document.setFilename("ecs/second.rml");
    updateDocuments(scene);
    EXPECT_EQ(findDocument(scene, "ecs second"), second);

    // a component edit through the update flag (the editor's path) reloads
    document.getComponent<UIDocumentComponent>().filename = "ecs/basic.rml";
    document.reload();
    updateDocuments(scene);
    EXPECT_NE(findDocument(scene, "ecs basic"), nullptr);
    EXPECT_EQ(documentCount(scene), 1);

    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentSystemTest, EmptyFilenameUnloads){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument document = fixture.addDocument("ecs/basic.rml");
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());

    document.setFilename("");
    updateDocuments(scene);

    EXPECT_FALSE(document.isLoaded());
    EXPECT_FALSE(document.getComponent<UIDocumentComponent>().loadFailed);
    EXPECT_FALSE(documentSystem(scene).isDocumentLoaded(document.getEntity()));
    EXPECT_EQ(documentCount(scene), 0);
    EXPECT_FALSE(document.getBody().isValid());
    EXPECT_TRUE(documentSystem(scene).isAllLoaded());
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentSystemTest, RemoveComponentUnloads){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument document = fixture.addDocument("ecs/basic.rml");
    updateDocuments(scene);
    UIElement body = document.getBody();
    ASSERT_TRUE(body.isValid());

    scene.removeComponent<UIDocumentComponent>(document.getEntity());

    EXPECT_FALSE(documentSystem(scene).isDocumentLoaded(document.getEntity()));
    EXPECT_EQ(documentCount(scene), 0);
    EXPECT_FALSE(body.isValid());

    updateDocuments(scene);
    EXPECT_EQ(documentCount(scene), 0);
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentSystemTest, DestroyEntityUnloads){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    Entity entity;
    UIElement box;
    {
        // owns its entity: destroyed with it
        UIDocument document(&scene);
        document.setFilename("ecs/basic.rml");
        updateDocuments(scene);
        entity = document.getEntity();
        box = document.getElementById("box");
        ASSERT_TRUE(box.isValid());
        ASSERT_EQ(documentCount(scene), 1);
    }

    EXPECT_FALSE(documentSystem(scene).isDocumentLoaded(entity));
    EXPECT_EQ(documentCount(scene), 0);
    EXPECT_FALSE(box.isValid());

    // the element goes with the next update; the handle stays safe to use
    updateDocuments(scene);
    EXPECT_FALSE(box.isValid());
    EXPECT_EQ(box.getId(), "");
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentSystemTest, DisabledHides){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument document = fixture.addDocument("ecs/basic.rml");
    updateDocuments(scene);
    Rml::ElementDocument* rml = findDocument(scene, "ecs basic");
    ASSERT_NE(rml, nullptr);
    EXPECT_TRUE(rml->IsVisible());

    document.hide();
    EXPECT_FALSE(document.isVisible());
    updateDocuments(scene);
    EXPECT_FALSE(rml->IsVisible());
    // still loaded: showing it again keeps the DOM
    EXPECT_TRUE(document.isLoaded());

    document.show();
    updateDocuments(scene);
    EXPECT_TRUE(rml->IsVisible());
    EXPECT_EQ(findDocument(scene, "ecs basic"), rml);

    // the component's checkbox, as the editor writes it
    document.getComponent<UIDocumentComponent>().enabled = false;
    updateDocuments(scene);
    EXPECT_FALSE(rml->IsVisible());

    // A document loaded while hidden stays hidden; setVisible is the same public state.
    document.setVisible(true);
    updateDocuments(scene);
    EXPECT_TRUE(rml->IsVisible());
    document.setVisible(false);
    ASSERT_TRUE(document.load("ecs/second.rml"));
    Rml::ElementDocument* second = findDocument(scene, "ecs second");
    ASSERT_NE(second, nullptr);
    EXPECT_FALSE(second->IsVisible());

    // a disabled document is not waited for
    EXPECT_TRUE(documentSystem(scene).isAllLoaded());
}

TEST_F(UIDocumentSystemTest, InactiveInHierarchyHides){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    GameObject& parent = fixture.add<GameObject>();
    UIDocument document = fixture.addDocument("ecs/basic.rml");
    parent.addChild(document.getEntity());
    updateDocuments(scene);
    Rml::ElementDocument* rml = findDocument(scene, "ecs basic");
    ASSERT_NE(rml, nullptr);
    EXPECT_TRUE(rml->IsVisible());

    parent.setActive(false);
    updateDocuments(scene);
    EXPECT_FALSE(rml->IsVisible());
    EXPECT_TRUE(document.isVisible());   // the component is still enabled

    parent.setActive(true);
    updateDocuments(scene);
    EXPECT_TRUE(rml->IsVisible());
}

TEST_F(UIDocumentSystemTest, SortOrderZIndex){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument a = fixture.addDocument("ecs/overlap_a.rml", 0);
    UIDocument b = fixture.addDocument("ecs/overlap_b.rml", 1);
    updateDocuments(scene);

    Rml::ElementDocument* rmlA = findDocument(scene, "ecs overlap a");
    Rml::ElementDocument* rmlB = findDocument(scene, "ecs overlap b");
    ASSERT_NE(rmlA, nullptr);
    ASSERT_NE(rmlB, nullptr);
    EXPECT_FLOAT_EQ(zIndexOf(rmlA), 0.0f);
    EXPECT_FLOAT_EQ(zIndexOf(rmlB), 1.0f);
    EXPECT_EQ(idAt(scene, 150, 150), "b-box");

    // swapped
    a.setSortOrder(2);
    updateDocuments(scene);
    EXPECT_FLOAT_EQ(zIndexOf(rmlA), 2.0f);
    EXPECT_EQ(idAt(scene, 150, 150), "a-box");

    // negative orders are fine
    a.setSortOrder(-5);
    updateDocuments(scene);
    EXPECT_FLOAT_EQ(zIndexOf(rmlA), -5.0f);
    EXPECT_EQ(idAt(scene, 150, 150), "b-box");
}

TEST_F(UIDocumentSystemTest, FocusDoesNotReorder){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    fixture.addDocument("ecs/overlap_a.rml", 0);
    fixture.addDocument("ecs/overlap_b.rml", 1);
    updateDocuments(scene);
    ASSERT_EQ(idAt(scene, 150, 150), "b-box");

    // RmlUi pulls a focused document with z-index auto to the front; ours have numbers
    Rml::ElementDocument* rmlA = findDocument(scene, "ecs overlap a");
    ASSERT_NE(rmlA, nullptr);
    rmlA->Focus();
    updateDocuments(scene);
    EXPECT_EQ(rmlContext(scene)->GetFocusElement()->GetOwnerDocument(), rmlA);
    EXPECT_EQ(idAt(scene, 150, 150), "b-box");
}

TEST_F(UIDocumentSystemTest, CanvasResizeUpdatesDimensions){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument document = fixture.addDocument("ecs/basic.rml");
    updateDocuments(scene);
    EXPECT_EQ(documentSystem(scene).getContext()->getDimensions(), Rml::Vector2i(640, 360));

    GpuEnvironment::setView(800, 600, 800, 600, Scaling::FITWIDTH);
    updateDocuments(scene);
    EXPECT_EQ(documentSystem(scene).getContext()->getDimensions(), Rml::Vector2i(800, 600));
    EXPECT_EQ(rmlContext(scene)->GetDimensions(), Rml::Vector2i(800, 600));
    // body is 100% of the context
    EXPECT_FLOAT_EQ(document.getBody().getBoundingBox().getWidth(), 800.0f);

    GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);
    updateDocuments(scene);
    EXPECT_EQ(documentSystem(scene).getContext()->getDimensions(), Rml::Vector2i(640, 360));
}

TEST_F(UIDocumentSystemTest, ReferenceSizeOverrides){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument document = fixture.addDocument("ecs/basic.rml");
    // set before the context exists: it is created at that size
    documentSystem(scene).setReferenceSize(1280, 720);
    updateDocuments(scene);
    EXPECT_EQ(documentSystem(scene).getContext()->getDimensions(), Rml::Vector2i(1280, 720));

    // the box is laid out at document px, reported in canvas px (half: 640x360 canvas)
    const Rect box = document.getElementById("box").getBoundingBox();
    EXPECT_FLOAT_EQ(box.getX(), 20.0f);
    EXPECT_FLOAT_EQ(box.getY(), 15.0f);
    EXPECT_FLOAT_EQ(box.getWidth(), 110.0f);
    EXPECT_FLOAT_EQ(box.getHeight(), 60.0f);

    documentSystem(scene).clearReferenceSize();
    updateDocuments(scene);
    EXPECT_EQ(documentSystem(scene).getContext()->getDimensions(), Rml::Vector2i(640, 360));
}

TEST_F(UIDocumentSystemTest, TwoScenesTwoContexts){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument("ecs/basic.rml");

    // a second scene (not a layer: only its system is updated)
    std::unique_ptr<Scene> other(new Scene());
    std::unique_ptr<UIDocument> otherDocument(new UIDocument(other.get()));
    otherDocument->setFilename("ecs/second.rml");

    updateDocuments(scene);
    updateDocuments(*other);

    ASSERT_NE(documentSystem(scene).getContext(), nullptr);
    ASSERT_NE(documentSystem(*other).getContext(), nullptr);
    EXPECT_NE(documentSystem(scene).getContext(), documentSystem(*other).getContext());
    EXPECT_EQ(HtmlUI::getContextCount(), 2u);
    EXPECT_EQ(HtmlUI::getReferenceCount(), 2);

    // each context holds its scene's documents only
    EXPECT_EQ(documentCount(scene), 1);
    EXPECT_EQ(documentCount(*other), 1);
    EXPECT_TRUE(document.getElementById("box").isValid());
    EXPECT_FALSE(document.getElementById("second").isValid());
    EXPECT_TRUE(otherDocument->getElementById("second").isValid());

    otherDocument.reset();
    other.reset();
    EXPECT_EQ(HtmlUI::getContextCount(), 1u);
    EXPECT_EQ(HtmlUI::getReferenceCount(), 1);
    EXPECT_TRUE(document.getElementById("box").isValid());
}

TEST_F(UIDocumentSystemTest, SceneDestroyReleasesHtmlUI){
    std::unique_ptr<Scene> scene(new Scene());
    std::unique_ptr<UIDocument> document(new UIDocument(scene.get()));
    document->setFilename("ecs/basic.rml");
    updateDocuments(*scene);
    ASSERT_TRUE(document->isLoaded());
    EXPECT_EQ(HtmlUI::getReferenceCount(), 1);
    UIElement box = document->getElementById("box");
    ASSERT_TRUE(box.isValid());

    // the GameObject handle outlives nothing: entities go with the scene
    document->setEntityOwned(false);
    scene.reset();

    EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
    EXPECT_EQ(HtmlUI::getContextCount(), 0u);
    EXPECT_FALSE(HtmlUI::isInitialized());
    // an element of a released RmlUi is only invalid
    EXPECT_FALSE(box.isValid());
    EXPECT_EQ(box.getTagName(), "");
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentSystemTest, ViewDestroyedAndReloaded){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument document = fixture.addDocument("ecs/basic.rml");
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());

    HtmlUIContext* context = documentSystem(scene).getContext();
    UIElement box = document.getElementById("box");
    ASSERT_TRUE(box.isValid());

    // the platform drops the view (Android without its context, Web context loss): the scenes
    // release their GPU objects only (S95 REQ-15; the context and its documents went before)
    Engine::systemViewDestroyed();
    EXPECT_EQ(documentSystem(scene).getContext(), context);
    EXPECT_TRUE(HtmlUI::isInitialized());
    EXPECT_TRUE(document.isLoaded());
    EXPECT_FALSE(document.getComponent<UIDocumentComponent>().needReload);
    EXPECT_TRUE(box.isValid());

    // nothing happens without a view
    updateDocuments(scene);
    EXPECT_EQ(documentSystem(scene).getContext(), context);

    Engine::systemViewLoaded();
    Engine::systemViewChanged();
    updateDocuments(scene);
    EXPECT_TRUE(document.isLoaded());
    EXPECT_EQ(documentSystem(scene).getContext(), context);
    EXPECT_TRUE(box.isValid());
    EXPECT_EQ(documentSystem(scene).getLoadCount(document.getEntity()), 1u);
    EXPECT_EQ(HtmlUI::getReferenceCount(), 1);
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentSystemTest, DebuggerToggle){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    fixture.addDocument("ecs/basic.rml");
    updateDocuments(scene);
    ASSERT_TRUE(fontEngineHasFonts());

    // asked before the context exists: applied when it is created
    std::unique_ptr<Scene> other(new Scene());
    documentSystem(*other).setDebuggerVisible(true);
    EXPECT_EQ(HtmlUI::getDebuggerContext(), nullptr);
    std::unique_ptr<UIDocument> otherDocument(new UIDocument(other.get()));
    otherDocument->setEntityOwned(false);
    otherDocument->setFilename("ecs/second.rml");
    updateDocuments(*other);
    ASSERT_NE(documentSystem(*other).getContext(), nullptr);
    EXPECT_EQ(HtmlUI::getDebuggerContext(), documentSystem(*other).getContext());

    // moved to the first scene
    documentSystem(scene).setDebuggerVisible(true);
    HtmlUIContext* context = documentSystem(scene).getContext();
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(HtmlUI::getDebuggerContext(), context);

    documentSystem(scene).setDebuggerVisible(false);
    updateDocuments(scene);
    documentSystem(scene).setDebuggerVisible(true);
    updateDocuments(scene);
    EXPECT_EQ(HtmlUI::getDebuggerContext(), context);

    // the document is not counted as the debugger's
    EXPECT_TRUE(documentSystem(scene).isDocumentLoaded(fixture.getScene().getComponentArray<UIDocumentComponent>()->getEntity(0)));

    // the other scene going away leaves the debugger where it is
    other.reset();
    EXPECT_EQ(HtmlUI::getDebuggerContext(), context);

    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentSystemTest, DebuggerShutsDownWithTheScene){
    {
        SceneFixture fixture;
        Scene& scene = fixture.getScene();
        fixture.addDocument("ecs/basic.rml");
        updateDocuments(scene);
        ASSERT_TRUE(fontEngineHasFonts());
        documentSystem(scene).setDebuggerVisible(true);
        EXPECT_NE(HtmlUI::getDebuggerContext(), nullptr);
    }
    EXPECT_EQ(HtmlUI::getDebuggerContext(), nullptr);
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentTest, LoadNowAndGetElementById){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument& document = fixture.add<UIDocument>();
    EXPECT_EQ(document.getFilename(), "");
    EXPECT_FALSE(document.load(""));
    EXPECT_FALSE(document.isLoaded());
    EXPECT_EQ(errors(), 0u);

    // synchronous: elements and layout right away, no update needed
    EXPECT_TRUE(document.load("ecs/basic.rml"));
    EXPECT_EQ(document.getFilename(), "ecs/basic.rml");
    EXPECT_TRUE(document.isLoaded());

    UIElement box = document.getElementById("box");
    ASSERT_TRUE(box.isValid());
    EXPECT_EQ(box.getId(), "box");
    EXPECT_FLOAT_EQ(box.getBoundingBox().getWidth(), 220.0f);
    EXPECT_FALSE(document.getElementById("nothing").isValid());

    UIElement body = document.getBody();
    ASSERT_TRUE(body.isValid());
    EXPECT_EQ(body.getTagName(), "body");

    // shown at once too
    Rml::ElementDocument* rml = findDocument(scene, "ecs basic");
    ASSERT_NE(rml, nullptr);
    EXPECT_TRUE(rml->IsVisible());

    // a missing file: false, the old document is gone
    EXPECT_FALSE(document.load("ecs/missing.html"));
    EXPECT_FALSE(document.isLoaded());
    EXPECT_FALSE(box.isValid());
    EXPECT_EQ(errors("could not load document 'ecs/missing.html'"), 1u);

    // and loads again
    EXPECT_TRUE(document.load("ecs/second.rml"));
    EXPECT_TRUE(document.getElementById("second").isValid());
}

TEST_F(UIDocumentTest, ReloadKeepsSortOrder){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument document = fixture.addDocument("ecs/basic.rml", 3);
    EXPECT_EQ(document.getSortOrder(), 3);
    updateDocuments(scene);
    Rml::ElementDocument* rml = findDocument(scene, "ecs basic");
    ASSERT_NE(rml, nullptr);
    EXPECT_FLOAT_EQ(zIndexOf(rml), 3.0f);

    UIElement box = document.getElementById("box");
    box.setClass("changed", true);

    document.reload();
    updateDocuments(scene);
    Rml::ElementDocument* reloaded = findDocument(scene, "ecs basic");
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(documentCount(scene), 1);
    EXPECT_FLOAT_EQ(zIndexOf(reloaded), 3.0f);
    EXPECT_EQ(document.getSortOrder(), 3);

    // a new DOM: the old element is gone, the new one has no runtime changes
    EXPECT_FALSE(box.isValid());
    UIElement newBox = document.getElementById("box");
    ASSERT_TRUE(newBox.isValid());
    EXPECT_FALSE(newBox.hasClass("changed"));
}

TEST_F(UIDocumentClonerTest, InstantiateCopiesFields){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument document = fixture.addDocument("ecs/overlap_a.rml", 4);
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());

    std::vector<Entity> copies = EntityCloner::clone(&scene, EntityCloner::collectHierarchy(&scene, document.getEntity()), &scene);
    ASSERT_EQ(copies.size(), 1u);

    UIDocumentComponent& copy = scene.getComponent<UIDocumentComponent>(copies[0]);
    EXPECT_EQ(copy.filename, "ecs/overlap_a.rml");
    EXPECT_EQ(copy.sortOrder, 4);
    EXPECT_TRUE(copy.enabled);
    // its own document, loaded by the next update
    EXPECT_TRUE(copy.needReload);
    EXPECT_FALSE(copy.loaded);
    EXPECT_FALSE(copy.loadFailed);

    updateDocuments(scene);
    EXPECT_TRUE(copy.loaded);
    EXPECT_EQ(documentCount(scene), 2);
    UIDocument copyObject(&scene, copies[0]);
    EXPECT_TRUE(copyObject.getElementById("a-box").isValid());
    EXPECT_TRUE(document.getElementById("a-box").isValid());

    // hidden in the source: hidden in the copy
    document.hide();
    std::vector<Entity> hidden = EntityCloner::clone(&scene, {document.getEntity()}, &scene);
    ASSERT_EQ(hidden.size(), 1u);
    EXPECT_FALSE(scene.getComponent<UIDocumentComponent>(hidden[0]).enabled);

    scene.destroyEntity(copies[0]);
    scene.destroyEntity(hidden[0]);
    updateDocuments(scene);
    EXPECT_EQ(documentCount(scene), 1);
    EXPECT_EQ(errors(), 0u);
}

// S93 R13, R14 (AC17, AC18): the glyph cache outlives HTML UI contexts, RmlUi itself (shut down with the last context) and
// a view loss. The arcade shell (fixtures/arcade-shell: outlined and glowing bold text up to 324 px, a 2048x2048 atlas)
// shown a second time rasterizes no glyph and draws no effect the slow way.
namespace {

    struct ShellFrames {
        uint32_t glyphsRasterized = 0;
        uint32_t textEffectFallbacks = 0;
        double firstFrameTextMs = 0.0;   // GenerateString in the first frame that drew text
    };

    // frames one at a time until the shell is loaded and drawn, then 2 more, adding up each frame's text statistics
    ShellFrames drawShell(SceneFixture& fixture, UIDocument& document){
        ShellFrames frames;
        RmlFontEngine& fonts = HtmlUI::getFontEngine();
        Scene* scene = &fixture.getScene();
        int after = -1;
        for (int frame = 0; frame < 300 && after != 0; frame++){
            const double before = fonts.getGenerateStringMs();
            GpuEnvironment::drawFrames(1);
            const HtmlUIRenderStats stats = document.getRenderStats();
            frames.glyphsRasterized += stats.glyphsRasterized;
            frames.textEffectFallbacks += stats.textEffectFallbacks;
            if (frames.firstFrameTextMs == 0.0)
                frames.firstFrameTextMs = fonts.getGenerateStringMs() - before;
            const bool loaded = document.isLoaded() && scene->getSystem<UIDocumentSystem>()->isAllLoaded() && stats.textRegenerations == 0 &&
                                fonts.getGenerateStringMs() > 0.0;
            if (after > 0)
                after--;
            else if (after < 0 && loaded)
                after = 2;
        }
        return frames;
    }

}

TEST_F(UIDocumentSystemTest, GlyphCacheSurvivesContext){
    RmlFontEngine& fonts = HtmlUI::getFontEngine();
    {
        SceneFixture fixture;
        UIDocument document = fixture.addDocument("arcade-shell/shell.html");
        const ShellFrames cold = drawShell(fixture, document);
        // the first time: every glyph is made, effects the CPU way while the distance glyphs are pending
        EXPECT_GT(cold.glyphsRasterized, 0u);
        fonts.finishDistanceGlyphsForTesting();
    }
    // the scene and its context are gone, and RmlUi with them (the last context)
    ASSERT_EQ(HtmlUI::getReferenceCount(), 0);
    EXPECT_GT(fonts.getPageCount(), 0u);

    SceneFixture fixture;
    UIDocument document = fixture.addDocument("arcade-shell/shell.html");
    const ShellFrames warm = drawShell(fixture, document);
    EXPECT_EQ(warm.glyphsRasterized, 0u);
    EXPECT_EQ(warm.textEffectFallbacks, 0u);
    // R41: the text of the shell's first frame in 2 ms (enforced by the benchmark run, JASPER_HTMLUI_PERF_SLACK)
    const double slack = test::compat::benchmarkPerfSlack();
    std::printf("[bench] DocumentSystem.GlyphCacheSurvivesContext: first frame's text %.3f ms (budget 2 ms%s)\n", warm.firstFrameTextMs,
                slack > 0.0 ? "" : ", not enforced: JASPER_HTMLUI_PERF_SLACK unset");
    if (slack > 0.0){
        EXPECT_LE(warm.firstFrameTextMs, 2.0 * slack);
    }
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentSystemTest, GlyphCacheSurvivesViewLoss){
    RmlFontEngine& fonts = HtmlUI::getFontEngine();
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument("arcade-shell/shell.html");
    EXPECT_GT(drawShell(fixture, document).glyphsRasterized, 0u);
    fonts.finishDistanceGlyphsForTesting();

    // the platform drops the view (Android background, a lost GL context): since S95 WP-4 the documents keep their
    // DOM and RmlUi stays, only GPU objects go (the atlas pages' textures among them)
    Engine::systemViewDestroyed();
    EXPECT_TRUE(HtmlUI::isInitialized());
    EXPECT_GT(fonts.getPageCount(), 0u);
    Engine::systemViewLoaded();
    Engine::systemViewChanged();

    const ShellFrames back = drawShell(fixture, document);
    EXPECT_EQ(back.glyphsRasterized, 0u);
    EXPECT_EQ(back.textEffectFallbacks, 0u);
    // the pages are textures of the new context's renderer again, each uploaded
    HtmlUIContext* context = documentSystem(scene).getContext();
    ASSERT_NE(context, nullptr);
    const std::vector<const HtmlUITexture*> pages = context->getRenderer().getAtlasTextures();
    EXPECT_FALSE(pages.empty());
    for (const HtmlUITexture* page : pages){
        ASSERT_TRUE(page->atlasGeneration);
        EXPECT_EQ(page->uploadedGeneration, *page->atlasGeneration);
    }
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentSystemTest, ImageCacheLetsGoOfTexturesOnViewLossWithoutDocuments){
    // S93 R19: the shared image cache keeps released images for the next documents, beyond the last context and RmlUi
    // itself. A view lost while no document is up (a scene without UI, the Android background) takes their textures: the
    // cache lets go of them then, so a document loaded before the view comes back decodes and uploads its images again
    // instead of reusing dead ones (they drew nothing: IntegrationTest.ScriptsUseDocumentsBeforeTheViewLoads after
    // another test had shown the same image)
    HtmlUIImageCache& images = HtmlUIImageCache::get();
    {
        SceneFixture fixture;
        UIDocument document = fixture.addDocument("arcade-shell/shell.html");
        drawShell(fixture, document);
    }
    ASSERT_EQ(HtmlUI::getReferenceCount(), 0);
    std::vector<uint32_t> handles;
    images.getGpuHandles(handles);
    ASSERT_FALSE(handles.empty()) << "the atlas stays cached after its document";

    Engine::systemViewDestroyed();
    handles.clear();
    images.getGpuHandles(handles);
    EXPECT_TRUE(handles.empty()) << "textures of the lost view are kept";
    Engine::systemViewLoaded();
    Engine::systemViewChanged();

    const uint64_t decodes = images.getCounters().decodes;
    const uint64_t uploads = images.getCounters().uploads;
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("arcade-shell/shell.html");
    drawShell(fixture, document);
    EXPECT_GT(images.getCounters().decodes, decodes);
    EXPECT_GT(images.getCounters().uploads, uploads);
    EXPECT_EQ(errors(), 0u);
}
