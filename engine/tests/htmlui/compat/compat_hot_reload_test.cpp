// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Phase-2 integration (p2-integrate): hot reload of the files an .html document draws with,
// through UIDocumentSystem::reloadChangedFiles as the editor's watcher calls it. The document
// lives under the assets directory, as in a project: the compat layer hands RmlUi its
// references as asset:// URLs, and phase 1's reload names the changed file by its asset path.
// - an image replaced on disk (an <img> and a CSS background) draws with the new pixels, the
//   document and its elements stay;
// - a font replaced on disk (@font-face url()) reloads the document with the new face;
// - a linked sheet and an @imported sheet restyle in place; the document itself reloads.

#include "TestGoldenCommon.h"
#include "TestDocumentCommon.h"

#include "HtmlUI.h"
#include "LuaBinding.h"
#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/ImportManifest.h"
#include "subsystem/UIDocumentSystem.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include "lua.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::test;
namespace hc = jasperengine::htmlui::compat;
namespace fs = std::filesystem;

namespace {

    // an uncompressed 24-bit BMP of one colour (the engine's image loader reads it)
    std::string bmp(int width, int height, uint8_t r, uint8_t g, uint8_t b){
        const int row = (width * 3 + 3) & ~3;
        const uint32_t size = 54 + (uint32_t)(row * height);
        std::string out(size, '\0');
        const auto put32 = [&out](size_t at, uint32_t value){
            for (int i = 0; i < 4; i++)
                out[at + (size_t)i] = (char)((value >> (8 * i)) & 0xFF);
        };
        out[0] = 'B'; out[1] = 'M';
        put32(2, size); put32(10, 54); put32(14, 40); put32(18, (uint32_t)width); put32(22, (uint32_t)height);
        out[26] = 1; out[28] = 24;
        put32(34, (uint32_t)(row * height));
        for (int y = 0; y < height; y++){
            for (int x = 0; x < width; x++){
                const size_t at = 54 + (size_t)(y * row + x * 3);
                out[at] = (char)b; out[at + 1] = (char)g; out[at + 2] = (char)r;
            }
        }
        return out;
    }

    const char* documentHtml =
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "  <link rel=\"stylesheet\" href=\"hot.css\">\n"
        "</head>\n"
        "<body>\n"
        "  <img id=\"pic\" src=\"pic.bmp\" alt=\"\">\n"
        "  <div id=\"layer\"></div>\n"
        "  <div id=\"imp\"></div>\n"
        "  <span id=\"txt\">Hot reload text</span>\n"
        "</body>\n"
        "</html>\n";

    const char* documentCss =
        "@import url(\"more.css\");\n"
        "@font-face { font-family: \"Hot\"; src: url(\"hot.ttf\") format(\"truetype\"); }\n"
        "* { margin: 0; padding: 0; }\n"
        "body { background: rgb(0, 0, 0); }\n"
        "#pic { position: absolute; left: 20px; top: 20px; width: 100px; height: 100px; }\n"
        "#layer { position: absolute; left: 140px; top: 20px; width: 100px; height: 100px; background: url(pic.bmp) center / cover; }\n"
        "#txt { position: absolute; left: 20px; top: 250px; font-family: \"Hot\", sans-serif; font-size: 32px; color: white; }\n";

    const char* importedCss = "#imp { position: absolute; left: 260px; top: 20px; width: 100px; height: 100px; background: rgb(0, 0, 255); }\n";

    class CompatHotReloadTest : public FullPathGoldenTest {
    protected:
        fs::path dir;

        void SetUp() override{
            FullPathGoldenTest::SetUp();
            ASSERT_EQ(HtmlUI::getReferenceCount(), 0) << "an earlier test left RmlUi initialised";
            const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
            dir = fs::path(JASPER_HTMLUI_TEST_BINARY_DIR) / "compat-hot-reload" / info->name();
            fs::remove_all(dir);
            fs::create_directories(dir / "ui");
            write("ui/hot.html", documentHtml);
            write("ui/hot.css", documentCss);
            write("ui/more.css", importedCss);
            write("ui/pic.bmp", bmp(8, 8, 255, 0, 255));
            fs::copy_file(fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / "fixtures/compat/pages/htmlui-imports/fonts/inter/Inter-400.ttf",
                          dir / "ui/hot.ttf");
            // the directory is the project's assets directory
            TestSystem::get().setAssetPath(dir.generic_string());
            hc::ImportManifest::reload();
            hc::DocumentLoader::clearCache();
        }

