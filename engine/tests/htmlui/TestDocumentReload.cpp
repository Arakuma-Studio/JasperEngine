// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Hot reload of documents (phase 1, p1-editor, spec §3.8.6-§3.8.7): dependencies (scanner and
// runtime file log), per-document stamps behind reloadChangedFiles, stylesheet-only reloads
// that keep the DOM, full reloads that keep scroll, focus and form values, onLoad/onUnload,
// image and font reloads, per-document messages, the debugger and the update delay the editor
// redraws by. Documents live in a scratch directory under the build tree (absolute paths), so
// the tests can change them.

#include "TestDocumentCommon.h"

#include "HtmlUIDependencies.h"
#include "RmlFontEngine.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/effects/EffectRegistry.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::test;

namespace fs = std::filesystem;

namespace {

    const std::string reloadCss =
        "body { display: block; width: 100%; height: 100%; margin: 0px; font-family: sans-serif; font-size: 16px; }\n"
        "div, p, input { display: block; }\n"
        "#box { width: 100px; height: 50px; background-color: #ff0000; }\n"
        "#list { width: 200px; height: 60px; overflow: auto; }\n"
        "#list p { height: 40px; }\n"
        "#name { width: 200px; height: 30px; }\n";

    std::string reloadHtml(const std::string& extra = ""){
        return
            "<html>\n"
            "<head>\n"
            "    <title>reload</title>\n"
            "    <link type=\"text/css\" href=\"reload.css\"/>\n"
            "</head>\n"
            "<body>\n"
            "    <div id=\"box\"></div>\n"
            "    <input id=\"name\" type=\"text\" value=\"\"/>\n"
            "    <input id=\"agree\" type=\"checkbox\"/>\n"
            "    <div id=\"list\"><p>1</p><p>2</p><p>3</p><p>4</p><p>5</p></div>\n"
            "    <img src=\"pic.png\"/>\n" + extra +
            "</body>\n"
            "</html>\n";
    }

    class UIDocumentReloadTest : public ::testing::Test {
    protected:
        fs::path dir;

        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            ASSERT_EQ(HtmlUI::getReferenceCount(), 0) << "an earlier test left RmlUi initialised";
            TestSystem::get().clearLogs();

            const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
            dir = fs::path(JASPER_HTMLUI_TEST_BINARY_DIR) / "reload" / info->name();
            fs::remove_all(dir);
            fs::create_directories(dir);
            fs::copy_file(fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / "fixtures/common/checker.png", dir / "pic.png");
            write("reload.css", reloadCss);
            write("reload.rml", reloadHtml());
        }

        void TearDown() override{
            EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
            EXPECT_EQ(HtmlUI::getContextCount(), 0u);
        }

        std::string path(const std::string& name) const{
            return (dir / name).generic_string();
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

        std::vector<std::string> allFiles(Scene& scene, Entity entity){
            return documentSystem(scene).getDocumentFiles(entity);
        }

        size_t errors(const std::string& text = ""){
            return TestSystem::get().countLogs(D_LOG_ERROR, text);
        }
    };

    bool contains(const std::vector<std::string>& list, const std::string& value){
        return std::find(list.begin(), list.end(), value) != list.end();
    }

    std::string backgroundOf(Rml::Element* element){
        return element->GetProperty(Rml::PropertyId::BackgroundColor)->ToString();
    }

}

