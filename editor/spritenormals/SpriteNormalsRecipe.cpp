// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteNormalsRecipe.h"

#include "EditorVersion.h"
#include "texsynth/SynthesisRecipe.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using nlohmann::json;

namespace jasperengine::editor::spritenormals {

    namespace {

        // A float as the recipe records it: 6 decimals (1.15, not 1.149999976158142), whole
        // values as integers (3, not 3.0), so the text reads like the settings that were typed
        json number(double v) {
            const double rounded = std::round(v * 1e6) / 1e6;
            if (rounded == std::floor(rounded) && std::abs(rounded) < 1e15) return static_cast<int64_t>(rounded);
            return rounded;
        }

        json cellJson(const Cell& c) { return json::array({c.x, c.y, c.width, c.height}); }

        std::string article(const char* type) {
            const std::string t = type;
            return (t == "object" || t == "array") ? "an " + t : "a " + t;
        }

        // "strength is a string, expected a number"
        std::string wrongType(const std::string& where, const json& value, const char* expected) {
            return where + " is " + article(value.type_name()) + ", expected " + expected;
        }

        bool readNumber(const json& object, const char* key, const std::string& prefix, float& out, std::string& error) {
            const auto it = object.find(key);
            if (it == object.end()) return true;
            if (!it->is_number()) {
                error = wrongType(prefix + key, *it, "a number");
                return false;
            }
            out = it->get<float>();
            return true;
        }

        bool readBool(const json& object, const char* key, const std::string& prefix, bool& out, std::string& error) {
            const auto it = object.find(key);
            if (it == object.end()) return true;
            if (!it->is_boolean()) {
                error = wrongType(prefix + key, *it, "true or false");
                return false;
            }
            out = it->get<bool>();
            return true;
        }

        template <typename E>
        bool readEnum(const json& object, const char* key, E& out, bool (*parse)(const std::string&, E&), const char* names, std::string& error) {
            const auto it = object.find(key);
            if (it == object.end()) return true;
            if (!it->is_string()) {
                error = wrongType(key, *it, "a string");
                return false;
            }
            if (!parse(it->get<std::string>(), out)) {
                error = std::string(key) + " \"" + texsynth::printableUtf8(it->get<std::string>()) + "\" is not one of " + names;
                return false;
            }
            return true;
        }

        // A whole number (2, not 2.5); values past +-1e9 saturate there (validateSettings names them)
        bool readInt(const json& object, const char* key, const std::string& prefix, int& out, std::string& error) {
            const auto it = object.find(key);
            if (it == object.end()) return true;
            if (!it->is_number()) {
                error = wrongType(prefix + key, *it, "a whole number");
                return false;
            }
            const double v = it->get<double>();
            if (!std::isfinite(v) || std::floor(v) != v) {
                error = prefix + key + " is " + it->dump() + ", expected a whole number";
                return false;
            }
            out = static_cast<int>(std::min(std::max(v, -1e9), 1e9));
            return true;
        }

        bool quantizeFromJson(const json& value, Quantize& q, std::string& error) {
            if (!value.is_object()) {
                error = wrongType("quantize", value, "an object {directions, tilts, flat_deg, rotation_deg}");
                return false;
            }
            if (!readInt(value, "directions", "quantize.", q.directions, error) || !readInt(value, "tilts", "quantize.", q.tilts, error) ||
                !readNumber(value, "flat_deg", "quantize.", q.flatDeg, error) || !readNumber(value, "rotation_deg", "quantize.", q.rotationDeg, error)) {
                return false;
            }
            if (q.rotationDeg == 360.0f) q.rotationDeg = 0.0f;   // a whole turn: every surface takes 360 as 0
            return true;
        }

        bool gapsFromJson(const json& value, Gaps& gaps, std::string& error) {
            if (!value.is_object()) {
                error = wrongType("gaps", value, "an object {share}");
                return false;
            }
            return readNumber(value, "share", "gaps.", gaps.share, error);
        }