        void TearDown() override{
            hc::DocumentLoader::clearCache();
            TestSystem::get().setAssetPath("");
            hc::ImportManifest::reload();
            EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
            FullPathGoldenTest::TearDown();
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

        void replace(const std::string& name, const fs::path& source){
            std::ifstream in(source, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            ASSERT_FALSE(content.empty()) << source;
            write(name, content);
        }

        float widthOf(Rml::ElementDocument* document, const char* id){
            Rml::Element* element = document ? document->GetElementById(id) : nullptr;
            return element ? element->GetBox().GetSize(Rml::BoxArea::Border).x : -1.0f;
        }
    };

}

TEST_F(CompatHotReloadTest, ReplacedImageDrawsWithoutReloadingTheDocument){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument("ui/hot.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    UIDocumentSystem& system = documentSystem(scene);
    Rml::ElementDocument* rml = system.getDocument(document.getEntity());
    ASSERT_NE(rml, nullptr);
    ASSERT_NE(system.getCompiledDocument(document.getEntity()), nullptr);

    Screen frame;
    ASSERT_TRUE(capture(frame, 3));
    ASSERT_TRUE(frame.is(70, 70, 255, 0, 255)) << "the <img>";
    ASSERT_TRUE(frame.is(190, 70, 255, 0, 255)) << "the CSS background";
    const std::vector<std::string> files = system.getDocumentFiles(document.getEntity());
    EXPECT_NE(std::find(files.begin(), files.end(), "ui/pic.bmp"), files.end()) << "the image is one of the document's files";

    write("ui/pic.bmp", bmp(8, 8, 0, 255, 255));
    const std::vector<UIDocumentSystem::Reloaded> reloaded = system.reloadChangedFiles({"ui/pic.bmp"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly);
    EXPECT_EQ(system.getDocument(document.getEntity()), rml) << "an image change keeps the document";
    EXPECT_EQ(system.getLoadCount(document.getEntity()), 1u);

    ASSERT_TRUE(capture(frame, 3));
    EXPECT_TRUE(frame.is(70, 70, 0, 255, 255)) << "the <img> draws the replaced file";
    EXPECT_TRUE(frame.is(190, 70, 0, 255, 255)) << "the CSS background draws the replaced file";

    // the same file again: nothing to do (the stamps are current)
    EXPECT_TRUE(system.reloadChangedFiles({"ui/pic.bmp"}).empty());
}

TEST_F(CompatHotReloadTest, ReplacedFontReloadsTheDocumentWithTheNewFace){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument("ui/hot.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    UIDocumentSystem& system = documentSystem(scene);
    GpuEnvironment::drawFrames(2);
    Rml::ElementDocument* rml = system.getDocument(document.getEntity());
    ASSERT_NE(rml, nullptr);
    const float inter = widthOf(rml, "txt");
    ASSERT_GT(inter, 100.0f);
    for (const hc::Diagnostic& diagnostic : system.getDiagnostics(document.getEntity()))
        EXPECT_NE(diagnostic.code, hc::codes::UnknownFontFamily) << hc::toText(diagnostic);
    const std::vector<std::string> files = system.getDocumentFiles(document.getEntity());
    EXPECT_NE(std::find(files.begin(), files.end(), "ui/hot.ttf"), files.end()) << "the font is one of the document's files";

    // Orbitron is much wider than Inter
    replace("ui/hot.ttf", fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / "fixtures/compat/pages/htmlui-imports/fonts/orbitron/Orbitron-700.ttf");
    const std::vector<UIDocumentSystem::Reloaded> reloaded = system.reloadChangedFiles({"ui/hot.ttf"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_FALSE(reloaded[0].stylesheetOnly);
    ASSERT_TRUE(fixture.waitUntilLoaded());
    GpuEnvironment::drawFrames(2);
    Rml::ElementDocument* after = system.getDocument(document.getEntity());
    ASSERT_NE(after, nullptr);
    EXPECT_GT(widthOf(after, "txt"), inter * 1.15f) << "the text is laid out with the replaced font";
}

TEST_F(CompatHotReloadTest, SheetsRestyleAndTheDocumentReloads){
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument("ui/hot.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    UIDocumentSystem& system = documentSystem(scene);
    Rml::ElementDocument* rml = system.getDocument(document.getEntity());
    Screen frame;
    ASSERT_TRUE(capture(frame, 3));
    ASSERT_TRUE(frame.is(310, 70, 0, 0, 255)) << "the @imported sheet";

    // the @imported sheet: a dependency of the document, restyled in place
    write("ui/more.css", std::string(importedCss) + "#imp { background: rgb(255, 255, 0); }\n");
    std::vector<UIDocumentSystem::Reloaded> reloaded = system.reloadChangedFiles({"ui/more.css"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly);
    EXPECT_EQ(system.getDocument(document.getEntity()), rml);
    ASSERT_TRUE(capture(frame, 3));
    EXPECT_TRUE(frame.is(310, 70, 255, 255, 0));

    // the linked sheet
    write("ui/hot.css", std::string(documentCss) + "#pic { left: 400px; }\n");
    reloaded = system.reloadChangedFiles({"ui/hot.css"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly);
    EXPECT_EQ(system.getDocument(document.getEntity()), rml);
    ASSERT_TRUE(capture(frame, 3));
    EXPECT_TRUE(frame.is(450, 70, 255, 0, 255)) << "the <img> moved";
    EXPECT_TRUE(frame.is(310, 70, 255, 255, 0)) << "the @imported sheet's change stays";

    // the document: reloaded in full, once
    std::string html = documentHtml;
    html.replace(html.find("</body>"), 7, "<div id=\"added\" style=\"position: absolute; left: 500px; top: 200px; width: 50px; "
                                          "height: 50px; background: lime\"></div></body>");
    write("ui/hot.html", html);
    reloaded = system.reloadChangedFiles({"ui/hot.html"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_FALSE(reloaded[0].stylesheetOnly);
    ASSERT_TRUE(fixture.waitUntilLoaded());
    // (the count is of loads and stylesheet reloads, as for .rml documents)
    EXPECT_EQ(system.getLoadCount(document.getEntity()), 4u);
    ASSERT_TRUE(capture(frame, 3));
    EXPECT_TRUE(frame.is(525, 225, 0, 255, 0));
    // one summary line per load: two loads and two restyles
    EXPECT_LE(TestSystem::get().countLogs(0, "ui/hot.html: 0 errors"), 4u);
}

// p4-checks: a stylesheet-only reload replaces the document's diagnostics (getDiagnostics is
// of the last (re)load, which MCP's pick-up reports), an @imported sheet's included
TEST_F(CompatHotReloadTest, RestyleReplacesTheDiagnostics){
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/hot.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    UIDocumentSystem& system = documentSystem(fixture.getScene());
    auto count = [&](const std::string& file){
        size_t found = 0;
        for (const hc::Diagnostic& diagnostic : system.getDiagnostics(document.getEntity())){
            if (diagnostic.file == file && diagnostic.code == hc::codes::UnsupportedProperty)
                found++;
        }
        return found;
    };
    EXPECT_EQ(count("ui/hot.css"), 0u);

    write("ui/hot.css", std::string(documentCss) + "#pic { mix-blend-mode: screen; }\n");
    std::vector<UIDocumentSystem::Reloaded> reloaded = system.reloadChangedFiles({"ui/hot.css"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly);
    EXPECT_EQ(count("ui/hot.css"), 1u) << "the edited sheet's warning";
    const std::vector<hc::Diagnostic>& now = system.getDiagnostics(document.getEntity());
    const auto blend = std::find_if(now.begin(), now.end(), [](const hc::Diagnostic& d){ return d.code == hc::codes::UnsupportedProperty; });
    ASSERT_NE(blend, now.end());
    EXPECT_EQ(blend->line, 8u);

    write("ui/more.css", std::string(importedCss) + "#imp { zoom: 2; }\n");
    reloaded = system.reloadChangedFiles({"ui/more.css"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_TRUE(reloaded[0].stylesheetOnly);
    EXPECT_EQ(count("ui/more.css"), 1u) << "the @imported sheet's warning";
    EXPECT_EQ(count("ui/hot.css"), 1u) << "kept: the linked sheet still has it";

    write("ui/hot.css", documentCss);
    write("ui/more.css", importedCss);
    reloaded = system.reloadChangedFiles({"ui/hot.css", "ui/more.css"});
    ASSERT_EQ(reloaded.size(), 1u);
    EXPECT_EQ(count("ui/hot.css") + count("ui/more.css"), 0u) << "fixed: gone";
    // the reloads logged the warnings: 'mix-blend-mode' twice (two restyles), 'zoom' once
    expectedWarnings = 3;
}

namespace {

    double luaNumber(const std::string& expression){
        lua_State* L = LuaBinding::getLuaState();
        const std::string code = "return " + expression;
        if (!L || luaL_loadstring(L, code.c_str()) != LUA_OK || LuaBinding::pcallWithTraceback(L, 0, 1) != LUA_OK){
            if (L)
                lua_pop(L, 1);
            return std::nan("");
        }
        const double value = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : std::nan("");
        lua_pop(L, 1);
        return value;
    }

    void luaRun(const std::string& code){
        lua_State* L = LuaBinding::getLuaState();
        ASSERT_NE(L, nullptr);
        ASSERT_EQ(luaL_dostring(L, code.c_str()), LUA_OK) << lua_tostring(L, -1);
    }

}

// The document's own <script type="text/lua"> runs again after every reload and attaches its
// listeners again: the ones it attached to the old elements are not moved over as well (they
// were, like a scene script's listeners on elements with an id: one more copy per reload, so
// a click counted twice after the first reload and three times after the second). A scene
// script's listener on the same element is still moved, once.
TEST_F(CompatHotReloadTest, DocumentScriptListenersAreAttachedOncePerReload){
    std::string html = documentHtml;
    html.replace(html.find("</body>"), 7,
        "<button id=\"go\">Go</button><div id=\"plain\">no id needed</div>\n"
        "<script type=\"text/lua\">\n"
        "  _G.hotRuns = (_G.hotRuns or 0) + 1\n"
        "  document:getElementById('go'):addEventListener('click', function() _G.hotClicks = (_G.hotClicks or 0) + 1 end)\n"
        "  document:querySelector('#plain'):addEventListener('click', function() _G.hotPlain = (_G.hotPlain or 0) + 1 end)\n"
        "  document:addEventListener('keydown', function() end)\n"
        "</script></body>");
    write("ui/hot.html", html);
    luaRun("hotRuns, hotClicks, hotPlain, sceneClicks = nil, nil, nil, nil");

    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument("ui/hot.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    UIDocumentSystem& system = documentSystem(scene);
    system.startDocumentScripts();
    GpuEnvironment::drawFrames(2);
    ASSERT_EQ(luaNumber("hotRuns"), 1);

    // a scene script's listener (C++ here), bound once as scripts do in init
    int sceneClicks = 0;
    document.getElementById("go").addEventListener("click", [&sceneClicks](UIEvent&){ sceneClicks++; });
    const size_t listeners = HtmlUIEventListener::liveCount();

    for (int reload = 1; reload <= 3; reload++){
        SCOPED_TRACE(reload);
        write("ui/hot.html", html + "<!-- " + std::to_string(reload) + " -->\n");
        const std::vector<UIDocumentSystem::Reloaded> reloaded = system.reloadChangedFiles({"ui/hot.html"});
        ASSERT_EQ(reloaded.size(), 1u);
        EXPECT_FALSE(reloaded[0].stylesheetOnly);
        ASSERT_TRUE(fixture.waitUntilLoaded());
        GpuEnvironment::drawFrames(2);
        EXPECT_EQ(luaNumber("hotRuns"), reload + 1) << "the script ran again, once";
        EXPECT_EQ(HtmlUIEventListener::liveCount(), listeners) << "as many listeners as before the reload";

        luaRun("hotClicks, hotPlain = 0, 0");
        sceneClicks = 0;
        document.getElementById("go").click();
        document.getElementById("plain").click();
        GpuEnvironment::drawFrames(1);
        EXPECT_EQ(luaNumber("hotClicks"), 1) << "one count per click";
        EXPECT_EQ(luaNumber("hotPlain"), 1) << "the listener on an element without an id is the new run's";
        EXPECT_EQ(sceneClicks, 1) << "the scene script's listener moved to the new element";
    }
    // no "listeners on elements without an id were dropped" warning for the script's own
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "were dropped by the reload"), 0u);

    LuaBinding::cleanupLuaScripts(&scene);
    luaRun("hotRuns, hotClicks, hotPlain = nil, nil, nil; collectgarbage()");
    EXPECT_EQ(HtmlUIScriptState::liveLuaRefs(), 0u);
}

// A family an .html document names and nothing provides is reported by the compat layer, at
// the document, as a note (HTMLUI-CSS-0023): the font engine's own warning ("font-family
// '<name>' is not loaded", no location) is not logged as well. It still is for an .rml
// document, which has no other report.
TEST_F(CompatHotReloadTest, UnknownFontFamilyIsReportedOnceAtTheDocument){
    write("ui/hot.css", std::string(documentCss) + "body { font-family: 'P2 Missing Face', 'Segoe UI', sans-serif; }\n");
    write("ui/plain.rml", "<rml><head><style>body { display: block; font-family: P2 Rml Missing Face, sans-serif; }</style></head>"
                          "<body>text</body></rml>\n");
    expectedWarnings = 1;   // the .rml document's

    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument("ui/hot.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    GpuEnvironment::drawFrames(2);
    UIDocumentSystem& system = documentSystem(scene);

    size_t notes = 0;
    for (const hc::Diagnostic& diagnostic : system.getDiagnostics(document.getEntity())){
        if (diagnostic.code == hc::codes::UnknownFontFamily){
            notes++;
            EXPECT_EQ(diagnostic.severity, hc::Severity::Info);
            EXPECT_EQ(diagnostic.file, "ui/hot.html");
        }
    }
    EXPECT_EQ(notes, 2u) << "one note per family";
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "is not loaded"), 0u) << logText(D_LOG_WARN);

    UIDocument plain = fixture.addDocument("ui/plain.rml", 1);
    ASSERT_TRUE(fixture.waitUntilLoaded());
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "font-family 'p2 rml missing face' is not loaded"), 1u) << logText(D_LOG_WARN);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "is not loaded"), 1u) << logText(D_LOG_WARN);
}
