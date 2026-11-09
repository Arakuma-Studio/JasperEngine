// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/src/pixel/core/recipe.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

#include "CleanupRecipe.h"

#include "CleanupCommon.h"
#include "CleanupSheet.h"
#include "pixelart/BuiltinPalettes.h"
#include "pixelart/Palette.h"
#include "pixelart/PixelColor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

namespace jasperengine::editor::pixelart::cleanup {

    using jasperengine::pixelart::ColorMetric;
    using nlohmann::json;

    namespace {

        std::string recipeNumberText(double value) {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", value);
            return text;
        }

        std::string recipeJoin(const std::vector<std::string>& items) {
            std::string out;
            for (const std::string& item : items) {
                if (!out.empty()) out += ", ";
                out += item;
            }
            return out;
        }

        // canonical numbers: 6 decimals, whole values as integers, no negative zero
        json recipeNumber(double value) {
            double rounded = std::floor(value * 1e6 + 0.5) / 1e6;
            if (rounded == 0) rounded = 0;
            if (std::abs(rounded) < 9.0e15 && rounded == std::floor(rounded)) return json(static_cast<int64_t>(rounded));
            return json(rounded);
        }

        bool recipeIsGuid(const std::string& text) {
            return text.size() == 32 && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
        }

        bool recipeEndsWith(const std::string& text, const std::string& suffix) {
            if (text.size() < suffix.size()) return false;
            for (size_t i = 0; i < suffix.size(); i++) {
                if (std::tolower((unsigned char)text[text.size() - suffix.size() + i]) != suffix[i]) return false;
            }
            return true;
        }

        // ritmo's presetPalette slug: lower case, "(16)" and every non-alphanumeric dropped
        std::string recipeSlug(std::string_view text) {
            std::string lower;
            for (char c : text) lower.push_back((char)std::tolower((unsigned char)c));
            std::string withoutCounts;
            for (size_t i = 0; i < lower.size(); i++) {
                if (lower[i] == '(') {
                    size_t j = i + 1;
                    while (j < lower.size() && std::isdigit((unsigned char)lower[j])) j++;
                    if (j > i + 1 && j < lower.size() && lower[j] == ')') {
                        i = j;
                        continue;
                    }
                }
                withoutCounts.push_back(lower[i]);
            }
            std::string slug;
            for (char c : withoutCounts) {
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) slug.push_back(c);
            }
            return slug;
        }

        // Reads one object level, collecting every problem
        class RecipeReader {
        public:
            explicit RecipeReader(std::vector<PixelProblem>& problems) : problems(problems) {}

            void error(const std::string& field, const std::string& message, const std::string& fix) {
                problems.push_back(PixelProblem{"bad_recipe", field, message, fix});
            }

            static std::string at(const std::string& path, const std::string& key) { return path.empty() ? key : path + "." + key; }

            // unknown keys fail naming the valid ones
            void keys(const json& object, const std::string& path, const std::vector<std::string>& valid) {
                for (auto it = object.begin(); it != object.end(); ++it) {
                    if (std::find(valid.begin(), valid.end(), it.key()) == valid.end()) {
                        error(at(path, it.key()), "unknown key \"" + it.key() + "\"" + (path.empty() ? "" : " in " + path),
                              "valid keys" + (path.empty() ? std::string() : " of " + path) + ": " + recipeJoin(valid));
                    }
                }
            }

            const json* find(const json& object, const std::string& key) {
                auto it = object.find(key);
                return it == object.end() ? nullptr : &*it;
            }

            double number(const json& object, const std::string& key, const std::string& path, double fallback, double low, double high) {
                const json* value = find(object, key);
                if (!value) return fallback;
                return numberValue(*value, at(path, key), key, fallback, low, high);
            }

            double numberValue(const json& value, const std::string& field, const std::string& key, double fallback, double low, double high) {
                const std::string range = key + " is " + recipeNumberText(low) + "-" + recipeNumberText(high);
                if (!value.is_number()) {
                    error(field, field + " must be a number, not " + value.dump(), range);
                    return fallback;
                }
                const double v = value.get<double>();
                if (!std::isfinite(v) || v < low || v > high) {
                    error(field, field + " is " + recipeNumberText(v) + ", out of range", range);
                    return fallback;
                }
                return v;
            }

            int integer(const json& object, const std::string& key, const std::string& path, int fallback, int low, int high) {
                const json* value = find(object, key);
                if (!value) return fallback;
                const std::string field = at(path, key);
                const std::string range = key + " is a whole number " + std::to_string(low) + "-" + std::to_string(high);
                if (!value->is_number() || value->get<double>() != std::floor(value->get<double>())) {
                    error(field, field + " must be a whole number, not " + value->dump(), range);
                    return fallback;
                }
                const double v = value->get<double>();
                if (v < low || v > high) {
                    error(field, field + " is " + recipeNumberText(v) + ", out of range", range);
                    return fallback;
                }
                return (int)v;
            }

            bool boolean(const json& object, const std::string& key, const std::string& path, bool fallback) {
                const json* value = find(object, key);
                if (!value) return fallback;
                if (!value->is_boolean()) {
                    error(at(path, key), at(path, key) + " must be true or false, not " + value->dump(), key + " is true or false");
                    return fallback;
                }
                return value->get<bool>();
            }

