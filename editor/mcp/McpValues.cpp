// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpValues.h"

#include "Project.h"
#include "Stream.h"
#include "command/type/PropertyCmd.h"
#include "component/MeshComponent.h"
#include "component/TextComponent.h"
#include "script/ScriptProperty.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace {

bool readComponents(const Json& value, float* out, int count, const char* const* keys) {
    if (value.is_array()) {
        if (static_cast<int>(value.size()) != count) return false;
        for (int i = 0; i < count; i++) {
            if (!value[i].is_number()) return false;
            out[i] = value[i].get<float>();
        }
        return true;
    }
    if (value.is_object()) {
        bool any = false;
        for (int i = 0; i < count; i++) {
            auto it = value.find(keys[i]);
            if (it == value.end()) continue;
            if (!it->is_number()) return false;
            out[i] = it->get<float>();
            any = true;
        }
        return any;
    }
    return false;
}

const char* const XYZW[] = {"x", "y", "z", "w"};

// String properties holding an asset reference instead of plain text
bool isAssetPathProperty(const std::string& propertyName) {
    return propertyName == "filename";
}

bool isSafeRelative(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const fs::path& part : path.lexically_normal()) {
        if (part == "..") return false;
    }
    return true;
}

} // namespace

std::string McpValues::propertyTypeName(PropertyType type) {
    switch (type) {
        case PropertyType::Bool: return "bool";
        case PropertyType::String: return "string";
        case PropertyType::Float: return "float";
        case PropertyType::Double: return "double";
        case PropertyType::Vector2: return "vector2";
        case PropertyType::Vector3: return "vector3";
        case PropertyType::Vector4: return "vector4";
        case PropertyType::Quat: return "quaternion";
        case PropertyType::Int: return "int";
        case PropertyType::UInt: return "uint";
        case PropertyType::Material: return "material";
        case PropertyType::Texture: return "texture";
        case PropertyType::Font: return "font";
        case PropertyType::Enum: return "enum";
        case PropertyType::Ease: return "ease";
        case PropertyType::Custom: return "custom";
        case PropertyType::Entity: return "entity";
        case PropertyType::EntityReference: return "entity_reference";
    }
    return "unknown";
}

bool McpValues::parseVector2(const Json& value, Vector2& out) {
    float v[2] = {out.x, out.y};
    if (!readComponents(value, v, 2, XYZW)) return false;
    out = Vector2(v[0], v[1]);
    return true;
}

bool McpValues::parseVector3(const Json& value, Vector3& out) {
    float v[3] = {out.x, out.y, out.z};
    if (!readComponents(value, v, 3, XYZW)) return false;
    out = Vector3(v[0], v[1], v[2]);
    return true;
}

bool McpValues::parseVector4(const Json& value, Vector4& out) {
    float v[4] = {out.x, out.y, out.z, out.w};
    // A color without alpha keeps the current alpha
    if (value.is_array() && value.size() == 3) {
        for (int i = 0; i < 3; i++) {
            if (!value[i].is_number()) return false;
            v[i] = value[i].get<float>();
        }
        out = Vector4(v[0], v[1], v[2], v[3]);
        return true;
    }
    if (!readComponents(value, v, 4, XYZW)) return false;
    out = Vector4(v[0], v[1], v[2], v[3]);
    return true;
}

bool McpValues::parseQuaternion(const Json& value, Quaternion& out) {
    if (value.is_object() && value.contains("euler")) {
        float e[3] = {0.0f, 0.0f, 0.0f};
        if (!readComponents(value["euler"], e, 3, XYZW)) return false;
        out = Quaternion(e[0], e[1], e[2]);
        return true;
    }
    float v[4] = {out.x, out.y, out.z, out.w};
    if (!readComponents(value, v, 4, XYZW)) return false;
    out = Quaternion(v[3], v[0], v[1], v[2]);
    return true;
}

Json McpValues::toJson(const Vector2& value) { return Json::array({value.x, value.y}); }
Json McpValues::toJson(const Vector3& value) { return Json::array({value.x, value.y, value.z}); }
Json McpValues::toJson(const Vector4& value) { return Json::array({value.x, value.y, value.z, value.w}); }
Json McpValues::toJson(const Quaternion& value) { return Json::array({value.x, value.y, value.z, value.w}); }

std::string McpValues::assetPathToJson(Project* project, const std::string& stored) {
    if (stored.empty() || !project) return stored;
    return project->normalizeToProjectRelative(project->resolveAssetPath(stored)).generic_string();
}

bool McpValues::assetPathFromJson(Project* project, const std::string& projectRelative, std::string& stored, std::string& error) {
    if (projectRelative.empty()) {
        stored.clear();
        return true;
    }
    if (!isSafeRelative(projectRelative)) {
        error = "\"" + projectRelative + "\" must be a project-relative path (no '..', not absolute).";
        return false;
    }
    const fs::path full = project->getProjectPath() / projectRelative;
    if (!project->isInsideAssetsPath(full)) {
        error = "\"" + projectRelative + "\" must be inside the assets directory (\"" +
                project->getAssetsDir().generic_string() + "\").";
        return false;
    }
    stored = project->normalizeToAssetsRelative(full).generic_string();
    return true;
}

