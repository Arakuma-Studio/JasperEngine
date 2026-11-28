// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The HTML UI follows the input scheme (docs/specs/input-device-awareness.md AC10-AC13): the
// root attributes without a recompile, visibility="touch-scheme", <input-prompt>, and the hover
// and pointer media features swapped for the pointer in use, and what a switch costs (AC14).
// Frames run through Engine::systemUpdate (the input latch, then the documents' update).

#include "TestDocumentCommon.h"
#include "compat/PerfFixtures.h"

#include "Engine.h"
#include "Input.h"
#include "compat/DocumentLoader.h"
#include "input/InputActions.h"
#include "input/InputScheme.h"
#include "object/ui/UIDocument.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class InputSchemeDocumentTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);
            InputSchemeTracker::setOverride(std::nullopt);
            InputSchemeTracker::reset(false);
            fixture.reset(new SceneFixture());
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            Engine::systemTouchCancel();
            Engine::systemGamepadDisconnect(0);
            fixture.reset();
            InputActions::unload();
            InputActions::resetRuntimeState();
            TestSystem::get().touchDevice = false;
            InputSchemeTracker::reset(false);
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u);
        }

        UIDocument& load(const char* path){
            UIDocument& document = fixture->add<UIDocument>();
            EXPECT_TRUE(document.load(path)) << path;
            frames(3);
            return document;
        }

        static void frames(int count){
            for (int i = 0; i < count; i++)
                Engine::systemUpdate();
        }

        Rml::Element* element(const char* id){
            Rml::Context* context = rmlContext(fixture->getScene());
            for (int i = 0; context && i < context->GetNumDocuments(); i++){
                if (Rml::Element* found = context->GetDocument(i)->GetElementById(id))
                    return found;
            }
            return nullptr;
        }

        bool displayed(const char* id){
            Rml::Element* found = element(id);
            return found && found->GetComputedValues().display() != Rml::Style::Display::None;
        }

        static std::string text(Rml::Element* element){
            std::string out;
            for (int i = 0; element && i < element->GetNumChildren(); i++){
                if (Rml::ElementText* child = rmlui_dynamic_cast<Rml::ElementText*>(element->GetChild(i)))
                    out += child->GetText();
            }
            return out;
        }

        static std::string attribute(Rml::Element* element, const char* name){
            const Rml::Variant* value = element ? element->GetAttribute(name) : nullptr;
            return value ? value->Get<Rml::String>() : std::string("<none>");
        }

        void padPress(const char* name = "DualSense Wireless Controller"){
            if (!Input::isGamepadConnected(0))
                Engine::systemGamepadConnect(0, name);
            Engine::systemGamepadButtonDown(0, D_GAMEPAD_BUTTON_A);
            Engine::systemGamepadButtonUp(0, D_GAMEPAD_BUTTON_A);
        }

        std::unique_ptr<SceneFixture> fixture;
    };

}

TEST_F(InputSchemeDocumentTest, RootAttributeFollowsScheme){
    load("input-scheme/root.html");
    Rml::ElementDocument* document = findDocument(fixture->getScene(), "scheme-root");
    ASSERT_NE(document, nullptr);
    EXPECT_EQ(attribute(document, "data-input-scheme"), "keyboard-mouse");
    EXPECT_EQ(attribute(document, "data-gamepad-family"), "generic");
    EXPECT_FALSE(displayed("pad"));
    const std::shared_ptr<const htmlui::compat::CompatDocument> before = htmlui::compat::DocumentLoader::compiledOf(document);
    ASSERT_NE(before, nullptr);

    padPress();
    frames(2);
    EXPECT_EQ(attribute(document, "data-input-scheme"), "gamepad");
    EXPECT_EQ(attribute(document, "data-gamepad-family"), "playstation");
    EXPECT_TRUE(displayed("pad"));
    EXPECT_FALSE(displayed("touch"));
    EXPECT_FLOAT_EQ(element("pad")->GetBox().GetSize().x, 40.0f);
    // the selectors' values change at runtime: the same compile
    EXPECT_EQ(htmlui::compat::DocumentLoader::compiledOf(document), before);

    Engine::systemTouchStart(1, 300, 300);
    Engine::systemTouchEnd(1, 300, 300);
    frames(2);
    EXPECT_EQ(attribute(document, "data-input-scheme"), "touch");
    EXPECT_FALSE(displayed("pad"));
    EXPECT_TRUE(displayed("touch"));
    EXPECT_EQ(htmlui::compat::DocumentLoader::compiledOf(document), before);

    // a .rml document gets them too
    load("ecs/input.rml");
    Rml::Context* context = rmlContext(fixture->getScene());
    Rml::ElementDocument* rml = nullptr;
    for (int i = 0; i < context->GetNumDocuments(); i++){
        if (!htmlui::compat::DocumentLoader::compiledOf(context->GetDocument(i)))
            rml = context->GetDocument(i);
    }
    ASSERT_NE(rml, nullptr);
    EXPECT_EQ(attribute(rml, "data-input-scheme"), "touch");
    Engine::systemKeyDown(D_KEY_SPACE, false, 0);
    Engine::systemKeyUp(D_KEY_SPACE, false, 0);
    frames(2);
    EXPECT_EQ(attribute(rml, "data-input-scheme"), "keyboard-mouse");
    EXPECT_EQ(attribute(document, "data-input-scheme"), "keyboard-mouse");
}

