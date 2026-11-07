// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The virtual views in documents (S17 R1-R13, AC2-AC6): the pool bound and no element created
// while scrolling, scroll anchoring of measured rows, data-model binding of rows, keyboard and
// gamepad navigation with selection, the tree, the table's sort and resize, and the grid's lanes.

#include "../TestScriptCommon.h"

#include "Input.h"
#include "object/ui/UIItemsView.h"
#include "htmlui/widgets/HeightIndex.h"
#include "htmlui/widgets/ItemsViewElement.h"
#include "htmlui/widgets/WidgetsRuntime.h"
#include "HtmlUIClock.h"
#include <filesystem>
#include "htmlui/compat/DocumentLoader.h"
#include <RmlUi/Core/ElementDocument.h>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Input.h>

#include <cmath>
#include <set>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class HtmlUIWidgets : public ScriptTest {
    protected:
        const char* documentFile() const override { return ""; }

        void TearDown() override{
            htmlui::ItemsViewElement::resetForkFeatures();
            ScriptTest::TearDown();
        }

        // `inv`: items[k] = {id = "id<k>", name = "Item <k>"} (k 0-based), pick(i) records i
        void makeInventory(int count){
            ASSERT_EQ(lua(R"(
                picked = nil
                local items = {}
                for k = 0, )" + std::to_string(count - 1) + R"( do items[k + 1] = { id = "id" .. k, name = "Item " .. k } end
                model = doc:createDataModel("inv", { items = items, pick = function(event, i) picked = i end })
            )"), "");
        }

        void load(const char* file){
            ASSERT_TRUE(document->load(file));
            update();
            update();
        }

        UIItemsView view(const char* id){
            return document->getElementById(id).getItemsView();
        }

        // the text of the row showing item `index` (1-based), "" when it is not in the window
        std::string rowText(UIItemsView& items, int index, const char* selector = ".name"){
            UIElement row = items.getRow(index);
            if (!row.isValid())
                return std::string();
            UIElement part = row.querySelector(selector);
            return part.isValid() ? part.getTextContent() : row.getTextContent();
        }

        void key(Rml::Input::KeyIdentifier identifier, int modifiers = 0){
            context()->ProcessKeyDown(identifier, modifiers);
            context()->ProcessKeyUp(identifier, modifiers);
            update();
        }

        Rml::Element* rml(const UIElement& element){
            return UIElementAccess::get(element);
        }
    };

}

// AC2: a 10k list in a 600 px view of 48 px rows pools at most 13 + 8 + 8 rows; scrolling creates
// no element once warm, and the visible range is HeightIndex's
TEST_F(HtmlUIWidgets, ListPoolBound){
    makeInventory(10000);
    load("s17/list.html");
    UIItemsView list = view("list");
    ASSERT_TRUE(list.isValid());
    UIItemsViewState state = list.getState();
    EXPECT_EQ(state.kind, "list");
    EXPECT_EQ(state.count, 10000);
    EXPECT_EQ(state.source, "model:inv");
    EXPECT_LE(state.pool, 13 + 8 + 8);
    EXPECT_GT(state.pool, 13);
    EXPECT_EQ(state.poolBound, 13 + 1 + 8 + 8);   // (the bound counts a line cut at each end)
    EXPECT_FLOAT_EQ(state.totalHeight, 10000 * 48.0f);
    EXPECT_EQ(rowText(list, 1), "Item 0");
    EXPECT_EQ(rowText(list, 2, ".n"), "1");

    // warm up, then every 97th item: no element is made
    for (int index : {5000, 120, 9990}){
        list.scrollToIndex(index, "start");
        update();
    }
    htmlui::HeightIndex heights;
    heights.reset(10000, 48.0f);
    const uint64_t before = htmlui::WidgetsRuntime::elementsCreated();
    for (int index = 1; index <= 10000; index += 97){
        list.scrollToIndex(index, "start");
        update();
        state = list.getState();
        ASSERT_LE(state.pool, 13 + 8 + 8) << index;
        const float top = state.scrollTop;
        EXPECT_EQ(state.first, heights.indexAt(top) + 1) << index;
        EXPECT_EQ(state.last, heights.indexAt(top + 600.0f - 0.5f) + 1) << index;
        EXPECT_EQ(rowText(list, index), "Item " + std::to_string(index - 1)) << index;
    }
    EXPECT_EQ(htmlui::WidgetsRuntime::elementsCreated(), before) << "scrolling made elements";
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}

