// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SchemaArgs.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>

namespace jasperengine::editor::cli {

namespace {

std::vector<std::string> typesOf(const Json& schema) {
    std::vector<std::string> out;
    if (!schema.is_object() || !schema.contains("type")) return out;
    const Json& type = schema["type"];
    if (type.is_string()) out.push_back(type.get<std::string>());
    else if (type.is_array()) {
        for (const Json& item : type) {
            if (item.is_string()) out.push_back(item.get<std::string>());
        }
    }
    return out;
}

bool isRefSchema(const Json& schema) {
    std::vector<std::string> types = typesOf(schema);
    std::sort(types.begin(), types.end());
    return types == std::vector<std::string>{"integer", "string"};
}

bool isScalarKind(ArgKind kind) {
    return kind == ArgKind::String || kind == ArgKind::Integer || kind == ArgKind::Number || kind == ArgKind::Boolean ||
           kind == ArgKind::Enum || kind == ArgKind::Ref;
}

// A schema a plain word may stand for (a string branch somewhere): its JSON-less value is a string
bool acceptsString(const Json& schema) {
    if (!schema.is_object()) return false;
    if (!schema.contains("type") && !schema.contains("anyOf")) return true;   // any
    for (const std::string& type : typesOf(schema)) {
        if (type == "string") return true;
    }
    if (schema.contains("anyOf") && schema["anyOf"].is_array()) {
        for (const Json& branch : schema["anyOf"]) {
            if (acceptsString(branch)) return true;
        }
    }
    return false;
}

bool acceptsBoolean(const Json& schema) {
    if (!schema.is_object()) return false;
    for (const std::string& type : typesOf(schema)) {
        if (type == "boolean") return true;
    }
    if (schema.contains("anyOf") && schema["anyOf"].is_array()) {
        for (const Json& branch : schema["anyOf"]) {
            if (acceptsBoolean(branch)) return true;
        }
    }
    return false;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool parseBoolWord(const std::string& text, bool& value) {
    const std::string word = lower(text);
    if (word == "true" || word == "1" || word == "yes" || word == "on") {
        value = true;
        return true;
    }
    if (word == "false" || word == "0" || word == "no" || word == "off") {
        value = false;
        return true;
    }
    return false;
}

bool isDecimalInteger(const std::string& text) {
    size_t i = text.size() > 1 && text[0] == '-' ? 1 : 0;
    if (i >= text.size()) return false;
    for (; i < text.size(); i++) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    }
    return true;
}

// [-]digits[.digits][e[+-]digits] or [-].digits...: no hex, inf or nan
bool isDecimalNumber(const std::string& text) {
    size_t i = 0;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) i++;
    size_t digits = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { i++; digits++; }
    if (i < text.size() && text[i] == '.') {
        i++;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { i++; digits++; }
    }
    if (digits == 0) return false;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        i++;
        if (i < text.size() && (text[i] == '-' || text[i] == '+')) i++;
        size_t exponent = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { i++; exponent++; }
        if (exponent == 0) return false;
    }
    return i == text.size();
}

std::string trim(const std::string& text) {
    const size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

std::string joinValues(const Json& values) {
    std::string out;
    for (const Json& value : values) {
        out += (out.empty() ? "" : ", ") + (value.is_string() ? value.get<std::string>() : value.dump());
    }
    return out;
}

std::string jsonTypeName(const Json& value) {
    if (value.is_null()) return "null";
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer()) return "integer";
    if (value.is_number()) return "number";
    if (value.is_string()) return "string";
    if (value.is_array()) return "array";
    return "object";
}

bool matchesType(const Json& value, const std::string& type) {
    if (type == "integer") {
        if (value.is_number_integer()) return true;
        return value.is_number_float() && std::isfinite(value.get<double>()) && std::floor(value.get<double>()) == value.get<double>();
    }
    if (type == "number") return value.is_number();
    if (type == "string") return value.is_string();
    if (type == "boolean") return value.is_boolean();
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "null") return value.is_null();
    return true;
}

class Converter {
public:
    Converter(const ArgSpec& spec, const FileReader& reader): spec(spec), reader(reader) {}

