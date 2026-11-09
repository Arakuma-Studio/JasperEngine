// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtIO.h"
#include "TitleArtMigrations.h"
#include "TitleArtSchema.h"
#include "TitleEffectRegistry.h"
#include "TitleTranslations.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <regex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;

// ---- small helpers ----

void NormalizeContext::warn(const std::string& path, const std::string& message) {
    if (warnings) warnings->push_back({path, message});
}

std::string titleart::jsonPathMember(const std::string& path, const std::string& name) {
    return path.empty() ? name : path + "." + name;
}

std::string titleart::jsonPathIndex(const std::string& path, size_t index) {
    return path + "[" + std::to_string(index) + "]";
}

std::string TitleArtParseError::toString() const {
    if (line > 0) return std::to_string(line) + ":" + std::to_string(column) + ": " + message;
    return message;
}

namespace {

    std::string shortNumber(double v) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.7g", v);
        return text;
    }

    // ---- the canonical writer ----

    void writeNumber(const Json& value, std::string& out) {
        if (value.is_number_unsigned()) {
            out += std::to_string(value.get<uint64_t>());
            return;
        }
        if (value.is_number_integer()) {
            out += std::to_string(value.get<int64_t>());
            return;
        }
        double v = value.get<double>();
        if (!std::isfinite(v) || v == 0.0) v = 0.0;
        if (v == std::floor(v) && std::abs(v) < 1e15) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.0f", v);
            out += text;
            return;
        }
        out += shortNumber(v);
    }

    // a JSON string literal (UTF-8 kept, control characters escaped)
    void writeString(const std::string& text, std::string& out) {
        out += '"';
        for (unsigned char c : text) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20) {
                        char escaped[8];
                        std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                        out += escaped;
                    } else {
                        out += (char)c;
                    }
            }
        }
        out += '"';
    }

    void writeValue(const Json& value, int indent, std::string& out) {
        const size_t pad = (size_t)indent * 2;
        const size_t inner = pad + 2;
        switch (value.type()) {
            case Json::value_t::null: out += "null"; return;
            case Json::value_t::boolean: out += value.get<bool>() ? "true" : "false"; return;
            case Json::value_t::number_integer:
            case Json::value_t::number_unsigned:
            case Json::value_t::number_float:
                writeNumber(value, out);
                return;
            case Json::value_t::string:
                writeString(value.get_ref<const std::string&>(), out);
                return;
            case Json::value_t::array: {
                if (value.empty()) {
                    out += "[]";
                    return;
                }
                const bool numbers = std::all_of(value.begin(), value.end(), [](const Json& v) { return v.is_number(); });
                if (numbers) {
                    out += "[";
                    for (size_t i = 0; i < value.size(); i++) {
                        if (i) out += ", ";
                        writeNumber(value[i], out);
                    }
                    out += "]";
                    return;
                }
                out += "[\n";
                for (size_t i = 0; i < value.size(); i++) {
                    out.append(inner, ' ');
                    writeValue(value[i], indent + 1, out);
                    out += i + 1 < value.size() ? ",\n" : "\n";
                }
                out.append(pad, ' ');
                out += ']';
                return;
            }
            case Json::value_t::object: {
                if (value.empty()) {
                    out += "{}";
                    return;
                }
                out += "{\n";
                size_t i = 0;
                for (auto it = value.begin(); it != value.end(); ++it, ++i) {
                    out.append(inner, ' ');
                    writeString(it.key(), out);
                    out += ": ";
                    writeValue(it.value(), indent + 1, out);
                    out += i + 1 < value.size() ? ",\n" : "\n";
                }
                out.append(pad, ' ');
                out += '}';
                return;
            }
            default:
                out += "null";
                return;
        }
    }

    // ---- value checks ----

    enum class Check { Ok, Clamped, Invalid };

    std::string typeWord(const SchemaMember& m) {
        switch (m.type) {
            case ValueType::Number: return "a number";
            case ValueType::Integer: return "a whole number";
            case ValueType::Bool: return "true or false";
            case ValueType::String: return "a string";
            case ValueType::Enum: return "one of " + [&m] {
                std::string list;
                for (size_t i = 0; i < m.options.size(); i++) list += (i ? ", " : "") + m.options[i];
                return list;
            }();
            case ValueType::Color: return "a colour (#rrggbb, #rgb or [r, g, b] in 0..1)";
            case ValueType::Vec2: return "[x, y]";
            case ValueType::Vec2List: return std::to_string(m.maxItems) + " [x, y] pairs";
            case ValueType::AssetRef: return "an asset ({path, guid} or null)";
            default: return std::string("a ") + valueTypeName(m.type);
        }
    }

    bool isFiniteNumber(const Json& v) {
        return v.is_number() && std::isfinite(v.get<double>());
    }

    // a number in [min, max]; `message` says what a clamp did
    Check clampNumber(Json& v, double min, double max, bool integer, std::string& message) {
        double x = v.get<double>();
        Check result = Check::Ok;
        if (integer && x != std::floor(x)) {
            message = shortNumber(x) + " rounded to " + shortNumber(std::round(x));
            x = std::round(x);
            result = Check::Clamped;
        }
        if (x < min || x > max) {
            const double clamped = std::clamp(x, min, max);
            message = shortNumber(x) + " clamped to " + shortNumber(clamped);
            x = clamped;
            result = Check::Clamped;
        }
        if (integer) v = (int64_t)x;
        else if (result == Check::Clamped || v.is_number_float()) v = x;
        return result;
    }

    Check checkAssetRef(Json& v, std::string& message) {
        if (v.is_null()) return Check::Ok;
        if (v.is_string()) {
            v = Json{{"path", v.get<std::string>()}, {"guid", ""}};
            return Check::Ok;
        }
        if (!v.is_object()) return Check::Invalid;
        Json out = Json::object();
        Json path = v.contains("path") ? v["path"] : Json("");
        Json guid = v.contains("guid") ? v["guid"] : Json("");
        if (!path.is_string()) path = "";
        Check result = Check::Ok;
        if (!guid.is_string() || (!guid.get<std::string>().empty() && !isTitleArtGuid(guid.get<std::string>()))) {
            message = "not a GUID: the path hint is kept";
            guid = "";
            result = Check::Clamped;
        }
        if (path.get<std::string>().empty() && guid.get<std::string>().empty()) {
            v = nullptr;
            return result;
        }
        out["path"] = path;
        out["guid"] = guid;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (it.key() != "path" && it.key() != "guid") out[it.key()] = it.value();
        }
        v = std::move(out);
        return result;
    }

    void normalizeObjectAt(Json& object, const SchemaKind& baseKind, const std::string& path, NormalizeContext& context);

    // members whose check never warns with its own path or recurses (their path is built lazily)
    bool isScalar(ValueType type) {
        switch (type) {
            case ValueType::Number: case ValueType::Integer: case ValueType::Bool: case ValueType::String:
            case ValueType::Enum: case ValueType::Color: case ValueType::Vec2: case ValueType::Vec2List:
            case ValueType::AssetRef: case ValueType::Id: case ValueType::LayerRef:
                return true;
            default:
                return false;
        }
    }

    Json memberDefault(const SchemaMember& m) {
        if (m.type == ValueType::Object && m.defaultValue.is_null() && !m.nullable) return Json::object();
        return m.defaultValue;
    }

    bool validMapKey(MapKeys keys, const std::string& key) {
        if (key.empty()) return false;
        switch (keys) {
            case MapKeys::Any: return true;
            case MapKeys::Index:
                return key.size() <= 9 && std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
                       (key.size() == 1 || key[0] != '0');
            case MapKeys::Tag:
                return key.size() == 4 && std::all_of(key.begin(), key.end(), [](char c) { return c >= 32 && c < 127; });
            case MapKeys::PartKey:
                return key == "face" || key == "bevel" || key == "side" || key.rfind("rim:", 0) == 0 ||
                       key.rfind("rim#", 0) == 0 || key.rfind("band:", 0) == 0 || key.rfind("band#", 0) == 0;
            case MapKeys::LanguageCode:
                return std::all_of(key.begin(), key.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '-'; });
        }
        return false;
    }

    // a map's entries in canonical order (Index keys numerically, others lexically)
    template<class Fn>
    Json sortedMap(const Json& map, MapKeys keys, Fn&& normalizeEntry) {
        std::vector<std::string> names;
        for (auto it = map.begin(); it != map.end(); ++it) names.push_back(it.key());
        if (keys == MapKeys::Index) {
            std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
                return a.size() != b.size() ? a.size() < b.size() : a < b;
            });
        } else {
            std::sort(names.begin(), names.end());
        }
        Json out = Json::object();
        for (const std::string& name : names) {
            Json entry = map[name];
            if (normalizeEntry(name, entry)) out[name] = std::move(entry);
        }
        return out;
    }

    // checks one member's value; Invalid leaves it for the caller to replace or refuse
    Check checkMember(const SchemaMember& m, Json& v, const Json& siblings, const std::string& path, NormalizeContext& context,
                      std::string& message) {
        const TitleArtSchema& schema = TitleArtSchema::get();
        switch (m.type) {
            case ValueType::Number:
                if (m.nullable && v.is_null()) return Check::Ok;
                if (!isFiniteNumber(v)) return Check::Invalid;
                return clampNumber(v, m.min, m.max, false, message);
            case ValueType::Integer: {
                if (!isFiniteNumber(v)) return Check::Invalid;
                Check result = clampNumber(v, m.min, m.max, true, message);
                if (!m.allowed.empty()) {
                    const double x = v.get<double>();
                    double best = m.allowed.front();
                    for (double a : m.allowed) {
                        if (std::abs(a - x) < std::abs(best - x)) best = a;
                    }
                    if (best != x) {
                        message = shortNumber(x) + " is not one of the allowed values: " + shortNumber(best) + " used";
                        v = (int64_t)best;
                        result = Check::Clamped;
                    }
                }
                return result;
            }
            case ValueType::Bool:
                return v.is_boolean() ? Check::Ok : Check::Invalid;
            case ValueType::String:
            case ValueType::LayerRef:
            case ValueType::Id:
                if (!v.is_string()) return Check::Invalid;
                if (!m.pattern.empty() && !std::regex_match(v.get<std::string>(), std::regex(m.pattern))) return Check::Invalid;
                return Check::Ok;
            case ValueType::Enum:
                if (!v.is_string()) return Check::Invalid;
                return std::find(m.options.begin(), m.options.end(), v.get<std::string>()) != m.options.end() ? Check::Ok : Check::Invalid;
            case ValueType::Color: {
                TitleColor color;
                if (!parseTitleColor(v, color)) return Check::Invalid;
                const std::string formatted = formatTitleColor(color);
                if (!v.is_string() || v.get<std::string>() != formatted) v = formatted;
                return Check::Ok;
            }
            case ValueType::Vec2: {
                if (!v.is_array() || v.size() != 2 || !isFiniteNumber(v[0]) || !isFiniteNumber(v[1])) return Check::Invalid;
                Check result = Check::Ok;
                for (size_t i = 0; i < 2; i++) {
                    if (clampNumber(v[i], m.min, m.max, false, message) == Check::Clamped) result = Check::Clamped;
                }
                return result;
            }
            case ValueType::Vec2List: {
                if (!v.is_array() || v.size() != m.maxItems) return Check::Invalid;
                for (const Json& p : v) {
                    if (!p.is_array() || p.size() != 2 || !isFiniteNumber(p[0]) || !isFiniteNumber(p[1])) return Check::Invalid;
                }
                return Check::Ok;
            }
            case ValueType::AssetRef:
                return checkAssetRef(v, message);
            case ValueType::AssetRefList: {
                if (!v.is_array()) return Check::Invalid;
                Json out = Json::array();
                for (size_t i = 0; i < v.size(); i++) {
                    Json ref = v[i];
                    std::string note;
                    const Check c = checkAssetRef(ref, note);
                    if (c == Check::Invalid || ref.is_null()) {
                        context.warn(jsonPathIndex(path, i), "not an asset: dropped");
                        continue;
                    }
                    if (!note.empty()) context.warn(jsonPathIndex(path, i), note);
                    out.push_back(std::move(ref));
                }
                if (m.maxItems && out.size() > m.maxItems) {
                    context.warn(path, std::to_string(out.size()) + " entries: only the first " + std::to_string(m.maxItems) + " are kept");
                    out.erase(out.begin() + (long)m.maxItems, out.end());
                }
                v = std::move(out);
                return Check::Ok;
            }
            case ValueType::StringList: {
                if (!v.is_array()) return Check::Invalid;
                Json out = Json::array();
                for (size_t i = 0; i < v.size(); i++) {
                    if (v[i].is_string()) out.push_back(v[i]);
                    else context.warn(jsonPathIndex(path, i), "not a string: dropped");
                }
                if (m.maxItems && out.size() > m.maxItems) out.erase(out.begin() + (long)m.maxItems, out.end());
                v = std::move(out);
                return Check::Ok;
            }
            case ValueType::Object: {
                if (m.nullable && v.is_null()) return Check::Ok;
                if (!v.is_object()) return Check::Invalid;
                const SchemaKind* child = schema.kind(m.kind);
                if (child) normalizeObjectAt(v, *child, path, context);
                return Check::Ok;
            }
            case ValueType::ObjectArray: {
                if (!v.is_array()) return Check::Invalid;
                const SchemaKind* child = schema.kind(m.kind);
                Json out = Json::array();
                for (size_t i = 0; i < v.size(); i++) {
                    Json item = std::move(v[i]);
                    // a nullable list keeps its null entries (S86's per-character band materials)
                    if (m.nullable && item.is_null()) {
                        out.push_back(nullptr);
                        continue;
                    }
                    if (!item.is_object()) {
                        context.warn(jsonPathIndex(path, i), "not an object: dropped");
                        continue;
                    }
                    if (child) normalizeObjectAt(item, *child, jsonPathIndex(path, out.size()), context);
                    out.push_back(std::move(item));
                }
                if (m.maxItems && out.size() > m.maxItems) {
                    context.warn(path, std::to_string(out.size()) + " entries: only the first " + std::to_string(m.maxItems) + " are kept");
                    out.erase(out.begin() + (long)m.maxItems, out.end());
                }
                v = std::move(out);
                return Check::Ok;
            }
            case ValueType::ObjectMap: {
                if (!v.is_object()) return Check::Invalid;
                const SchemaKind* child = schema.kind(m.kind);
                v = sortedMap(v, m.mapKeys, [&](const std::string& key, Json& entry) {
                    const std::string at = jsonPathMember(path, key);
                    if (!validMapKey(m.mapKeys, key)) {
                        context.warn(at, "not a valid key: dropped");
                        return false;
                    }
                    if (!entry.is_object()) {
                        context.warn(at, "not an object: dropped");
                        return false;
                    }
                    if (child) normalizeObjectAt(entry, *child, at, context);
                    return true;
                });
                return Check::Ok;
            }
            case ValueType::ObjectArrayMap: {
                if (!v.is_object()) return Check::Invalid;
                const SchemaKind* child = schema.kind(m.kind);
                v = sortedMap(v, m.mapKeys, [&](const std::string& key, Json& entry) {
                    const std::string at = jsonPathMember(path, key);
                    if (!validMapKey(m.mapKeys, key)) {
                        context.warn(at, "not a valid key: dropped");
                        return false;
                    }
                    if (!entry.is_array()) {
                        context.warn(at, "not a list: dropped");
                        return false;
                    }
                    Json out = Json::array();
                    for (size_t i = 0; i < entry.size(); i++) {
                        Json item = std::move(entry[i]);
                        if (!item.is_object()) continue;
                        if (child) normalizeObjectAt(item, *child, jsonPathIndex(at, out.size()), context);
                        out.push_back(std::move(item));
                    }
                    if (m.maxItems && out.size() > m.maxItems) {
                        context.warn(at, std::to_string(out.size()) + " entries: only the first " + std::to_string(m.maxItems) + " are kept");
                        out.erase(out.begin() + (long)m.maxItems, out.end());
                    }
                    entry = std::move(out);
                    return true;
                });
                return Check::Ok;
            }
            case ValueType::NumberMap:
            case ValueType::IntegerMap: {
                if (!v.is_object()) return Check::Invalid;
                const bool integer = m.type == ValueType::IntegerMap;
                v = sortedMap(v, m.mapKeys, [&](const std::string& key, Json& entry) {
                    const std::string at = jsonPathMember(path, key);
                    if (!validMapKey(m.mapKeys, key) || !isFiniteNumber(entry)) {
                        context.warn(at, "not a valid entry: dropped");
                        return false;
                    }
                    std::string note;
                    if (clampNumber(entry, m.min, m.max, integer, note) == Check::Clamped) context.warn(at, note);
                    return true;
                });
                return Check::Ok;
            }
            case ValueType::EffectParams:
            case ValueType::GeneratorParams: {
                if (!v.is_object()) return Check::Invalid;
                const std::string type = siblings.value("type", std::string());
                if (m.type == ValueType::EffectParams) {
                    if (const EffectDef* def = findEffect(type)) normalizeParams(def->params, v, path, context);
                } else {
                    if (const GeneratorDef* def = findGenerator(type)) normalizeParams(def->params, v, path, context);
                }
                return Check::Ok;
            }
            case ValueType::Json:
                if (m.normalize) m.normalize(v, path, context);
                return Check::Ok;
        }
        return Check::Ok;
    }

    void normalizeObjectAt(Json& object, const SchemaKind& baseKind, const std::string& path, NormalizeContext& context) {
        const TitleArtSchema& schema = TitleArtSchema::get();
        if (!object.is_object()) {
            context.warn(path, "expected an object: the defaults are used");
            object = Json::object();
        }
        const SchemaKind* kind = &baseKind;
        if (!baseKind.discriminator.empty()) {
            auto type = object.find(baseKind.discriminator);
            if (type == object.end() || !type->is_string()) {
                const SchemaMember* d = schema.member(baseKind, baseKind.discriminator);
                const std::string fallback = d && d->defaultValue.is_string() ? d->defaultValue.get<std::string>() : std::string();
                context.warn(jsonPathMember(path, baseKind.discriminator), "missing: '" + fallback + "' is used");
                object[baseKind.discriminator] = fallback;
            }
            kind = schema.variantOf(baseKind, object);
            // a type this build does not know: kept as it is, drawn as nothing (unknown_type)
            if (!kind) return;
        }
        if (baseKind.prepare) baseKind.prepare(object, context);
        if (kind != &baseKind && kind->prepare) kind->prepare(object, context);

        const std::vector<const SchemaMember*>& members = schema.membersOf(*kind);
        Json out = Json::object();
        Json::object_t& read = object.get_ref<Json::object_t&>();
        Json::object_t& written = out.get_ref<Json::object_t&>();
        written.reserve(members.size() + 2);
        // a canonical object holds the members in table order: look at the next one first
        size_t hint = 0;
        size_t found = 0;
        for (const SchemaMember* m : members) {
            Json* slot = nullptr;
            if (hint < read.size() && (read.begin() + (long)hint)->first == m->key) {
                slot = &(read.begin() + (long)hint++)->second;
            } else {
                for (size_t i = 0; i < read.size(); i++) {
                    auto entry = read.begin() + (long)i;
                    if (entry->first == m->key) {
                        slot = &entry->second;
                        hint = i + 1;
                        break;
                    }
                }
            }
            const bool present = slot != nullptr;
            found += present;
            if (kind->sparse && !present &&
                std::find(kind->alwaysWritten.begin(), kind->alwaysWritten.end(), m->key) == kind->alwaysWritten.end()) {
                continue;
            }
            Json value = present ? std::move(*slot) : memberDefault(*m);
            // a scalar's path is only built for a warning (normalize visits every member)
            std::string at = isScalar(m->type) ? std::string() : jsonPathMember(path, m->key);
            std::string message;
            Check check = checkMember(*m, value, out, at, context, message);
            if (check == Check::Invalid) {
                if (at.empty()) at = jsonPathMember(path, m->key);
                context.warn(at, "expected " + typeWord(*m) + ": the default is used");
                value = memberDefault(*m);
                checkMember(*m, value, out, at, context, message);
            } else if (check == Check::Clamped && !message.empty()) {
                if (at.empty()) at = jsonPathMember(path, m->key);
                context.warn(at, message);
            }
            // the keys are the schema's, unique: append without ordered_map's search
            written.emplace_back(m->key, std::move(value));
        }
        // unknown members follow, in the order read (reserved ones included)
        if (found < read.size()) {
            for (auto& entry : read) {
                if (!out.contains(entry.first)) written.emplace_back(entry.first, std::move(entry.second));
            }
        }
        object = std::move(out);
        if (baseKind.finish) baseKind.finish(object, path, context);
        if (kind != &baseKind && kind->finish) kind->finish(object, path, context);
    }

    // ---- the document pass ----

    // utf-8 code points of a string
    size_t codepointCount(const std::string& text) {
        size_t count = 0;
        for (unsigned char c : text) {
            if ((c & 0xC0) != 0x80) count++;
        }
        return count;
    }

    std::string truncateCodepoints(const std::string& text, size_t limit) {
        size_t count = 0;
        for (size_t i = 0; i < text.size(); i++) {
            if (((unsigned char)text[i] & 0xC0) != 0x80) {
                if (count == limit) return text.substr(0, i);
                count++;
            }
        }
        return text;
    }

    struct DocPass {
        NormalizeContext& context;
        TitleIdGenerator& ids;
        std::set<std::string> seen;
        size_t layers = 0;

        void fixId(Json& object, const std::string& path) {
            if (!object.is_object()) return;
            Json& id = object["id"];
            const std::string current = id.is_string() ? id.get<std::string>() : std::string();
            if (isTitleArtId(current) && seen.insert(current).second) return;
            if (!current.empty()) {
                context.warn(jsonPathMember(path, "id"), isTitleArtId(current) ? "'" + current + "' is used twice: a new id is given"
                                                                               : "'" + current + "' is not an id: a new one is given");
            }
            std::string fresh = ids.next();
            while (!seen.insert(fresh).second) fresh = ids.next();
            id = fresh;
        }

        void fixList(Json& list, const std::string& path) {
            if (!list.is_array()) return;
            for (size_t i = 0; i < list.size(); i++) fixId(list[i], jsonPathIndex(path, i));
        }

        void partKeys(Json& layer, const std::string& path) {
            auto parts = layer.find("part_effects");
            if (parts == layer.end() || !parts->is_object()) return;
            const Json* rims = nullptr;
            auto style = layer.find("style");
            if (style != layer.end() && style->is_object()) {
                auto r = style->find("rims");
                if (r != style->end() && r->is_array()) rims = &*r;
            }
            Json out = Json::object();
            std::vector<std::string> names;
            for (auto it = parts->begin(); it != parts->end(); ++it) names.push_back(it.key());
            for (const std::string& key : names) {
                std::string target = key;
                if (key.rfind("rim#", 0) == 0) {
                    const std::string digits = key.substr(4);
                    const bool number = !digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; });
                    const size_t n = number && digits.size() < 6 ? (size_t)std::stoul(digits) : SIZE_MAX;
                    if (rims && n < rims->size()) {
                        target = "rim:" + (*rims)[n].value("id", std::string());
                    } else {
                        context.warn(jsonPathMember(jsonPathMember(path, "part_effects"), key), "names no rim: dropped");
                        continue;
                    }
                } else if (key.rfind("rim:", 0) == 0) {
                    const std::string id = key.substr(4);
                    const bool exists = rims && std::any_of(rims->begin(), rims->end(), [&](const Json& r) { return r.value("id", std::string()) == id; });
                    if (!exists) {
                        context.warn(jsonPathMember(jsonPathMember(path, "part_effects"), key), "the rim does not exist: dropped");
                        continue;
                    }
                }
                Json& slot = out[target];
                if (slot.is_array()) {
                    for (Json& e : (*parts)[key]) slot.push_back(std::move(e));
                } else {
                    slot = std::move((*parts)[key]);
                }
            }
            // canonical order again (aliases became ids)
            Json sorted = Json::object();
            std::vector<std::string> keys;
            for (auto it = out.begin(); it != out.end(); ++it) keys.push_back(it.key());
            std::sort(keys.begin(), keys.end());
            for (const std::string& k : keys) sorted[k] = std::move(out[k]);
            *parts = std::move(sorted);
        }

        void layer(Json& l, const std::string& path, int depth) {
            fixId(l, path);
            if (layerTypeOf(l) == LayerType::Unknown) return;
            if (l.contains("masks")) fixList(l["masks"], jsonPathMember(path, "masks"));
            if (l.contains("effects")) fixList(l["effects"], jsonPathMember(path, "effects"));
            auto style = l.find("style");
            if (style != l.end() && style->is_object() && style->contains("rims")) {
                fixList((*style)["rims"], jsonPathMember(jsonPathMember(path, "style"), "rims"));
            }
            partKeys(l, path);
            auto parts = l.find("part_effects");
            if (parts != l.end() && parts->is_object()) {
                for (auto it = parts->begin(); it != parts->end(); ++it) {
                    fixList(it.value(), jsonPathMember(jsonPathMember(path, "part_effects"), it.key()));
                }
            }
            if (layerTypeOf(l) == LayerType::Text) {
                Json& text = l["text"];
                if (text.is_string() && codepointCount(text.get<std::string>()) > TITLE_MAX_TEXT_CODEPOINTS) {
                    context.warn(jsonPathMember(path, "text"), "longer than " + std::to_string(TITLE_MAX_TEXT_CODEPOINTS) + " characters: cut");
                    text = truncateCodepoints(text.get<std::string>(), TITLE_MAX_TEXT_CODEPOINTS);
                }
            }
            if (layerTypeOf(l) == LayerType::Group) {
                Json& children = l["children"];
                if (depth + 1 >= TITLE_MAX_GROUP_DEPTH && children.is_array() && !children.empty()) {
                    context.warn(jsonPathMember(path, "children"), "groups nest at most " + std::to_string(TITLE_MAX_GROUP_DEPTH) + " deep: the children are dropped");
                    children = Json::array();
                }
                list(children, jsonPathMember(path, "children"), depth + 1);
            }
        }

        void list(Json& layersList, const std::string& path, int depth) {
            if (!layersList.is_array()) return;
            Json out = Json::array();
            for (size_t i = 0; i < layersList.size(); i++) {
                if (layers >= TITLE_MAX_LAYERS) {
                    context.warn(path, "more than " + std::to_string(TITLE_MAX_LAYERS) + " layers: the rest are dropped");
                    break;
                }
                layers++;
                Json l = std::move(layersList[i]);
                layer(l, jsonPathIndex(path, out.size()), depth);
                out.push_back(std::move(l));
            }
            layersList = std::move(out);
        }
    };

    // a unique value among `used` ("main", "main_2", ...)
    std::string uniqueName(const std::string& base, std::set<std::string>& used, size_t maxLength) {
        if (used.insert(base).second) return base;
        for (int n = 2;; n++) {
            const std::string suffix = "_" + std::to_string(n);
            std::string candidate = base.substr(0, std::min(base.size(), maxLength - suffix.size())) + suffix;
            if (used.insert(candidate).second) return candidate;
        }
    }

    void normalizeDocument(Json& root, NormalizeContext& context) {
        const TitleArtSchema& schema = TitleArtSchema::get();
        if (!root.is_object()) root = Json::object();
        normalizeObjectAt(root, *schema.kind("document"), "", context);
        root["format"] = TITLE_ART_FORMAT;
        root["version"] = TITLE_ART_VERSION;
        const std::string guid = root["guid"].get<std::string>();
        if (!isTitleArtGuid(guid)) {
            if (!guid.empty()) context.warn("guid", "'" + guid + "' is not a GUID: a new one is given");
            root["guid"] = generateTitleArtGuid();
        }

        // canvas pixel budget
        Json& canvas = root["canvas"];
        int64_t w = canvas["width"].get<int64_t>(), h = canvas["height"].get<int64_t>();
        if (w * h > TITLE_MAX_CANVAS_PIXELS) {
            const double k = std::sqrt((double)TITLE_MAX_CANVAS_PIXELS / (double)(w * h));
            const int64_t nw = std::max<int64_t>(TITLE_MIN_CANVAS_SIDE, (int64_t)std::floor((double)w * k));
            const int64_t nh = std::max<int64_t>(TITLE_MIN_CANVAS_SIDE, (int64_t)std::floor((double)h * k));
            context.warn("canvas", std::to_string(w) + " x " + std::to_string(h) + " is more than " +
                                   std::to_string(TITLE_MAX_CANVAS_PIXELS) + " pixels: " + std::to_string(nw) + " x " + std::to_string(nh) + " used");
            canvas["width"] = nw;
            canvas["height"] = nh;
        }

        TitleIdGenerator localIds;
        TitleIdGenerator& ids = context.ids ? *context.ids : localIds;
        DocPass pass{context, ids, {}, 0};
        pass.list(root["layers"], "layers", 0);

        std::set<std::string> outputIds;
        Json& outputs = root["outputs"];
        for (size_t i = 0; i < outputs.size(); i++) {
            const std::string id = outputs[i]["id"].get<std::string>();
            const std::string unique = uniqueName(id, outputIds, 32);
            if (unique != id) {
                context.warn(jsonPathMember(jsonPathIndex("outputs", i), "id"), "'" + id + "' is used twice: '" + unique + "' is given");
                outputs[i]["id"] = unique;
            }
        }
        std::set<std::string> names;
        Json& exposed = root["exposed"];
        for (size_t i = 0; i < exposed.size(); i++) {
            const std::string name = exposed[i]["name"].get<std::string>();
            const std::string unique = uniqueName(name, names, 32);
            if (unique != name) {
                context.warn(jsonPathMember(jsonPathIndex("exposed", i), "name"), "'" + name + "' is used twice: '" + unique + "' is given");
                exposed[i]["name"] = unique;
            }
        }
        // S87's language rules (codes, keys, output ids)
        normalizeTitleLanguages(root, context);
    }

    // byte offset -> 1-based line and column
    void lineColumn(const std::string& text, size_t byte, int& line, int& column) {
        line = 1;
        column = 1;
        for (size_t i = 0; i < byte && i < text.size(); i++) {
            if (text[i] == '\n') {
                line++;
                column = 1;
            } else {
                column++;
            }
        }
    }

}

