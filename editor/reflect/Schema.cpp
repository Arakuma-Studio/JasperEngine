// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// JSON Schemas of the components (docs/specs/S08-reflection.md R24, R25, R27): what
// list_component_types gives as `schema`, the metadata get_entity detail "full" adds per
// property, and the jasper://reflection document. A schema describes the values get_entity
// reads (McpValues): those are what set_properties takes back.

#include "reflect/Reflect.h"
#include "reflect/TypeOps.h"

#include "StreamMigrations.h"
#include "mcp/McpValues.h"
#include "stream/CurveCodec.h"

#include <cmath>
#include <cstring>
#include <string_view>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::reflect;

namespace {

    Json numberArray(size_t count) {
        return Json{{"type", "array"}, {"items", Json{{"type", "number"}}}, {"minItems", count}, {"maxItems", count}};
    }

    void addRange(Json& schema, const FieldDesc& field) {
        if (std::isfinite(field.min)) schema["minimum"] = mcp::McpValues::number(field.min);
        if (std::isfinite(field.max)) schema["maximum"] = mcp::McpValues::number(field.max);
    }

    // An unsigned value of its own width (ops::UIntAs: a collider's uint16_t, a body's size_t)
    uint64_t readUnsigned(const TypeOps& ops, const void* value) {
        switch (ops.size) {
            case 1: { uint8_t v; std::memcpy(&v, value, 1); return v; }
            case 2: { uint16_t v; std::memcpy(&v, value, 2); return v; }
            case 8: { uint64_t v; std::memcpy(&v, value, 8); return v; }
            default: { uint32_t v; std::memcpy(&v, value, 4); return v; }
        }
    }

    // The JSON of a default value, for the kinds whose JSON does not depend on the scene
    Json defaultJson(const FieldDesc& field, const void* value) {
        if (!value) return nullptr;
        if (field.ops->kind == Kind::Bool) return *static_cast<const bool*>(value);
        if (field.ops->kind == Kind::Int) return *static_cast<const int*>(value);
        if (field.ops->kind == Kind::UInt || field.ops->kind == Kind::LayerMask) return readUnsigned(*field.ops, value);
        if (field.ops->kind == Kind::Float) return mcp::McpValues::number(*static_cast<const float*>(value));
        if (field.ops->kind == Kind::Double) return *static_cast<const double*>(value);
        if (field.ops->kind == Kind::Vector2) return mcp::McpValues::toJson(*static_cast<const Vector2*>(value));
        if (field.ops->kind == Kind::Vector3) return mcp::McpValues::toJson(*static_cast<const Vector3*>(value));
        if (field.ops->kind == Kind::Vector4) return mcp::McpValues::toJson(*static_cast<const Vector4*>(value));
        if (field.ops->kind == Kind::Quat) return mcp::McpValues::toJson(*static_cast<const Quaternion*>(value));
        if (field.ops->propertyType == PropertyType::String) return *static_cast<const std::string*>(value);
        if (field.ops->kind == Kind::Enum && field.enumDesc) {
            const EnumItem* item = enumItem(*field.enumDesc, readEnum(field, value));
            return item ? Json(enumMcpToken(*item)) : Json(nullptr);
        }
        return nullptr;
    }

    std::string description(const FieldDesc& field) {
        std::string text = label(field);
        const std::string tip = tooltip(field);
        if (!tip.empty()) text += ": " + tip;
        return text;
    }

}

// ---------------------------------------------------------------- per type

Json reflect::detail::boolSchema(const FieldDesc&) { return Json{{"type", "boolean"}}; }

Json reflect::detail::intSchema(const FieldDesc& field) {
    Json schema{{"type", "integer"}};
    addRange(schema, field);
    return schema;
}

Json reflect::detail::colorWriteSchema(const FieldDesc& field) {
    Json schema = intSchema(field);
    schema["type"] = Json::array({"integer", "string"});
    return schema;
}

