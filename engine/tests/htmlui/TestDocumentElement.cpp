// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIElement: the minimal phase-0 element API over a loaded document (text, markup,
// attributes, classes, inline style, boxes) and its weak reference semantics.

#include "TestDocumentCommon.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class UIElementTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            fixture.reset(new SceneFixture());
            document = &fixture->add<UIDocument>();
            ASSERT_TRUE(document->load("ecs/basic.rml"));
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            fixture.reset();
            EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
        }

        Scene& scene(){
            return fixture->getScene();
        }

        std::unique_ptr<SceneFixture> fixture;
        UIDocument* document = nullptr;
    };

}

TEST_F(UIElementTest, TextContent){
    UIElement label = document->getElementById("label");
    ASSERT_TRUE(label.isValid());
    EXPECT_EQ(label.getTagName(), "p");
    EXPECT_EQ(label.getTextContent(), "Hello World");
    EXPECT_EQ(document->getElementById("name").getTextContent(), "World");
    // no text at all
    EXPECT_EQ(document->getElementById("box").getTextContent(), "");

    // replaces the children; markup is text, not parsed
    label.setTextContent("<b>Score</b> &amp; 10");
    EXPECT_EQ(label.getTextContent(), "<b>Score</b> &amp; 10");
    EXPECT_FALSE(document->getElementById("name").isValid());

    label.setTextContent("");
    EXPECT_EQ(label.getTextContent(), "");
    EXPECT_EQ(label.getInnerHTML(), "");

    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}

TEST_F(UIElementTest, InnerHTML){
    UIElement label = document->getElementById("label");
    ASSERT_TRUE(label.isValid());
    const std::string markup = label.getInnerHTML();
    EXPECT_NE(markup.find("Hello"), std::string::npos) << markup;
    EXPECT_NE(markup.find("<span id=\"name\">World</span>"), std::string::npos) << markup;

    label.setInnerHTML("<span id=\"inner\" class=\"x\">Inner &amp; more</span>");
    UIElement inner = document->getElementById("inner");
    ASSERT_TRUE(inner.isValid());
    EXPECT_EQ(inner.getTagName(), "span");
    EXPECT_TRUE(inner.hasClass("x"));
    EXPECT_EQ(label.getTextContent(), "Inner & more");
    EXPECT_FALSE(document->getElementById("name").isValid());
}

TEST_F(UIElementTest, Attributes){
    UIElement box = document->getElementById("box");
    ASSERT_TRUE(box.isValid());
    EXPECT_TRUE(box.hasAttribute("data-value"));
    EXPECT_EQ(box.getAttribute("data-value"), "7");
    EXPECT_FALSE(box.hasAttribute("title"));
    EXPECT_EQ(box.getAttribute("title"), "");

    box.setAttribute("title", "Box title");
    EXPECT_TRUE(box.hasAttribute("title"));
    EXPECT_EQ(box.getAttribute("title"), "Box title");

    box.setAttribute("data-value", "8");
    EXPECT_EQ(box.getAttribute("data-value"), "8");

    box.removeAttribute("data-value");
    EXPECT_FALSE(box.hasAttribute("data-value"));
    // removing a missing attribute is harmless
    box.removeAttribute("data-value");

    // id is an attribute too
    box.setAttribute("id", "renamed");
    EXPECT_EQ(box.getId(), "renamed");
    EXPECT_TRUE(document->getElementById("renamed").isValid());
}

TEST_F(UIElementTest, Classes){
    UIElement box = document->getElementById("box");
    ASSERT_TRUE(box.isValid());
    EXPECT_TRUE(box.hasClass("panel"));
    EXPECT_TRUE(box.hasClass("big"));
    EXPECT_FALSE(box.hasClass("small"));

    box.setClass("small", true);
    EXPECT_TRUE(box.hasClass("small"));
    box.setClass("big", false);
    EXPECT_FALSE(box.hasClass("big"));
    EXPECT_TRUE(box.hasClass("panel"));

    // the class attribute sets the whole list
    box.setAttribute("class", "one two");
    EXPECT_TRUE(box.hasClass("one"));
    EXPECT_TRUE(box.hasClass("two"));
    EXPECT_FALSE(box.hasClass("panel"));
}