        bool regionFromJson(const json& value, const std::string& where, Region& region, std::string& error) {
            if (!value.is_object()) {
                error = wrongType(where, value, "an object");
                return false;
            }
            const std::string prefix = where + ".";
            if (value.contains("colors")) {
                const json& colors = value["colors"];
                if (!colors.is_array()) {
                    error = wrongType(prefix + "colors", colors, "an array of \"#rrggbb\"");
                    return false;
                }
                region.colors.clear();
                for (size_t i = 0; i < colors.size(); i++) {
                    const std::string at = prefix + "colors[" + std::to_string(i) + "]";
                    uint32_t rgb = 0;
                    if (!colors[i].is_string()) {
                        error = wrongType(at, colors[i], "a colour \"#rrggbb\"");
                        return false;
                    }
                    if (!parseColor(colors[i].get<std::string>(), rgb)) {
                        error = at + " \"" + texsynth::printableUtf8(colors[i].get<std::string>()) + "\" is not a colour \"#rrggbb\"";
                        return false;
                    }
                    region.colors.push_back(rgb);
                }
            }
            return readNumber(value, "tolerance", prefix, region.tolerance, error) && readNumber(value, "detail", prefix, region.detail, error) &&
                   readNumber(value, "lift", prefix, region.lift, error) && readBool(value, "flat", prefix, region.flat, error);
        }

        // ---- validateRecipe

        struct Checker {
            std::string error;

            bool fail(const std::string& message) {
                if (error.empty()) error = message;
                return false;
            }
            bool string(const json& object, const char* key, const std::string& where, bool required = false) {
                if (!object.contains(key)) return required ? fail(where + key + " is missing") : true;
                return object[key].is_string() || fail(wrongType(where + key, object[key], "a string"));
            }
            bool integer(const json& object, const char* key, const std::string& where, int64_t lo, int64_t hi, bool required = false) {
                if (!object.contains(key)) return required ? fail(where + key + " is missing") : true;
                return integerValue(object[key], where + key, lo, hi);
            }
            bool integerValue(const json& v, const std::string& where, int64_t lo, int64_t hi) {
                bool ok = false;
                if (v.is_number_unsigned()) ok = v.get<uint64_t>() <= static_cast<uint64_t>(hi) && (lo <= 0 || v.get<uint64_t>() >= static_cast<uint64_t>(lo));
                else if (v.is_number_integer()) ok = v.get<int64_t>() >= lo && v.get<int64_t>() <= hi;
                if (ok) return true;
                return fail(where + " is " + (v.is_number() ? v.dump() : article(v.type_name())) + ", expected an integer in [" + std::to_string(lo) +
                            ", " + std::to_string(hi) + "]");
            }
            // within +-1e6: read as float, a larger value would not convert
            bool numberValue(const json& v, const std::string& where) {
                if (v.is_number() && std::abs(v.get<double>()) <= 1e6) return true;
                if (!v.is_number()) return fail(wrongType(where, v, "a number"));
                return fail(where + " is " + v.dump() + ", expected a number within +-1e6");
            }
            bool pair(const json& object, const char* key, const std::string& where) {
                if (!object.contains(key)) return true;
                const json& v = object[key];
                if (!v.is_array() || v.size() != 2) return fail(wrongType(where + key, v, "[min, max]"));
                return numberValue(v[0], where + key + "[0]") && numberValue(v[1], where + key + "[1]");
            }
        };

    }

    // ------------------------------------------------------------------ settings