TEST_F(UIDocumentReloadTest, DocumentFilesAndLoadCount){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("reload.rml"));

    // before the first load: what the scanner sees
    std::vector<std::string> files = allFiles(scene, document.getEntity());
    EXPECT_TRUE(contains(files, path("reload.rml")));
    EXPECT_TRUE(contains(files, path("reload.css")));
    EXPECT_TRUE(contains(files, path("pic.png")));
    EXPECT_EQ(documentSystem(scene).getLoadCount(document.getEntity()), 0u);

    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());
    EXPECT_EQ(documentSystem(scene).getLoadCount(document.getEntity()), 1u);
    ASSERT_NE(documentSystem(scene).getDocument(document.getEntity()), nullptr);

    // the document first, its stylesheet and image, nothing twice
    files = allFiles(scene, document.getEntity());
    ASSERT_FALSE(files.empty());
    EXPECT_EQ(files[0], path("reload.rml"));
    EXPECT_TRUE(contains(files, path("reload.css")));
    EXPECT_TRUE(contains(files, path("pic.png")));
    std::vector<std::string> sorted = files;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(std::unique(sorted.begin(), sorted.end()), sorted.end());

    // nothing changed: nothing reloads, whoever asks
    EXPECT_TRUE(documentSystem(scene).reloadChangedFiles(files).empty());
    EXPECT_EQ(documentSystem(scene).getLoadCount(document.getEntity()), 1u);

    // a file the document does not use changes nothing either
    write("unrelated.css", "p { color: red; }");
    EXPECT_TRUE(documentSystem(scene).reloadChangedFiles({path("unrelated.css")}).empty());

    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentReloadTest, StyleSheetChangeKeepsTheDom){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("reload.rml"));
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());

    Rml::ElementDocument* rml = documentSystem(scene).getDocument(document.getEntity());
    UIElement box = document.getElementById("box");
    ASSERT_TRUE(box.isValid());
    Rml::Element* rmlBox = rml->GetElementById("box");
    EXPECT_EQ(backgroundOf(rmlBox), "#ff0000");

    std::string css = reloadCss;
    css.replace(css.find("#ff0000"), 7, "#00ff00");
    write("reload.css", css);

    const std::vector<UIDocumentSystem::Reloaded> reloaded = documentSystem(scene).reloadChangedFiles(allFiles(scene, document.getEntity()));
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_EQ(reloaded[0].entity, document.getEntity());
    EXPECT_TRUE(reloaded[0].stylesheetOnly);

    // same document, same element, new style
    EXPECT_EQ(documentSystem(scene).getDocument(document.getEntity()), rml);
    EXPECT_TRUE(box.isValid());
    EXPECT_EQ(rml->GetElementById("box"), rmlBox);
    EXPECT_EQ(backgroundOf(rmlBox), "#00ff00");
    EXPECT_EQ(documentSystem(scene).getLoadCount(document.getEntity()), 2u);

    // the second caller (MCP's sync after the editor's watcher) finds the stamps current
    EXPECT_TRUE(documentSystem(scene).reloadChangedFiles({path("reload.css")}).empty());
    EXPECT_EQ(documentSystem(scene).getLoadCount(document.getEntity()), 2u);

    EXPECT_EQ(errors(), 0u);
}

// p5-tooling: a file written while a (re)load is reading the document's files (the editor
// writing right after play stopped, an agent's next edit) is not taken as seen: the next
// reloadChangedFiles reloads it. The hook runs after the files were read and before the load
// records what they were, the window the stamps used to be taken in.
TEST_F(UIDocumentReloadTest, FileChangedDuringAReloadIsReloadedNext){
    struct HookReset {
        ~HookReset(){ UIDocumentSystem::setLoadedHookForTesting(nullptr); }
    } hookReset;
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("reload.rml"));
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());
    Rml::ElementDocument* rml = documentSystem(scene).getDocument(document.getEntity());
    ASSERT_EQ(backgroundOf(rml->GetElementById("box")), "#ff0000");

    auto colored = [](const char* color){
        std::string css = reloadCss;
        css.replace(css.find("#ff0000"), 7, color);
        return css;
    };
    // a stylesheet-only reload, the stylesheet written again while it runs
    int calls = 0;
    UIDocumentSystem::setLoadedHookForTesting([&](Entity){
        if (calls++ == 0)
            write("reload.css", colored("#0000ff"));
    });
    write("reload.css", colored("#00ff00"));
    ASSERT_EQ(documentSystem(scene).reloadChangedFiles(allFiles(scene, document.getEntity())).size(), 1u);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(backgroundOf(rml->GetElementById("box")), "#00ff00") << "the reload read the file before the second write";
    const std::vector<UIDocumentSystem::Reloaded> again = documentSystem(scene).reloadChangedFiles(allFiles(scene, document.getEntity()));
    ASSERT_EQ(again.size(), 1u) << "the file written during the reload is seen as changed";
    EXPECT_EQ(backgroundOf(rml->GetElementById("box")), "#0000ff");
    EXPECT_TRUE(documentSystem(scene).reloadChangedFiles(allFiles(scene, document.getEntity())).empty()) << "then current";

    // a full reload (the document changed), the stylesheet written while it runs
    calls = 0;
    UIDocumentSystem::setLoadedHookForTesting([&](Entity){
        if (calls++ == 0)
            write("reload.css", colored("#ffff00"));
    });
    write("reload.rml", reloadHtml("    <p id=\"added\">added</p>\n"));
    ASSERT_EQ(documentSystem(scene).reloadChangedFiles(allFiles(scene, document.getEntity())).size(), 1u);
    rml = documentSystem(scene).getDocument(document.getEntity());
    ASSERT_NE(rml->GetElementById("added"), nullptr);
    UIDocumentSystem::setLoadedHookForTesting(nullptr);
    ASSERT_EQ(documentSystem(scene).reloadChangedFiles(allFiles(scene, document.getEntity())).size(), 1u)
        << "the stylesheet written during the full reload is seen as changed";
    rml = documentSystem(scene).getDocument(document.getEntity());
    EXPECT_EQ(backgroundOf(rml->GetElementById("box")), "#ffff00");
    EXPECT_EQ(errors(), 0u);
}