Json reflect::detail::uintSchema(const FieldDesc& field) {
    Json schema{{"type", "integer"}, {"minimum", 0}};
    if (field.ops->kind == Kind::LayerMask) schema["maximum"] = 4294967295u;
    addRange(schema, field);
    return schema;
}

Json reflect::detail::numberSchema(const FieldDesc& field) {
    Json schema{{"type", "number"}};
    addRange(schema, field);
    return schema;
}

Json reflect::detail::numberOrNullSchema(const FieldDesc& field) {
    Json schema = numberSchema(field);
    schema["type"] = Json::array({"number", "null"});
    return schema;
}

Json reflect::detail::vector2Schema(const FieldDesc&) { return numberArray(2); }
Json reflect::detail::vector3Schema(const FieldDesc&) { return numberArray(3); }
Json reflect::detail::vector4Schema(const FieldDesc&) { return numberArray(4); }

Json reflect::detail::quatSchema(const FieldDesc&) {
    return Json{{"type", "object"}, {"properties", Json{{"quat", numberArray(4)}, {"euler", numberArray(3)}}}};
}

Json reflect::detail::matrix4Schema(const FieldDesc&) {
    return Json{{"type", "array"}, {"items", numberArray(4)}, {"minItems", 4}, {"maxItems", 4}};
}

Json reflect::detail::rectSchema(const FieldDesc&) { return numberArray(4); }

Json reflect::detail::stringSchema(const FieldDesc&) { return Json{{"type", "string"}}; }

Json reflect::detail::enumSchema(const FieldDesc& field) {
    Json tokens = Json::array();
    if (field.enumDesc) {
        for (const EnumItem& item : field.enumDesc->items) {
            if (enumOffered(field, item)) tokens.push_back(enumMcpToken(item));
        }
    }
    return Json{{"type", "string"}, {"enum", tokens}};
}

Json reflect::detail::easeSchema(const FieldDesc&) {
    Json tokens = Json::array();
    for (const auto& option : mcp::McpValues::easeOptions()) tokens.push_back(option.name);
    return Json{{"type", "string"}, {"enum", tokens}};
}

Json reflect::detail::curveSchema(const FieldDesc&) {
    return curvecodec::curveSchema();
}

Json reflect::detail::gradientSchema(const FieldDesc&) {
    return curvecodec::gradientSchema();
}

Json reflect::detail::entitySchema(const FieldDesc&) {
    return Json{{"type", {"object", "null"}}, {"properties", Json{{"id", Json{{"type", "integer"}}}, {"name", Json{{"type", "string"}}},
                {"path", Json{{"type", "string"}}}}}};
}

Json reflect::detail::entityReferenceSchema(const FieldDesc&) {
    return Json{{"type", {"object", "null"}}, {"properties", Json{{"id", Json{{"type", "integer"}}}, {"name", Json{{"type", "string"}}},
                {"path", Json{{"type", "string"}}}, {"scene", Json{{"type", "integer"}}}}}};
}

Json reflect::detail::textureSchema(const FieldDesc&) {
    return Json{{"type", {"string", "object", "null"}}};
}

Json reflect::detail::fontSchema(const FieldDesc&) {
    return Json{{"type", "array"}, {"items", Json{{"type", "string"}}}};
}

Json reflect::detail::materialSchema(const FieldDesc&) {
    return Json{{"type", "object"}};
}

Json reflect::detail::structSchema(const FieldDesc& field) {
    Json properties = Json::object();
    if (field.structDesc) {
        for (const FieldDesc& member : field.structDesc->fields) {
            if (!isProperty(member) || (member.flags & Proxy)) continue;
            properties[member.name] = fieldSchema(*field.structDesc, member);
        }
    }
    return Json{{"type", "object"}, {"properties", properties}};
}