    // The value of one occurrence of the flag (text = what followed it)
    Json convert(const std::string& raw) const {
        std::string text = raw;
        bool fromFile = false;
        if (text.size() > 1 && text[0] == '@') {
            text = reader(text.substr(1));
            fromFile = true;
        }
        switch (spec.kind) {
            case ArgKind::String:
            case ArgKind::Enum:
                if (fromFile && !text.empty() && text.back() == '\n') text.pop_back();
                if (fromFile && !text.empty() && text.back() == '\r') text.pop_back();
                return scalar(spec.kind, spec.schema, text);
            case ArgKind::ScalarArray:
                return array(fromFile ? trim(text) : text);
            case ArgKind::FreeObject:
                return freeObject(fromFile ? trim(text) : text);
            case ArgKind::Json:
                return json(trim(text));
            default:
                return scalar(spec.kind, spec.schema, fromFile ? trim(text) : text);
        }
    }

    Json scalar(ArgKind kind, const Json& schema, const std::string& text) const {
        switch (kind) {
            case ArgKind::String:
                return text;
            case ArgKind::Integer: {
                if (!isDecimalInteger(text)) fail("expects an integer, not \"" + text + "\"");
                errno = 0;
                const long long value = std::strtoll(text.c_str(), nullptr, 10);
                if (errno == ERANGE) fail("integer out of range: " + text);
                return value;
            }
            case ArgKind::Number: {
                if (!isDecimalNumber(text)) fail("expects a number, not \"" + text + "\"");
                const double value = std::strtod(text.c_str(), nullptr);
                if (!std::isfinite(value)) fail("expects a finite number, not \"" + text + "\"");
                return value;
            }
            case ArgKind::Boolean: {
                bool value = false;
                if (!parseBoolWord(text, value)) fail("expects true/false/1/0/yes/no/on/off, not \"" + text + "\"");
                return value;
            }
            case ArgKind::Enum: {
                for (const Json& option : schema["enum"]) {
                    if (option.is_string() ? option.get<std::string>() == text : option.dump() == text) return option;
                }
                fail("must be one of: " + joinValues(schema["enum"]) + " (not \"" + text + "\")");
                return nullptr;
            }
            case ArgKind::Ref: {
                if (isDecimalInteger(text)) return std::strtoll(text.c_str(), nullptr, 10);
                if (!text.empty() && text[0] == '"') {
                    const Json parsed = Json::parse(text, nullptr, false);
                    if (parsed.is_discarded() || !parsed.is_string()) fail("a quoted name must be a JSON string: " + text);
                    return parsed;
                }
                return text;
            }
            default:
                return text;
        }
    }

