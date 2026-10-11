// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// McpValues for the shader library: shader references, manifests (list_shaders), and the
// JSON of shaderParams / shaderTextures / shaderFeatures and the render state of a material
// or a UI/Points/LineRenderer/Skybox component.

#include "McpValues.h"

#include "Project.h"
#include "shader/ShaderVariantCompiler.h"
#include "util/CameraTextureLink.h"
#include "util/Color.h"
#include "util/Util.h"

#include <algorithm>

#include <cctype>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace fs = std::filesystem;

namespace {

const char* propertyKindName(ShaderPropertyKind kind) {
    switch (kind) {
        case ShaderPropertyKind::FLOAT: return "float";
        case ShaderPropertyKind::RANGE: return "range";
        case ShaderPropertyKind::INT: return "int";
        case ShaderPropertyKind::TOGGLE: return "toggle";
        case ShaderPropertyKind::COLOR: return "color";
        case ShaderPropertyKind::HDRCOLOR: return "hdrcolor";
        case ShaderPropertyKind::VECTOR2: return "vector2";
        case ShaderPropertyKind::VECTOR3: return "vector3";
        case ShaderPropertyKind::VECTOR4: return "vector4";
    }
    return "float";
}

// components of a property's value that mean something
int propertyComponents(ShaderPropertyKind kind) {
    switch (kind) {
        case ShaderPropertyKind::VECTOR2: return 2;
        case ShaderPropertyKind::VECTOR3: return 3;
        case ShaderPropertyKind::VECTOR4:
        case ShaderPropertyKind::COLOR:
        case ShaderPropertyKind::HDRCOLOR: return 4;
        default: return 1;
    }
}

bool isColorKind(ShaderPropertyKind kind) {
    return kind == ShaderPropertyKind::COLOR || kind == ShaderPropertyKind::HDRCOLOR;
}

const char* textureDefaultName(ShaderTextureDefault value) {
    switch (value) {
        case ShaderTextureDefault::WHITE: return "white";
        case ShaderTextureDefault::BLACK: return "black";
        case ShaderTextureDefault::GRAY: return "gray";
        case ShaderTextureDefault::NORMAL: return "normal";
    }
    return "white";
}

Json propertyValueJson(const ShaderManifestProperty& property, const Vector4& value) {
    const int count = propertyComponents(property.kind);
    if (count == 1) return McpValues::number(value.x);
    Json out = Json::array();
    const float v[4] = {value.x, value.y, value.z, value.w};
    for (int i = 0; i < count; i++) out.push_back(McpValues::number(v[i]));
    return out;
}

std::string memberNames(const ShaderManifest& manifest) {
    std::string text;
    for (const ShaderManifestProperty& property : manifest.properties) text += (text.empty() ? "" : ", ") + property.member;
    return text.empty() ? "(none)" : text;
}

std::string textureNames(const ShaderManifest& manifest) {
    std::string text;
    for (const ShaderManifestTexture& texture : manifest.textures) text += (text.empty() ? "" : ", ") + texture.uniform;
    return text.empty() ? "(none)" : text;
}

std::string defineNames(const ShaderManifest& manifest) {
    std::string text;
    for (const ShaderManifestFeature& feature : manifest.features) text += (text.empty() ? "" : ", ") + feature.define;
    for (const ShaderManifestKeywords& group : manifest.keywords) {
        for (const std::string& value : group.values) text += (text.empty() ? "" : ", ") + group.group + "_" + value;
    }
    return text.empty() ? "(none)" : text;
}

// a "library:" reference whose shader the build does not have: its values cannot be checked
std::string unknownLibraryShaderText() {
    return "the shader is not a library shader the engine has (set an existing one first); the library shaders: " +
           McpValues::libraryShaderNamesText() + ".";
}

bool parseHex(const std::string& text, Vector4& out) {
    std::string hex = text;
    if (!hex.empty() && hex[0] == '#') hex = hex.substr(1);
    if (hex.size() != 6 && hex.size() != 8) return false;
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (size_t i = 0; i < hex.size() / 2; i++) {
        v[i] = static_cast<float>(std::stoi(hex.substr(i * 2, 2), nullptr, 16)) / 255.0f;
    }
    out = Vector4(v[0], v[1], v[2], v[3]);
    return true;
}

} // namespace

std::shared_ptr<const ShaderManifest> McpValues::findManifest(const std::string& reference) {
    if (reference.empty()) return nullptr;
    std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(reference);
    return manifest && manifest->present ? manifest : nullptr;
}

