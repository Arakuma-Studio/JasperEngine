// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Shine and Rainbow recipes are ported from Pachislop src/effects/a/recipes.ts (glintOverlay,
// rainbowOverlay, hueWheel) by SafEight, used with the author's permission.

#include "FxRecipes.h"
#include "FxPresets.h"

#include "../TitleArtOps.h"

#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    double toLin(double c) {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }
    double toSrgb(double c) {
        return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    }
    double smooth(double a, double b, double x) {
        const double t = std::min(1.0, std::max(0.0, (x - a) / (b - a)));
        return t * t * (3.0 - 2.0 * t);
    }
    std::string hex(double r, double g, double b) {
        auto byte = [](double v) { return (int)std::lround(std::min(1.0, std::max(0.0, v)) * 255.0); };
        char text[8];
        std::snprintf(text, sizeof(text), "#%02x%02x%02x", byte(r), byte(g), byte(b));
        return text;
    }
    Json stop(double pos, const std::string& color) {
        return Json{{"pos", std::round(pos * 1e5) / 1e5}, {"color", color}, {"alpha", 1}, {"mid", 0.5}};
    }

    // the hue wheel at HSV saturation `s`: seven stops red .. red, exact in sRGB (recipes.ts hueWheel)
    Json hueWheel(double s) {
        const double corners[7][3] = {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}};
        Json stops = Json::array();
        for (int i = 0; i < 7; i++) {
            stops.push_back(stop(i / 6.0, hex(1 - s + s * corners[i][0], 1 - s + s * corners[i][1], 1 - s + s * corners[i][2])));
        }
        return Json{{"stops", stops}, {"interp", "rgb"}};
    }

    // Shine: a white band swept across the layer (recipes.ts glintOverlay with angle 30, position
    // -20, width 70, softness 85, a second band, intensity 90), written in Gradient Overlay's keys
    Json shineParams() {
        const double angle = 30, position = -20, width = 70, softness = 85, intensity = 90;
        const double soft = std::min(0.999, std::max(0.0, softness / 100.0));
        const double w = std::max(0.5, width), len = 5.0 * w;
        const double k = std::max(0.0, intensity / 100.0), amount = std::min(1.0, k), gain = amount > 0.0 ? k / amount : 1.0;
        struct Band {
            double m, h, peak;
        };
        const std::vector<Band> bands = {{0.5, w / len, 1.0}, {0.5 - 2.1 * w / len, 0.32 * w / len, 0.8}};
        auto value = [&](double x) {
            double v = 0.0;
            for (const Band& b : bands) v += b.peak * (1.0 - smooth(b.h * (1.0 - soft), b.h, std::fabs(x - b.m)));
            return v;
        };
        std::set<double> xs = {0.0, 1.0};
        for (const Band& b : bands) {
            const double hi = b.h * (1.0 - soft);
            for (int side : {-1, 1}) {
                for (int i = 0; i <= 16; i++) xs.insert(b.m + side * (hi + (b.h - hi) * i / 16.0));
            }
        }
        Json stops = Json::array();
        for (double x : xs) {
            if (x < 0.0 || x > 1.0) continue;
            const double v = value(x) * gain;
            const double c = toSrgb(std::min(1.0, toLin(1.0) * v));
            stops.push_back(stop(x, hex(c, c, c)));
        }
        // the gradient holds at most 32 stops: keep the ends and every other feather sample
        while (stops.size() > 32) {
            Json thinned = Json::array();
            for (size_t i = 0; i < stops.size(); i++) {
                if (i == 0 || i + 1 == stops.size() || i % 2 == 0) thinned.push_back(stops[i]);
            }
            stops = thinned;
        }
        return Json{{"gradient", Json{{"stops", stops}, {"interp", "rgb"}}}, {"style", "sweep"}, {"angle", angle}, {"length", len},
                    {"position", position}, {"wrap", "clamp"}, {"amount", amount}, {"mode", "light"}};
    }

    // Rainbow: one spectrum cycle across the layer at saturation 0.9 in Color mode (recipes.ts
    // rainbowOverlay with bands 1, angle 20, phase 0)
    Json rainbowParams() {
        const double bands = 1.0, phase = 0.0;
        const double off = 0.5 * bands - 0.5 - phase;
        return Json{{"gradient", hueWheel(0.9)}, {"style", "linear"}, {"angle", 20}, {"scale", 1.0 / bands}, {"offset", off - std::floor(off)},
                    {"wrap", "repeat"}, {"amount", 1}, {"mode", "color"}};
    }

    // Vignette: the layer darkened toward its box's corners, light multiplied through a radial ramp
    Json vignetteParams() {
        Json stops = Json::array({stop(0.0, "#ffffff"), stop(0.55, "#ffffff"), stop(1.0, "#1a1a1a")});
        return Json{{"gradient", Json{{"stops", stops}, {"interp", "rgb"}}}, {"style", "radial"}, {"scale", 1}, {"offset", 0}, {"wrap", "clamp"},
                    {"amount", 0.6}, {"mode", "shade"}};
    }

}

