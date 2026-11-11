// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The standard title of docs/specs/S80-title-art-core.md §7: 確定 in Dela Gothic One at size 200
// in the classic style, Pachislop's `classicTitleStyle2D` (src/model/defaults.ts, by SafEight,
// used with the author's permission) converted member for member: snake_case keys, the rim's
// downward offsetY 3 negated to the document's y-up. Pachislop's effect references were drawn
// with it at scale 0.25; the classic fixture adds Outer Glow and Drop Shadow at their defaults.
// The font is the test font (titleart/fonts, TitleTestAssets.h) at the asset path DELA_GOTHIC.

#pragma once

#include "TitleTestAssets.h"

#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtIO.h"

#include <string>
#include <utility>
#include <vector>

namespace jasperengine::titleart::test {

    // a gradient of [pos, colour] stops (Pachislop's g())
    inline Json standardGradient(std::vector<std::pair<double, const char*>> stops) {
        Json out = Json::array();
        for (const auto& [pos, color] : stops) out.push_back(Json{{"pos", pos}, {"color", color}});
        return Json{{"stops", out}};
    }

    inline Json standardSolid(const char* color) {
        return Json{{"kind", "solid"}, {"color", color}};
    }

    inline Json standardLinear(const Json& gradient) {
        return Json{{"kind", "linear"}, {"color", gradient["stops"][0]["color"]}, {"gradient", gradient}, {"angle", 90}};
    }

    inline Json standardRim(const char* id, double width, Json material, Json offset = Json::array({0, 0})) {
        return Json{{"id", id}, {"enabled", true}, {"width", width}, {"join", "miter"}, {"offset", std::move(offset)}, {"material", std::move(material)}};
    }

    // Pachislop's GRADIENTS.goldFill, goldRamp and redFill
    inline Json goldFill() {
        return standardGradient({{0, "#7a2a00"}, {0.25, "#fff4c8"}, {0.49, "#8a3000"}, {0.6, "#ffc860"}, {0.79, "#a84400"}, {1, "#ffe890"}});
    }
    inline Json goldRamp() {
        return standardGradient({{0, "#3a0c00"}, {0.25, "#a83800"}, {0.5, "#f08c00"}, {0.75, "#ffd23a"}, {1, "#fff8d6"}});
    }
    inline Json redFill() {
        return standardGradient({{0, "#ff5a5a"}, {0.45, "#e00012"}, {1, "#5a0000"}});
    }

    // classicTitleStyle2D()
    inline Json classicStyle() {
        Json face{{"fill", standardLinear(goldFill())},
                  {"bevel", {{"enabled", true}, {"size", 3}, {"depth", 3}, {"gloss", "ring"}}},
                  {"finish", {{"enabled", true}, {"gradient", goldRamp()}, {"amount", 0.85}}},
                  {"inner_glow", {{"enabled", true}, {"color", "#fff3b0"}, {"size", 1.5}, {"opacity", 0.5}, {"noise", 0.35}}},
                  {"gloss", {{"enabled", true}, {"opacity", 0.45}, {"position", 0.45}, {"softness", 0.12}, {"curve", 0.25}, {"angle", 0}}}};
        Json rims = Json::array();
        rims.push_back(standardRim("00000101", 1.5, Json{{"fill", standardSolid("#1a0a00")}}));
        rims.push_back(standardRim("00000102", 2.5, Json{{"fill", standardSolid("#ffffff")}, {"bevel", {{"enabled", true}, {"size", 1.5}, {"depth", 2}}}}));
        rims.push_back(standardRim("00000103", 4,
                                   Json{{"fill", standardLinear(redFill())},
                                        {"bevel", {{"enabled", true}, {"size", 2.5}, {"depth", 3}, {"highlight_opacity", 0.6}}}}));
        // Pachislop's offsetY 3 is downward on screen: -3 in the y-up document
        rims.push_back(standardRim("00000104", 2.5, Json{{"fill", standardSolid("#000000")}}, Json::array({0, -3})));
        return Json{{"face", face}, {"rims", rims}, {"knockout", false}};
    }

    // the standard title's layer: `text` in Dela Gothic One, size 200, the classic style
    inline Json standardTitleLayer(const char* id = "00000001", const std::string& text = "確定") {
        return Json{{"id", id}, {"type", "text"}, {"name", "Title"}, {"text", text}, {"font", fontRef(DELA_GOTHIC)}, {"size", 200},
                    {"style", classicStyle()}};
    }

    inline Json standardEffect(const char* id, const char* type, Json params = Json::object()) {
        return Json{{"id", id}, {"type", type}, {"enabled", true}, {"params", std::move(params)}};
    }

    // a normalized document on Pachislop's 1920 x 1080 transparent canvas holding `layers`
    inline TitleArtDoc standardDocument(const std::vector<Json>& layers) {
        Json root{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}, {"guid", "5354414e444152445449544c45000001"}};
        root["canvas"] = Json{{"width", 1920}, {"height", 1080}, {"background", {{"transparent", true}, {"color", "#000000"}}}};
        root["layers"] = Json::array();
        for (const Json& layer : layers) root["layers"].push_back(layer);
        return parseTitleArtJson(std::move(root)).doc;
    }

    // the standard title alone, optionally with effects (each at its params) on the layer
    inline TitleArtDoc standardTitleDocument(const std::vector<Json>& effects = {}) {
        Json layer = standardTitleLayer();
        layer["effects"] = Json::array();
        for (const Json& e : effects) layer["effects"].push_back(e);
        return standardDocument({layer});
    }

    // §7's classic fixture: the standard title with Outer Glow and Drop Shadow at their defaults
    inline TitleArtDoc classicFixtureDocument() {
        return standardTitleDocument({standardEffect("00000201", "outer_glow"), standardEffect("00000202", "drop_shadow")});
    }

}
