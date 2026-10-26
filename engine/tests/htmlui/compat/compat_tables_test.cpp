// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Public support information (phase-2 §3.5, Appendices A-E): completeness against the
// committed W3C property index and independently specified lookup results.

#include "CompatTestCommon.h"

#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/SupportTables.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

    std::vector<std::string> readPropertyIndex(){
        std::string text;
        std::vector<std::string> names;
        if (!readFixture("compat/css-properties.txt", text))
            return names;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)){
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            names.push_back(line);
        }
        return names;
    }


}

TEST(CompatTables, EveryW3CPropertyHasARow){
    const std::vector<std::string> names = readPropertyIndex();
    ASSERT_GT(names.size(), 700u);
    for (const std::string& name : names)
        EXPECT_NE(findProperty(name), nullptr) << "'" << name << "' (W3C property index) has no row";
}

TEST(CompatTables, AppendixSamples){
    // A
    const PropertySupport* background = findProperty("background");
    ASSERT_NE(background, nullptr);
    EXPECT_EQ(background->support, Support::Rewritten);
    EXPECT_EQ(background->rewrite, RewriteId::Background);
    const PropertySupport* color = findProperty("color");
    ASSERT_NE(color, nullptr);
    EXPECT_TRUE(color->inherited);
    const PropertySupport* aspect = findProperty("aspect-ratio");
    ASSERT_NE(aspect, nullptr);
    EXPECT_EQ(aspect->support, Support::Unsupported);
    const PropertySupport* custom = findProperty("--brand-color");
    ASSERT_NE(custom, nullptr);
    EXPECT_STREQ(custom->name, "--*");
    EXPECT_EQ(findProperty("colr"), nullptr);
    const PropertySupport* border = findProperty("border");
    ASSERT_NE(border, nullptr);
    EXPECT_EQ(border->fork, ForkFeature::BorderStyle);

    // A.10
    const ValueSupport* grid = findValue("display", "grid");
    ASSERT_NE(grid, nullptr);
    EXPECT_EQ(grid->support, Support::Fallback);
    EXPECT_EQ(grid->fork, ForkFeature::Grid);
    EXPECT_NE(findValue("display", "GRID"), nullptr);
    const ValueSupport* sticky = findValue("position", "sticky");
    ASSERT_NE(sticky, nullptr);
    EXPECT_EQ(sticky->support, Support::Approximated);

    // B
    const SelectorSupport* hover = findPseudoClass("hover");
    ASSERT_NE(hover, nullptr);
    EXPECT_EQ(hover->support, Support::Native);
    EXPECT_EQ(findPseudoClass(":hover"), hover);
    const SelectorSupport* has = findPseudoClass(":has(.x)");
    ASSERT_NE(has, nullptr);
    EXPECT_EQ(has->support, Support::Unsupported);
    const SelectorSupport* is = findPseudoClass("is");
    ASSERT_NE(is, nullptr);
    EXPECT_EQ(is->fork, ForkFeature::RootIsWhere);
    const SelectorSupport* before = findPseudoElement("::before");
    ASSERT_NE(before, nullptr);
    EXPECT_EQ(before->support, Support::Fallback);
    EXPECT_EQ(before->fork, ForkFeature::PseudoElements);
    EXPECT_EQ(findPseudoElement("before"), before);
    const SelectorSupport* thumb = findPseudoElement("-webkit-slider-thumb");
    ASSERT_NE(thumb, nullptr);
    EXPECT_STREQ(thumb->output, "sliderbar");

    // C
    const AtRuleSupport* media = findAtRule("@media");
    ASSERT_NE(media, nullptr);
    EXPECT_EQ(findAtRule("media"), media);
    const AtRuleSupport* container = findAtRule("container");
    ASSERT_NE(container, nullptr);
    EXPECT_EQ(container->support, Support::Unsupported);
    const MediaFeatureSupport* scheme = findMediaFeature("prefers-color-scheme");
    ASSERT_NE(scheme, nullptr);
    EXPECT_EQ(scheme->support, Support::Rewritten);

    // D
    const ElementSupport* html = findElement("html");
    ASSERT_NE(html, nullptr);
    EXPECT_STREQ(html->rmlTag, "body");
    const ElementSupport* body = findElement("BODY");
    ASSERT_NE(body, nullptr);
    EXPECT_STREQ(body->rmlTag, "htmlbody");
    const ElementSupport* canvas = findElement("canvas");
    ASSERT_NE(canvas, nullptr);
    EXPECT_EQ(canvas->support, Support::Unsupported);
    // phase 6: <video> is the engine's element
    const ElementSupport* video = findElement("video");
    ASSERT_NE(video, nullptr);
    EXPECT_EQ(video->support, Support::Emulated);
    const AttributeSupport* playsinline = findAttribute("video", "playsinline");
    ASSERT_NE(playsinline, nullptr);
    EXPECT_EQ(playsinline->support, Support::Ignored);
    const AttributeSupport* onclick = findAttribute("div", "onclick");
    ASSERT_NE(onclick, nullptr);
    EXPECT_EQ(onclick->support, Support::Unsupported);
    const AttributeSupport* data = findAttribute("div", "data-player-name");
    ASSERT_NE(data, nullptr);
    EXPECT_STREQ(data->name, "data-*");
    const AttributeSupport* style = findAttribute("span", "style");
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->support, Support::Rewritten);
    const AttributeSupport* action = findAttribute("form", "action");
    ASSERT_NE(action, nullptr);
    EXPECT_STREQ(action->element, "form");
    const InputTypeSupport* date = findInputType("date");
    ASSERT_NE(date, nullptr);
    EXPECT_STREQ(date->rmlType, "text");
    EXPECT_EQ(date->severity, Severity::Warning);
    const InputTypeSupport* email = findInputType("email");
    ASSERT_NE(email, nullptr);
    EXPECT_EQ(email->severity, Severity::Info);

    // E
    const UnitSupport* q = findUnit("q");
    ASSERT_NE(q, nullptr);
    EXPECT_STREQ(q->unit, "Q");
    const UnitSupport* fr = findUnit("fr");
    ASSERT_NE(fr, nullptr);
    EXPECT_EQ(fr->fork, ForkFeature::Grid);
    const FunctionSupport* calc = findFunction("CALC");
    ASSERT_NE(calc, nullptr);
    EXPECT_EQ(calc->fork, ForkFeature::Calc);
    const FunctionSupport* round = findFunction("round");
    ASSERT_NE(round, nullptr);
    EXPECT_EQ(round->support, Support::Approximated);
    EXPECT_EQ(round->fork, ForkFeature::None);
}