TEST_F(InputSchemeDocumentTest, TouchSchemeVisibility){
    InputActions::loadDefaults();
    InputActions::resetRuntimeState();
    InputSchemeTracker::reset(true);
    load("input-scheme/controls.html");
    Rml::Element* stick = element("stick");
    ASSERT_NE(stick, nullptr);
    EXPECT_EQ(stick->GetComputedValues().visibility(), Rml::Style::Visibility::Visible);

    // a finger on the stick, half a radius right of its centre
    Engine::systemTouchStart(0, 100, 260);
    Engine::systemTouchMove(0, 140, 260);
    frames(1);
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.5f, 0.01f);

    // a key: hidden, inert, and what it wrote released
    Engine::systemKeyDown(D_KEY_ENTER, false, 0);
    frames(2);
    EXPECT_EQ(InputSchemeTracker::getCurrent(), InputScheme::KeyboardMouse);
    EXPECT_EQ(stick->GetComputedValues().visibility(), Rml::Style::Visibility::Hidden);
    EXPECT_NEAR(InputActions::getAxis("Horizontal"), 0.0f, 0.01f);
    Engine::systemKeyUp(D_KEY_ENTER, false, 0);
    Engine::systemTouchEnd(0, 140, 260);

    // the next finger brings it back
    Engine::systemTouchStart(0, 100, 260);
    Engine::systemTouchEnd(0, 100, 260);
    frames(2);
    EXPECT_EQ(stick->GetComputedValues().visibility(), Rml::Style::Visibility::Visible);
}

TEST_F(InputSchemeDocumentTest, InputPromptUpdates){
    ASSERT_TRUE(InputActions::loadFromText("prompt.inputactions", R"({"version":1,"maps":[{"name":"Gameplay","actions":[
     {"name":"Jump","bindings":[{"id":"00000001","path":"keyboard/space","groups":["Keyboard&Mouse"]},{"id":"00000002","path":"gamepad/a"}]}]}]})"));
    load("input-scheme/prompt.html");
    Rml::Element* jump = element("jump");
    Rml::Element* glyph = element("glyph");
    Rml::Element* touch = element("touch");
    ASSERT_NE(jump, nullptr);
    ASSERT_NE(glyph, nullptr);
    EXPECT_EQ(text(jump), "Space");
    EXPECT_EQ(attribute(jump, "data-glyph"), "keyboard/space");
    EXPECT_EQ(attribute(jump, "data-scheme"), "keyboard-mouse");
    EXPECT_EQ(attribute(jump, "data-empty"), "false");
    // no touch binding
    EXPECT_EQ(text(touch), "");
    EXPECT_EQ(attribute(touch, "data-empty"), "true");
    EXPECT_EQ(attribute(touch, "data-scheme"), "touch");

    padPress();
    frames(2);
    EXPECT_EQ(text(jump), "Cross");
    EXPECT_EQ(attribute(jump, "data-glyph"), "gamepad/playstation/cross");
    EXPECT_EQ(attribute(jump, "data-scheme"), "gamepad");
    // the author's children stay; the attributes follow
    EXPECT_EQ(attribute(glyph, "data-glyph"), "gamepad/playstation/cross");
    ASSERT_EQ(glyph->GetNumChildren(), 1);
    EXPECT_EQ(glyph->GetChild(0)->GetTagName(), "span");

    Engine::systemKeyDown(D_KEY_SPACE, false, 0);
    Engine::systemKeyUp(D_KEY_SPACE, false, 0);
    frames(2);
    EXPECT_EQ(text(jump), "Space");
    // a rebinding shows the next frame
    InputActions::setOverride("00000001", "keyboard/e");
    frames(1);
    EXPECT_EQ(text(jump), "E");
    EXPECT_EQ(attribute(jump, "data-glyph"), "keyboard/e");
}

