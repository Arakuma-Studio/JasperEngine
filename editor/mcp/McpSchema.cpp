// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpSchema.h"

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
    // additionalProperties: true is the JSON Schema default, so it is not spelled out
    return described({{"type", "object"}}, description);
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

Json number(const std::string& description, double minimum, double maximum) {
    return described({{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}}, description);
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

Json quat(const std::string& description) {
    return described({
        {"anyOf", Json::array({
            fixedVector(4, ""),
            {{"type", "object"}, {"properties", {{"euler", fixedVector(3, "")}}}, {"required", {"euler"}}}
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

// Refs carry no type: McpRefs accepts an id, a name, a path and the {"id"|"name"|"path"} objects
// results return, and the server instructions (Addressing) explain them once. A type hint on each of
// the ~150 ref arguments cost about 4,000 characters in every agent session. The default
// descriptions are empty for the same reason.
Json sceneRef(const std::string& description) {
    return described(Json::object(), description);
}

Json entityRef(const std::string& description) {
    return described(Json::object(), description);
}

Json entities(const std::string& description) {
    return described({{"type", "array"}}, description);
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