    json settingsToJson(const Settings& s) {
        json regions = json::array();
        for (const Region& r : s.regions) {
            json colors = json::array();
            for (uint32_t rgb : r.colors) colors.push_back(colorText(rgb));
            regions.push_back({{"colors", colors}, {"tolerance", number(r.tolerance)}, {"detail", number(r.detail)}, {"lift", number(r.lift)}, {"flat", r.flat}});
        }
        json j = json::object();
        j["preset"] = toString(s.preset);
        j["strength"] = number(s.strength);
        j["detail"] = number(s.detail);
        j["smoothing"] = number(s.smoothing);
        j["volume"] = number(s.volume);
        j["radius"] = s.radius ? number(*s.radius) : json("auto");
        j["softness"] = number(s.softness);
        j["alpha_threshold"] = number(s.alphaThreshold);
        j["profile"] = toString(s.profile);
        j["edges"] = toString(s.edges);
        j["resolution_scale"] = toString(s.resolutionScale);
        j["soft_edge"] = toString(s.softEdge);
        // the settings version only when it is not the current one (a map made before
        // high-resolution support, regenerated with its rules)
        if (s.version != kSettingsVersion) j["version"] = s.version;
        j["ignore_outline"] = s.ignoreOutline;
        j["regions"] = regions;
        j["directx"] = s.directx;
        j["height"] = s.height;
        // only when on (unquantized recipes and fingerprints are unchanged), or when the preset's
        // is on (an override to off must be recorded: readers resolve over the preset)
        if (s.quantize.on() || presetSettings(s.preset).quantize.on()) {
            j["quantize"] = {{"directions", s.quantize.directions},
                             {"tilts", s.quantize.tilts},
                             {"flat_deg", number(s.quantize.flatDeg)},
                             {"rotation_deg", number(s.quantize.rotationDeg == 360.0f ? 0.0f : s.quantize.rotationDeg)}};
        }
        // the same rule: recipes without gaps are unchanged, an override to off on stones is recorded
        if (s.gaps.on() || presetSettings(s.preset).gaps.on()) j["gaps"] = {{"share", number(s.gaps.share)}};
        return j;
    }

    bool settingsFromJson(const json& j, Settings& out, std::string& error) {
        if (!j.is_object()) {
            error = wrongType("settings", j, "an object");
            return false;
        }
        Settings s = out;
        const std::string presets = presetNames();
        if (!readEnum<Preset>(j, "preset", s.preset, parsePreset, presets.c_str(), error)) return false;
        if (!readNumber(j, "strength", "", s.strength, error) || !readNumber(j, "detail", "", s.detail, error) ||
            !readNumber(j, "smoothing", "", s.smoothing, error) || !readNumber(j, "volume", "", s.volume, error) ||
            !readNumber(j, "softness", "", s.softness, error) || !readNumber(j, "alpha_threshold", "", s.alphaThreshold, error)) {
            return false;
        }
        if (j.contains("radius")) {
            const json& r = j["radius"];
            if (r.is_string() && r.get<std::string>() == "auto") s.radius.reset();
            else if (r.is_number()) s.radius = r.get<float>();
            else {
                error = r.is_string() ? "radius \"" + texsynth::printableUtf8(r.get<std::string>()) + "\" is not \"auto\" or a number"
                                      : wrongType("radius", r, "\"auto\" or a number");
                return false;
            }
        }
        if (!readEnum<Profile>(j, "profile", s.profile, parseProfile, "auto, rounded, dome, bevel, inflate", error) ||
            !readEnum<Edges>(j, "edges", s.edges, parseEdges, "transparent, wrap, extend", error) ||
            !readEnum<ResolutionScale>(j, "resolution_scale", s.resolutionScale, parseResolutionScale, "auto, off", error) ||
            !readEnum<SoftEdge>(j, "soft_edge", s.softEdge, parseSoftEdge, "auto, solid", error) ||
            !readInt(j, "version", "", s.version, error)) {
            return false;
        }
        if (!readBool(j, "ignore_outline", "", s.ignoreOutline, error) || !readBool(j, "directx", "", s.directx, error) ||
            !readBool(j, "height", "", s.height, error)) {
            return false;
        }
        if (j.contains("regions")) {
            const json& regions = j["regions"];
            if (!regions.is_array()) {
                error = wrongType("regions", regions, "an array");
                return false;
            }
            s.regions.clear();
            for (size_t i = 0; i < regions.size(); i++) {
                Region region;
                if (!regionFromJson(regions[i], "regions[" + std::to_string(i) + "]", region, error)) return false;
                s.regions.push_back(region);
            }
        }
        if (j.contains("quantize") && !quantizeFromJson(j["quantize"], s.quantize, error)) return false;
        if (j.contains("gaps") && !gapsFromJson(j["gaps"], s.gaps, error)) return false;
        out = s;
        return true;
    }