// ---- public API ----

std::string titleart::writeTitleArtJson(const Json& value, int indent) {
    std::string out;
    out.reserve(4096);
    writeValue(value, indent, out);
    return out;
}

namespace {

    // the members marked omitDefault that hold their default are left out of the text (S85's
    // additions): normalize puts them back, so the tree is the same and the file is the one a
    // build without them wrote
    void stripOmittedDefaults(Json& object, const SchemaKind& base) {
        if (!object.is_object()) return;
        const TitleArtSchema& schema = TitleArtSchema::get();
        const SchemaKind* kind = base.discriminator.empty() ? &base : schema.variantOf(base, object);
        if (!kind) return;
        for (const SchemaMember* m : schema.membersOf(*kind)) {
            auto it = object.find(m->key);
            if (it == object.end()) continue;
            if (m->omitDefault) {
                const Json def = m->type == ValueType::Object && m->defaultValue.is_null() && !m->kind.empty() ? schema.defaultObject(m->kind)
                                                                                                              : m->defaultValue;
                if (*it == def) {
                    object.erase(it);
                    continue;
                }
            }
            const SchemaKind* inner = m->kind.empty() ? nullptr : schema.kind(m->kind);
            if (!inner) continue;
            if (m->type == ValueType::Object) {
                stripOmittedDefaults(*it, *inner);
            } else if (m->type == ValueType::ObjectArray && it->is_array()) {
                for (Json& e : *it) stripOmittedDefaults(e, *inner);
            } else if (m->type == ValueType::ObjectMap && it->is_object()) {
                for (auto entry = it->begin(); entry != it->end(); ++entry) stripOmittedDefaults(entry.value(), *inner);
            }
        }
    }

}

