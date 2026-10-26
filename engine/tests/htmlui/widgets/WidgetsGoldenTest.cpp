// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Goldens of the widgets (S17 AC7, golden/s17/): the list, grid, tree and table views at rest and
// scrolled, a title tooltip at each corner of the view (flipped and clamped), a rich tooltip with
// its arrow, a drag's ghost with the allowed and denied classes, and a Markdown sample of every
// block kind. Drawn through the full path (UIDocumentSystem, the scene's camera pass) on a pinned
// HtmlUIClock; each test also probes what its fixture defines, so a golden cannot pin a wrong image.

#include "../TestGoldenCommon.h"
#include "../TestDocumentCommon.h"

#include "Engine.h"
#include "HtmlUIClock.h"
#include "Input.h"
#include "object/ui/UIDocument.h"
#include "object/ui/UIItemsView.h"
#include "object/ui/UIElementInternal.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Input.h>

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class WidgetsGolden : public FullPathGoldenTest {
    protected:
        void SetUp() override{
            FullPathGoldenTest::SetUp();
            HtmlUIClock::setFixedTime(100.0);
        }

        void TearDown() override{
            HtmlUIClock::reset();
            FullPathGoldenTest::TearDown();
        }

        // advances the UI clock and draws a frame
        static void at(double seconds){
            HtmlUIClock::setFixedTime(100.0 + seconds);
            GpuEnvironment::drawFrames(1);
        }

        static void moveTo(Scene& scene, float x, float y){
            rmlContext(scene)->ProcessMouseMove((int)x, (int)y, 0);
        }

        static void moveToCenter(Scene& scene, const UIElement& element){
            const UIRect rect = element.getBoundingClientRect();
            moveTo(scene, rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f);
        }

        // a rectangle in canvas px (screen = canvas here) inside the view
        static PixelRect inside(const UIRect& rect, int pad = 2){
            return {(int)rect.x + pad, (int)rect.y + pad, (int)(rect.x + rect.width) - pad, (int)(rect.y + rect.height) - pad};
        }
    };

    // the engine's classes (htmlui-*) are hidden from scripts and UIElement: asked of the RmlUi element
    bool hasClass(const UIElement& element, const char* name){
        Rml::Element* rml = UIElementAccess::get(element);
        return rml && rml->IsClassSet(name);
    }

    UIItemsProvider textProvider(int count, const std::string& prefix){
        UIItemsProvider provider;
        provider.count = count;
        provider.bind = [prefix](const UIElement& row, int index){
            UIElement target = row;
            UIElement inner = row.querySelector("div");
            if (inner.isValid())
                target = inner;
            target.setTextContent(prefix + std::to_string(index));
        };
        return provider;
    }

}

