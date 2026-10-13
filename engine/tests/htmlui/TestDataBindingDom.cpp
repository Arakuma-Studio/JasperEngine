// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Data binding as the DOM shows it (phase 4, p4-input): the hidden template RmlUi keeps after
// the rows of a data-for is no element of the Lua DOM (queries, child lists, getElementById) nor
// of the inspector, and no sibling for structural selectors (fork P4-5); a data-style expression
// of a model that does not have its variable yet (edit mode's placeholder models) sets nothing
// and logs nothing (fork P4-4), while a value invalid with every variable present still warns.

#include "InspectorTestCommon.h"

#include "object/ui/UIDataModel.h"
#include "object/ui/UIElementInternal.h"
#include "object/ui/UIValue.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::htmlui;

namespace {

    const char* kListCss =
        "html, body { margin: 0; }\n"
        ".row { display: block; width: 10px; height: 10px; }\n"
        ".row:last-child { width: 77px; }\n"
        ".row:only-child { width: 55px; }\n";
    const char* kListBody =
        "<div data-model=\"hud\">\n"
        "<ul id=\"log\"><li class=\"row\" id=\"entry\" data-for=\"entry : log\">{{ entry }}</li></ul>\n"
        "<p id=\"after\">after</p>\n"
        "</div>";

    class DataBindingDomTest : public InspectorTestBase {
    protected:
        std::unique_ptr<SceneFixture> fixture;

        void SetUp() override{
            InspectorTestBase::SetUp();
            if (HasFatalFailure())
                return;
            fixture.reset(new SceneFixture());
        }

        void TearDown() override{
            fixture.reset();
            InspectorTestBase::TearDown();
        }

        std::vector<std::string> texts(const std::vector<UIElement>& elements){
            std::vector<std::string> out;
            for (const UIElement& element : elements)
                out.push_back(element.getTextContent());
            return out;
        }

        std::vector<InspectElement> query(const UIDocument& document, const std::string& selector, int depth = 0){
            std::vector<InspectElement> out;
            bool truncated = false;
            std::string error;
            EXPECT_TRUE(HtmlUIInspector::query(&fixture->getScene(), document.getEntity(), selector, depth, 1000, out, truncated, error)) << error;
            return out;
        }

        Rml::Element* rml(const UIElement& element){
            return UIElementAccess::get(element);
        }
    };

}