    // ------------------------------------------------------------------ frames

    const char* toString(FramesFrom from) {
        switch (from) {
            case FramesFrom::Single: return "single";
            case FramesFrom::Sprite: return "sprite";
            case FramesFrom::Gif: return "gif";
            case FramesFrom::Apng: return "apng";
            case FramesFrom::Given: return "given";
        }
        return "single";
    }

    bool parseFramesFrom(const std::string& text, FramesFrom& out) {
        for (FramesFrom f : {FramesFrom::Single, FramesFrom::Sprite, FramesFrom::Gif, FramesFrom::Apng, FramesFrom::Given}) {
            if (text == toString(f)) {
                out = f;
                return true;
            }
        }
        return false;
    }

    // ------------------------------------------------------------------ recipe

    json makeRecipe(const RecipeInput& in) {
        json recipe = json::object();
        recipe["format"] = kRecipeFormat;
        // a generation with version 1 settings (an old map regenerated) writes a version 1 recipe
        recipe["version"] = in.settings.version < kRecipeVersion ? in.settings.version : kRecipeVersion;
        recipe["generator"] = std::string("JasperEngine ") + JASPERENGINE_EDITOR_VERSION;
        recipe["role"] = in.role;

        json source = json::object();
        source["path"] = in.sourcePath;
        source["sha1"] = in.sourceSha1;
        source["kind"] = in.fromSprite ? "sprite" : toString(in.sourceKind);
        // entity exactly when the kind is "sprite"; its submesh when not the first
        if (in.fromSprite) source["entity"] = in.spriteEntity;
        if (in.fromSprite && in.spriteSubmesh != 0) source["submesh"] = in.spriteSubmesh;
        recipe["source"] = source;

        json frames = json::object();
        frames["from"] = toString(in.framesFrom);
        json cells = json::array();
        for (const Cell& c : in.cells) cells.push_back(cellJson(c));
        frames["cells"] = cells;
        if (in.sourceKind == SourceKind::Gif || in.sourceKind == SourceKind::Apng) {
            frames["columns"] = in.columns;
            frames["rows"] = in.rows;
            json delays = json::array();
            for (const Delay& d : in.delays) delays.push_back(json::array({d.numerator, d.denominator}));
            frames["delays_ms"] = delays;
            frames["loop"] = in.loopCount;
        }
        recipe["frames"] = frames;
        recipe["settings"] = settingsToJson(in.settings);

        if (in.settings.radius) {
            recipe["radius_resolved"] = number(*in.settings.radius);
        } else if (in.result) {
            if (in.result->radiusPerIsland) {
                recipe["radius_resolved"] = "per-island";
                recipe["radius_range"] = json::array({number(in.result->radiusMin), number(in.result->radiusMax)});
            } else {
                recipe["radius_resolved"] = number(in.result->radiusMin);
            }
        }
        if (in.result && in.settings.ignoreOutline && in.result->outlineColor) recipe["outline"] = colorText(*in.result->outlineColor);
        if (in.result && in.settings.height) {
            recipe["height_range_px"] = json::array({number(in.result->heightMin), number(in.result->heightMax)});
        }
        auto output = [](const std::string& path) { return path.empty() ? json(nullptr) : json(path); };
        recipe["outputs"] = {{"normal", output(in.normalOutput)}, {"atlas", output(in.atlasOutput)}, {"height", output(in.heightOutput)}};
        // the paint layer is not a source: painting never changes source.sha1 (staleness)
        if (in.paintedPixels > 0) recipe["paint"] = {{"painted", in.paintedPixels}};
        return recipe;
    }

