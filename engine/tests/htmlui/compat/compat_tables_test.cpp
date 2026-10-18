// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The support tables (phase-2 §3.5, Appendices A-E): the lint phase 4 relies on (sorted,
// unique, notes and suggestions), completeness against RmlUi's StyleSheetSpecification and the
// committed W3C property index (fixtures/compat/css-properties.txt), lookups, fork features.

#include "CompatTestCommon.h"

#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/SupportTables.h"

#include "HtmlUI.h"

#include <RmlUi/Core/ID.h>
#include <RmlUi/Core/StyleSheetSpecification.h>

#include <gtest/gtest.h>

#include <cstring>
#include <set>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

    template<typename Row>
    void checkRowContract(const Row& row, const std::string& label){
        if (row.support != Support::Native)
            EXPECT_GT(std::strlen(row.note), 0u) << label << ": a " << supportName(row.support) << " row needs a note";
        if (row.support == Support::Unsupported)
            EXPECT_GT(std::strlen(row.suggestion), 0u) << label << ": an unsupported row needs a suggestion";
        if (row.support == Support::Fallback){
            EXPECT_NE(row.fork, ForkFeature::None) << label << ": a fallback row names its fork feature";
            EXPECT_EQ(row.supportWithFork, Support::Native) << label;
        }
        if (row.fork == ForkFeature::None)
            EXPECT_EQ(row.supportWithFork, row.support) << label << ": without a fork feature the support cannot change";
    }

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

    struct RmlUiSession {
        bool ok;
        RmlUiSession(): ok(HtmlUI::acquire()) {}
        ~RmlUiSession(){ if (ok) HtmlUI::release(); }
    };

}

TEST(CompatTables, PropertyTableSortedUniqueAndLinted){
    const Span<const PropertySupport> table = propertyTable();
    ASSERT_GT(table.size(), 700u);
    for (size_t i = 0; i < table.size(); i++){
        const PropertySupport& row = table[i];
        if (i > 0)
            EXPECT_LT(std::strcmp(table[i - 1].name, row.name), 0) << row.name << " is not after " << table[i - 1].name;
        checkRowContract(row, row.name);
        EXPECT_NE(row.initial, nullptr);
        EXPECT_NE(row.output, nullptr);
        EXPECT_GT(std::strlen(row.spec), 0u) << row.name;
        EXPECT_LT((size_t)row.rewrite, (size_t)RewriteId::Count) << row.name;
        // a property dropped in every fork state is never dispatched to a rewrite (one a fork
        // feature makes supported is, with the feature: phase 3)
        const auto dropped = [](Support support){ return support == Support::Unsupported || support == Support::Ignored; };
        if (dropped(row.support) && (row.fork == ForkFeature::None || dropped(row.supportWithFork)))
            EXPECT_EQ(row.rewrite, RewriteId::None) << row.name;
        EXPECT_EQ(findProperty(row.name), &row) << row.name;
    }
}

TEST(CompatTables, EveryRewriteIdIsUsed){
    std::set<RewriteId> used;
    for (const PropertySupport& row : propertyTable())
        used.insert(row.rewrite);
    for (size_t i = 1; i < (size_t)RewriteId::Count; i++)
        EXPECT_TRUE(used.count((RewriteId)i)) << "RewriteId::" << rewriteIdName((RewriteId)i) << " has no property row";
    EXPECT_STREQ(rewriteIdName(RewriteId::Background), "Background");
    EXPECT_STREQ(rewriteIdName(RewriteId::LineClamp), "LineClamp");
}

TEST(CompatTables, EveryW3CPropertyHasARow){
    const std::vector<std::string> names = readPropertyIndex();
    ASSERT_GT(names.size(), 700u);
    for (const std::string& name : names)
        EXPECT_NE(findProperty(name), nullptr) << "'" << name << "' (W3C property index) has no row";
}

TEST(CompatTables, EveryRmlUiPropertyAndShorthandHasARow){
    RmlUiSession rml;
    ASSERT_TRUE(rml.ok);

    // (phase 3 declares the ids of every feature in the fork's ID.h at once; an id whose package has not
    // registered it yet has no name and nothing parses into it)
    size_t checked = 0;
    for (size_t id = 1; id < (size_t)Rml::PropertyId::NumDefinedIds; id++){
        if (!Rml::StyleSheetSpecification::GetProperty((Rml::PropertyId)id))
            continue;
        const std::string& name = Rml::StyleSheetSpecification::GetPropertyName((Rml::PropertyId)id);
        ASSERT_FALSE(name.empty()) << id;
        EXPECT_NE(findProperty(name), nullptr) << "RmlUi property '" << name << "' has no row";
        checked++;
    }
    for (size_t id = 1; id < (size_t)Rml::ShorthandId::NumDefinedIds; id++){
        if (!Rml::StyleSheetSpecification::GetShorthand((Rml::ShorthandId)id))
            continue;
        const std::string& name = Rml::StyleSheetSpecification::GetShorthandName((Rml::ShorthandId)id);
        ASSERT_FALSE(name.empty()) << id;
        EXPECT_NE(findProperty(name), nullptr) << "RmlUi shorthand '" << name << "' has no row";
        checked++;
    }
    // properties registered at run time (the compat runtime's, phase-3 fork patches)
    for (const Rml::PropertyId id : Rml::StyleSheetSpecification::GetRegisteredProperties()){
        const std::string& name = Rml::StyleSheetSpecification::GetPropertyName(id);
        EXPECT_NE(findProperty(name), nullptr) << "registered property '" << name << "' has no row";
    }
    EXPECT_GT(checked, 100u);
}

