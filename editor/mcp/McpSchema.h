// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "McpJson.h"

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

// JSON Schema builders for tool inputSchema / outputSchema. Every builder takes the
// description the agent reads; put real constraints in the schema (enums, ranges, required).
namespace jasperengine::editor::mcp::schema {

    using Props = std::initializer_list<std::pair<const char*, Json>>;

    // {type:object, properties, required, additionalProperties}
    Json object(Props properties, std::initializer_list<const char*> required = {}, bool additionalProperties = false);
    Json object(const Json& properties, const std::vector<std::string>& required, bool additionalProperties = false);
    // An object with any keys (free-form)
    Json anyObject(const std::string& description);

    Json string(const std::string& description);
    Json stringEnum(const std::string& description, const std::vector<std::string>& values);
    Json integer(const std::string& description);
    Json integer(const std::string& description, long long minimum, long long maximum);
    Json number(const std::string& description);
    Json number(const std::string& description, double minimum, double maximum);
    Json boolean(const std::string& description);
    Json array(const Json& items, const std::string& description);
    // Any JSON value (property values of any type)
    Json any(const std::string& description);

    // [x,y] / [x,y,z] / [x,y,z,w] (an {x,y,z,w} object is accepted too, see McpValues)
    Json vec2(const std::string& description);
    Json vec3(const std::string& description);
    Json vec4(const std::string& description);
    // [x,y,z,w], or {"euler":[x,y,z]} in degrees
    Json quat(const std::string& description);
    // [r,g,b] or [r,g,b,a], 0-1
    Json color(const std::string& description);

    // Scene reference: numeric id, scene name or project-relative .scene path. Omit for the selected scene.
    Json sceneRef(const std::string& description = {});
    // GameObject reference: numeric id or a '/'-joined name path from the scene root ("Player/Gun", "Enemy#2")
    Json entityRef(const std::string& description = {});
    // Component type name as Catalog::getComponentType accepts (transform, mesh, rigidbody2d...)
    Json componentName(const std::string& description = {});
    // Project-relative asset path
    Json assetPath(const std::string& description);

    // Adds "default" to a property schema (documentation; handlers apply the default)
    Json withDefault(Json propertySchema, Json defaultValue);

}
