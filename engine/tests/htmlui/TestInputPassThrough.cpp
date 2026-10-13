// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Pointer routing through stacked screen documents (phase-1 spec §3.6.2 "as implemented",
// fork P4-1): a page (a document, or an HTML document's <body>) that paints no background is
// not hit itself, so the pointer goes on to the next document below in z-order, then to the
// game. Hover, presses, releases, wheel and touches follow it; a press keeps its capture; a
// press nothing takes releases the focus; the game sees only what no document took.

#include "InspectorTestCommon.h"

#include "Engine.h"
#include "HtmlUIContext.h"
#include "Input.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>

#include <functional>
#include <map>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const char* callbackTag = "htmlui-tests-input-pass-through";

    // counts the events an element gets, by type
    class Counter : public Rml::EventListener {
    public:
        void ProcessEvent(Rml::Event& event) override{
            counts[event.GetType()]++;
        }
        int operator[](const std::string& type) const{
            auto it = counts.find(type);
            return it != counts.end() ? it->second : 0;
        }
        std::map<std::string, int> counts;
    };

    const char* kButtonCss =
        "html, body { margin: 0; }\n"
        "button { position: absolute; width: 120px; height: 40px; }\n";

    class InputPassThroughTest : public InspectorTestBase {
    protected:
        std::unique_ptr<SceneFixture> fixture;

        void SetUp() override{
            InspectorTestBase::SetUp();
            if (HasFatalFailure())
                return;
            fixture.reset(new SceneFixture());
            Engine::onMouseDown.add(callbackTag, std::function<void(int, float, float, int)>([this](int, float, float, int){ gameDowns++; }));
            Engine::onMouseUp.add(callbackTag, std::function<void(int, float, float, int)>([this](int, float, float, int){ gameUps++; }));
            Engine::onMouseMove.add(callbackTag, std::function<void(float, float, int)>([this](float, float, int){ gameMoves++; }));
            Engine::onMouseScroll.add(callbackTag, std::function<void(float, float, int)>([this](float, float, int){ gameScrolls++; }));
            Engine::onTouchStart.add(callbackTag, std::function<void(int, float, float)>([this](int, float, float){ gameTouches++; }));
        }

        void TearDown() override{
            Engine::onMouseDown.remove(callbackTag);
            Engine::onMouseUp.remove(callbackTag);
            Engine::onMouseMove.remove(callbackTag);
            Engine::onMouseScroll.remove(callbackTag);
            Engine::onTouchStart.remove(callbackTag);
            Engine::systemTouchCancel();
            Engine::setIgnoreEventsHandledByUI(true);
            for (const Listening& entry : listeners)
                entry.element->RemoveEventListener(entry.type, entry.counter);
            listeners.clear();
            fixture.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << logText(D_LOG_ERROR);
            InspectorTestBase::TearDown();
        }

        UIDocument add(const std::string& name, const std::string& css, const std::string& body, int sortOrder){
            page(name, css, body);
            return fixture->addDocument("ui/" + name + ".html", sortOrder);
        }

        UIDocumentSystem& system(){
            return *fixture->getScene().getSystem<UIDocumentSystem>();
        }

        Rml::Context* context(){
            return system().getContext()->getRmlContext();
        }

        Rml::Element* element(const UIDocument& document, const char* id){
            Rml::ElementDocument* rml = system().getDocument(document.getEntity());
            return rml ? rml->GetElementById(id) : nullptr;
        }

        // the events of `types` the element gets, counted
        Counter& listen(Rml::Element* element, std::initializer_list<const char*> types){
            counters.emplace_back(new Counter());
            Counter* counter = counters.back().get();
            for (const char* type : types){
                element->AddEventListener(type, counter);
                listeners.push_back(Listening{element, type, counter});
            }
            return *counter;
        }

        void click(float x, float y){
            Engine::systemMouseMove(x, y, 0);
            Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, x, y, 0);
            Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, x, y, 0);
        }

        std::string hoverId(){
            Rml::Element* hover = context()->GetHoverElement();
            return hover && hover != context()->GetRootElement() ? hover->GetId() : std::string("(none)");
        }

        int gameDowns = 0, gameUps = 0, gameMoves = 0, gameScrolls = 0, gameTouches = 0;
        struct Listening {
            Rml::Element* element;
            std::string type;
            Counter* counter;
        };
        std::vector<std::unique_ptr<Counter>> counters;
        std::vector<Listening> listeners;
    };

}

