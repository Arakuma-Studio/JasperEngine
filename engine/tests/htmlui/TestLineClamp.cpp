// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Line clamping (phase 6, p6-line-clamp: Chromium 153's legacy -webkit-line-clamp, docs/html-ui/FORK.md
// P6-L1) through the engine's APIs: the clamp changed from Lua (style.webkitLineClamp, a class), the
// element metrics a script reads (offsetHeight, clientHeight, scrollHeight, getBoundingClientRect),
// the inspector's boxes, styles and flags, and hit testing of the lines past the clamp point.

#include "InspectorTestCommon.h"
#include "TestScriptCommon.h"

#include <RmlUi/Core.h>

using namespace jasperengine;
using namespace jasperengine::htmlui;
using namespace jasperengine::test;

namespace {

    class LineClampScriptTest : public ScriptTest {
    protected:
        const char* documentFile() const override { return "p6/line_clamp.html"; }
    };

    class LineClampInspectorTest : public InspectorTestBase {};

    // the lines of 20px of the clamped description: the unclamped twin's height
    float lineCount(UIElement element){
        return element.getOffsetHeight() / 20.0f;
    }

}

TEST_F(LineClampScriptTest, TheClampFollowsTheScript){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    update();
    UIElement desc = document->getElementById("desc");
    UIElement plain = document->getElementById("plain");
    ASSERT_TRUE(desc.isValid() && plain.isValid());
    const float lines = lineCount(plain);
    ASSERT_GE(lines, 5.0f) << "the description wraps into more lines than the clamps below";

    // two lines: what a script reads is the clamped box; the scrollable overflow keeps every line
    EXPECT_FLOAT_EQ(desc.getOffsetHeight(), 40.0f);
    EXPECT_FLOAT_EQ(desc.getClientHeight(), 40.0f);
    EXPECT_FLOAT_EQ(desc.getBoundingClientRect().height, 40.0f);
    EXPECT_FLOAT_EQ(desc.getScrollHeight(), plain.getOffsetHeight());
    EXPECT_EQ(desc.getStyle("display"), "flow-root");

    // CSSOM's webkit-cased name, a number, the hyphenated name
    EXPECT_LUA(R"(
        local desc = doc:getElementById("desc")
        desc.style.webkitLineClamp = "3"
    )");
    update();
    EXPECT_FLOAT_EQ(desc.getOffsetHeight(), 60.0f);
    EXPECT_LUA(R"(doc:getElementById("desc").style["-webkit-line-clamp"] = 1)");
    update();
    EXPECT_FLOAT_EQ(desc.getOffsetHeight(), 20.0f);

    // none: every line (the legacy box is a flex container then), as the class of a "read more" toggle does
    EXPECT_LUA(R"(doc:getElementById("desc").style.webkitLineClamp = "none")");
    update();
    EXPECT_FLOAT_EQ(desc.getOffsetHeight(), plain.getOffsetHeight());
    EXPECT_EQ(desc.getStyle("display"), "-webkit-box");
    EXPECT_LUA(R"(
        local desc = doc:getElementById("desc")
        desc.style.webkitLineClamp = ""
        desc.classList:add("open")
    )");
    update();
    EXPECT_FLOAT_EQ(desc.getOffsetHeight(), plain.getOffsetHeight());
    EXPECT_LUA(R"(doc:getElementById("desc").classList:remove("open"))");
    update();
    EXPECT_FLOAT_EQ(desc.getOffsetHeight(), 40.0f);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN), 0u);
}

