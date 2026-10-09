// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The element API on HTML documents (phase-2 §3.12.4, hook H5) through phase 1's Lua test
// harness: DOM names (tagName, document.body/documentElement), selectors through the CSS
// rewriter, hidden data-htmlui-* attributes and htmlui-* classes, innerHTML/outerHTML as HTML,
// template.content, dialogs, style setters, <script type="text/lua"> with `document` bound and
// errors at the HTML line.

#include "../TestScriptCommon.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/css/CssRewriter.h"
#include "htmlui/compat/html/HtmlTransform.h"

#include <RmlUi/Core/Element.h>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::htmlui::compat;

namespace {

    class CompatLuaTest : public ScriptTest {
    protected:
        const char* documentFile() const override { return "compat/lua/dom.html"; }
    };

    // the CSS rewriter maps names (p2-css); the seam passes selectors through
    bool rewriterMapsNames(){
        return CssRewriter::rewriteSelector("body", nullptr) != "body";
    }

}

TEST_F(CompatLuaTest, DomNames){
    EXPECT_LUA(R"(
        local box = doc:getElementById("box")
        assert(box.tagName == "DIV", box.tagName)
        assert(box.localName == "div")
        assert(doc.body.tagName == "BODY", doc.body.tagName)
        assert(doc.body.localName == "body")
        assert(doc.documentElement.tagName == "HTML", doc.documentElement.tagName)
        -- the author id of <html> is kept as data-html-id, the root's own id is not visible
        assert(doc.body.parentElement == doc.documentElement)
        assert(doc:getElementById("row").tagName == "TEMPLATE")
    )");
}

TEST_F(CompatLuaTest, SelectorsThroughTheRewriter){
    EXPECT_LUA(R"(
        assert(doc:querySelector("#box") == doc:getElementById("box"))
        assert(#doc:querySelectorAll(".panel") == 1)
        assert(doc:getElementById("box"):matches("div.panel"))
        assert(doc:getElementById("box"):closest(".page") ~= nil)
    )");
    if (rewriterMapsNames()){
        EXPECT_LUA(R"(
            assert(doc:querySelector("body") == doc.body)
            assert(doc:querySelector("template") == doc:getElementById("row"))
        )");
    }
}

TEST_F(CompatLuaTest, EngineAttributesAndClassesAreHidden){
    EXPECT_LUA(R"(
        local box = doc:getElementById("box")
        assert(box:getAttribute("data-htmlui-loc") == nil or box:getAttribute("data-htmlui-loc") == "")
        assert(not box:hasAttribute("data-htmlui-loc"))
        for name, _ in pairs(box:getAttributes()) do
            assert(not name:find("^data%-htmlui%-"), name)
        end
        -- tabindex gave it a hint class
        assert(box.className == "panel", box.className)
        assert(not box.classList:contains("htmlui-h0"))
        assert(box.dataset.kind == "main")
        assert(box.dataset.htmluiLoc == nil)
        assert(not box.outerHTML:find("htmlui"), box.outerHTML)
        assert(box.outerHTML:find('<div ', 1, true) == 1, box.outerHTML)
        -- setting the class list keeps the engine's classes
        box.className = "other"
        assert(box.className == "other")
        box:setAttribute("data-htmlui-loc", "7")
    )");
    Rml::Element* box = context()->GetDocument(0)->GetElementById("box");
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->IsClassSet("other"));
    EXPECT_TRUE(box->IsClassSet("htmlui-h0") || box->GetClassNames().find("htmlui-h") != std::string::npos) << box->GetClassNames();
    EXPECT_NE(box->GetAttribute<Rml::String>("data-htmlui-loc", ""), "7");
}

TEST_F(CompatLuaTest, InnerHtmlIsHtml){
    EXPECT_LUA(R"(
        local box = doc:getElementById("box")
        box.innerHTML = "a&nbsp;b<br><span class='s'>c</span><img src='img.png'>"
        assert(box.childElementCount == 3, box.childElementCount)
        local html = box.innerHTML
        assert(html:find("a\194\160b<br>", 1, true), html)
        assert(html:find('<span class="s">c</span>', 1, true), html)
        assert(not html:find("data-htmlui"), html)
        box:insertAdjacentHTML("beforeend", "<em>e</em>")
        box:insertAdjacentHTML("afterbegin", "<b>first</b>")
        assert(box.firstElementChild.tagName == "B")
        assert(box.lastElementChild.tagName == "EM")
        box.lastElementChild.outerHTML = "<strong>s</strong><i>i</i>"
        assert(box.lastElementChild.tagName == "I")
    )");
}