            std::string text(const json& object, const std::string& key, const std::string& path, const std::string& fallback) {
                const json* value = find(object, key);
                if (!value) return fallback;
                if (!value->is_string()) {
                    error(at(path, key), at(path, key) + " must be a string, not " + value->dump(), key + " is a string");
                    return fallback;
                }
                return value->get<std::string>();
            }

            // index of the option, or fallbackIndex
            int oneOf(const json& object, const std::string& key, const std::string& path, const std::vector<std::string>& options, int fallbackIndex) {
                const json* value = find(object, key);
                if (!value) return fallbackIndex;
                if (value->is_string()) {
                    const auto it = std::find(options.begin(), options.end(), value->get<std::string>());
                    if (it != options.end()) return (int)(it - options.begin());
                }
                error(at(path, key), at(path, key) + " is " + value->dump() + ", not a valid value", key + " is one of " + recipeJoin(options));
                return fallbackIndex;
            }

            // an object at key (empty when absent; a problem when not an object)
            const json* object(const json& parent, const std::string& key, const std::string& path) {
                const json* value = find(parent, key);
                if (!value) return nullptr;
                if (!value->is_object()) {
                    error(at(path, key), at(path, key) + " must be an object, not " + value->dump(), key + " is an object");
                    return nullptr;
                }
                return value;
            }

            std::vector<PixelProblem>& problems;
        };

        const std::vector<std::string> kFollowNames = {"off", "straight", "bend"};
        const std::vector<std::string> kOutlineNames = {"auto", "on", "off"};
        const std::vector<std::string> kMetricNames = {"oklab", "ciede2000"};
        const std::vector<std::string> kDitherNames = {"none", "ordered"};
        const std::vector<std::string> kReduceNames = {"common", "outlines"};
        const std::vector<std::string> kAnchorNames = {"ground", "feet", "center", "box"};
        const std::vector<std::string> kLoopNames = {"forward", "pingpong"};
        const std::vector<std::string> kFormatNames = {"png", "aseprite"};
        const std::vector<std::string> kMethodNames = {"dominant", "balanced", "distinct"};
        const std::vector<std::string> kToolNames = {"erase", "restore", "paint"};

        constexpr double kMaxCell = 128;      // R3's largest cell (max_cell)
        constexpr double kMaxSide = 4096;     // R1's largest source side

        const char* const kPaletteFix =
            "palette is \"none\", \"auto\", {\"auto\": {\"count\": 2-256, \"method\": \"dominant\"|\"balanced\"|\"distinct\"}}, "
            "a preset name (e.g. \"sweetie-16\"), a .palette asset path or GUID, a list of \"#rrggbb\", or {\"name\", \"colors\", \"locked\"}";

        void readGrid(RecipeReader& r, const json& raw, Recipe& out) {
            const json* value = r.find(raw, "grid");
            if (!value || (value->is_string() && value->get<std::string>() == "auto")) return;
            if (!value->is_object()) {
                r.error("grid", "grid is " + value->dump(), "grid is \"auto\", {\"kind\": \"cells\", \"cell_width\", \"offset_x\", \"offset_y\"} or {\"kind\": \"size\", \"width\"}");
                return;
            }
            const std::string kind = r.text(*value, "kind", "grid", "");
            if (kind == "auto") {
                r.keys(*value, "grid", {"kind"});
                return;
            }
            if (kind == "cells") {
                r.keys(*value, "grid", {"kind", "cell_width", "cell_height", "offset_x", "offset_y"});
                out.grid.kind = GridKind::Cells;
                if (!r.find(*value, "cell_width")) r.error("grid.cell_width", "a cells grid needs cell_width", "cell_width is 1-128");
                out.grid.spec.cellWidth = r.number(*value, "cell_width", "grid", 8, 1, kMaxCell);
                out.grid.spec.cellHeight = r.number(*value, "cell_height", "grid", out.grid.spec.cellWidth, 1, kMaxCell);
                out.grid.spec.offsetX = r.number(*value, "offset_x", "grid", 0, -kMaxSide, kMaxSide);
                out.grid.spec.offsetY = r.number(*value, "offset_y", "grid", 0, -kMaxSide, kMaxSide);
                return;
            }
            if (kind == "size") {
                r.keys(*value, "grid", {"kind", "width", "height"});
                out.grid.kind = GridKind::Size;
                if (!r.find(*value, "width")) r.error("grid.width", "a size grid needs width", "width is 1-4096");
                out.grid.width = r.number(*value, "width", "grid", 1, 1, kMaxSide);
                if (r.find(*value, "height")) out.grid.height = r.number(*value, "height", "grid", out.grid.width, 1, kMaxSide);
                return;
            }
            r.error("grid.kind", "grid.kind is \"" + kind + "\"", "grid.kind is one of auto, cells, size");
        }

