// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// HTML5 drag and drop (S17 R18-R23, AC9): the event order of a pointer drag, a cancelled one and
// one released over a target that does not accept, typed payloads by identity, dropzone, the
// feedback classes and the ghost, UIDrag's programmatic drags, and drops inside a virtual view.

#include "../TestScriptCommon.h"

#include "HtmlUIClock.h"
#include "htmlui/widgets/DragDropSession.h"
#include "object/ui/UIItemsView.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // the listeners every test uses: `log` gets "<event> <id>" and the dropEffect at dragend
    const char* const kListeners = R"(
        log = {}
        dragSeen = false
        dropped = nil
        item = { kind = "weapon", name = "Sword" }
        local function record(name)
            return function(e)
                local id = e.currentTarget.id
                if name == "dragend" then id = id .. " " .. e.dataTransfer.dropEffect end
                log[#log + 1] = name .. " " .. id
            end
        end
        local src = doc:getElementById("src")
        src:addEventListener("dragstart", function(e)
            log[#log + 1] = "dragstart src"
            e.dataTransfer:setData("item", item)
            e.dataTransfer.effectAllowed = "move"
        end)
        -- drag repeats with every move: logged once
        src:addEventListener("drag", function(e) if not dragSeen then dragSeen = true; log[#log + 1] = "drag src" end end)
        src:addEventListener("dragend", record("dragend"))
        for _, id in ipairs({ "nope", "bag", "trash" }) do
            local target = doc:getElementById(id)
            target:addEventListener("dragenter", record("dragenter"))
            target:addEventListener("dragleave", record("dragleave"))
            target:addEventListener("dragover", function(e)
                if log[#log] ~= "dragover " .. id then log[#log + 1] = "dragover " .. id end
                if id == "trash" and e.dataTransfer:getData("item").kind == "weapon" then e:preventDefault() end
            end)
            target:addEventListener("drop", function(e)
                log[#log + 1] = "drop " .. id
                dropped = e.dataTransfer:getData("item")
            end)
        end
    )";

    class TestScriptEvents : public ScriptTest {
    protected:
        const char* documentFile() const override { return ""; }

        void SetUp() override{
            ScriptTest::SetUp();
            ASSERT_EQ(lua(R"(
                local items = {}
                for k = 0, 99 do items[k + 1] = { id = "s" .. k, name = "Slot " .. k } end
                model = doc:createDataModel("inv", { items = items })
            )"), "");
            ASSERT_TRUE(document->load("s17/drag.html"));
            update();
            update();
            ASSERT_EQ(lua(kListeners), "");
        }

        Rml::Vector2f centerOf(const char* id){
            const UIRect rect = document->getElementById(id).getBoundingClientRect();
            return Rml::Vector2f(rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f);
        }

        void move(Rml::Vector2f point){
            context()->ProcessMouseMove((int)point.x, (int)point.y, 0);
            update();
        }

        // pressed on src and moved a little: the drag has started
        void startDrag(){
            const Rml::Vector2f from = centerOf("src");
            context()->ProcessMouseMove((int)from.x, (int)from.y, 0);
            context()->ProcessMouseButtonDown(0, 0);
            move(from + Rml::Vector2f(6.0f, 0.0f));
        }

        std::vector<std::string> log(){
            std::vector<std::string> entries;
            lua_getglobal(L, "log");
            const int length = (int)lua_rawlen(L, -1);
            for (int i = 1; i <= length; i++){
                lua_rawgeti(L, -1, i);
                entries.push_back(lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            return entries;
        }

        Rml::Element* rml(const char* id){
            return UIElementAccess::get(document->getElementById(id));
        }
    };

}

TEST_F(TestScriptEvents, DataTransferOrderOfADrop){
    startDrag();
    // the source and the document know
    EXPECT_TRUE(rml("src")->IsClassSet("htmlui-drag-source"));
    Rml::Element* body = UIElementAccess::get(document->getBody());
    EXPECT_TRUE(body->IsClassSet("htmlui-dragging"));
    move(centerOf("nope"));
    move(centerOf("nope") + Rml::Vector2f(3.0f, 0.0f));
    move(centerOf("bag"));
    EXPECT_TRUE(rml("bag")->IsClassSet("htmlui-drop-allowed")) << "dropzone=\"item move\"";
    // the ghost, a clone of the source, follows the pointer
    Rml::ElementList ghosts;
    body->QuerySelectorAll(ghosts, ".htmlui-drag-ghost");
    ASSERT_EQ(ghosts.size(), 1u);
    EXPECT_EQ(ghosts[0]->GetInnerRML(), "Sword");
    context()->ProcessMouseButtonUp(0, 0);
    update();

    EXPECT_EQ(log(), (std::vector<std::string>{"dragstart src", "drag src", "dragenter nope", "dragover nope", "dragleave nope",
                                                "dragenter bag", "dragover bag", "drop bag", "dragend src move"}));
    EXPECT_LUA("assert(dropped == item, 'the table arrives by identity')");
    EXPECT_FALSE(rml("bag")->IsClassSet("htmlui-drop-allowed"));
    EXPECT_FALSE(rml("src")->IsClassSet("htmlui-drag-source"));
    EXPECT_FALSE(body->IsClassSet("htmlui-dragging"));
    ghosts.clear();
    body->QuerySelectorAll(ghosts, ".htmlui-drag-ghost");
    EXPECT_TRUE(ghosts.empty());
}

TEST_F(TestScriptEvents, DataTransferAcceptedByALuaListener){
    startDrag();
    move(centerOf("trash"));
    EXPECT_TRUE(rml("trash")->IsClassSet("htmlui-drop-allowed"));
    context()->ProcessMouseButtonUp(0, 0);
    update();
    EXPECT_EQ(log(), (std::vector<std::string>{"dragstart src", "drag src", "dragenter trash", "dragover trash", "drop trash",
                                                "dragend src move"}));
}

TEST_F(TestScriptEvents, DataTransferNotAcceptedEndsWithNone){
    startDrag();
    move(centerOf("nope"));
    EXPECT_TRUE(rml("nope")->IsClassSet("htmlui-drop-denied") || !rml("nope")->IsClassSet("htmlui-drop-allowed"));
    context()->ProcessMouseButtonUp(0, 0);
    update();
    EXPECT_EQ(log(), (std::vector<std::string>{"dragstart src", "drag src", "dragenter nope", "dragover nope", "dragleave nope",
                                                "dragend src none"}));
    EXPECT_LUA("assert(dropped == nil)");
}

TEST_F(TestScriptEvents, DataTransferEscapeCancels){
    startDrag();
    move(centerOf("bag"));
    context()->ProcessKeyDown(Rml::Input::KI_ESCAPE, 0);
    update();
    context()->ProcessMouseButtonUp(0, 0);
    update();
    EXPECT_EQ(log(), (std::vector<std::string>{"dragstart src", "drag src", "dragenter bag", "dragover bag", "dragleave bag",
                                                "dragend src none"}));
}

TEST_F(TestScriptEvents, DataTransferPreventedDragStartStartsNoDrag){
    EXPECT_LUA(R"(doc:getElementById("src"):addEventListener("dragstart", function(e) e:preventDefault() end))");
    startDrag();
    move(centerOf("bag"));
    context()->ProcessMouseButtonUp(0, 0);
    update();
    EXPECT_EQ(log(), std::vector<std::string>{"dragstart src"});
}

TEST_F(TestScriptEvents, DataTransferProgrammaticDrag){
    EXPECT_LUA(R"(
        local src, bag = doc:getElementById("src"), doc:getElementById("bag")
        assert(UIDrag.begin(src, { data = { other = 1 }, effectAllowed = "move" }))
        assert(UIDrag.isActive())
        assert(UIDrag.getDataTransfer():getData("other") == 1)
        assert(not UIDrag.over(doc:getElementById("nope")))
        assert(UIDrag.drop(bag))
        assert(not UIDrag.isActive())
    )");
    EXPECT_EQ(log(), (std::vector<std::string>{"dragstart src", "drag src", "dragenter nope", "dragover nope",
                                                "dragleave nope", "dragenter bag", "dragover bag", "drop bag", "dragend src move"}));
    EXPECT_LUA(R"(
        log = {}
        dragSeen = false
        assert(UIDrag.begin(doc:getElementById("src")))
        UIDrag.over(doc:getElementById("bag"))
        UIDrag.cancel()
    )");
    EXPECT_EQ(log(), (std::vector<std::string>{"dragstart src", "drag src", "dragenter bag", "dragover bag", "dragleave bag",
                                                "dragend src none"}));
}

// R22: over a virtual view, where among its rows; a drag from a row carries the row
TEST_F(TestScriptEvents, DataTransferInsideAListView){
    EXPECT_LUA(R"(
        drops = {}
        local view = doc:getElementById("slots")
        view:addEventListener("dragover", function(e) last = { e.dropIndex, e.dropPosition } end)
        view:addEventListener("drop", function(e)
            local row = e.dataTransfer:getData("htmlui/row")
            drops[#drops + 1] = { e.dropIndex, e.dropPosition, row and row.index, row and row.key }
        end)
    )");
    UIItemsView view = document->getElementById("slots").getItemsView();
    UIElement row3 = view.getRow(3);
    ASSERT_TRUE(row3.isValid());
    const UIRect rect = row3.getBoundingClientRect();
    UIElement row1 = view.getRow(1);
    const UIRect from = row1.getBoundingClientRect();

    // a drag from row 1 (its .slot is draggable) to the top, middle and bottom bands of row 3
    struct Band { float y; const char* position; };
    for (const Band& band : {Band{rect.y + 4.0f, "before"}, Band{rect.y + rect.height * 0.5f, "on"}, Band{rect.y + rect.height - 4.0f, "after"}}){
        context()->ProcessMouseMove((int)(from.x + 20.0f), (int)(from.y + 20.0f), 0);
        context()->ProcessMouseButtonDown(0, 0);
        move(Rml::Vector2f(from.x + 26.0f, from.y + 20.0f));
        move(Rml::Vector2f(rect.x + 50.0f, band.y));
        EXPECT_EQ(luaString("last[1]"), "3") << band.position;
        EXPECT_EQ(luaString("last[2]"), band.position);
        context()->ProcessMouseButtonUp(0, 0);
        update();
    }
    EXPECT_LUA(R"(
        assert(#drops == 3, #drops)
        assert(drops[1][1] == 3 and drops[1][2] == "before" and drops[1][3] == 1 and drops[1][4] == "s0", tostring(drops[1][2]))
        assert(drops[2][2] == "on" and drops[3][2] == "after")
    )");
}

TEST_F(TestScriptEvents, DataTransferAutoScrollsNearAViewsEdge){
    // R21: within 32 px of a scroll container's edge, up to 600 px/s (on HtmlUIClock)
    HtmlUIClock::setFixedTime(10.0);
    UIItemsView view = document->getElementById("slots").getItemsView();
    UIElement list = document->getElementById("slots");
    const UIRect box = list.getBoundingClientRect();
    const UIRect from = view.getRow(1).getBoundingClientRect();
    context()->ProcessMouseMove((int)(from.x + 20.0f), (int)(from.y + 20.0f), 0);
    context()->ProcessMouseButtonDown(0, 0);
    move(Rml::Vector2f(from.x + 26.0f, from.y + 20.0f));
    // 4 px above the bottom edge: (1 - 4 / 32) of the top speed
    move(Rml::Vector2f(box.x + 50.0f, box.y + box.height - 4.0f));
    EXPECT_FLOAT_EQ(list.getScrollTop(), 0.0f);
    for (int frame = 1; frame <= 3; frame++){
        HtmlUIClock::setFixedTime(10.0 + 0.05 * frame);
        update();
    }
    const float expected = htmlui::DragDropSession::kAutoScrollSpeed * (1.0f - 4.0f / htmlui::DragDropSession::kAutoScrollEdge) * 0.15f;
    EXPECT_NEAR(list.getScrollTop(), expected, 6.0f);
    // the middle: no scroll
    move(Rml::Vector2f(box.x + 50.0f, box.y + box.height * 0.5f));
    const float stopped = list.getScrollTop();
    HtmlUIClock::setFixedTime(10.5);
    update();
    EXPECT_FLOAT_EQ(list.getScrollTop(), stopped);
    context()->ProcessKeyDown(Rml::Input::KI_ESCAPE, 0);
    update();
    context()->ProcessMouseButtonUp(0, 0);
    update();
    HtmlUIClock::reset();
}

TEST_F(TestScriptEvents, DataTransferTouchLongPressDragsASwipeScrolls){
    // R32 (fork P7-H): on touch, a draggable row of a scrolling view drags after a 400 ms press that
    // moved at most 8 px; a touch that moves away first scrolls the view
    HtmlUIClock::setFixedTime(20.0);
    EXPECT_LUA(R"(
        starts = 0
        doc:getElementById("slots"):addEventListener("dragstart", function() starts = starts + 1 end)
    )");
    UIElement list = document->getElementById("slots");
    UIItemsView view = list.getItemsView();
    const UIRect row = view.getRow(3).getBoundingClientRect();
    Rml::Touch touch{7, Rml::Vector2f(row.x + 30.0f, row.y + 20.0f)};

    // a swipe up: the list scrolls, nothing drags
    context()->ProcessTouchStart(Rml::TouchList{touch}, 0);
    update();
    for (int step = 1; step <= 6; step++){
        HtmlUIClock::setFixedTime(20.0 + 0.02 * step);
        touch.position.y -= 10.0f;
        context()->ProcessTouchMove(Rml::TouchList{touch}, 0);
        update();
    }
    EXPECT_GT(list.getScrollTop(), 20.0f);
    context()->ProcessTouchEnd(Rml::TouchList{touch}, 0);
    update();
    EXPECT_EQ(luaString("starts"), "0");

    // a long press, then a move: the row drags and the list does not scroll
    HtmlUIClock::setFixedTime(30.0);
    update();
    const float scrolled = list.getScrollTop();
    const UIRect again = view.getRow(view.getVisibleRange().first + 1).getBoundingClientRect();
    touch = Rml::Touch{8, Rml::Vector2f(again.x + 30.0f, again.y + 20.0f)};
    context()->ProcessTouchStart(Rml::TouchList{touch}, 0);
    update();
    HtmlUIClock::setFixedTime(30.2);
    touch.position.y += 3.0f;
    context()->ProcessTouchMove(Rml::TouchList{touch}, 0);
    update();
    EXPECT_EQ(luaString("starts"), "0") << "not before 400 ms";
    HtmlUIClock::setFixedTime(30.45);
    touch.position.y += 3.0f;
    context()->ProcessTouchMove(Rml::TouchList{touch}, 0);
    update();
    touch.position.y += 30.0f;
    context()->ProcessTouchMove(Rml::TouchList{touch}, 0);
    update();
    EXPECT_EQ(luaString("starts"), "1");
    EXPECT_NEAR(list.getScrollTop(), scrolled, 0.5f) << "a drag does not scroll";
    context()->ProcessTouchEnd(Rml::TouchList{touch}, 0);
    update();
    HtmlUIClock::reset();
}
