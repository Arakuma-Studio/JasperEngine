// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PaletteEdit.h"

#include "pixelart/RampGenerator.h"

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;

namespace pxc = jasperengine::pixelart;
using Json = nlohmann::json;

// file-local, in a namespace of its own: a unity build merges this directory's sources
namespace jasperengine::editor::pixelart::paletteedit {
namespace {

    // an op's problem, the field named "ops[<i>].<key>"
    struct OpError {
        std::string kind = "invalid_value";
        std::string field;
        std::string message;
        std::string fix;
    };

    struct OpContext {
        const Json& op;
        size_t position;
        std::vector<OpError>& errors;

        std::string field(const std::string& key) const {
            return "ops[" + std::to_string(position) + "]" + (key.empty() ? "" : "." + key);
        }
        void fail(const std::string& key, const std::string& message, const std::string& fix, const std::string& kind = "invalid_value") const {
            errors.push_back({kind, field(key), message, fix});
        }
        bool has(const char* key) const { return op.contains(key) && !op[key].is_null(); }

        bool integer(const char* key, int& out) const {
            if (!has(key)) {
                fail(key, std::string("needs \"") + key + "\"", std::string("add \"") + key + "\" (a whole number)");
                return false;
            }
            const Json& value = op[key];
            if (!value.is_number() || std::floor(value.get<double>()) != value.get<double>()) {
                fail(key, std::string("\"") + key + "\" must be a whole number", "give a whole number");
                return false;
            }
            out = (int)value.get<double>();
            return true;
        }
        bool number(const char* key, float& out) const {
            if (!has(key)) return true;
            if (!op[key].is_number() || !std::isfinite(op[key].get<double>())) {
                fail(key, std::string("\"") + key + "\" must be a number", "give a number");
                return false;
            }
            out = (float)op[key].get<double>();
            return true;
        }
        bool text(const char* key, std::string& out, bool required = true) const {
            if (!has(key)) {
                if (required) fail(key, std::string("needs \"") + key + "\"", std::string("add \"") + key + "\" (a string)");
                return !required;
            }
            if (!op[key].is_string()) {
                fail(key, std::string("\"") + key + "\" must be a string", "give a string");
                return false;
            }
            out = op[key].get<std::string>();
            return true;
        }
        bool color(const Json& value, const std::string& key, pxc::Rgba8& out) const {
            if (!value.is_string() || !pxc::parseHex(value.get<std::string>(), out)) {
                fail(key, "not a colour: " + value.dump(), "write \"#rrggbb\" or \"#rrggbbaa\"");
                return false;
            }
            return true;
        }
        bool index(const pxc::Palette& palette, const char* key, int& out) const {
            if (!integer(key, out)) return false;
            if (!palette.validIndex(out)) {
                fail(key, "index " + std::to_string(out) + " is not in the palette (0 to " + std::to_string(std::max(0, palette.count() - 1)) + ")",
                     "use an index from 0 to " + std::to_string(std::max(0, palette.count() - 1)), "index_out_of_range");
                return false;
            }
            return true;
        }
        // "indices": [..] or "index": n
        bool indices(const pxc::Palette& palette, std::vector<int>& out) const {
            if (has("indices")) {
                if (!op["indices"].is_array()) {
                    fail("indices", "\"indices\" must be a list of colour indices", "give [0, 3, ...]");
                    return false;
                }
                for (size_t i = 0; i < op["indices"].size(); i++) {
                    const Json& value = op["indices"][i];
                    if (!value.is_number_integer() || !palette.validIndex(value.get<int>())) {
                        fail("indices[" + std::to_string(i) + "]", "not a colour index of the palette: " + value.dump(),
                             "use indices from 0 to " + std::to_string(std::max(0, palette.count() - 1)), "index_out_of_range");
                        return false;
                    }
                    out.push_back(value.get<int>());
                }
                return true;
            }
            int single = 0;
            if (!index(palette, "index", single)) return false;
            out.push_back(single);
            return true;
        }
        // ramp colours: palette indices, or hex colours (appended to the palette, an exact
        // duplicate reusing its index)
        bool rampColors(pxc::Palette& palette, std::vector<uint16_t>& out) const {
            if (!has("colors") || !op["colors"].is_array()) {
                fail("colors", "needs \"colors\": palette indices or hex colours, darkest first", "add \"colors\": [2, 3, 4]");
                return false;
            }
            for (size_t i = 0; i < op["colors"].size(); i++) {
                const Json& value = op["colors"][i];
                const std::string key = "colors[" + std::to_string(i) + "]";
                if (value.is_number_integer()) {
                    if (!palette.validIndex(value.get<int>())) {
                        fail(key, "not a colour index of the palette: " + value.dump(), "use an index of the palette, or a hex colour", "index_out_of_range");
                        return false;
                    }
                    out.push_back((uint16_t)value.get<int>());
                } else {
                    pxc::Rgba8 color;
                    if (!this->color(value, key, color)) return false;
                    int found = palette.indexOf(color);
                    if (found < 0) {
                        palette.colors.push_back(color);
                        found = palette.count() - 1;
                    }
                    out.push_back((uint16_t)found);
                }
            }
            return true;
        }
    };