    bool recipeText(const json& recipe, std::string& text, std::string& error) {
        return texsynth::recipeText(recipe, text, error);
    }

    bool validateRecipe(const json& recipe, std::string& error) {
        Checker c;
        if (!recipe.is_object()) {
            error = wrongType("the recipe", recipe, "an object");
            return false;
        }
        if (!recipe.contains("format") || !recipe["format"].is_string() || recipe["format"].get<std::string>() != kRecipeFormat) {
            error = std::string("format is not \"") + kRecipeFormat + "\" (not a sprite normals recipe)";
            return false;
        }
        bool ok = c.integer(recipe, "version", "", 1, 1000000, true) && c.string(recipe, "generator", "") && c.string(recipe, "role", "");
        if (ok) {
            if (!recipe.contains("source")) ok = c.fail("source is missing");
            else if (!recipe["source"].is_object()) ok = c.fail(wrongType("source", recipe["source"], "an object"));
            else {
                const json& s = recipe["source"];
                ok = c.string(s, "path", "source.", true) && c.string(s, "sha1", "source.") && c.string(s, "kind", "source.") &&
                     c.string(s, "entity", "source.") && c.integer(s, "submesh", "source.", 0, 65535);
            }
        }
        if (ok && recipe.contains("frames")) {
            const json& f = recipe["frames"];
            if (!f.is_object()) ok = c.fail(wrongType("frames", f, "an object"));
            else {
                ok = c.string(f, "from", "frames.") && c.integer(f, "columns", "frames.", 0, 65535) && c.integer(f, "rows", "frames.", 0, 65535) &&
                     c.integer(f, "loop", "frames.", 0, 0xFFFFFFFFLL);
                if (ok && f.contains("from")) {
                    FramesFrom from;
                    if (!parseFramesFrom(f["from"].get<std::string>(), from)) {
                        ok = c.fail("frames.from \"" + texsynth::printableUtf8(f["from"].get<std::string>()) + "\" is not one of single, sprite, gif, apng, given");
                    }
                }
                if (ok && f.contains("cells")) {
                    const json& cells = f["cells"];
                    if (!cells.is_array()) ok = c.fail(wrongType("frames.cells", cells, "an array of [x, y, w, h]"));
                    for (size_t i = 0; ok && cells.is_array() && i < cells.size(); i++) {
                        const std::string at = "frames.cells[" + std::to_string(i) + "]";
                        if (!cells[i].is_array() || cells[i].size() != 4) {
                            ok = c.fail(wrongType(at, cells[i], "[x, y, w, h]"));
                            break;
                        }
                        for (size_t k = 0; ok && k < 4; k++) ok = c.integerValue(cells[i][k], at + "[" + std::to_string(k) + "]", 0, 65535);
                    }
                }
                if (ok && f.contains("delays_ms")) {
                    const json& delays = f["delays_ms"];
                    if (!delays.is_array()) ok = c.fail(wrongType("frames.delays_ms", delays, "an array of [numerator, denominator]"));
                    for (size_t i = 0; ok && delays.is_array() && i < delays.size(); i++) {
                        const std::string at = "frames.delays_ms[" + std::to_string(i) + "]";
                        if (!delays[i].is_array() || delays[i].size() != 2) {
                            ok = c.fail(wrongType(at, delays[i], "[numerator, denominator]"));
                            break;
                        }
                        ok = c.integerValue(delays[i][0], at + "[0]", 0, 0xFFFFFFFFLL) && c.integerValue(delays[i][1], at + "[1]", 1, 0xFFFFFFFFLL);
                    }
                }
            }
        }
        if (ok && recipe.contains("settings")) {
            // the readers' types: settingsFromJson on a scratch copy (ranges are validateSettings')
            Settings scratch;
            std::string why;
            if (!settingsFromJson(recipe["settings"], scratch, why)) ok = c.fail(recipe["settings"].is_object() ? "settings." + why : why);
            if (ok) {
                const json& s = recipe["settings"];
                for (const char* key : {"strength", "detail", "smoothing", "volume", "softness", "alpha_threshold"}) {
                    if (ok && s.contains(key)) ok = c.numberValue(s[key], std::string("settings.") + key);
                }
                if (ok && s.contains("radius") && s["radius"].is_number()) ok = c.numberValue(s["radius"], "settings.radius");
                if (ok && s.contains("quantize")) {
                    for (const char* key : {"flat_deg", "rotation_deg"}) {
                        if (ok && s["quantize"].contains(key)) ok = c.numberValue(s["quantize"][key], std::string("settings.quantize.") + key);
                    }
                }
                if (ok && s.contains("gaps") && s["gaps"].contains("share")) ok = c.numberValue(s["gaps"]["share"], "settings.gaps.share");
            }
        }
        if (ok && recipe.contains("radius_resolved")) {
            const json& r = recipe["radius_resolved"];
            if (r.is_string()) {
                if (r.get<std::string>() != "per-island") ok = c.fail("radius_resolved \"" + texsynth::printableUtf8(r.get<std::string>()) + "\" is not a number or \"per-island\"");
            } else {
                ok = c.numberValue(r, "radius_resolved");
            }
        }
        ok = ok && c.pair(recipe, "radius_range", "") && c.pair(recipe, "height_range_px", "") && c.string(recipe, "outline", "");
        if (ok && recipe.contains("paint")) {
            if (!recipe["paint"].is_object()) ok = c.fail(wrongType("paint", recipe["paint"], "an object"));
            else ok = c.integer(recipe["paint"], "painted", "paint.", 0, kMaxPixels);
        }
        if (ok && recipe.contains("outputs")) {
            const json& outputs = recipe["outputs"];
            if (!outputs.is_object()) ok = c.fail(wrongType("outputs", outputs, "an object"));
            for (auto it = outputs.begin(); ok && outputs.is_object() && it != outputs.end(); ++it) {
                if (!it.value().is_string() && !it.value().is_null()) ok = c.fail(wrongType("outputs." + it.key(), it.value(), "a string or null"));
            }
        }
        if (!ok) error = c.error;
        return ok;
    }