        void readRecipePalette(RecipeReader& r, const json& raw, Recipe& out) {
            const json* value = r.find(raw, "palette");
            if (!value || value->is_null()) return;
            if (value->is_string()) {
                const std::string name = value->get<std::string>();
                if (name == "default") return;
                if (name == "none" || name == "original") {
                    out.paletteMode = PaletteMode::None;
                    return;
                }
                if (name == "auto") {
                    out.paletteMode = PaletteMode::Auto;
                    out.autoPalette = AutoPalette{};
                    return;
                }
                if (recipeEndsWith(name, ".palette") || recipeIsGuid(name)) {
                    out.paletteMode = PaletteMode::Fixed;
                    RecipePalette palette;
                    (recipeIsGuid(name) ? palette.assetGuid : palette.assetPath) = name;
                    out.palette = palette;
                    return;
                }
                if (std::optional<RecipePalette> preset = presetPalette(name)) {
                    out.paletteMode = PaletteMode::Fixed;
                    out.palette = std::move(*preset);
                    return;
                }
                r.error("palette", "no preset palette named \"" + name + "\"", kPaletteFix);
                return;
            }
            const json* list = value->is_array() ? value : nullptr;
            RecipePalette palette;
            palette.name = "Custom";
            if (value->is_object()) {
                if (const json* automatic = r.find(*value, "auto")) {
                    r.keys(*value, "palette", {"auto"});
                    out.paletteMode = PaletteMode::Auto;
                    AutoPalette autoPalette;
                    if (automatic->is_object()) {
                        r.keys(*automatic, "palette.auto", {"count", "method"});
                        autoPalette.count = r.integer(*automatic, "count", "palette.auto", 16, 2, 256);
                        autoPalette.method = (ProposeMethod)r.oneOf(*automatic, "method", "palette.auto", kMethodNames, 0);
                    } else if (!(automatic->is_boolean() && automatic->get<bool>())) {
                        r.error("palette.auto", "palette.auto is " + automatic->dump(), "palette.auto is {\"count\": 2-256, \"method\": \"dominant\"|\"balanced\"|\"distinct\"}");
                    }
                    out.autoPalette = autoPalette;
                    return;
                }
                r.keys(*value, "palette", {"name", "colors", "locked", "asset"});
                palette.name = r.text(*value, "name", "palette", "Custom");
                if (const json* asset = r.object(*value, "asset", "palette")) {
                    r.keys(*asset, "palette.asset", {"guid", "path"});
                    palette.assetGuid = r.text(*asset, "guid", "palette.asset", "");
                    palette.assetPath = r.text(*asset, "path", "palette.asset", "");
                }
                list = r.find(*value, "colors");
                if (list && !list->is_array()) {
                    r.error("palette.colors", "palette.colors is " + list->dump(), "palette.colors is a list of \"#rrggbb\"");
                    list = nullptr;
                }
            }
            if (!value->is_object() && !list) {
                r.error("palette", "palette is " + value->dump(), kPaletteFix);
                return;
            }
            if (list) {
                for (size_t i = 0; i < list->size(); i++) {
                    const json& entry = (*list)[i];
                    jasperengine::pixelart::Rgba8 color;
                    bool hasAlpha = false;
                    if (!entry.is_string() || !jasperengine::pixelart::parseHex(entry.get<std::string>(), color, &hasAlpha) || hasAlpha) {
                        r.error("palette.colors[" + std::to_string(i) + "]", "palette colour " + entry.dump() + " is not a hex colour", "a palette colour is \"#rrggbb\"");
                        continue;
                    }
                    color.a = 255;
                    palette.colors.push_back(color);
                }
            }
            const bool unresolvedAsset = palette.colors.empty() && (!palette.assetGuid.empty() || !palette.assetPath.empty());
            if (!unresolvedAsset && (palette.colors.empty() || palette.colors.size() > 256)) {
                r.error("palette.colors", "the palette has " + std::to_string(palette.colors.size()) + " colours", "a palette holds 1-256 colours");
            }
            if (value->is_object()) {
                if (const json* locked = r.find(*value, "locked")) {
                    if (!locked->is_array()) {
                        r.error("palette.locked", "palette.locked is " + locked->dump(), "palette.locked is a list of colour indices");
                    } else {
                        for (const json& index : *locked) {
                            if (!index.is_number_integer() || index.get<int64_t>() < 0 || index.get<int64_t>() >= (int64_t)palette.colors.size()) {
                                r.error("palette.locked", "locked index " + index.dump() + " is not a colour of the palette",
                                        "palette.locked holds indices 0-" + std::to_string(std::max<int>(0, (int)palette.colors.size() - 1)));
                                continue;
                            }
                            palette.locked.push_back((int)index.get<int64_t>());
                        }
                    }
                }
            }
            out.paletteMode = PaletteMode::Fixed;
            out.palette = std::move(palette);
        }

