// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Phase-2 integration (p2-integrate): play stop on the compat path. A screen document and a
// texture-mode document written as standard HTML/CSS (a linked sheet with an @import, an
// inline <style>, a data: URI, a Lucide icon, clip-path, <script type="text/lua"> with
// listeners and an anime animation) are played and stopped like the editor does:
// - stop releases what the document scripts made (Lua refs 0, the element pool empty) and
//   unloads the documents of every context; with them go the compat bindings: the RmlUi log
//   registrations of the loaded compiles;
// - the documents load again from the compiled-document cache (the same compile), and the
//   scripts run again at the next play;
// - cycles leak nothing: GPU objects, cache entries and virtual files are as many after the
//   twelfth cycle as after the second;
// - the cache is the process's by design (spec 3.12.2: at most 64 entries, least recently
//   used go): DocumentLoader::clearCache releases every entry with its virtual files, and
//   only the files that live for the process stay (the user-agent sheet's, data: URIs).

#include "TestGoldenCommon.h"
#include "TestDocumentCommon.h"

#include "HtmlUI.h"
#include "HtmlUIClock.h"
#include "HtmlUIContext.h"
#include "RmlRenderer.h"
#include "LuaBinding.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/anim/AnimEngine.h"
#include "htmlui/anim/AnimLua.h"
#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/RmlLogTranslator.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/compat/ua/UserAgentStyle.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "render/SystemRender.h"
#include "subsystem/UIDocumentSystem.h"

#include "lua.hpp"
#include "sokol_gfx.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

using namespace jasperengine;
using namespace jasperengine::test;
namespace hc = jasperengine::htmlui::compat;

namespace {

    std::string lua(const std::string& code){
        lua_State* L = LuaBinding::getLuaState();
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || LuaBinding::pcallWithTraceback(L, 0, 0) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    double luaNumber(const std::string& expression){
        lua_State* L = LuaBinding::getLuaState();
        const std::string code = "return " + expression;
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || LuaBinding::pcallWithTraceback(L, 0, 1) != LUA_OK){
            lua_pop(L, 1);
            return std::nan("");
        }
        const double value = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : std::nan("");
        lua_pop(L, 1);
        return value;
    }

    void drawFramesAt(double& time, int frames, double step){
        for (int i = 0; i < frames; i++){
            time += step;
            HtmlUIClock::setFixedTime(time);
            GpuEnvironment::drawFrames(1);
        }
    }

    class CompatPlayStopTest : public FullPathGoldenTest {
    protected:
        void SetUp() override{
            FullPathGoldenTest::SetUp();
            ASSERT_NE(LuaBinding::getLuaState(), nullptr);
            AnimEngine::instance().reset();
            hc::DocumentLoader::clearCache();
            time = 0.0;
            HtmlUIClock::setFixedTime(time);
            lua("playStopRuns, playStopClicks, playStopUpdates, textureClicks = nil, nil, nil, nil");
        }

        void TearDown() override{
            lua("playStopRuns, playStopClicks, playStopUpdates, textureClicks = nil, nil, nil, nil; collectgarbage()");
            AnimEngine::instance().reset();
            screenDocument.reset();
            textureDocument.reset();
            fixture.reset();
            lua("collectgarbage()");
            HtmlUIClock::reset();
            hc::DocumentLoader::clearCache();
            EXPECT_EQ(HtmlUIScriptState::liveLuaRefs(), 0u);
            EXPECT_EQ(AnimLua::getLiveLuaRefs(), 0u);
            EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 0u);
            EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
            FullPathGoldenTest::TearDown();
        }

        void createScene(){
            fixture.reset(new SceneFixture());
            screenDocument.reset(new UIDocument(fixture->addDocument("compat/integrate/screen.html")));
            textureDocument.reset(new UIDocument(fixture->addDocument("compat/integrate/texture.html", 1)));
            UIDocumentComponent& component = textureDocument->getComponent<UIDocumentComponent>();
            component.renderMode = UIDocumentRenderMode::TEXTURE;
            component.textureWidth = 256;
            component.textureHeight = 256;
        }

