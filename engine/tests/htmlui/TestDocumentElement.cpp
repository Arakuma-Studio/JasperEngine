// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIElement error handling: rejected inline declarations preserve the current style, and
// weak handles remain safe after their document unloads. DOM behavior lives in TestScriptElement.

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

TEST_F(UIElementTest, InvalidAfterUnload){
    UIElement box = document->getElementById("box");
    UIElement copy = box;   // value type: copies share the reference
    UIElement detached = document->createElement("div");
    ASSERT_TRUE(box.isValid());

    document->setFilename("");
    updateDocuments(scene());

    for (const UIElement& element : {box, copy, detached}){
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

    TestSystem::get().clearLogs();
    box.getId();
    copy.getId();
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN), 0u) << "repeated dead-handle calls must not flood the console";

    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
}
