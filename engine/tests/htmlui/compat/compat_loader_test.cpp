// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// DocumentLoader (phase-2 §3.12): the compile cache and its invalidation, the dependency list,
// loading into an RmlUi context (bindings, virtual sheets through RmlFile, autofocus), restyles
// that keep the DOM (viewport, stylesheet hot reload through UIDocumentSystem, hook H2), the
// htmlnavigate default action (hook H4), themes, diagnostics of a document.

#include "../TestDocumentCommon.h"
#include "CompatTestCommon.h"

#include "harness/RecordingRenderInterface.h"

#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/ImportManifest.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/compat/css/CssRewriter.h"

#include "HtmlUI.h"
#include "RmlFile.h"
#include "object/ui/UIDocument.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::htmlui::compat;

namespace fs = std::filesystem;

namespace {

    bool contains(const std::vector<std::string>& list, const std::string& value){
        return std::find(list.begin(), list.end(), value) != list.end();
    }

    // a scratch directory of real files (absolute paths), as hot reload needs
    class ScratchFiles {
    public:
        explicit ScratchFiles(const std::string& name){
            dir = fs::path(JASPER_HTMLUI_TEST_BINARY_DIR) / "compat-loader" / name;
            fs::remove_all(dir);
            fs::create_directories(dir);
        }
        std::string path(const std::string& name) const{
            return (dir / name).generic_string();
        }
        // new content, and a later modification time whatever the file system's resolution
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

    private:
        fs::path dir;
    };

    struct RmlUiSession {
        bool ok;
        RecordingRenderInterface recording;
        Rml::Context* context = nullptr;
        RmlUiSession(): ok(HtmlUI::acquire()){
            if (ok)
                context = Rml::CreateContext("compat-loader", Rml::Vector2i(1280, 720), &recording);
        }
        ~RmlUiSession(){
            if (context)
                Rml::RemoveContext("compat-loader");
            Rml::ReleaseTextures(&recording);
            Rml::ReleaseRenderManagers();
            if (ok)
                HtmlUI::release();
        }
    };

    CompatOptions options(){
        CompatOptions result;
        result.viewport = CompatSize{1280, 720};
        return result;
    }

}

TEST(CompatLoader, CacheHitReturnsTheSamePointer){
    DocumentLoader::clearCache();
    VirtualFiles::add("@htmlui/test/cache/menu.html", "<!DOCTYPE html><link rel=\"stylesheet\" href=\"menu.css\"><p class=\"a\">x</p>");
    VirtualFiles::add("@htmlui/test/cache/menu.css", ".a { color: red; }");

    std::shared_ptr<const CompatDocument> first = DocumentLoader::compile("@htmlui/test/cache/menu.html", options());
    ASSERT_TRUE(first->compiled);
    EXPECT_EQ(DocumentLoader::compile("@htmlui/test/cache/menu.html", options()), first);
    EXPECT_EQ(DocumentLoader::cacheSize(), 1u);
    // the stylesheet's virtual file exists while the entry does
    ASSERT_EQ(first->stylesheets.size(), 1u);
    EXPECT_TRUE(VirtualFiles::exists(first->stylesheets[0].virtualPath));

    // other options: another compile
    CompatOptions dense = options();
    dense.dpRatio = 2.0f;
    std::shared_ptr<const CompatDocument> second = DocumentLoader::compile("@htmlui/test/cache/menu.html", dense);
    EXPECT_NE(second, first);

    // a dependency's content changed: the next compile sees it even without invalidate
    VirtualFiles::add("@htmlui/test/cache/menu.css", ".a { color: blue; }");
    std::shared_ptr<const CompatDocument> changed = DocumentLoader::compile("@htmlui/test/cache/menu.html", options());
    EXPECT_NE(changed, first);
    EXPECT_NE(changed->stylesheets[0].rcss.find("#0000ffff"), std::string::npos) << changed->stylesheets[0].rcss;   // blue
    // the stale entry went, with its virtual files
    EXPECT_FALSE(VirtualFiles::exists(first->stylesheets[0].virtualPath));

    // invalidate drops every compile listing the file
    const size_t before = DocumentLoader::cacheSize();
    DocumentLoader::invalidate("@htmlui/test/cache/menu.css");
    EXPECT_LT(DocumentLoader::cacheSize(), before);
    EXPECT_FALSE(VirtualFiles::exists(changed->stylesheets[0].virtualPath));
    EXPECT_NE(DocumentLoader::compile("@htmlui/test/cache/menu.html", options()), changed);

    // a missing document is not cached (creating it must load)
    std::shared_ptr<const CompatDocument> missing = DocumentLoader::compile("@htmlui/test/cache/none.html", options());
    EXPECT_FALSE(missing->compiled);
    ASSERT_EQ(missing->diagnostics.size(), 1u);
    EXPECT_EQ(missing->diagnostics[0].code, codes::FileNotFound);

    DocumentLoader::clearCache();
    EXPECT_EQ(DocumentLoader::cacheSize(), 0u);
    VirtualFiles::remove("@htmlui/test/cache/menu.html");
    VirtualFiles::remove("@htmlui/test/cache/menu.css");
}

