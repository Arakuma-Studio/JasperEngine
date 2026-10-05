// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpSchema.h"

namespace jasperengine::editor::mcp::schema {

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
    return {{"type", "object"}, {"description", description}, {"additionalProperties", true}};
}

Json string(const std::string& description) {
    return {{"type", "string"}, {"description", description}};
}

Json stringEnum(const std::string& description, const std::vector<std::string>& values) {
    return {{"type", "string"}, {"description", description}, {"enum", values}};
}

Json integer(const std::string& description) {
    return {{"type", "integer"}, {"description", description}};
}

Json integer(const std::string& description, long long minimum, long long maximum) {
    return {{"type", "integer"}, {"description", description}, {"minimum", minimum}, {"maximum", maximum}};
}

Json number(const std::string& description) {
    return {{"type", "number"}, {"description", description}};
}

Json number(const std::string& description, double minimum, double maximum) {
    return {{"type", "number"}, {"description", description}, {"minimum", minimum}, {"maximum", maximum}};
}

Json boolean(const std::string& description) {
    return {{"type", "boolean"}, {"description", description}};
}

Json array(const Json& items, const std::string& description) {
    return {{"type", "array"}, {"items", items}, {"description", description}};
}

Json any(const std::string& description) {
    return {{"description", description}};
}

namespace {
Json fixedVector(int size, const std::string& description) {
    return {
        {"type", "array"},
        {"items", {{"type", "number"}}},
        {"minItems", size},
        {"maxItems", size},
        {"description", description}
    };
}
}

Json vec2(const std::string& description) { return fixedVector(2, description); }
Json vec3(const std::string& description) { return fixedVector(3, description); }
Json vec4(const std::string& description) { return fixedVector(4, description); }

Json quat(const std::string& description) {
    return {
        {"description", description + " Quaternion [x,y,z,w], or {\"euler\":[x,y,z]} in degrees."},
        {"anyOf", Json::array({
            fixedVector(4, "Quaternion [x,y,z,w]"),
            {{"type", "object"}, {"properties", {{"euler", fixedVector(3, "Euler angles in degrees")}}}, {"required", {"euler"}}}
        })}
    };
}

Json color(const std::string& description) {
    return {
        {"type", "array"},
        {"items", {{"type", "number"}}},
        {"minItems", 3},
        {"maxItems", 4},
        {"description", description + " Color [r,g,b] or [r,g,b,a], components 0-1."}
    };
}

Json sceneRef(const std::string& description) {
    return {
        {"type", {"integer", "string"}},
        {"description", description.empty()
            ? "Scene: numeric id, scene name, or project-relative .scene path. Omit for the selected scene."
            : description}
    };
}

Json entityRef(const std::string& description) {
    return {
        {"type", {"integer", "string"}},
        {"description", description.empty()
            ? "GameObject: numeric id, or a '/'-joined name path from the scene root (\"Player/Gun\"); "
              "\"Name#N\" picks the Nth earlier sibling with the same name (\"Enemy#2\")."
            : description}
    };
}

Json componentName(const std::string& description) {
    return {
        {"type", "string"},
        {"description", description.empty()
            ? "Component type name, e.g. transform, mesh, sprite, camera, light, rigidbody, rigidbody2d, script, text, ui."
            : description}
    };
}

Json assetPath(const std::string& description) {
    return {{"type", "string"}, {"description", description + " Project-relative path (no '..', not absolute)."}};
}

Json withDefault(Json propertySchema, Json defaultValue) {
    propertySchema["default"] = std::move(defaultValue);
    return propertySchema;
}

}
