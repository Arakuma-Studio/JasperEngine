// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The built-in Lucide pack (phase-2 §3.11.8): every icon of the pinned release resolves,
// aliases and PascalCase/camelCase names resolve as lucide.createIcons() resolves them, the
// closest names for suggestions and the <svg> attributes identical to Lucide's
// replaceElement (fixtures/compat/runtime/lucide-replace-element.json, from
// tools/htmlui/gen_lucide_replace_fixtures.mjs).

#include "CompatTestCommon.h"

#include "htmlui/compat/runtime/LucideIcons.h"

#include "tinyxml2.h"

#include <gtest/gtest.h>

#include <set>

using namespace jasperengine::test::compat;

TEST(CompatLucide, EveryIconResolvesToWellFormedShapes){
    const std::vector<std::string> names = LucideIcons::names();
    ASSERT_FALSE(names.empty());
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
    const std::set<std::string> shapes = {"path", "circle", "rect", "line", "ellipse", "polyline", "polygon"};
    for (const std::string& name : names){
        const std::optional<std::string_view> markup = LucideIcons::find(name);
        ASSERT_TRUE(markup.has_value()) << name;
        ASSERT_FALSE(markup->empty()) << name;

        // the shapes parse as SVG children; Lucide's root attributes are not repeated
        const std::string document = "<svg xmlns=\"http://www.w3.org/2000/svg\">" + std::string(*markup) + "</svg>";
        tinyxml2::XMLDocument xml;
        ASSERT_EQ(xml.Parse(document.c_str(), document.size()), tinyxml2::XML_SUCCESS) << name;
        for (const tinyxml2::XMLElement* child = xml.RootElement()->FirstChildElement(); child; child = child->NextSiblingElement())
            EXPECT_EQ(shapes.count(child->Name()), 1u) << name << ": " << child->Name();
    }
}

TEST(CompatLucide, KnownIconMarkup){
    EXPECT_EQ(LucideIcons::find("house").value_or(""),
              "<path d=\"M15 21v-8a1 1 0 0 0-1-1h-4a1 1 0 0 0-1 1v8\"/>"
              "<path d=\"M3 10a2 2 0 0 1 .709-1.528l7-6a2 2 0 0 1 2.582 0l7 6A2 2 0 0 1 21 10v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z\"/>");
}

TEST(CompatLucide, AliasesAndNameForms){
    // an alias of the pinned release, and the forms createIcons accepts
    EXPECT_EQ(LucideIcons::resolve("home"), "house");
    EXPECT_EQ(LucideIcons::find("home"), LucideIcons::find("house"));
    EXPECT_EQ(LucideIcons::resolve("ArrowRight"), "arrow-right");
    EXPECT_EQ(LucideIcons::resolve("arrowRight"), "arrow-right");
    EXPECT_EQ(LucideIcons::resolve("arrow_right"), "arrow-right");
    EXPECT_EQ(LucideIcons::resolve("Home"), "house");
    EXPECT_EQ(LucideIcons::resolve("not-an-icon"), "");
    EXPECT_FALSE(LucideIcons::find("").has_value());
    EXPECT_FALSE(LucideIcons::find("not-an-icon").has_value());
    EXPECT_FALSE(LucideIcons::svgMarkup("not-an-icon").has_value());
}


TEST(CompatLucide, ClosestNames){
    const std::vector<std::string> closest = LucideIcons::closest("hous");
    ASSERT_EQ(closest.size(), 3u);
    EXPECT_EQ(closest[0], "house");
    const std::vector<std::string> chevron = LucideIcons::closest("ChevronRigth", 3);
    ASSERT_FALSE(chevron.empty());
    EXPECT_EQ(chevron[0], "chevron-right");
    EXPECT_EQ(LucideIcons::closest("x", 0).size(), 0u);
    EXPECT_EQ(LucideIcons::closest("house", 1), std::vector<std::string>{"house"});
}


TEST(CompatLucide, SvgAttributesMatchLucideReplaceElement){
    std::string text;
    ASSERT_TRUE(readFixture("compat/runtime/lucide-replace-element.json", text));
    const nlohmann::json fixtures = nlohmann::json::parse(text);
    ASSERT_GE(fixtures["cases"].size(), 10u);
    for (const nlohmann::json& fixture : fixtures["cases"]){
        std::vector<LucideAttribute> element;
        std::string name;
        for (const nlohmann::json& pair : fixture["element"]){
            element.push_back({pair[0].get<std::string>(), pair[1].get<std::string>()});
            if (element.back().name == "data-lucide")
                name = element.back().value;
        }
        std::vector<LucideAttribute> expected;
        for (const nlohmann::json& pair : fixture["svg"])
            expected.push_back({pair[0].get<std::string>(), pair[1].get<std::string>()});

        const std::vector<LucideAttribute> actual = LucideIcons::svgAttributes(name, element);
        ASSERT_EQ(actual.size(), expected.size()) << fixture.dump();
        for (size_t i = 0; i < actual.size(); i++){
            EXPECT_EQ(actual[i].name, expected[i].name) << fixture.dump();
            EXPECT_EQ(actual[i].value, expected[i].value) << fixture.dump();
        }
    }
}

TEST(CompatLucide, SvgMarkupEscapesAndHoldsTheShapes){
    const std::optional<std::string> markup = LucideIcons::svgMarkup("house", {{"data-lucide", "house"}, {"title", "a \"<home>\" & more"}});
    ASSERT_TRUE(markup.has_value());
    EXPECT_EQ(markup->rfind("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\"", 0), 0u) << *markup;
    EXPECT_NE(markup->find("title=\"a &quot;&lt;home&gt;&quot; &amp; more\""), std::string::npos) << *markup;
    EXPECT_EQ(markup->find("aria-hidden"), std::string::npos);
    tinyxml2::XMLDocument xml;
    ASSERT_EQ(xml.Parse(markup->c_str(), markup->size()), tinyxml2::XML_SUCCESS);
    EXPECT_NE(xml.RootElement()->FirstChildElement("path"), nullptr);
}
