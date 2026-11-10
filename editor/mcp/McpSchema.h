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
    // One of several shapes (the keyword subset's anyOf)
    Json anyOf(const std::string& description, std::vector<Json> schemas);

    // [x,y] / [x,y,z] / [x,y,z,w] (an {x,y,z,w} object is accepted too, see McpValues)
    Json vec2(const std::string& description);
    Json vec3(const std::string& description);
    Json vec4(const std::string& description);
    // [x,y,z,w], or {"euler":[x,y,z]} in degrees (the instructions' Addressing explains it)
    Json quat(const std::string& description = {});
    // [r,g,b] or [r,g,b,a], 0-1
    Json color(const std::string& description = {});

    // The reference conventions (scene id/name/path, entity id/path/"Name#N", {"id"|"name"|"path"}
    // objects) are explained once in the server instructions (Addressing), so these default
    // descriptions stay short: tool definitions are paid for in every agent session.
    // Empty description = none (the instructions' Addressing section covers it).
    // Scene reference: id, name, .scene path or {"id"|"name"|"path"}; omitted = the selected scene.
    Json sceneRef(const std::string& description = {});
    // GameObject reference: id, path ("Player/Gun", "Enemy#2"), unique bare name or {"id"|"name"|"path"}
    Json entityRef(const std::string& description = {});
    // An array of GameObject references
    Json entities(const std::string& description = {});
    // Component type name as Catalog::getComponentType accepts (transform, mesh, rigidbody2d...);
    // empty description = none (the instructions' Addressing explains component names)
    Json componentName(const std::string& description = {});
    // Project-relative asset path (the description should say which kind of asset)
    Json assetPath(const std::string& description);

    // Adds "default" to a property schema (documentation; handlers apply the default)
    Json withDefault(Json propertySchema, Json defaultValue);

}