// hover and click through the transparent part of the upper document onto the lower one's button
TEST_F(InputPassThroughTest, ClickAndHoverReachTheDocumentBelow){
    UIDocument lower = add("lower", std::string(kButtonCss) + "#low { left: 100px; top: 100px; }\n", "<button id=\"low\">Low</button>", 0);
    UIDocument upper = add("upper", std::string(kButtonCss) + "#high { left: 400px; top: 100px; }\n", "<button id=\"high\">High</button>", 1);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    Rml::Element* low = element(lower, "low");
    Rml::Element* high = element(upper, "high");
    ASSERT_TRUE(low && high);
    Counter& lowEvents = listen(low, {"click", "mouseover", "mousedown"});
    Counter& highEvents = listen(high, {"click"});

    Engine::systemMouseMove(150, 120, 0);
    EXPECT_EQ(hoverId(), "low");
    EXPECT_TRUE(low->IsPseudoClassSet("hover"));
    EXPECT_EQ(lowEvents["mouseover"], 1);
    EXPECT_EQ(gameMoves, 0) << "the move is the documents'";

    click(150, 120);
    EXPECT_EQ(lowEvents["mousedown"], 1);
    EXPECT_EQ(lowEvents["click"], 1);
    EXPECT_EQ(highEvents["click"], 0);
    EXPECT_EQ(gameDowns, 0);
    EXPECT_EQ(gameUps, 0);
    EXPECT_EQ(context()->GetFocusElement(), low) << "the press focuses the element below";

    // the inspector agrees: the upper document has nothing there, the lower one its button
    std::vector<htmlui::InspectElement> stack;
    std::string error;
    ASSERT_TRUE(htmlui::HtmlUIInspector::hitTest(&fixture->getScene(), upper.getEntity(), 150, 120, stack, error)) << error;
    EXPECT_TRUE(stack.empty());
    ASSERT_TRUE(htmlui::HtmlUIInspector::hitTest(&fixture->getScene(), lower.getEntity(), 150, 120, stack, error)) << error;
    ASSERT_FALSE(stack.empty());
    EXPECT_EQ(stack[0].ref, "#low");

    // the upper document's own button still takes its clicks
    click(450, 120);
    EXPECT_EQ(highEvents["click"], 1);
    EXPECT_EQ(lowEvents["click"], 1);
    EXPECT_EQ(context()->GetFocusElement(), high);
}

// where an element of the upper document covers one of the lower, the upper wins
TEST_F(InputPassThroughTest, TheUpperDocumentsElementWins){
    UIDocument lower = add("lower", std::string(kButtonCss) + "#under { left: 600px; top: 100px; }\n", "<button id=\"under\">Under</button>", 0);
    UIDocument upper = add("upper", std::string(kButtonCss) + "#over { left: 600px; top: 100px; }\n", "<button id=\"over\">Over</button>", 5);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    Counter& under = listen(element(lower, "under"), {"click"});
    Counter& over = listen(element(upper, "over"), {"click"});
    click(650, 120);
    EXPECT_EQ(over["click"], 1);
    EXPECT_EQ(under["click"], 0);
    EXPECT_EQ(gameDowns, 0);

    // swapped orders: the other one is on top
    upper.setSortOrder(-1);
    GpuEnvironment::drawFrames(1);
    click(650, 120);
    EXPECT_EQ(under["click"], 1);
    EXPECT_EQ(over["click"], 1);
}

