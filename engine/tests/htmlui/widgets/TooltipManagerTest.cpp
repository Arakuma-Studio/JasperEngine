// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Tooltips on a fixed HtmlUIClock (S17 R14-R17, AC8): the delay, the hop, the source identity,
// the hide triggers, keyboard focus, rich tooltips and the Lua API.

#include "../TestScriptCommon.h"

#include "HtmlUIClock.h"
#include "htmlui/widgets/TooltipManager.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class TooltipManager : public ScriptTest {
    protected:
        const char* documentFile() const override { return "s17/tooltips.html"; }

        void SetUp() override{
            ScriptTest::SetUp();
            HtmlUIClock::setFixedTime(10.0);
            update();
        }

        void TearDown() override{
            HtmlUIClock::reset();
            ScriptTest::TearDown();
        }

        void at(double seconds){
            HtmlUIClock::setFixedTime(10.0 + seconds);
            update();
        }

        Rml::Vector2f centerOf(const char* id){
            const UIRect rect = document->getElementById(id).getBoundingClientRect();
            return Rml::Vector2f(rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f);
        }

        void hover(const char* id){
            const Rml::Vector2f point = centerOf(id);
            context()->ProcessMouseMove((int)point.x, (int)point.y, 0);
        }

        UITooltipInfo tip(){
            return document->getTooltip();
        }
    };

}

TEST_F(TooltipManager, TitleShowsAfterTheDelayBelowThePointer){
    hover("a");
    at(0.0);
    at(0.49);
    EXPECT_FALSE(tip().visible);
    at(0.5);
    at(0.5);   // laid out out of view first, placed the frame after
    UITooltipInfo info = tip();
    ASSERT_TRUE(info.visible);
    EXPECT_EQ(info.text, "Alpha tip");
    EXPECT_TRUE(info.source == document->getElementById("a"));
    const Rml::Vector2f pointer = centerOf("a");
    EXPECT_NEAR(info.rect.y, pointer.y + 1.0f + htmlui::TooltipManager::kTitleGap, 1.0f);
    EXPECT_EQ(info.side, "bottom");
    // the element is the author's to style
    EXPECT_TRUE(info.tooltip.isValid());
    EXPECT_EQ(info.tooltip.getAttribute("role"), "tooltip");

    // a child of the source does not restart it
    hover("a-child");
    at(0.6);
    EXPECT_TRUE(tip().visible);
    EXPECT_TRUE(tip().source == document->getElementById("a"));
}

TEST_F(TooltipManager, HopShowsTheNextAtOnce){
    hover("a");
    at(0.0);
    at(0.5);
    at(0.5);
    ASSERT_TRUE(tip().visible);
    hover("b");
    at(0.6);
    at(0.6);
    EXPECT_TRUE(tip().visible);
    EXPECT_EQ(tip().text, "Beta");
    // away for longer than the hop window: the delay again
    hover("plain");
    at(0.7);
    EXPECT_FALSE(tip().visible);
    at(1.3);
    hover("a");
    at(1.3);
    at(1.4);
    EXPECT_FALSE(tip().visible);
    at(1.8);
    at(1.8);
    EXPECT_TRUE(tip().visible);
}

TEST_F(TooltipManager, HidesOnPressWheelKeyAndRemoval){
    auto show = [&](double time){
        hover("plain");
        at(time);
        hover("a");
        at(time + 0.01);
        at(time + 0.6);
        at(time + 0.6);
        ASSERT_TRUE(tip().visible) << time;
    };
    show(1.0);
    context()->ProcessMouseButtonDown(0, 0);
    context()->ProcessMouseButtonUp(0, 0);
    at(1.7);
    EXPECT_FALSE(tip().visible) << "mousedown";

    show(3.0);
    context()->ProcessMouseWheel(Rml::Vector2f(0.0f, 1.0f), 0);
    at(3.7);
    EXPECT_FALSE(tip().visible) << "wheel";

    show(5.0);
    context()->ProcessKeyDown(Rml::Input::KI_LSHIFT, 0);
    at(5.7);
    EXPECT_TRUE(tip().visible) << "a modifier keeps it";
    context()->ProcessKeyDown(Rml::Input::KI_A, 0);
    at(5.8);
    EXPECT_FALSE(tip().visible) << "keydown";

    show(7.0);
    std::vector<std::string> events;
    UIElement a = document->getElementById("a");
    a.addEventListener("tooltiphide", [&](UIEvent&){ events.push_back("hide"); });
    a.remove();
    at(7.7);
    EXPECT_FALSE(tip().visible) << "removal";
    EXPECT_EQ(events, std::vector<std::string>{"hide"});
}