// p5-tooling: elements keep the decorators and filters of an effect value whose text does not
// change, so a stylesheet-only reload across a change of the project effects (an effect declared
// from another file, read again, forgotten: MCP test_rename_frag_during_play) loads the document
// again in full; a stylesheet-only reload without one stays one.
TEST_F(UIDocumentReloadTest, ProjectEffectChangeDuringARestyleReloadsInFull){
    struct Reset {
        ~Reset(){
            UIDocumentSystem::setLoadedHookForTesting(nullptr);
            htmlui::EffectRegistry::get().clearProjectEffects();
            htmlui::compat::VirtualFiles::remove("@htmlui/test/reload/fx/restyle-glow.frag");
        }
    } reset;
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("reload.rml"));
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());
    Rml::ElementDocument* rml = documentSystem(scene).getDocument(document.getEntity());
    auto colored = [](const char* color){
        std::string css = reloadCss;
        css.replace(css.find("#ff0000"), 7, color);
        return css;
    };

    // a stylesheet-only reload: still one
    write("reload.css", colored("#00ff00"));
    std::vector<UIDocumentSystem::Reloaded> reloaded = documentSystem(scene).reloadChangedFiles(allFiles(scene, document.getEntity()));
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly);
    EXPECT_EQ(documentSystem(scene).getDocument(document.getEntity()), rml);

    // a project effect declared while it runs (as a compiled stylesheet's @effect does)
    const std::string frag = "#version 450\n"
                             "// @effect     restyle-glow\n"
                             "// @kind       paint\n"
                             "// @category   light\n"
                             "// @summary    A project effect of the reload tests\n"
                             "// @source     original\n"
                             "// @cost       low\n"
                             "#include \"effects/lib/jfx_common.glsl\"\n"
                             "vec4 jfx_paint(JfxContext c){ return vec4(1.0); }\n"
                             "#include \"effects/lib/main_paint.glsl\"\n";
    htmlui::compat::VirtualFiles::add("@htmlui/test/reload/fx/restyle-glow.frag", frag);
    int calls = 0;
    UIDocumentSystem::setLoadedHookForTesting([&](Entity){
        if (calls++ == 0){
            std::vector<htmlui::EffectDiagnostic> diags;
            EXPECT_TRUE(htmlui::EffectRegistry::get().declareProjectEffect("restyle-glow", "@htmlui/test/reload/fx/restyle-glow.frag",
                                                                           "reload.css", 1, diags));
        }
    });
    write("reload.css", colored("#0000ff"));
    reloaded = documentSystem(scene).reloadChangedFiles(allFiles(scene, document.getEntity()));
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_FALSE(reloaded[0].stylesheetOnly) << "loaded again in full";
    rml = documentSystem(scene).getDocument(document.getEntity());
    ASSERT_NE(rml, nullptr);
    EXPECT_EQ(backgroundOf(rml->GetElementById("box")), "#0000ff");
    EXPECT_EQ(errors(), 0u);
}

