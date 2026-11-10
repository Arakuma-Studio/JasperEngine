// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The checked reads Tiled's and LDtk's importers share. Map files are untrusted: every value is
// fetched through these, which accept a JSON number or bool, or the same as XML attribute text
// (TiledImporter turns a .tmx into the .tmj shape first), and report a wrong type or size as a
// parse problem with the value's location instead of throwing nlohmann's own exceptions.

#ifndef TILEMAP_IMPORT_JSON_READER_H
#define TILEMAP_IMPORT_JSON_READER_H

#include "tilemap/import/ImportPlan.h"

#include "json.hpp"
#include "util/NumberText.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace jasperengine::editor::tilemapimport::detail {

    // No layer may decode more cells than this (nor all layers of a file three times as many)
    inline constexpr int64_t kMaxLayerCells = 64000000;
    inline constexpr int64_t kMaxMapCells = 192000000;
    // Tilesets list every tile: a bigger count than any real atlas is a corrupt file
    inline constexpr int64_t kMaxTilesetTiles = 1000000;
    inline constexpr int64_t kMaxCoordinate = 1ll << 30;

    // Unwinds to the importer's entry point, which adds the problem and returns false
    struct ParseFailure {
        ImportProblem problem;
    };

    // The file being read and the location a value was found at: a JSON path, and for XML the
    // element's line, which turns the .tmx into the .tmj shape records in "__line"
    class SourceContext {
    public:
        std::string file;

        [[noreturn]] void fail(const std::string& field, const std::string& message, int line = 0) const {
            ImportProblem p;
            p.field = field.empty() && line > 0 ? "line " + std::to_string(line) : field;
            p.message = message;
            p.line = line;
            p.file = file;
            throw ParseFailure{std::move(p)};
        }
    };

    inline int lineOf(const nlohmann::json& node) {
        if (!node.is_object()) return 0;
        const auto it = node.find("__line");
        return it != node.end() && it->is_number_integer() ? it->get<int>() : 0;
    }

    inline std::string keyPath(const std::string& path, const char* key) {
        return path.empty() ? std::string(key) : path + "." + key;
    }

    inline std::string indexPath(const std::string& path, size_t index) {
        return path + "[" + std::to_string(index) + "]";
    }

    inline const nlohmann::json* findKey(const nlohmann::json& object, const char* key) {
        if (!object.is_object()) return nullptr;
        const auto it = object.find(key);
        return it == object.end() || it->is_null() ? nullptr : &*it;
    }

    inline bool parseIntText(const std::string& text, int64_t& out) {
        long long value = 0;
        if (!NumberText::parseInteger(text, value)) return false;
        out = value;
        return true;
    }

    // finite only: NumberText accepts "inf" and "nan", which no map field means
    inline bool parseDoubleText(const std::string& text, double& out) {
        return NumberText::parseDouble(text, out) && std::isfinite(out);
    }

    inline int64_t asInt(const nlohmann::json& v, const SourceContext& ctx, const std::string& path, int line = 0) {
        if (v.is_number_integer() || v.is_number_unsigned()) {
            if (v.is_number_unsigned() && v.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                ctx.fail(path, "the number is too large", line);
            }
            return v.get<int64_t>();
        }
        if (v.is_number_float()) {
            const double d = v.get<double>();
            if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 9.0e15) return static_cast<int64_t>(d);
            ctx.fail(path, "expected a whole number", line);
        }
        if (v.is_string()) {
            int64_t out = 0;
            const std::string& s = v.get_ref<const std::string&>();
            if (parseIntText(s, out)) return out;
            double d = 0.0;
            if (parseDoubleText(s, d) && std::floor(d) == d && std::fabs(d) < 9.0e15) return static_cast<int64_t>(d);
            ctx.fail(path, "expected a whole number, found '" + s + "'", line);
        }
        ctx.fail(path, "expected a whole number", line);
    }

    inline double asDouble(const nlohmann::json& v, const SourceContext& ctx, const std::string& path, int line = 0) {
        if (v.is_number()) {
            const double d = v.get<double>();
            if (std::isfinite(d)) return d;
            ctx.fail(path, "expected a finite number", line);
        }
        if (v.is_string()) {
            double out = 0.0;
            const std::string& s = v.get_ref<const std::string&>();
            if (parseDoubleText(s, out)) return out;
            ctx.fail(path, "expected a number, found '" + s + "'", line);
        }
        ctx.fail(path, "expected a number", line);
    }

    inline bool asBool(const nlohmann::json& v, const SourceContext& ctx, const std::string& path, int line = 0) {
        if (v.is_boolean()) return v.get<bool>();
        if (v.is_number_integer() || v.is_number_unsigned()) return v.get<int64_t>() != 0;
        if (v.is_string()) {
            const std::string& s = v.get_ref<const std::string&>();
            if (s == "1" || s == "true") return true;
            if (s == "0" || s == "false") return false;
        }
        ctx.fail(path, "expected true or false", line);
    }

    inline std::string asString(const nlohmann::json& v, const SourceContext& ctx, const std::string& path, int line = 0) {
        if (v.is_string()) return v.get<std::string>();
        ctx.fail(path, "expected a string", line);
    }

    inline std::string getString(const nlohmann::json& o, const char* key, const std::string& fallback, const SourceContext& ctx, const std::string& path) {
        const nlohmann::json* v = findKey(o, key);
        return v ? asString(*v, ctx, keyPath(path, key), lineOf(o)) : fallback;
    }

    inline int64_t getInt(const nlohmann::json& o, const char* key, int64_t fallback, const SourceContext& ctx, const std::string& path) {
        const nlohmann::json* v = findKey(o, key);
        return v ? asInt(*v, ctx, keyPath(path, key), lineOf(o)) : fallback;
    }

    inline double getDouble(const nlohmann::json& o, const char* key, double fallback, const SourceContext& ctx, const std::string& path) {
        const nlohmann::json* v = findKey(o, key);
        return v ? asDouble(*v, ctx, keyPath(path, key), lineOf(o)) : fallback;
    }

    inline bool getBool(const nlohmann::json& o, const char* key, bool fallback, const SourceContext& ctx, const std::string& path) {
        const nlohmann::json* v = findKey(o, key);
        return v ? asBool(*v, ctx, keyPath(path, key), lineOf(o)) : fallback;
    }

    // A required whole number within [min, max]
    inline int requireInt(const nlohmann::json& o, const char* key, int min, int max, const SourceContext& ctx, const std::string& path) {
        const nlohmann::json* v = findKey(o, key);
        if (!v) ctx.fail(keyPath(path, key), std::string("missing ") + key, lineOf(o));
        const int64_t value = asInt(*v, ctx, keyPath(path, key), lineOf(o));
        if (value < min || value > max) {
            ctx.fail(keyPath(path, key), std::string(key) + " is outside " + std::to_string(min) + ".." + std::to_string(max), lineOf(o));
        }
        return static_cast<int>(value);
    }

    // A whole number within [min, max], or the fallback when absent
    inline int boundedInt(const nlohmann::json& o, const char* key, int fallback, int min, int max, const SourceContext& ctx, const std::string& path) {
        const nlohmann::json* v = findKey(o, key);
        if (!v) return fallback;
        const int64_t value = asInt(*v, ctx, keyPath(path, key), lineOf(o));
        if (value < min || value > max) {
            ctx.fail(keyPath(path, key), std::string(key) + " is outside " + std::to_string(min) + ".." + std::to_string(max), lineOf(o));
        }
        return static_cast<int>(value);
    }

    // The JSON document, or a parse failure at the line and column nlohmann stopped at
    inline nlohmann::json parseJsonDocument(std::string_view text, const SourceContext& ctx) {
        try {
            return nlohmann::json::parse(text.begin(), text.end());
        } catch (const nlohmann::json::parse_error& e) {
            int line = 1;
            const size_t stop = e.byte > 0 ? std::min<size_t>(static_cast<size_t>(e.byte) - 1, text.size()) : 0;
            for (size_t i = 0; i < stop; i++) {
                if (text[i] == '\n') line++;
            }
            ctx.fail("", "not valid JSON: " + std::string(e.what()), line);
        }
    }

    // The path a file names, as a path relative to the map's directory, normalized
    std::string joinMapPath(const std::string& baseDir, const std::string& relative);

    // "#rrggbb" of a colour written "#rrggbb", "#aarrggbb", "rrggbb" or "aarrggbb"; "#ffffff" when malformed
    std::string normalizeColor(const std::string& text);

}

#endif // TILEMAP_IMPORT_JSON_READER_H