    Json array(const std::string& text) const {
        if (!text.empty() && text[0] == '[') {
            const Json parsed = Json::parse(text, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_array()) fail("is not a valid JSON array: " + text);
            return parsed;
        }
        const Json& items = spec.schema.contains("items") ? spec.schema["items"] : Json::object();
        const ArgKind itemKind = classify(items);
        Json out = Json::array();
        if (text.empty()) return out;
        size_t start = 0;
        while (true) {
            const size_t comma = text.find(',', start);
            const std::string piece = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            out.push_back(scalar(itemKind, items, itemKind == ArgKind::String ? piece : trim(piece)));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return out;
    }

    Json freeObject(const std::string& text) const {
        if (!text.empty() && text[0] == '{') {
            const Json parsed = Json::parse(text, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) fail("is not a valid JSON object: " + text);
            return parsed;
        }
        const size_t eq = text.find('=');
        if (eq == std::string::npos || eq == 0) fail("takes a JSON object or KEY=VALUE, not \"" + text + "\"");
        const std::string key = text.substr(0, eq);
        std::string value = text.substr(eq + 1);
        if (value.size() > 1 && value[0] == '@') value = trim(reader(value.substr(1)));
        const Json parsed = Json::parse(value, nullptr, false);
        return Json{{key, parsed.is_discarded() ? Json(value) : parsed}};
    }

    Json json(const std::string& text) const {
        const Json parsed = Json::parse(text, nullptr, false);
        if (!parsed.is_discarded()) return parsed;
        if (acceptsString(spec.schema)) return text;
        fail("takes JSON (" + describeType(spec.schema) + "), not \"" + text + "\"");
        return nullptr;
    }

    [[noreturn]] void fail(const std::string& message) const {
        throw UsageError(spec.flag + " " + message);
    }

private:
    const ArgSpec& spec;
    const FileReader& reader;
};

void validateInto(const Json& schema, const Json& value, const std::string& path, bool closed, std::vector<std::string>& errors) {
    if (!schema.is_object()) return;
    const std::string where = path.empty() ? std::string("arguments") : path;

    if (schema.contains("anyOf") && schema["anyOf"].is_array()) {
        bool any = false;
        for (const Json& branch : schema["anyOf"]) {
            std::vector<std::string> branchErrors;
            validateInto(branch, value, path, false, branchErrors);
            if (branchErrors.empty()) {
                any = true;
                break;
            }
        }
        if (!any) errors.push_back(where + ": matches none of the allowed forms (" + describeType(schema) + ")");
    }

    const std::vector<std::string> types = typesOf(schema);
    if (!types.empty()) {
        bool ok = false;
        for (const std::string& type : types) ok = ok || matchesType(value, type);
        if (!ok) {
            std::string expected;
            for (const std::string& type : types) expected += (expected.empty() ? "" : " or ") + type;
            errors.push_back(where + ": expected " + expected + ", got " + jsonTypeName(value));
            return;
        }
    }

    if (schema.contains("enum") && schema["enum"].is_array()) {
        if (std::find(schema["enum"].begin(), schema["enum"].end(), value) == schema["enum"].end()) {
            errors.push_back(where + ": must be one of " + joinValues(schema["enum"]));
        }
    }

    if (value.is_number()) {
        const double number = value.get<double>();
        if (schema.contains("minimum") && schema["minimum"].is_number() && number < schema["minimum"].get<double>()) {
            errors.push_back(where + ": " + value.dump() + " is below the minimum " + schema["minimum"].dump());
        }
        if (schema.contains("maximum") && schema["maximum"].is_number() && number > schema["maximum"].get<double>()) {
            errors.push_back(where + ": " + value.dump() + " is above the maximum " + schema["maximum"].dump());
        }
    }

    if (value.is_array()) {
        const size_t count = value.size();
        const bool hasMin = schema.contains("minItems") && schema["minItems"].is_number_integer();
        const bool hasMax = schema.contains("maxItems") && schema["maxItems"].is_number_integer();
        const size_t minItems = hasMin ? schema["minItems"].get<size_t>() : 0;
        const size_t maxItems = hasMax ? schema["maxItems"].get<size_t>() : 0;
        if (hasMin && hasMax && minItems == maxItems && count != minItems) {
            errors.push_back(where + ": expected " + std::to_string(minItems) + " items, got " + std::to_string(count));
        } else {
            if (hasMin && count < minItems) {
                errors.push_back(where + ": expected at least " + std::to_string(minItems) + " items, got " + std::to_string(count));
            }
            if (hasMax && count > maxItems) {
                errors.push_back(where + ": expected at most " + std::to_string(maxItems) + " items, got " + std::to_string(count));
            }
        }
        if (schema.contains("items")) {
            for (size_t i = 0; i < count; i++) {
                validateInto(schema["items"], value[i], where + "[" + std::to_string(i) + "]", false, errors);
            }
        }
    }

    if (value.is_object()) {
        const Json properties = schema.contains("properties") && schema["properties"].is_object() ? schema["properties"] : Json::object();
        if (schema.contains("required") && schema["required"].is_array()) {
            for (const Json& key : schema["required"]) {
                if (key.is_string() && !value.contains(key.get<std::string>())) {
                    errors.push_back((path.empty() ? std::string() : path + ".") + key.get<std::string>() + ": required");
                }
            }
        }
        const bool open = schema.value("additionalProperties", !closed) != false;
        std::vector<std::string> known;
        for (auto it = properties.begin(); it != properties.end(); ++it) known.push_back(it.key());
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string child = (path.empty() ? std::string() : path + ".") + it.key();
            if (properties.contains(it.key())) {
                validateInto(properties[it.key()], it.value(), child, false, errors);
            } else if (!open) {
                std::string message = child + ": unknown key";
                const std::vector<std::string> close = closeMatches(it.key(), known);
                if (!close.empty()) message += " (did you mean " + close[0] + "?)";
                std::string valid;
                for (const std::string& key : known) valid += (valid.empty() ? "" : ", ") + key;
                message += "; keys: " + (valid.empty() ? std::string("none") : valid);
                errors.push_back(message);
            }
        }
    }
}

std::string range(const Json& schema) {
    const bool hasMin = schema.contains("minimum");
    const bool hasMax = schema.contains("maximum");
    if (!hasMin && !hasMax) return {};
    return " " + (hasMin ? schema["minimum"].dump() : std::string()) + ".." + (hasMax ? schema["maximum"].dump() : std::string());
}

} // namespace

std::string snakeName(const std::string& name) {
    std::string out = name;
    std::replace(out.begin(), out.end(), '-', '_');
    return out;
}