// p5-tooling: the editor's poller asks with a settle window: a document whose changed files are
// still being written (modified less than the window ago) is left for a later call with its
// stamps untouched, so files written back to back reload once whichever caller sees them first.
TEST_F(UIDocumentReloadTest, SettleWindowReloadsABatchOfWritesOnce){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("reload.rml"));
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());
    const uint32_t loads = documentSystem(scene).getLoadCount(document.getEntity());
    auto touch = [&](const std::string& name, const std::string& content, int secondsAgo){
        {
            std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
            out << content;
        }
        std::error_code error;
        fs::last_write_time(dir / name, fs::file_time_type::clock::now() - std::chrono::seconds(secondsAgo), error);
    };
    std::string css = reloadCss;
    css.replace(css.find("#ff0000"), 7, "#00ff00");

    // the stylesheet just written, the document not yet: the poll waits
    touch("reload.css", css, 0);
    EXPECT_TRUE(documentSystem(scene).reloadChangedFiles(allFiles(scene, document.getEntity()), 60.0).empty()) << "still settling";
    EXPECT_EQ(documentSystem(scene).getLoadCount(document.getEntity()), loads);
    // the document written too, both settled now (a minute and more ago): one reload for both
    touch("reload.css", css, 120);
    touch("reload.rml", reloadHtml("    <p id=\"added\">added</p>\n"), 120);
    const std::vector<UIDocumentSystem::Reloaded> reloaded = documentSystem(scene).reloadChangedFiles(allFiles(scene, document.getEntity()), 60.0);
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_EQ(documentSystem(scene).getLoadCount(document.getEntity()), loads + 1) << "one reload for the two files";
    Rml::ElementDocument* rml = documentSystem(scene).getDocument(document.getEntity());
    ASSERT_NE(rml->GetElementById("added"), nullptr);
    EXPECT_EQ(backgroundOf(rml->GetElementById("box")), "#00ff00");
    // and nothing left for any caller
    EXPECT_TRUE(documentSystem(scene).reloadChangedFiles(allFiles(scene, document.getEntity())).empty());
    // without a window (MCP's pick-up, a save from the code editor) a fresh file reloads at once
    css.replace(css.find("#00ff00"), 7, "#0000ff");
    touch("reload.css", css, 0);
    EXPECT_EQ(documentSystem(scene).reloadChangedFiles(allFiles(scene, document.getEntity())).size(), 1u);
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentReloadTest, DocumentChangeKeepsScrollFocusAndValues){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("reload.rml"));
    UIDocumentComponent& component = document.getComponent<UIDocumentComponent>();
    int loads = 0;
    int unloads = 0;
    component.onLoad.add("test_load", std::function<void()>([&loads](){ loads++; }));
    component.onUnload.add("test_unload", std::function<void()>([&unloads](){ unloads++; }));

    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());
    EXPECT_EQ(loads, 1);
    EXPECT_EQ(unloads, 0);

    Rml::ElementDocument* oldDocument = documentSystem(scene).getDocument(document.getEntity());
    auto* name = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(oldDocument->GetElementById("name"));
    ASSERT_NE(name, nullptr);
    name->SetValue("héllo");
    oldDocument->GetElementById("agree")->SetAttribute("checked", "");
    name->Focus();
    name->SetSelectionRange(1, 3);
    updateDocuments(scene);
    Rml::Element* list = oldDocument->GetElementById("list");
    list->SetScrollTop(40.0f);
    updateDocuments(scene);
    ASSERT_GT(list->GetScrollTop(), 0.0f);
    const float scrollTop = list->GetScrollTop();
    UIElement handleWithoutReload = document.getElementById("box");

    write("reload.rml", reloadHtml("    <p id=\"added\">new paragraph</p>\n"));
    const std::vector<UIDocumentSystem::Reloaded> reloaded = documentSystem(scene).reloadChangedFiles(allFiles(scene, document.getEntity()));
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_FALSE(reloaded[0].stylesheetOnly);

    Rml::ElementDocument* newDocument = documentSystem(scene).getDocument(document.getEntity());
    ASSERT_NE(newDocument, nullptr);
    EXPECT_NE(newDocument, oldDocument);
    ASSERT_NE(newDocument->GetElementById("added"), nullptr);
    EXPECT_TRUE(newDocument->IsVisible());
    EXPECT_EQ(documentCount(scene), 1);
    // handles of the old DOM are gone with it
    EXPECT_FALSE(handleWithoutReload.isValid());

    auto* newName = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(newDocument->GetElementById("name"));
    ASSERT_NE(newName, nullptr);
    EXPECT_EQ(newName->GetValue(), "héllo");
    EXPECT_TRUE(newDocument->GetElementById("agree")->HasAttribute("checked"));
    EXPECT_FLOAT_EQ(newDocument->GetElementById("list")->GetScrollTop(), scrollTop);
    EXPECT_EQ(rmlContext(scene)->GetFocusElement(), newName);
    int start = -1, end = -1;
    newName->GetSelection(&start, &end, nullptr);
    EXPECT_EQ(start, 1);
    EXPECT_EQ(end, 3);

    EXPECT_EQ(documentSystem(scene).getLoadCount(document.getEntity()), 2u);
    EXPECT_TRUE(component.loaded);
    EXPECT_FALSE(component.needReload);

    // onUnload and onLoad fire at the end of the next update (p1-lua's notifications), and
    // that update does not load the document again
    updateDocuments(scene);
    EXPECT_EQ(documentSystem(scene).getDocument(document.getEntity()), newDocument);
    EXPECT_EQ(loads, 2);
    EXPECT_EQ(unloads, 1);
    updateDocuments(scene);
    EXPECT_EQ(loads, 2);

    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentReloadTest, ReloadDocumentWithoutStateAndBeforeLoad){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("reload.rml"));

    // not loaded yet: a plain load
    documentSystem(scene).reloadDocument(document.getEntity(), true);
    ASSERT_TRUE(document.isLoaded());
    Rml::ElementDocument* first = documentSystem(scene).getDocument(document.getEntity());
    rmlui_dynamic_cast<Rml::ElementFormControlInput*>(first->GetElementById("name"))->SetValue("typed");

    // without state: a fresh document
    documentSystem(scene).reloadDocument(document.getEntity(), false);
    Rml::ElementDocument* second = documentSystem(scene).getDocument(document.getEntity());
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second, first);
    EXPECT_EQ(rmlui_dynamic_cast<Rml::ElementFormControlInput*>(second->GetElementById("name"))->GetValue(), "");
    EXPECT_EQ(documentCount(scene), 1);
    EXPECT_EQ(documentSystem(scene).getLoadCount(document.getEntity()), 2u);

    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentReloadTest, BrokenReloadUnloadsAndFixingItLoads){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("reload.rml"));
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());

    fs::remove(dir / "reload.rml");
    const std::vector<UIDocumentSystem::Reloaded> reloaded = documentSystem(scene).reloadChangedFiles({path("reload.rml")});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_FALSE(document.isLoaded());
    EXPECT_TRUE(document.getComponent<UIDocumentComponent>().loadFailed);
    EXPECT_EQ(documentCount(scene), 0);
    const std::vector<HtmlUIMessage>& messages = documentSystem(scene).getDocumentMessages(document.getEntity());
    ASSERT_FALSE(messages.empty());
    EXPECT_EQ(messages.back().level, HtmlUIMessageLevel::Error);
    TestSystem::get().clearLogs();

    // the file comes back: its stamp changed again
    write("reload.rml", reloadHtml());
    ASSERT_EQ(documentSystem(scene).reloadChangedFiles({path("reload.rml")}).size(), 1u);
    EXPECT_TRUE(document.isLoaded());
    EXPECT_TRUE(documentSystem(scene).getDocumentMessages(document.getEntity()).empty());
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentReloadTest, MissingStyleSheetCreatedLater){
    write("late.rml", "<html><head><link type=\"text/css\" href=\"late.css\"/></head><body><div id=\"box\"></div></body></html>\n");
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("late.rml"));
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());
    // the missing sheet is a dependency too
    EXPECT_TRUE(contains(allFiles(scene, document.getEntity()), path("late.css")));
    TestSystem::get().clearLogs();

    write("late.css", "body { display: block; } #box { display: block; width: 10px; height: 10px; background-color: #0000ff; }\n");
    const std::vector<UIDocumentSystem::Reloaded> reloaded = documentSystem(scene).reloadChangedFiles({path("late.css")});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly);
    Rml::Element* box = documentSystem(scene).getDocument(document.getEntity())->GetElementById("box");
    EXPECT_EQ(backgroundOf(box), "#0000ff");
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentReloadTest, ImageChangeKeepsTheDocument){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("reload.rml"));
    updateDocuments(scene);
    Rml::ElementDocument* rml = documentSystem(scene).getDocument(document.getEntity());

    fs::copy_file(fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / "fixtures/common/alpha50.png", dir / "pic.png", fs::copy_options::overwrite_existing);
    fs::last_write_time(dir / "pic.png", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    const std::vector<UIDocumentSystem::Reloaded> reloaded = documentSystem(scene).reloadChangedFiles({path("pic.png")});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly);
    EXPECT_EQ(documentSystem(scene).getDocument(document.getEntity()), rml);
    EXPECT_EQ(documentSystem(scene).getLoadCount(document.getEntity()), 1u);
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentReloadTest, FontChangeReloadsTheDocument){
    fs::copy_file(fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / "fixtures/fonts/Roboto-Bold.ttf", dir / "face.ttf");
    // RmlUi 6.3 opens a bare @font-face src as it is (from the asset root): an absolute path here
    write("font.css", "@font-face { font-family: Face; src: " + path("face.ttf") + "; }\nbody { display: block; font-family: Face; font-size: 20px; }\n");
    write("font.rml", "<html><head><link type=\"text/css\" href=\"font.css\"/></head><body><p id=\"text\">Text</p></body></html>\n");
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("font.rml"));
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());
    EXPECT_TRUE(contains(allFiles(scene, document.getEntity()), path("face.ttf")));
    Rml::ElementDocument* rml = documentSystem(scene).getDocument(document.getEntity());

    fs::copy_file(fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / "fixtures/fonts/Roboto-Italic.ttf", dir / "face.ttf", fs::copy_options::overwrite_existing);
    fs::last_write_time(dir / "face.ttf", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    const std::vector<UIDocumentSystem::Reloaded> reloaded = documentSystem(scene).reloadChangedFiles({path("face.ttf")});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_FALSE(reloaded[0].stylesheetOnly);
    EXPECT_NE(documentSystem(scene).getDocument(document.getEntity()), rml);
    EXPECT_NE(documentSystem(scene).getDocument(document.getEntity())->GetElementById("text"), nullptr);
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentReloadTest, MessagesOfTheDocument){
    write("broken.rml",
          "<html>\n"
          "<head><link type=\"text/css\" href=\"reload.css\"/></head>\n"
          "<body>\n"
          "<div id=\"a\"></span>\n"
          "</body>\n"
          "</html>\n");
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument good = fixture.addDocument(path("reload.rml"));
    UIDocument broken = fixture.addDocument(path("broken.rml"));
    updateDocuments(scene);

    EXPECT_TRUE(documentSystem(scene).getDocumentMessages(good.getEntity()).empty());
    const std::vector<HtmlUIMessage>& messages = documentSystem(scene).getDocumentMessages(broken.getEntity());
    ASSERT_FALSE(messages.empty());
    bool located = false;
    for (const HtmlUIMessage& message : messages){
        EXPECT_NE(message.level, HtmlUIMessageLevel::Info);
        if (message.file == path("broken.rml") && message.line == 4)
            located = true;
    }
    EXPECT_TRUE(located) << messages.front().text;

    // an entity without a document has none
    EXPECT_TRUE(documentSystem(scene).getDocumentMessages(NULL_ENTITY).empty());
}

