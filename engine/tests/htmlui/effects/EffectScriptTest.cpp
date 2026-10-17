// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The effects from Lua and the runtime fallback form (phase-5 spec 3.4 item 8, 3.5, 3.12, §6):
// - UIDocument.setEffectTime pins the effect clock (the one goldens and set_ui_effect_time pin),
//   getEffectTime reads it, nil runs it again; UIElement:getEffects lists an element's effects.
// - effect() that reaches RmlUi without compat (set on an RmlUi element, as an RCSS sheet or C++
//   code would) is instanced by the fallback `effect` instancers, draws, is listed, and its problems
//   go to the Console: with the element's address, or the call's text when the effect is unknown.

#include "EffectTestCommon.h"
#include "TestScriptCommon.h"

#include "htmlui/effects/EffectInstancers.h"

#include <set>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;

namespace {

    class EffectScriptTest : public ScriptTest {
    protected:
        const char* documentFile() const override { return "effects/script.html"; }
    };

    size_t colours(const TestImage& image, const Rml::Rectanglef& box){
        std::set<uint32_t> distinct;
        for (int y = std::max(0, (int)box.Top()); y < std::min(image.height, (int)box.Bottom()); y++){
            for (int x = std::max(0, (int)box.Left()); x < std::min(image.width, (int)box.Right()); x++){
                const uint8_t* p = &image.rgba[((size_t)y * image.width + x) * 4];
                distinct.insert((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
            }
        }
        return distinct.size();
    }

}

TEST_F(EffectScriptTest, LuaPinsTheEffectClockAndListsEffects){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    EXPECT_EQ(lua(R"(
        UIDocument.setEffectTime(1.5)
        assert(UIDocument.getEffectTime() == 1.5, "getEffectTime: " .. tostring(UIDocument.getEffectTime()))
        local fx = doc:getElementById("hero"):getEffects()
        assert(#fx == 1, "hero: " .. #fx .. " effects")
        assert(fx[1].property == "background-image" and fx[1].index == 1 and fx[1].name == "stripes" and fx[1].form == "paint",
               "hero: " .. tostring(fx[1].property) .. " " .. tostring(fx[1].index) .. " " .. tostring(fx[1].name) .. " " .. tostring(fx[1].form))
        assert(fx[1].params.density == "6" and fx[1].params["time-scale"] == "1", "hero's params")
        local px = doc:getElementById("pix"):getEffects()
        assert(#px == 1 and px[1].property == "filter" and px[1].name == "pixelate" and px[1].form == "filter", "pix")
        assert(px[1].params.size == "8px", "pix size: " .. tostring(px[1].params.size))
    )"), "");
    // the clock every effect reads (and the golden harness pins in C++)
    EXPECT_TRUE(EffectClock::isFixed());
    EXPECT_DOUBLE_EQ(EffectClock::now(), 1.5);
    EXPECT_DOUBLE_EQ(UIDocument::getEffectTime(), 1.5);
    EXPECT_EQ(lua("UIDocument.setEffectTime(nil)"), "");
    EXPECT_FALSE(EffectClock::isFixed());
}

TEST(EffectFallbackTest, EffectsThatBypassCompatAreInstancedAndReported){
    EffectScene scene(360, 240, 1);
    if (!scene.valid())
        GTEST_SKIP() << "no GPU";
    for (const char* name : {"stripes", "pixelate"}){
        const EffectManifest* m = EffectRegistry::get().find(name);
        ASSERT_NE(m, nullptr) << name;
        ASSERT_TRUE(warmUpEffect(*m)) << name;
    }
    std::string error;
    ASSERT_TRUE(scene.loadHtml("<!DOCTYPE html><html><head><style>\n"
                               "html, body { margin: 0; width: 360px; height: 240px; background: #101218; }\n"
                               "div { position: absolute; width: 100px; height: 80px; top: 20px; background-color: #e74c3c; }\n"
                               "#a { left: 10px; background-color: transparent; } #b { left: 130px; } #c { left: 250px; }\n"
                               "</style></head><body><div id=\"a\"></div><div id=\"b\">B</div><div id=\"c\">C</div></body></html>",
                               &error)) << error;
    Rml::Element* a = scene.element("a");
    Rml::Element* b = scene.element("b");
    Rml::Element* c = scene.element("c");
    ASSERT_TRUE(a && b && c);
    TestSystem::get().clearLogs();
    // straight to RmlUi, not through compat (Lua's style goes through compat): the `effect` instancers
    a->SetProperty("background-image", "effect(stripes, density 4, speeed 2)");
    b->SetProperty("filter", "effect(pixelate, size 6px)");
    c->SetProperty("filter", "effect(nosuch)");
    TestImage image;
    {
        FixedEffectClock clock(1.5);
        ASSERT_TRUE(scene.capture(image));
    }

    const std::vector<EffectUse> usesA = collectEffectUses(a);
    ASSERT_EQ(usesA.size(), 1u);
    EXPECT_EQ(usesA[0].name, "stripes");
    EXPECT_FALSE(usesA[0].filterForm);
    bool density = false;
    for (const auto& param : usesA[0].params)
        density = density || (param.first == "density" && param.second == "4");
    EXPECT_TRUE(density) << "the parameter given in the raw call";
    const std::vector<EffectUse> usesB = collectEffectUses(b);
    ASSERT_EQ(usesB.size(), 1u);
    EXPECT_EQ(usesB[0].name, "pixelate");
    EXPECT_TRUE(usesB[0].filterForm);
    EXPECT_TRUE(collectEffectUses(c).empty()) << "an unknown effect is dropped";
    EXPECT_GT(colours(image, scene.box("a")), 2u) << "the fallback decorator draws the stripes";

    // nothing dropped silently: the unknown parameter with the element's address, the unknown effect
    const std::string warnings = logText(D_LOG_WARN);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI] effect(stripes) on " + a->GetAddress()), 1u) << warnings;
    EXPECT_NE(warnings.find("speeed"), std::string::npos) << warnings;
    EXPECT_GE(TestSystem::get().countLogs(D_LOG_WARN, "unknown effect 'nosuch'"), 1u) << warnings;
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}