Json reflect::detail::arraySchema(const FieldDesc& field) {
    Json items = Json::object();
    if (field.structDesc) {
        Json properties = Json::object();
        for (const FieldDesc& member : field.structDesc->fields) {
            if (!isProperty(member) || (member.flags & Proxy)) continue;
            properties[member.name] = fieldSchema(*field.structDesc, member);
        }
        items = Json{{"type", "object"}, {"properties", properties}};
    } else if (field.elementOps && field.elementOps->kind == Kind::Array) {
        // arrays of arrays (a morph track's weights per key): the field describes one level only
        items = Json{{"type", "array"}};
    } else if (field.elementOps && field.elementOps->schema) {
        items = field.elementOps->schema(field);
    }
    // (an empty list reads as null)
    return Json{{"type", Json::array({"array", "null"})}, {"items", items}};
}

Json reflect::detail::customSchema(const FieldDesc&) {
    return Json::object();
}

// ---------------------------------------------------------------- per field and component

Json reflect::fieldSchema(const StructDesc& owner, const FieldDesc& field) {
    Json schema = field.ops->schema ? field.ops->schema(field) : Json::object();
    // a Sorting Layer reads as {id, name} and takes either, or the bare id or name (McpValues)
    if (field.drawer && std::string_view(field.drawer) == "sortingLayer") {
        schema = Json{{"type", Json::array({"integer", "string", "object"})}, {"minimum", 0},
                      {"properties", Json{{"id", Json{{"type", "integer"}, {"minimum", 0}}}, {"name", Json{{"type", "string"}}}}}};
    }
    schema["description"] = description(field);
    if (!(field.flags & NoDefault)) {
        const Json def = defaultJson(field, defaultValue(owner, field));
        if (!def.is_null()) schema["default"] = def;
    }
    if (field.flags & (ReadOnly | NoMcpWrite | Derived)) schema["readOnly"] = true;
    if (field.asset != AssetKind::None) schema["x-asset-kind"] = assetKindName(field.asset);
    else if (field.ops->propertyType == PropertyType::Texture) schema["x-asset-kind"] = "texture";
    else if (field.ops->propertyType == PropertyType::Shader) schema["x-asset-kind"] = "shader";
    else if (field.ops->propertyType == PropertyType::Font) schema["x-asset-kind"] = "font";
    if (!field.aliases.empty()) {
        Json aliases = Json::array();
        for (const char* alias : field.aliases) aliases.push_back(alias);
        schema["x-aliases"] = aliases;
    }
    if (field.unit != Unit::None) schema["x-unit"] = unitSymbol(field.unit);
    return schema;
}

Json reflect::schema(const ComponentDesc& desc) {
    Json properties = Json::object();
    for (const FieldDesc& field : desc.body.fields) {
        if (!isProperty(field)) continue;
        properties[field.name] = fieldSchema(desc.body, field);
    }
    Json schema = {
        {"$schema", "https://json-schema.org/draft/2020-12/schema"},
        {"title", desc.displayName},
        {"type", "object"},
        {"properties", properties}
    };
    if (!desc.keyAliases.empty()) {
        Json aliases = Json::array();
        for (const char* alias : desc.keyAliases) aliases.push_back(alias);
        schema["x-aliases"] = aliases;
    }
    return schema;
}

Json reflect::fieldMetadata(const FieldDesc& field) {
    Json meta = Json::object();
    meta["label"] = label(field);
    const char* unit = unitSymbol(field.unit);
    if (*unit) meta["unit"] = unit;
    if (hasRange(field)) {
        meta["range"] = Json::array({std::isfinite(field.min) ? Json(mcp::McpValues::number(field.min)) : Json(nullptr),
                                     std::isfinite(field.max) ? Json(mcp::McpValues::number(field.max)) : Json(nullptr)});
    }
    if (hasUiRange(field)) {
        meta["ui_range"] = Json::array({mcp::McpValues::number(field.uiMin), mcp::McpValues::number(field.uiMax)});
    }
    const std::string tip = tooltip(field);
    if (!tip.empty()) meta["tooltip"] = tip;
    meta["read_only"] = (field.flags & (ReadOnly | NoMcpWrite | Derived)) != 0;
    if (field.asset != AssetKind::None) meta["asset_kind"] = assetKindName(field.asset);
    else if (field.ops->propertyType == PropertyType::Texture) meta["asset_kind"] = "texture";
    else if (field.ops->propertyType == PropertyType::Shader) meta["asset_kind"] = "shader";
    else if (field.ops->propertyType == PropertyType::Font) meta["asset_kind"] = "font";
    if (field.group) meta["group"] = field.group;
    return meta;
}