ArgKind classify(const Json& schema) {
    if (!schema.is_object()) return ArgKind::Json;
    if (schema.contains("anyOf")) return ArgKind::Json;
    if (schema.contains("enum") && schema["enum"].is_array()) return ArgKind::Enum;
    if (isRefSchema(schema)) return ArgKind::Ref;
    const std::vector<std::string> types = typesOf(schema);
    if (types.size() != 1) return ArgKind::Json;
    const std::string& type = types[0];
    if (type == "string") return ArgKind::String;
    if (type == "integer") return ArgKind::Integer;
    if (type == "number") return ArgKind::Number;
    if (type == "boolean") return ArgKind::Boolean;
    if (type == "array") {
        const Json items = schema.contains("items") ? schema["items"] : Json::object();
        return isScalarKind(classify(items)) ? ArgKind::ScalarArray : ArgKind::Json;
    }
    if (type == "object") {
        const bool declared = schema.contains("properties") && schema["properties"].is_object() && !schema["properties"].empty();
        const bool open = schema.value("additionalProperties", true) != false;
        return open && (!declared || schema.value("additionalProperties", false) == true) ? ArgKind::FreeObject : ArgKind::Json;
    }
    return ArgKind::Json;
}

std::vector<ArgSpec> argSpecs(const Json& inputSchema) {
    std::vector<ArgSpec> out;
    if (!inputSchema.is_object() || !inputSchema.contains("properties") || !inputSchema["properties"].is_object()) return out;
    std::vector<std::string> required;
    if (inputSchema.contains("required") && inputSchema["required"].is_array()) {
        for (const Json& key : inputSchema["required"]) {
            if (key.is_string()) required.push_back(key.get<std::string>());
        }
    }
    const Json& properties = inputSchema["properties"];
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        ArgSpec spec;
        spec.key = it.key();
        spec.flag = "--" + it.key();
        std::replace(spec.flag.begin(), spec.flag.end(), '_', '-');
        spec.schema = it.value();
        spec.kind = classify(it.value());
        spec.required = std::find(required.begin(), required.end(), it.key()) != required.end();
        out.push_back(std::move(spec));
    }
    return out;
}

