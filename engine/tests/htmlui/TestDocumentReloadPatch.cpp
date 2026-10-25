// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The live tree patch reload of HTML documents (S17 R27-R31, AC12): after an edit of the file the
// same element handles are valid, listeners on elements without an id still fire, an anime
// animation keeps running, a node a script appended stays, form values, scroll and focus are
// kept; an unchanged <script> does not run again and a changed one runs once; onReload fires and
// onLoad does not. A changed data-for replaces only its subtree; a broken edit keeps the live UI
// and reports its diagnostics; mode "rebuild" loads the document again as before.

#include "TestDocumentCommon.h"

#include "HtmlUIClock.h"
#include "component/UIDocumentComponent.h"
#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "object/ui/UIDocument.h"
#include "script/LuaBinding.h"

#include "object/ui/UIElementInternal.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::test;

namespace fs = std::filesystem;

namespace {

    const char* const kPatchCss =
        "body { margin: 0; font-family: sans-serif; font-size: 14px; }\n"
        "#list { height: 60px; overflow: auto; }\n"
        "#list p { height: 40px; margin: 0; }\n"
        "#box { width: 50px; height: 20px; }\n";

    std::string page(const std::string& body, const std::string& scripts){
        return "<!DOCTYPE html>\n<html>\n<head>\n<title>patch</title>\n<link rel=\"stylesheet\" href=\"patch.css\">\n</head>\n<body>\n" + body +
               scripts + "</body>\n</html>\n";
    }

    const char* const kScriptA =
        "<script type=\"text/lua\">\n"
        "  _G.runsA = (_G.runsA or 0) + 1\n"
        "  document:querySelector('.noid'):addEventListener('click', function() _G.noidClicks = (_G.noidClicks or 0) + 1 end)\n"
        "</script>\n";

    std::string scriptB(const std::string& tag){
        return "<script type=\"text/lua\">\n  _G.runsB = (_G.runsB or 0) + 1\n  _G.bTag = '" + tag + "'\n</script>\n";
    }

    const char* const kBodyV1 =
        "<div id=\"panel\" class=\"a\">\n"
        "  <p id=\"text\">Hello</p>\n"
        "  <div class=\"noid\">No id</div>\n"
        "  <input id=\"name\" type=\"text\">\n"
        "  <div id=\"list\"><p>1</p><p>2</p><p>3</p><p>4</p><p>5</p></div>\n"
        "  <div id=\"box\"></div>\n"
        "  <button id=\"remove-me\">X</button>\n"
        "</div>\n";

    const char* const kBodyV2 =
        "<div id=\"panel\" class=\"a b\">\n"
        "  <p id=\"text\">Hello world</p>\n"
        "  <div class=\"noid\">No id</div>\n"
        "  <button id=\"new\">New</button>\n"
        "  <input id=\"name\" type=\"text\">\n"
        "  <div id=\"list\"><p>1</p><p>2</p><p>3</p><p>4</p><p>5</p></div>\n"
        "  <div id=\"box\"></div>\n"
        "</div>\n";

    double luaNumber(const std::string& expression){
        lua_State* L = LuaBinding::getLuaState();
        const std::string code = "return " + expression;
        if (!L || luaL_loadstring(L, code.c_str()) != LUA_OK || LuaBinding::pcallWithTraceback(L, 0, 1) != LUA_OK){
            if (L)
                lua_pop(L, 1);
            return std::nan("");
        }
        const double value = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : (lua_toboolean(L, -1) ? 1.0 : 0.0);
        lua_pop(L, 1);
        return value;
    }