std::string McpValues::libraryShaderNamesText() {
    std::string names;
    for (const std::string& name : ShaderManifests::getLibraryShaderNames()) names += (names.empty() ? "library:" : ", library:") + name;
    return names;
}

bool McpValues::checkShaderReference(Project* project, const std::string& reference, ShaderType type, std::string& error) {
    if (reference.empty()) return true;
    if (ShaderReference::isLibrary(reference)) {
        std::string vert, frag, why;
        if (!ShaderVariantCompiler::getLibraryShaderPaths(reference, type == ShaderType::DEPTH ? ShaderType::DEPTH : type, vert, frag, &why)) {
            error = why + ". The library shaders: " + libraryShaderNamesText() + " (list_shaders describes them).";
            return false;
        }
        // a mesh shader does not draw a UI: the manifest says what it is for
        std::shared_ptr<const ShaderManifest> manifest = findManifest(reference);
        if (manifest && manifest->hasType && type != ShaderType::DEPTH && manifest->type != type) {
            error = reference + " is a " + ShaderManifest::getShaderTypeToken(manifest->type) + " shader; this takes a " +
                    ShaderManifest::getShaderTypeToken(type) + " shader (list_shaders type=\"" +
                    ShaderManifest::getShaderTypeToken(type) + "\").";
            return false;
        }
        return true;
    }
    if (reference.compare(0, std::char_traits<char>::length(ShaderReference::LIBRARY_PREFIX), ShaderReference::LIBRARY_PREFIX) == 0) {
        error = "\"library:\" is reserved for library shaders.";
        return false;
    }
    Util::CustomShaderPaths paths = project ? project->resolveCustomShaderFiles(reference) : Util::resolveCustomShaderPaths(reference);
    std::error_code ec;
    if (!project || !fs::exists(project->getProjectPath() / paths.vert, ec) || !fs::exists(project->getProjectPath() / paths.frag, ec)) {
        error = "references missing files (" + paths.vert + ", " + paths.frag + "): use a library shader (list_shaders) "
                "or fork the built-in shader first.";
        return false;
    }
    return true;
}

