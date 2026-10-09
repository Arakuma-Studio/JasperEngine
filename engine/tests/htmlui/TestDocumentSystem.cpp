// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem: lazy RmlUi context per scene, loading and unloading with the
// component, visibility, sort order, sizes, scenes and view lifetime; the UIDocument
// GameObject and cloning. Driven by UIDocumentSystem::update only (no rendering).

#include "TestDocumentCommon.h"

#include "Engine.h"
#include "RmlFontEngine.h"
#include "object/GameObject.h"
#include "registry/EntityCloner.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Property.h>

#include <gtest/gtest.h>

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

TEST_F(UIDocumentSystemTest, LazyNoContextWithoutDocuments){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    updateDocuments(scene);
    EXPECT_EQ(documentSystem(scene).getContext(), nullptr);
    EXPECT_FALSE(HtmlUI::isInitialized());

    // a component without a file is not a document yet
    UIDocument document = fixture.addDocument("");
    updateDocuments(scene);
    EXPECT_FALSE(documentSystem(scene).hasDocuments());
    EXPECT_EQ(documentSystem(scene).getContext(), nullptr);
    EXPECT_FALSE(HtmlUI::isInitialized());
    EXPECT_TRUE(documentSystem(scene).isAllLoaded());

    EXPECT_EQ(errors(), 0u);
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

    // the platform drops the view (Android pause, Web context loss): scenes destroy their systems
    Engine::systemViewDestroyed();
    EXPECT_EQ(documentSystem(scene).getContext(), nullptr);
    EXPECT_FALSE(HtmlUI::isInitialized());
    EXPECT_FALSE(document.isLoaded());
    EXPECT_TRUE(document.getComponent<UIDocumentComponent>().needReload);

    // nothing happens without a view
    updateDocuments(scene);
    EXPECT_EQ(documentSystem(scene).getContext(), nullptr);

    Engine::systemViewLoaded();
    Engine::systemViewChanged();
    updateDocuments(scene);
    EXPECT_TRUE(document.isLoaded());
    EXPECT_TRUE(document.getElementById("box").isValid());
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
    EXPECT_FALSE(document.isLoaded());

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

TEST_F(UIDocumentTest, LoadEmptyFilenameFails){
    SceneFixture fixture;
    UIDocument& document = fixture.add<UIDocument>();

    // an empty filename is nothing to load
    EXPECT_FALSE(document.load(""));
    EXPECT_FALSE(document.isLoaded());
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentTest, ShowHide){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    UIDocument& document = fixture.add<UIDocument>();
    EXPECT_TRUE(document.isVisible());
    ASSERT_TRUE(document.load("ecs/basic.rml"));
    Rml::ElementDocument* rml = findDocument(scene, "ecs basic");
    ASSERT_NE(rml, nullptr);

    document.hide();
    EXPECT_FALSE(document.isVisible());
    EXPECT_FALSE(document.getComponent<UIDocumentComponent>().enabled);
    updateDocuments(scene);
    EXPECT_FALSE(rml->IsVisible());

    document.setVisible(true);
    updateDocuments(scene);
    EXPECT_TRUE(rml->IsVisible());

    document.setVisible(false);
    document.show();
    updateDocuments(scene);
    EXPECT_TRUE(document.isVisible());
    EXPECT_TRUE(rml->IsVisible());

    // a document loaded while hidden stays hidden
    document.hide();
    ASSERT_TRUE(document.load("ecs/second.rml"));
    Rml::ElementDocument* second = findDocument(scene, "ecs second");
    ASSERT_NE(second, nullptr);
    EXPECT_FALSE(second->IsVisible());
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