const std::vector<TitleFxRecipe>& editor::titleFxRecipes() {
    static const std::vector<TitleFxRecipe> recipes = {
        {"long_shadow", "Long Shadow", "Shadow & Depth", "extrude", "long_shadow", {"long shadow", "flat shadow", "ロングシャドウ"}},
        {"shine", "Shine", "Color", "gradient_overlay", nullptr, {"shine", "glint band", "sweep", "glint overlay", "光沢"}},
        {"rainbow", "Rainbow", "Color", "gradient_overlay", nullptr, {"rainbow", "spectrum", "虹"}},
        {"vignette", "Vignette", "Color", "gradient_overlay", nullptr, {"vignette", "darken edges"}},
        {"emboss", "Emboss", "Texture & Material", "material", "emboss", {"emboss", "raised", "エンボス"}},
        {"deboss", "Deboss", "Texture & Material", "material", "deboss", {"deboss", "pressed", "stamped", "型押し"}},
        {"icicles", "Icicles", "Texture & Material", "drips", "icicles", {"icicles", "ice drips", "つらら"}},
        {"comic_dots", "Comic Dots", "Stylize", "halftone", "comic_dots", {"comic", "manga tone", "トーン"}},
        {"ben_day", "Ben-Day Dots", "Stylize", "halftone", "ben_day", {"ben-day", "pop art dots"}},
        {"crt", "CRT", "Stylize", "scanlines", "crt", {"crt", "arcade monitor", "ブラウン管"}},
    };
    return recipes;
}

const TitleFxRecipe* editor::findTitleFxRecipe(const std::string& id) {
    for (const TitleFxRecipe& r : titleFxRecipes()) {
        if (id == r.id) return &r;
    }
    return nullptr;
}

Json editor::titleFxRecipeParams(const TitleFxRecipe& recipe) {
    if (recipe.preset) {
        const TitleFxPreset* p = TitleFxPresets::find(recipe.type, recipe.preset);
        return p ? p->params : Json::object();
    }
    const std::string id = recipe.id;
    if (id == "shine") return shineParams();
    if (id == "rainbow") return rainbowParams();
    return vignetteParams();
}

const std::vector<TitleFxStyleSetting>& editor::titleFxStyleSettings() {
    static const std::vector<TitleFxStyleSetting> settings = {
        {"dry_brush", "Dry Brush (かすれ)", "Texture & Material", "style.erode.enabled", "text, shape, vector and image layers (any other: Distress's dry brush)",
         {"かすれ", "dry brush", "kasure", "brush gaps", "erode"}},
        {"glint_3d", "Glint (3D highlights)", "Glow", "style_3d.glint.enabled", "Dimension 3D layers (S86)", {"glint", "3d glint", "star filter", "キラ"}},
    };
    return settings;
}

bool editor::applyTitleFxPreset(const std::string& type, const std::string& preset, Json& params, std::string& error, std::vector<std::string>& valid) {
    const TitleFxPreset* p = TitleFxPresets::find(type, preset);
    if (!p) {
        for (const TitleFxPreset& q : TitleFxPresets::forType(type)) valid.push_back(q.id);
        error = "'" + preset + "' is not a preset of " + type + (valid.empty() ? std::string(" (it has none)") : std::string());
        return false;
    }
    for (auto it = p->params.begin(); it != p->params.end(); ++it) params[it.key()] = it.value();
    return true;
}

void editor::registerTitleFxOps() {
    TitleArtOps::registerOp("apply_recipe", [](TitleOpContext& ctx, const TitleJson& op) {
        std::string id;
        if (!ctx.requireString(op, "recipe", id)) return false;
        const TitleFxRecipe* recipe = findTitleFxRecipe(id);
        if (!recipe) {
            std::vector<std::string> valid;
            for (const TitleFxRecipe& r : titleFxRecipes()) valid.push_back(r.id);
            return ctx.fail("bad_value", "'" + id + "' is not a recipe", "recipe", valid, titleart::nearestName(id, valid));
        }
        TitleJson add = TitleJson{{"op", "add_effect"}, {"type", recipe->type}, {"params", titleFxRecipeParams(*recipe)}};
        if (op.contains("layer")) add["layer"] = op["layer"];
        if (op.contains("part")) add["part"] = op["part"];
        std::string undo;
        if (!TitleArtOps::runCoreOp(ctx, add, &undo)) return false;
        ctx.undoName = recipe->name;
        return true;
    });
}