TEST_F(WidgetsGolden, ViewsAtRestAndScrolled){
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("s17/golden_views.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    UIItemsView list = document.getElementById("list").getItemsView();
    UIItemsView grid = document.getElementById("grid").getItemsView();
    UIItemsView tree = document.getElementById("tree").getItemsView();
    UIItemsView table = document.getElementById("table").getItemsView();
    ASSERT_TRUE(list.isValid() && grid.isValid() && tree.isValid() && table.isValid());

    list.setProvider(textProvider(1000, "Row "));
    grid.setProvider(textProvider(500, ""));
    UIItemsProvider nodes;
    nodes.children = [](const std::string& key) -> std::vector<std::string> {
        if (key.empty())
            return {"Act I", "Act II", "Act III", "Act IV"};
        if (key.rfind("Act", 0) == 0){
            std::vector<std::string> children;
            for (int i = 1; i <= 6; i++)
                children.push_back(key + " / quest " + std::to_string(i));
            return children;
        }
        return {};
    };
    nodes.hasChildren = [](const std::string& key){ return key.rfind("Act", 0) == 0 && key.find('/') == std::string::npos; };
    UIItemsView* treeView = &tree;
    nodes.bind = [treeView](const UIElement& row, int index){
        UIElement target = row.querySelector("div").isValid() ? row.querySelector("div") : row;
        target.setTextContent(treeView->getNode(index).key);
    };
    tree.setProvider(nodes);
    UIItemsProvider servers;
    servers.count = 300;
    servers.bind = [](const UIElement& row, int index){
        static const char* const maps[] = {"Harbor", "Dunes", "Peaks"};
        for (UIElement cell : row.querySelectorAll(".htmlui-cell")){
            const std::string column = cell.getAttribute("data-column");
            cell.setTextContent(column == "name" ? "Server " + std::to_string(index) : column == "ping" ? std::to_string((index * 37) % 180)
                                                                                                         : std::string(maps[index % 3]));
        }
    };
    table.setProvider(servers);
    tree.expand("Act I");
    tree.expand("Act III");
    list.setSelection({3});
    GpuEnvironment::drawFrames(3);

    Screen rest;
    ASSERT_TRUE(capture(rest, 2));
    for (const char* id : {"list", "grid", "tree", "table"}){
        const UIRect rect = document.getElementById(id).getBoundingClientRect();
        EXPECT_FALSE(rest.ink(inside(rect, 4), 0x26, 0x2a, 0x33).empty()) << id << " draws its rows";
    }
    // the selected row's colour: item 3 (1-based), the third row
    const UIRect listRect = document.getElementById("list").getBoundingClientRect();
    EXPECT_GT(rest.countColor({(int)listRect.x + 200, (int)listRect.y + 2 * 24 + 4, (int)listRect.x + 290, (int)listRect.y + 3 * 24 - 4},
                              0x3b, 0x5b, 0xdb, 12), 100u);
    EXPECT_SCREEN_GOLDEN(rest, "s17/views_rest");

    list.scrollToIndex(500, "start");
    grid.scrollToIndex(250, "center");
    tree.scrollToIndex(20, "end");
    table.scrollToIndex(150, "start");
    GpuEnvironment::drawFrames(3);
    Screen scrolled;
    ASSERT_TRUE(capture(scrolled, 2));
    EXPECT_EQ(list.getVisibleRange().first, 500);
    EXPECT_EQ(list.getRow(500).getTextContent(), "Row 500");
    // the table's header stays at the top (sticky), over the rows
    const UIRect tableRect = document.getElementById("table").getBoundingClientRect();
    const UIRect headerRect = document.getElementById("table").querySelector(".htmlui-table-header").getBoundingClientRect();
    EXPECT_NEAR(headerRect.y, tableRect.y, 0.5f);
    EXPECT_GT(scrolled.countColor({(int)tableRect.x + 120, (int)tableRect.y + 2, (int)tableRect.x + 140, (int)tableRect.y + 12}, 0x3b, 0x42, 0x52, 8), 100u)
        << "the header's background at the top of the scrolled table";
    EXPECT_SCREEN_GOLDEN(scrolled, "s17/views_scrolled");
}

TEST_F(WidgetsGolden, TitleTooltipsAtTheCorners){
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("s17/golden_tooltips.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene& scene = fixture.getScene();

    double time = 0.0;
    for (const char* id : {"tl", "tr", "bl", "br"}){
        moveTo(scene, 320.0f, 180.0f);
        time += 2.0;
        at(time);
        moveToCenter(scene, document.getElementById(id));
        at(time);
        at(time + 0.5);
        at(time + 0.5);
        const UITooltipInfo tip = document.getTooltip();
        ASSERT_TRUE(tip.visible) << id;
        // inside the view with its 4 px pad: flipped above at the bottom, clamped at the sides
        EXPECT_GE(tip.rect.x, 4.0f - 0.5f) << id;
        EXPECT_GE(tip.rect.y, 4.0f - 0.5f) << id;
        EXPECT_LE(tip.rect.x + tip.rect.width, 640.0f - 4.0f + 0.5f) << id;
        EXPECT_LE(tip.rect.y + tip.rect.height, 360.0f - 4.0f + 0.5f) << id;
        EXPECT_EQ(tip.side, id[0] == 'b' ? "top" : "bottom") << id;
        EXPECT_NE(tip.text.find("corner tooltip"), std::string::npos) << id << ": " << tip.text;
        EXPECT_TRUE(tip.source == document.getElementById(id)) << id;
        Screen screen;
        ASSERT_TRUE(capture(screen));
        EXPECT_FALSE(screen.inkOnBackground(inside(tip.rect)).empty()) << id << ": the tooltip draws";
        EXPECT_SCREEN_GOLDEN(screen, std::string("s17/tooltip_") + id);
    }
}

TEST_F(WidgetsGolden, RichTooltipWithArrow){
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("s17/golden_tooltips.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene& scene = fixture.getScene();
    UITooltipOptions options;
    options.placement = "right";
    options.arrow = true;
    options.delay = 0.2f;
    document.getElementById("rich").setTooltip(document.getElementById("card"), options);
    moveTo(scene, 320.0f, 300.0f);
    at(0.5);
    moveToCenter(scene, document.getElementById("rich"));
    at(1.0);
    for (int frame = 0; frame < 10 && !document.getTooltip().visible; frame++)
        at(1.0 + 0.1 * frame);
    at(2.0);
    const UITooltipInfo tip = document.getTooltip();
    ASSERT_TRUE(tip.visible) << "source " << (tip.source.isValid() ? tip.source.getId() : std::string("none"));
    EXPECT_EQ(tip.side, "right");
    EXPECT_TRUE(tip.tooltip.querySelector(".htmlui-tooltip-arrow").isValid());
    EXPECT_NE(tip.text.find("Ember Blade"), std::string::npos);
    const UIRect source = document.getElementById("rich").getBoundingClientRect();
    EXPECT_GT(tip.rect.x, source.x + source.width);
    Screen screen;
    ASSERT_TRUE(capture(screen));
    EXPECT_SCREEN_GOLDEN(screen, "s17/tooltip_rich_arrow");
}

TEST_F(WidgetsGolden, DragGhostAndDropClasses){
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("s17/golden_drag.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene& scene = fixture.getScene();
    UIElement source = document.getElementById("src");
    source.addEventListener("dragstart", [](UIEvent& event){ event.getDataTransfer().setData("item", "sword"); });

    // through the engine's pointer input, as a player's mouse
    const UIRect from = source.getBoundingClientRect();
    Engine::systemMouseMove(from.x + 20.0f, from.y + 20.0f, 0);
    at(0.05);
    Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, from.x + 20.0f, from.y + 20.0f, 0);
    at(0.06);
    Engine::systemMouseMove(from.x + 30.0f, from.y + 20.0f, 0);
    at(0.1);
    const UIRect yes = document.getElementById("yes").getBoundingClientRect();
    Engine::systemMouseMove(yes.x + 30.0f, yes.y + 30.0f, 0);
    at(0.2);
    at(0.2);
    EXPECT_TRUE(hasClass(document.getElementById("yes"), "htmlui-drop-allowed"));
    EXPECT_TRUE(hasClass(source, "htmlui-drag-source"));
    Screen allowed;
    ASSERT_TRUE(capture(allowed));
    // the green target under the ghost (the source's orange at 0.7)
    EXPECT_GT(allowed.countColor(inside(yes, 4), 0x2f, 0x9e, 0x44, 30), 200u);
    // the ghost (a copy of the source at 0.7 opacity) at the pointer, over the target
    Rml::Element* ghost = UIElementAccess::get(document.getBody())->GetOwnerDocument()->QuerySelector(".htmlui-drag-ghost");
    ASSERT_NE(ghost, nullptr);
    EXPECT_NEAR(ghost->GetAbsoluteOffset(Rml::BoxArea::Border).x, yes.x + 30.0f - 20.0f, 2.0f) << "where the source was grabbed";
    EXPECT_SCREEN_GOLDEN(allowed, "s17/drag_allowed");

    const UIRect no = document.getElementById("no").getBoundingClientRect();
    Engine::systemMouseMove(no.x + 30.0f, no.y + 30.0f, 0);
    at(0.3);
    at(0.3);
    EXPECT_TRUE(hasClass(document.getElementById("no"), "htmlui-drop-denied"));
    EXPECT_FALSE(hasClass(document.getElementById("yes"), "htmlui-drop-allowed"));
    Screen denied;
    ASSERT_TRUE(capture(denied));
    EXPECT_SCREEN_GOLDEN(denied, "s17/drag_denied");

    rmlContext(scene)->ProcessKeyDown(Rml::Input::KI_ESCAPE, 0);
    at(0.4);
    Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, no.x + 30.0f, no.y + 30.0f, 0);
    at(0.5);
    EXPECT_FALSE(hasClass(source, "htmlui-drag-source"));
}

TEST_F(WidgetsGolden, MarkdownEveryBlockKind){
    GpuEnvironment::setView(640, 480, 640, 480, Scaling::FITWIDTH);
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("s17/golden_markdown.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    GpuEnvironment::drawFrames(2);
    UIElement md = document.getElementById("md");
    for (const char* selector : {"h1", "h2", "p", "ol", "ol ul", "blockquote", "hr", "table", "pre code", "strong", "em", "code", "a"})
        EXPECT_TRUE(md.querySelector(selector).isValid()) << selector;
    Screen screen;
    ASSERT_TRUE(capture(screen));
    EXPECT_SCREEN_GOLDEN(screen, "s17/markdown");
}