std::string readArgFile(const std::string& path) {
    if (path == "-") {
        return std::string(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) throw UsageError("can't read @" + path);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

Json parseToolArgs(const Json& inputSchema, std::span<const std::string> tokens, const FileReader& reader) {
    const std::vector<ArgSpec> specs = argSpecs(inputSchema);
    auto find = [&](const std::string& name) -> const ArgSpec* {
        const std::string key = snakeName(name);
        for (const ArgSpec& spec : specs) {
            if (spec.key == key) return &spec;
        }
        return nullptr;
    };

    Json args = Json::object();
    bool positional = false;
    bool flags = false;
    for (size_t i = 0; i < tokens.size(); i++) {
        const std::string& token = tokens[i];
        if (token.rfind("--", 0) != 0) {
            const bool objectLike = !token.empty() && (token[0] == '{' || (token[0] == '@' && token.size() > 1));
            if (!objectLike || positional || i != 0) {
                throw UsageError("unexpected argument \"" + token + "\": tool arguments are --flags or one JSON object");
            }
            const std::string text = token[0] == '@' ? reader(token.substr(1)) : token;
            const Json parsed = Json::parse(text, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                throw UsageError("the tool arguments must be one JSON object" +
                                 std::string(token[0] == '@' ? " (in " + token + ")" : ""));
            }
            args = parsed;
            positional = true;
            continue;
        }
        flags = true;
        std::string name = token.substr(2);
        std::string value;
        const size_t eq = name.find('=');
        const bool hasValue = eq != std::string::npos;
        if (hasValue) {
            value = name.substr(eq + 1);
            name.resize(eq);
        }
        const ArgSpec* spec = find(name);
        if (!spec && !hasValue && (name.rfind("no-", 0) == 0 || name.rfind("no_", 0) == 0)) {
            const ArgSpec* negated = find(name.substr(3));
            if (negated && negated->kind == ArgKind::Boolean) {
                if (args.contains(negated->key)) throw UsageError(negated->flag + " given twice");
                args[negated->key] = false;
                continue;
            }
        }
        if (!spec) {
            std::vector<std::string> names;
            for (const ArgSpec& candidate : specs) names.push_back(candidate.flag.substr(2));
            std::string message = "unknown flag --" + name;
            const std::vector<std::string> close = closeMatches(snakeName(name), [&]() {
                std::vector<std::string> keys;
                for (const ArgSpec& candidate : specs) keys.push_back(candidate.key);
                return keys;
            }());
            if (!close.empty()) {
                std::string flag = close[0];
                std::replace(flag.begin(), flag.end(), '_', '-');
                message += " (did you mean --" + flag + "?)";
            }
            std::string valid;
            for (const std::string& flag : names) valid += (valid.empty() ? "" : " ") + ("--" + flag);
            message += ". Flags: " + (valid.empty() ? std::string("none (the tool takes no arguments)") : valid);
            throw UsageError(message);
        }

        const bool next = i + 1 < tokens.size();
        const bool nextIsFlag = next && tokens[i + 1].rfind("--", 0) == 0;
        Json converted;
        const Converter converter(*spec, reader);
        if (spec->kind == ArgKind::Boolean && !hasValue) {
            // --k alone is true; a word after it is its value (and must be a boolean word)
            converted = next && !nextIsFlag ? converter.convert(tokens[++i]) : Json(true);
        } else if (spec->kind == ArgKind::Json && !hasValue && (!next || nextIsFlag) && acceptsBoolean(spec->schema)) {
            converted = true;
        } else {
            if (!hasValue) {
                if (!next || nextIsFlag) throw UsageError(spec->flag + " needs a value (" + describeType(spec->schema) + ")");
                value = tokens[++i];
            }
            converted = converter.convert(value);
        }

        if (spec->kind == ArgKind::ScalarArray) {
            if (!args.contains(spec->key)) args[spec->key] = Json::array();
            for (const Json& item : converted) args[spec->key].push_back(item);
        } else if (spec->kind == ArgKind::FreeObject) {
            if (!args.contains(spec->key)) args[spec->key] = Json::object();
            for (auto it = converted.begin(); it != converted.end(); ++it) args[spec->key][it.key()] = it.value();
        } else {
            if (args.contains(spec->key)) throw UsageError(spec->flag + " given twice");
            args[spec->key] = converted;
        }
    }
    if (positional && flags) throw UsageError("give the tool arguments as one JSON object or as flags, not both");
    return args;
}

std::vector<std::string> validate(const Json& schema, const Json& value, bool closedTop) {
    std::vector<std::string> errors;
    validateInto(schema, value, "", closedTop, errors);
    return errors;
}

std::string describeType(const Json& schema) {
    if (!schema.is_object()) return "any JSON";
    if (schema.contains("anyOf") && schema["anyOf"].is_array()) {
        std::string out;
        for (const Json& branch : schema["anyOf"]) out += (out.empty() ? "" : " or ") + describeType(branch);
        return out;
    }
    if (schema.contains("enum") && schema["enum"].is_array()) {
        std::string out;
        for (const Json& value : schema["enum"]) out += (out.empty() ? "" : " | ") + (value.is_string() ? value.get<std::string>() : value.dump());
        return "one of: " + out;
    }
    if (isRefSchema(schema)) return "ref (id, name or path)";
    const std::vector<std::string> types = typesOf(schema);
    if (types.empty()) return "any JSON";
    if (types.size() > 1) {
        std::string out;
        for (const std::string& type : types) out += (out.empty() ? "" : " or ") + type;
        return out;
    }
    const std::string& type = types[0];
    if (type == "integer" || type == "number") return type + range(schema);
    if (type == "array") {
        const Json items = schema.contains("items") ? schema["items"] : Json::object();
        std::string count;
        const bool hasMin = schema.contains("minItems");
        const bool hasMax = schema.contains("maxItems");
        if (hasMin && hasMax && schema["minItems"] == schema["maxItems"]) count = "[" + schema["minItems"].dump() + "]";
        else if (hasMin || hasMax) {
            count = "[" + (hasMin ? schema["minItems"].dump() : std::string("0")) + ".." + (hasMax ? schema["maxItems"].dump() : std::string()) + "]";
        } else count = "[]";
        const ArgKind itemKind = classify(items);
        if (isScalarKind(itemKind)) return describeType(items) + count;
        return "array" + count + " of " + describeType(items);
    }
    if (type == "object") {
        if (classify(schema) == ArgKind::FreeObject) return "object (JSON or KEY=VALUE)";
        std::string keys;
        if (schema.contains("properties")) {
            for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) keys += (keys.empty() ? "" : ", ") + it.key();
        }
        return "object {" + keys + "}";
    }
    return type;
}

size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) row[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); j++) {
            const size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

std::vector<std::string> closeMatches(const std::string& word, const std::vector<std::string>& candidates) {
    std::vector<std::pair<size_t, std::string>> scored;
    for (const std::string& candidate : candidates) {
        const size_t distance = editDistance(word, candidate);
        if (distance <= 2) scored.emplace_back(distance, candidate);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::string> out;
    for (const auto& item : scored) out.push_back(item.second);
    return out;
}

}