        UIDocumentSystem& system(){
            return *fixture->getScene().getSystem<UIDocumentSystem>();
        }

        // play start: the scene's scripts started, the document scripts run
        void startScripts(){
            system().startDocumentScripts();
        }

        // what the editor's play stop does (Project::stop: anime first, then
        // LuaBinding::cleanupLuaScripts for each runtime scene)
        void stopScripts(){
            AnimEngine::instance().reset();
            LuaBinding::cleanupLuaScripts(&fixture->getScene());
        }

        std::unique_ptr<SceneFixture> fixture;
        std::unique_ptr<UIDocument> screenDocument, textureDocument;
        double time = 0.0;
    };

}

TEST_F(CompatPlayStopTest, StopReleasesWhatTheCompatPathHolds){
    createScene();
    ASSERT_TRUE(fixture->waitUntilLoaded());
    ASSERT_TRUE(screenDocument->isLoaded());
    ASSERT_TRUE(textureDocument->isLoaded());
    // one registration per loaded compile (and the user-agent sheet's, until HtmlUI is released)
    const size_t registrationsLoaded = hc::RmlLogTranslator::registrationCount();
    ASSERT_GE(registrationsLoaded, 2u);

    // both documents went through the compat layer: compiled, bound, their sheets virtual files
    const Entity screenEntity = screenDocument->getEntity();
    const Entity textureEntity = textureDocument->getEntity();
    const std::shared_ptr<const hc::CompatDocument> compiled = system().getCompiledDocument(screenEntity);
    ASSERT_NE(compiled, nullptr);
    ASSERT_NE(system().getCompiledDocument(textureEntity), nullptr);
    EXPECT_EQ(hc::DocumentLoader::cacheSize(), 2u);
    EXPECT_FALSE(hc::VirtualFiles::list("@htmlui/doc/").empty());
    for (const hc::Diagnostic& diagnostic : system().getDiagnostics(screenEntity))
        EXPECT_NE(diagnostic.severity, hc::Severity::Error) << hc::toText(diagnostic);
    EXPECT_EQ(screenDocument->getDocumentElement().getTagName(), "HTML");

    startScripts();
    drawFramesAt(time, 8, 1.0 / 30.0);
    EXPECT_EQ(luaNumber("playStopRuns"), 1);
    EXPECT_GT(HtmlUIScriptState::liveLuaRefs(), 0u);
    EXPECT_GT(AnimLua::getLiveLuaRefs(), 0u);

    // stop: nothing of the scripts stays, the documents of both contexts are unloaded and
    // with them the bindings of their compiles
    stopScripts();
    EXPECT_EQ(HtmlUIScriptState::liveLuaRefs(), 0u);
    EXPECT_EQ(AnimLua::getLiveLuaRefs(), 0u);
    EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 0u);
    HtmlUIContext* textureContext = system().getTextureContext(textureEntity);
    ASSERT_NE(textureContext, nullptr);
    EXPECT_EQ(system().getContext()->getRmlContext()->GetNumDocuments(), 0);
    EXPECT_EQ(textureContext->getRmlContext()->GetNumDocuments(), 0);
    EXPECT_EQ(hc::RmlLogTranslator::registrationCount(), registrationsLoaded - 2);

    // the documents load again as they are on disk: the same compiles, from the cache
    ASSERT_TRUE(fixture->waitUntilLoaded());
    EXPECT_TRUE(screenDocument->isLoaded());
    EXPECT_TRUE(textureDocument->isLoaded());
    EXPECT_EQ(system().getCompiledDocument(screenEntity), compiled);
    EXPECT_EQ(hc::DocumentLoader::cacheSize(), 2u);
    EXPECT_EQ(hc::RmlLogTranslator::registrationCount(), registrationsLoaded);
    // no document script runs in edit mode
    drawFramesAt(time, 2, 0.0);
    EXPECT_EQ(luaNumber("playStopRuns"), 1);
    EXPECT_EQ(HtmlUIScriptState::liveLuaRefs(), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}