// AC3: rows above the first visible one grow; it keeps its place on screen
TEST_F(HtmlUIWidgets, AutoHeightAnchor){
    ASSERT_TRUE(document->load("s17/provider.html"));
    update();
    ASSERT_EQ(lua(R"(
        lines = {}
        for i = 1, 400 do lines[i] = 1 + (i % 3) end
        local v = doc:getElementById("log").itemsView
        v:setProvider{ count = #lines,
            bind = function(row, i)
                local parts = {}
                for k = 1, lines[i] do parts[k] = "line " .. k .. " of message " .. i end
                row:querySelector(".text").innerHTML = table.concat(parts, "<br/>")
            end }
    )"), "");
    update();
    update();
    UIItemsView log = view("log");
    log.scrollToIndex(200, "start");
    for (int i = 0; i < 4; i++)
        update();
    UIItemsViewState state = log.getState();
    const int first = state.first;
    ASSERT_GT(first, 10);
    UIElement row = log.getRow(first);
    ASSERT_TRUE(row.isValid());
    const float y = row.getBoundingClientRect().y;

    // the overscan rows above it (measured) get taller
    std::vector<int> above;
    for (int i = first - 3; i < first; i++)
        above.push_back(i);
    std::string grow;
    for (int i : above)
        grow += "lines[" + std::to_string(i) + "] = 6; ";
    ASSERT_EQ(lua(grow), "");
    log.refreshItems(above);
    for (int i = 0; i < 4; i++)
        update();
    UIElement same = log.getRow(first);
    ASSERT_TRUE(same.isValid());
    EXPECT_NEAR(same.getBoundingClientRect().y, y, 0.5f);
    EXPECT_GT(log.getState().totalHeight, state.totalHeight);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}

// AC4: one entry's change updates its row; the count follows the array; with P7-D no element is
// made by a rebind
TEST_F(HtmlUIWidgets, ModelBinding){
    makeInventory(10000);
    load("s17/list.html");
    UIItemsView list = view("list");
    EXPECT_TRUE(list.getState().rowAliases);
    list.scrollToIndex(5001, "center");
    update();
    ASSERT_EQ(rowText(list, 5001), "Item 5000");
    const std::string neighbour = rowText(list, 5002);

    const uint64_t before = htmlui::WidgetsRuntime::elementsCreated();
    EXPECT_LUA(R"(model:set("items[5000].name", "X"))");
    update();
    EXPECT_EQ(rowText(list, 5001), "X");
    EXPECT_EQ(rowText(list, 5002), neighbour);
    EXPECT_EQ(htmlui::WidgetsRuntime::elementsCreated(), before);

    // grow and shrink: the count and the sizer
    EXPECT_LUA(R"(for k = 1, 10 do model.items[#model.items + 1] = { id = "new" .. k, name = "New " .. k } end)");
    update();
    EXPECT_EQ(list.getCount(), 10010);
    EXPECT_FLOAT_EQ(list.getState().totalHeight, 10010 * 48.0f);
    EXPECT_LUA(R"(model.items = { { id = "a", name = "A" }, { id = "b", name = "B" } })");
    update();
    update();
    EXPECT_EQ(list.getCount(), 2);
    EXPECT_FLOAT_EQ(list.getState().totalHeight, 2 * 48.0f);
    EXPECT_EQ(rowText(list, 2), "B");
    EXPECT_FALSE(list.getRow(3).isValid());

    // data-event-click="pick(i)" on the template's element: the row's 0-based index
    UIElement row = list.getRow(2);
    ASSERT_TRUE(row.isValid());
    row.querySelector(".row").click();
    update();
    EXPECT_EQ(luaString("picked"), "1");
    EXPECT_EQ(list.getSelection(), std::vector<int>{2});
    EXPECT_EQ(list.getSelectedKeys(), std::vector<std::string>{"b"});
}

// Without the fork's row aliases the rows are made again for another item: correct, slower (R35)
TEST_F(HtmlUIWidgets, ModelBindingWithoutRowAliases){
    htmlui::ItemsViewElement::setForkFeatures(false, false);
    makeInventory(2000);
    load("s17/list.html");
    UIItemsView list = view("list");
    EXPECT_FALSE(list.getState().rowAliases);
    EXPECT_FALSE(list.getState().layoutBoundary);
    list.scrollToIndex(1500, "start");
    update();
    EXPECT_EQ(rowText(list, 1500), "Item 1499");
    EXPECT_LUA(R"(model:set("items[1499].name", "Y"))");
    update();
    EXPECT_EQ(rowText(list, 1500), "Y");
}

// AC5: keyboard selection and the active item
TEST_F(HtmlUIWidgets, Keyboard){
    makeInventory(10000);
    load("s17/list.html");
    UIItemsView list = view("list");
    std::vector<std::vector<int>> selections;
    document->getElementById("list").addEventListener("select", [&](UIEvent& event){
        std::vector<int> indices;
        const UIValue detail = event.getDetail();
        if (const UIValue* values = detail.find("indices")){
            for (const UIValue& value : HtmlUIScriptAccess::items(*values))
                indices.push_back((int)value.asNumber());
        }
        selections.push_back(indices);
    });
    document->getElementById("list").focus();
    update();

    key(Rml::Input::KI_DOWN);
    key(Rml::Input::KI_DOWN);
    key(Rml::Input::KI_DOWN);
    EXPECT_EQ(list.getActiveIndex(), 3);
    EXPECT_EQ(list.getSelection(), std::vector<int>{3});
    // the focus is on the active row
    EXPECT_TRUE(UIElementAccess::wrap(context()->GetFocusElement()) == list.getRow(3));

    key(Rml::Input::KI_NEXT);   // one viewport: 600 px below row 3's top
    EXPECT_EQ(list.getActiveIndex(), 3 + 12);
    key(Rml::Input::KI_END);
    EXPECT_EQ(list.getActiveIndex(), 10000);
    EXPECT_EQ(list.getState().last, 10000);
    key(Rml::Input::KI_UP, Rml::Input::KM_SHIFT);
    EXPECT_EQ(list.getActiveIndex(), 9999);
    EXPECT_EQ(list.getSelection(), (std::vector<int>{9999, 10000}));
    key(Rml::Input::KI_A, Rml::Input::KM_CTRL);
    EXPECT_EQ(list.getSelection().size(), 10000u);
    ASSERT_FALSE(selections.empty());
    EXPECT_EQ(selections.back().size(), 10000u);

    // Home, then Up at the first item is not the view's
    key(Rml::Input::KI_HOME);
    EXPECT_EQ(list.getActiveIndex(), 1);
    std::vector<int> activated;
    document->getElementById("list").addEventListener("activate", [&](UIEvent& event){
        activated.push_back((int)event.getDetail().find("index")->asNumber());
    });
    key(Rml::Input::KI_RETURN);
    EXPECT_EQ(activated, std::vector<int>{1});
}

// AC5: the d-pad moves the active item; Up at the first one leaves the view (spatial navigation);
// A activates
TEST_F(HtmlUIWidgets, GamepadNavigation){
    makeInventory(100);
    load("s17/list.html");
    UIItemsView list = view("list");
    std::vector<int> activated;
    document->getElementById("list").addEventListener("activate", [&](UIEvent& event){
        activated.push_back((int)event.getDetail().find("index")->asNumber());
    });
    UIDocumentSystem& system = documentSystem(scene());
    document->getElementById("list").focus();
    update();
    auto pad = [&](GamepadUIKey padKey){
        system.eventOnGamepadKeyDown(0, padKey, false);
        system.eventOnGamepadKeyUp(0, padKey);
        update();
    };
    pad(GamepadUIKey::DPAD_DOWN);
    pad(GamepadUIKey::DPAD_DOWN);
    EXPECT_EQ(list.getActiveIndex(), 2);
    pad(GamepadUIKey::A);
    EXPECT_EQ(activated, std::vector<int>{2});
    pad(GamepadUIKey::DPAD_UP);
    EXPECT_EQ(list.getActiveIndex(), 1);
    // at the edge: the element above takes the focus
    pad(GamepadUIKey::DPAD_UP);
    EXPECT_EQ(list.getActiveIndex(), 1);
    EXPECT_EQ(UIElementAccess::wrap(context()->GetFocusElement()).getId(), "above");
}

// AC6: the tree's expansion with the keys and from Lua, its events and aliases
TEST_F(HtmlUIWidgets, Tree){
    ASSERT_EQ(lua(R"(
        model = doc:createDataModel("quests", { roots = {
            { id = "main", title = "Main", children = {
                { id = "m1", title = "Find the key", children = { { id = "m1a", title = "Ask the miller" } } },
                { id = "m2", title = "Open the door" } } },
            { id = "side", title = "Side", children = { { id = "s1", title = "Fish" } } },
            { id = "done", title = "Done" } } })
        events = {}
    )"), "");
    load("s17/tree.html");
    UIItemsView tree = view("tree");
    UIElement element = document->getElementById("tree");
    std::vector<std::string> events;
    element.addEventListener("expand", [&](UIEvent& event){ events.push_back("expand " + event.getDetail().find("key")->asString()); });
    element.addEventListener("collapse", [&](UIEvent& event){ events.push_back("collapse " + event.getDetail().find("key")->asString()); });
    EXPECT_EQ(tree.getCount(), 3);
    EXPECT_EQ(rowText(tree, 1, ".label"), "Main");

    element.focus();
    update();
    key(Rml::Input::KI_DOWN);        // Main active
    key(Rml::Input::KI_RIGHT);       // expands Main
    EXPECT_EQ(tree.getCount(), 5);
    EXPECT_TRUE(tree.isExpanded("main"));
    key(Rml::Input::KI_RIGHT);       // to its first child
    EXPECT_EQ(tree.getActiveIndex(), 2);
    EXPECT_EQ(tree.getNode(2).key, "m1");
    EXPECT_EQ(tree.getNode(2).depth, 1);
    EXPECT_EQ(rowText(tree, 2, ".depth"), "1");
    key(Rml::Input::KI_RIGHT);       // expands m1
    EXPECT_EQ(tree.getCount(), 6);
    EXPECT_EQ(tree.getNode(3).key, "m1a");
    key(Rml::Input::KI_LEFT);        // collapses m1
    EXPECT_EQ(tree.getCount(), 5);
    key(Rml::Input::KI_LEFT);        // to its parent
    EXPECT_EQ(tree.getActiveIndex(), 1);
    key(Rml::Input::KI_LEFT);        // collapses Main
    EXPECT_EQ(tree.getCount(), 3);
    EXPECT_EQ(events, (std::vector<std::string>{"expand main", "expand m1", "collapse m1", "collapse main"}));

    EXPECT_LUA(R"(
        local v = doc:getElementById("tree").itemsView
        assert(v:expand("side"))
        assert(v.count == 4, v.count)
        assert(v:getNode(3).key == "s1" and v:getNode(3).parent == 2)
        assert(v:toggle("side") and v.count == 3)
    )");
}

// AC6: the header's sort event and classes; a resize by a pointer drag on a header edge widens the
// header cell and every pooled cell of the column equally
TEST_F(HtmlUIWidgets, Table){
    ASSERT_EQ(lua(R"(
        local servers = {}
        for k = 1, 500 do servers[k] = { id = k, name = "Server " .. k, ping = k % 90, map = "map" .. (k % 7) } end
        model = doc:createDataModel("browser", { servers = servers })
    )"), "");
    load("s17/table.html");
    UIItemsView table = view("servers");
    ASSERT_EQ(table.getColumns().size(), 3u);
    UIElement element = document->getElementById("servers");
    std::vector<std::string> sorts;
    element.addEventListener("sort", [&](UIEvent& event){
        sorts.push_back(event.getDetail().find("key")->asString() + " " + event.getDetail().find("direction")->asString());
    });
    UIElement row = table.getRow(1);
    ASSERT_TRUE(row.isValid());
    EXPECT_EQ(row.querySelector(".map").getTextContent(), "map1");

    Rml::Element* header = rml(element)->QuerySelector(".htmlui-header-cell[data-column=ping]");
    ASSERT_NE(header, nullptr);
    UIElementAccess::wrap(header).click();
    update();
    UIElementAccess::wrap(header).click();
    update();
    EXPECT_EQ(sorts, (std::vector<std::string>{"ping asc", "ping desc"}));
    EXPECT_TRUE(header->IsClassSet("sorted-desc"));
    EXPECT_FALSE(header->IsClassSet("sorted-asc"));
    EXPECT_EQ(table.getState().sortKey, "ping");

    // drag the Name column's edge 50 px to the right
    std::vector<float> resized;
    element.addEventListener("columnresize", [&](UIEvent& event){ resized.push_back((float)event.getDetail().find("width")->asNumber()); });
    Rml::Element* resizer = rml(element)->QuerySelector(".htmlui-header-cell[data-column=name] .htmlui-column-resizer");
    ASSERT_NE(resizer, nullptr);
    const Rml::Vector2f at = resizer->GetAbsoluteOffset(Rml::BoxArea::Border) + resizer->GetBox().GetSize() * 0.5f;
    context()->ProcessMouseMove((int)at.x, (int)at.y, 0);
    context()->ProcessMouseButtonDown(0, 0);
    for (int dx = 10; dx <= 50; dx += 10)
        context()->ProcessMouseMove((int)at.x + dx, (int)at.y, 0);
    context()->ProcessMouseButtonUp(0, 0);
    update();
    update();
    ASSERT_FALSE(resized.empty());
    EXPECT_NEAR(resized.back(), 250.0f, 0.5f);
    Rml::Element* nameHeader = rml(element)->QuerySelector(".htmlui-header-cell[data-column=name]");
    EXPECT_NEAR(nameHeader->GetBox().GetSize(Rml::BoxArea::Border).x, 250.0f, 0.5f);
    Rml::ElementList cells;
    rml(element)->QuerySelectorAll(cells, ".htmlui-cell[data-column=name]");
    ASSERT_GT(cells.size(), 10u);
    for (Rml::Element* cell : cells){
        if (cell->IsVisible(true)){
            EXPECT_NEAR(cell->GetBox().GetSize(Rml::BoxArea::Border).x, 250.0f, 0.5f);
        }
    }
    EXPECT_EQ(sorts.size(), 2u) << "a resize is no header click";
}

// AC6: a grid narrowed from 4 to 3 lanes keeps no stale cell
TEST_F(HtmlUIWidgets, Grid){
    ASSERT_EQ(lua(R"(
        local slots = {}
        for k = 0, 99 do slots[k + 1] = { name = "S" .. k } end
        model = doc:createDataModel("bag", { slots = slots })
    )"), "");
    load("s17/grid.html");
    UIItemsView grid = view("grid");
    UIElement element = document->getElementById("grid");
    // the vertical scroll bar takes its width from the 400 px: room for 3 cells, then 2
    const float scrollbar = 400.0f - element.getClientWidth();
    element.setStyle("width", Rml::ToString(400.0f + scrollbar) + "px");
    update();
    update();
    EXPECT_EQ(grid.getState().lanes, 4);
    element.setStyle("width", Rml::ToString(300.0f + scrollbar) + "px");
    update();
    update();
    const UIItemsViewState state = grid.getState();
    EXPECT_EQ(state.lanes, 3);
    // every shown cell is where its item goes, once
    std::set<int> items;
    Rml::ElementList rows;
    rml(element)->QuerySelectorAll(rows, ".htmlui-row");
    auto* view = rmlui_dynamic_cast<htmlui::ItemsViewElement*>(rml(element));
    for (Rml::Element* row : rows){
        if (!row->IsVisible(true))
            continue;
        const int item = view->itemOfElement(row);
        ASSERT_GE(item, 0);
        EXPECT_TRUE(items.insert(item).second) << "item " << item << " shown twice";
        EXPECT_NEAR(row->GetOffsetLeft(), (float)(item % 3) * 100.0f, 0.5f) << item;
        EXPECT_NEAR(row->GetOffsetTop(), (float)(item / 3) * 100.0f, 0.5f) << item;
        EXPECT_EQ(row->GetChild(0)->GetInnerRML(), "S" + std::to_string(item));
    }
    for (int item = state.first - 1; item < state.last; item++)
        EXPECT_TRUE(items.count(item)) << "item " << item << " is not shown";
}


// R12, R13: anchor="end" follows appended items only while at the end; a view without a definite
// height reports HTMLUI-HTML-0029 (and lays out 20 rows), a data-for of more than 500 elements
// HTMLUI-HTML-0031, each once per element
TEST_F(HtmlUIWidgets, AnchorEndAndRuntimeDiagnostics){
    ASSERT_EQ(lua(R"(
        local rows = {}
        for k = 1, 600 do rows[k] = k end
        doc:createDataModel("big", { rows = rows })
    )"), "");
    TestSystem::get().clearLogs();
    load("s17/diagnostics.html");
    ASSERT_EQ(lua(R"(
        doc:getElementById("unsized").itemsView:setProvider{ count = 1000, bind = function(row, i) row.textContent = "u" .. i end }
        doc:getElementById("chat").itemsView:setProvider{ count = 50, bind = function(row, i) row.textContent = "m" .. i end }
    )"), "");
    for (int i = 0; i < 3; i++)
        update();

    // the document's diagnostics (check --ui, inspect_ui), the warning also on the log
    Rml::ElementDocument* rmlDocument = UIElementAccess::get(document->getBody())->GetOwnerDocument();
    auto logsWith = [rmlDocument](const std::string& id){
        size_t count = 0;
        for (const htmlui::compat::Diagnostic& each : htmlui::compat::DocumentLoader::diagnosticsOf(rmlDocument))
            count += each.code.id() == id ? 1 : 0;
        return count;
    };
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTMLUI-HTML-0029]"), 1u);
    EXPECT_EQ(logsWith("HTMLUI-HTML-0029"), 1u);
    EXPECT_EQ(logsWith("HTMLUI-HTML-0031"), 1u);
    UIItemsView unsized = view("unsized");
    const std::pair<int, int> range = unsized.getVisibleRange();
    EXPECT_NEAR(range.second - range.first + 1, 20, 1) << range.first << ".." << range.second;

    // at the end: an append keeps the view at the end
    UIItemsView chat = view("chat");
    UIElement chatElement = document->getElementById("chat");
    chat.scrollToIndex(50, "end");
    for (int i = 0; i < 2; i++)
        update();
    const float maxBefore = chatElement.getScrollHeight() - chatElement.getClientHeight();
    EXPECT_NEAR(chatElement.getScrollTop(), maxBefore, 1.0f);
    chat.setCount(60);
    for (int i = 0; i < 3; i++)
        update();
    EXPECT_NEAR(chatElement.getScrollTop(), chatElement.getScrollHeight() - chatElement.getClientHeight(), 1.0f);
    EXPECT_EQ(chat.getVisibleRange().second, 60);

    // scrolled up: an append leaves it where it is
    chat.scrollToIndex(10, "start");
    for (int i = 0; i < 2; i++)
        update();
    const float top = chatElement.getScrollTop();
    chat.setCount(70);
    for (int i = 0; i < 3; i++)
        update();
    EXPECT_NEAR(chatElement.getScrollTop(), top, 0.5f);

    // reported once: more updates and scrolling add none
    unsized.scrollToIndex(500, "start");
    for (int i = 0; i < 3; i++)
        update();
    EXPECT_EQ(logsWith("HTMLUI-HTML-0029"), 1u);
    EXPECT_EQ(logsWith("HTMLUI-HTML-0031"), 1u);
}

// AC10: <markdown> from its raw content (an autolink survives, raw HTML is text and noted with
// HTMLUI-HTML-0032) and from src, data-markdown following its expression, el:setMarkdown and
// UIDocument.markdownToHtml
TEST_F(HtmlUIWidgets, MarkdownElements){
    ASSERT_EQ(lua(R"(chat = doc:createDataModel("chat", { message = "**hi**" }))"), "");
    load("s17/markdown.html");

    UIElement inlineMd = document->getElementById("inline");
    ASSERT_TRUE(inlineMd.isValid());
    UIElement body = inlineMd.querySelector(".markdown-body");
    ASSERT_TRUE(body.isValid()) << inlineMd.getInnerHTML();
    EXPECT_EQ(body.querySelector("h2").getTextContent(), "Notes");
    UIElement link = body.querySelector("a");
    ASSERT_TRUE(link.isValid()) << body.getInnerHTML();
    EXPECT_EQ(link.getAttribute("href"), "https://a.b");
    EXPECT_FALSE(body.querySelector("b").isValid()) << "raw HTML is not interpreted";
    EXPECT_NE(body.getTextContent().find("<b>raw</b>"), std::string::npos) << body.getTextContent();
    EXPECT_TRUE(body.querySelector("ul ul li").isValid()) << "nested list";
    Rml::ElementDocument* rmlDocument = UIElementAccess::get(document->getBody())->GetOwnerDocument();
    size_t rawNotes = 0;
    for (const htmlui::compat::Diagnostic& each : htmlui::compat::DocumentLoader::diagnosticsOf(rmlDocument))
        rawNotes += each.code.id() == "HTMLUI-HTML-0032" ? 1 : 0;
    EXPECT_EQ(rawNotes, 1u);

    UIElement file = document->getElementById("file");
    EXPECT_EQ(file.querySelector("h1").getTextContent(), "Journal");
    EXPECT_EQ(file.querySelector("em").getTextContent(), "one");

    UIElement bound = document->getElementById("bound");
    EXPECT_EQ(bound.querySelector("strong").getTextContent(), "hi") << bound.getInnerHTML();
    ASSERT_EQ(lua(R"(chat.message = "# Title")"), "");
    update();
    EXPECT_EQ(bound.querySelector("h1").getTextContent(), "Title") << bound.getInnerHTML();
    EXPECT_FALSE(bound.querySelector("strong").isValid());

    ASSERT_EQ(lua(R"(
        doc:getElementById("set"):setMarkdown("`code` <script>x()</script>")
        html = UIDocument.markdownToHtml("*a*")
    )"), "");
    update();
    UIElement set = document->getElementById("set");
    EXPECT_EQ(set.querySelector("code").getTextContent(), "code");
    EXPECT_FALSE(set.querySelector("script").isValid());
    EXPECT_NE(luaString("html").find("<em>a</em>"), std::string::npos) << luaString("html");
}

// The server_browser example's flow with a stand-in for its script (the MCP example test's steps):
// select a row, filter the model, select again, Down and Enter, the header's rich tooltip and a
// row's data-bound title
TEST_F(HtmlUIWidgets, ServerBrowserExampleFlow){
    namespace fs = std::filesystem;
    const fs::path library = fs::path(JASPER_HTMLUI_TEST_SOURCE_DIR) / ".." / ".." / ".." / "docs" / "mcp" / "guide" / "html-ui-examples";
    TestSystem::get().setAssetPath(library.lexically_normal().string());
    struct Restore { ~Restore(){ TestSystem::get().setAssetPath(""); } } restore;
    // the example's size (the Ping column is past 640 px; the next test's SetUp sets 640 x 360 again)
    GpuEnvironment::setView(1280, 720, 1280, 720, Scaling::FITWIDTH);
    HtmlUIClock::setFixedTime(100.0);
    ASSERT_EQ(lua(R"(
        all = {}
        local regions = { "EU", "NA", "Asia" }
        for i = 1, 300 do
            all[i] = { id = i, name = "Server " .. i, map = "Harbor", region = regions[i % 3 + 1], players = i % 9, max = 8,
                       ping = 12 + (i * 37) % 220 }
            all[i].tip = all[i].name .. ": " .. all[i].players .. " players"
        end
        local function visible(region)
            local list = {}
            for _, s in ipairs(all) do if region == "" or s.region == region then list[#list + 1] = s end end
            return list
        end
        joined = nil
        model = doc:createDataModel("browser", { servers = visible(""), shown = 300, region = "",
            selected = { id = 0, name = "", map = "", region = "", players = 0, max = 0, ping = 0, tip = "" },
            filter = function(event, region) model.region = region; model.servers = visible(region); model.shown = #model.servers end,
            join = function() end })
    )"), "");
    load("server_browser/server_browser.html");
    ASSERT_EQ(lua(R"(
        local t = doc:getElementById("servers")
        t:addEventListener("select", function(e) local id = tonumber(e.detail.keys[1]); model.selected = id and all[id] or model.selected end)
        t:addEventListener("activate", function(e) joined = tonumber(e.detail.key) end)
    )"), "");
    for (int i = 0; i < 3; i++)
        update();
    UIItemsView table = view("servers");
    ASSERT_EQ(table.getCount(), 300);
    auto clickRow = [&](int index){
        const UIRect rect = table.getRow(index).getBoundingClientRect();
        context()->ProcessMouseMove((int)(rect.x + 40.0f), (int)(rect.y + rect.height * 0.5f), 0);
        context()->ProcessMouseButtonDown(0, 0);
        context()->ProcessMouseButtonUp(0, 0);
        update();
        update();
    };
    clickRow(2);
    EXPECT_EQ(luaString("model.selected.id"), "2");
    // the EU chip
    UIElement chip = document->getElementById("region-eu");
    const UIRect chipRect = chip.getBoundingClientRect();
    context()->ProcessMouseMove((int)(chipRect.x + 5.0f), (int)(chipRect.y + 5.0f), 0);
    context()->ProcessMouseButtonDown(0, 0);
    context()->ProcessMouseButtonUp(0, 0);
    for (int i = 0; i < 3; i++)
        update();
    EXPECT_EQ(table.getCount(), 100);
    clickRow(2);
    key(Rml::Input::KI_DOWN);
    key(Rml::Input::KI_RETURN);
    EXPECT_EQ(table.getActiveIndex(), 3);
    EXPECT_NE(luaString("joined"), "nil");

    // the Ping header's rich tooltip, then a row's title
    // (the header's copy: the <table-column>'s own content is not displayed)
    UIElement head;
    for (const UIElement& each : document->querySelectorAll(".ping-head"))
        if (each.getBoundingClientRect().width > 0.0f)
            head = each;
    ASSERT_TRUE(head.isValid());
    const UIRect headRect = head.getBoundingClientRect();
    context()->ProcessMouseMove((int)(headRect.x + 3.0f), (int)(headRect.y + 3.0f), 0);
    for (double t = 100.1; t < 101.2; t += 0.1){
        HtmlUIClock::setFixedTime(t);
        update();
    }
    EXPECT_TRUE(document->getTooltip().visible);
    EXPECT_NE(document->getTooltip().text.find("round trip"), std::string::npos) << document->getTooltip().text;
    const UIRect nameRect = table.getRow(4).querySelector(".name").getBoundingClientRect();
    context()->ProcessMouseMove((int)(nameRect.x + 3.0f), (int)(nameRect.y + 3.0f), 0);
    for (double t = 101.3; t < 102.5; t += 0.1){
        HtmlUIClock::setFixedTime(t);
        update();
    }
    EXPECT_TRUE(document->getTooltip().visible);
    EXPECT_NE(document->getTooltip().text.find("players"), std::string::npos) << document->getTooltip().text;
    HtmlUIClock::reset();
}
