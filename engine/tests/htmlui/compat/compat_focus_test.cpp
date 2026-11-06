// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// FocusTracker (phase-2 §3.8, §3.11.4): :html-focus only on the focused element (never its
// ancestors, never a document), :html-focus-visible only when RmlUi marks the focus visible
// (keyboard navigation, text fields, focus(true)), across Tab and click sequences, blur,
// removal of the focused element.
//
// Phase 3 (p3-cascade): with the fork's FocusPseudoClasses present, updateRuntime no longer runs
// FocusTracker (the fork's :focus is leaf-only, :focus-within and :focus-visible are native and
// compat passes them through): CompatFocusTest drives FocusTracker itself, as it would run with
// the feature absent; CompatNativeFocus checks the present state.

#include "PerfFixtures.h"
#include "RuntimeTestCommon.h"

#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/runtime/FocusTracker.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::compat;
using namespace jasperengine::htmlui::compat;

namespace {

    class CompatFocusTest : public RuntimeContextTest {
    protected:
        // phase 2's emulation, run before each Context::Update as updateRuntime runs it while
        // FocusPseudoClasses is absent
        void frame(){
            FocusTracker::update(context);
            RuntimeContextTest::frame();
        }
        void click(Rml::Element* element, int modifiers = 0){
            RuntimeContextTest::click(element, modifiers);
            frame();
        }
        void key(Rml::Input::KeyIdentifier identifier, int modifiers = 0){
            RuntimeContextTest::key(identifier, modifiers);
            frame();
        }

        bool focus(const char* id){
            return byId(id)->IsPseudoClassSet(FocusTracker::kFocus);
        }

        bool visible(const char* id){
            return byId(id)->IsPseudoClassSet(FocusTracker::kFocusVisible);
        }

        // how many elements of the document carry :html-focus
        int focusCount(Rml::Element* element){
            int count = element->IsPseudoClassSet(FocusTracker::kFocus) ? 1 : 0;
            for (int i = 0; i < element->GetNumChildren(); i++)
                count += focusCount(element->GetChild(i));
            return count;
        }

        Rml::ElementDocument* load(){
            return show("<div id='outer'><div id='one' class='item'>one</div><input id='text' type='text'/>"
                        "<div id='two' class='item'>two</div></div>",
                        ".item, input { tab-index: auto; focus: auto; width: 100px; height: 20px; } "
                        ".item:html-focus { width: 120px; } .item:html-focus-visible { height: 30px; } "
                        "#outer:html-focus { width: 7px; }");
        }
    };

}

TEST_F(CompatFocusTest, NothingFocusedAfterLoad){
    ASSERT_NE(load(), nullptr);
    frame();   // the native fork skips emulation in updateRuntime
    // the document has RmlUi's :focus after Show(); it never gets :html-focus
    EXPECT_TRUE(document->IsPseudoClassSet("focus"));
    EXPECT_FALSE(document->IsPseudoClassSet(FocusTracker::kFocus));
    EXPECT_EQ(focusCount(document), 0);
    EXPECT_EQ(FocusTracker::focused(context), nullptr);
}

TEST_F(CompatFocusTest, ClickFocusesTheLeafWithoutVisibleFocus){
    ASSERT_NE(load(), nullptr);
    click(byId("one"));
    ASSERT_EQ(context->GetFocusElement(), byId("one"));
    EXPECT_TRUE(focus("one"));
    EXPECT_FALSE(visible("one"));
    // the ancestors have :focus-within (the fork's; phase 2's RmlUi set :focus on the chain), not :html-focus
    EXPECT_TRUE(byId("outer")->IsPseudoClassSet("focus-within"));
    EXPECT_FALSE(focus("outer"));
    EXPECT_EQ(focusCount(document), 1);
    frame();
    EXPECT_FLOAT_EQ(byId("one")->GetBox().GetSize().x, 120.0f);
    EXPECT_FLOAT_EQ(byId("one")->GetBox().GetSize().y, 20.0f);
    EXPECT_NE(byId("outer")->GetBox().GetSize().x, 7.0f);
}

TEST_F(CompatFocusTest, TabFocusIsVisibleAndMoves){
    ASSERT_NE(load(), nullptr);
    click(byId("one"));
    key(Rml::Input::KI_TAB);
    // the text field after "one", by keyboard: visible
    ASSERT_EQ(context->GetFocusElement(), byId("text"));
    EXPECT_FALSE(focus("one"));
    EXPECT_FALSE(visible("one"));
    EXPECT_TRUE(focus("text"));
    EXPECT_TRUE(visible("text"));

    key(Rml::Input::KI_TAB);
    ASSERT_EQ(context->GetFocusElement(), byId("two"));
    EXPECT_TRUE(focus("two"));
    EXPECT_TRUE(visible("two"));
    EXPECT_FALSE(focus("text"));
    EXPECT_EQ(focusCount(document), 1);
    frame();
    EXPECT_FLOAT_EQ(byId("two")->GetBox().GetSize().x, 120.0f);
    EXPECT_FLOAT_EQ(byId("two")->GetBox().GetSize().y, 30.0f);

    // shift+tab goes back
    key(Rml::Input::KI_TAB, Rml::Input::KM_SHIFT);
    EXPECT_TRUE(focus("text"));
    EXPECT_FALSE(focus("two"));
}

TEST_F(CompatFocusTest, TextFieldsShowFocusOnClick){
    ASSERT_NE(load(), nullptr);
    click(byId("text"));
    EXPECT_TRUE(focus("text"));
    EXPECT_TRUE(visible("text"));   // as Chromium: a text field always shows its focus
}