TEST_F(CompatPlayStopTest, CyclesLeakNothing){
    const int kCycles = 12;
    (void)hc::UserAgentStyle::get(hc::ForkFeatures::present());
    const size_t registrationsBefore = hc::RmlLogTranslator::registrationCount();
    createScene();
    ASSERT_TRUE(fixture->waitUntilLoaded());
    const size_t registrationsLoaded = hc::RmlLogTranslator::registrationCount();

    size_t screenTextures = 0, screenGeometry = 0, textureTextures = 0, textureGeometry = 0, pipelines = 0;
    size_t cacheEntries = 0, virtualFiles = 0;
    for (int cycle = 0; cycle < kCycles; cycle++){
        startScripts();
        drawFramesAt(time, 4, 0.25);
        EXPECT_EQ(luaNumber("playStopRuns"), cycle + 1) << "cycle " << cycle;
        stopScripts();
        ASSERT_TRUE(fixture->waitUntilLoaded()) << "cycle " << cycle;
        drawFramesAt(time, 2, 0.0);

        HtmlUIContext* screenContext = system().getContext();
        HtmlUIContext* textureContext = system().getTextureContext(textureDocument->getEntity());
        ASSERT_TRUE(screenContext && textureContext);
        if (cycle == 1){
            screenTextures = screenContext->getRenderer().getLiveTextureCount();
            screenGeometry = screenContext->getRenderer().getLiveGeometryCount();
            textureTextures = textureContext->getRenderer().getLiveTextureCount();
            textureGeometry = textureContext->getRenderer().getLiveGeometryCount();
            pipelines = SystemRender::getPipelineCacheSize();
            cacheEntries = hc::DocumentLoader::cacheSize();
            virtualFiles = hc::VirtualFiles::count();
            EXPECT_GT(screenTextures, 0u);
            EXPECT_GT(screenGeometry, 0u);
            EXPECT_GT(textureGeometry, 0u);
            EXPECT_EQ(cacheEntries, 2u);
        }else if (cycle > 1){
            EXPECT_EQ(screenContext->getRenderer().getLiveTextureCount(), screenTextures) << "cycle " << cycle;
            EXPECT_EQ(screenContext->getRenderer().getLiveGeometryCount(), screenGeometry) << "cycle " << cycle;
            EXPECT_EQ(textureContext->getRenderer().getLiveTextureCount(), textureTextures) << "cycle " << cycle;
            EXPECT_EQ(textureContext->getRenderer().getLiveGeometryCount(), textureGeometry) << "cycle " << cycle;
            EXPECT_EQ(SystemRender::getPipelineCacheSize(), pipelines) << "cycle " << cycle;
            EXPECT_EQ(hc::DocumentLoader::cacheSize(), cacheEntries) << "cycle " << cycle;
            EXPECT_EQ(hc::VirtualFiles::count(), virtualFiles) << "cycle " << cycle;
        }
        EXPECT_EQ(HtmlUIScriptState::liveLuaRefs(), 0u) << "cycle " << cycle;
        EXPECT_EQ(AnimLua::getLiveLuaRefs(), 0u) << "cycle " << cycle;
        EXPECT_EQ(hc::RmlLogTranslator::registrationCount(), registrationsLoaded) << "cycle " << cycle;
        if (::testing::Test::HasFailure())
            break;
    }

    // the scene goes: no binding stays; the cache keeps the compiles (the process's, bounded)
    // until it is cleared, and then only the files that live for the process are left
    screenDocument.reset();
    textureDocument.reset();
    fixture.reset();
    EXPECT_EQ(hc::RmlLogTranslator::registrationCount(), registrationsBefore);
    EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
    EXPECT_EQ(hc::DocumentLoader::cacheSize(), 2u);
    EXPECT_FALSE(hc::VirtualFiles::list("@htmlui/doc/").empty());
    hc::DocumentLoader::clearCache();
    EXPECT_EQ(hc::DocumentLoader::cacheSize(), 0u);
    std::string left;
    for (const std::string& path : hc::VirtualFiles::list("@htmlui/doc/"))
        left += path + "\n";
    EXPECT_TRUE(left.empty()) << left;
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}