TEST(CompatLoader, DependenciesAreComplete){
    ScratchFiles files("dependencies");
    files.write("menu.html", "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"menu.css\">"
                             "<script type=\"text/lua\" src=\"menu.lua\"></script></head>"
                             "<body><img src=\"icon.svg\"><img src=\"https://images.example.com/a.png\"></body></html>");
    files.write("menu.css", "@import \"base.css\";\n.a { color: red; }\n");
    files.write("base.css", ".b { color: blue; }\n");
    files.write("menu.lua", "local x = 1\n");
    files.write("icon.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"/>");

    std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compile(files.path("menu.html"), options());
    ASSERT_TRUE(compiled->compiled);
    const std::vector<std::string>& dependencies = compiled->dependencies;
    EXPECT_EQ(dependencies[0], files.path("menu.html"));
    EXPECT_TRUE(contains(dependencies, files.path("menu.css")));
    EXPECT_TRUE(contains(dependencies, files.path("menu.lua")));
    EXPECT_TRUE(contains(dependencies, files.path("icon.svg")));
    // a remote URL: its import would change the output
    EXPECT_TRUE(contains(dependencies, ImportManifest::kPath));
    // @import chains are the CSS rewriter's (p2-css); the seam does not follow them
    if (CssRewriter::rewriteSelector("body", nullptr) != "body"){
        EXPECT_TRUE(contains(dependencies, files.path("base.css")));
    }
    ASSERT_EQ(compiled->scripts.size(), 1u);
    EXPECT_EQ(compiled->scripts[0].srcPath, files.path("menu.lua"));
    EXPECT_EQ(compiled->scripts[0].code, "local x = 1\n");
    // the importers' discovery
    ASSERT_EQ(compiled->remoteRefs.size(), 1u);
    EXPECT_EQ(compiled->remoteRefs[0].kind, "image");
}

TEST(CompatLoader, LoadBindsTheDocument){
    RmlUiSession rml;
    ASSERT_TRUE(rml.context);
    VirtualFiles::add("@htmlui/test/load/menu.html",
                      "<!DOCTYPE html><html><head><title>Menu</title><link rel=\"stylesheet\" href=\"menu.css\"></head>"
                      "<body><input id=\"first\" type=\"text\"><input id=\"second\" type=\"text\" autofocus></body></html>");
    VirtualFiles::add("@htmlui/test/load/menu.css", "input { width: 80px; }");

    std::shared_ptr<const CompatDocument> compiled;
    Rml::ElementDocument* document = DocumentLoader::load(rml.context, "@htmlui/test/load/menu.html", options(), &compiled);
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(DocumentLoader::compiledOf(document), compiled);
    EXPECT_EQ(document->GetId(), DocumentLoader::kRootId);
    EXPECT_TRUE(DocumentLoader::isCompatDocument(document));
    EXPECT_EQ(document->GetTitle(), "Menu");
    ASSERT_GT(document->GetNumChildren(), 0);
    EXPECT_EQ(document->GetChild(0)->GetTagName(), "htmlbody");
    EXPECT_EQ(DocumentLoader::diagnosticsOf(document).size(), compiled->diagnostics.size());

    // the rewritten sheet is a virtual file RmlFile serves
    ASSERT_EQ(compiled->stylesheets.size(), 1u);
    RmlFile file;
    Rml::FileHandle handle = file.Open("/" + compiled->stylesheets[0].virtualPath);
    EXPECT_NE(handle, 0u);
    if (handle)
        file.Close(handle);

    // autofocus once shown
    document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    DocumentLoader::applyAutofocus(document);
    rml.context->Update();
    ASSERT_NE(rml.context->GetFocusElement(), nullptr);
    EXPECT_EQ(rml.context->GetFocusElement()->GetId(), "second");

    // locateElement reads data-htmlui-loc
    Rml::Element* second = document->GetElementById("second");
    std::optional<Diagnostic> location = locateElement(*compiled, second);
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->file, "@htmlui/test/load/menu.html");
    EXPECT_EQ(location->line, 1u);
    EXPECT_GT(location->column, 100u);

    // the binding goes with the document
    rml.context->UnloadDocument(document);
    rml.context->Update();
    EXPECT_EQ(DocumentLoader::compiledOf(document), nullptr);
    VirtualFiles::remove("@htmlui/test/load/menu.html");
    VirtualFiles::remove("@htmlui/test/load/menu.css");
}