Json McpValues::manifestToJson(const std::string& reference, const ShaderManifest& manifest, bool detail) {
    Json out = {
        {"shader", reference},
        {"name", manifest.name},
        {"type", ShaderManifest::getShaderTypeToken(manifest.type)},
        {"category", manifest.category},
        {"description", manifest.description}
    };
    Json lighting = Json::array();
    if (manifest.lighting.empty()) lighting.push_back("none");
    for (ShaderLightingMode mode : manifest.lighting) lighting.push_back(ShaderManifest::getLightingModeName(mode));
    out["lighting"] = lighting;
    if (!detail) {
        Json features = Json::array();
        for (const ShaderManifestFeature& feature : manifest.features) features.push_back(feature.define);
        out["features"] = features;
        return out;
    }

    // @require: the inputs, then the feature= ones ("!<DEFINE>": while it is off)
    auto requireJson = [](uint32_t flags) {
        Json list = Json::array();
        const std::pair<uint32_t, const char*> inputs[] = {
            {ShaderRequire_Position, "position"}, {ShaderRequire_Normals, "normals"}, {ShaderRequire_Tangents, "tangents"},
            {ShaderRequire_UV1, "uv1"}, {ShaderRequire_UV2, "uv2"}, {ShaderRequire_Color, "color"},
            {ShaderRequire_RawColor, "rawcolor"}, {ShaderRequire_SmoothNormals, "smoothnormals"}};
        for (const auto& [flag, name] : inputs) {
            if (flags & flag) list.push_back(name);
        }
        return list;
    };
    out["require"] = requireJson(manifest.require);
    Json requireRules = Json::array();
    for (const ShaderManifestRequireRule& rule : manifest.requireRules) {
        requireRules.push_back({{"require", requireJson(rule.require)}, {"feature", (rule.negate ? "!" : "") + rule.define}});
    }
    out["require_rules"] = requireRules;

    // @state defaults, as the material properties name them, then the @state[<DEFINE>] rules
    // that apply over them while a feature or keyword is on ("!<DEFINE>": off), in order
    auto stateJson = [](const ShaderManifestState& s) {
        Json state = Json::object();
        if (s.blend != MaterialBlendMode::AUTO) state["blendMode"] = enumToJson((int)s.blend, *enumTable("material_blend_mode"));
        if (s.cull != MaterialCullMode::AUTO) state["cullMode"] = enumToJson((int)s.cull, *enumTable("material_cull_mode"));
        if (s.depthWrite != MaterialDepthWrite::AUTO) state["depthWrite"] = enumToJson((int)s.depthWrite, *enumTable("material_depth_write"));
        if (s.depthTest != MaterialDepthTest::AUTO) state["depthTest"] = enumToJson((int)s.depthTest, *enumTable("material_depth_test"));
        if (s.hasDepthOffset) {
            state["depthOffsetFactor"] = number(s.depthOffsetFactor);
            state["depthOffsetUnits"] = number(s.depthOffsetUnits);
        }
        if (s.queue == ShaderQueue::GEOMETRY) state["queue"] = "geometry";
        else if (s.queue == ShaderQueue::ALPHATEST) state["queue"] = "alphatest";
        else if (s.queue == ShaderQueue::TRANSPARENT) state["queue"] = "transparent";
        if (s.shadows == ShaderShadowCasting::ON) state["shadows"] = "on";
        else if (s.shadows == ShaderShadowCasting::OFF) state["shadows"] = "off";
        return state;
    };
    out["state"] = stateJson(manifest.state);
    Json stateRules = Json::array();
    for (const ShaderManifestStateRule& rule : manifest.stateRules) {
        stateRules.push_back({{"feature", (rule.negate ? "!" : "") + rule.define}, {"state", stateJson(rule.state)}});
    }
    out["state_rules"] = stateRules;

    Json properties = Json::array();
    for (const ShaderManifestProperty& property : manifest.properties) {
        Json entry = {{"member", property.member}, {"kind", propertyKindName(property.kind)},
                      {"default", propertyValueJson(property, property.defaultValue)}, {"label", property.label}};
        if (property.kind == ShaderPropertyKind::RANGE) {
            entry["min"] = number(property.min);
            entry["max"] = number(property.max);
        }
        if (!property.feature.empty()) entry["feature"] = property.feature;
        properties.push_back(entry);
    }
    out["properties"] = properties;

    Json textures = Json::array();
    for (const ShaderManifestTexture& texture : manifest.textures) {
        Json entry = {{"uniform", texture.uniform}, {"default", textureDefaultName(texture.defaultTexture)}, {"label", texture.label}};
        if (texture.cube) entry["cube"] = true;
        if (!texture.feature.empty()) entry["feature"] = texture.feature;
        textures.push_back(entry);
    }
    out["textures"] = textures;

    Json features = Json::array();
    for (const ShaderManifestFeature& feature : manifest.features) {
        features.push_back({{"define", feature.define}, {"label", feature.label}, {"default_on", feature.defaultOn}});
    }
    out["features"] = features;

    Json keywords = Json::array();
    for (const ShaderManifestKeywords& group : manifest.keywords) {
        Json defines = Json::array();
        for (const std::string& value : group.values) defines.push_back(group.group + "_" + value);
        keywords.push_back({{"group", group.group}, {"label", group.label}, {"values", group.values}, {"defines", defines}});
    }
    out["keywords"] = keywords;
    out["depth_program"] = manifest.depth;
    return out;
}

// ------------------------------------------------------------------ parameters

Json McpValues::shaderParamsToJson(const ShaderUniformValues& values) {
    return shaderUniformsToJson(values);
}

bool McpValues::shaderParamFromJson(const ShaderManifestProperty* property, const std::string& member, const Json& value,
                                    Vector4& out, std::string& error) {
    if (value.is_string()) {
        if (!property || !isColorKind(property->kind) || !parseHex(value.get<std::string>(), out)) {
            error = "\"" + member + "\" takes a number or up to 4 numbers" +
                    std::string(property && isColorKind(property->kind) ? ", or \"#RRGGBB[AA]\" (sRGB)" : "") + ".";
            return false;
        }
        // hex is sRGB, the value is stored linear (alpha stays)
        const Vector4 linear = Color::sRGBToLinear(out);
        out = Vector4(linear.x, linear.y, linear.z, out.w);
        return true;
    }
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (value.is_number()) {
        v[0] = value.get<float>();
    } else if (value.is_boolean() && property && property->kind == ShaderPropertyKind::TOGGLE) {
        v[0] = value.get<bool>() ? 1.0f : 0.0f;
    } else if (value.is_array() && !value.empty() && value.size() <= 4) {
        for (size_t i = 0; i < value.size(); i++) {
            if (!value[i].is_number()) {
                error = "\"" + member + "\" must be a number or up to 4 numbers.";
                return false;
            }
            v[i] = value[i].get<float>();
        }
        // a colour given as [r,g,b] keeps an opaque alpha
        if (property && isColorKind(property->kind) && value.size() == 3) v[3] = 1.0f;
    } else {
        error = "\"" + member + "\" must be a number or up to 4 numbers" +
                std::string(property && isColorKind(property->kind) ? " ([r,g,b,a] linear), or \"#RRGGBB[AA]\" (sRGB)" : "") + ".";
        return false;
    }
    out = Vector4(v[0], v[1], v[2], v[3]);
    return true;
}

