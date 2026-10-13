// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// p4-gaps (phase-4 spec 4.3.2): the parity gaps the example library found, tested through the
// public APIs that showed them.
//
// - GeometryApiParity: the element API's geometry (UIElement: getBoundingClientRect, offset*,
//   client*, scroll*; what Lua reads) and the inspector's boxes against Chromium's references of
//   fixtures/p3/layout/display-none-geometry: an element hidden with display: none, and its
//   descendants, have no box (RmlUi keeps the last one it laid out).

#include "FixtureRunner.h"

#include "htmlui/HtmlUIInspectorInternal.h"
#include "object/ui/UIElement.h"
#include "object/ui/UIElementInternal.h"

#include "json.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct ReferenceBox {
        std::string id;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    };

    // golden/browser/<area>/<name>.boxes.json: capture suffix -> boxes
    std::map<std::string, std::vector<ReferenceBox>> referenceBoxes(const std::string& fixture){
        std::map<std::string, std::vector<ReferenceBox>> captures;
        std::ifstream file(browserReferenceDirectory() + "/" + fixture + ".boxes.json");
        std::stringstream text;
        text << file.rdbuf();
        const nlohmann::json json = nlohmann::json::parse(text.str());
        for (const auto& capture : json.at("captures").items()){
            for (const nlohmann::json& element : capture.value()){
                ReferenceBox box;
                box.id = element.at("id").get<std::string>();
                box.x = element.at("x").get<float>();
                box.y = element.at("y").get<float>();
                box.w = element.at("w").get<float>();
                box.h = element.at("h").get<float>();
                captures[capture.key()].push_back(box);
            }
        }
        return captures;
    }

}

TEST(GeometryApiParity, MatchesChromium){
    const std::string fixture = "layout/display-none-geometry";
    FixtureSidecar sidecar;
    std::string error;
    ASSERT_TRUE(loadSidecar(browserFixturesDirectory() + "/" + fixture + ".json", sidecar, &error)) << error;
    FixtureRunner runner(sidecar.width, sidecar.height, sidecar.sampleCount);
    if (!runner.valid())
        GTEST_SKIP() << "no GPU";
    ASSERT_TRUE(runner.load("fixtures/p3/" + fixture + ".html", &error)) << error;
    const auto references = referenceBoxes(fixture);

    int captures = 0;
    for (const FixtureStep& step : sidecar.steps){
        if (step.kind != FixtureStep::Kind::Capture){
            ASSERT_TRUE(runner.step(step, &error)) << error;
            continue;
        }
        captures++;
        const auto found = references.find(step.suffix);
        ASSERT_NE(found, references.end()) << step.suffix;
        for (const ReferenceBox& expected : found->second){
            SCOPED_TRACE(step.suffix + " #" + expected.id);
            Rml::Element* element = runner.document()->GetElementById(expected.id);
            ASSERT_NE(element, nullptr);
            const UIElement api = UIElementAccess::wrap(element);

            const UIRect rect = api.getBoundingClientRect();
            EXPECT_NEAR(rect.x, expected.x, 1.0f);
            EXPECT_NEAR(rect.y, expected.y, 1.0f);
            EXPECT_NEAR(rect.width, expected.w, 1.0f);
            EXPECT_NEAR(rect.height, expected.h, 1.0f);
            EXPECT_NEAR(rect.right, expected.x + expected.w, 1.0f);
            EXPECT_NEAR(rect.bottom, expected.y + expected.h, 1.0f);
            EXPECT_NEAR(api.getOffsetWidth(), expected.w, 1.0f);
            EXPECT_NEAR(api.getOffsetHeight(), expected.h, 1.0f);

            const htmlui::InspectRect border = htmlui::inspect::borderRect(element);
            EXPECT_NEAR(border.x, expected.x, 1.0f);
            EXPECT_NEAR(border.y, expected.y, 1.0f);
            EXPECT_NEAR(border.width, expected.w, 1.0f);
            EXPECT_NEAR(border.height, expected.h, 1.0f);

            // without a box, every size and position is 0 (a browser's offsetTop/Left, client*,
            // scroll* of a display: none element)
            if (expected.w == 0.0f && expected.h == 0.0f){
                EXPECT_FALSE(htmlui::inspect::hasLayoutBox(element));
                EXPECT_EQ(api.getOffsetLeft(), 0.0f);
                EXPECT_EQ(api.getOffsetTop(), 0.0f);
                EXPECT_EQ(api.getClientWidth(), 0.0f);
                EXPECT_EQ(api.getClientHeight(), 0.0f);
                EXPECT_EQ(api.getScrollWidth(), 0.0f);
                EXPECT_EQ(api.getScrollHeight(), 0.0f);
                EXPECT_EQ(api.getScrollTop(), 0.0f);
                EXPECT_EQ(api.getScrollLeft(), 0.0f);
                const Rect box = api.getBoundingBox();
                EXPECT_EQ(box.getWidth(), 0.0f);
                EXPECT_EQ(box.getHeight(), 0.0f);
            }else{
                EXPECT_TRUE(htmlui::inspect::hasLayoutBox(element));
                EXPECT_GT(api.getClientWidth(), 0.0f);
            }
        }
    }
    EXPECT_EQ(captures, 3);

    // an element that is not in a document has no box either
    Rml::ElementPtr detached = runner.document()->CreateElement("div");
    detached->SetProperty("width", "50px");
    EXPECT_FALSE(htmlui::inspect::hasLayoutBox(detached.get()));
    EXPECT_EQ(UIElementAccess::wrap(detached.get()).getOffsetWidth(), 0.0f);
}