TEST(CompatLoader, RestylesKeepTheElements){
    RmlUiSession rml;
    ASSERT_TRUE(rml.context);
    // w descriptors read the viewport: the compile depends on it
    VirtualFiles::add("@htmlui/test/restyle/menu.html",
                      "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"menu.css\"></head><body>"
                      "<div id=\"box\"></div><img srcset=\"a.png 400w, b.png 1600w\" sizes=\"50vw\"></body></html>");
    VirtualFiles::add("@htmlui/test/restyle/menu.css", "#box { display: block; width: 10px; height: 10px; }");

    std::shared_ptr<const CompatDocument> compiled;
    Rml::ElementDocument* document = DocumentLoader::load(rml.context, "@htmlui/test/restyle/menu.html", options(), &compiled);
    ASSERT_NE(document, nullptr);
    EXPECT_TRUE(compiled->dependsOnViewport);
    document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    rml.context->Update();
    Rml::Element* box = document->GetElementById("box");
    ASSERT_NE(box, nullptr);
    EXPECT_FLOAT_EQ(box->GetBox().GetSize().x, 10.0f);

    // canvas resized: new compile, same elements
    CompatOptions resized = options();
    resized.viewport = CompatSize{800, 600};
    std::shared_ptr<const CompatDocument> fresh = DocumentLoader::restyleForViewport(document, compiled, resized);
    EXPECT_NE(fresh, compiled);
    EXPECT_EQ(DocumentLoader::compiledOf(document), fresh);
    EXPECT_EQ(document->GetElementById("box"), box);

    // a stylesheet changed: the sheets swap in place
    VirtualFiles::add("@htmlui/test/restyle/menu.css", "#box { display: block; width: 30px; height: 10px; }");
    std::shared_ptr<const CompatDocument> edited = DocumentLoader::compile("@htmlui/test/restyle/menu.html", resized);
    ASSERT_TRUE(DocumentLoader::sameRml(*edited, *fresh));
    DocumentLoader::restyle(document, edited, resized);
    rml.context->Update();
    EXPECT_EQ(document->GetElementById("box"), box);
    EXPECT_FLOAT_EQ(box->GetBox().GetSize().x, 30.0f);
    EXPECT_EQ(DocumentLoader::compiledOf(document), edited);

    rml.context->UnloadDocument(document);
    rml.context->Update();
    VirtualFiles::remove("@htmlui/test/restyle/menu.html");
    VirtualFiles::remove("@htmlui/test/restyle/menu.css");
}