bool McpValues::shaderParamsFromJson(const ShaderManifest* manifest, bool library, const Json& value, ShaderUniformValues& out,
                                     std::string& error, const std::set<std::string>* checkedKeys) {
    out.clear();
    if (value.is_null()) return true;
    if (!value.is_object()) {
        error = "shader parameters are an object {member: number | [x,y,z,w] | \"#RRGGBB\"}.";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& member = it.key();
        if (ShaderUniforms::isReserved(member)) {
            error = "\"" + member + "\" is set by the engine every frame (reserved).";
            return false;
        }
        const ShaderManifestProperty* property = manifest ? manifest->findProperty(member) : nullptr;
        // values kept from another shader stay; only what this edit names must be the shader's
        const bool checked = !checkedKeys || checkedKeys->count(member) > 0;
        if (library && checked && !manifest) {
            error = unknownLibraryShaderText();
            return false;
        }
        if (library && checked && !property) {
            error = "the shader has no parameter \"" + member + "\"; its parameters: " + memberNames(*manifest) + ".";
            return false;
        }
        Vector4 v;
        if (!shaderParamFromJson(property, member, it.value(), v, error)) return false;
        out.push_back({member, v});
    }
    return true;
}

// ------------------------------------------------------------------ textures

Json McpValues::shaderTexturesToJson(Project* project, SceneProject* scene, const std::vector<std::pair<std::string, Texture>>& textures) {
    Json out = Json::object();
    for (const auto& [uniform, texture] : textures) {
        Json json = textureToJson(project, scene, texture);
        if (!json.is_null()) out[uniform] = json;
    }
    return out;
}

bool McpValues::shaderTexturesFromJson(Project* project, SceneProject* scene, const ShaderManifest* manifest, bool library,
                                       const Json& value, std::vector<std::pair<std::string, Texture>>& out, std::string& error,
                                       const std::set<std::string>* checkedKeys) {
    out.clear();
    if (value.is_null()) return true;
    if (!value.is_object()) {
        error = "shader textures are an object {uniform: \"image path\" | {texture} | null}.";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& uniform = it.key();
        const ShaderManifestTexture* declared = manifest ? manifest->findTexture(uniform) : nullptr;
        const bool checked = !checkedKeys || checkedKeys->count(uniform) > 0;
        if (library && checked && !manifest) {
            error = unknownLibraryShaderText();
            return false;
        }
        if (library && checked && !declared) {
            error = "the shader has no texture \"" + uniform + "\"; its textures: " + textureNames(*manifest) + ".";
            return false;
        }
        Texture texture;
        Entity camera = NULL_ENTITY;
        std::string why;
        if (!textureFromJson(project, scene, it.value(), texture, camera, why)) {
            error = uniform + ": " + why;
            return false;
        }
        // a shader texture is an image: render textures are not linked, exported or remapped
        // for these slots (a camera's texture goes in a material's baseColorTexture, ...)
        if (camera != NULL_ENTITY || CameraTextureLink::isCameraTexture(texture) || texture.isFramebuffer()) {
            error = uniform + ": a shader texture is an image path; a camera's render texture goes in a built-in "
                    "material texture (baseColorTexture, emissiveTexture) or a UI image's texture.";
            return false;
        }
        if (texture.empty()) continue; // null: the manifest default
        if (declared && declared->cube != texture.isCubeMap()) {
            error = uniform + (declared->cube ? " is a cube map: {\"cube\": \"image\" | [6 face paths]}."
                                              : " is a 2D texture: an image path, not a cube map.");
            return false;
        }
        out.push_back({uniform, texture});
    }
    return true;
}

// ------------------------------------------------------------------ features

Json McpValues::shaderFeaturesToJson(const std::vector<std::string>& features) {
    Json out = Json::array();
    for (const std::string& feature : features) out.push_back(feature);
    return out;
}