std::string titleart::writeTitleArt(const TitleArtDoc& doc) {
    const SchemaKind* document = TitleArtSchema::get().kind("document");
    if (!document) return writeTitleArtJson(doc.root) + "\n";
    Json root = doc.root;
    stripOmittedDefaults(root, *document);
    return writeTitleArtJson(root) + "\n";
}

void titleart::normalizeTitleArtObject(Json& object, const std::string& kind, const std::string& path, NormalizeContext& context) {
    const SchemaKind* k = TitleArtSchema::get().kind(kind);
    if (k) normalizeObjectAt(object, *k, path, context);
}

void titleart::normalizeTitleArt(TitleArtDoc& doc, NormalizeContext& context) {
    normalizeDocument(doc.root, context);
}

void titleart::normalizeTitleArt(TitleArtDoc& doc, std::vector<TitleWarning>* warnings) {
    NormalizeContext context;
    context.warnings = warnings;
    normalizeDocument(doc.root, context);
}

TitleArtParseResult titleart::parseTitleArtJson(Json root) {
    TitleArtParseResult result;
    if (!root.is_object()) {
        result.error = {"not_object", 0, 0, "a Title Art file is a JSON object"};
        return result;
    }
    auto format = root.find("format");
    if (format == root.end() || !format->is_string() || format->get<std::string>() != TITLE_ART_FORMAT) {
        result.error = {"bad_format", 0, 0, std::string("not a Title Art file: \"format\" must be \"") + TITLE_ART_FORMAT + "\""};
        return result;
    }
    auto version = root.find("version");
    if (version == root.end() || !version->is_number_integer() || version->get<int64_t>() < 1) {
        result.error = {"bad_version", 0, 0, "\"version\" must be a whole number from 1"};
        return result;
    }
    result.versionRead = (int)version->get<int64_t>();
    if (result.versionRead > TITLE_ART_VERSION) {
        result.error = {"version_newer", 0, 0, "this Title Art file is version " + std::to_string(result.versionRead) +
                                                   ", written by a newer build; this build reads up to version " +
                                                   std::to_string(TITLE_ART_VERSION)};
        return result;
    }
    if (result.versionRead < TITLE_ART_VERSION) {
        MigrationLog log;
        std::string error;
        if (!titleArtMigrations().run(root, result.versionRead, TITLE_ART_VERSION, log, &error)) {
            result.error = {"migration_failed", 0, 0, error};
            return result;
        }
        result.migrationsApplied = log.applied;
        for (const std::string& note : log.notes) result.warnings.push_back({"", note});
    }
    result.doc.root = std::move(root);
    NormalizeContext context;
    context.warnings = &result.warnings;
    normalizeDocument(result.doc.root, context);
    result.ok = true;
    return result;
}

