// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIElement / UIDocument C++ DOM API (phase 1 §3.9.1): tree navigation, queries, mutation and
// the detached pool, attributes, classes, style, dataset, form controls, scroll and geometry,
// handle lifetimes.

#include "TestScriptCommon.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class ScriptElementTest : public ScriptTest {};

    std::vector<std::string> ids(const std::vector<UIElement>& elements){
        std::vector<std::string> result;
        for (const UIElement& element : elements){
            result.push_back(element.getId());
        }
        return result;
    }

}

TEST_F(ScriptElementTest, TreeNavigation){
    UIElement list = document->getElementById("list");
    ASSERT_TRUE(list.isValid());
    EXPECT_EQ(list.getTagName(), "div");
    EXPECT_EQ(list.getChildElementCount(), 3);
    EXPECT_EQ(ids(list.getChildren()), (std::vector<std::string>{"i1", "i2", "i3"}));
    EXPECT_EQ(list.getFirstElementChild().getId(), "i1");
    EXPECT_EQ(list.getLastElementChild().getId(), "i3");
    UIElement i2 = document->getElementById("i2");
    EXPECT_EQ(i2.getPreviousElementSibling().getId(), "i1");
    EXPECT_EQ(i2.getNextElementSibling().getId(), "i3");
    EXPECT_FALSE(document->getElementById("i3").getNextElementSibling().isValid());
    EXPECT_TRUE(i2.getParentElement() == list);
    EXPECT_EQ(list.getParentElement().getId(), "outer");

    // the document element is the top: no parent, no siblings
    UIElement body = document->getBody();
    ASSERT_TRUE(body.isValid());
    EXPECT_FALSE(body.getParentElement().isValid());
    EXPECT_FALSE(body.getNextElementSibling().isValid());

    // text nodes are not element children
    UIElement btn = document->getElementById("btn");
    EXPECT_EQ(btn.getChildElementCount(), 1);
    EXPECT_EQ(document->getElementById("i1").getChildElementCount(), 0);
    EXPECT_EQ(document->getElementById("i1").getTextContent(), "one");

    EXPECT_TRUE(list.contains(i2));
    EXPECT_TRUE(list.contains(list));
    EXPECT_FALSE(i2.contains(list));
    EXPECT_FALSE(list.contains(UIElement()));
}

TEST_F(ScriptElementTest, Queries){
    EXPECT_EQ(document->querySelector(".item").getId(), "i1");
    EXPECT_EQ(ids(document->querySelectorAll(".item")), (std::vector<std::string>{"i1", "i2", "i3"}));
    EXPECT_FALSE(document->querySelector(".missing").isValid());
    EXPECT_TRUE(document->querySelectorAll(".missing").empty());

    UIElement outer = document->getElementById("outer");
    EXPECT_EQ(outer.querySelector("#i3").getId(), "i3");
    EXPECT_EQ(outer.querySelectorAll("div").size(), 4u);

    UIElement i3 = document->getElementById("i3");
    EXPECT_TRUE(i3.matches(".item"));
    EXPECT_FALSE(i3.matches("#i1"));
    // closest starts at the element itself, as in the DOM
    EXPECT_TRUE(i3.closest(".item") == i3);
    EXPECT_EQ(i3.closest("#outer").getId(), "outer");
    EXPECT_FALSE(i3.closest("#btn").isValid());
}

TEST_F(ScriptElementTest, IdentityAndClasses){
    UIElement outer = document->getElementById("outer");
    EXPECT_EQ(outer.getClassName(), "a b");
    outer.setClassName("x y");
    EXPECT_TRUE(outer.containsClass("x"));
    EXPECT_FALSE(outer.containsClass("a"));

    outer.addClass("z");
    EXPECT_TRUE(outer.hasClass("z"));
    outer.removeClass("z");
    EXPECT_FALSE(outer.hasClass("z"));
    EXPECT_TRUE(outer.toggleClass("t"));
    EXPECT_TRUE(outer.containsClass("t"));
    EXPECT_FALSE(outer.toggleClass("t"));
    EXPECT_FALSE(outer.containsClass("t"));
    EXPECT_TRUE(outer.toggleClass("f", true));
    EXPECT_TRUE(outer.toggleClass("f", true));
    EXPECT_FALSE(outer.toggleClass("f", false));
    EXPECT_FALSE(outer.containsClass("f"));

    outer.setId("renamed");
    EXPECT_EQ(outer.getId(), "renamed");
    EXPECT_TRUE(document->getElementById("renamed") == outer);
}

