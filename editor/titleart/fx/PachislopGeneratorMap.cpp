// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PachislopGeneratorMap.h"

#include "titleart/TitleEffectRegistry.h"

#include "title_pachislop_fx_map.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace ta = jasperengine::titleart;

namespace {

    // a YAML node as JSON: quoted scalars stay strings, plain ones become booleans and numbers
    ta::Json toJson(const YAML::Node& node) {
        switch (node.Type()) {
            case YAML::NodeType::Map: {
                ta::Json out = ta::Json::object();
                for (const auto& entry : node) out[entry.first.as<std::string>()] = toJson(entry.second);
                return out;
            }
            case YAML::NodeType::Sequence: {
                ta::Json out = ta::Json::array();
                for (const auto& entry : node) out.push_back(toJson(entry));
                return out;
            }
            case YAML::NodeType::Scalar: {
                const std::string& t = node.Scalar();
                if (node.Tag() == "!") return t;
                if (t == "true") return true;
                if (t == "false") return false;
                if (t == "null" || t == "~") return nullptr;
                char* end = nullptr;
                const long long i = std::strtoll(t.c_str(), &end, 10);
                if (!t.empty() && end && *end == '\0') return (int64_t)i;
                const double d = std::strtod(t.c_str(), &end);
                if (!t.empty() && end && *end == '\0') return d;
                return t;
            }
            default:
                return nullptr;
        }
    }

    std::string text(const ta::Json& j, const char* key) {
        if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return {};
        return j[key].get<std::string>();
    }

    TitlePachislopRow parseRow(const ta::Json& j) {
        TitlePachislopRow row;
        row.pachislop = text(j, "pachislop");
        row.type = text(j, "type");
        row.follow = text(j, "follow");
        if (j.contains("presets") && j["presets"].is_object()) {
            for (auto it = j["presets"].begin(); it != j["presets"].end(); ++it) {
                if (it.value().is_string()) row.presets.emplace_back(it.key(), it.value().get<std::string>());
            }
        }
        // "x/y": "center" -> x is component 0 of center, y component 1
        if (j.contains("vec2") && j["vec2"].is_object()) {
            for (auto it = j["vec2"].begin(); it != j["vec2"].end(); ++it) {
                const size_t slash = it.key().find('/');
                if (slash == std::string::npos || !it.value().is_string()) continue;
                const std::string target = it.value().get<std::string>();
                row.vec2[it.key().substr(0, slash)] = {target, 0};
                row.vec2[it.key().substr(slash + 1)] = {target, 1};
            }
        }
        if (j.contains("renames") && j["renames"].is_object()) {
            for (auto it = j["renames"].begin(); it != j["renames"].end(); ++it) {
                if (it.value().is_string()) row.renames[it.key()] = it.value().get<std::string>();
            }
        }
        if (j.contains("negated") && j["negated"].is_array()) {
            for (const ta::Json& key : j["negated"]) {
                if (key.is_string()) row.negated.insert(key.get<std::string>());
            }
        }
        if (j.contains("values") && j["values"].is_object()) {
            for (auto it = j["values"].begin(); it != j["values"].end(); ++it) {
                if (!it.value().is_object()) continue;
                std::map<std::string, std::string>& map = row.values[it.key()];
                for (auto v = it.value().begin(); v != it.value().end(); ++v) {
                    if (v.value().is_string()) map[v.key()] = v.value().get<std::string>();
                }
            }
        }
        if (j.contains("rows") && j["rows"].is_object()) {
            row.rowParams = text(j["rows"], "params");
            row.rowCount = text(j["rows"], "count");
            row.rowInto = text(j["rows"], "into");
        }
        return row;
    }

    // "rows[].angle" for a row field: the key `negated` and `values` use for it
    std::string rowFieldKey(const std::string& into, const std::string& field) {
        return into + "[]." + field;
    }

    // a value of our param `def` (`key` as `negated` and `values` name it) from Pachislop's
    ta::Json convertValue(const TitlePachislopRow& row, const ta::ParamDef& def, const std::string& key, const ta::Json& value) {
        if (value.is_string()) {
            auto map = row.values.find(key);
            if (map != row.values.end()) {
                auto mapped = map->second.find(value.get<std::string>());
                if (mapped != map->second.end()) return mapped->second;
            }
            return value;
        }
        if (value.is_number() && row.negated.count(key)) {
            if (value.is_number_integer()) return -value.get<int64_t>();
            const double negated = -value.get<double>();
            return negated == 0.0 ? ta::Json(0) : ta::Json(negated);
        }
        // Pachislop's stops leave alpha 1, mid 0.5 and the rgb interpolation out; ours store them
        if (def.kind == ta::ParamKind::Gradient && value.is_object() && value.contains("stops") && value["stops"].is_array()) {
            ta::Json stops = ta::Json::array();
            for (const ta::Json& s : value["stops"]) {
                if (!s.is_object()) continue;
                stops.push_back(ta::Json{{"pos", s.value("pos", ta::Json(0))},
                                         {"color", s.value("color", ta::Json("#000000"))},
                                         {"alpha", s.value("alpha", ta::Json(1))},
                                         {"mid", s.value("mid", ta::Json(0.5))}});
            }
            return ta::Json{{"stops", stops}, {"interp", value.value("interp", ta::Json("rgb"))}};
        }
        return value;
    }