    pxc::PaletteRamp* findRampOf(pxc::Palette& palette, const OpContext& ctx) {
        std::string name;
        if (!ctx.text("name", name)) return nullptr;
        const int at = palette.findRampIndex(name);
        if (at < 0) {
            std::string names;
            for (const pxc::PaletteRamp& ramp : palette.ramps) names += (names.empty() ? "" : ", ") + ramp.name;
            ctx.fail("name", "no ramp '" + name + "'", names.empty() ? "add a ramp first (add_ramp)" : "use one of: " + names, "not_found");
            return nullptr;
        }
        return &palette.ramps[at];
    }

    pxc::PaletteVariant* findVariantOf(pxc::Palette& palette, const OpContext& ctx) {
        std::string name;
        if (!ctx.text("name", name)) return nullptr;
        const int at = palette.findVariant(name);
        if (at < 0) {
            std::string names;
            for (const pxc::PaletteVariant& variant : palette.variants) names += (names.empty() ? "" : ", ") + variant.name;
            ctx.fail("name", "no variant '" + name + "'", names.empty() ? "add a variant first (add_variant)" : "use one of: " + names, "not_found");
            return nullptr;
        }
        return &palette.variants[at];
    }

    // a variant's colours: null keeps the base colour
    bool variantColors(const OpContext& ctx, std::vector<std::optional<pxc::Rgba8>>& out) {
        if (!ctx.op["colors"].is_array()) {
            ctx.fail("colors", "\"colors\" must be a list of hex colours and nulls (null: the base colour)", "give [null, \"#29366f\", ...]");
            return false;
        }
        for (size_t i = 0; i < ctx.op["colors"].size(); i++) {
            const Json& value = ctx.op["colors"][i];
            if (value.is_null()) {
                out.push_back(std::nullopt);
                continue;
            }
            pxc::Rgba8 color;
            if (!ctx.color(value, "colors[" + std::to_string(i) + "]", color)) return false;
            out.push_back(color);
        }
        return true;
    }

    bool rampRecipe(const pxc::Palette& palette, const OpContext& ctx, RampRecipe& recipe) {
        if (!ctx.has("base")) {
            ctx.fail("base", "generate_ramp needs \"base\": a hex colour or a palette index", "add \"base\": \"#b13e53\"");
            return false;
        }
        const Json& base = ctx.op["base"];
        if (base.is_number_integer()) {
            if (!palette.validIndex(base.get<int>())) {
                ctx.fail("base", "not a colour index of the palette: " + base.dump(), "use a palette index or a hex colour", "index_out_of_range");
                return false;
            }
            recipe.base = palette.colors[base.get<int>()];
        } else if (!ctx.color(base, "base", recipe.base)) {
            return false;
        }
        for (const char* key : {"darker", "lighter"}) {
            if (!ctx.has(key)) continue;
            int steps = 0;
            if (!ctx.integer(key, steps)) return false;
            if (steps < 0 || steps > RampRecipe::kMaxSteps) {
                ctx.fail(key, std::string("\"") + key + "\" is 0 to 15 steps", "give 0 to 15");
                return false;
            }
            (std::string(key) == "darker" ? recipe.darker : recipe.lighter) = steps;
        }
        std::string mode = "hue_shift";
        if (!ctx.text("mode", mode, false)) return false;
        if (mode == "hue_shift") recipe.mode = RampMode::HueShift;
        else if (mode == "oklch") recipe.mode = RampMode::Oklch;
        else {
            ctx.fail("mode", "unknown mode '" + mode + "'", "use hue_shift or oklch");
            return false;
        }
        return ctx.number("hue", recipe.hue) && ctx.number("saturation", recipe.saturation) && ctx.number("value", recipe.value) &&
               ctx.number("hue_lighten_limit", recipe.hueLightenLimit) && ctx.number("hue_darken_limit", recipe.hueDarkenLimit) &&
               ctx.number("saturation_floor", recipe.saturationFloor) && ctx.number("value_floor", recipe.valueFloor) &&
               ctx.number("darkest_l", recipe.darkestL) && ctx.number("lightest_l", recipe.lightestL) && ctx.number("chroma_peak", recipe.chromaPeak);
    }