// A compile that is not cached (a check or the importers' discovery reading through their own
// FileAccess: the editor does one at every load of a document; compileString) owns the virtual
// files of its sheets like a cached one: they go with it. They stayed for the life of the
// process, one copy of every linked sheet per check.
TEST(CompatVirtualFiles, UncachedCompilesReleaseTheirSheets){
    hc::DocumentLoader::clearCache();
    const size_t before = hc::VirtualFiles::list("@htmlui/doc/").size();

    std::map<std::string, std::string> disk = {
        {"ui/menu.html", "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"menu.css\"></head><body><p>x</p></body></html>"},
        {"ui/menu.css", "@import url('more.css'); p { color: red; }"},
        {"ui/more.css", "p { margin: 0; }"},
    };
    hc::FileAccess files;
    files.read = [&disk](const std::string& path, std::string& content){
        const auto it = disk.find(path);
        if (it == disk.end())
            return false;
        content = it->second;
        return true;
    };
    files.exists = [&disk](const std::string& path){ return disk.count(path) > 0; };

    {
        const std::shared_ptr<const hc::CompatDocument> compiled = hc::DocumentLoader::compile("ui/menu.html", hc::CompatOptions(), files);
        ASSERT_TRUE(compiled->compiled);
        // its sheet is a virtual file while the compile lives (RmlUi reads it from there)
        ASSERT_FALSE(compiled->virtualPaths.empty());
        EXPECT_TRUE(hc::VirtualFiles::exists(compiled->virtualPaths[0]));
        EXPECT_GT(hc::VirtualFiles::list("@htmlui/doc/").size(), before);
    }
    EXPECT_EQ(hc::VirtualFiles::list("@htmlui/doc/").size(), before);

    // the same through the public checks, many times
    hc::CheckOptions options;
    options.files = files;
    options.verifyWithRmlUi = false;
    for (int i = 0; i < 20; i++){
        EXPECT_FALSE(hc::checkDocument("ui/menu.html", options).files.empty());
        EXPECT_TRUE(hc::collectRemoteReferences("ui/menu.html", files).empty());
    }
    EXPECT_EQ(hc::VirtualFiles::list("@htmlui/doc/").size(), before);

    {
        const std::shared_ptr<const hc::CompatDocument> text = hc::DocumentLoader::compileString(disk["ui/menu.html"], "ui/other.html", hc::CompatOptions());
        (void)text;
    }
    EXPECT_EQ(hc::VirtualFiles::list("@htmlui/doc/").size(), before);
    EXPECT_EQ(hc::DocumentLoader::cacheSize(), 0u);
}

// The process ends with compiles still in the cache (a game or the editor quitting with
// documents loaded): they are destroyed with the statics, in the reverse order those were
// first used, and release their virtual files then. The registry of virtual files must
// outlive them: destroyed first, it made the process spin at exit (this suite did not end).
// Leaving an entry behind at the end of every run keeps that path exercised.
namespace {

    class CompileLeftCachedAtExit : public ::testing::Environment {
    public:
        void TearDown() override{
            const std::shared_ptr<const hc::CompatDocument> compiled =
                hc::DocumentLoader::compile("compat/integrate/texture.html", hc::CompatOptions());
            if (!compiled->compiled || hc::DocumentLoader::cacheSize() == 0)
                ADD_FAILURE() << "no compile left in the cache for the exit of the process";
        }
    };

    ::testing::Environment* const compileLeftCachedAtExit = ::testing::AddGlobalTestEnvironment(new CompileLeftCachedAtExit());

}