bool McpValues::shaderFeaturesFromJson(const std::string& shader, const ShaderManifest* manifest, bool library, const Json& value,
                                       std::vector<std::string>& inout, std::string& error) {
    auto checkDefine = [&](const std::string& define) {
        const std::string bare = (!define.empty() && define[0] == '!') ? define.substr(1) : define;
        if (!ShaderReference::isValidDefine(bare)) {
            error = "\"" + define + "\" is not a define name (upper case letters, digits and _).";
            return false;
        }
        if (library && !manifest) {
            error = unknownLibraryShaderText();
            return false;
        }
        if (library && !manifest->findFeature(bare) && !manifest->findKeywordGroup(bare)) {
            error = "the shader has no feature \"" + bare + "\"; its features and keywords: " + defineNames(*manifest) + ".";
            return false;
        }
        return true;
    };
    if (value.is_null()) {
        inout.clear();
        return true;
    }
    if (value.is_array()) {
        std::vector<std::string> list;
        for (const Json& item : value) {
            if (!item.is_string()) {
                error = "shaderFeatures is a list of define names, or {define: true|false}.";
                return false;
            }
            if (!checkDefine(item.get<std::string>())) return false;
            list.push_back(item.get<std::string>());
        }
        inout = list;
        return true;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!it.value().is_boolean()) {
                error = "shaderFeatures {define: true|false}: \"" + it.key() + "\" must be true or false.";
                return false;
            }
            if (!checkDefine(it.key())) return false;
            ShaderLibraryValues::setFeature(inout, shader, it.key(), it.value().get<bool>());
        }
        return true;
    }
    error = "shaderFeatures is a list of define names, or {define: true|false}.";
    return false;
}

// ------------------------------------------------------------------ material keys

bool McpValues::isMaterialShaderLibraryKey(const std::string& key) {
    static const char* const keys[] = {"shader", "shaderParams", "shaderTextures", "shaderFeatures", "blendMode", "cullMode",
                                       "depthWrite", "depthTest", "depthOffsetFactor", "depthOffsetUnits", "sortPriority", "lighting"};
    for (const char* known : keys) {
        if (key == known) return true;
    }
    return false;
}

void McpValues::materialShaderLibraryToJson(Project* project, SceneProject* scene, const Material& material, Json& out, bool all) {
    out["shader"] = material.shader;
    if (all || !material.shaderParams.empty()) out["shaderParams"] = shaderParamsToJson(material.shaderParams);
    if (all || !material.shaderTextures.empty()) out["shaderTextures"] = shaderTexturesToJson(project, scene, material.shaderTextures);
    if (all || !material.shaderFeatures.empty()) out["shaderFeatures"] = shaderFeaturesToJson(material.shaderFeatures);
    out["blendMode"] = enumToJson((int)material.blendMode, *enumTable("material_blend_mode"));
    out["cullMode"] = enumToJson((int)material.cullMode, *enumTable("material_cull_mode"));
    out["depthWrite"] = enumToJson((int)material.depthWrite, *enumTable("material_depth_write"));
    out["depthTest"] = enumToJson((int)material.depthTest, *enumTable("material_depth_test"));
    if (all || material.depthOffsetFactor != 0.0f || material.depthOffsetUnits != 0.0f) {
        out["depthOffsetFactor"] = number(material.depthOffsetFactor);
        out["depthOffsetUnits"] = number(material.depthOffsetUnits);
    }
    if (all || material.sortPriority != 0) out["sortPriority"] = material.sortPriority;
    out["lighting"] = enumToJson((int)material.lighting, *enumTable("material_lighting"));
}