    void reorderAll(pxc::Palette& palette, const std::vector<int>& order) {
        palette.remap(order);
    }

    bool applyOne(pxc::Palette& palette, const OpContext& ctx, std::vector<std::string>& warnings) {
        std::string op;
        if (!ctx.op.is_object() || !ctx.text("op", op)) {
            if (!ctx.op.is_object()) ctx.fail("", "an op is an object {op, ...}", "write {\"op\": \"set\", \"index\": 0, \"color\": \"#000000\"}");
            return false;
        }
        if (op == "set") {
            int index = 0;
            pxc::Rgba8 color;
            if (!ctx.index(palette, "index", index) || !ctx.has("color") || !ctx.color(ctx.op["color"], "color", color)) {
                if (!ctx.has("color")) ctx.fail("color", "set needs \"color\"", "add \"color\": \"#rrggbb\"");
                return false;
            }
            palette.colors[index] = color;
        } else if (op == "insert") {
            int at = palette.count();
            if (ctx.has("index")) {
                if (!ctx.integer("index", at)) return false;
                if (at < 0 || at > palette.count()) {
                    ctx.fail("index", "insert at 0 to " + std::to_string(palette.count()), "give an index from 0 to the colour count", "index_out_of_range");
                    return false;
                }
            }
            std::vector<pxc::Rgba8> colors;
            if (ctx.has("color")) {
                pxc::Rgba8 color;
                if (!ctx.color(ctx.op["color"], "color", color)) return false;
                colors.push_back(color);
            } else if (ctx.has("colors") && ctx.op["colors"].is_array()) {
                for (size_t i = 0; i < ctx.op["colors"].size(); i++) {
                    pxc::Rgba8 color;
                    if (!ctx.color(ctx.op["colors"][i], "colors[" + std::to_string(i) + "]", color)) return false;
                    colors.push_back(color);
                }
            } else {
                ctx.fail("color", "insert needs \"color\" or \"colors\"", "add \"color\": \"#rrggbb\"");
                return false;
            }
            std::vector<int> order(palette.count());
            std::iota(order.begin(), order.end(), 0);
            order.insert(order.begin() + at, colors.size(), -1);
            palette.remap(order);
            for (size_t i = 0; i < colors.size(); i++) palette.colors[at + i] = colors[i];
        } else if (op == "remove") {
            std::vector<int> drop;
            if (!ctx.indices(palette, drop)) return false;
            std::vector<int> order;
            for (int i = 0; i < palette.count(); i++) {
                if (std::find(drop.begin(), drop.end(), i) == drop.end()) order.push_back(i);
            }
            if (order.empty()) {
                ctx.fail("indices", "a palette keeps at least one colour", "leave one colour");
                return false;
            }
            palette.remap(order);
        } else if (op == "move") {
            int from = 0, to = 0;
            if (!ctx.index(palette, "from", from) || !ctx.index(palette, "to", to)) return false;
            std::vector<int> order(palette.count());
            std::iota(order.begin(), order.end(), 0);
            order.erase(order.begin() + from);
            order.insert(order.begin() + to, from);
            palette.remap(order);
        } else if (op == "sort") {
            std::string by;
            PaletteSort sort;
            if (!ctx.text("by", by)) return false;
            if (!parsePaletteSort(by, sort)) {
                ctx.fail("by", "unknown sort '" + by + "'", "use hue, saturation, luma, value or reverse");
                return false;
            }
            reorderAll(palette, paletteSortOrder(palette, sort));
            warnings.push_back("sorting changes colour numbers; indexed Aseprite files keep their old numbers");
        } else if (op == "rename_color") {
            int index = 0;
            std::string name;
            if (!ctx.index(palette, "index", index) || !ctx.text("name", name)) return false;
            if (name.empty()) palette.names.erase(index);
            else palette.names[index] = name;
        } else if (op == "lock") {
            std::vector<int> which;
            if (!ctx.indices(palette, which)) return false;
            bool locked = true;
            if (ctx.has("locked")) {
                if (!ctx.op["locked"].is_boolean()) {
                    ctx.fail("locked", "\"locked\" is true or false", "give true (lock) or false (unlock)");
                    return false;
                }
                locked = ctx.op["locked"].get<bool>();
            }
            for (int index : which) {
                auto it = std::find(palette.locked.begin(), palette.locked.end(), (uint16_t)index);
                if (locked && it == palette.locked.end()) palette.locked.push_back((uint16_t)index);
                if (!locked && it != palette.locked.end()) palette.locked.erase(it);
            }
            std::sort(palette.locked.begin(), palette.locked.end());
        } else if (op == "set_transparent") {
            int index = -1;
            if (!ctx.integer("index", index)) return false;
            if (index != -1 && !palette.validIndex(index)) {
                ctx.fail("index", "not a colour index of the palette: " + std::to_string(index), "use an index, or -1 for none", "index_out_of_range");
                return false;
            }
            palette.transparent = index;
        } else if (op == "set_info") {
            std::string value;
            if (ctx.has("name") && ctx.text("name", value)) palette.name = value;
            if (ctx.has("author") && ctx.text("author", value)) palette.author = value;
            if (ctx.has("description") && ctx.text("description", value)) palette.description = value;
            if (ctx.has("columns")) {
                int columns = 0;
                if (!ctx.integer("columns", columns)) return false;
                if (columns < 1 || columns > pxc::Palette::kMaxColumns) {
                    ctx.fail("columns", "\"columns\" is 1 to 64", "give 1 to 64");
                    return false;
                }
                palette.columns = columns;
            }
        } else if (op == "add_ramp") {
            pxc::PaletteRamp ramp;
            if (!ctx.text("name", ramp.name) || !ctx.rampColors(palette, ramp.colors) || !ctx.number("cycle", ramp.cycle)) return false;
            if (palette.findRamp(ramp.name)) {
                ctx.fail("name", "a ramp called '" + ramp.name + "' exists", "use another name, or set_ramp", "exists");
                return false;
            }
            palette.ramps.push_back(ramp);
        } else if (op == "set_ramp") {
            pxc::PaletteRamp* ramp = findRampOf(palette, ctx);
            if (!ramp) return false;
            const int at = (int)(ramp - palette.ramps.data());
            if (ctx.has("colors")) {
                std::vector<uint16_t> colors;
                if (!ctx.rampColors(palette, colors)) return false;
                palette.ramps[at].colors = colors;
            }
            if (!ctx.number("cycle", palette.ramps[at].cycle)) return false;
            if (ctx.has("reverse") && ctx.op["reverse"].is_boolean() && ctx.op["reverse"].get<bool>())
                std::reverse(palette.ramps[at].colors.begin(), palette.ramps[at].colors.end());
            if (ctx.has("fit")) {
                int steps = 0;
                if (!ctx.integer("fit", steps)) return false;
                if (steps < 1 || steps > pxc::Palette::kMaxColors) {
                    ctx.fail("fit", "\"fit\" is 1 to 256 steps", "give the number of steps the ramp keeps");
                    return false;
                }
                // pixelfx's fit over the indices: each carried as a colour key (lossless below 2^24)
                std::vector<pxc::Rgba8> keys;
                for (uint16_t index : palette.ramps[at].colors) keys.push_back(pxc::unpackRgb(index));
                std::vector<uint16_t> fitted;
                for (const pxc::Rgba8& key : pxc::fitRamp(keys, steps)) fitted.push_back((uint16_t)pxc::packRgb(key));
                palette.ramps[at].colors = fitted;
            }
            std::string rename;
            if (ctx.has("rename")) {
                if (!ctx.text("rename", rename)) return false;
                if (rename != palette.ramps[at].name && palette.findRamp(rename)) {
                    ctx.fail("rename", "a ramp called '" + rename + "' exists", "use another name", "exists");
                    return false;
                }
                palette.ramps[at].name = rename;
            }
        } else if (op == "remove_ramp") {
            pxc::PaletteRamp* ramp = findRampOf(palette, ctx);
            if (!ramp) return false;
            palette.ramps.erase(palette.ramps.begin() + (ramp - palette.ramps.data()));
        } else if (op == "generate_ramp") {
            RampRecipe recipe;
            if (!rampRecipe(palette, ctx, recipe)) return false;
            std::string name = "ramp " + std::to_string(palette.ramps.size() + 1), replace;
            if (!ctx.text("name", name, false) || !ctx.text("replace", replace, false)) return false;
            const PaletteEditResult added = addGeneratedRamp(palette, generateRamp(recipe), name, replace);
            for (const PixelProblem& problem : added.errors) ctx.fail(problem.field.empty() ? "name" : problem.field, problem.message, problem.fix, problem.kind);
            if (!added.ok) return false;
        } else if (op == "add_variant") {
            pxc::PaletteVariant variant;
            if (!ctx.text("name", variant.name)) return false;
            if (palette.findVariant(variant.name) >= 0) {
                ctx.fail("name", "a variant called '" + variant.name + "' exists", "use another name, or set_variant", "exists");
                return false;
            }
            if (ctx.has("colors") && !variantColors(ctx, variant.colors)) return false;
            palette.variants.push_back(variant);
        } else if (op == "set_variant") {
            pxc::PaletteVariant* variant = findVariantOf(palette, ctx);
            if (!variant) return false;
            const int at = (int)(variant - palette.variants.data());
            if (ctx.has("colors")) {
                std::vector<std::optional<pxc::Rgba8>> colors;
                if (!variantColors(ctx, colors)) return false;
                palette.variants[at].colors = colors;
            }
            if (ctx.has("index")) {
                int index = 0;
                if (!ctx.index(palette, "index", index)) return false;
                std::optional<pxc::Rgba8> color;
                if (ctx.has("color")) {
                    pxc::Rgba8 value;
                    if (!ctx.color(ctx.op["color"], "color", value)) return false;
                    color = value;
                }
                std::vector<std::optional<pxc::Rgba8>>& colors = palette.variants[at].colors;
                if ((int)colors.size() <= index) colors.resize(index + 1);
                colors[index] = color;
            }
            if (ctx.has("rename")) {
                std::string rename;
                if (!ctx.text("rename", rename)) return false;
                if (rename != palette.variants[at].name && palette.findVariant(rename) >= 0) {
                    ctx.fail("rename", "a variant called '" + rename + "' exists", "use another name", "exists");
                    return false;
                }
                palette.variants[at].name = rename;
            }
        } else if (op == "remove_variant") {
            pxc::PaletteVariant* variant = findVariantOf(palette, ctx);
            if (!variant) return false;
            palette.variants.erase(palette.variants.begin() + (variant - palette.variants.data()));
        } else {
            ctx.fail("op", "unknown op '" + op + "'",
                     "use set, insert, remove, move, sort, rename_color, lock, set_transparent, set_info, add_ramp, set_ramp, remove_ramp, "
                     "generate_ramp, add_variant, set_variant or remove_variant");
            return false;
        }
        return true;
    }

}
}