    // our Vec2's value to set one component of: what is there, else the definition's default
    ta::Json& vec2Of(ta::Json& params, const ta::ParamDef& def) {
        ta::Json& v = params[def.key];
        if (v.is_array() && v.size() == 2) return v;
        v = def.defaultValue.is_array() && def.defaultValue.size() == 2 ? def.defaultValue : ta::Json::array({0, 0});
        return v;
    }

    int rowCountOf(const ta::Json& value) {
        // Pachislop's rows is an enum of "1".."6"
        if (value.is_number()) return (int)value.get<double>();
        if (value.is_string()) return (int)std::strtol(value.get<std::string>().c_str(), nullptr, 10);
        return -1;
    }

    void note(std::vector<std::string>* notes, std::string message) {
        if (notes) notes->push_back(std::move(message));
    }

}

const std::vector<TitlePachislopRow>& editor::titlePachislopGeneratorRows() {
    static const std::vector<TitlePachislopRow> rows = []() {
        std::vector<TitlePachislopRow> out;
        // the generators' section of the map the effects stream's PachislopFxMap.cpp reads too
        ta::Json file;
        try {
            file = toJson(YAML::Load(std::string(reinterpret_cast<const char*>(title_pachislop_fx_map), sizeof(title_pachislop_fx_map) - 1)));
        } catch (const YAML::Exception&) {
            return out;
        }
        if (!file.is_object() || !file.contains("generators") || !file["generators"].is_array()) return out;
        for (const ta::Json& j : file["generators"]) {
            if (j.is_object()) out.push_back(parseRow(j));
        }
        return out;
    }();
    return rows;
}

const TitlePachislopRow* editor::findTitlePachislopGenerator(std::string_view pachislopType) {
    for (const TitlePachislopRow& row : titlePachislopGeneratorRows()) {
        if (row.pachislop == pachislopType) return &row;
    }
    return nullptr;
}

std::string editor::titlePachislopGeneratorType(std::string_view pachislopType) {
    const TitlePachislopRow* row = findTitlePachislopGenerator(pachislopType);
    return row ? row->type : std::string();
}

std::string editor::titlePachislopTypeOfGenerator(std::string_view type) {
    for (const TitlePachislopRow& row : titlePachislopGeneratorRows()) {
        if (row.type == type) return row.pachislop;
    }
    return {};
}

std::string editor::titlePachislopGeneratorPresetId(std::string_view pachislopType, std::string_view pachislopPresetId) {
    const TitlePachislopRow* row = findTitlePachislopGenerator(pachislopType);
    if (!row) return {};
    for (const auto& [theirs, ours] : row->presets) {
        if (theirs == pachislopPresetId) return ours;
    }
    return {};
}

std::string editor::titlePachislopSnakeCase(std::string_view key) {
    std::string out;
    for (char c : key) {
        if (std::isupper((unsigned char)c)) {
            out += '_';
            out += (char)std::tolower((unsigned char)c);
        } else {
            out += c;
        }
    }
    return out;
}

TitlePachislopParam editor::titlePachislopGeneratorParam(const TitlePachislopRow& row, std::string_view pachislopKey) {
    using Kind = TitlePachislopParam::Kind;
    TitlePachislopParam out;
    const std::string key(pachislopKey);
    if (!row.follow.empty() && key == row.follow) {
        out.kind = Kind::LayerFollow;
        return out;
    }
    if (auto v = row.vec2.find(key); v != row.vec2.end()) {
        out.kind = Kind::Component;
        out.key = v->second.first;
        out.index = v->second.second;
        return out;
    }
    if (!row.rowParams.empty()) {
        if (key == row.rowCount) {
            out.kind = Kind::RowCount;
            out.key = row.rowInto;
            return out;
        }
        // "row{n}": "row3Angle" is field `angle` of row 2
        const size_t n = row.rowParams.find("{n}");
        const std::string prefix = n == std::string::npos ? row.rowParams : row.rowParams.substr(0, n);
        size_t digits = prefix.size();
        while (digits < key.size() && std::isdigit((unsigned char)key[digits])) digits++;
        if (key.compare(0, prefix.size(), prefix) == 0 && digits > prefix.size() && digits < key.size() &&
            std::isupper((unsigned char)key[digits])) {
            std::string field = key.substr(digits);
            field[0] = (char)std::tolower((unsigned char)field[0]);
            out.kind = Kind::RowField;
            out.key = row.rowInto;
            out.index = std::atoi(key.substr(prefix.size(), digits - prefix.size()).c_str()) - 1;
            out.field = titlePachislopSnakeCase(field);
            out.negated = row.negated.count(rowFieldKey(row.rowInto, out.field)) > 0;
            return out;
        }
    }
    auto renamed = row.renames.find(key);
    out.key = renamed != row.renames.end() ? renamed->second : titlePachislopSnakeCase(key);
    out.negated = row.negated.count(out.key) > 0;
    return out;
}

