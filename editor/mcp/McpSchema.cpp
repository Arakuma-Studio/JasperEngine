// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpSchema.h"

#include <cmath>

namespace jasperengine::editor::mcp::schema {

namespace {
// An empty description is left out (tool definitions are paid for in every agent session)
Json described(Json schema, const std::string& description) {
    if (!description.empty()) schema["description"] = description;
    return schema;
}
}

Json object(Props properties, std::initializer_list<const char*> required, bool additionalProperties) {
    Json props = Json::object();
    for (const auto& item : properties) props[item.first] = item.second;
    std::vector<std::string> req;
    for (const char* key : required) req.push_back(key);
    return object(props, req, additionalProperties);
}

Json object(const Json& properties, const std::vector<std::string>& required, bool additionalProperties) {
    Json out = {
        {"type", "object"},
        {"properties", properties.is_object() ? properties : Json::object()},
        {"additionalProperties", additionalProperties}
    };
    if (!required.empty()) out["required"] = required;
    return out;
}

Json anyObject(const std::string& description) {
    // a free-form object says so (S31 conventions: an object with properties declares
    // additionalProperties; upstream's trim left it to the JSON Schema default)
    return described({{"type", "object"}, {"additionalProperties", true}}, description);
}

Json string(const std::string& description) {
    return described({{"type", "string"}}, description);
}

Json stringEnum(const std::string& description, const std::vector<std::string>& values) {
    return described({{"type", "string"}, {"enum", values}}, description);
}

Json integer(const std::string& description) {
    return described({{"type", "integer"}}, description);
}

Json integer(const std::string& description, long long minimum, long long maximum) {
    return described({{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}}, description);
}

Json number(const std::string& description) {
    return described({{"type", "number"}}, description);
}

namespace {
// whole bounds as integers: 3600, not 3600.0 (the same JSON Schema, shorter in every session)
Json bound(double value) {
    if (std::isfinite(value) && std::floor(value) == value && std::fabs(value) < 1e15) return static_cast<long long>(value);
    return value;
}
}

Json number(const std::string& description, double minimum, double maximum) {
    return described({{"type", "number"}, {"minimum", bound(minimum)}, {"maximum", bound(maximum)}}, description);
}

Json boolean(const std::string& description) {
    return described({{"type", "boolean"}}, description);
}

Json array(const Json& items, const std::string& description) {
    return described({{"type", "array"}, {"items", items}}, description);
}

Json any(const std::string& description) {
    return described(Json::object(), description);
}

Json anyOf(const std::string& description, std::vector<Json> schemas) {
    return described({{"anyOf", std::move(schemas)}}, description);
}

namespace {
Json fixedVector(int size, const std::string& description) {
    return described({
        {"type", "array"},
        {"items", {{"type", "number"}}},
        {"minItems", size},
        {"maxItems", size}
    }, description);
}
}

Json vec2(const std::string& description) { return fixedVector(2, description); }
Json vec3(const std::string& description) { return fixedVector(3, description); }
Json vec4(const std::string& description) { return fixedVector(4, description); }

// The two shapes in words (McpValues validates them): an anyOf of both costs ~200 characters more
Json quat(const std::string& description) {
    return described({
        {"anyOf", Json::array({
            fixedVector(4, ""),
            {{"type", "object"}, {"properties", {{"euler", fixedVector(3, "")}}}, {"required", {"euler"}},
             {"additionalProperties", false}}
        })}
    }, description);
}

Json color(const std::string& description) {
    return described({
        {"type", "array"},
        {"items", {{"type", "number"}}},
        {"minItems", 3},
        {"maxItems", 4}
    }, description);
}

// Refs advertise [integer, string] (the CLI's `call` tells an id from a name by it: --entity 42 is
// the id, --entity '"42"' the name); McpRefs also reads a numeric string like an integer id and
// accepts the {"id"|"name"|"path"} objects results return. The server instructions (Addressing)
// explain the conventions, so the default descriptions are empty.
Json sceneRef(const std::string& description) {
    return described({{"type", {"integer", "string"}}}, description);
}

Json entityRef(const std::string& description) {
    return described({{"type", {"integer", "string"}}}, description);
}

Json entities(const std::string& description) {
    return described({{"type", "array"}, {"items", {{"type", {"integer", "string"}}}}}, description);
}

Json componentName(const std::string& description) {
    return described({{"type", "string"}}, description);
}

Json assetPath(const std::string& description) {
    return described({{"type", "string"}}, description);
}

Json withDefault(Json propertySchema, Json defaultValue) {
    // false is what an omitted boolean means anyway
    if (defaultValue.is_boolean() && !defaultValue.get<bool>() && propertySchema.value("type", "") == "boolean") {
        return propertySchema;
    }
    propertySchema["default"] = std::move(defaultValue);
    return propertySchema;
}

}