using namespace jasperengine::editor::pixelart::paletteedit;

const char* jasperengine::editor::pixelart::paletteSortName(PaletteSort sort) {
    switch (sort) {
        case PaletteSort::Hue: return "hue";
        case PaletteSort::Saturation: return "saturation";
        case PaletteSort::Luma: return "luma";
        case PaletteSort::Value: return "value";
        case PaletteSort::Reverse: return "reverse";
    }
    return "hue";
}

bool jasperengine::editor::pixelart::parsePaletteSort(const std::string& name, PaletteSort& out) {
    for (PaletteSort sort : {PaletteSort::Hue, PaletteSort::Saturation, PaletteSort::Luma, PaletteSort::Value, PaletteSort::Reverse}) {
        if (name == paletteSortName(sort)) {
            out = sort;
            return true;
        }
    }
    return false;
}

std::vector<int> jasperengine::editor::pixelart::paletteSortOrder(const pxc::Palette& palette, PaletteSort sort) {
    std::vector<int> order(palette.count());
    std::iota(order.begin(), order.end(), 0);
    if (sort == PaletteSort::Reverse) {
        std::reverse(order.begin(), order.end());
        return order;
    }
    std::vector<float> key(palette.count());
    for (int i = 0; i < palette.count(); i++) {
        const pxc::Rgba8 color = palette.colors[i];
        const pxc::Hsv hsv = pxc::toHsv(color);
        switch (sort) {
            case PaletteSort::Hue: key[i] = hsv.s > 0.0f ? hsv.h : -1.0f; break;   // greys first
            case PaletteSort::Saturation: key[i] = hsv.s; break;
            case PaletteSort::Luma: key[i] = pxc::toOklab(color).l; break;
            case PaletteSort::Value: key[i] = hsv.v; break;
            case PaletteSort::Reverse: break;
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return key[a] < key[b]; });
    return order;
}