TEST_F(UIElementTest, StyleProperty){
    UIElement box = document->getElementById("box");
    ASSERT_TRUE(box.isValid());

    EXPECT_TRUE(box.setStyleProperty("width", "50px"));
    updateDocuments(scene());
    EXPECT_FLOAT_EQ(box.getBoundingBox().getWidth(), 70.0f);   // + padding

    // rejected: false and one warning naming the declaration
    TestSystem::get().clearLogs();
    EXPECT_FALSE(box.setStyleProperty("width", "wide"));
    EXPECT_FALSE(box.setStyleProperty("no-such-property", "1px"));
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "width: wide"), 1u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "no-such-property: 1px"), 1u);
    updateDocuments(scene());
    EXPECT_FLOAT_EQ(box.getBoundingBox().getWidth(), 70.0f);

    // back to the stylesheet's value
    box.removeStyleProperty("width");
    updateDocuments(scene());
    EXPECT_FLOAT_EQ(box.getBoundingBox().getWidth(), 220.0f);

    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}

TEST_F(UIElementTest, BoundingBox){
    // left 40, top 30, 200x100 content + 10 px padding: the border box
    const Rect box = document->getElementById("box").getBoundingBox();
    EXPECT_FLOAT_EQ(box.getX(), 40.0f);
    EXPECT_FLOAT_EQ(box.getY(), 30.0f);
    EXPECT_FLOAT_EQ(box.getWidth(), 220.0f);
    EXPECT_FLOAT_EQ(box.getHeight(), 120.0f);

    const Rect label = document->getElementById("label").getBoundingBox();
    EXPECT_FLOAT_EQ(label.getX(), 300.0f);
    EXPECT_FLOAT_EQ(label.getY(), 30.0f);
    EXPECT_FLOAT_EQ(label.getWidth(), 200.0f);
    EXPECT_FLOAT_EQ(label.getHeight(), 40.0f);

    const Rect body = document->getBody().getBoundingBox();
    EXPECT_FLOAT_EQ(body.getX(), 0.0f);
    EXPECT_FLOAT_EQ(body.getY(), 0.0f);
    EXPECT_FLOAT_EQ(body.getWidth(), (float)Engine::getCanvasWidth());
    EXPECT_FLOAT_EQ(body.getHeight(), (float)Engine::getCanvasHeight());
}

TEST_F(UIElementTest, PseudoClasses){
    UIElement box = document->getElementById("box");
    EXPECT_FALSE(box.isPseudoClassSet("hover"));
    EXPECT_FALSE(box.isPseudoClassSet("active"));
    EXPECT_FALSE(box.isPseudoClassSet("focus"));
}

TEST_F(UIElementTest, InvalidAfterUnload){
    UIElement box = document->getElementById("box");
    UIElement copy = box;   // value type: copies share the reference
    ASSERT_TRUE(box.isValid());

    document->setFilename("");
    updateDocuments(scene());

    for (const UIElement& element : {box, copy}){
        EXPECT_FALSE(element.isValid());
        EXPECT_EQ(element.getId(), "");
        EXPECT_EQ(element.getTagName(), "");
        EXPECT_EQ(element.getTextContent(), "");
        EXPECT_EQ(element.getInnerHTML(), "");
        EXPECT_FALSE(element.hasAttribute("data-value"));
        EXPECT_EQ(element.getAttribute("data-value"), "");
        EXPECT_FALSE(element.hasClass("panel"));
        EXPECT_FALSE(element.isPseudoClassSet("hover"));
        const Rect rect = element.getBoundingBox();
        EXPECT_EQ(rect.getWidth(), 0.0f);
        EXPECT_EQ(rect.getHeight(), 0.0f);
    }

    // setters are no-ops
    box.setTextContent("x");
    box.setInnerHTML("<div/>");
    box.setAttribute("a", "b");
    box.removeAttribute("a");
    box.setClass("c", true);
    EXPECT_FALSE(box.setStyleProperty("width", "10px"));
    box.removeStyleProperty("width");

    // a default element is invalid the same way
    UIElement none;
    EXPECT_FALSE(none.isValid());
    EXPECT_EQ(none.getId(), "");

    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}