// The findings of a document are the same list wherever they are read (phase-2 spec 6):
// UIDocument::getDiagnostics() of the loaded document and compat::checkDocument() of its
// file. The fixture is a menu with the mistakes of a page written for a browser (a Tailwind
// CDN script and sheet, JavaScript, onclick=, grid, sticky, a misspelled property, a remote
// image without an import, a family that is not loaded): every one is in the list, at its
// line and column, with its code and a suggestion.
TEST_F(CompatPlayStopTest, DocumentAndCheckReturnTheSameFindings){
    fixture.reset(new SceneFixture());
    screenDocument.reset(new UIDocument(fixture->addDocument("compat/integrate/mistakes.html")));
    ASSERT_TRUE(fixture->waitUntilLoaded());
    drawFramesAt(time, 2, 0.0);

    std::vector<std::string> loaded;
    for (const hc::Diagnostic& diagnostic : screenDocument->getDiagnostics())
        loaded.push_back(hc::toText(diagnostic));
    std::vector<std::string> checked;
    for (const hc::Diagnostic& diagnostic : hc::checkDocument("compat/integrate/mistakes.html", hc::CheckOptions()).diagnostics)
        checked.push_back(hc::toText(diagnostic));
    std::sort(loaded.begin(), loaded.end());
    std::sort(checked.begin(), checked.end());
    EXPECT_EQ(loaded, checked);

    const char* expected[] = {
        "compat/integrate/mistakes.html:7:3: warning: Tailwind Play CDN script not run",
        "compat/integrate/mistakes.html:8:15: error: remote stylesheet 'https://cdn.jsdelivr.net/npm/tailwindcss@2.2.19/dist/tailwind.min.css' is not imported",
        "compat/integrate/mistakes.html:14:28: error: remote image 'https://images.example.net/logo.png' is not imported",
        "compat/integrate/mistakes.html:16:27: error: 'onclick' (JavaScript) is not run (fix: use element:addEventListener('click', fn) in Lua",
        "compat/integrate/mistakes.html:20:3: error: JavaScript is not run (the engine has no JavaScript engine) (fix: use <script type=\"text/lua\">",
        "compat/integrate/mistakes.css:17:3: warning: 'backgroud-color' is not a CSS property; dropped (fix: did you mean 'background-color'?) [HTMLUI-CSS-0001]",
        "compat/integrate/mistakes.html: info: font family 'Segoe UI' is not loaded and not generic",
    };
    for (const char* line : expected){
        const bool found = std::any_of(loaded.begin(), loaded.end(), [line](const std::string& text){ return text.compare(0, std::strlen(line), line) == 0; });
        EXPECT_TRUE(found) << "no finding starting with: " << line;
    }
    // the fork sticks the menu (ForkFeature::Sticky, phase 3): 'position: sticky' has no finding; without it, it is drawn as relative
    if (hc::ForkFeatures::present().has(hc::ForkFeature::Sticky)){
        EXPECT_TRUE(std::none_of(loaded.begin(), loaded.end(), [](const std::string& text){ return text.find("'position: sticky'") != std::string::npos; }));
    }else{
        const char* line = "compat/integrate/mistakes.css:13:3: warning: 'position: sticky' is drawn as 'relative'";
        EXPECT_TRUE(std::any_of(loaded.begin(), loaded.end(), [line](const std::string& text){ return text.compare(0, std::strlen(line), line) == 0; }))
            << "no finding starting with: " << line;
    }
    // the fork lays the grid out (ForkFeature::Grid, phase 3): 'display: grid' has no finding
    if (hc::ForkFeatures::present().has(hc::ForkFeature::Grid)){
        EXPECT_TRUE(std::none_of(loaded.begin(), loaded.end(), [](const std::string& text){ return text.find("'display: grid'") != std::string::npos; }));
    }else{
        EXPECT_TRUE(std::any_of(loaded.begin(), loaded.end(), [](const std::string& text){
            return text.rfind("compat/integrate/mistakes.css:22:3: warning: 'display: grid' is drawn as a wrapping flex container", 0) == 0;
        }));
    }
    // each once
    EXPECT_EQ(std::adjacent_find(loaded.begin(), loaded.end()), loaded.end());

    // (the fixture's errors are logged: expected here)
    TestSystem::get().clearLogs();
    expectedWarnings = 0;
}