bool McpValues::materialShaderLibraryFromJson(Project* project, SceneProject* scene, const Json& value, Material& out, std::string& error) {
    if (value.contains("shader")) {
        if (!value["shader"].is_string()) {
            error = "shader must be a string: \"\" (built-in Lit), \"library:<name>\" or a fork base path.";
            return false;
        }
        const std::string shader = value["shader"].get<std::string>();
        std::string why;
        if (!checkShaderReference(project, shader, ShaderType::MESH, why)) {
            error = "shader: " + why;
            return false;
        }
        const bool changed = out.shader != shader;
        out.shader = shader;
        // a lighting mode the new shader does not list would draw its default anyway: auto
        if (changed && !value.contains("lighting") && !Catalog::supportsLighting(out.lighting, findManifest(shader).get())) {
            out.lighting = MaterialLighting::AUTO;
        }
    }
    const std::shared_ptr<const ShaderManifest> manifest = findManifest(out.shader);
    const bool library = ShaderReference::isLibrary(out.shader);
    if (value.contains("shaderParams")) {
        // an object merges into the values set (null removes one), null clears them all
        const Json& params = value["shaderParams"];
        if (params.is_null()) {
            out.shaderParams.clear();
        } else if (!params.is_object()) {
            error = "shaderParams must be an object {member: value} (null removes a member).";
            return false;
        } else {
            for (auto it = params.begin(); it != params.end(); ++it) {
                if (it.value().is_null()) {
                    ShaderUniforms::remove(out.shaderParams, it.key());
                    continue;
                }
                ShaderUniformValues one;
                std::string why;
                if (!shaderParamsFromJson(manifest.get(), library, Json{{it.key(), it.value()}}, one, why)) {
                    error = "shaderParams: " + why;
                    return false;
                }
                ShaderUniforms::set(out.shaderParams, one[0].first, one[0].second);
            }
        }
    }
    if (value.contains("shaderTextures")) {
        // merges like shaderParams: null removes one (the manifest default binds)
        const Json& textures = value["shaderTextures"];
        if (textures.is_null()) {
            out.shaderTextures.clear();
        } else if (!textures.is_object()) {
            error = "shaderTextures must be an object {uniform: \"image path\" | null}.";
            return false;
        } else {
            for (auto it = textures.begin(); it != textures.end(); ++it) {
                std::vector<std::pair<std::string, Texture>> one;
                std::string why;
                if (!shaderTexturesFromJson(project, scene, manifest.get(), library, Json{{it.key(), it.value()}}, one, why)) {
                    error = "shaderTextures: " + why;
                    return false;
                }
                auto found = std::find_if(out.shaderTextures.begin(), out.shaderTextures.end(),
                                          [&](const auto& entry) { return entry.first == it.key(); });
                if (one.empty()) {
                    if (found != out.shaderTextures.end()) out.shaderTextures.erase(found);
                } else if (found != out.shaderTextures.end()) {
                    found->second = one[0].second;
                } else {
                    out.shaderTextures.push_back(one[0]);
                }
            }
        }
    }
    if (value.contains("shaderFeatures")) {
        std::string why;
        if (!shaderFeaturesFromJson(out.shader, manifest.get(), library, value["shaderFeatures"], out.shaderFeatures, why)) {
            error = "shaderFeatures: " + why;
            return false;
        }
    }
    auto enumKey = [&](const char* key, const char* table, auto& target) {
        if (!value.contains(key)) return true;
        int parsed = 0;
        std::string why;
        if (!enumFromJson(value[key], *enumTable(table), parsed, why)) {
            error = std::string(key) + ": " + why;
            return false;
        }
        target = static_cast<std::remove_reference_t<decltype(target)>>(parsed);
        return true;
    };
    if (!enumKey("blendMode", "material_blend_mode", out.blendMode) ||
        !enumKey("cullMode", "material_cull_mode", out.cullMode) ||
        !enumKey("depthWrite", "material_depth_write", out.depthWrite) ||
        !enumKey("depthTest", "material_depth_test", out.depthTest) ||
        !enumKey("lighting", "material_lighting", out.lighting)) {
        return false;
    }
    // a lighting mode the shader does not list draws with its default instead
    if (value.contains("lighting") && !Catalog::supportsLighting(out.lighting, manifest.get())) {
        std::string modes = "auto";
        for (MaterialLighting mode : Catalog::getShaderLightingModes(manifest.get())) {
            modes += ", " + enumToJson((int)mode, *enumTable("material_lighting")).get<std::string>();
        }
        error = "lighting: " + (out.shader.empty() ? std::string("the built-in Lit shader") : out.shader) + " takes " + modes + ".";
        return false;
    }
    for (const char* key : {"depthOffsetFactor", "depthOffsetUnits"}) {
        if (!value.contains(key)) continue;
        if (!value[key].is_number()) {
            error = std::string(key) + " must be a number.";
            return false;
        }
        (std::string(key) == "depthOffsetFactor" ? out.depthOffsetFactor : out.depthOffsetUnits) = value[key].get<float>();
    }
    if (value.contains("sortPriority")) {
        if (!value["sortPriority"].is_number_integer()) {
            error = "sortPriority must be an integer (lower draws first among transparent objects).";
            return false;
        }
        out.sortPriority = value["sortPriority"].get<int>();
    }
    return true;
}