Json McpValues::toJson(Project* project, const std::string& propertyName, const PropertyData& property) {
    if (!property.ref) return Json();
    switch (property.type) {
        case PropertyType::Bool:
            return *static_cast<bool*>(property.ref);
        case PropertyType::String: {
            const std::string& value = *static_cast<std::string*>(property.ref);
            return isAssetPathProperty(propertyName) ? assetPathToJson(project, value) : value;
        }
        case PropertyType::Float:
            return *static_cast<float*>(property.ref);
        case PropertyType::Double:
            return *static_cast<double*>(property.ref);
        case PropertyType::Vector2:
            return toJson(*static_cast<Vector2*>(property.ref));
        case PropertyType::Vector3:
            return toJson(*static_cast<Vector3*>(property.ref));
        case PropertyType::Vector4:
            return toJson(*static_cast<Vector4*>(property.ref));
        case PropertyType::Quat:
            return toJson(*static_cast<Quaternion*>(property.ref));
        case PropertyType::Int:
        case PropertyType::Enum:
            return *static_cast<int*>(property.ref);
        case PropertyType::Ease:
            return Stream::easeTypeToString(static_cast<EaseType>(*static_cast<int*>(property.ref)));
        case PropertyType::UInt:
            return *static_cast<unsigned int*>(property.ref);
        case PropertyType::Font: {
            // the chain in use: the main font first, then its fallbacks
            const FontArray& fonts = *static_cast<FontArray*>(property.ref);
            size_t numFonts = 0;
            for (size_t i = 0; i < fonts.size(); i++) {
                if (!fonts[i].empty()) numFonts = i + 1;
            }
            Json list = Json::array();
            for (size_t i = 0; i < numFonts; i++) list.push_back(assetPathToJson(project, fonts[i]));
            return list;
        }
        case PropertyType::Texture: {
            // Scaled SVG sources round-trip as "<path>?svgScale=N"
            const Texture* tex = static_cast<Texture*>(property.ref);
            return TextureData::buildSvgScalePath(assetPathToJson(project, tex->getPath()), tex->getSvgScale());
        }
        case PropertyType::Entity:
            return *static_cast<Entity*>(property.ref);
        case PropertyType::EntityReference: {
            const EntityReference& ref = *static_cast<EntityReference*>(property.ref);
            return Json{{"entity", ref.entity}, {"scene_id", ref.sceneId}};
        }
        case PropertyType::Material: {
            const Material& material = *static_cast<Material*>(property.ref);
            return Json{{"name", material.name},
                        {"baseColorFactor", toJson(material.baseColorFactor)},
                        {"metallicFactor", material.metallicFactor},
                        {"roughnessFactor", material.roughnessFactor},
                        {"emissiveFactor", toJson(material.emissiveFactor)},
                        {"baseColorTexture", TextureData::buildSvgScalePath(
                            assetPathToJson(project, material.baseColorTexture.getPath()),
                            material.baseColorTexture.getSvgScale())}};
        }
        case PropertyType::Custom:
            if (propertyName == "shaderUniforms") {
                Json json = Json::object();
                for (const auto& uniform : *static_cast<ShaderUniformValues*>(property.ref)) {
                    json[uniform.first] = toJson(uniform.second);
                }
                return json;
            }
            return Json{{"unsupported", "custom"}};
    }
    return Json();
}