TEST_F(LineClampInspectorTest, BoxesStylesAndHits){
    page("clamp", "html, body { margin: 0; }\n"
                  "body { font-family: sans-serif; font-size: 14px; line-height: 20px; }\n"
                  ".card { position: absolute; left: 20px; top: 20px; width: 160px; background: #222; }\n"
                  ".desc { display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: 2; overflow: hidden; margin: 0; }\n"
                  "#visible { position: absolute; left: 300px; top: 20px; width: 160px; overflow: visible; }\n",
         "<div class=\"card\" id=\"card\"><p class=\"desc\" id=\"desc\">A sword forged in dragonfire that deals extra fire damage, "
         "ignites the enemies it strikes on critical hits and glows in the dark.</p></div>\n"
         "<p class=\"desc\" id=\"visible\"><span id=\"text\">A sword forged in dragonfire that deals extra fire damage, ignites the "
         "enemies it strikes on critical hits and glows in the dark.</span></p>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/clamp.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();
    const Entity entity = document.getEntity();

    // the inspector's boxes: the clamped border box, the overflow of the lines after it
    InspectDetail detail;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::detail(scene, entity, "#desc", {"display", "-webkit-line-clamp", "-webkit-box-orient"}, detail, error)) << error;
    EXPECT_RECT(detail.box.border, 20, 20, 160, 40);
    EXPECT_EQ(detail.box.clientHeight, 40.0f);
    EXPECT_GE(detail.box.scrollHeight, 80.0f);
    EXPECT_TRUE(detail.element.flags.overflowY);
    ASSERT_EQ(detail.styles.size(), 3u);
    EXPECT_EQ(detail.styles[0].value, "flow-root");
    EXPECT_EQ(detail.styles[1].value, "2");
    EXPECT_EQ(detail.styles[2].value, "vertical");
    // the card around it is as tall as the two lines
    InspectRect card;
    ASSERT_TRUE(HtmlUIInspector::elementRect(scene, entity, "#card", card, error)) << error;
    EXPECT_RECT(card, 20, 20, 160, 40);

    // the lines past the clamp point: clipped by overflow: hidden, not hit; with overflow: visible they are
    // drawn and their text is hit, as in Chromium
    std::vector<InspectElement> stack;
    ASSERT_TRUE(HtmlUIInspector::hitTest(scene, entity, 30, 30, stack, error)) << error;
    ASSERT_FALSE(stack.empty());
    EXPECT_EQ(stack[0].ref, "#desc");
    ASSERT_TRUE(HtmlUIInspector::hitTest(scene, entity, 30, 70, stack, error)) << error;
    for (const InspectElement& hit : stack){
        EXPECT_NE(hit.ref, "#desc");
        EXPECT_NE(hit.ref, "#card");
    }
    ASSERT_TRUE(HtmlUIInspector::hitTest(scene, entity, 310, 70, stack, error)) << error;
    ASSERT_FALSE(stack.empty());
    EXPECT_EQ(stack[0].ref, "#text");
    EXPECT_EQ(logText(D_LOG_WARN), "");
}

// An atomic inline the ellipsis hides is neither drawn nor hit (Chromium's LineTruncator), and shows
// again when the clamp goes.
TEST_F(LineClampInspectorTest, AHiddenInlineBlockIsNotHit){
    page("ib", "html, body { margin: 0; }\n"
               "body { font-family: sans-serif; font-size: 14px; line-height: 20px; }\n"
               ".desc { position: absolute; left: 0; top: 0; width: 100px; display: -webkit-box; -webkit-box-orient: vertical;\n"
               "        -webkit-line-clamp: 1; overflow: hidden; margin: 0; white-space: nowrap; }\n"
               ".gem { display: inline-block; width: 30px; height: 14px; background: #4f8cff; }\n",
         "<p class=\"desc\" id=\"desc\">WWWWWWWWW<span class=\"gem\" id=\"gem\"></span><br>more</p>");
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("ui/ib.html");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    Scene* scene = &fixture.getScene();
    const Entity entity = document.getEntity();

    InspectRect gem;
    std::string error;
    ASSERT_TRUE(HtmlUIInspector::elementRect(scene, entity, "#gem", gem, error)) << error;
    std::vector<InspectElement> stack;
    const float x = std::min(gem.x + gem.width * 0.5f, 99.0f), y = gem.y + gem.height * 0.5f;
    ASSERT_TRUE(HtmlUIInspector::hitTest(scene, entity, x, y, stack, error)) << error;
    ASSERT_FALSE(stack.empty());
    EXPECT_EQ(stack[0].ref, "#desc") << "the hidden inline-block is hit";

    // no clamp: drawn and hit again
    Rml::Element* desc = rmlContext(*scene)->GetDocument(0)->GetElementById("desc");
    ASSERT_NE(desc, nullptr);
    desc->SetProperty("-webkit-line-clamp", "none");
    updateDocuments(*scene);
    ASSERT_TRUE(HtmlUIInspector::elementRect(scene, entity, "#gem", gem, error)) << error;
    ASSERT_TRUE(HtmlUIInspector::hitTest(scene, entity, gem.x + gem.width * 0.5f, gem.y + gem.height * 0.5f, stack, error)) << error;
    ASSERT_FALSE(stack.empty());
    EXPECT_EQ(stack[0].ref, "#gem");
}