TEST_F(CompatFocusTest, ScriptFocusBlurAndRemoval){
    ASSERT_NE(load(), nullptr);
    ASSERT_TRUE(byId("two")->Focus(false));
    frame();
    EXPECT_TRUE(focus("two"));
    EXPECT_FALSE(visible("two"));
    // focus(true) on another element: visible there
    ASSERT_TRUE(byId("one")->Focus(true));
    frame();
    EXPECT_TRUE(visible("one"));
    EXPECT_FALSE(focus("two"));

    // RmlUi's blur hands the focus to the parent (focusable unless `focus: none`): the
    // pseudo-classes follow RmlUi's focus element, on the leaf only
    byId("one")->Blur();
    frame();
    EXPECT_FALSE(focus("one"));
    EXPECT_FALSE(visible("one"));
    EXPECT_EQ(FocusTracker::focused(context), context->GetFocusElement() == document ? nullptr : context->GetFocusElement());
    EXPECT_LE(focusCount(document), 1);

    // the focused element goes away: nothing dangles, at most the new focus holds it
    ASSERT_TRUE(byId("two")->Focus(true));
    frame();
    EXPECT_TRUE(focus("two"));
    Rml::Element* two = byId("two");
    two->GetParentNode()->RemoveChild(two);
    frame();
    frame();
    EXPECT_LE(focusCount(document), 1);
    Rml::Element* now = context->GetFocusElement();
    EXPECT_EQ(FocusTracker::focused(context), (now == document || now == nullptr) ? nullptr : now);
}

TEST_F(CompatFocusTest, PerContext){
    ASSERT_NE(load(), nullptr);
    Rml::Context* other = Rml::CreateContext(name + "-other", Rml::Vector2i(200, 100), &recording);
    ASSERT_NE(other, nullptr);
    Rml::ElementDocument* second = other->LoadDocumentFromMemory(
        "<rml><head><style>div { display: block; tab-index: auto; focus: auto; width: 50px; height: 20px; }</style></head>"
        "<body><div id='x'/></body></rml>", "compat/runtime/other.rml");
    ASSERT_NE(second, nullptr);
    second->Show();
    second->GetElementById("x")->Focus(true);
    byId("one")->Focus(false);
    FocusTracker::update(other);
    other->Update();
    frame();
    EXPECT_TRUE(second->GetElementById("x")->IsPseudoClassSet(FocusTracker::kFocusVisible));
    EXPECT_TRUE(focus("one"));
    EXPECT_EQ(FocusTracker::focused(other), second->GetElementById("x"));
    EXPECT_EQ(FocusTracker::focused(context), byId("one"));
    other->UnloadAllDocuments();
    other->Update();
    Rml::RemoveContext(name + "-other");
}

// phase-2 §3.14: "FocusTracker per frame <= 1 us": the update of a frame in which the focus
// did not move (every frame but a few), with an element focused
TEST_F(CompatFocusTest, PerFrameCost){
    ASSERT_NE(load(), nullptr);
    click(byId("one"));
    ASSERT_TRUE(focus("one"));
#ifdef NDEBUG
    using jasperengine::test::compat::gatedCpuMs;
    using jasperengine::test::compat::perfSlack;
    const double target = 0.001 * perfSlack();
    const double ms = gatedCpuMs([&](){ FocusTracker::update(context); }, 20000, target);
    std::printf("[perf] FocusTracker::update per frame: %.4f us (target 1 us)\n", ms * 1000.0);
    EXPECT_LE(ms, target);
    EXPECT_TRUE(focus("one"));
#else
    FocusTracker::update(context);
    GTEST_SKIP() << "performance targets are measured in Release builds";
#endif
}

// FocusPseudoClasses present: nothing tracks the focus, the fork's pseudo-classes do it all
TEST_F(RuntimeContextTest, CompatNativeFocus){
    ASSERT_TRUE(ForkFeatures::present().has(ForkFeature::FocusPseudoClasses));
    ASSERT_NE(show("<div id='outer'><div id='one' class='item'>one</div><input id='text' type='text'/></div>",
                   ".item, input { tab-index: auto; focus: auto; width: 100px; height: 20px; } "
                   ".item:focus { width: 120px; } .item:focus-visible { height: 30px; } #outer:focus-within { width: 300px; } "
                   "#outer:focus { height: 7px; }"),
              nullptr);
    click(byId("one"));
    ASSERT_EQ(context->GetFocusElement(), byId("one"));
    EXPECT_EQ(FocusTracker::focused(context), nullptr);
    EXPECT_FALSE(byId("one")->IsPseudoClassSet(FocusTracker::kFocus));
    frame();
    EXPECT_FLOAT_EQ(byId("one")->GetBox().GetSize().x, 120.0f);
    EXPECT_FLOAT_EQ(byId("one")->GetBox().GetSize().y, 20.0f);
    EXPECT_FLOAT_EQ(byId("outer")->GetBox().GetSize().x, 300.0f);
    EXPECT_NE(byId("outer")->GetBox().GetSize().y, 7.0f);

    key(Rml::Input::KI_TAB);
    ASSERT_EQ(context->GetFocusElement(), byId("text"));
    EXPECT_TRUE(byId("text")->IsPseudoClassSet("focus-visible"));
    EXPECT_FALSE(byId("one")->IsPseudoClassSet("focus"));
    frame();
    EXPECT_FLOAT_EQ(byId("one")->GetBox().GetSize().x, 100.0f);
    EXPECT_FLOAT_EQ(byId("outer")->GetBox().GetSize().x, 300.0f);
}