Json reflect::reflectionDocument() {
    Json components = Json::array();
    for (const ComponentDesc* desc : all()) {
        Json fields = Json::array();
        for (const FieldDesc& field : desc->body.fields) {
            Json entry = {
                {"name", field.name},
                {"yaml_key", field.yamlKey},
                {"kind", kindName(field.kind)},
                {"label", label(field)},
                {"saved", isSaved(field)},
                {"property", isProperty(field)}
            };
            if (field.enumDesc) entry["enum"] = field.enumDesc->cppType;
            if (field.flags & Proxy) entry["proxy"] = true;
            if (!field.aliases.empty()) {
                Json aliases = Json::array();
                for (const char* alias : field.aliases) aliases.push_back(alias);
                entry["aliases"] = aliases;
            }
            fields.push_back(entry);
        }
        Json item = {
            {"key", desc->key},
            {"cpp_name", desc->cppName},
            {"display_name", desc->displayName},
            {"has_enabled", desc->hasEnabled},
            {"codec", desc->codec ? "custom" : "table"},
            {"fields", fields},
            {"schema", schema(*desc)}
        };
        if (!desc->migrations.empty()) {
            Json migrations = Json::array();
            for (const Migration& migration : desc->migrations) migrations.push_back(migration.toFormatVersion);
            item["migrations"] = migrations;
        }
        components.push_back(item);
    }
    // the scene's post-processing block (R15)
    const StructDesc& postProcessing = postProcessingStruct();
    Json postProcessingProperties = Json::object();
    for (const FieldDesc& field : postProcessing.fields) postProcessingProperties[field.name] = fieldSchema(postProcessing, field);
    // Project Settings > Debug: project.yaml's debug block
    const StructDesc& debugSettings = debugSettingsStruct();
    Json debugProperties = Json::object();
    for (const FieldDesc& field : debugSettings.fields) debugProperties[field.name] = fieldSchema(debugSettings, field);

    std::vector<const EnumDesc*> enumList(enumTables().begin(), enumTables().end());
    for (const EnumDesc* desc : postProcessingEnums()) enumList.push_back(desc);
    for (const EnumDesc* desc : debugSettingsEnums()) enumList.push_back(desc);
    Json enumsJson = Json::array();
    for (const EnumDesc* desc : enumList) {
        Json items = Json::array();
        for (const EnumItem& item : desc->items) {
            Json entry = {{"value", item.value}, {"token", item.token}, {"mcp_token", enumMcpToken(item)}, {"label", item.label},
                          {"cpp_name", item.cppName}};
            if (item.hidden) entry["hidden"] = true;
            if (!item.aliases.empty()) {
                Json aliases = Json::array();
                for (const char* alias : item.aliases) aliases.push_back(alias);
                entry["aliases"] = aliases;
            }
            items.push_back(entry);
        }
        enumsJson.push_back({{"name", desc->cppType}, {"fallback", desc->fallback}, {"items", items}});
    }
    return Json{
        {"format_version", StreamMigrations::currentVersion(DocumentKind::Scene)},
        {"prefab_format_version", StreamMigrations::currentVersion(DocumentKind::Prefab)},
        {"components", components},
        {"post_processing", Json{{"type", "object"}, {"properties", postProcessingProperties}}},
        {"project_debug", Json{{"type", "object"}, {"properties", debugProperties}}},
        {"enums", enumsJson}
    };
}