TEST(CompatTables, Names){
    EXPECT_STREQ(supportName(Support::Native), "native");
    EXPECT_STREQ(supportName(Support::Rewritten), "rewritten");
    EXPECT_STREQ(supportName(Support::Approximated), "approximated");
    EXPECT_STREQ(supportName(Support::Emulated), "emulated");
    EXPECT_STREQ(supportName(Support::Fallback), "fallback");
    EXPECT_STREQ(supportName(Support::Unsupported), "unsupported");
    EXPECT_STREQ(supportName(Support::Ignored), "ignored");
    EXPECT_STREQ(selectorKindName(SelectorKind::PseudoClass), "pseudo_class");
    EXPECT_STREQ(selectorKindName(SelectorKind::PseudoElement), "pseudo_element");
    EXPECT_STREQ(selectorKindName(SelectorKind::Selector), "selector");
}

TEST(CompatTables, CssPropertyNameOfRcss){
    EXPECT_EQ(cssPropertyName("decorator"), "background-image");
    EXPECT_EQ(cssPropertyName("font-effect"), "text-shadow");
    EXPECT_EQ(cssPropertyName("-rmlui-direction"), "direction");
    EXPECT_EQ(cssPropertyName("color"), "color");
    EXPECT_EQ(cssPropertyName("box-shadow"), "box-shadow");
    EXPECT_EQ(cssPropertyName("white-space"), "white-space");
    EXPECT_EQ(cssPropertyName("tab-index"), "tab-index");
    EXPECT_EQ(cssPropertyName("not-a-property"), "not-a-property");
}