        void readEdits(RecipeReader& r, const json& raw, Recipe& out) {
            const json* value = r.object(raw, "edits", "");
            if (!value) return;
            r.keys(*value, "edits", {"size", "strokes"});
            out.edits.size = r.text(*value, "size", "edits", "");
            const json* strokes = r.find(*value, "strokes");
            if (!strokes) return;
            if (!strokes->is_array()) {
                r.error("edits.strokes", "edits.strokes is " + strokes->dump(), "edits.strokes is a list of strokes, each a list of edits");
                return;
            }
            for (size_t s = 0; s < strokes->size(); s++) {
                const json& stroke = (*strokes)[s];
                const std::string where = "edits.strokes[" + std::to_string(s) + "]";
                if (!stroke.is_array()) {
                    r.error(where, where + " is " + stroke.dump(), "a stroke is a list of edits");
                    continue;
                }
                std::vector<Edit> edits;
                for (size_t e = 0; e < stroke.size(); e++) {
                    const json& item = stroke[e];
                    const std::string field = where + "[" + std::to_string(e) + "]";
                    if (!item.is_object()) {
                        r.error(field, field + " is " + item.dump(), "an edit is {\"tool\", \"x\", \"y\", \"area\", \"color\"}");
                        continue;
                    }
                    r.keys(item, field, {"tool", "x", "y", "area", "color"});
                    Edit edit;
                    if (!r.find(item, "tool")) r.error(field + ".tool", "an edit needs a tool", "tool is one of " + recipeJoin(kToolNames));
                    edit.tool = (EditTool)r.oneOf(item, "tool", field, kToolNames, 0);
                    edit.x = r.integer(item, "x", field, 0, 0, (int)kMaxSide);
                    edit.y = r.integer(item, "y", field, 0, 0, (int)kMaxSide);
                    edit.area = r.boolean(item, "area", field, false);
                    if (edit.tool == EditTool::Paint) {
                        const std::string hex = r.text(item, "color", field, "");
                        bool hasAlpha = false;
                        if (!jasperengine::pixelart::parseHex(hex, edit.color, &hasAlpha) || hasAlpha) {
                            r.error(field + ".color", "a paint edit needs a colour, not \"" + hex + "\"", "color is \"#rrggbb\"");
                        }
                        edit.color.a = 255;
                    } else if (r.find(item, "color")) {
                        r.error(field + ".color", "only a paint edit has a colour", "drop color, or use tool \"paint\"");
                    }
                    edits.push_back(edit);
                }
                out.edits.strokes.push_back(std::move(edits));
            }
        }