    std::string luaRun(const std::string& code){
        lua_State* L = LuaBinding::getLuaState();
        if (luaL_dostring(L, code.c_str()) != LUA_OK){
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    class TestDocumentReload : public ::testing::Test {
    protected:
        fs::path dir;

        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            ASSERT_EQ(HtmlUI::getReferenceCount(), 0) << "an earlier test left RmlUi initialised";
            TestSystem::get().clearLogs();
            const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
            dir = fs::path(JASPER_HTMLUI_TEST_BINARY_DIR) / "reload-patch" / info->name();
            fs::remove_all(dir);
            fs::create_directories(dir);
            write("patch.css", kPatchCss);
            luaRun("runsA, runsB, noidClicks, bTag, loads, reloads, lastReport, doc, handles = nil, nil, nil, nil, nil, nil, nil, nil, nil");
        }

        void TearDown() override{
            luaRun("doc, handles, lastReport, anim = nil, nil, nil, nil; collectgarbage()");
            EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
        }

        std::string path(const std::string& name) const{
            return (dir / name).generic_string();
        }

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

        // the document as `doc` in Lua, its scripts running, onLoad/onReload counted
        void start(SceneFixture& fixture, UIDocument& document){
            Scene& scene = fixture.getScene();
            updateDocuments(scene);
            ASSERT_TRUE(document.isLoaded());
            documentSystem(scene).startDocumentScripts();
            lua_State* L = LuaBinding::getLuaState();
            ASSERT_TRUE(luabridge::push(L, document));
            lua_setglobal(L, "doc");
            ASSERT_EQ(luaRun(R"(
                loads, reloads = 0, 0
                doc.onLoad = function() loads = loads + 1 end
                doc.onReload = function(report) reloads = reloads + 1; lastReport = report end
            )"), "");
            updateDocuments(scene);
        }

        UIReloadReport reload(SceneFixture& fixture, const UIDocument& document, const std::string& file){
            Scene& scene = fixture.getScene();
            documentSystem(scene).reloadChangedFiles({path(file)});
            updateDocuments(scene);
            updateDocuments(scene);
            return documentSystem(scene).getLastReloadReport(document.getEntity());
        }
    };

}

TEST_F(TestDocumentReload, PatchKeepsHandlesListenersAndState){
    write("patch.html", page(kBodyV1, std::string(kScriptA) + scriptB("one")));
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("patch.html"));
    start(fixture, document);
    ASSERT_EQ(luaNumber("runsA"), 1);
    ASSERT_EQ(luaNumber("runsB"), 1);

    // what a game does to the live UI
    ASSERT_EQ(luaRun(R"(
        handles = { text = doc:getElementById("text"), noid = doc:querySelector(".noid"), panel = doc:getElementById("panel") }
        handles.panel.classList:add("lua")
        handles.panel:setAttribute("data-x", "kept")
        local added = doc:createElement("span")
        added.id = "added"
        added.textContent = "from Lua"
        handles.panel:insertBefore(added, doc:getElementById("name"))
        doc:getElementById("name").value = "typed"
        anim = anime.animate("#box", { translateX = 200, duration = 10000, ease = "linear" })
    )"), "");
    UIElement input = document.getElementById("name");
    input.focus();
    UIElement list = document.getElementById("list");
    updateDocuments(scene);
    list.setScrollTop(30.0f);
    updateDocuments(scene);
    const double animatedBefore = luaNumber("anim.currentTime");
    const uint32_t loadsBefore = documentSystem(scene).getLoadCount(document.getEntity());

    write("patch.html", page(kBodyV2, std::string(kScriptA) + scriptB("two")));
    const UIReloadReport report = reload(fixture, document, "patch.html");

    EXPECT_EQ(report.mode, "patch") << report.fallbackReason;
    EXPECT_EQ(report.inserted, 1);
    EXPECT_EQ(report.removed, 1);
    EXPECT_EQ(report.replaced, 0);
    EXPECT_GE(report.updated, 2);
    EXPECT_GE(report.runtimeKept, 1);
    EXPECT_EQ(report.scriptsRerun, 1);
    EXPECT_EQ(documentSystem(scene).getLoadCount(document.getEntity()), loadsBefore + 1);

    // the same elements, updated where the author changed them
    EXPECT_EQ(luaRun(R"(
        assert(handles.text:isValid() and handles.text == doc:getElementById("text"))
        assert(handles.text.textContent == "Hello world", handles.text.textContent)
        assert(handles.panel == doc:getElementById("panel"))
        assert(handles.panel.classList:contains("a") and handles.panel.classList:contains("b"))
        assert(handles.panel.classList:contains("lua"), "a class Lua added stays")
        assert(handles.panel:getAttribute("data-x") == "kept")
        assert(doc:getElementById("added") and doc:getElementById("added").textContent == "from Lua", "the Lua node stays")
        -- after the authored node it followed: the new button goes after it
        assert(doc:getElementById("added").previousElementSibling == doc:querySelector(".noid"))
        assert(doc:getElementById("added").nextElementSibling == doc:getElementById("new"))
        assert(doc:getElementById("new") ~= nil)
        assert(doc:getElementById("remove-me") == nil)
        assert(doc:getElementById("name").value == "typed")
    )"), "");
    EXPECT_TRUE(input == document.getElementById("name"));
    EXPECT_TRUE(UIElementAccess::wrap(rmlContext(scene)->GetFocusElement()) == input) << "the focus stays";
    EXPECT_NEAR(list.getScrollTop(), 30.0f, 0.5f);

    // the unchanged script did not run again; its listener on an element without an id is alive
    EXPECT_EQ(luaNumber("runsA"), 1);
    EXPECT_EQ(luaNumber("runsB"), 2);
    EXPECT_EQ(luaRun("assert(bTag == 'two')"), "");
    luaRun("noidClicks = 0");
    document.querySelector(".noid").click();
    EXPECT_EQ(luaNumber("noidClicks"), 1);

    // onReload, not onLoad; the animation runs on
    EXPECT_EQ(luaNumber("reloads"), 1);
    EXPECT_EQ(luaNumber("loads"), 0);
    EXPECT_EQ(luaRun("assert(lastReport.mode == 'patch' and lastReport.inserted == 1)"), "");
    GpuEnvironment::drawFrames(3);
    EXPECT_EQ(luaRun("assert(not anim.completed and not anim.paused)"), "");
    EXPECT_GE(luaNumber("anim.currentTime"), animatedBefore);

    LuaBinding::cleanupLuaScripts(&scene);
    luaRun("runsA, runsB, noidClicks, bTag, loads, reloads = nil, nil, nil, nil, nil, nil; collectgarbage()");
}

TEST_F(TestDocumentReload, PatchDataForReplacesOnlyThatSubtree){
    const std::string modelBody =
        "<div data-model=\"shop\">\n"
        "  <p id=\"gold\">Gold: {{gold}}</p>\n"
        "  <ul id=\"items\"><li data-for=\"item : items\">{{item}}</li></ul>\n"
        "</div>\n";
    write("model.html", page(modelBody, ""));
    ASSERT_EQ(luaRun(R"(handles = nil)"), "");
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("model.html"));
    UIValue items = UIValue::array();
    items.push(UIValue("Sword"));
    items.push(UIValue("Shield"));
    UIValue initial = UIValue::object();
    initial["gold"] = UIValue(5);
    initial["items"] = items;
    updateDocuments(scene);
    UIDataModel model = document.createDataModel("shop", initial);
    updateDocuments(scene);
    updateDocuments(scene);
    UIElement gold = document.getElementById("gold");
    UIElement list = document.getElementById("items");
    ASSERT_EQ(gold.getTextContent(), "Gold: 5");
    ASSERT_EQ(list.getChildren().size(), 2u);

    write("model.html", page(
        "<div data-model=\"shop\">\n"
        "  <p id=\"gold\">Gold: {{gold}}</p>\n"
        "  <ul id=\"items\"><li data-for=\"item : items\"><b>{{item}}</b></li></ul>\n"
        "</div>\n", ""));
    const UIReloadReport report = reload(fixture, document, "model.html");
    EXPECT_EQ(report.mode, "patch") << report.fallbackReason;
    EXPECT_EQ(report.replaced, 1);
    ASSERT_EQ(report.causes.size(), 1u);
    EXPECT_NE(report.causes[0].find("data-for"), std::string::npos);
    EXPECT_TRUE(gold.isValid() && gold == document.getElementById("gold"));
    EXPECT_TRUE(list.isValid() && list == document.getElementById("items"));
    const std::vector<UIElement> rows = list.getChildren();
    ASSERT_EQ(rows.size(), 2u) << "the old copies went with their template";
    EXPECT_TRUE(rows[0].querySelector("b").isValid());
    EXPECT_EQ(rows[1].getTextContent(), "Shield");
    model.set("gold", UIValue(7));
    updateDocuments(scene);
    EXPECT_EQ(gold.getTextContent(), "Gold: 7");
}

TEST_F(TestDocumentReload, PatchBrokenEditKeepsTheLiveUi){
    write("broken.html", page(kBodyV1, kScriptA));
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("broken.html"));
    start(fixture, document);
    UIElement text = document.getElementById("text");
    ASSERT_TRUE(text.isValid());

    // a script that does not compile and markup the parser recovers from
    write("broken.html", page(std::string(kBodyV1) + "<div class=\"oops\"><p>unclosed</div>\n",
                              std::string(kScriptA) + "<script type=\"text/lua\">\n  this is not lua (\n</script>\n"));
    const UIReloadReport report = reload(fixture, document, "broken.html");
    EXPECT_EQ(report.mode, "patch");
    EXPECT_TRUE(text.isValid() && text == document.getElementById("text")) << "the live UI stays";
    EXPECT_EQ(luaNumber("runsA"), 1);
    bool scriptError = false;
    for (const htmlui::compat::Diagnostic& diagnostic : document.getDiagnostics())
        scriptError |= diagnostic.code == htmlui::compat::codes::LuaScriptError;
    EXPECT_TRUE(scriptError) << "the error is reported";
    EXPECT_GE(TestSystem::get().countLogs(D_LOG_ERROR, "Lua script error"), 1u);
    TestSystem::get().clearLogs();
    LuaBinding::cleanupLuaScripts(&scene);
    luaRun("runsA, noidClicks, loads, reloads = nil, nil, nil, nil; collectgarbage()");
}

TEST_F(TestDocumentReload, PatchRebuildModeLoadsTheDocumentAgain){
    write("rebuild.html", page(kBodyV1, kScriptA));
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument(path("rebuild.html"));
    start(fixture, document);
    UIElement text = document.getElementById("text");

    // asked by a script
    ASSERT_EQ(luaRun(R"(doc:reload{ mode = "rebuild" })"), "");
    updateDocuments(scene);
    updateDocuments(scene);
    EXPECT_EQ(documentSystem(scene).getLastReloadReport(document.getEntity()).mode, "rebuild");
    EXPECT_FALSE(text.isValid()) << "a rebuild makes new elements";
    EXPECT_EQ(luaNumber("runsA"), 2) << "and runs the scripts again";
    EXPECT_EQ(luaNumber("loads"), 1);
    EXPECT_EQ(luaNumber("reloads"), 0);

    // the system's mode (the editor preference)
    documentSystem(scene).setReloadMode(UIReloadMode::REBUILD);
    text = document.getElementById("text");
    write("rebuild.html", page(kBodyV2, kScriptA));
    const UIReloadReport report = reload(fixture, document, "rebuild.html");
    EXPECT_EQ(report.mode, "rebuild");
    EXPECT_FALSE(text.isValid());
    EXPECT_TRUE(document.getElementById("new").isValid());
    LuaBinding::cleanupLuaScripts(&scene);
    luaRun("runsA, noidClicks, loads, reloads = nil, nil, nil, nil; collectgarbage()");
}