    // ------------------------------------------------------------------ PNG chunk

    bool encodeWithRecipe(const Image& image, const json& recipe, std::vector<uint8_t>& png, std::string& error) {
        std::vector<uint8_t> plain;
        if (!encodePng(image, plain, error)) return false;
        std::string text;
        if (!recipeText(recipe, text, error)) return false;
        return texsynth::insertRecipeChunk(plain, text, png, error, kRecipeKeyword);
    }

    namespace {

        std::optional<json> parseRecipe(const std::optional<std::string>& text, std::string* error, RecipeReadError* origin) {
            if (!text) return std::nullopt;
            json parsed = json::parse(*text, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                if (error) *error = std::string("the ") + kRecipeKeyword + " chunk is not a JSON object";
                if (origin) *origin = RecipeReadError::Chunk;
                return std::nullopt;
            }
            return parsed;
        }

    }

    std::optional<json> readRecipe(const uint8_t* bytes, size_t size, std::string* error, RecipeReadError* origin) {
        if (error) error->clear();
        return parseRecipe(texsynth::readRecipeTextFromPng(bytes, size, error, kRecipeKeyword, origin), error, origin);
    }

    std::optional<json> readRecipe(const fs::path& png, std::string* error, RecipeReadError* origin) {
        if (error) error->clear();
        return parseRecipe(texsynth::readRecipeText(png, error, kRecipeKeyword, origin), error, origin);
    }

    std::optional<json> readRecipeHead(const fs::path& png, std::string* error, RecipeReadError* origin) {
        if (error) error->clear();
        return parseRecipe(texsynth::readRecipeTextHead(png, error, kRecipeKeyword, origin), error, origin);
    }

}