TEST_F(CompatLuaTest, TemplateContent){
    EXPECT_LUA(R"(
        local template = doc:getElementById("row")
        local list = doc:getElementById("list")
        for i = 1, 3 do
            local item = template.content:cloneNode(true)
            item:querySelector(".name").textContent = "item " .. i
            list:appendChild(item)
        end
        assert(list.childElementCount == 3, list.childElementCount)
        assert(list.firstElementChild.tagName == "LI")
        assert(list.lastElementChild:querySelector(".name").textContent == "item 3")
    )");
}

TEST_F(CompatLuaTest, DialogAndStyles){
    EXPECT_LUA(R"(
        local dialog = doc:getElementById("dialog")
        closedCount = 0
        dialog:addEventListener("close", function() closedCount = closedCount + 1 end)
        dialog:showModal()
        assert(dialog:hasAttribute("open"))
        dialog:close("ok")
        assert(not dialog:hasAttribute("open"))
    )");
    // HtmlDialogElement queues `close` to the next update, as browsers queue it
    update();
    EXPECT_EQ(luaString("closedCount"), "1");
    EXPECT_LUA("closedCount = nil");
    EXPECT_LUA(R"(
        local box = doc:getElementById("box")
        box.style.width = "50px"
        box.style.cssText = "height: 30px; width: 60px"
        assert(box.style.cssText:find("width: 60px", 1, true), box.style.cssText)
        assert(box.style.cssText:find("height: 30px", 1, true), box.style.cssText)
    )");
}

TEST_F(CompatLuaTest, DocumentScriptsRunInPlay){
    // edit mode: nothing runs
    EXPECT_EQ(luaString("doc:getElementById('out').textContent"), "not run");
    documentSystem(scene()).startDocumentScripts();
    EXPECT_EQ(luaString("doc:getElementById('out').textContent"), "ran table");
    // a fresh environment: the script's globals stay in it
    EXPECT_EQ(luaString("scriptGlobal"), "nil");
    // and again after every reload
    document->reload();
    EXPECT_EQ(luaString("doc:getElementById('out').textContent"), "ran table");
}

TEST_F(CompatLuaTest, ScriptErrorsAtTheHtmlLine){
    UIDocument& other = fixture->add<UIDocument>();
    documentSystem(scene()).startDocumentScripts();
    TestSystem::get().clearLogs();
    ASSERT_TRUE(other.load("compat/lua/error.html"));
    EXPECT_GE(TestSystem::get().countLogs(D_LOG_ERROR, "compat/lua/error.html:8:"), 1u);
    bool found = false;
    for (const Diagnostic& diagnostic : other.getDiagnostics()){
        if (diagnostic.code == codes::LuaScriptError){
            EXPECT_EQ(diagnostic.file, "compat/lua/error.html");
            EXPECT_EQ(diagnostic.line, 8u);
            EXPECT_NE(diagnostic.message.find("nil value"), std::string::npos) << diagnostic.message;
            found = true;
        }
    }
    EXPECT_TRUE(found);
    // the next script still ran
    EXPECT_EQ(other.getElementById("before").getTextContent(), "second ran");
}

TEST_F(CompatLuaTest, LucideIconSetAtRuntime){
    if (!lucideAvailable())
        GTEST_SKIP() << "this build has no Lucide icon pack (p2-runtime)";
    EXPECT_LUA(R"(
        local icon = doc:getElementById("icon")
        assert(icon.tagName == "svg", icon.tagName)
        icon:setAttribute("data-lucide", "pause")
        icon = doc:getElementById("icon")
        assert(icon:getAttribute("data-lucide") == "pause")
        assert(icon.classList:contains("lucide-pause"))
    )");
}

TEST_F(CompatLuaTest, Diagnostics){
    EXPECT_LUA(R"(
        local list = doc:getDiagnostics()
        assert(type(list) == "table")
        local email = false
        for _, d in ipairs(list) do
            assert(d.code:find("^HTMLUI%-"))
            assert(d.text:find(d.code, 1, true))
            if d.code == "HTMLUI-HTML-0008" then email = true end
        end
        assert(email, "the email input is reported")
        doc:setColorScheme("dark")
        doc:setReducedMotion(true)
    )");
    EXPECT_TRUE(context()->IsThemeActive("dark"));
    EXPECT_FALSE(context()->IsThemeActive("light"));
    EXPECT_TRUE(context()->IsThemeActive("reduced-motion"));
}