TitleArtParseResult titleart::parseTitleArt(const std::string& text) {
    TitleArtParseResult result;
    Json root;
    try {
        root = Json::parse(text);
    } catch (const Json::parse_error& e) {
        int line = 0, column = 0;
        lineColumn(text, e.byte > 0 ? (size_t)e.byte - 1 : 0, line, column);
        result.error = {"not_json", line, column, std::string("not valid JSON: ") + e.what()};
        return result;
    }
    return parseTitleArtJson(std::move(root));
}

TitleArtDoc titleart::makeNewTitleArt(const NewTitleArtOptions& options) {
    TitleIdGenerator ids = options.idSeed ? TitleIdGenerator(options.idSeed) : TitleIdGenerator();
    const std::string layerId = ids.next();
    TitleArtDoc doc;
    doc.root = Json::object();
    doc.root["format"] = TITLE_ART_FORMAT;
    doc.root["version"] = TITLE_ART_VERSION;
    doc.root["guid"] = options.guid.empty() ? generateTitleArtGuid() : options.guid;
    doc.root["canvas"] = Json{{"width", options.width}, {"height", options.height}};
    doc.root["layers"] = Json::array({Json{{"id", layerId}, {"type", "text"}, {"name", "Title"}, {"text", options.text}}});
    doc.root["exposed"] = Json::array({Json{{"name", "text"}, {"type", "text"},
                                            {"targets", Json::array({Json{{"layer", layerId}, {"field", "text"}}})}}});
    doc.root["outputs"] = Json::array({Json{{"id", "main"}}});
    NormalizeContext context;
    context.ids = &ids;
    normalizeDocument(doc.root, context);
    return doc;
}