TEST_F(DataBindingDomTest, DataForTemplateIsNotInTheDom){
    page("list", kListCss, kListBody);
    UIDocument document = fixture->addDocument("ui/list.html");
    UIValue initial = UIValue::object();
    initial["log"] = UIValue::array();
    initial["log"].push("a");
    initial["log"].push("b");
    initial["log"].push("c");
    UIDataModel model = document.createDataModel("hud", initial);
    ASSERT_TRUE(fixture->waitUntilLoaded());

    // RmlUi keeps the template after the rows
    UIElement log = document.getElementById("log");
    ASSERT_TRUE(log.isValid());
    ASSERT_EQ(rml(log)->GetNumChildren(), 4);
    EXPECT_TRUE(rml(log)->GetChild(3)->HasAttribute("data-for"));

    // the Lua DOM shows the rows, in order
    EXPECT_EQ(texts(document.querySelectorAll("li")), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(texts(document.querySelectorAll(".row")), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(texts(log.querySelectorAll("li")), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(texts(log.getChildren()), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(log.getChildElementCount(), 3);
    EXPECT_EQ(log.getLastElementChild().getTextContent(), "c");
    EXPECT_FALSE(log.getLastElementChild().getNextElementSibling().isValid());
    EXPECT_EQ(document.querySelector("li:last-child").getTextContent(), "c");
    EXPECT_EQ(document.getElementById("entry").getTextContent(), "a") << "the first row has the template's id";

    // the last row is :last-child for the stylesheet too (fork P4-5)
    EXPECT_EQ(rml(log.getLastElementChild())->GetProperty("width")->ToString(), "77px");
    EXPECT_EQ(rml(log.getFirstElementChild())->GetProperty("width")->ToString(), "10px");

    // the inspector lists the rows only
    EXPECT_EQ(query(document, "li").size(), 3u);
    const std::vector<InspectElement> tree = query(document, "#log", 1);
    ASSERT_EQ(tree.size(), 4u);   // the list and its three rows
    EXPECT_EQ(tree[3].ref, "#log > li:nth-child(3)");
    EXPECT_EQ(query(document, "#log > li:nth-child(3)").size(), 1u);
    // assert_ui's count sees the rows (the inventory example's manifest: count .slot = 24)
    AssertSpec count;
    count.kind = AssertKind::Count;
    count.selector = ".row";
    count.count = 3;
    AssertResult counted = HtmlUIAssertions::run(&fixture->getScene(), document.getEntity(), count);
    EXPECT_TRUE(counted.passed) << (counted.failures.empty() ? counted.error : counted.failures[0].message);
    EXPECT_EQ(counted.matched, 3u);
    AssertSpec visible;
    visible.kind = AssertKind::Visible;
    visible.selector = "#log > li";
    counted = HtmlUIAssertions::run(&fixture->getScene(), document.getEntity(), visible);
    EXPECT_TRUE(counted.passed) << "the hidden template is not a match that fails visible";
    EXPECT_EQ(counted.matched, 3u);

    // one row: :only-child; no rows: nothing, and a query of the template's selector finds nothing
    UIValue one = UIValue::array();
    one.push("z");
    model.set("log", one);
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(texts(document.querySelectorAll("li")), (std::vector<std::string>{"z"}));
    EXPECT_EQ(rml(log.getFirstElementChild())->GetProperty("width")->ToString(), "55px");
    model.set("log", UIValue::array());
    GpuEnvironment::drawFrames(2);
    EXPECT_TRUE(document.querySelectorAll("li").empty());
    EXPECT_FALSE(document.querySelector("li").isValid());
    EXPECT_FALSE(document.querySelector(".row").isValid());
    EXPECT_FALSE(document.getElementById("entry").isValid());
    EXPECT_EQ(log.getChildElementCount(), 0);
    EXPECT_FALSE(log.getFirstElementChild().isValid());
    EXPECT_TRUE(query(document, "li").empty());
    EXPECT_EQ(document.querySelector("#log + p").getTextContent(), "after");
}

// edit mode: no script created the model, so `health` is missing; the binding sets nothing and
// logs nothing until the model has it. A literal that is no width still warns
TEST_F(DataBindingDomTest, DataStyleOfAMissingVariableLogsNothing){
    page("hud", "html, body { margin: 0; } .bar { display: block; height: 10px; }\n",
         "<div data-model=\"hud\">\n"
         "<div id=\"fill\" class=\"bar\" data-style-width=\"health + '%'\" data-class-low=\"health < 30\"></div>\n"
         "<span id=\"label\">{{ health }} / 100</span>\n"
         "</div>\n"
         "<div data-model=\"bad\"><div id=\"bad\" class=\"bar\" data-style-width=\"'abc'\"></div></div>");
    UIDocument document = fixture->addDocument("ui/hud.html");
    ASSERT_TRUE(fixture->waitUntilLoaded());

    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "width: %;"), 0u) << logText(D_LOG_WARN);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "width: %;"), 0u) << logText(D_LOG_ERROR);
    EXPECT_GE(TestSystem::get().countLogs(D_LOG_WARN, "width: abc;"), 1u) << "a real syntax error is still reported\n" << logText(D_LOG_WARN);
    UIElement fill = document.getElementById("fill");
    ASSERT_TRUE(fill.isValid());
    EXPECT_EQ(rml(fill)->GetLocalProperty("width"), nullptr);
    EXPECT_NE(document.getElementById("label").getTextContent().find("/ 100"), std::string::npos);

    // the model arrives (play): the width follows it, still without a warning
    TestSystem::get().clearLogs();
    UIValue initial = UIValue::object();
    initial["health"] = 40;
    UIDataModel model = document.createDataModel("hud", initial);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    fill = document.getElementById("fill");
    ASSERT_TRUE(fill.isValid());
    ASSERT_NE(rml(fill)->GetLocalProperty("width"), nullptr);
    EXPECT_EQ(rml(fill)->GetLocalProperty("width")->ToString(), "40%");
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "width: %;"), 0u) << logText(D_LOG_WARN);
}