TEST_F(InputSchemeDocumentTest, HoverFollowsPointer){
    // a touch device: the pointer starts as a finger
    TestSystem::get().touchDevice = true;
    InputSchemeTracker::reset(true);
    const size_t cacheBefore = htmlui::compat::DocumentLoader::cacheSize();
    load("input-scheme/hover.html");
    load("input-scheme/plain.html");
    Rml::ElementDocument* hover = findDocument(fixture->getScene(), "scheme-hover");
    Rml::ElementDocument* plain = findDocument(fixture->getScene(), "scheme-plain");
    ASSERT_NE(hover, nullptr);
    ASSERT_NE(plain, nullptr);
    EXPECT_FALSE(displayed("tip"));
    const std::shared_ptr<const htmlui::compat::CompatDocument> coarse = htmlui::compat::DocumentLoader::compiledOf(hover);
    const std::shared_ptr<const htmlui::compat::CompatDocument> plainCompile = htmlui::compat::DocumentLoader::compiledOf(plain);
    ASSERT_NE(coarse, nullptr);
    EXPECT_TRUE(coarse->dependsOnPointer);
    EXPECT_FALSE(plainCompile->dependsOnPointer);

    // the mouse's variant is compiled in the background after the load (three compiles: two
    // documents and the variant)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (htmlui::compat::DocumentLoader::cacheSize() < cacheBefore + 3 && std::chrono::steady_clock::now() < deadline){
        frames(1);
        std::this_thread::yield();
    }
    const size_t prewarmed = htmlui::compat::DocumentLoader::cacheSize();
    ASSERT_EQ(prewarmed, cacheBefore + 3) << "the other pointer's variant was not prewarmed";

    // the variables the tip reads: --tint follows the variant, --kept stays
    const auto tint = [&](){ return element("tip")->GetComputedValues().color(); };
    const auto kept = [&](){ return element("tip")->GetComputedValues().padding_left().value; };
    EXPECT_EQ(tint(), Rml::Colourb(255, 0, 0, 255));
    EXPECT_FLOAT_EQ(kept(), 4.0f);

    // the mouse: shown, from the cache
    Engine::systemMouseScroll(0, 1, 0);
    frames(2);
    EXPECT_EQ(InputSchemeTracker::getPointer(), InputScheme::KeyboardMouse);
    EXPECT_TRUE(displayed("tip"));
    EXPECT_EQ(tint(), Rml::Colourb(0, 0, 255, 255));
    EXPECT_FLOAT_EQ(kept(), 4.0f);
    const std::shared_ptr<const htmlui::compat::CompatDocument> fine = htmlui::compat::DocumentLoader::compiledOf(hover);
    EXPECT_NE(fine, coarse);
    EXPECT_FALSE(fine->options.coarsePointer);
    EXPECT_EQ(htmlui::compat::DocumentLoader::cacheSize(), prewarmed);
    // a document that never read hover or pointer keeps its compile
    EXPECT_EQ(htmlui::compat::DocumentLoader::compiledOf(plain), plainCompile);

    // a finger: hidden again, the first compile back
    Engine::systemTouchStart(1, 300, 300);
    Engine::systemTouchEnd(1, 300, 300);
    frames(2);
    EXPECT_FALSE(displayed("tip"));
    EXPECT_EQ(tint(), Rml::Colourb(255, 0, 0, 255));
    EXPECT_FLOAT_EQ(kept(), 4.0f);
    EXPECT_EQ(htmlui::compat::DocumentLoader::compiledOf(hover), coarse);
    EXPECT_EQ(htmlui::compat::DocumentLoader::cacheSize(), prewarmed);
    EXPECT_EQ(htmlui::compat::DocumentLoader::compiledOf(plain), plainCompile);
}