TEST_F(UIDocumentReloadTest, StampsAreSharedBetweenCallers){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument a = fixture.addDocument(path("reload.rml"));
    write("other.rml", "<html><head><link type=\"text/css\" href=\"reload.css\"/></head><body/></html>\n");
    UIDocument b = fixture.addDocument(path("other.rml"));
    updateDocuments(scene);

    // both documents use the sheet: both restyle, once
    write("reload.css", reloadCss + "#box { height: 60px; }\n");
    std::vector<UIDocumentSystem::Reloaded> reloaded = documentSystem(scene).reloadChangedFiles({path("reload.css")});
    ASSERT_EQ(reloaded.size(), 2u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly && reloaded[1].stylesheetOnly);
    EXPECT_TRUE(documentSystem(scene).reloadChangedFiles({path("reload.css")}).empty());

    // only b's own file
    write("other.rml", "<html><head><link type=\"text/css\" href=\"reload.css\"/></head><body><p/></body></html>\n");
    reloaded = documentSystem(scene).reloadChangedFiles({path("reload.css"), path("other.rml"), path("reload.rml")});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_EQ(reloaded[0].entity, b.getEntity());
    EXPECT_EQ(documentSystem(scene).getLoadCount(a.getEntity()), 2u);
    EXPECT_EQ(documentSystem(scene).getLoadCount(b.getEntity()), 3u);
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentReloadTest, NextUpdateDelayWhileAnimating){
    write("anim.rml",
          "<html><head><style>\n"
          "@keyframes pulse { from { opacity: 0; } to { opacity: 1; } }\n"
          "body { display: block; width: 100%; height: 100%; }\n"
          "#a { display: block; width: 10px; height: 10px; background-color: red; animation: 1s linear infinite pulse; }\n"
          "</style></head><body><div id=\"a\"/></body></html>\n");
    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    // nothing loaded: never
    EXPECT_TRUE(std::isinf(documentSystem(scene).getNextUpdateDelay()));

    UIDocument still = fixture.addDocument(path("reload.rml"));
    updateDocuments(scene);
    EXPECT_TRUE(std::isinf(documentSystem(scene).getNextUpdateDelay()));

    UIDocument animated = fixture.addDocument(path("anim.rml"));
    updateDocuments(scene);
    updateDocuments(scene);
    EXPECT_EQ(documentSystem(scene).getNextUpdateDelay(), 0.0);

    animated.getComponent<UIDocumentComponent>().enabled = false;
    updateDocuments(scene);
    updateDocuments(scene);
    EXPECT_TRUE(std::isinf(documentSystem(scene).getNextUpdateDelay()));
    (void)still;
}