Command* McpValues::buildPropertyCommand(Project* project, uint32_t sceneId, Entity entity, ComponentType component,
                                         const std::string& propertyName, const PropertyData& property,
                                         const Json& value, std::string& error, std::function<void()> onChanged) {
    const std::string typeName = propertyTypeName(property.type);
    auto expect = [&](const std::string& what) {
        error = "Property " + propertyName + " is " + typeName + ": value must be " + what + ".";
        return nullptr;
    };

    switch (property.type) {
        case PropertyType::Bool:
            if (!value.is_boolean()) return expect("true or false");
            return new PropertyCmd<bool>(project, sceneId, entity, component, propertyName, value.get<bool>(), onChanged);
        case PropertyType::String: {
            if (!value.is_string()) return expect("a string");
            std::string text = value.get<std::string>();
            if (isAssetPathProperty(propertyName) && !assetPathFromJson(project, text, text, error)) return nullptr;
            return new PropertyCmd<std::string>(project, sceneId, entity, component, propertyName, text, onChanged);
        }
        case PropertyType::Float:
            if (!value.is_number()) return expect("a number");
            return new PropertyCmd<float>(project, sceneId, entity, component, propertyName, value.get<float>(), onChanged);
        case PropertyType::Double:
            if (!value.is_number()) return expect("a number");
            return new PropertyCmd<double>(project, sceneId, entity, component, propertyName, value.get<double>(), onChanged);
        case PropertyType::Vector2: {
            Vector2 v = property.ref ? *static_cast<Vector2*>(property.ref) : Vector2::ZERO;
            if (!parseVector2(value, v)) return expect("[x,y]");
            return new PropertyCmd<Vector2>(project, sceneId, entity, component, propertyName, v, onChanged);
        }
        case PropertyType::Vector3: {
            Vector3 v = property.ref ? *static_cast<Vector3*>(property.ref) : Vector3::ZERO;
            if (!parseVector3(value, v)) return expect("[x,y,z]");
            return new PropertyCmd<Vector3>(project, sceneId, entity, component, propertyName, v, onChanged);
        }
        case PropertyType::Vector4: {
            Vector4 v = property.ref ? *static_cast<Vector4*>(property.ref) : Vector4::ZERO;
            if (!parseVector4(value, v)) return expect("[x,y,z,w] (or [r,g,b] keeping alpha)");
            return new PropertyCmd<Vector4>(project, sceneId, entity, component, propertyName, v, onChanged);
        }
        case PropertyType::Quat: {
            Quaternion q = property.ref ? *static_cast<Quaternion*>(property.ref) : Quaternion::IDENTITY;
            if (!parseQuaternion(value, q)) return expect("[x,y,z,w] or {\"euler\":[x,y,z]} in degrees");
            return new PropertyCmd<Quaternion>(project, sceneId, entity, component, propertyName, q, onChanged);
        }
        case PropertyType::Int:
        case PropertyType::Enum:
            if (!value.is_number_integer()) return expect("an integer");
            return new PropertyCmd<int>(project, sceneId, entity, component, propertyName, value.get<int>(), onChanged);
        case PropertyType::Ease: {
            int ease = 0;
            if (value.is_number_integer()) {
                ease = value.get<int>();
            } else if (value.is_string()) {
                const std::string name = value.get<std::string>();
                const EaseType parsed = Stream::stringToEaseType(name);
                // the parser falls back to linear: only accept names that round-trip
                if (Stream::easeTypeToString(parsed) != name) return expect("an ease name such as linear, ease_in_quad");
                ease = static_cast<int>(parsed);
            } else {
                return expect("an ease name or its integer value");
            }
            return new PropertyCmd<int>(project, sceneId, entity, component, propertyName, ease, onChanged);
        }
        case PropertyType::UInt:
            if (!value.is_number_integer() || value.get<long long>() < 0) return expect("a non-negative integer");
            return new PropertyCmd<unsigned int>(project, sceneId, entity, component, propertyName,
                                                 value.get<unsigned int>(), onChanged);
        case PropertyType::Font: {
            // An array sets the whole chain (main font, then fallbacks), a string the main font
            FontArray fonts;
            if (property.ref) fonts = *static_cast<FontArray*>(property.ref);
            std::vector<std::string> requested;
            const bool wholeChain = value.is_array();
            if (wholeChain) {
                if (value.size() > fonts.size()) return expect("up to " + std::to_string(fonts.size()) + " font paths");
                for (const Json& item : value) {
                    if (!item.is_string()) return expect("font path strings");
                    requested.push_back(item.get<std::string>());
                }
            } else if (value.is_string()) {
                requested.push_back(value.get<std::string>());
            } else {
                return expect("a font path or an array of font paths");
            }
            for (size_t i = 0; i < fonts.size(); i++) {
                if (i >= requested.size()) {
                    if (wholeChain) fonts[i].clear();
                    continue;
                }
                std::string stored;
                if (!assetPathFromJson(project, requested[i], stored, error)) return nullptr;
                fonts[i] = stored;
            }
            return new PropertyCmd<FontArray>(project, sceneId, entity, component, propertyName, fonts, onChanged);
        }
        case PropertyType::Texture: {
            if (!value.is_string()) return expect("a project-relative texture path");
            std::string stored;
            if (!assetPathFromJson(project, value.get<std::string>(), stored, error)) return nullptr;
            return new PropertyCmd<Texture>(project, sceneId, entity, component, propertyName, Texture(stored), onChanged);
        }
        case PropertyType::Entity:
            if (!value.is_number_integer() || value.get<long long>() < 0) return expect("a GameObject id");
            return new PropertyCmd<Entity>(project, sceneId, entity, component, propertyName, value.get<Entity>(), onChanged);
        case PropertyType::EntityReference: {
            EntityReference ref;
            ref.sceneId = sceneId;
            if (value.is_number_integer() && value.get<long long>() >= 0) {
                ref.entity = value.get<Entity>();
            } else if (value.is_object() && value.contains("entity") && value["entity"].is_number_integer()) {
                ref.entity = value["entity"].get<Entity>();
                if (value.contains("scene_id") && value["scene_id"].is_number_integer()) {
                    ref.sceneId = value["scene_id"].get<uint32_t>();
                }
            } else {
                return expect("a GameObject id or {\"entity\": id, \"scene_id\": id}");
            }
            return new PropertyCmd<EntityReference>(project, sceneId, entity, component, propertyName, ref, onChanged);
        }
        case PropertyType::Material:
        case PropertyType::Custom:
            error = "Property " + propertyName + " is " + typeName + ", which this tool can't set yet.";
            return nullptr;
    }
    error = "Unsupported property type.";
    return nullptr;
}