TEST_F(ScriptElementTest, AttributesAndDataset){
    UIElement outer = document->getElementById("outer");
    std::map<std::string, std::string> attributes = outer.getAttributes();
    EXPECT_EQ(attributes["id"], "outer");
    EXPECT_EQ(attributes["title"], "T");
    EXPECT_EQ(attributes["data-user-name"], "ann");

    EXPECT_EQ(outer.getDataset("userName"), "ann");
    outer.setDataset("gameScore", "12");
    EXPECT_EQ(outer.getAttribute("data-game-score"), "12");
    EXPECT_EQ(outer.getDataset("missing"), "");
}

TEST_F(ScriptElementTest, Style){
    UIElement btn = document->getElementById("btn");
    EXPECT_EQ(btn.getStyle("width"), "100px");
    btn.setStyle("width", "150px");
    update();
    EXPECT_EQ(btn.getStyle("width"), "150px");
    EXPECT_FLOAT_EQ(btn.getOffsetWidth(), 150.0f);
    // "" removes the inline declaration: the stylesheet's value again
    btn.setStyle("width", "");
    update();
    EXPECT_EQ(btn.getStyle("width"), "100px");
    btn.setStyle("height", "10px");
    btn.removeStyle("height");
    update();
    EXPECT_FLOAT_EQ(btn.getOffsetHeight(), 40.0f);
    EXPECT_EQ(btn.getStyle("no-such-property"), "");
}

TEST_F(ScriptElementTest, ContentAndMarkup){
    UIElement label = document->getElementById("label");
    label.setTextContent("<b>x</b>");
    EXPECT_EQ(label.getTextContent(), "<b>x</b>");
    UIElement list = document->getElementById("list");
    list.setInnerHTML("<div id=\"n1\" class=\"item\">new</div>");
    EXPECT_EQ(list.getChildElementCount(), 1);
    EXPECT_EQ(document->getElementById("n1").getTextContent(), "new");
    EXPECT_NE(list.getInnerHTML().find("n1"), std::string::npos);
}

TEST_F(ScriptElementTest, FormControls){
    UIElement text = document->getElementById("text");
    EXPECT_EQ(text.getValue(), "hello");
    text.setValue("world");
    EXPECT_EQ(text.getValue(), "world");

    UIElement check = document->getElementById("check");
    EXPECT_FALSE(check.getChecked());
    check.setChecked(true);
    EXPECT_TRUE(check.getChecked());
    check.setChecked(false);
    EXPECT_FALSE(check.getChecked());

    EXPECT_FALSE(text.getDisabled());
    text.setDisabled(true);
    EXPECT_TRUE(text.getDisabled());
    EXPECT_TRUE(text.hasAttribute("disabled"));
    text.setDisabled(false);
    EXPECT_FALSE(text.getDisabled());
}

TEST_F(ScriptElementTest, FocusAndClick){
    UIElement text = document->getElementById("text");
    text.focus();
    update();
    EXPECT_TRUE(text.isPseudoClassSet("focus"));
    text.blur();
    update();
    EXPECT_FALSE(text.isPseudoClassSet("focus"));

    int clicks = 0;
    UIElement btn = document->getElementById("btn");
    btn.addEventListener("click", [&](UIEvent& event){
        clicks++;
        EXPECT_EQ(event.getType(), "click");
        EXPECT_TRUE(event.getTarget() == btn);
    });
    btn.click();
    EXPECT_EQ(clicks, 1);
}

