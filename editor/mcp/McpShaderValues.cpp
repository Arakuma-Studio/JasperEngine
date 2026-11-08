// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// McpValues for the shader library: shader references, manifests (list_shaders), and the
// JSON of shaderParams / shaderTextures / shaderFeatures and the render state of a material
// or a UI/Points/LineRenderer/Skybox component.

#include "McpValues.h"
#include "component/TerrainComponent.h"
#include "shader/ShaderGlobals.h"

#include "McpRefs.h"
#include "Project.h"
#include "subsystem/RenderSystem.h"
#include "shader/ShaderVariantCompiler.h"
#include "util/CameraTextureLink.h"
#include "util/Color.h"
#include "util/Util.h"

#include <algorithm>

#include <cctype>
#include <filesystem>
#include <type_traits>

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
        case ShaderTextureDefault::RIPPLES: return "ripples";
        case ShaderTextureDefault::NOISE: return "noise";
        case ShaderTextureDefault::PARTICLE: return "particle";
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

Json McpValues::sceneInputsToJson(uint32_t inputs) {
    Json names = Json::array();
    for (const std::string& name : ShaderManifest::getRequireNames(inputs & ShaderRequire_SceneInputs)) names.push_back(name);
    return names;
}

Json McpValues::entitySceneInputsToJson(Scene* scene, Entity entity) {
    Json list = Json::array();
    if (!scene) return list;
    // a mesh's submeshes as the checks see them (ProjectChecks::sceneInputFindings: not terrains)
    const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    if (mesh && !scene->findComponent<TerrainComponent>(entity)) {
        for (unsigned int s = 0; s < mesh->numSubmeshes; s++) {
            const uint32_t reads = Catalog::getMaterialSceneInputs(scene, entity, s, false);
            const uint32_t passReads = Catalog::getMaterialSceneInputs(scene, entity, s, true);
            if (!reads && !passReads) continue;
            bool opaqueQueue = false;
            RenderSystem::getSubmeshSceneInputs(*mesh, mesh->submeshes[s], scene->getDefaultCustomShader(ShaderType::MESH), opaqueQueue);
            Json entry = {{"component", McpRefs::componentName(ComponentType::MeshComponent)}, {"submesh", s},
                          {"reads", sceneInputsToJson(reads)}};
            if (passReads) entry["extra_pass_reads"] = sceneInputsToJson(passReads);
            entry["queue"] = opaqueQueue ? "opaque" : "transparent";
            if (opaqueQueue) {
                const Catalog::SceneInputQueueFix fix = Catalog::getSceneInputQueueFix(scene, entity, s);
                entry["error"] = "reads the scene " + Catalog::getSceneInputsLabel(reads | passReads) +
                                 " but draws in the opaque queue: it would read its own depth and a black colour; " +
                                 Catalog::getSceneInputQueueAdvice(fix, s);
                // the set_properties values of the inspector's fix button
                if (fix == Catalog::SceneInputQueueFix::ALPHA_BLEND) {
                    entry["fix"] = {{Catalog::getSceneInputQueueFixProperty(fix, s), enumToJson((int)MaterialBlendMode::ALPHA, *enumTable("material_blend_mode"))}};
                } else if (fix == Catalog::SceneInputQueueFix::MESH_TRANSPARENT) {
                    entry["fix"] = {{Catalog::getSceneInputQueueFixProperty(fix, s), true}};
                }
            }
            list.push_back(entry);
        }
    }
    if (scene->findComponent<PointsComponent>(entity)) {
        if (const uint32_t reads = Catalog::getComponentSceneInputs(scene, entity, ComponentType::PointsComponent)) {
            list.push_back({{"component", McpRefs::componentName(ComponentType::PointsComponent)}, {"reads", sceneInputsToJson(reads)}});
        }
    }
    return list;
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
    // what some variant reads of the scene (phase 3): "scenedepth" adds a depth pre-pass and
    // "scenecolor" a copy of the opaque scene to the camera passes that draw it; cost_note says
    // when (which features) and what that costs (Catalog: what the inspector's marks say)
    const uint32_t possibleSceneInputs = manifest.getPossibleSceneInputs();
    out["scene_inputs"] = sceneInputsToJson(possibleSceneInputs);
    if (possibleSceneInputs) {
        // the summary keeps it to one line; the detail adds the whole cost
        out["cost_note"] = Catalog::getShaderSceneInputsSummary(manifest) + ": " + Catalog::getSceneInputsCost(possibleSceneInputs, true) +
                           (detail ? ". " + Catalog::getSceneInputsCost(possibleSceneInputs) : std::string("."));
    }
    // what the engine treats it as (@role): water floats bodies and answers Water queries, underwater switches
    if (manifest.role != ShaderRole::NONE)
        out["role"] = manifest.role == ShaderRole::WATER ? "water" : "underwater";
    // a planar reflection (@require planarreflection): a camera pass per water height
    if (manifest.getPossibleRequire() & ShaderRequire_PlanarReflection) {
        const std::string planar = "planar reflection: the scene drawn again, mirrored, once per water height a camera sees "
                                   "(scene setting planar_reflection_resolution; off draws none)";
        out["cost_note"] = out.contains("cost_note") ? out["cost_note"].get<std::string>() + " " + planar + "." : planar + ".";
    }
    if (!detail) {
        Json features = Json::array();
        for (const ShaderManifestFeature& feature : manifest.features) features.push_back(feature.define);
        out["features"] = features;
        return out;
    }

    // @require: the inputs, then the feature= ones ("!<DEFINE>": while it is off)
    auto requireJson = [](uint32_t flags) {
        Json list = Json::array();
        for (const std::string& name : ShaderManifest::getRequireNames(flags)) list.push_back(name);
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
        // phase 2: the stencil keys this @state sets (a rule replaces only those) and colorwrite=
        if (s.stencilSet != 0) {
            Json stencil = Json::object();
            const Json full = stencilToJson(s.stencil);
            const std::pair<uint8_t, const char*> fields[] = {
                {ShaderStencil_Compare, "compare"}, {ShaderStencil_Ref, "ref"}, {ShaderStencil_ReadMask, "readMask"},
                {ShaderStencil_WriteMask, "writeMask"}, {ShaderStencil_Pass, "pass"}, {ShaderStencil_Fail, "fail"},
                {ShaderStencil_DepthFail, "depthFail"}};
            for (const auto& [bit, name] : fields) {
                if (s.stencilSet & bit) stencil[name] = full[name];
            }
            state["stencil"] = stencil;
        }
        if (s.colorWrite >= 0) state["colorWrite"] = colorWriteToJson(s.colorWrite);
        if (s.hasPriority) state["priority"] = s.priority;
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
        // the default an unset slot binds instead while that define is on
        if (!texture.featureDefaultDefine.empty())
            entry["default_with"] = {{"feature", texture.featureDefaultDefine}, {"default", textureDefaultName(texture.featureDefaultTexture)}};
        textures.push_back(entry);
    }
    out["textures"] = textures;

    Json features = Json::array();
    for (const ShaderManifestFeature& feature : manifest.features) {
        Json entry = {{"define", feature.define}, {"label", feature.label}, {"default_on", feature.defaultOn}};
        // the scene inputs it turns on (scene_inputs_when_off: a "feature=!X" rule's)
        if (const uint32_t inputs = Catalog::getFeatureSceneInputs(manifest, feature.define)) entry["scene_inputs"] = sceneInputsToJson(inputs);
        if (const uint32_t inputs = Catalog::getFeatureSceneInputs(manifest, feature.define, true)) entry["scene_inputs_when_off"] = sceneInputsToJson(inputs);
        features.push_back(entry);
    }
    out["features"] = features;

    Json keywords = Json::array();
    for (const ShaderManifestKeywords& group : manifest.keywords) {
        Json defines = Json::array();
        Json valueInputs = Json::object();
        for (const std::string& value : group.values) {
            defines.push_back(group.group + "_" + value);
            if (const uint32_t inputs = Catalog::getFeatureSceneInputs(manifest, group.group + "_" + value)) valueInputs[value] = sceneInputsToJson(inputs);
        }
        Json entry = {{"group", group.group}, {"label", group.label}, {"values", group.values}, {"defines", defines}};
        if (!valueInputs.empty()) entry["scene_inputs"] = valueInputs;
        keywords.push_back(entry);
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
            error = uniform + (declared->cube ? " is a cube map: {\"cube\": \"image\" | [front, back, left, right, up, down]}."
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

// ------------------------------------------------------------------ stencil, colour write

Json McpValues::stencilToJson(const MaterialStencil& stencil) {
    return {
        {"compare", enumToJson((int)stencil.compare, *enumTable("material_stencil_compare"))},
        {"ref", stencil.ref},
        {"readMask", stencil.readMask},
        {"writeMask", stencil.writeMask},
        {"pass", enumToJson((int)stencil.pass, *enumTable("material_stencil_op"))},
        {"fail", enumToJson((int)stencil.fail, *enumTable("material_stencil_op"))},
        {"depthFail", enumToJson((int)stencil.depthFail, *enumTable("material_stencil_op"))}
    };
}

bool McpValues::stencilFromJson(const Json& value, MaterialStencil& inout, std::string& error) {
    if (value.is_null()) {
        inout = MaterialStencil();
        return true;
    }
    if (!value.is_object()) {
        error = "stencil is an object {compare, ref, readMask, writeMask, pass, fail, depthFail} (the keys given change; "
                "null resets it to auto).";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& key = it.key();
        const Json& item = it.value();
        std::string why;
        int parsed = 0;
        if (key == "compare") {
            if (!enumFromJson(item, *enumTable("material_stencil_compare"), parsed, why)) {
                error = "stencil.compare: " + why;
                return false;
            }
            inout.compare = static_cast<MaterialStencilCompare>(parsed);
        } else if (key == "pass" || key == "fail" || key == "depthFail" || key == "zfail") {
            if (!enumFromJson(item, *enumTable("material_stencil_op"), parsed, why)) {
                error = "stencil." + key + ": " + why;
                return false;
            }
            (key == "pass" ? inout.pass : key == "fail" ? inout.fail : inout.depthFail) = static_cast<MaterialStencilOp>(parsed);
        } else if (key == "ref" || key == "readMask" || key == "writeMask") {
            if (!item.is_number_integer() || item.get<long long>() < 0 || item.get<long long>() > 255) {
                error = "stencil." + key + " must be an integer 0..255.";
                return false;
            }
            (key == "ref" ? inout.ref : key == "readMask" ? inout.readMask : inout.writeMask) = item.get<int>();
        } else {
            error = "unknown stencil key \"" + key + "\"; valid: compare, ref, readMask, writeMask, pass, fail, depthFail.";
            return false;
        }
    }
    return true;
}

Json McpValues::colorWriteToJson(int colorWrite) {
    return ShaderManifest::getColorWriteName(colorWrite);
}

bool McpValues::colorWriteFromJson(const Json& value, int& out, std::string& error) {
    if (value.is_null()) {
        out = -1;
        return true;
    }
    if (value.is_number_integer()) {
        const long long mask = value.get<long long>();
        if (mask < -1 || mask > MATERIAL_COLOR_WRITE_ALL) {
            error = "colorWrite takes \"auto\" (-1), \"none\" (0), channels of \"rgba\" (\"rgb\", \"a\"...) or a mask 0..15 (r 1, g 2, b 4, a 8).";
            return false;
        }
        out = static_cast<int>(mask);
        return true;
    }
    if (value.is_string()) {
        std::string name = value.get<std::string>();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const int mask = name == "auto" ? -1 : name == "all" ? (int)MATERIAL_COLOR_WRITE_ALL : ShaderManifest::parseColorWrite(name);
        if (name == "auto" || mask >= 0) {
            out = mask;
            return true;
        }
    }
    error = "colorWrite takes \"auto\", \"none\", channels of \"rgba\" (\"rgb\", \"a\"...) or a mask 0..15 (r 1, g 2, b 4, a 8).";
    return false;
}

// ------------------------------------------------------------------ material keys

bool McpValues::isMaterialShaderLibraryKey(const std::string& key) {
    static const char* const keys[] = {"shader", "shaderParams", "shaderTextures", "shaderFeatures", "shaderGlobalBindings", "blendMode",
                                       "cullMode", "depthWrite", "depthTest", "depthOffsetFactor", "depthOffsetUnits", "sortPriority",
                                       "lighting", "stencil", "colorWrite", "extraPass"};
    for (const char* known : keys) {
        if (key == known) return true;
    }
    return false;
}

namespace {

// The shader, its values and the render state shared by a material and its extra pass
template<typename T>
void shaderStateToJson(Project* project, SceneProject* scene, const T& value, Json& out, bool all) {
    out["shader"] = value.shader;
    if (all || !value.shaderParams.empty()) out["shaderParams"] = McpValues::shaderParamsToJson(value.shaderParams);
    if (all || !value.shaderTextures.empty()) out["shaderTextures"] = McpValues::shaderTexturesToJson(project, scene, value.shaderTextures);
    if (all || !value.shaderFeatures.empty()) out["shaderFeatures"] = McpValues::shaderFeaturesToJson(value.shaderFeatures);
    out["blendMode"] = McpValues::enumToJson((int)value.blendMode, *McpValues::enumTable("material_blend_mode"));
    out["cullMode"] = McpValues::enumToJson((int)value.cullMode, *McpValues::enumTable("material_cull_mode"));
    out["depthWrite"] = McpValues::enumToJson((int)value.depthWrite, *McpValues::enumTable("material_depth_write"));
    out["depthTest"] = McpValues::enumToJson((int)value.depthTest, *McpValues::enumTable("material_depth_test"));
    if (all || value.depthOffsetFactor != 0.0f || value.depthOffsetUnits != 0.0f) {
        out["depthOffsetFactor"] = McpValues::number(value.depthOffsetFactor);
        out["depthOffsetUnits"] = McpValues::number(value.depthOffsetUnits);
    }
    if (all || value.stencil != MaterialStencil()) out["stencil"] = McpValues::stencilToJson(value.stencil);
    out["colorWrite"] = McpValues::colorWriteToJson(value.colorWrite);
}

// Applies the keys a material and its extra pass share ("shader" first: the others follow its
// manifest). `where` prefixes the errors ("" or "extraPass.").
template<typename T>
bool shaderStateFromJson(Project* project, SceneProject* scene, const Json& value, T& out, const std::string& where, std::string& error) {
    constexpr bool isMaterial = std::is_same_v<T, Material>;
    if (value.contains("shader")) {
        if (!value["shader"].is_string()) {
            error = where + (isMaterial ? "shader must be a string: \"\" (built-in Lit), \"library:<name>\" or a fork base path."
                                        : "shader must be a string: \"\" (no extra pass), \"library:<name>\" or a fork base path.");
            return false;
        }
        const std::string shader = value["shader"].get<std::string>();
        std::string why;
        if (!McpValues::checkShaderReference(project, shader, ShaderType::MESH, why)) {
            error = where + "shader: " + why;
            return false;
        }
        const bool changed = out.shader != shader;
        out.shader = shader;
        if constexpr (isMaterial) {
            // a lighting mode the new shader does not list would draw its default anyway: auto
            if (changed && !value.contains("lighting") && !Catalog::supportsLighting(out.lighting, McpValues::findManifest(shader).get())) {
                out.lighting = MaterialLighting::AUTO;
            }
        }
    }
    const std::shared_ptr<const ShaderManifest> manifest = McpValues::findManifest(out.shader);
    const bool library = ShaderReference::isLibrary(out.shader);
    if (value.contains("shaderParams")) {
        // an object merges into the values set (null removes one), null clears them all
        const Json& params = value["shaderParams"];
        if (params.is_null()) {
            out.shaderParams.clear();
        } else if (!params.is_object()) {
            error = where + "shaderParams must be an object {member: value} (null removes a member).";
            return false;
        } else {
            for (auto it = params.begin(); it != params.end(); ++it) {
                if (it.value().is_null()) {
                    ShaderUniforms::remove(out.shaderParams, it.key());
                    continue;
                }
                ShaderUniformValues one;
                std::string why;
                if (!McpValues::shaderParamsFromJson(manifest.get(), library, Json{{it.key(), it.value()}}, one, why)) {
                    error = where + "shaderParams: " + why;
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
            error = where + "shaderTextures must be an object {uniform: \"image path\" | null}.";
            return false;
        } else {
            for (auto it = textures.begin(); it != textures.end(); ++it) {
                std::vector<std::pair<std::string, Texture>> one;
                std::string why;
                if (!McpValues::shaderTexturesFromJson(project, scene, manifest.get(), library, Json{{it.key(), it.value()}}, one, why)) {
                    error = where + "shaderTextures: " + why;
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
        if (!McpValues::shaderFeaturesFromJson(out.shader, manifest.get(), library, value["shaderFeatures"], out.shaderFeatures, why)) {
            error = where + "shaderFeatures: " + why;
            return false;
        }
    }
    auto enumKey = [&](const char* key, const char* table, auto& target) {
        if (!value.contains(key)) return true;
        int parsed = 0;
        std::string why;
        if (!McpValues::enumFromJson(value[key], *McpValues::enumTable(table), parsed, why)) {
            error = where + key + ": " + why;
            return false;
        }
        target = static_cast<std::remove_reference_t<decltype(target)>>(parsed);
        return true;
    };
    if (!enumKey("blendMode", "material_blend_mode", out.blendMode) ||
        !enumKey("cullMode", "material_cull_mode", out.cullMode) ||
        !enumKey("depthWrite", "material_depth_write", out.depthWrite) ||
        !enumKey("depthTest", "material_depth_test", out.depthTest)) {
        return false;
    }
    if constexpr (isMaterial) {
        if (!enumKey("lighting", "material_lighting", out.lighting)) return false;
        // a lighting mode the shader does not list draws with its default instead
        if (value.contains("lighting") && !Catalog::supportsLighting(out.lighting, manifest.get())) {
            std::string modes = "auto";
            for (MaterialLighting mode : Catalog::getShaderLightingModes(manifest.get())) {
                modes += ", " + McpValues::enumToJson((int)mode, *McpValues::enumTable("material_lighting")).template get<std::string>();
            }
            error = "lighting: " + (out.shader.empty() ? std::string("the built-in Lit shader") : out.shader.path) + " takes " + modes + ".";
            return false;
        }
    } else {
        if (!enumKey("order", "material_pass_order", out.order)) return false;
    }
    for (const char* key : {"depthOffsetFactor", "depthOffsetUnits"}) {
        if (!value.contains(key)) continue;
        if (!value[key].is_number()) {
            error = where + key + " must be a number.";
            return false;
        }
        (std::string(key) == "depthOffsetFactor" ? out.depthOffsetFactor : out.depthOffsetUnits) = value[key].template get<float>();
    }
    if (value.contains("stencil")) {
        std::string why;
        if (!McpValues::stencilFromJson(value["stencil"], out.stencil, why)) {
            error = where + why;
            return false;
        }
    }
    if (value.contains("colorWrite")) {
        std::string why;
        if (!McpValues::colorWriteFromJson(value["colorWrite"], out.colorWrite, why)) {
            error = where + why;
            return false;
        }
    }
    return true;
}

} // namespace

Json McpValues::materialPassToJson(Project* project, SceneProject* scene, const MaterialPass& pass, bool all) {
    Json out = Json::object();
    shaderStateToJson(project, scene, pass, out, all);
    out["order"] = enumToJson((int)pass.order, *enumTable("material_pass_order"));
    return out;
}

bool McpValues::materialPassFromJson(Project* project, SceneProject* scene, const Json& value, MaterialPass& inout, std::string& error) {
    if (value.is_null()) {
        inout = MaterialPass();
        return true;
    }
    if (!value.is_object()) {
        error = "extraPass is an object {shader, shaderParams, shaderTextures, shaderFeatures, blendMode, cullMode, depthWrite, "
                "depthTest, depthOffsetFactor, depthOffsetUnits, stencil, colorWrite, order} (the keys given change; null removes "
                "the pass).";
        return false;
    }
    static const char* const keys[] = {"shader", "shaderParams", "shaderTextures", "shaderFeatures", "blendMode", "cullMode",
                                       "depthWrite", "depthTest", "depthOffsetFactor", "depthOffsetUnits", "stencil", "colorWrite", "order"};
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::find_if(std::begin(keys), std::end(keys), [&](const char* key) { return it.key() == key; }) == std::end(keys)) {
            error = "unknown extraPass key \"" + it.key() + "\"; valid: shader, shaderParams, shaderTextures, shaderFeatures, "
                    "blendMode, cullMode, depthWrite, depthTest, depthOffsetFactor, depthOffsetUnits, stencil, colorWrite, order.";
            return false;
        }
    }
    return shaderStateFromJson(project, scene, value, inout, "extraPass.", error);
}

void McpValues::materialShaderLibraryToJson(Project* project, SceneProject* scene, const Material& material, Json& out, bool all) {
    shaderStateToJson(project, scene, material, out, all);
    // members following global shader values {member: global}
    if (all || !material.shaderGlobalBindings.empty()) {
        Json bindings = Json::object();
        for (const auto& binding : material.shaderGlobalBindings) bindings[binding.first] = binding.second;
        out["shaderGlobalBindings"] = bindings;
    }
    if (all || material.sortPriority != 0) out["sortPriority"] = material.sortPriority;
    out["lighting"] = enumToJson((int)material.lighting, *enumTable("material_lighting"));
    // the extra pass: null without one
    if (material.extraPass != MaterialPass()) out["extraPass"] = materialPassToJson(project, scene, material.extraPass, all);
    else if (all) out["extraPass"] = nullptr;
}

bool McpValues::materialShaderLibraryFromJson(Project* project, SceneProject* scene, const Json& value, Material& out, std::string& error) {
    if (!shaderStateFromJson(project, scene, value, out, "", error)) return false;
    if (value.contains("shaderGlobalBindings")) {
        // merged: {member: global | null}
        const Json& bindings = value["shaderGlobalBindings"];
        if (!bindings.is_object()) {
            error = "shaderGlobalBindings is an object {member: global name | null}.";
            return false;
        }
        for (auto it = bindings.begin(); it != bindings.end(); ++it) {
            if (!it.value().is_string() && !it.value().is_null()) {
                error = "shaderGlobalBindings." + it.key() + ": a global's name, or null to unbind.";
                return false;
            }
            const std::string global = it.value().is_string() ? it.value().get<std::string>() : std::string();
            std::string why;
            if (!global.empty() && !ShaderGlobals::isValidName(global, &why)) {
                error = "shaderGlobalBindings." + it.key() + ": " + why;
                return false;
            }
            ShaderGlobals::setBinding(out.shaderGlobalBindings, it.key(), global);
        }
    }
    if (value.contains("sortPriority")) {
        if (!value["sortPriority"].is_number_integer()) {
            error = "sortPriority must be an integer (draw order within its queue, opaque or transparent: lower draws first).";
            return false;
        }
        out.sortPriority = value["sortPriority"].get<int>();
    }
    if (value.contains("extraPass") && !materialPassFromJson(project, scene, value["extraPass"], out.extraPass.edit(), error)) {
        return false;
    }
    return true;
}