std::string titleart::readTitleArtGuid(const std::string& text) {
    const Json root = Json::parse(text, nullptr, false);
    if (!root.is_object()) return {};
    auto guid = root.find("guid");
    return guid != root.end() && guid->is_string() && isTitleArtGuid(guid->get<std::string>()) ? guid->get<std::string>() : std::string();
}

// ---- fields ----

namespace {

    const Json* findById(const Json& list, const std::string& id) {
        if (!list.is_array()) return nullptr;
        for (const Json& item : list) {
            if (item.is_object() && item.value("id", std::string()) == id) return &item;
        }
        return nullptr;
    }

    long indexById(const Json& list, const std::string& id) {
        if (!list.is_array()) return -1;
        for (size_t i = 0; i < list.size(); i++) {
            if (list[i].is_object() && list[i].value("id", std::string()) == id) return (long)i;
        }
        return -1;
    }

    // `<n>` of "rim#<n>" (and the like), -1 when not a number
    long positional(const std::string& text) {
        if (text.empty() || text.size() > 6 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) return -1;
        return std::stol(text);
    }

    std::vector<std::string> idsOf(const Json& list, const std::string& prefix) {
        std::vector<std::string> out;
        if (!list.is_array()) return out;
        for (size_t i = 0; i < list.size(); i++) {
            out.push_back(prefix + "#" + std::to_string(i));
            if (list[i].is_object()) out.push_back(prefix + ":" + list[i].value("id", std::string()));
        }
        return out;
    }