// a press on the lower document keeps the pointer while it moves over the upper document's content
TEST_F(InputPassThroughTest, APressKeepsItsCaptureOverTheUpperDocument){
    UIDocument lower = add("lower", std::string(kButtonCss) + "#low { left: 100px; top: 100px; }\n", "<button id=\"low\">Low</button>", 0);
    UIDocument upper = add("upper", std::string(kButtonCss) + "#high { left: 400px; top: 100px; }\n", "<button id=\"high\">High</button>", 1);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    Rml::Element* low = element(lower, "low");
    Rml::Element* high = element(upper, "high");
    // a draggable element (RCSS 'drag: drag'): RmlUi's drag events stay with it
    low->SetProperty("drag", "drag");
    GpuEnvironment::drawFrames(1);
    ASSERT_EQ(low->GetComputedValues().drag(), Rml::Style::Drag::Drag);
    Counter& lowEvents = listen(low, {"dragstart", "drag", "dragend", "click"});
    Counter& highEvents = listen(high, {"click", "mouseup"});

    Engine::systemMouseMove(150, 120, 0);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, 150, 120, 0);
    EXPECT_TRUE(low->IsPseudoClassSet("active"));
    EXPECT_TRUE(system().hasPointerCapture(0, D_MOUSE_BUTTON_LEFT, false));
    Engine::systemMouseMove(300, 120, 0);
    Engine::systemMouseMove(450, 120, 0);
    EXPECT_EQ(lowEvents["dragstart"], 1);
    EXPECT_GE(lowEvents["drag"], 1);
    EXPECT_TRUE(low->IsPseudoClassSet("active")) << "the pressed element stays pressed";
    EXPECT_FALSE(high->IsPseudoClassSet("active"));
    EXPECT_TRUE(system().hasPointerCapture(0, D_MOUSE_BUTTON_LEFT, false));
    // over the empty page too: still the documents'
    Engine::systemMouseMove(900, 600, 0);
    EXPECT_EQ(gameMoves, 0) << "a captured pointer's moves are not the game's";
    Engine::systemMouseMove(450, 120, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, 450, 120, 0);
    EXPECT_EQ(lowEvents["dragend"], 1);
    EXPECT_EQ(lowEvents["click"], 0);
    EXPECT_EQ(highEvents["click"], 0) << "a release is not a click on another element";
    EXPECT_EQ(gameUps, 0);
    EXPECT_FALSE(system().hasPointerCapture(0, D_MOUSE_BUTTON_LEFT, false));
    EXPECT_FALSE(low->IsPseudoClassSet("active"));
}

// two empty pages: everything is the game's, and a press there takes the focus from a field
TEST_F(InputPassThroughTest, BothPagesEmptyGoToTheGame){
    UIDocument lower = add("lower", "html, body { margin: 0; }\n#name { position: absolute; left: 100px; top: 100px; width: 200px; }\n",
                           "<input id=\"name\" type=\"text\">", 0);
    add("upper", "html, body { margin: 0; }\n", "<p>upper</p>", 1);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    Rml::Element* name = element(lower, "name");
    ASSERT_NE(name, nullptr);

    click(150, 110);
    EXPECT_EQ(context()->GetFocusElement(), name);
    EXPECT_TRUE(Engine::wantsTextComposition());
    EXPECT_EQ(gameDowns, 0);

    Engine::systemMouseMove(900, 600, 0);
    EXPECT_EQ(hoverId(), "(none)");
    EXPECT_EQ(gameMoves, 1);
    EXPECT_FALSE(system().eventOnPointerMove(0, 900, 600, 0, false, false));
    click(900, 600);
    EXPECT_EQ(gameDowns, 1);
    EXPECT_EQ(gameUps, 1);
    EXPECT_NE(context()->GetFocusElement(), name) << "the field lost the focus";
    EXPECT_FALSE(Engine::wantsTextComposition());
    Engine::systemMouseScroll(0, -1, 0);
    EXPECT_EQ(gameScrolls, 1);

    // touches too
    Engine::systemTouchStart(1, 900, 600);
    EXPECT_EQ(gameTouches, 1);
    Engine::systemTouchEnd(1, 900, 600);
    Engine::systemTouchStart(2, 150, 110);
    EXPECT_EQ(gameTouches, 1) << "a touch on the field below is the documents'";
    Engine::systemTouchEnd(2, 150, 110);
}