TEST_F(UIDocumentReloadTest, DebuggerIsGlobal){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    fixture.addDocument(path("reload.rml"));
    updateDocuments(scene);
    ASSERT_TRUE(HtmlUI::isDebuggerCompiled());

    std::unique_ptr<Scene> other(new Scene());
    std::unique_ptr<UIDocument> otherDocument(new UIDocument(other.get()));
    otherDocument->setEntityOwned(false);
    otherDocument->setFilename("ecs/second.rml");
    updateDocuments(*other);

    EXPECT_FALSE(documentSystem(scene).isDebuggerVisible());
    EXPECT_TRUE(documentSystem(scene).setDebuggerVisible(true));
    EXPECT_TRUE(documentSystem(scene).isDebuggerVisible());
    EXPECT_FALSE(documentSystem(*other).isDebuggerVisible());

    // shown in the other scene: hidden here
    EXPECT_TRUE(documentSystem(*other).setDebuggerVisible(true));
    EXPECT_TRUE(documentSystem(*other).isDebuggerVisible());
    EXPECT_FALSE(documentSystem(scene).isDebuggerVisible());
    EXPECT_EQ(HtmlUI::getDebuggerContext(), documentSystem(*other).getContext());

    EXPECT_TRUE(documentSystem(*other).setDebuggerVisible(false));
    EXPECT_FALSE(documentSystem(*other).isDebuggerVisible());
    EXPECT_FALSE(documentSystem(scene).isDebuggerVisible());

    // asked before the context exists: visible once it does
    std::unique_ptr<Scene> late(new Scene());
    EXPECT_TRUE(documentSystem(*late).setDebuggerVisible(true));
    EXPECT_TRUE(documentSystem(*late).isDebuggerVisible());
    late.reset();
    other.reset();
    EXPECT_FALSE(documentSystem(scene).isDebuggerVisible());
    EXPECT_EQ(errors(), 0u);
}

TEST_F(UIDocumentReloadTest, NewDocumentTemplateLoadsCleanly){
    // the document Assets > Create > HTML Document writes (editor/util/HtmlUITemplates)
    const fs::path templates = fs::path(JASPER_HTMLUI_TEST_BINARY_DIR) / "editor-logic" / "templates" / "NewDocument.html";
    if (!fs::exists(templates))
        GTEST_SKIP() << "HtmlUITemplates.CreatesDocumentAndStyleSheetWithFreeNames writes it";

    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(templates.generic_string());
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());
    EXPECT_TRUE(document.getElementById("title").isValid());
    EXPECT_TRUE(document.getElementById("start").isValid());
    EXPECT_EQ(errors(), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI]"), 0u);
    EXPECT_TRUE(documentSystem(scene).getDocumentMessages(document.getEntity()).empty());
}