    const Json& memberOr(const Json* object, const char* key) {
        static const Json none;
        if (!object || !object->is_object()) return none;
        auto it = object->find(key);
        return it != object->end() ? *it : none;
    }

    // `<type>` or `<type>#<n>`: the index in `effects` of the nth effect of that type
    long effectIndex(const Json& effects, const std::string& spec, std::string& error, std::vector<std::string>& valid) {
        std::string type = spec;
        long nth = 0;
        const size_t hash = spec.find('#');
        if (hash != std::string::npos) {
            type = spec.substr(0, hash);
            nth = positional(spec.substr(hash + 1));
            if (nth < 0) {
                error = "'" + spec + "': #<n> takes a number";
                return -1;
            }
        }
        std::map<std::string, long> counts;
        if (effects.is_array()) {
            for (size_t i = 0; i < effects.size(); i++) {
                const std::string t = effects[i].value("type", std::string());
                const long n = counts[t]++;
                valid.push_back(n ? "fx:" + t + "#" + std::to_string(n) : "fx:" + t);
                if (t == type && n == nth) return (long)i;
            }
        }
        error = "no effect '" + spec + "' here";
        return -1;
    }

    bool isEffectEntryMember(const std::string& key) {
        return key == "enabled" || key == "id" || key == "type" || key == "from_look" || key == "params";
    }

}

ExpandedFieldPath titleart::expandFieldShorthand(const Json* layer, const std::string& path) {
    ExpandedFieldPath out;
    auto fail = [&out](const std::string& code, const std::string& error, std::vector<std::string> valid, const std::string& word) {
        out.ok = false;
        out.code = code;
        out.error = error;
        out.nearest = nearestName(word, valid);
        out.validHere = std::move(valid);
        return out;
    };
    // the head (before the first '.') and the rest
    const size_t dot = path.find('.');
    std::string head = dot == std::string::npos ? path : path.substr(0, dot);
    const std::string rest = dot == std::string::npos ? std::string() : path.substr(dot + 1);
    auto join = [](const std::string& a, const std::string& b) { return b.empty() ? a : a + "." + b; };
    const Json& style = memberOr(layer, "style");
    const Json& rims = memberOr(&style, "rims");

    // <part>/fx:<type>[#n]
    std::string partKey;
    const size_t slash = head.find('/');
    if (slash != std::string::npos) {
        const std::string part = head.substr(0, slash);
        head = head.substr(slash + 1);
        if (part.rfind("rim#", 0) == 0) {
            const long n = positional(part.substr(4));
            if (n < 0 || !rims.is_array() || (size_t)n >= rims.size()) return fail("bad_path", "'" + part + "' names no rim", idsOf(rims, "rim"), part);
            partKey = "rim:" + rims[(size_t)n].value("id", std::string());
        } else if (part.rfind("rim:", 0) == 0) {
            if (!findById(rims, part.substr(4))) return fail("bad_path", "'" + part + "' names no rim", idsOf(rims, "rim"), part);
            partKey = part;
        } else if (part == "face") {
            partKey = part;
        } else if (part == "bevel" || part == "side" || part.rfind("band", 0) == 0) {
            if (!TitleArtSchema::get().member(*TitleArtSchema::get().kind("layer.text"), "style_3d")) {
                return fail("not_available", "the 3D parts arrive with S86", {}, part);
            }
            partKey = part;
        } else {
            std::vector<std::string> valid = idsOf(rims, "rim");
            valid.insert(valid.begin(), "face");
            return fail("bad_path", "'" + part + "' is not a part", valid, part);
        }
        if (head.rfind("fx:", 0) != 0) return fail("bad_path", "a part path continues with fx:<type>", {}, head);
    }

    if (head.rfind("fx:", 0) == 0) {
        const Json& effects = partKey.empty() ? memberOr(layer, "effects") : memberOr(&memberOr(layer, "part_effects"), partKey.c_str());
        std::string error;
        std::vector<std::string> valid;
        const long index = effectIndex(effects, head.substr(3), error, valid);
        if (index < 0) return fail("bad_path", error, valid, head);
        const std::string entry = (partKey.empty() ? std::string("effects") : "part_effects." + partKey) + "[" + std::to_string(index) + "]";
        const std::string first = rest.substr(0, rest.find('.'));
        const EffectDef* def = findEffect(effects[(size_t)index].value("type", std::string()));
        const bool entryMember = isEffectEntryMember(first) && !(def && findParam(def->params, first));
        out.ok = true;
        out.path = rest.empty() ? entry : (entryMember ? join(entry, rest) : join(entry + ".params", rest));
        return out;
    }
    if (head == "face") {
        out.ok = true;
        out.path = join("style.face", rest);
        return out;
    }
    if (head.rfind("rim#", 0) == 0 || head.rfind("rim:", 0) == 0) {
        long index = head[3] == '#' ? positional(head.substr(4)) : indexById(rims, head.substr(4));
        if (index < 0 || !rims.is_array() || (size_t)index >= rims.size()) return fail("bad_path", "'" + head + "' names no rim", idsOf(rims, "rim"), head);
        out.ok = true;
        out.path = join("style.rims[" + std::to_string(index) + "]", rest);
        return out;
    }
    if (head.rfind("mask#", 0) == 0 || head.rfind("mask:", 0) == 0) {
        const Json& masks = memberOr(layer, "masks");
        long index = head[4] == '#' ? positional(head.substr(5)) : indexById(masks, head.substr(5));
        if (index < 0 || !masks.is_array() || (size_t)index >= masks.size()) return fail("bad_path", "'" + head + "' names no mask", idsOf(masks, "mask"), head);
        out.ok = true;
        out.path = join("masks[" + std::to_string(index) + "]", rest);
        return out;
    }
    if (head == "gen") {
        out.ok = true;
        out.path = join("generator.params", rest);
        return out;
    }
    if (head == "adj") {
        out.ok = true;
        out.path = join("adjustment", rest);
        return out;
    }
    if (head == "3d" || head.rfind("band#", 0) == 0 || head.rfind("band:", 0) == 0) {
        const SchemaKind* text = TitleArtSchema::get().kind("layer.text");
        if (!text || !TitleArtSchema::get().member(*text, "style_3d")) return fail("not_available", "'" + head + "' arrives with S86", {}, head);
        const Json& bands = memberOr(&memberOr(layer, "style_3d"), "bands");
        if (head == "3d") {
            out.ok = true;
            out.path = join("style_3d", rest);
            return out;
        }
        long index = head[4] == '#' ? positional(head.substr(5)) : indexById(bands, head.substr(5));
        if (index < 0 || !bands.is_array() || (size_t)index >= bands.size()) return fail("bad_path", "'" + head + "' names no band", idsOf(bands, "band"), head);
        out.ok = true;
        out.path = join("style_3d.bands[" + std::to_string(index) + "]", rest);
        return out;
    }
    out.ok = true;
    out.path = path;
    return out;
}

