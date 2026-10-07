// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Lua element API (phase 1 §3.9.5): every UIElement/UIDocument property and method,
// classList/style/dataset, listeners (duplicates, once, capture, removal, Lua refs), events,
// dead handles raising, the detached pool under the garbage collector.

#include "TestScriptCommon.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Input.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class ScriptLuaTest : public ScriptTest {};

}

TEST_F(ScriptLuaTest, ElementProperties){
    EXPECT_LUA(R"(
        local list = doc:getElementById("list")
        assert(list and list:isValid())
        assert(list.id == "list" and list.tagName == "div")
        assert(list.childElementCount == 3)
        local children = list.children
        assert(#children == 3 and children[1].id == "i1" and children[3].id == "i3")
        assert(list.firstElementChild.id == "i1" and list.lastElementChild.id == "i3")
        local i2 = doc:getElementById("i2")
        assert(i2.previousElementSibling.id == "i1" and i2.nextElementSibling.id == "i3")
        assert(i2.parentElement == list)
        assert(doc:getElementById("i3").nextElementSibling == nil)
        assert(doc:getElementById("missing") == nil)

        local outer = doc:getElementById("outer")
        assert(outer.className == "a b")
        outer.className = "x"
        assert(outer.className == "x")
        outer.id = "outer2"
        assert(doc:getElementById("outer2") == outer)

        local label = doc:getElementById("label")
        assert(label.textContent == "Button")
        label.textContent = "<i>Go</i>"
        assert(label.textContent == "<i>Go</i>")
        list.innerHTML = '<div id="n1">new</div>'
        assert(list.childElementCount == 1 and doc:getElementById("n1").textContent == "new")
        assert(string.find(list.innerHTML, "n1"))

        local text = doc:getElementById("text")
        assert(text.value == "hello")
        text.value = "typed"
        assert(text.value == "typed")
        local check = doc:getElementById("check")
        assert(check.checked == false)
        check.checked = true
        assert(check.checked == true)
        assert(text.disabled == false)
        text.disabled = true
        assert(text.disabled == true and text:hasAttribute("disabled"))

        local scroller = doc:getElementById("scroller")
        assert(scroller.clientHeight == 50 and scroller.scrollHeight >= 400)
        scroller.scrollTop = 100
        assert(scroller.scrollTop == 100)
        scroller.scrollLeft = 0
        assert(scroller.scrollLeft == 0 and scroller.scrollWidth > 0 and scroller.clientWidth > 0)

        local btn = doc:getElementById("btn")
        assert(btn.offsetLeft == 300 and btn.offsetTop == 20 and btn.offsetWidth == 100 and btn.offsetHeight == 40)
        local r = btn:getBoundingClientRect()
        assert(r.x == 300 and r.y == 20 and r.width == 100 and r.height == 40)
        assert(r.left == 300 and r.top == 20 and r.right == 400 and r.bottom == 60)
        assert(tostring(btn) == "UIElement(div#btn)")
    )");
}

TEST_F(ScriptLuaTest, ElementMethods){
    EXPECT_LUA(R"(
        local outer = doc:getElementById("outer")
        assert(outer:getAttribute("title") == "T")
        assert(outer:getAttribute("nope") == nil)
        outer:setAttribute("title", "U")
        assert(outer:hasAttribute("title") and outer:getAttribute("title") == "U")
        outer:removeAttribute("title")
        assert(not outer:hasAttribute("title"))
        local attributes = outer:getAttributes()
        assert(attributes.id == "outer" and attributes["data-user-name"] == "ann")

        assert(doc:querySelector(".item").id == "i1")
        assert(#doc:querySelectorAll(".item") == 3)
        assert(doc:querySelector(".none") == nil)
        assert(outer:querySelector("#i2").id == "i2")
        assert(#outer:querySelectorAll(".item") == 3)
        local i3 = doc:getElementById("i3")
        assert(i3:matches(".item") and not i3:matches("#btn"))
        assert(i3:closest(".item") == i3 and i3:closest("#outer") == outer and i3:closest("#btn") == nil)
        assert(outer:contains(i3) and not i3:contains(outer))

        local list = doc:getElementById("list")
        local made = doc:createElement("div")
        made.id = "made"
        assert(made.parentElement == nil)
        assert(list:appendChild(made) == made)
        assert(list.lastElementChild == made)
        list:insertBefore(made, doc:getElementById("i1"))
        assert(list.firstElementChild == made)
        list:insertBefore(made, nil)
        assert(list.lastElementChild == made)
        local removed = list:removeChild(doc:getElementById("i2"))
        assert(removed.id == "i2" and removed.parentElement == nil and removed:isValid())
        list:appendChild(removed)
        assert(list.lastElementChild.id == "i2")
        local old = list:replaceChild(doc:createElement("p"), doc:getElementById("i1"))
        assert(old.id == "i1" and list.firstElementChild.tagName == "p")
        made:remove()
        assert(doc:getElementById("made") == nil and made:isValid())
        local deep = outer:cloneNode(true)
        local shallow = outer:cloneNode()
        assert(deep.childElementCount > 0 and shallow.childElementCount == 0)
        local text = doc:createTextNode("words")
        assert(text.textContent == "words")

        local input = doc:getElementById("text")
        input:focus()
        assert(input:isPseudoClassSet("focus"))
        input:blur()
        local clicked = 0
        local btn = doc:getElementById("btn")
        btn:addEventListener("click", function() clicked = clicked + 1 end)
        btn:click()
        assert(clicked == 1)

        local scroller = doc:getElementById("scroller")
        scroller:scrollTo(0, 20)
        assert(scroller.scrollTop == 20)
        scroller:scrollBy(0, 5)
        assert(scroller.scrollTop == 25)
        doc:getElementById("tall"):scrollIntoView()

        btn:setClass("on", true)
        assert(btn:hasClass("on"))
        assert(btn:setStyleProperty("width", "110px"))
        btn:removeStyleProperty("width")
        assert(btn:getBoundingBox().width == 100)
    )");
}

TEST_F(ScriptLuaTest, ClassListStyleDataset){
    EXPECT_LUA(R"(
        local outer = doc:getElementById("outer")
        local classes = outer.classList
        classes:add("c", "d")
        assert(classes:contains("c") and classes:contains("d") and classes:contains("a"))
        classes:remove("a", "c")
        assert(not classes:contains("a") and not classes:contains("c"))
        assert(classes:toggle("e") == true and classes:contains("e"))
        assert(classes:toggle("e") == false and not classes:contains("e"))
        assert(classes:toggle("e", true) == true and classes:toggle("e", true) == true)
        assert(classes:toggle("e", false) == false)

        local btn = doc:getElementById("btn")
        assert(btn.style.width == "100px")
        assert(btn.style["width"] == "100px")
        btn.style.backgroundColor = "#ff0000"
        assert(btn.style["background-color"] ~= "")
        btn.style["width"] = "120px"
        assert(btn.style:getPropertyValue("width") == "120px")
        btn.style:setProperty("height", "10px")
        assert(btn.style.height == "10px")
        assert(btn.style:removeProperty("height") == "10px")
        assert(btn.style.height == "40px")
        btn.style.width = nil
        assert(btn.style.width == "100px")
        btn.style.width = "130px"
        btn.style.width = ""
        assert(btn.style.width == "100px")

        assert(outer.dataset.userName == "ann")
        assert(outer.dataset.missing == nil)
        outer.dataset.gameScore = 12
        assert(outer:getAttribute("data-game-score") == "12")
        outer.dataset.gameScore = nil
        assert(not outer:hasAttribute("data-game-score"))
    )");
}

TEST_F(ScriptLuaTest, DocumentApi){
    EXPECT_LUA(R"(
        assert(doc.loaded and doc:isLoaded())
        assert(doc.filename == "p1/script_dom.html")
        assert(doc.title == "script dom")
        doc.title = "T2"
        assert(doc.title == "T2")
        assert(doc.body ~= nil and doc.body.tagName == "body")
        assert(doc.visible)
        doc:hide()
        assert(not doc.visible)
        doc:show()
        doc.sortOrder = 3
        assert(doc.sortOrder == 3)
        assert(doc:getTargetTexture() == nil)
        UIDocument.setDebuggerVisible(false)
        assert(UIDocument.isDebuggerVisible() == false)
        local heard = 0
        local function onClick(event) heard = heard + 1 end
        doc:addEventListener("click", onClick)
        doc:getElementById("btn"):click()
        doc:removeEventListener("click", onClick)
        doc:getElementById("btn"):click()
        assert(heard == 1)
    )");
}

TEST_F(ScriptLuaTest, Listeners){
    EXPECT_LUA(R"(
        log = {}
        local list = doc:getElementById("list")
        local i1 = doc:getElementById("i1")
        local function bubble(e) log[#log + 1] = "bubble" .. e.eventPhase end
        local function capture(e) log[#log + 1] = "capture" .. e.eventPhase end
        local function target(e)
            log[#log + 1] = "target" .. e.eventPhase
            assert(e.type == "click" and e.target == i1 and e.currentTarget == i1)
        end
        list:addEventListener("click", bubble)
        list:addEventListener("click", bubble)          -- a duplicate: ignored
        list:addEventListener("click", capture, true)
        list:addEventListener("click", capture, {capture = true})   -- duplicate too
        i1:addEventListener("click", target)
        i1:click()
        assert(table.concat(log, ",") == "capture1,target2,bubble3", table.concat(log, ","))

        log = {}
        list:removeEventListener("click", bubble)
        list:removeEventListener("click", capture)       -- the bubble one: not there
        i1:click()
        assert(table.concat(log, ",") == "capture1,target2", table.concat(log, ","))
        list:removeEventListener("click", capture, true)

        -- once
        log = {}
        i1:addEventListener("dblclick", function() log[#log + 1] = "once" end, {once = true})
        i1:dispatchEvent("dblclick")
        i1:dispatchEvent("dblclick")
        assert(#log == 1)

        -- stopPropagation / stopImmediatePropagation
        log = {}
        list:addEventListener("ping", function() log[#log + 1] = "list" end)
        i1:addEventListener("ping", function(e) log[#log + 1] = "a"; e:stopPropagation() end)
        i1:addEventListener("ping", function(e) log[#log + 1] = "b" end)
        i1:dispatchEvent("ping")
        assert(table.concat(log, ",") == "a,b", table.concat(log, ","))
        log = {}
        local i2 = doc:getElementById("i2")
        i2:addEventListener("pong", function(e) log[#log + 1] = "a"; e:stopImmediatePropagation() end)
        i2:addEventListener("pong", function(e) log[#log + 1] = "b" end)
        i2:dispatchEvent("pong")
        assert(table.concat(log, ",") == "a")

        -- a listener removing itself while it runs
        local count = 0
        local function selfRemoving(e)
            count = count + 1
            i2:removeEventListener("tick", selfRemoving)
        end
        i2:addEventListener("tick", selfRemoving)
        i2:dispatchEvent("tick")
        i2:dispatchEvent("tick")
        assert(count == 1)
    )");
    EXPECT_GT(HtmlUIScriptState::liveLuaRefs(), 0u);

    // errors in a listener go to the Console with a traceback, the dispatch goes on
    TestSystem::get().clearLogs();
    EXPECT_LUA(R"(
        local btn = doc:getElementById("btn")
        btn:addEventListener("click", function() error("boom") end)
        btn:click()
    )");
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "boom"), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "stack traceback"), 1u);
    TestSystem::get().clearLogs();

    // bad arguments raise
    EXPECT_NE(lua(R"(doc:getElementById("btn"):addEventListener("click", 5))"), "");
    EXPECT_NE(lua(R"(doc:getElementById("btn"):addEventListener("click", function() end, "x"))"), "");
    // documents never get these: a warning, no error
    EXPECT_LUA(R"(doc:getElementById("btn"):addEventListener("mouseenter", function() end))");
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "'mouseenter'"), 1u);
    TestSystem::get().clearLogs();

    // unloading the document frees every listener and its Lua reference
    ASSERT_TRUE(document->load("ecs/basic.html"));
    update();
    EXPECT_EQ(HtmlUIScriptState::liveLuaRefs(), 0u);
}

TEST_F(ScriptLuaTest, EventFields){
    EXPECT_LUA(R"(
        result = {}
        local btn = doc:getElementById("btn")
        btn:addEventListener("mousedown", function(e)
            result[#result + 1] = e.button
            result.x, result.y = e.clientX, e.clientY
            result.ctrl, result.shift = e.ctrlKey, e.shiftKey
        end)
        local scroller = doc:getElementById("scroller")
        scroller:addEventListener("wheel", function(e) result.dy = e.deltaY; result.dx = e.deltaX end)
        local text = doc:getElementById("text")
        text:addEventListener("textinput", function(e) result.data = (result.data or "") .. e.data end)
        local list = doc:getElementById("list")
        list:addEventListener("custom", function(e)
            result.detail = e.detail
            assert(e.defaultPrevented == false)
            e:preventDefault()
            assert(e.defaultPrevented == true)
        end)
        result.notPrevented = list:dispatchEvent("custom", {points = 5, tags = {"a", "b"}})
        assert(result.notPrevented == false)
        assert(result.detail.points == 5 and result.detail.tags[2] == "b")
        assert(doc:getElementById("i1"):dispatchEvent("unheard") == true)
        local kept
        list:addEventListener("keep", function(e) kept = e end)
        list:dispatchEvent("keep")
        assert(kept.type == "" and kept.target == nil and not kept:isValid())
    )");

    context()->ProcessMouseMove(350, 40, Rml::Input::KM_CTRL);
    context()->ProcessMouseButtonDown(1, Rml::Input::KM_CTRL | Rml::Input::KM_SHIFT);
    context()->ProcessMouseButtonUp(1, 0);
    context()->ProcessMouseButtonDown(2, 0);
    context()->ProcessMouseButtonUp(2, 0);
    context()->ProcessMouseMove(50, 225, 0);
    context()->ProcessMouseWheel(Rml::Vector2f(1.0f, -2.0f), 0);
    document->getElementById("text").focus();
    context()->ProcessTextInput("hé");

    EXPECT_LUA(R"(
        assert(result[1] == 2 and result[2] == 1, tostring(result[1]) .. tostring(result[2]))
        assert(result.x == 350 and result.y == 40)
        assert(result.dx == 1 and result.dy == -2)
        assert(result.data == "hé")
    )");
}

TEST_F(ScriptLuaTest, InputAndChange){
    EXPECT_LUA(R"(
        result = {input = 0, change = 0, box = 0}
        local text = doc:getElementById("text")
        text:addEventListener("input", function(e) assert(e.type == "input"); result.input = result.input + 1 end)
        text:addEventListener("change", function(e) assert(e.type == "change"); result.change = result.change + 1 end)
        doc:getElementById("check"):addEventListener("change", function() result.box = result.box + 1 end)
        text:focus()
    )");
    context()->ProcessTextInput("xy");
    EXPECT_LUA(R"(
        assert(result.input > 0 and result.change == 0)
        doc:getElementById("text"):blur()
        assert(result.change == 1)
        doc:getElementById("check"):click()
        assert(result.box == 1)
    )");
}

TEST_F(ScriptLuaTest, DeadHandlesRaise){
    EXPECT_LUA(R"(
        el = doc:getElementById("btn")
        result = el.style
        model = el.classList
    )");
    ASSERT_TRUE(document->load("ecs/basic.html"));
    setDocumentGlobal();

    const std::string message = kHtmlUIDeadElementMessage;
    EXPECT_LUA("assert(el:isValid() == false)");
    EXPECT_EQ(lua("return el.id"), message);
    EXPECT_EQ(lua("el.id = 'x'"), message);
    EXPECT_EQ(lua("el:getAttribute('id')"), message);
    EXPECT_EQ(lua("el:appendChild(doc:getElementById('box'))"), message);
    EXPECT_EQ(lua("return result.width"), message);
    EXPECT_EQ(lua("result.width = '1px'"), message);
    EXPECT_EQ(lua("model:add('x')"), message);
    // equality and tostring still work
    EXPECT_LUA("assert(el == el and tostring(el) == 'UIElement(invalid)')");
}

TEST_F(ScriptLuaTest, DetachedPoolAndGarbageCollection){
    EXPECT_LUA(R"(
        local made = doc:createElement("div")
        made.id = "temp"
    )");
    EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 1u);
    update();
    EXPECT_LUA("collectgarbage(); collectgarbage()");
    update();
    EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 0u);

    // a removed child kept in a global survives updates and goes back in
    EXPECT_LUA(R"(el = doc:getElementById("list"):removeChild(doc:getElementById("i2")))");
    update();
    EXPECT_LUA("collectgarbage()");
    update();
    EXPECT_LUA(R"(
        assert(el:isValid() and el.textContent == "two")
        doc:getElementById("outer"):appendChild(el)
        assert(doc:getElementById("i2") == el)
    )");
}

TEST_F(ScriptLuaTest, LoadAndUnloadNotifications){
    EXPECT_LUA(R"(
        result = {loads = 0, unloads = 0}
        doc.onLoad = function() result.loads = result.loads + 1 end
        doc.onUnload = function() result.unloads = result.unloads + 1 end
    )");
    ASSERT_TRUE(document->load("ecs/basic.html"));
    update();
    EXPECT_LUA("assert(result.loads == 1 and result.unloads == 1, result.loads .. ' ' .. result.unloads)");
    document->reload();
    update();
    EXPECT_LUA("assert(result.loads == 2 and result.unloads == 2)");
}

TEST_F(ScriptLuaTest, ListenersMoveOnReload){
    // what p1-editor's full reload does: the new document is loaded while the old one lives,
    // then the listeners of id-ed elements (and of the document) move
    EXPECT_LUA(R"(
        result = {btn = 0, doc = 0, anon = 0}
        doc:getElementById("btn"):addEventListener("click", function() result.btn = result.btn + 1 end)
        doc:addEventListener("click", function() result.doc = result.doc + 1 end)
        doc:getElementById("label").parentElement.firstElementChild:addEventListener("click", function() end)
        doc:querySelector(".item"):addEventListener("click", function() result.anon = result.anon + 1 end)
        doc:getElementById("i1"):removeAttribute("id")
    )");

    Rml::ElementDocument* oldDocument = documentSystem(scene()).getDocument(document->getEntity());
    ASSERT_NE(oldDocument, nullptr);
    Rml::ElementDocument* newDocument = context()->LoadDocument("p1/script_dom.html");
    ASSERT_NE(newDocument, nullptr);

    std::unordered_map<Rml::Element*, Rml::Element*> oldToNew;
    oldToNew[oldDocument] = newDocument;
    for (const char* id : {"btn", "label", "list", "outer"}){
        oldToNew[oldDocument->GetElementById(id)] = newDocument->GetElementById(id);
    }
    const size_t refs = HtmlUIScriptState::liveLuaRefs();
    TestSystem::get().clearLogs();
    documentSystem(scene()).getScriptState().onDocumentReloaded(document->getEntity(), oldToNew);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "1 event listener(s) on elements without an id were dropped"), 1u);
    EXPECT_EQ(HtmlUIScriptState::liveLuaRefs(), refs - 1);
    TestSystem::get().clearLogs();

    UIElementAccess::wrap(newDocument->GetElementById("btn")).click();
    UIElementAccess::wrap(oldDocument->GetElementById("btn")).click();
    EXPECT_LUA("assert(result.btn == 1 and result.doc == 1, result.btn .. ' ' .. result.doc)");
    context()->UnloadDocument(newDocument);
    update();
}

TEST_F(ScriptLuaTest, LuaRefsReleasedWithTheScene){
    EXPECT_LUA(R"(
        local btn = doc:getElementById("btn")
        btn:addEventListener("click", function() end)
        btn:addEventListener("keydown", function() end, true)
        doc:addEventListener("load", function() end)
        doc:createElement("div"):addEventListener("click", function() end)
    )");
    EXPECT_EQ(HtmlUIScriptState::liveLuaRefs(), 4u);
    // TearDown destroys the scene and checks liveLuaRefs() == 0
}

TEST_F(ScriptLuaTest, LuaFunctionBaseReleasesItsReference){
    // FunctionSubscribe's Lua functions (doc.onLoad) unref their registry slot when destroyed
    auto registryFunctions = [this](){
        size_t count = 0;
        lua_pushnil(L);
        while (lua_next(L, LUA_REGISTRYINDEX) != 0){
            if (lua_isfunction(L, -1))
                count++;
            lua_pop(L, 1);
        }
        return count;
    };
    const size_t before = registryFunctions();
    EXPECT_LUA("doc.onLoad = function() end");
    EXPECT_GT(registryFunctions(), before);
    EXPECT_TRUE(document->getComponent<UIDocumentComponent>().onLoad.remove("luaFunction"));
    EXPECT_EQ(registryFunctions(), before);
}