TEST_F(ScriptElementTest, ScrollAndGeometry){
    UIElement scroller = document->getElementById("scroller");
    EXPECT_FLOAT_EQ(scroller.getClientHeight(), 50.0f);
    EXPECT_GE(scroller.getScrollHeight(), 400.0f);
    scroller.setScrollTop(120);
    EXPECT_FLOAT_EQ(scroller.getScrollTop(), 120.0f);
    scroller.scrollBy(0, 30);
    EXPECT_FLOAT_EQ(scroller.getScrollTop(), 150.0f);
    scroller.scrollTo(0, 10);
    EXPECT_FLOAT_EQ(scroller.getScrollTop(), 10.0f);
    scroller.setScrollLeft(0);
    EXPECT_FLOAT_EQ(scroller.getScrollLeft(), 0.0f);
    EXPECT_GT(scroller.getScrollWidth(), 0.0f);
    EXPECT_GT(scroller.getClientWidth(), 0.0f);

    UIElement btn = document->getElementById("btn");
    EXPECT_FLOAT_EQ(btn.getOffsetLeft(), 300.0f);
    EXPECT_FLOAT_EQ(btn.getOffsetTop(), 20.0f);
    EXPECT_FLOAT_EQ(btn.getOffsetWidth(), 100.0f);
    EXPECT_FLOAT_EQ(btn.getOffsetHeight(), 40.0f);
    UIRect rect = btn.getBoundingClientRect();
    EXPECT_FLOAT_EQ(rect.x, 300.0f);
    EXPECT_FLOAT_EQ(rect.y, 20.0f);
    EXPECT_FLOAT_EQ(rect.width, 100.0f);
    EXPECT_FLOAT_EQ(rect.height, 40.0f);
    EXPECT_FLOAT_EQ(rect.right, 400.0f);
    EXPECT_FLOAT_EQ(rect.bottom, 60.0f);

    // transforms (as of the last drawn frame, like RmlUi's hit test) move the client rect
    ASSERT_TRUE(fixture->waitUntilLoaded());
    UIRect moved = document->getElementById("moved").getBoundingClientRect();
    EXPECT_NEAR(moved.left, 40.0f, 0.01f);
    EXPECT_NEAR(moved.right, 90.0f, 0.01f);
    EXPECT_NEAR(moved.top, 300.0f, 0.01f);

    // scrollIntoView scrolls the container to the child
    scroller.setScrollTop(0);
    UIElement tall = document->getElementById("tall");
    tall.setInnerHTML("<div style=\"height: 10px; margin-top: 300px;\" id=\"deep\"></div>");
    update();
    document->getElementById("deep").scrollIntoView(true);
    EXPECT_GT(scroller.getScrollTop(), 200.0f);
}

TEST_F(ScriptElementTest, MutationAndDetachedPool){
    UIElement list = document->getElementById("list");
    UIElement created = document->createElement("div");
    ASSERT_TRUE(created.isValid());
    EXPECT_FALSE(created.getParentElement().isValid());
    created.setId("c1");
    created.addClass("item");
    created.setTextContent("four");
    EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 1u);

    EXPECT_TRUE(list.appendChild(created) == created);
    EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 0u);
    EXPECT_EQ(ids(list.getChildren()), (std::vector<std::string>{"i1", "i2", "i3", "c1"}));
    EXPECT_TRUE(document->getElementById("c1") == created);

    // insertBefore moves an element that is already in the tree
    list.insertBefore(created, document->getElementById("i1"));
    EXPECT_EQ(ids(list.getChildren()), (std::vector<std::string>{"c1", "i1", "i2", "i3"}));
    // a null reference appends
    list.insertBefore(created, UIElement());
    EXPECT_EQ(ids(list.getChildren()), (std::vector<std::string>{"i1", "i2", "i3", "c1"}));

    // removed children stay usable and can be inserted again
    UIElement i2 = document->getElementById("i2");
    UIElement removed = list.removeChild(i2);
    EXPECT_TRUE(removed == i2);
    EXPECT_TRUE(i2.isValid());
    EXPECT_EQ(i2.getTextContent(), "two");
    EXPECT_FALSE(document->getElementById("i2").isValid());
    update();
    EXPECT_TRUE(i2.isValid()) << "a detached element with a handle survives the update";
    list.appendChild(i2);
    EXPECT_EQ(ids(list.getChildren()), (std::vector<std::string>{"i1", "i3", "c1", "i2"}));

    // replaceChild returns the old child, detached
    UIElement text = document->createTextNode("plain");
    EXPECT_EQ(text.getTextContent(), "plain");
    UIElement old = list.replaceChild(document->createElement("p"), document->getElementById("i1"));
    EXPECT_EQ(old.getId(), "i1");
    EXPECT_FALSE(old.getParentElement().isValid());
    EXPECT_EQ(list.getFirstElementChild().getTagName(), "p");

    // remove()
    created.remove();
    EXPECT_FALSE(document->getElementById("c1").isValid());
    EXPECT_TRUE(created.isValid());

    // cloneNode: deep copies the children, shallow does not
    UIElement outer = document->getElementById("outer");
    UIElement deep = outer.cloneNode(true);
    UIElement shallow = outer.cloneNode(false);
    EXPECT_EQ(deep.getId(), "outer");
    EXPECT_GT(deep.getChildElementCount(), 0);
    EXPECT_EQ(shallow.getChildElementCount(), 0);
    EXPECT_TRUE(shallow.hasClass("a"));

    // DOM errors: a cycle, a reference that is not a child
    TestSystem::get().clearLogs();
    EXPECT_FALSE(i2.appendChild(list).isValid());
    EXPECT_FALSE(list.insertBefore(document->createElement("b"), outer).isValid());
    EXPECT_FALSE(list.removeChild(outer).isValid());
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 3u);
    TestSystem::get().clearLogs();
}