TEST_F(TooltipManager, PerElementDelayAndConfigure){
    hover("quick");
    at(0.0);
    at(0.1);
    at(0.1);
    EXPECT_TRUE(tip().visible) << "tooltip-delay=100ms";
    EXPECT_EQ(tip().text, "Quick");

    UITooltipConfig config;
    config.enabled = false;
    document->configureTooltips(config);
    at(0.2);
    EXPECT_FALSE(tip().visible);
    hover("a");
    at(0.3);
    at(2.0);
    EXPECT_FALSE(tip().visible) << "disabled";
}

TEST_F(TooltipManager, KeyboardFocusShowsIt){
    // Tab from the document: the first button, focus-visible
    UIElementAccess::get(document->getBody())->GetOwnerDocument()->Focus();
    context()->ProcessKeyDown(Rml::Input::KI_TAB, 0);
    context()->ProcessKeyUp(Rml::Input::KI_TAB, 0);
    at(0.0);
    ASSERT_EQ(UIElementAccess::wrap(context()->GetFocusElement()).getId(), "a");
    at(0.5);
    at(0.5);
    EXPECT_TRUE(tip().visible);
    EXPECT_EQ(tip().text, "Alpha tip");
    // a focus tooltip sits on its element's side: above, flipped below at the top of the view
    const UIRect source = document->getElementById("a").getBoundingClientRect();
    EXPECT_EQ(tip().side, "bottom");
    EXPECT_NEAR(tip().rect.y, source.y + source.height + htmlui::TooltipManager::kRichGap, 1.0f);
    document->getElementById("a").blur();
    at(0.6);
    EXPECT_FALSE(tip().visible);
}

TEST_F(TooltipManager, RichTooltipsFromTemplateAndLua){
    hover("rich");
    at(0.0);
    at(0.5);
    at(0.5);
    UITooltipInfo info = tip();
    ASSERT_TRUE(info.visible);
    EXPECT_EQ(info.text, "Rich tip");
    EXPECT_TRUE(info.tooltip.querySelector(".tip-b").isValid());
    // above its source, centred
    const UIRect source = document->getElementById("rich").getBoundingClientRect();
    EXPECT_EQ(info.side, "top");
    EXPECT_NEAR(info.rect.y + info.rect.height, source.y - htmlui::TooltipManager::kRichGap, 1.0f);
    EXPECT_NEAR(info.rect.x + info.rect.width * 0.5f, source.x + source.width * 0.5f, 1.0f);

    // Lua: a function on each show, right of the element, an arrow
    EXPECT_LUA(R"(
        price = 3
        shows = 0
        doc:getElementById("plain"):setTooltip(function() shows = shows + 1; return "Sell for " .. price .. " gold" end,
                                               { placement = "right", arrow = true, delay = 0.2 })
        doc:getElementById("plain"):addEventListener("tooltipshow", function() shown = true end)
    )");
    hover("plain");
    at(2.0);
    at(2.2);
    at(2.2);
    info = tip();
    ASSERT_TRUE(info.visible);
    EXPECT_EQ(info.text, "Sell for 3 gold");
    EXPECT_EQ(info.side, "right");
    EXPECT_TRUE(info.tooltip.querySelector(".htmlui-tooltip-arrow").isValid() ||
                UIElementAccess::get(info.tooltip)->QuerySelector(".htmlui-tooltip-arrow") != nullptr);
    EXPECT_EQ(luaString("shown"), "true");
    EXPECT_LUA(R"(
        assert(doc.tooltip.visible and doc.tooltip.text == "Sell for 3 gold", doc.tooltip.text)
        assert(doc.tooltip.source == doc:getElementById("plain"))
        doc.tooltip:hide()
        assert(not doc.tooltip.visible)
        price = 5
        assert(doc.tooltip:show(doc:getElementById("plain")))
    )");
    at(2.3);
    EXPECT_EQ(tip().text, "Sell for 5 gold");
    EXPECT_EQ(luaString("shows"), "2");
}