TEST(CompatTables, ValueTableGroupedByProperty){
    const Span<const ValueSupport> table = valueTable();
    ASSERT_GT(table.size(), 100u);
    std::set<std::string> finished;
    std::string current;
    std::set<std::string> valuesOfCurrent;
    for (size_t i = 0; i < table.size(); i++){
        const ValueSupport& row = table[i];
        const std::string label = std::string(row.property) + ": " + row.value;
        checkRowContract(row, label);
        EXPECT_NE(findProperty(row.property), nullptr) << label << ": the property has no row";
        if (row.property != current){
            if (!current.empty()){
                finished.insert(current);
                EXPECT_LT(current, std::string(row.property)) << "value table not sorted at " << label;
            }
            EXPECT_FALSE(finished.count(row.property)) << row.property << " rows are not contiguous";
            current = row.property;
            valuesOfCurrent.clear();
        }
        EXPECT_TRUE(valuesOfCurrent.insert(row.value).second) << "duplicate " << label;
    }
    const Span<const ValueSupport> display = valuesOf("display");
    EXPECT_GT(display.size(), 10u);
    for (const ValueSupport& row : display)
        EXPECT_STREQ(row.property, "display");
    EXPECT_TRUE(valuesOf("no-such-property").empty());
}

TEST(CompatTables, OtherTablesLinted){
    for (const SelectorSupport& row : selectorTable())
        checkRowContract(row, row.name);
    for (const AtRuleSupport& row : atRuleTable()){
        checkRowContract(row, row.name);
        EXPECT_EQ(row.name[0], '@') << row.name;
    }
    for (const MediaFeatureSupport& row : mediaFeatureTable())
        checkRowContract(row, row.name);
    for (const InputTypeSupport& row : inputTypeTable()){
        checkRowContract(row, row.type);
        EXPECT_GT(std::strlen(row.rmlType), 0u) << row.type;
        // Appendix D.3: approximations and the unsupported file are reported; native and
        // emulated types (number, reset, image) need no diagnostic of their own
        if (row.reported)
            EXPECT_NE(row.support, Support::Native) << row.type;
        if (row.support == Support::Native)
            EXPECT_FALSE(row.reported) << row.type;
    }
    for (const UnitSupport& row : unitTable())
        checkRowContract(row, row.unit);

    const Span<const ElementSupport> elements = elementTable();
    for (size_t i = 0; i < elements.size(); i++){
        checkRowContract(elements[i], elements[i].name);
        if (i > 0)
            EXPECT_LT(std::strcmp(elements[i - 1].name, elements[i].name), 0) << elements[i].name;
    }
    const Span<const FunctionSupport> functions = functionTable();
    for (size_t i = 0; i < functions.size(); i++){
        checkRowContract(functions[i], functions[i].name);
        if (i > 0)
            EXPECT_LT(std::strcmp(functions[i - 1].name, functions[i].name), 0) << functions[i].name;
    }
    const Span<const AttributeSupport> attributes = attributeTable();
    for (size_t i = 0; i < attributes.size(); i++){
        const AttributeSupport& row = attributes[i];
        checkRowContract(row, std::string(row.element) + " " + row.name);
        if (i > 0){
            const AttributeSupport& previous = attributes[i - 1];
            const int byElement = std::strcmp(previous.element, row.element);
            EXPECT_TRUE(byElement < 0 || (byElement == 0 && std::strcmp(previous.name, row.name) < 0))
                << row.element << " " << row.name << " is not after " << previous.element << " " << previous.name;
        }
    }
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

TEST(CompatTables, EffectiveSupportFollowsForkFeatures){
    const ValueSupport* grid = findValue("display", "grid");
    ASSERT_NE(grid, nullptr);
    EXPECT_EQ(effectiveSupport(*grid, ForkFeatures::none()), Support::Fallback);
    EXPECT_EQ(effectiveSupport(*grid, ForkFeatures::none().with(ForkFeature::Grid, true)), Support::Native);
    EXPECT_EQ(effectiveSupport(*grid, ForkFeatures::all()), Support::Native);
    EXPECT_EQ(effectiveSupport(*grid, ForkFeatures::all().with(ForkFeature::Grid, false)), Support::Fallback);
    EXPECT_EQ(effectiveSupport(Support::Unsupported, ForkFeature::None, Support::Unsupported, ForkFeatures::all()), Support::Unsupported);

    // every row: present() gives the table's own support, or the one with the fork when the
    // row's feature is present
    for (const PropertySupport& row : propertyTable())
        EXPECT_EQ(effectiveSupport(row, ForkFeatures::present()), ForkFeatures::present().has(row.fork) ? row.supportWithFork : row.support) << row.name;
    // calc(), min(), max(), clamp(): native with the fork's Calc (p3-base), folded without it
    for (const char* name : {"calc", "min", "max", "clamp"}){
        const FunctionSupport* function = findFunction(name);
        ASSERT_NE(function, nullptr) << name;
        EXPECT_EQ(effectiveSupport(*function, ForkFeatures::present()), Support::Native) << name;
        EXPECT_EQ(effectiveSupport(*function, ForkFeatures::present().with(ForkFeature::Calc, false)), Support::Fallback) << name;
    }
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