TEST_F(ScriptElementTest, DetachedElementsFreedWithoutHandles){
    {
        UIElement created = document->createElement("div");
        UIElement child = document->createElement("span");
        created.appendChild(child);
        EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 1u);
        // a handle of a descendant keeps the detached tree
        created = UIElement();
        update();
        EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 1u);
        EXPECT_TRUE(child.isValid());
    }
    update();
    EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 0u);

    // an element removed while a handle from before the removal exists
    UIElement i3 = document->getElementById("i3");
    document->getElementById("list").removeChild(i3);
    update();
    EXPECT_TRUE(i3.isValid());
    i3 = UIElement();
    update();
    EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 0u);
}

TEST_F(ScriptElementTest, HandlesAfterUnload){
    UIElement btn = document->getElementById("btn");
    UIElement created = document->createElement("div");
    ASSERT_TRUE(btn.isValid());

    ASSERT_TRUE(document->load("ecs/basic.rml"));
    EXPECT_FALSE(btn.isValid());
    EXPECT_FALSE(created.isValid()) << "the detached pool goes with its document";
    EXPECT_EQ(HtmlUIElementRegistry::poolSize(), 0u);

    // every method returns a default and logs a warning once per method (process-wide: an
    // earlier test may have used it already), never again
    EXPECT_EQ(btn.getId(), "");
    EXPECT_FALSE(btn.appendChild(created).isValid());
    EXPECT_FLOAT_EQ(btn.getBoundingClientRect().width, 0.0f);
    EXPECT_FALSE(btn.getParentElement().isValid());
    EXPECT_LE(TestSystem::get().countLogs(D_LOG_WARN, "(UIElement::getId)"), 1u);
    TestSystem::get().clearLogs();
    EXPECT_EQ(btn.getId(), "");
    EXPECT_FALSE(btn.appendChild(created).isValid());
    btn.getBoundingClientRect();
    btn.getParentElement();
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, kHtmlUIDeadElementMessage), 0u);
    TestSystem::get().clearLogs();
}

TEST_F(ScriptElementTest, DocumentApi){
    EXPECT_EQ(document->getTitle(), "script dom");
    document->setTitle("renamed");
    EXPECT_EQ(document->getTitle(), "renamed");
    EXPECT_TRUE(document->isLoaded());
    EXPECT_EQ(document->getTargetTexture(), nullptr);

    int clicks = 0;
    uint64_t id = document->addEventListener("click", [&](UIEvent&){ clicks++; });
    EXPECT_NE(id, 0u);
    document->getElementById("btn").click();
    EXPECT_EQ(clicks, 1);
    document->removeEventListener(id);
    document->getElementById("btn").click();
    EXPECT_EQ(clicks, 1);

    UIDocument::setDebuggerVisible(false);
    EXPECT_FALSE(UIDocument::isDebuggerVisible());
}