// three layers, each reachable where the ones above are empty
TEST_F(InputPassThroughTest, ThreeLayers){
    UIDocument bottom = add("bottom", std::string(kButtonCss) + "#b0 { left: 0; top: 0; }\n#hidden0 { left: 400px; top: 0; }\n",
                            "<button id=\"b0\">0</button><button id=\"hidden0\">h</button>", 0);
    UIDocument middle = add("middle", std::string(kButtonCss) + "#b1 { left: 200px; top: 0; }\n#cover1 { left: 400px; top: 0; }\n",
                            "<button id=\"b1\">1</button><button id=\"cover1\">c</button>", 1);
    UIDocument top = add("top", std::string(kButtonCss) + "#b2 { left: 600px; top: 0; }\n", "<button id=\"b2\">2</button>", 2);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    Counter& c0 = listen(element(bottom, "b0"), {"click"});
    Counter& h0 = listen(element(bottom, "hidden0"), {"click"});
    Counter& c1 = listen(element(middle, "b1"), {"click"});
    Counter& cover = listen(element(middle, "cover1"), {"click"});
    Counter& c2 = listen(element(top, "b2"), {"click"});

    click(50, 20);
    click(250, 20);
    click(650, 20);
    click(450, 20);
    EXPECT_EQ(c0["click"], 1);
    EXPECT_EQ(c1["click"], 1);
    EXPECT_EQ(c2["click"], 1);
    EXPECT_EQ(cover["click"], 1);
    EXPECT_EQ(h0["click"], 0) << "covered by the middle layer's button";
    EXPECT_EQ(gameDowns, 0);
    click(1000, 500);
    EXPECT_EQ(gameDowns, 1);
}

// an upper page that paints a background, or a part of it with one, stops the pointer
TEST_F(InputPassThroughTest, PaintedPagesStopThePointer){
    UIDocument lower = add("lower", std::string(kButtonCss) + "#low { left: 100px; top: 100px; }\n#low2 { left: 100px; top: 400px; }\n",
                           "<button id=\"low\">Low</button><button id=\"low2\">Low 2</button>", 0);
    UIDocument upper = add("upper", "html, body { margin: 0; }\nbody { background: rgba(0, 0, 0, 0.5); height: 300px; }\n", "<p>dim</p>", 1);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    Counter& low = listen(element(lower, "low"), {"click"});
    Counter& low2 = listen(element(lower, "low2"), {"click"});

    // the body's background is the canvas's: it covers the whole viewport
    click(150, 120);
    click(150, 420);
    EXPECT_EQ(low["click"], 0);
    EXPECT_EQ(low2["click"], 0);
    EXPECT_EQ(gameDowns, 0) << "the dim page takes the pointer";
    (void)upper;
}

// pointer-events: none parts of the upper page let the pointer through as well
TEST_F(InputPassThroughTest, PointerEventsNone){
    UIDocument lower = add("lower", std::string(kButtonCss) + "#low { left: 100px; top: 100px; }\n", "<button id=\"low\">Low</button>", 0);
    UIDocument upper = add("upper", std::string(kButtonCss) +
                           ".overlay { position: absolute; left: 0; top: 0; width: 600px; height: 400px; background: rgba(0, 0, 0, 0.3); pointer-events: none; }\n"
                           "#tip { left: 300px; top: 100px; pointer-events: auto; }\n",
                           "<div class=\"overlay\"><button id=\"tip\">Tip</button></div>", 1);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    Counter& low = listen(element(lower, "low"), {"click"});
    Counter& tip = listen(element(upper, "tip"), {"click"});
    click(150, 120);
    EXPECT_EQ(low["click"], 1) << "through the overlay and the transparent page";
    click(350, 120);
    EXPECT_EQ(tip["click"], 1);
    EXPECT_EQ(low["click"], 1);
    EXPECT_EQ(gameDowns, 0);
}