FieldSetResult titleart::setTitleArtField(Json& object, const std::string& kind, const std::string& path, const Json& value, bool allowReadOnly) {
    FieldSetResult result;
    const TitleArtSchema& schema = TitleArtSchema::get();
    const SchemaKind* k = schema.kind(kind);
    if (!k) {
        result.code = "bad_path";
        result.error = "no schema kind '" + kind + "'";
        return result;
    }
    const ExpandedFieldPath expanded = expandFieldShorthand(kind == "document" ? nullptr : &object, path);
    if (!expanded.ok) {
        result.code = expanded.code;
        result.error = expanded.error;
        result.validHere = expanded.validHere;
        result.nearest = expanded.nearest;
        return result;
    }
    std::vector<FieldSegment> segments;
    std::string error;
    if (!parseFieldPath(expanded.path, segments, &error)) {
        result.code = "bad_path";
        result.error = error;
        return result;
    }
    // look up on a copy, so a failed write leaves `object` as it was (map entries are created on
    // the way)
    Json working = object;
    FieldLookup lookup = lookupField(working, *k, segments, true);
    if (!lookup.ok) {
        result.code = lookup.code;
        result.error = lookup.error;
        result.validHere = lookup.validHere;
        result.nearest = lookup.nearest;
        result.longPath = lookup.longPath;
        return result;
    }
    result.longPath = lookup.longPath;
    result.pattern = lookup.pattern;
    result.member = lookup.member;
    result.param = lookup.param;
    if (lookup.owner && lookup.owner->name != "document" && lookup.owner->name.rfind("layer", 0) != 0 && lookup.owner->name != "transform") {
        result.ownerLabel = lookup.owner->label;
    }
    if (lookup.param && lookup.owner && lookup.owner->name == "effect") {
        // the effect entry holding the params: its definition's label
        std::vector<FieldSegment> entry = segments;
        while (!entry.empty() && !(entry.back().key == "params" && !entry.back().isIndex)) entry.pop_back();
        if (!entry.empty()) entry.pop_back();
        if (!entry.empty()) {
            FieldLookup holder = lookupField(working, *k, entry, false);
            if (holder.ok && holder.value) {
                if (const EffectDef* def = findEffect(holder.value->value("type", std::string()))) result.ownerLabel = def->label;
            }
        }
    }
    result.from = *lookup.value;
    Json candidate = value;
    std::vector<TitleWarning> warnings;
    NormalizeContext context;
    context.warnings = &warnings;
    if (lookup.param) {
        if (!normalizeParamValue(*lookup.param, candidate, lookup.longPath, context)) {
            result.code = "bad_value";
            result.error = "'" + lookup.longPath + "' takes a " + std::string(paramKindName(lookup.param->kind));
            return result;
        }
    } else {
        const SchemaMember& m = *lookup.member;
        if (m.readOnly && !allowReadOnly) {
            result.code = "read_only";
            result.error = "'" + lookup.longPath + "' cannot be set";
            return result;
        }
        // what the path ends at: the member itself, one element of a list or map, or a map entry
        const char end = lookup.pattern.empty() ? ' ' : lookup.pattern.back();
        const bool element = (end == ']' && (m.type == ValueType::ObjectArray || m.type == ValueType::ObjectArrayMap)) ||
                             (end == '*' && m.type == ValueType::ObjectMap);
        const bool numberEntry = end == '*' && (m.type == ValueType::NumberMap || m.type == ValueType::IntegerMap);
        const bool listEntry = end == '*' && m.type == ValueType::ObjectArrayMap;
        auto refuse = [&](const std::string& what) {
            result.code = "bad_value";
            result.error = "'" + lookup.longPath + "' takes " + what;
            return result;
        };
        const SchemaKind* child = m.kind.empty() ? nullptr : schema.kind(m.kind);
        if (element) {
            if (!candidate.is_object()) return refuse("an object");
            if (child) normalizeObjectAt(candidate, *child, lookup.longPath, context);
        } else if (numberEntry) {
            if (!isFiniteNumber(candidate)) return refuse("a number");
            if (clampNumber(candidate, m.min, m.max, m.type == ValueType::IntegerMap, result.clampMessage) == Check::Clamped) result.clamped = true;
        } else if (listEntry) {
            if (!candidate.is_array()) return refuse("a list of effects");
            for (size_t i = 0; i < candidate.size(); i++) {
                if (!candidate[i].is_object()) return refuse("a list of effects");
                if (child) normalizeObjectAt(candidate[i], *child, jsonPathIndex(lookup.longPath, i), context);
            }
        } else {
            const Check check = checkMember(m, candidate, Json::object(), lookup.longPath, context, result.clampMessage);
            if (check == Check::Invalid) return refuse(typeWord(m));
            if (check == Check::Clamped) result.clamped = true;
        }
    }
    for (const TitleWarning& w : warnings) {
        result.clamped = true;
        if (result.clampMessage.empty()) result.clampMessage = w.message;
    }
    *lookup.value = candidate;
    result.to = candidate;
    object = std::move(working);
    result.ok = true;
    return result;
}