PaletteEditResult jasperengine::editor::pixelart::addGeneratedRamp(pxc::Palette& palette, const std::vector<pxc::Rgba8>& colors, const std::string& rampName,
                                             const std::string& replace) {
    PaletteEditResult result;
    std::vector<uint16_t> indices;
    pxc::Palette edited = palette;
    for (const pxc::Rgba8& color : colors) {
        int found = edited.indexOf(color);
        if (found < 0) {
            edited.colors.push_back(color);
            found = edited.count() - 1;
        }
        indices.push_back((uint16_t)found);
    }
    if (!replace.empty()) {
        const int at = edited.findRampIndex(replace);
        if (at < 0) {
            result.ok = false;
            result.errors.push_back({"not_found", "replace", "no ramp '" + replace + "' to replace", "name an existing ramp, or leave replace out"});
            return result;
        }
        edited.ramps[at].colors = indices;
    } else {
        if (edited.findRamp(rampName)) {
            result.ok = false;
            result.errors.push_back({"exists", "name", "a ramp called '" + rampName + "' exists", "use another name, or replace it"});
            return result;
        }
        edited.ramps.push_back({rampName, indices, 0.0f});
    }
    palette = std::move(edited);
    return result;
}

PaletteEditResult jasperengine::editor::pixelart::applyPaletteOps(pxc::Palette& palette, const Json& ops) {
    PaletteEditResult result;
    if (palette.readOnly) {
        result.ok = false;
        result.errors.push_back({"read_only", "", "this palette was written by a newer format version and is read-only here",
                                 "edit it with the editor that wrote it, or save a copy"});
        return result;
    }
    if (!ops.is_array()) {
        result.ok = false;
        result.errors.push_back({"invalid_value", "ops", "\"ops\" must be a list of {op, ...}", "give [{\"op\": \"set\", ...}]"});
        return result;
    }
    pxc::Palette edited = palette;
    std::vector<OpError> errors;
    for (size_t i = 0; i < ops.size() && errors.empty(); i++) {
        const OpContext ctx{ops[i], i, errors};
        if (!applyOne(edited, ctx, result.warnings) && errors.empty()) ctx.fail("", "the op could not be applied", "check its fields");
    }
    if (errors.empty()) {
        std::vector<pxc::Diagnostic> diagnostics;
        pxc::PaletteCodec::validate(edited, diagnostics);
        for (const pxc::Diagnostic& diagnostic : diagnostics) {
            if (diagnostic.severity == pxc::Diagnostic::Severity::Error)
                errors.push_back({diagnostic.kind, diagnostic.field, diagnostic.message, diagnostic.fix});
            else if (diagnostic.kind != "unknown_keys")
                result.warnings.push_back(diagnostic.message);
        }
    }
    if (!errors.empty()) {
        result.ok = false;
        for (const OpError& error : errors) result.errors.push_back({error.kind, error.field, error.message, error.fix});
        return result;
    }
    for (int i = 0; i < edited.count(); i++) {
        if (i >= palette.count() || palette.colors[i] != edited.colors[i]) result.changed.push_back(i);
    }
    palette = std::move(edited);
    return result;
}