        void readSheet(RecipeReader& r, const json& raw, Recipe& out) {
            const json* value = r.find(raw, "sheet");
            if (!value) return;
            if (value->is_string()) {
                const std::string mode = value->get<std::string>();
                if (mode == "auto") return;
                if (mode == "off") {
                    out.sheetMode = SheetMode::Off;
                    return;
                }
            }
            if (!value->is_object()) {
                r.error("sheet", "sheet is " + value->dump(), "sheet is \"auto\", \"off\" or {\"frames\", \"animations\", \"anchor\", \"padding\"}");
                return;
            }
            r.keys(*value, "sheet", {"frames", "animations", "anchor", "padding", "fps", "loop"});
            Sheet sheet;
            if (const json* frames = r.find(*value, "frames"); frames && !frames->is_array()) {
                r.error("sheet.frames", "sheet.frames is " + frames->dump(), "sheet.frames is a list of {\"box\", \"include\", \"nudge\"}");
            } else if (frames) {
                for (size_t i = 0; i < frames->size(); i++) {
                    const json& entry = (*frames)[i];
                    const std::string where = "sheet.frames[" + std::to_string(i) + "]";
                    if (!entry.is_object()) {
                        r.error(where, where + " is " + entry.dump(), "a frame is {\"box\": {\"x\", \"y\", \"width\", \"height\"}, \"include\", \"nudge\"}");
                        continue;
                    }
                    r.keys(entry, where, {"box", "include", "nudge"});
                    SheetFrame frame;
                    const json empty = json::object();
                    const json* box = r.object(entry, "box", where);
                    const json& b = box ? *box : empty;
                    if (box) r.keys(*box, where + ".box", {"x", "y", "width", "height"});
                    const double x = r.number(b, "x", where + ".box", 0, -kMaxSide, 2 * kMaxSide);
                    const double y = r.number(b, "y", where + ".box", 0, -kMaxSide, 2 * kMaxSide);
                    const double width = r.number(b, "width", where + ".box", 0, 0, 2 * kMaxSide);
                    const double height = r.number(b, "height", where + ".box", 0, 0, 2 * kMaxSide);
                    frame.include = r.boolean(entry, "include", where, true);
                    if (const json* nudge = r.object(entry, "nudge", where)) {
                        r.keys(*nudge, where + ".nudge", {"x", "y"});
                        frame.nudge.x = (int)jsRound(r.number(*nudge, "x", where + ".nudge", 0, -kMaxSide, kMaxSide));
                        frame.nudge.y = (int)jsRound(r.number(*nudge, "y", where + ".nudge", 0, -kMaxSide, kMaxSide));
                    }
                    if (!(width >= 1) || !(height >= 1)) {
                        r.error(where + ".box", where + " needs a box at least 1 pixel wide and tall", "box width and height are at least 1");
                        continue;
                    }
                    frame.box = {(int)jsRound(x), (int)jsRound(y), (int)jsRound(width), (int)jsRound(height)};
                    sheet.frames.push_back(frame);
                }
            }
            auto playing = [&](const json& entry, const std::string& where, SheetAnimation& animation) {
                const json* fps = r.find(entry, "fps");
                if (fps && fps->is_number() && !(fps->get<double>() > 0)) {
                    r.error(where + ".fps", where + ".fps must be more than 0", "fps is more than 0, at most 60");
                } else {
                    animation.fps = r.number(entry, "fps", where, DEFAULT_FPS, 0, 60);
                }
                animation.loop = (Loop)r.oneOf(entry, "loop", where, kLoopNames, 0);
            };
            const json* animations = r.find(*value, "animations");
            if (!animations) {
                // A recipe from before sheets held animations: every frame is one animation
                SheetAnimation played;
                playing(*value, "sheet", played);
                if (!sheet.frames.empty()) {
                    std::vector<int> all;
                    for (int i = 0; i < (int)sheet.frames.size(); i++) all.push_back(i);
                    SheetAnimation animation = animationOf("animation-1", all);
                    animation.fps = played.fps;
                    animation.loop = played.loop;
                    sheet.animations.push_back(animation);
                }
            } else if (!animations->is_array()) {
                r.error("sheet.animations", "sheet.animations must be a list", "sheet.animations is a list of {\"name\", \"frames\", \"include\", \"fps\", \"loop\"}");
            } else {
                if (r.find(*value, "fps")) r.error("sheet.fps", "sheet.fps goes with each animation", "set fps in sheet.animations[i]");
                if (r.find(*value, "loop")) r.error("sheet.loop", "sheet.loop goes with each animation", "set loop in sheet.animations[i]");
                std::set<int> seen;
                std::set<std::string> names;
                const int count = (int)sheet.frames.size();
                for (size_t a = 0; a < animations->size(); a++) {
                    const json& entry = (*animations)[a];
                    const std::string where = "sheet.animations[" + std::to_string(a) + "]";
                    if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() || !isAnimationName(entry["name"].get<std::string>()) ||
                        !entry.contains("frames") || !entry["frames"].is_array()) {
                        r.error(where, where + " needs a name (letters, digits, \"-\", \"_\", \".\") and a list of frames",
                                "an animation is {\"name\": \"walk\", \"frames\": [0, 1]}");
                        continue;
                    }
                    r.keys(entry, where, {"name", "frames", "include", "fps", "loop"});
                    SheetAnimation animation;
                    animation.name = entry["name"].get<std::string>();
                    if (names.count(animation.name)) r.error(where + ".name", where + ": another animation is named \"" + animation.name + "\"", "animation names are unique");
                    names.insert(animation.name);
                    bool allValid = true;
                    for (const json& index : entry["frames"]) {
                        if (index.is_number_integer() && index.get<int64_t>() >= 0 && index.get<int64_t>() < count) {
                            animation.frames.push_back((int)index.get<int64_t>());
                        } else {
                            allValid = false;
                        }
                    }
                    if (!allValid || animation.frames.empty()) {
                        r.error(where + ".frames", where + ".frames must be frame numbers from 0 to " + std::to_string(count - 1) + ", at least one",
                                "frames are 0-" + std::to_string(std::max(0, count - 1)));
                    }
                    for (int i : animation.frames) {
                        if (seen.count(i)) r.error(where + ".frames", "frame " + std::to_string(i) + " is in more than one animation", "put each frame in one animation");
                        seen.insert(i);
                    }
                    animation.include = r.boolean(entry, "include", where, true);
                    playing(entry, where, animation);
                    sheet.animations.push_back(animation);
                }
                if ((int)seen.size() != count) {
                    r.error("sheet.animations", "every frame must be in an animation (" + std::to_string(count - (int)seen.size()) + " are not)",
                            "list every frame 0-" + std::to_string(std::max(0, count - 1)) + " in one animation");
                }
            }
            sheet.anchor = (Anchor)r.oneOf(*value, "anchor", "sheet", kAnchorNames, 0);
            sheet.padding = r.integer(*value, "padding", "sheet", 1, 0, 256);
            out.sheetMode = SheetMode::Given;
            out.sheet = std::move(sheet);
        }

        json encodeEdit(const Edit& edit) {
            json e = {{"tool", kToolNames[(int)edit.tool]}, {"x", edit.x}, {"y", edit.y}, {"area", edit.area}};
            if (edit.tool == EditTool::Paint) e["color"] = jasperengine::pixelart::toHex(edit.color, false);
            return e;
        }

    }

    const std::vector<std::string>& recipeKeys() {
        static const std::vector<std::string> keys = {
            "format", "version", "pipeline_version", "engine_version", "grid", "follow", "outlines", "moved", "sample", "palette",
            "metric", "dither", "dither_strength", "cleanup", "clean_keep", "target_width", "reduce_mode", "remove_background", "crop",
            "background", "lost_threshold", "edits", "sheet", "alpha_cutoff", "seed", "source", "result_sha256", "output_format", "forced"};
        return keys;
    }

    std::optional<RecipePalette> presetPalette(std::string_view name) {
        const std::string wanted = recipeSlug(name);
        if (wanted.empty()) return std::nullopt;
        for (const BuiltinPalette& builtin : builtinPalettes()) {
            if (recipeSlug(builtin.id) != wanted && recipeSlug(builtin.palette.name) != wanted) continue;
            RecipePalette palette;
            palette.name = builtin.palette.name.empty() ? builtin.id : builtin.palette.name;
            for (int i = 0; i < builtin.palette.count(); i++) {
                // a transparent slot is no colour to snap to
                if (i == builtin.palette.transparent || builtin.palette.colors[i].a == 0) continue;
                jasperengine::pixelart::Rgba8 color = builtin.palette.colors[i];
                color.a = 255;
                palette.colors.push_back(color);
            }
            return palette;
        }
        return std::nullopt;
    }

    bool decodeRecipe(const json& raw, Recipe& out, std::vector<PixelProblem>& problems) {
        out = Recipe();
        const size_t before = problems.size();
        RecipeReader r(problems);
        if (!raw.is_object()) {
            r.error("", "a recipe is a JSON object, not " + raw.dump(), "pass an object such as {\"palette\": \"sweetie-16\"}");
            return false;
        }
        // a newer recipe is refused whole: its keys may mean something this editor does not do
        if (const json* version = r.find(raw, "version")) {
            if (version->is_number_integer() && version->get<int64_t>() > Recipe::kVersion) {
                problems.push_back(PixelProblem{"recipe_version", "version",
                                                "the recipe is version " + version->dump() + "; this editor reads version " + std::to_string(Recipe::kVersion),
                                                "open it in a newer editor, or remove version to read it as version 1"});
                return false;
            }
        }
        if (const json* pipeline = r.find(raw, "pipeline_version")) {
            if (pipeline->is_number_integer() && pipeline->get<int64_t>() > Recipe::kPipelineVersion) {
                problems.push_back(PixelProblem{"recipe_version", "pipeline_version",
                                                "the recipe was made by cleanup pipeline " + pipeline->dump() + "; this editor runs pipeline " +
                                                    std::to_string(Recipe::kPipelineVersion),
                                                "open it in a newer editor, or remove pipeline_version to run it with this pipeline"});
                return false;
            }
        }
        r.keys(raw, "", recipeKeys());
        const std::string format = r.text(raw, "format", "", Recipe::kFormat);
        if (format != Recipe::kFormat) r.error("format", "format is \"" + format + "\"", std::string("format is \"") + Recipe::kFormat + "\"");
        r.integer(raw, "version", "", Recipe::kVersion, 1, Recipe::kVersion);
        out.pipelineVersion = r.integer(raw, "pipeline_version", "", Recipe::kPipelineVersion, 1, Recipe::kPipelineVersion);
        out.engineVersion = r.text(raw, "engine_version", "", "");

        readGrid(r, raw, out);
        out.follow = (Follow)r.oneOf(raw, "follow", "", kFollowNames, 0);
        out.outlines = (Outlines)r.oneOf(raw, "outlines", "", kOutlineNames, 0);
        if (const json* moved = r.find(raw, "moved")) {
            if (!moved->is_array()) {
                r.error("moved", "moved is " + moved->dump(), "moved is a list of [\"x:3\", 41.5] (a line and its position in source pixels)");
            } else {
                for (size_t i = 0; i < moved->size(); i++) {
                    const json& entry = (*moved)[i];
                    const std::string field = "moved[" + std::to_string(i) + "]";
                    bool ok = entry.is_array() && entry.size() == 2 && entry[0].is_string() && entry[1].is_number();
                    if (ok) {
                        const std::string id = entry[0].get<std::string>();
                        ok = id.size() > 2 && (id[0] == 'x' || id[0] == 'y') && id[1] == ':' &&
                             std::all_of(id.begin() + 2, id.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
                    }
                    if (!ok) {
                        r.error(field, field + " is " + entry.dump(), "a moved line is [\"x:<index>\" or \"y:<index>\", position]");
                        continue;
                    }
                    out.moved.push_back({entry[0].get<std::string>(), entry[1].get<double>()});
                }
            }
        }
        if (const json* sample = r.object(raw, "sample", "")) {
            r.keys(*sample, "sample", {"inset", "alpha_threshold"});
            out.sample.inset = r.number(*sample, "inset", "sample", 0.25, 0, 0.45);
            out.sample.alphaThreshold = r.number(*sample, "alpha_threshold", "sample", 128, 0, 255);
        }
        readRecipePalette(r, raw, out);
        const int metric = r.oneOf(raw, "metric", "", kMetricNames, 0);
        out.metric = metric == 1 ? ColorMetric::Ciede2000 : ColorMetric::OkLab;
        out.dither = (Dither)r.oneOf(raw, "dither", "", kDitherNames, 0);
        out.ditherStrength = r.number(raw, "dither_strength", "", 0.5, 0, 1);
        if (const json* cleanup = r.object(raw, "cleanup", "")) {
            r.keys(*cleanup, "cleanup", {"size", "strength"});
            out.cleanup.size = r.integer(*cleanup, "size", "cleanup", 0, 0, 16);
            out.cleanup.strength = r.number(*cleanup, "strength", "cleanup", 0.5, 0, 1);
        }
        if (const json* keep = r.object(raw, "clean_keep", "")) {
            r.keys(*keep, "clean_keep", {"size", "cells"});
            out.cleanKeep.size = r.text(*keep, "size", "clean_keep", "");
            if (const json* cells = r.find(*keep, "cells")) {
                if (!cells->is_array()) {
                    r.error("clean_keep.cells", "clean_keep.cells is " + cells->dump(), "clean_keep.cells is a list of cell indices");
                } else {
                    for (const json& cell : *cells) {
                        if (!cell.is_number_integer() || cell.get<int64_t>() < 0) {
                            r.error("clean_keep.cells", "cell " + cell.dump() + " is not a cell index", "a cell index is a whole number from 0");
                            continue;
                        }
                        out.cleanKeep.cells.push_back((int)cell.get<int64_t>());
                    }
                }
            }
        }
        if (const json* target = r.find(raw, "target_width"); target && !target->is_null()) {
            out.targetWidth = r.number(raw, "target_width", "", 1, 1, kMaxSide);
        }
        out.reduceMode = (ReduceMode)r.oneOf(raw, "reduce_mode", "", kReduceNames, 1);
        out.removeBackground = r.boolean(raw, "remove_background", "", false);
        out.crop = r.boolean(raw, "crop", "", false);
        if (const json* background = r.object(raw, "background", "")) {
            r.keys(*background, "background", {"tolerance", "enclosed", "specks", "shadow"});
            out.background.tolerance = r.number(*background, "tolerance", "background", 0.08, 0, 0.3);
            out.background.enclosed = r.boolean(*background, "enclosed", "background", false);
            out.background.specks = r.boolean(*background, "specks", "background", true);
            out.background.shadow = r.boolean(*background, "shadow", "background", false);
        }
        out.lostThreshold = r.number(raw, "lost_threshold", "", 0.06, 0, 1);
        readEdits(r, raw, out);
        readSheet(r, raw, out);
        out.alphaCutoff = r.number(raw, "alpha_cutoff", "", 128, 0, 255);
        if (const json* seed = r.find(raw, "seed")) {
            if (!seed->is_number_integer() || seed->get<int64_t>() < 0 || seed->get<int64_t>() > 4294967295LL) {
                r.error("seed", "seed is " + seed->dump(), "seed is a whole number 0-4294967295");
            } else {
                out.seed = (uint32_t)seed->get<int64_t>();
            }
        }
        if (const json* source = r.object(raw, "source", "")) {
            r.keys(*source, "source", {"path", "sha256", "frame", "layer"});
            out.source.path = r.text(*source, "path", "source", "");
            out.source.sha256 = r.text(*source, "sha256", "source", "");
            if (r.find(*source, "frame")) out.source.frame = r.integer(*source, "frame", "source", 1, 1, 65535);
            out.source.layer = r.text(*source, "layer", "source", "");
        }
        out.resultSha256 = r.text(raw, "result_sha256", "", "");
        if (const json* format = r.find(raw, "output_format"); format && !format->is_null()) {
            out.format = (OutputFormat)r.oneOf(raw, "output_format", "", kFormatNames, 0);
        }
        out.forced = r.boolean(raw, "forced", "", false);
        return problems.size() == before;
    }

    bool decodeRecipeText(std::string_view text, Recipe& out, std::vector<PixelProblem>& problems) {
        json raw = json::parse(text.begin(), text.end(), nullptr, false);
        if (raw.is_discarded()) {
            out = Recipe();
            problems.push_back(PixelProblem{"bad_recipe", "", "the recipe is not JSON", "pass a JSON object such as {\"palette\": \"sweetie-16\"}"});
            return false;
        }
        return decodeRecipe(raw, out, problems);
    }

    json encodeRecipe(const Recipe& recipe) {
        json j = json::object();
        j["format"] = Recipe::kFormat;
        j["version"] = Recipe::kVersion;
        j["pipeline_version"] = recipe.pipelineVersion;
        j["engine_version"] = recipe.engineVersion;
        switch (recipe.grid.kind) {
            case GridKind::Auto: j["grid"] = "auto"; break;
            case GridKind::Cells:
                j["grid"] = {{"kind", "cells"},
                             {"cell_width", recipeNumber(recipe.grid.spec.cellWidth)},
                             {"cell_height", recipeNumber(recipe.grid.spec.cellHeight)},
                             {"offset_x", recipeNumber(recipe.grid.spec.offsetX)},
                             {"offset_y", recipeNumber(recipe.grid.spec.offsetY)}};
                break;
            case GridKind::Size: {
                json g = {{"kind", "size"}, {"width", recipeNumber(recipe.grid.width)}};
                if (recipe.grid.height) g["height"] = recipeNumber(*recipe.grid.height);
                j["grid"] = g;
                break;
            }
        }
        j["follow"] = kFollowNames[(int)recipe.follow];
        j["outlines"] = kOutlineNames[(int)recipe.outlines];
        json moved = json::array();
        for (const auto& [id, position] : recipe.moved) moved.push_back({id, recipeNumber(position)});
        j["moved"] = moved;
        j["sample"] = {{"inset", recipeNumber(recipe.sample.inset)}, {"alpha_threshold", recipeNumber(recipe.sample.alphaThreshold)}};
        switch (recipe.paletteMode) {
            case PaletteMode::Default: break;
            case PaletteMode::None: j["palette"] = "none"; break;
            case PaletteMode::Auto: {
                const AutoPalette a = recipe.autoPalette.value_or(AutoPalette{});
                j["palette"] = {{"auto", {{"count", a.count}, {"method", kMethodNames[(int)a.method]}}}};
                break;
            }
            case PaletteMode::Fixed: {
                const RecipePalette p = recipe.palette.value_or(RecipePalette{});
                json colors = json::array();
                for (jasperengine::pixelart::Rgba8 c : p.colors) colors.push_back(jasperengine::pixelart::toHex(c, false));
                json palette = {{"name", p.name}, {"colors", colors}, {"locked", p.locked}};
                if (!p.assetGuid.empty() || !p.assetPath.empty()) palette["asset"] = {{"guid", p.assetGuid}, {"path", p.assetPath}};
                j["palette"] = palette;
                break;
            }
        }
        j["metric"] = kMetricNames[recipe.metric == ColorMetric::Ciede2000 ? 1 : 0];
        j["dither"] = kDitherNames[(int)recipe.dither];
        j["dither_strength"] = recipeNumber(recipe.ditherStrength);
        j["cleanup"] = {{"size", recipeNumber(recipe.cleanup.size)}, {"strength", recipeNumber(recipe.cleanup.strength)}};
        j["clean_keep"] = {{"size", recipe.cleanKeep.size}, {"cells", recipe.cleanKeep.cells}};
        j["target_width"] = recipe.targetWidth ? recipeNumber(*recipe.targetWidth) : json(nullptr);
        j["reduce_mode"] = kReduceNames[(int)recipe.reduceMode];
        j["remove_background"] = recipe.removeBackground;
        j["crop"] = recipe.crop;
        j["background"] = {{"tolerance", recipeNumber(recipe.background.tolerance)},
                           {"enclosed", recipe.background.enclosed},
                           {"specks", recipe.background.specks},
                           {"shadow", recipe.background.shadow}};
        j["lost_threshold"] = recipeNumber(recipe.lostThreshold);
        json strokes = json::array();
        for (const auto& stroke : recipe.edits.strokes) {
            json s = json::array();
            for (const Edit& edit : stroke) s.push_back(encodeEdit(edit));
            strokes.push_back(s);
        }
        j["edits"] = {{"size", recipe.edits.size}, {"strokes", strokes}};
        if (recipe.sheetMode == SheetMode::Given) {
            const Sheet sheet = recipe.sheet.value_or(Sheet{});
            json frames = json::array();
            for (const SheetFrame& f : sheet.frames) {
                frames.push_back({{"box", {{"x", f.box.x}, {"y", f.box.y}, {"width", f.box.width}, {"height", f.box.height}}},
                                  {"include", f.include},
                                  {"nudge", {{"x", f.nudge.x}, {"y", f.nudge.y}}}});
            }
            json animations = json::array();
            for (const SheetAnimation& a : sheet.animations) {
                animations.push_back({{"name", a.name}, {"frames", a.frames}, {"include", a.include}, {"fps", recipeNumber(a.fps)}, {"loop", kLoopNames[(int)a.loop]}});
            }
            j["sheet"] = {{"frames", frames}, {"animations", animations}, {"anchor", kAnchorNames[(int)sheet.anchor]}, {"padding", sheet.padding}};
        } else {
            j["sheet"] = recipe.sheetMode == SheetMode::Off ? "off" : "auto";
        }
        j["alpha_cutoff"] = recipeNumber(recipe.alphaCutoff);
        j["seed"] = recipe.seed;
        json source = {{"path", recipe.source.path}, {"sha256", recipe.source.sha256}, {"layer", recipe.source.layer}};
        if (recipe.source.frame) source["frame"] = *recipe.source.frame;
        j["source"] = source;
        j["result_sha256"] = recipe.resultSha256;
        j["output_format"] = recipe.format ? json(kFormatNames[(int)*recipe.format]) : json(nullptr);
        j["forced"] = recipe.forced;
        return j;
    }

    std::string recipeText(const Recipe& recipe) { return encodeRecipe(recipe).dump(); }

    const char* gridKindName(GridKind kind) {
        switch (kind) {
            case GridKind::Auto: return "auto";
            case GridKind::Cells: return "cells";
            case GridKind::Size: return "size";
        }
        return "auto";
    }
    const char* followName(Follow follow) { return kFollowNames[(int)follow].c_str(); }
    const char* outlinesName(Outlines outlines) { return kOutlineNames[(int)outlines].c_str(); }
    const char* reduceModeName(ReduceMode mode) { return kReduceNames[(int)mode].c_str(); }
    const char* anchorName(Anchor anchor) { return kAnchorNames[(int)anchor].c_str(); }
    const char* loopName(Loop loop) { return kLoopNames[(int)loop].c_str(); }
    const char* outputFormatName(OutputFormat format) { return kFormatNames[(int)format].c_str(); }
    const char* proposeMethodName(ProposeMethod method) { return kMethodNames[(int)method].c_str(); }

}