// input-device-awareness R23, AC14: a scheme change on a HUD of 120 elements in 3 documents
// (fixtures/bench/input-scheme.html) within 0.5 ms, a pointer change from the prewarmed cache
// within 1 ms (desktop), over a frame without a change. Printed; gated with
// JASPER_HTMLUI_PERF_SLACK (the benchmark run), as the other budgets near a build server's noise.
TEST_F(InputSchemeDocumentTest, SchemeSwitchCost){
    TestSystem::get().touchDevice = true;
    InputSchemeTracker::reset(true);
    for (int i = 0; i < 3; i++)
        load("bench/input-scheme.html");
    ASSERT_EQ(documentCount(fixture->getScene()), 3);
    // the prewarmed variant of the hover query (one document file: one variant)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::shared_ptr<const htmlui::compat::CompatDocument> compiled =
        htmlui::compat::DocumentLoader::compiledOf(findDocument(fixture->getScene(), "input-scheme bench"));
    ASSERT_TRUE(compiled && compiled->dependsOnPointer);
    htmlui::compat::CompatOptions fine = compiled->options;
    fine.coarsePointer = false;
    while (!htmlui::compat::DocumentLoader::findCompiled(compiled->sourcePath, fine) && std::chrono::steady_clock::now() < deadline){
        frames(1);
        std::this_thread::yield();
    }
    ASSERT_NE(htmlui::compat::DocumentLoader::findCompiled(compiled->sourcePath, fine), nullptr) << "the other pointer's variant was not prewarmed";
    // the frame after it is in parses both variants' sheets for each document (HtmlUIContext::update)
    frames(1);
    Scene& scene = fixture->getScene();
    Rml::Context* context = rmlContext(scene);
    auto containers = [&](){
        std::vector<const Rml::StyleSheetContainer*> out;
        for (int i = 0; i < context->GetNumDocuments(); i++)
            out.push_back(context->GetDocument(i)->GetStyleSheetContainer());
        return out;
    };
    const auto pointerSwitch = [&](bool toMouse){
        if (toMouse) InputSchemeTracker::noteMouseScroll();
        else InputSchemeTracker::noteTouchStart(1, 1, InputSchemeTracker::clock());
        InputSchemeTracker::latch();
        updateDocuments(scene);
    };
    // the first switch after the load: from the prewarmed sheets, nothing parsed then
    double start = compat::threadCpuMs();
    pointerSwitch(true);
    const double firstMs = compat::threadCpuMs() - start;
    const std::vector<const Rml::StyleSheetContainer*> mouseSheets = containers();
    pointerSwitch(false);
    const std::vector<const Rml::StyleSheetContainer*> touchSheets = containers();
    pointerSwitch(true);
    EXPECT_EQ(containers(), mouseSheets) << "a swap back reuses the document's parsed sheets";
    pointerSwitch(false);
    EXPECT_EQ(containers(), touchSheets);
    const double steadyMs = compat::medianCpuMs([&](){ InputSchemeTracker::latch(); updateDocuments(scene); }, 10);
    // Gamepad <-> Touch: the attributes only (the pointer stays a finger)
    bool pad = false;
    const double schemeMs = compat::medianCpuMs([&](){
        pad = !pad;
        if (pad) InputSchemeTracker::noteGamepadButton(0);
        else InputSchemeTracker::noteTouchStart(1, 1, InputSchemeTracker::clock());
        InputSchemeTracker::latch();
        updateDocuments(scene);
    }, 10);
    // Touch <-> Keyboard&Mouse: the attributes and the hover variant swapped
    bool mouse = false;
    const size_t before = htmlui::compat::DocumentLoader::cacheSize();
    const double pointerMs = compat::medianCpuMs([&](){
        mouse = !mouse;
        if (mouse) InputSchemeTracker::noteMouseScroll();
        else InputSchemeTracker::noteTouchStart(1, 1, InputSchemeTracker::clock());
        InputSchemeTracker::latch();
        updateDocuments(scene);
    }, 10);
    // every swap was a cache hit
    EXPECT_EQ(htmlui::compat::DocumentLoader::cacheSize(), before);
    const double scheme = std::max(0.0, schemeMs - steadyMs);
    const double pointer = std::max(0.0, pointerMs - steadyMs);
    const double first = std::max(0.0, firstMs - steadyMs);
    std::printf("[bench] input scheme change %.3f ms, pointer change %.3f ms (the first after the load %.3f ms; a frame "
                "without one %.3f ms)\n", scheme, pointer, first, steadyMs);
    if (compat::benchmarkPerfSlack() > 0.0){
        EXPECT_LE(scheme, 0.5 * compat::benchmarkPerfSlack());
        EXPECT_LE(pointer, 1.0 * compat::benchmarkPerfSlack());
        EXPECT_LE(first, 1.0 * compat::benchmarkPerfSlack());
    }
}