// A script changes the classes of <html>: the selectors whose first compound may match the root are banded for
// them (`.dark .x`, `.dark`), as for the classes the file wrote; nothing is compiled again while they stay.
TEST(CompatLoader, RootClassesChangedByAScript){
    RmlUiSession rml;
    ASSERT_TRUE(rml.context);
    VirtualFiles::add("@htmlui/test/rootclass/page.html",
                      "<!DOCTYPE html><html class=\"app\" lang=\"en\" data-x=\"1\"><head><style>"
                      "#box { display: block; width: 10px; height: 10px; } .dark #box { width: 20px; } "
                      "[data-theme=\"red\"] #box { height: 30px; } .app #box { margin-left: 5px; }"
                      "</style></head><body><div id=\"box\"></div></body></html>");
    std::shared_ptr<const CompatDocument> compiled;
    Rml::ElementDocument* document = DocumentLoader::load(rml.context, "@htmlui/test/rootclass/page.html", options(), &compiled);
    ASSERT_NE(document, nullptr);
    document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    rml.context->Update();
    Rml::Element* box = document->GetElementById("box");
    ASSERT_NE(box, nullptr);
    EXPECT_FLOAT_EQ(box->GetBox().GetSize().x, 10.0f);
    EXPECT_FLOAT_EQ(box->GetBox().GetEdge(Rml::BoxArea::Margin, Rml::BoxEdge::Left), 5.0f);

    // as loaded: nothing to do
    EXPECT_EQ(DocumentLoader::restyleForRoot(document, compiled), compiled);

    document->SetClass("dark", true);
    std::shared_ptr<const CompatDocument> dark = DocumentLoader::restyleForRoot(document, compiled);
    EXPECT_NE(dark, compiled);
    EXPECT_EQ(DocumentLoader::compiledOf(document), dark);
    rml.context->Update();
    EXPECT_EQ(document->GetElementById("box"), box);
    EXPECT_FLOAT_EQ(box->GetBox().GetSize().x, 20.0f);
    EXPECT_FLOAT_EQ(box->GetBox().GetEdge(Rml::BoxArea::Margin, Rml::BoxEdge::Left), 5.0f);
    EXPECT_EQ(DocumentLoader::restyleForRoot(document, dark), dark);

    // an attribute name the root did not have
    document->SetAttribute("data-theme", "red");
    std::shared_ptr<const CompatDocument> themed = DocumentLoader::restyleForRoot(document, dark);
    EXPECT_NE(themed, dark);
    rml.context->Update();
    EXPECT_FLOAT_EQ(box->GetBox().GetSize().y, 30.0f);
    // a value change of a known attribute is RmlUi's to match: no new compile
    document->SetAttribute("data-theme", "blue");
    EXPECT_EQ(DocumentLoader::restyleForRoot(document, themed), themed);
    rml.context->Update();
    EXPECT_FLOAT_EQ(box->GetBox().GetSize().y, 10.0f);

    // back to the file's classes: the first compile again (cached)
    document->SetClass("dark", false);
    document->RemoveAttribute("data-theme");
    std::shared_ptr<const CompatDocument> back = DocumentLoader::restyleForRoot(document, themed);
    EXPECT_EQ(back->rootSignature, compiled->rootSignature);
    rml.context->Update();
    EXPECT_FLOAT_EQ(box->GetBox().GetSize().x, 10.0f);

    rml.context->UnloadDocument(document);
    rml.context->Update();
    VirtualFiles::remove("@htmlui/test/rootclass/page.html");
}

class CompatLoaderSceneTest : public ::testing::Test {
protected:
    void SetUp() override{
        ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
        GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);
        TestSystem::get().clearLogs();
    }
    void TearDown() override{
        EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
    }
};

TEST_F(CompatLoaderSceneTest, StylesheetChangeKeepsTheDom){
    ScratchFiles files("hot-reload");
    files.write("menu.html", "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"menu.css\">"
                             "<script type=\"text/lua\" src=\"menu.lua\"></script></head>"
                             "<body><div id=\"box\"></div><input id=\"name\" type=\"text\"></body></html>");
    files.write("menu.css", "#box { display: block; width: 100px; height: 50px; }");
    files.write("menu.lua", "local x = 1\n");

    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(files.path("menu.html"));
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());
    UIDocumentSystem& system = documentSystem(scene);
    const Entity entity = document.getEntity();
    ASSERT_TRUE(system.getCompiledDocument(entity));

    // the compat dependencies are files of the document
    const std::vector<std::string> watched = system.getDocumentFiles(entity);
    EXPECT_TRUE(contains(watched, files.path("menu.css")));
    EXPECT_TRUE(contains(watched, files.path("menu.lua")));

    UIElement box = document.getElementById("box");
    Rml::ElementDocument* before = system.getDocument(entity);
    document.getElementById("name").setValue("typed");

    // a stylesheet: restyled in place
    files.write("menu.css", "#box { display: block; width: 200px; height: 50px; }");
    std::vector<UIDocumentSystem::Reloaded> reloaded = system.reloadChangedFiles({files.path("menu.css")});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly);
    EXPECT_EQ(system.getDocument(entity), before);
    EXPECT_TRUE(box.isValid());
    updateDocuments(scene);
    EXPECT_FLOAT_EQ(box.getBoundingClientRect().width, 200.0f);
    EXPECT_EQ(document.getElementById("name").getValue(), "typed");

    // the Lua src file: a full reload (its scripts run again)
    files.write("menu.lua", "local x = 2\n");
    reloaded = system.reloadChangedFiles({files.path("menu.lua")});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_FALSE(reloaded[0].stylesheetOnly);
    EXPECT_NE(system.getDocument(entity), before);
    // state kept by id
    EXPECT_EQ(document.getElementById("name").getValue(), "typed");

    // the document: full reload
    before = system.getDocument(entity);
    files.write("menu.html", "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"menu.css\"></head>"
                             "<body><div id=\"box\"></div><p id=\"added\">new</p></body></html>");
    reloaded = system.reloadChangedFiles({files.path("menu.html")});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_FALSE(reloaded[0].stylesheetOnly);
    EXPECT_TRUE(document.getElementById("added").isValid());
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u);
}