TitlePachislopGenerator editor::convertPachislopGenerator(std::string_view pachislopType, const ta::Json& pachislopParams,
                                                          std::vector<std::string>* notes) {
    using Kind = TitlePachislopParam::Kind;
    TitlePachislopGenerator out;
    const std::string at(pachislopType);
    const TitlePachislopRow* row = findTitlePachislopGenerator(pachislopType);
    if (!row) {
        note(notes, "Pachislop generator '" + at + "' has no counterpart");
        return out;
    }
    out.type = row->type;
    const ta::GeneratorDef* def = ta::findGenerator(row->type);
    if (!def) {
        note(notes, at + ": our generator '" + row->type + "' is not registered");
        return out;
    }
    if (!pachislopParams.is_object()) {
        if (!pachislopParams.is_null()) note(notes, at + ": its params are not an object");
        return out;
    }

    // numbered row params are gathered first: the row count may come after them
    int rowCount = -1;
    std::map<int, ta::Json> rowFields;
    for (auto it = pachislopParams.begin(); it != pachislopParams.end(); ++it) {
        const ta::Json& value = it.value();
        const TitlePachislopParam target = titlePachislopGeneratorParam(*row, it.key());
        const std::string where = at + "." + it.key();
        switch (target.kind) {
            case Kind::LayerFollow:
                if (value.is_string()) out.follow = value.get<std::string>();
                break;
            case Kind::Component: {
                const ta::ParamDef* pd = ta::findParam(def->params, target.key);
                if (!pd || pd->kind != ta::ParamKind::Vec2) {
                    note(notes, where + ": " + row->type + " has no point '" + target.key + "'; dropped");
                } else if (!value.is_number()) {
                    note(notes, where + ": not a number; dropped");
                } else {
                    vec2Of(out.params, *pd)[(size_t)target.index] = value;
                }
                break;
            }
            case Kind::RowCount:
                rowCount = rowCountOf(value);
                break;
            case Kind::RowField:
                if (target.index >= 0) rowFields[target.index][target.field] = value;
                break;
            case Kind::Param: {
                const ta::ParamDef* pd = ta::findParam(def->params, target.key);
                if (!pd) {
                    note(notes, where + ": " + row->type + " has no param '" + target.key + "'; dropped");
                    break;
                }
                out.params[target.key] = convertValue(*row, *pd, target.key, value);
                break;
            }
        }
    }

    if (!row->rowInto.empty() && (rowCount >= 0 || !rowFields.empty())) {
        const ta::ParamDef* pd = ta::findParam(def->params, row->rowInto);
        if (!pd || pd->kind != ta::ParamKind::RowArray) {
            note(notes, at + ": " + row->type + " has no rows '" + row->rowInto + "'; its row params dropped");
            return out;
        }
        // each row starts as our default's (Pachislop's ROW_DEFAULTS), as its rowsOf reads them
        const ta::Json base = pd->defaultValue.is_array() ? pd->defaultValue : ta::Json::array();
        const int limit = pd->maxRows > 0 ? (int)pd->maxRows : std::max<int>((int)base.size(), rowCount);
        const int count = std::clamp(rowCount >= 0 ? rowCount : (int)base.size(), 1, std::max(1, limit));
        ta::Json rows = ta::Json::array();
        for (int i = 0; i < count; i++) rows.push_back(i < (int)base.size() ? base[(size_t)i] : ta::defaultParams(pd->rows));
        for (const auto& [index, fields] : rowFields) {
            if (index >= count) {
                note(notes, at + ": row " + std::to_string(index + 1) + " is past its " + std::to_string(count) + " rows; dropped");
                continue;
            }
            for (auto f = fields.begin(); f != fields.end(); ++f) {
                const ta::ParamDef* field = ta::findParam(pd->rows, f.key());
                if (!field) {
                    note(notes, at + ": a row has no '" + f.key() + "'; dropped");
                    continue;
                }
                rows[(size_t)index][f.key()] = convertValue(*row, *field, rowFieldKey(row->rowInto, f.key()), f.value());
            }
        }
        out.params[row->rowInto] = rows;
    }
    return out;
}

TitlePachislopGenerator editor::convertPachislopGeneratorLayer(const ta::Json& pachislopLayer, std::vector<std::string>* notes) {
    const ta::Json generator = pachislopLayer.is_object() ? pachislopLayer.value("generator", ta::Json::object()) : ta::Json::object();
    const std::string type = text(generator, "type");
    const ta::Json params = generator.is_object() ? generator.value("params", ta::Json::object()) : ta::Json::object();
    TitlePachislopGenerator out = convertPachislopGenerator(type, params, notes);
    const std::string companionOf = text(pachislopLayer, "companionOf");
    if (!companionOf.empty()) {
        note(notes, type + ": a companion of layer '" + companionOf + "': it joins that layer's visible Group \"<layer> FX\" (D3)");
    }
    return out;
}