const Json* titleart::getTitleArtField(const Json& object, const std::string& kind, const std::string& path, std::string* longPath, std::string* error) {
    const SchemaKind* k = TitleArtSchema::get().kind(kind);
    if (!k) return nullptr;
    const ExpandedFieldPath expanded = expandFieldShorthand(kind == "document" ? nullptr : &object, path);
    if (!expanded.ok) {
        if (error) *error = expanded.error;
        return nullptr;
    }
    std::vector<FieldSegment> segments;
    if (!parseFieldPath(expanded.path, segments, error)) return nullptr;
    FieldLookup lookup = lookupField(const_cast<Json&>(object), *k, segments, false);
    if (!lookup.ok) {
        if (error) *error = lookup.error;
        return nullptr;
    }
    if (longPath) *longPath = lookup.longPath;
    return lookup.value;
}

void titleart::applyFieldOverrides(TitleArtDoc& doc, const std::vector<FieldOverride>& overrides, std::vector<TitleProblem>* problems) {
    if (overrides.empty()) return;
    for (const FieldOverride& o : overrides) {
        Json* target = &doc.root;
        std::string kind = "document";
        if (!o.layerId.empty()) {
            target = findLayer(doc.root, o.layerId);
            kind = "layer";
        }
        if (!target) {
            if (problems) problems->push_back({TitleSeverity::Warning, "override_skipped", o.layerId, o.path, "no layer '" + o.layerId + "'", {}});
            continue;
        }
        const FieldSetResult set = setTitleArtField(*target, kind, o.path, o.value, false);
        if (!set.ok && problems) {
            problems->push_back({TitleSeverity::Warning, "override_skipped", o.layerId, o.path, set.error, {}});
        }
    }
    normalizeTitleArt(doc, nullptr);
}

// ---- problems ----

std::vector<TitleProblem> titleart::titleArtDocumentProblems(const TitleArtDoc& doc) {
    std::vector<TitleProblem> problems;
    const TitleArtSchema& schema = TitleArtSchema::get();
    auto unknown = [&](const std::string& layer, const std::string& path, const std::string& what, const std::string& type) {
        problems.push_back({TitleSeverity::Warning, "unknown_type", layer, path,
                            what + " type '" + type + "' is not one this build knows: it is kept and draws nothing", Json{{"type", type}}});
    };
    auto effects = [&](const Json& list, const std::string& layer, const std::string& path) {
        if (!list.is_array()) return;
        for (size_t i = 0; i < list.size(); i++) {
            const std::string type = list[i].value("type", std::string());
            if (!findEffect(type)) unknown(layer, jsonPathIndex(path, i), "effect", type);
        }
    };
    const bool has3D =
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
        true;
#else
        false;
#endif
    forEachLayer(doc.root, [&](const Json& layer, const Json*, int) {
        const std::string id = layer.value("id", std::string());
        const LayerType type = layerTypeOf(layer);
        if (type == LayerType::Unknown) {
            unknown(id, "", "layer", layer.value("type", std::string()));
            return true;
        }
        effects(layer.value("effects", Json::array()), id, "effects");
        if (layer.contains("part_effects")) {
            for (auto it = layer["part_effects"].begin(); it != layer["part_effects"].end(); ++it) {
                effects(it.value(), id, "part_effects." + it.key());
            }
        }
        const Json& masks = layer.value("masks", Json::array());
        for (size_t i = 0; i < masks.size(); i++) {
            const Json& mask = masks[i];
            if (!schema.variantOf(*schema.kind("mask"), mask)) {
                unknown(id, jsonPathIndex("masks", i), "mask", mask.value("type", std::string()));
            } else if (mask.value("type", std::string()) == "layer") {
                const std::string source = mask.value("layer", std::string());
                if (!source.empty() && !findLayer(doc.root, source)) {
                    problems.push_back({TitleSeverity::Warning, "mask_layer_missing", id, jsonPathIndex("masks", i) + ".layer",
                                        "the mask's layer '" + source + "' does not exist: the mask shows everything", {}});
                }
            }
        }
        if (type == LayerType::Generator) {
            const Json& generator = layer.value("generator", Json::object());
            const std::string gen = generator.value("type", std::string());
            if (!findGenerator(gen)) unknown(id, "generator", "generator", gen);
            const std::string follow = layer.value("follow", std::string());
            if (!follow.empty() && !findLayer(doc.root, follow)) {
                problems.push_back({TitleSeverity::Warning, "follow_missing", id, "follow",
                                    "the followed layer '" + follow + "' does not exist: the generator is placed by its own transform", {}});
            }
        }
        if (type == LayerType::Adjustment && !schema.variantOf(*schema.kind("adjustment"), layer.value("adjustment", Json::object()))) {
            unknown(id, "adjustment", "adjustment", layer["adjustment"].value("type", std::string()));
        }
        if (layerTypeHasStyle(type)) {
            if (!has3D && layer.value("dimension", std::string("2d")) == "3d") {
                problems.push_back({TitleSeverity::Warning, "dimension_unavailable", id, "dimension",
                                    "Dimension 3D needs the 3D titles module: the layer renders as 2D", {}});
            }
            const bool ownPixels = type == LayerType::Image || (type == LayerType::Vector && layer.value("mode", std::string()) == "artwork");
            const Json* fill = getTitleArtField(layer, "layer", "style.face.fill.kind");
            if (!ownPixels && fill && fill->is_string() && fill->get<std::string>() == "source") {
                problems.push_back({TitleSeverity::Warning, "fill_source_without_pixels", id, "style.face.fill.kind",
                                    "a Source fill shows a layer's own pixels; this layer has none, so it is solid white", {}});
            }
        }
        return true;
    });
    const Json& exposed = doc.root.value("exposed", Json::array());
    for (size_t i = 0; i < exposed.size(); i++) {
        const Json& targets = exposed[i].value("targets", Json::array());
        for (size_t t = 0; t < targets.size(); t++) {
            const std::string layerId = targets[t].value("layer", std::string());
            const std::string field = targets[t].value("field", std::string());
            const Json* layer = layerId.empty() ? &doc.root : findLayer(doc.root, layerId);
            std::string error;
            if (!layer || !getTitleArtField(*layer, layerId.empty() ? "document" : "layer", field, nullptr, &error)) {
                problems.push_back({TitleSeverity::Warning, "exposed_target_missing", layerId,
                                    jsonPathIndex(jsonPathMember(jsonPathIndex("exposed", i), "targets"), t),
                                    "exposed parameter '" + exposed[i].value("name", std::string()) + "': " +
                                        (layer ? error : "no layer '" + layerId + "'"),
                                    {}});
            }
        }
    }
    return problems;
}