TEST_F(CompatLoaderSceneTest, HtmlNavigateLoadsTheLinkedDocument){
    ScratchFiles files("navigate");
    files.write("a.html", "<!DOCTYPE html><title>A</title><a id=\"go\" href=\"b.html#top\">B</a><a id=\"out\" href=\"https://example.com\">web</a>");
    files.write("b.html", "<!DOCTYPE html><title>B</title><p id=\"b\">b</p>");

    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(files.path("a.html"));
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());
    Rml::ElementDocument* rmlDocument = documentSystem(scene).getDocument(document.getEntity());

    // a prevented navigation stays
    UIElement out = document.getElementById("out");
    out.addEventListener("htmlnavigate", [](UIEvent& event){ event.preventDefault(); });
    Rml::Dictionary external;
    external["href"] = Rml::String("https://example.com");
    rmlDocument->GetElementById("out")->DispatchEvent("htmlnavigate", external);
    updateDocuments(scene);
    EXPECT_EQ(document.getFilename(), files.path("a.html"));

    Rml::Dictionary parameters;
    parameters["href"] = Rml::String("b.html#top");
    rmlDocument->GetElementById("go")->DispatchEvent("htmlnavigate", parameters);
    updateDocuments(scene);
    EXPECT_EQ(document.getFilename(), files.path("b.html"));
    EXPECT_TRUE(document.isLoaded());
    EXPECT_TRUE(document.getElementById("b").isValid());
}

TEST_F(CompatLoaderSceneTest, DiagnosticsAndThemes){
    ScratchFiles files("diagnostics");
    // the dark rule after the base rule: with the same specificity, the later one wins (as in CSS)
    files.write("menu.html", "<!DOCTYPE html><html><head><style>#box { display: block; width: 10px; height: 10px; }"
                             " @media (prefers-color-scheme: dark) { #box { width: 20px; } }</style></head>"
                             "<body><div id=\"box\"></div><canvas></canvas></body></html>");
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(files.path("menu.html"));
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());

    const std::vector<Diagnostic> diagnostics = document.getDiagnostics();
    bool placeholder = false;
    for (const Diagnostic& diagnostic : diagnostics){
        if (diagnostic.code == codes::UnsupportedElement)
            placeholder = true;
    }
    EXPECT_TRUE(placeholder);
    // logged with the summary line
    EXPECT_GE(TestSystem::get().countLogs(D_LOG_WARN, "[HTMLUI-HTML-0002]"), 1u);

    Rml::Context* context = rmlContext(scene);
    EXPECT_TRUE(context->IsThemeActive("light"));
    document.setColorScheme("dark");
    EXPECT_TRUE(context->IsThemeActive("dark"));
    EXPECT_FALSE(context->IsThemeActive("light"));
    updateDocuments(scene);
    // the media query becomes (theme: dark) with the CSS rewriter (p2-css)
    if (documentSystem(scene).getCompiledDocument(document.getEntity())->stylesheets[0].rcss.find("theme: dark") != std::string::npos){
        EXPECT_FLOAT_EQ(document.getElementById("box").getBoundingClientRect().width, 20.0f);
    }

    // a missing document: why, through the same API
    UIDocument missing = fixture.addDocument(files.path("none.html"));
    updateDocuments(scene);
    EXPECT_FALSE(missing.isLoaded());
    const std::vector<Diagnostic> why = missing.getDiagnostics();
    ASSERT_EQ(why.size(), 1u);
    EXPECT_EQ(why[0].code, codes::FileNotFound);
}

TEST_F(CompatLoaderSceneTest, TransparentBodyLetsThePointerThrough){
    ScratchFiles files("pointer");
    files.write("menu.html", "<!DOCTYPE html><html><head><style>body { margin: 0; width: 100%; height: 100%; }"
                             " #button { position: absolute; left: 10px; top: 10px; width: 50px; height: 50px; background-color: #ff0000; }"
                             "</style></head><body><div id=\"button\"></div></body></html>");
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(files.path("menu.html"));
    updateDocuments(scene);
    ASSERT_TRUE(document.isLoaded());
    UIDocumentSystem& system = documentSystem(scene);
    // over the button: the document takes it; over the transparent <body>: the game does
    EXPECT_TRUE(system.eventOnPointerMove(30.0f, 30.0f, 0));
    EXPECT_FALSE(system.eventOnPointerMove(300.0f, 300.0f, 0));
}
