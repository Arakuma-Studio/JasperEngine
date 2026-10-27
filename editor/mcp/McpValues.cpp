// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpValues.h"

#include "command/PlayChangeReview.h"
#include "DocumentFile.h"

#include "McpRefs.h"

#include "Out.h"
#include "Project.h"
#include "Stream.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/LinkMaterialCmd.h"
#include "command/type/ModelClearCmd.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "command/type/SetMainCameraCmd.h"
#include "command/type/UnlinkMaterialCmd.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/ScriptComponent.h"
#include "component/TextComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"
#include "util/CameraTextureLink.h"
#include "util/FluidTextureLink.h"
#include "component/FluidSimulationComponent.h"
#include "fluid/FluidPresets.h"
#include "fluid/FluidSimulator.h"
#include "util/EntityClipboard.h"
#include "util/NearestName.h"
#include "util/FbxImporter.h"
#include "util/FileUtils.h"
#include "util/Util.h"
#include "reflect/Enums.h"
#include "reflect/Reflect.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace fs = std::filesystem;

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace {

// ------------------------------------------------------------------ small helpers

// "Quad In Out", "QUAD_IN_OUT", "quad-in-out" → "quadinout"
std::string enumKey(const std::string& text) {
    std::string key;
    for (unsigned char c : text) {
        if (c == ' ' || c == '_' || c == '-') continue;
        key.push_back(static_cast<char>(std::tolower(c)));
    }
    return key;
}

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

const char* const RGBA[] = {"r", "g", "b", "a"};

// "#RRGGBB" / "#RRGGBBAA" → 0-1 components; count is 3 or 4 (alpha only when given)
bool parseHexColor(const Json& value, float* out, int& count) {
    if (!value.is_string()) return false;
    std::string text = value.get<std::string>();
    if (!text.empty() && text[0] == '#') text.erase(0, 1);
    if (text.size() != 6 && text.size() != 8) return false;
    for (char c : text) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    count = static_cast<int>(text.size() / 2);
    for (int i = 0; i < count; i++) {
        out[i] = static_cast<float>(std::stoi(text.substr(i * 2, 2), nullptr, 16)) / 255.0f;
    }
    return true;
}

// String properties holding an asset reference instead of plain text
bool isAssetPathProperty(const std::string& propertyName) {
    return propertyName == "filename";
}

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Stochastic Tiling's values of a material, in the Inspector's ranges: false (and error) out of
// them, naming the field, what it does and its default
struct StochasticMaterialValue { const char* name; float Material::*field; float lo, hi; const char* meaning; };
const StochasticMaterialValue stochasticMaterialValues[] = {
    {"stochasticCellSize", &Material::stochasticCellSize, 0.25f, 4.0f, "the hex cell width in texture repeats, default 1"},
    {"stochasticBlendSharpness", &Material::stochasticBlendSharpness, 0.0f, 1.0f, "how narrow the blend between cells is, default 0.4"},
    {"stochasticContrast", &Material::stochasticContrast, 0.0f, 1.0f, "default 0.45, 0 blends linearly"},
    {"stochasticRotation", &Material::stochasticRotation, 0.0f, 1.0f, "a random turn per cell, the share of +-180 degrees, default 0"}};

const StochasticMaterialValue* findStochasticMaterialValue(const std::string& name) {
    for (const StochasticMaterialValue& value : stochasticMaterialValues) {
        if (name == value.name) return &value;
    }
    return nullptr;
}

bool checkStochasticMaterialValue(const StochasticMaterialValue& param, const std::string& where, const Json& value, std::string& error) {
    const double number = value.is_number() ? value.get<double>() : -1.0;
    if (!value.is_number() || !std::isfinite(number) || number < param.lo || number > param.hi) {
        char range[64];
        std::snprintf(range, sizeof(range), "%g to %g", param.lo, param.hi);
        error = where + " must be a number from " + range + " (" + param.meaning + ").";
        return false;
    }
    return true;
}

bool isShaderPathProperty(const std::string& propertyName) {
    return propertyName == "customShader" || propertyName == "customDepthShader" ||
           (propertyName.compare(0, 10, "submeshes[") == 0 &&
            (endsWith(propertyName, "].material.shader") || endsWith(propertyName, "].material.extraPass.shader")));
}

// The shader type a shader-path property takes
ShaderType shaderPathType(ComponentType component, const std::string& propertyName) {
    if (propertyName == "customDepthShader") return ShaderType::DEPTH;
    switch (component) {
        case ComponentType::UIComponent: return ShaderType::UI;
        case ComponentType::PointsComponent: return ShaderType::POINTS;
        case ComponentType::LineRendererComponent: return ShaderType::LINES;
        case ComponentType::SkyboxComponent: return ShaderType::SKYBOX;
        default: return ShaderType::MESH;
    }
}

bool isSafeRelative(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const fs::path& part : path.lexically_normal()) {
        if (part == "..") return false;
    }
    return true;
}

// "submeshes[3].material" → 3 (true), anything else false
bool submeshMaterialIndex(const std::string& name, unsigned int& index) {
    const std::string prefix = "submeshes[";
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    size_t pos = prefix.size();
    size_t end = pos;
    while (end < name.size() && std::isdigit(static_cast<unsigned char>(name[end]))) end++;
    if (end == pos || name.compare(end, std::string::npos, "].material") != 0) return false;
    index = static_cast<unsigned int>(std::stoul(name.substr(pos, end - pos)));
    return true;
}

bool isIntegral(const Json& value) {
    if (value.is_number_integer()) return true;
    if (value.is_number_float()) {
        const double v = value.get<double>();
        return std::isfinite(v) && std::floor(v) == v && std::fabs(v) < 9.0e15;
    }
    return false;
}

long long integralValue(const Json& value) {
    return value.is_number_integer() ? value.get<long long>() : static_cast<long long>(value.get<double>());
}

// ------------------------------------------------------------------ enum tables

using EnumOptions = McpValues::EnumOptions;

// The options of a component enum: its table's MCP tokens and labels, in the Inspector's order,
// hidden values left out (docs/specs/S08-reflection.md R5); `mask` keeps the values a field offers
const EnumOptions& tableOptions(const reflect::EnumDesc& desc, uint64_t mask = 0) {
    static std::mutex mutex;
    static std::map<std::pair<const reflect::EnumDesc*, uint64_t>, EnumOptions> tables;
    std::lock_guard<std::mutex> lock(mutex);
    EnumOptions& options = tables[{&desc, mask}];
    if (options.empty()) {
        for (const reflect::EnumItem* item : reflect::enums::offered(desc, mask)) {
            options.push_back({item->value, reflect::enumMcpToken(*item), item->label});
        }
    }
    return options;
}

// framebuffers have no mipmaps: their filters
constexpr uint64_t kFramebufferFilters = reflect::enums::bit((int)TextureFilter::NEAREST) | reflect::enums::bit((int)TextureFilter::LINEAR);

const EnumOptions& atmosphereQualityOptions() {
    static const EnumOptions options = {
        {(int)AtmosphereQuality::OFF, "off", "Off"},
        {(int)AtmosphereQuality::LOW, "low", "Low"},
        {(int)AtmosphereQuality::MEDIUM, "medium", "Medium"},
        {(int)AtmosphereQuality::HIGH, "high", "High"}
    };
    return options;
}

const EnumOptions& lightStateOptions() {
    static const EnumOptions options = {
        {(int)LightState::OFF, "off", "Off"},
        {(int)LightState::ON, "on", "On"},
        {(int)LightState::AUTO, "auto", "Auto"}
    };
    return options;
}

const EnumOptions& shadowQualityOptions() {
    static const EnumOptions options = {
        {(int)ShadowQuality::AUTO, "auto", "Auto"},
        {(int)ShadowQuality::NONE, "none", "Hard Only"},
        {(int)ShadowQuality::LOW, "low", "Low"},
        {(int)ShadowQuality::MEDIUM, "medium", "Medium"},
        {(int)ShadowQuality::HIGH, "high", "High"},
        {(int)ShadowQuality::ULTRA, "ultra", "Ultra"}
    };
    return options;
}

const EnumOptions& shadowDebugViewOptions() {
    static const EnumOptions options = {
        {(int)ShadowDebugView::NONE, "none", "None"},
        {(int)ShadowDebugView::CASCADES, "cascades", "Cascades"},
        {(int)ShadowDebugView::SHADOW_FACTOR, "shadow_factor", "Shadow Factor"},
        {(int)ShadowDebugView::TEXEL_DENSITY, "texel_density", "Texel Density"},
        {(int)ShadowDebugView::CACHE, "cache", "Cache"},
        {(int)ShadowDebugView::ATLAS, "atlas", "Atlas"}
    };
    return options;
}

const EnumOptions& shadowTierOptions() {
    static const EnumOptions options = {
        {(int)ShadowTier::LOW, "low", "Low"},
        {(int)ShadowTier::MEDIUM, "medium", "Medium"},
        {(int)ShadowTier::HIGH, "high", "High"},
        {(int)ShadowTier::ULTRA, "ultra", "Ultra"}
    };
    return options;
}

const EnumOptions& litQualityOptions() {
    static const EnumOptions options = {
        {(int)LitQuality::STANDARD, "standard", "Standard"},
        {(int)LitQuality::HIGH, "high", "High"}
    };
    return options;
}

const EnumOptions& sceneColorScaleOptions() {
    static const EnumOptions options = {
        {(int)SceneColorScale::FULL, "full", "Full"},
        {(int)SceneColorScale::HALF, "half", "Half"}
    };
    return options;
}

// ------------------------------------------------------------------ YAML paths

struct PathSegment {
    std::string key;     // map key, or
    long index = -1;     // sequence index
};

bool splitPath(const std::string& path, std::vector<PathSegment>& out) {
    out.clear();
    std::string current;
    for (size_t i = 0; i < path.size(); i++) {
        const char c = path[i];
        if (c == '.') {
            if (current.empty() && (out.empty() || out.back().index < 0)) return false;
            if (!current.empty()) out.push_back({current, -1});
            current.clear();
        } else if (c == '[') {
            if (!current.empty()) out.push_back({current, -1});
            current.clear();
            size_t end = path.find(']', i);
            if (end == std::string::npos || end == i + 1) return false;
            const std::string digits = path.substr(i + 1, end - i - 1);
            if (!std::all_of(digits.begin(), digits.end(), [](unsigned char d) { return std::isdigit(d); })) return false;
            out.push_back({std::string(), std::stol(digits)});
            i = end;
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back({current, -1});
    return !out.empty();
}

// A script entry ({path, properties: [{name, value...}]}): the property named `key`
YAML::Node scriptPropertyValue(const YAML::Node& node, const std::string& key) {
    if (!node.IsMap()) return YAML::Node(YAML::NodeType::Undefined);
    const YAML::Node properties = node["properties"];
    if (!properties || !properties.IsSequence()) return YAML::Node(YAML::NodeType::Undefined);
    for (const YAML::Node& property : properties) {
        const YAML::Node name = property["name"];
        if (name && name.IsScalar() && name.Scalar() == key) return property["value"];
    }
    return YAML::Node(YAML::NodeType::Undefined);
}

// The child of `node` for one segment (const lookup: never creates)
YAML::Node childOf(const YAML::Node& node, const PathSegment& segment) {
    if (segment.index >= 0) {
        if (!node.IsSequence() || segment.index >= static_cast<long>(node.size())) return YAML::Node(YAML::NodeType::Undefined);
        return node[static_cast<size_t>(segment.index)];
    }
    if (!node.IsMap()) return YAML::Node(YAML::NodeType::Undefined);
    const YAML::Node child = node[segment.key];
    if (child) return child;
    return scriptPropertyValue(node, segment.key);
}

std::string formatNumber(const Json& value) {
    if (value.is_number_integer() || value.is_number_unsigned()) return value.dump();
    const double v = value.get<double>();
    if (std::isfinite(v) && std::floor(v) == v && std::fabs(v) < 1.0e15) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.1f", v);
        return buffer;
    }
    return value.dump();
}

bool isCanonicalInteger(const std::string& text) {
    size_t i = text[0] == '-' ? 1 : 0;
    if (i >= text.size() || text.size() - i > 18) return false;
    if (text[i] == '0' && text.size() - i > 1) return false;
    for (; i < text.size(); i++) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    }
    return true;
}

bool isCanonicalDecimal(const std::string& text) {
    // -?(0|[1-9]\d*)(\.\d+)?([eE][-+]?\d+)? with a fraction or an exponent
    size_t i = text[0] == '-' ? 1 : 0;
    if (i >= text.size()) return false;
    const size_t intStart = i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) i++;
    if (i == intStart) return false;
    if (text[intStart] == '0' && i - intStart > 1) return false;
    bool decorated = false;
    if (i < text.size() && text[i] == '.') {
        const size_t fracStart = ++i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) i++;
        if (i == fracStart) return false;
        decorated = true;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        i++;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) i++;
        const size_t expStart = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) i++;
        if (i == expStart) return false;
        decorated = true;
    }
    return decorated && i == text.size();
}

// ------------------------------------------------------------------ texture / material keys

struct MaterialTextureField {
    const char* name;
    Texture Material::*texture;
    int Material::*texCoord;
    const char* texCoordName;
};

const MaterialTextureField MATERIAL_TEXTURES[] = {
    {"baseColorTexture", &Material::baseColorTexture, &Material::baseColorTexCoord, "baseColorTexCoord"},
    {"emissiveTexture", &Material::emissiveTexture, &Material::emissiveTexCoord, "emissiveTexCoord"},
    {"metallicRoughnessTexture", &Material::metallicRoughnessTexture, &Material::metallicRoughnessTexCoord, "metallicRoughnessTexCoord"},
    {"occlusionTexture", &Material::occlusionTexture, &Material::occlusionTexCoord, "occlusionTexCoord"},
    {"normalTexture", &Material::normalTexture, &Material::normalTexCoord, "normalTexCoord"}
};

bool textureFileExists(Project* project, const std::string& stored) {
    if (stored.empty()) return true;
    const fs::path clean = TextureData::parseSvgScalePath(stored);
    return fs::exists(project->resolveAssetPath(clean.string()));
}

// ------------------------------------------------------------------ YAML edits for set_properties

struct YamlEdit {
    std::string property;
    std::string op;
    Json value;
    long index;
    bool known;   // a Catalog (custom) property: its keys may be created
};

} // namespace

// ==================================================================== names

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
        case PropertyType::Asset: return "asset";
        case PropertyType::Shader: return "shader";
    }
    return "unknown";
}

// ==================================================================== enums

const McpValues::EnumOptions& McpValues::easeOptions() {
    // a custom curve is a script's function: MCP offers the named eases only
    return tableOptions(reflect::enums::kEaseType, ~reflect::enums::bit((int)EaseType::CUSTOM));
}

const McpValues::EnumOptions* McpValues::enumTable(const std::string& name) {
    static const std::map<std::string, const EnumOptions*> tables = {
        {"light_state", &lightStateOptions()},
        {"shadow_quality", &shadowQualityOptions()},
        {"shadow_debug_view", &shadowDebugViewOptions()},
        {"shadow_tier", &shadowTierOptions()},
        {"scene_color_scale", &sceneColorScaleOptions()},
        {"lit_quality", &litQualityOptions()},
        {"light_unit", &tableOptions(reflect::enums::kLightUnit)},
        {"texture_filter", &tableOptions(reflect::enums::kTextureFilter)},
        // framebuffers have no mipmaps
        {"framebuffer_filter", &tableOptions(reflect::enums::kTextureFilter, kFramebufferFilters)},
        {"texture_wrap", &tableOptions(reflect::enums::kTextureWrap)},
        {"texture_alpha_border", &tableOptions(reflect::enums::kTextureAlphaBorder)},
        {"material_alpha_mode", &tableOptions(reflect::enums::kMaterialAlphaMode)},
        {"material_blend_mode", &tableOptions(reflect::enums::kMaterialBlendMode)},
        {"material_cull_mode", &tableOptions(reflect::enums::kMaterialCullMode)},
        {"material_depth_write", &tableOptions(reflect::enums::kMaterialDepthWrite)},
        {"material_depth_test", &tableOptions(reflect::enums::kMaterialDepthTest)},
        {"material_lighting", &tableOptions(reflect::enums::kMaterialLighting)},
        {"material_stencil_compare", &tableOptions(reflect::enums::kMaterialStencilCompare)},
        {"material_stencil_op", &tableOptions(reflect::enums::kMaterialStencilOp)},
        {"material_pass_order", &tableOptions(reflect::enums::kMaterialPassOrder)},
        {"light_shading", &tableOptions(reflect::enums::kLightShading)},
        {"primitive_type", &tableOptions(reflect::enums::kPrimitiveType)},
        {"body_type", &tableOptions(reflect::enums::kBodyType)},
        {"collider2d_type", &tableOptions(reflect::enums::kCollider2DType)},
        {"collider3d_type", &tableOptions(reflect::enums::kCollider3DType)},
        {"light_type", &tableOptions(reflect::enums::kLightType)},
        {"camera_type", &tableOptions(reflect::enums::kCameraType)},
        {"lattice_interpolation", &tableOptions(reflect::enums::kLatticeInterpolation)},
        {"lattice_outside", &tableOptions(reflect::enums::kLatticeOutside)},
        {"lattice_jiggle_anchor", &tableOptions(reflect::enums::kLatticeJiggleAnchor)},
        {"lattice_stage", &tableOptions(reflect::enums::kLatticeStage)},
        {"lattice_mask", &tableOptions(reflect::enums::kLatticeMaskChannel)},
        {"atmosphere_quality", &atmosphereQualityOptions()},
        {"ease", &easeOptions()}
    };
    auto it = tables.find(name);
    return it != tables.end() ? it->second : nullptr;
}

const McpValues::EnumOptions* McpValues::enumOptions(ComponentType component, const std::string& propertyName) {
    // the field's enum table ("colliders[2].type": the colliders' type field)
    const reflect::ComponentDesc* desc = reflect::find(component);
    const reflect::FieldDesc* field = desc ? reflect::findField(*desc, propertyName) : nullptr;
    if (!field || !field->enumDesc) return nullptr;
    if (field->enumDesc == &reflect::enums::kEaseType && !field->enumMask) return &easeOptions();
    return &tableOptions(*field->enumDesc, field->enumMask);
}

Json McpValues::enumToJson(int value, const EnumOptions& options) {
    for (const EnumOption& option : options) {
        if (option.value == value) return option.name;
    }
    return value;
}

Json McpValues::enumNames(const EnumOptions& options) {
    Json names = Json::array();
    for (const EnumOption& option : options) names.push_back(option.name);
    return names;
}

bool McpValues::enumFromJson(const Json& value, const EnumOptions& options, int& out, std::string& error) {
    auto namesText = [&options]() {
        std::string text;
        for (const EnumOption& option : options) {
            if (!text.empty()) text += ", ";
            text += option.name;
        }
        return text;
    };
    if (isIntegral(value)) {
        const long long number = integralValue(value);
        for (const EnumOption& option : options) {
            if (option.value == number) {
                out = option.value;
                return true;
            }
        }
        error = "value " + std::to_string(number) + " is not one of: " + namesText() + ".";
        return false;
    }
    if (!value.is_string()) {
        error = "value must be one of: " + namesText() + ".";
        return false;
    }
    const std::string key = enumKey(value.get<std::string>());
    for (const EnumOption& option : options) {
        if (enumKey(option.name) == key || enumKey(option.label) == key) {
            out = option.value;
            return true;
        }
    }
    // scene-file aliases
    static const std::map<std::string, std::string> aliases = {
        {"ortho", "orthographic"}, {"perspec", "perspective"},
        {"alphaopaque", "opaque"}, {"none", "noattenuation"}, {"gameobjectmesh", "entitymesh"},
        // the manifest's @state stencil names (lequal, gequal, incr, decr)
        {"lequal", "lessequal"}, {"gequal", "greaterequal"}, {"incr", "incrclamp"}, {"decr", "decrclamp"},
        {"gameobjectheightfield", "entityheightfield"}, {"rackandpinon", "rackandpinion"}
    };
    auto alias = aliases.find(key);
    if (alias != aliases.end()) {
        for (const EnumOption& option : options) {
            if (enumKey(option.name) == alias->second) {
                out = option.value;
                return true;
            }
        }
    }
    // "ease_in_out_quad" / "EaseInOutQuad" (other engines' names) for "quad_in_out"
    if (key.compare(0, 4, "ease") == 0) {
        for (const EnumOption& option : options) {
            const std::string name = option.name;
            for (const char* direction : {"_in_out", "_in", "_out"}) {
                const size_t at = name.size() >= std::strlen(direction) ? name.size() - std::strlen(direction) : std::string::npos;
                if (at != std::string::npos && at > 0 && name.compare(at, std::string::npos, direction) == 0) {
                    if (key == "ease" + enumKey(direction) + enumKey(name.substr(0, at))) {
                        out = option.value;
                        return true;
                    }
                    break;
                }
            }
        }
    }
    error = "\"" + value.get<std::string>() + "\" is not one of: " + namesText() + ".";
    return false;
}

// ==================================================================== shapes

bool McpValues::parseVector2(const Json& value, Vector2& out) {
    float v[2] = {out.x, out.y};
    if (!readComponents(value, v, 2, XYZW)) return false;
    out = Vector2(v[0], v[1]);
    return true;
}

bool McpValues::parseVector3(const Json& value, Vector3& out) {
    float v[4] = {out.x, out.y, out.z, 1.0f};
    int count = 0;
    if (parseHexColor(value, v, count)) {
        if (count != 3) return false;
        out = Vector3(v[0], v[1], v[2]);
        return true;
    }
    if (value.is_object() && value.contains("r")) {
        if (!readComponents(value, v, 3, RGBA)) return false;
    } else if (!readComponents(value, v, 3, XYZW)) {
        return false;
    }
    out = Vector3(v[0], v[1], v[2]);
    return true;
}

bool McpValues::parseVector4(const Json& value, Vector4& out) {
    float v[4] = {out.x, out.y, out.z, out.w};
    int count = 0;
    if (parseHexColor(value, v, count)) {
        if (count == 3) v[3] = out.w;
        out = Vector4(v[0], v[1], v[2], v[3]);
        return true;
    }
    // A color without alpha keeps the current alpha
    if (value.is_array() && value.size() == 3) {
        for (int i = 0; i < 3; i++) {
            if (!value[i].is_number()) return false;
            v[i] = value[i].get<float>();
        }
        out = Vector4(v[0], v[1], v[2], v[3]);
        return true;
    }
    if (value.is_object() && value.contains("r")) {
        if (!readComponents(value, v, 4, RGBA)) return false;
    } else if (!readComponents(value, v, 4, XYZW)) {
        return false;
    }
    out = Vector4(v[0], v[1], v[2], v[3]);
    return true;
}

Vector3 McpValues::eulerDegrees(const Quaternion& value) {
    Vector3 euler = Quaternion(value).normalize().getEulerAngles(RotationOrder::ZYX);
    // the Inspector's display: tiny values are 0, near-whole angles snap
    float* parts[3] = {&euler.x, &euler.y, &euler.z};
    for (float* part : parts) {
        if (std::fabs(*part) < 1e-4f) *part = 0.0f;
        const float rounded = std::round(*part * 1000.0f) / 1000.0f;
        *part = rounded == 0.0f ? 0.0f : rounded;
    }
    return euler;
}

bool McpValues::parseQuaternion(const Json& value, Quaternion& out) {
    auto fromEuler = [](const Json& euler, Quaternion& q) {
        float e[3] = {0.0f, 0.0f, 0.0f};
        if (!readComponents(euler, e, 3, XYZW)) return false;
        q = Quaternion(e[0], e[1], e[2]);
        return true;
    };
    auto fromQuat = [](const Json& quat, Quaternion& q) {
        float v[4] = {q.x, q.y, q.z, q.w};
        if (!readComponents(quat, v, 4, XYZW)) return false;
        q = Quaternion(v[3], v[0], v[1], v[2]);
        return true;
    };
    if (value.is_object() && (value.contains("euler") || value.contains("quat"))) {
        const bool hasEuler = value.contains("euler");
        const bool hasQuat = value.contains("quat");
        Quaternion byEuler = out;
        Quaternion byQuat = out;
        if (hasEuler && !fromEuler(value["euler"], byEuler)) return false;
        if (hasQuat && !fromQuat(value["quat"], byQuat)) return false;
        if (hasEuler && hasQuat) {
            // a read value written back with one of the two edited: the one that moved wins
            const float dot = std::fabs(byQuat.x * out.x + byQuat.y * out.y + byQuat.z * out.z + byQuat.w * out.w);
            out = dot > 0.99999f ? byEuler : byQuat;
        } else {
            out = hasEuler ? byEuler : byQuat;
        }
        return true;
    }
    return fromQuat(value, out);
}

double McpValues::number(float value) {
    if (!std::isfinite(value)) return value;
    // Not std::to_chars(float): Apple's libc++ only has it from macOS 13.3. The first
    // precision that reads back as the same float is the shortest (%.9g always does).
    char buffer[32];
    for (int precision = 1; precision <= 9; precision++) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, static_cast<double>(value));
        if (std::strtof(buffer, nullptr) == value) break;
    }
    return std::strtod(buffer, nullptr);
}

Json McpValues::toJson(const Vector2& value) { return Json::array({number(value.x), number(value.y)}); }
Json McpValues::toJson(const Vector3& value) { return Json::array({number(value.x), number(value.y), number(value.z)}); }
Json McpValues::toJson(const Vector4& value) {
    return Json::array({number(value.x), number(value.y), number(value.z), number(value.w)});
}
Json McpValues::toJson(const Quaternion& value) {
    return Json{{"quat", Json::array({number(value.x), number(value.y), number(value.z), number(value.w)})},
                {"euler", toJson(eulerDegrees(value))}};
}

std::string McpValues::assetPathToJson(Project* project, const std::string& stored) {
    if (stored.empty() || !project) return FileUtils::pathToGenericUtf8(fs::path(stored));
    return FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(project->resolveAssetPath(stored)));
}

bool McpValues::assetPathFromJson(Project* project, const std::string& requested, std::string& stored, std::string& error) {
    if (requested.empty()) {
        stored.clear();
        return true;
    }
    // a GUID names the asset it is (S04)
    std::string projectRelative = requested;
    if (Guid::isValidText(requested)) {
        const AssetDatabase::Resolution found = project->getAssetDatabase().resolve(Guid::parse(requested), std::string(), AssetType::Unknown, false);
        if (!found.found()) {
            error = "No asset has the GUID " + requested + ".";
            return false;
        }
        projectRelative = found.path;
    }
    const fs::path relative = FileUtils::pathFromUtf8(projectRelative);
    if (!isSafeRelative(relative)) {
        error = "\"" + projectRelative + "\" must be a project-relative path (no '..', not absolute).";
        return false;
    }
    const fs::path full = project->getProjectPath() / relative;
    if (!project->isInsideAssetsPath(full)) {
        error = "\"" + projectRelative + "\" must be inside the assets directory (\"" +
                FileUtils::pathToGenericUtf8(project->getAssetsDir()) + "\").";
        return false;
    }
    stored = project->normalizeToAssetsRelative(full).generic_string();
    return true;
}

Json McpValues::entityToJson(SceneProject* scene, Entity entity) {
    if (entity == NULL_ENTITY) return nullptr;
    if (!scene || !scene->scene || !scene->scene->isEntityCreated(entity)) return Json{{"id", entity}, {"missing", true}};
    return McpRefs::entityJson(scene, entity);
}

bool McpValues::entityFromJson(SceneProject* scene, const Json& value, Entity& out, std::string& error) {
    if (value.is_null() || (isIntegral(value) && integralValue(value) == 0) ||
        (value.is_string() && value.get<std::string>().empty())) {
        out = NULL_ENTITY;
        return true;
    }
    out = McpRefs::resolveEntity(scene, value, error);
    return out != NULL_ENTITY;
}

Json McpValues::entityReferenceToJson(Project* project, SceneProject* scene, const EntityReference& value) {
    if (value.entity == NULL_ENTITY) return nullptr;
    if (value.sceneId == 0 || (scene && value.sceneId == scene->id)) {
        Json out = entityToJson(scene, value.entity);
        if (value.sceneId != 0 && out.is_object()) out["scene"] = value.sceneId;
        return out;
    }
    SceneProject* other = project ? project->getScene(value.sceneId) : nullptr;
    Json out = other && other->scene ? entityToJson(other, value.entity) : Json{{"id", value.entity}};
    out["scene"] = value.sceneId;
    return out;
}

bool McpValues::entityReferenceFromJson(Project* project, SceneProject* scene, const Json& value, EntityReference& out,
                                        std::string& error) {
    out = EntityReference{};
    if (value.is_null()) return true;
    SceneProject* target = scene;
    Json ref = value;
    const bool explicitScene = value.is_object() && value.contains("scene") && !value["scene"].is_null();
    if (explicitScene) {
        target = McpRefs::resolveScene(project, value["scene"], error);
        if (!target) return false;
        ref.erase("scene");
        if (ref.contains("entity")) ref = ref["entity"];
    } else if (value.is_object() && value.contains("entity")) {
        ref = value["entity"];
    }
    Entity entity = NULL_ENTITY;
    if (!entityFromJson(target, ref, entity, error)) return false;
    out.entity = entity;
    // the same scene is 0, unless named explicitly (as a read of such a reference shows it)
    out.sceneId = (entity == NULL_ENTITY || (target == scene && !explicitScene)) ? 0 : target->id;
    return true;
}

Json McpValues::textureToJson(Project* project, SceneProject* scene, const Texture& texture) {
    const Entity camera = CameraTextureLink::parse(texture.getId());
    if (camera != NULL_ENTITY) {
        return Json{{"camera", entityToJson(scene, camera)}};
    }
    const Entity fluid = FluidTextureLink::parse(texture.getId());
    if (fluid != NULL_ENTITY) {
        return Json{{"fluid", entityToJson(scene, fluid)}};
    }
    if (texture.empty() || texture.isFramebuffer()) return nullptr;

    static const Texture defaults;
    const bool defaultSampler = texture.getMinFilter() == defaults.getMinFilter() &&
                                texture.getMagFilter() == defaults.getMagFilter() &&
                                texture.getWrapU() == defaults.getWrapU() &&
                                texture.getWrapV() == defaults.getWrapV() &&
                                texture.getAlphaBorder() == defaults.getAlphaBorder();
    const bool cube = texture.isCubeMap() || texture.getNumFaces() == 6;

    Json out = Json::object();
    if (!cube) {
        const std::string path = texture.getPath(0);
        if (path.empty()) {
            out["id"] = texture.getId();   // embedded data: not settable, kept on write
        } else {
            const std::string relative = assetPathToJson(project, path);
            if (defaultSampler) return TextureData::buildSvgScalePath(relative, texture.getSvgScale());
            out["path"] = relative;
            if (texture.getSvgScale() != 1.0f) out["svg_scale"] = number(texture.getSvgScale());
        }
    } else {
        bool anyOtherFace = false;
        for (size_t face = 1; face < 6; face++) {
            if (!texture.getPath(face).empty()) anyOtherFace = true;
        }
        if (!anyOtherFace && !texture.getPath(0).empty()) {
            out["cube"] = assetPathToJson(project, texture.getPath(0));
        } else {
            // front, back, left, right, up, down: the order a write takes (setCubePaths)
            Json faces = Json::array();
            for (size_t face : {4, 5, 1, 0, 2, 3}) faces.push_back(assetPathToJson(project, texture.getPath(face)));
            out["cube"] = faces;
        }
    }
    out["min_filter"] = enumToJson((int)texture.getMinFilter(), tableOptions(reflect::enums::kTextureFilter));
    out["mag_filter"] = enumToJson((int)texture.getMagFilter(), tableOptions(reflect::enums::kTextureFilter));
    out["wrap_u"] = enumToJson((int)texture.getWrapU(), tableOptions(reflect::enums::kTextureWrap));
    out["wrap_v"] = enumToJson((int)texture.getWrapV(), tableOptions(reflect::enums::kTextureWrap));
    out["alpha_border"] = enumToJson((int)texture.getAlphaBorder(), tableOptions(reflect::enums::kTextureAlphaBorder));
    return out;
}

bool McpValues::textureFromJson(Project* project, SceneProject* scene, const Json& value, Texture& out,
                                Entity& camera, std::string& error) {
    camera = NULL_ENTITY;
    auto storedPath = [&](const Json& path, std::string& stored) {
        if (!path.is_string()) {
            error = "texture paths must be strings (project-relative).";
            return false;
        }
        float scale = 1.0f;
        const std::string clean = TextureData::parseSvgScalePath(path.get<std::string>(), &scale);
        if (!assetPathFromJson(project, clean, stored, error)) return false;
        if (!stored.empty() && !textureFileExists(project, stored)) {
            error = "texture file \"" + clean + "\" does not exist.";
            return false;
        }
        if (scale != 1.0f && !stored.empty()) stored = TextureData::buildSvgScalePath(stored, scale);
        return true;
    };

    if (value.is_null() || (value.is_string() && value.get<std::string>().empty())) {
        out = Texture();
        return true;
    }
    // a link id as a scene file keeps it: "camera|<entity id>", "fluid|<entity id>"
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (text.rfind("camera|", 0) == 0 || text.rfind("fluid|", 0) == 0) {
            const Entity linked = CameraTextureLink::parseLink(text);
            if (linked == NULL_ENTITY) {
                error = "\"" + text + "\" is not a texture link; one is \"camera|<entity id>\" or \"fluid|<entity id>\".";
                return false;
            }
            Json link = Json::object();
            link[text[0] == 'c' ? "camera" : "fluid"] = linked;
            return textureFromJson(project, scene, link, out, camera, error);
        }
    }
    if (value.is_string()) {
        std::string stored;
        if (!storedPath(value, stored)) return false;
        out = Texture(stored);
        return true;
    }
    if (!value.is_object()) {
        error = "a texture is null, a project-relative path, {\"path\": ..., \"min_filter\": ...}, "
                "{\"cube\": [front, back, left, right, up, down] or \"path\"}, {\"camera\": <entity>} or {\"fluid\": <entity>}.";
        return false;
    }
    static const std::set<std::string> keys = {"path", "cube", "camera", "fluid", "id", "min_filter", "mag_filter", "wrap_u",
                                               "wrap_v", "alpha_border", "svg_scale"};
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!keys.count(it.key())) {
            error = "unknown texture key \"" + it.key() + "\"; valid: path, cube, camera, fluid, min_filter, mag_filter, "
                    "wrap_u, wrap_v, alpha_border, svg_scale.";
            return false;
        }
    }
    if (value.contains("fluid")) {
        if (!scene || !scene->scene) {
            error = "a fluid texture needs a loaded scene.";
            return false;
        }
        Entity fluidEntity = NULL_ENTITY;
        if (!entityFromJson(scene, value["fluid"], fluidEntity, error)) return false;
        if (fluidEntity == NULL_ENTITY || !scene->scene->isEntityCreated(fluidEntity) ||
            !scene->scene->findComponent<FluidSimulationComponent>(fluidEntity)) {
            error = "the fluid of a fluid texture must be a GameObject with a FluidSimulation component.";
            return false;
        }
        out = FluidTextureLink::make(scene->scene, fluidEntity);
        return true;
    }
    if (value.contains("camera")) {
        if (!scene || !scene->scene) {
            error = "a camera texture needs a loaded scene.";
            return false;
        }
        Entity cameraEntity = NULL_ENTITY;
        if (!entityFromJson(scene, value["camera"], cameraEntity, error)) return false;
        if (cameraEntity == NULL_ENTITY || !scene->scene->findComponent<CameraComponent>(cameraEntity)) {
            error = "the camera of a render texture must be a GameObject with a Camera component.";
            return false;
        }
        out = CameraTextureLink::make(scene->scene, cameraEntity);
        camera = cameraEntity;
        return true;
    }
    if (value.contains("path")) {
        std::string stored;
        if (!storedPath(value["path"], stored)) return false;
        out = Texture(stored);
    } else if (value.contains("cube")) {
        const Json& cube = value["cube"];
        out = Texture();
        if (cube.is_string()) {
            std::string stored;
            if (!storedPath(cube, stored)) return false;
            out.setCubeMap(stored);
        } else if (cube.is_array() && cube.size() == 6) {
            std::string faces[6];
            for (size_t face = 0; face < 6; face++) {
                if (!storedPath(cube[face], faces[face])) return false;
            }
            out.setCubePaths(faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]);
        } else {
            error = "cube must be one cube map image path or 6 face paths (front, back, left, right, up, down).";
            return false;
        }
    } else if (value.contains("id")) {
        error = "an embedded texture (\"id\") can't be assigned; give an image path instead.";
        return false;
    } else {
        out = Texture();
    }
    auto applyEnum = [&](const char* key, const EnumOptions& options, auto setter) {
        if (!value.contains(key)) return true;
        int parsed = 0;
        std::string why;
        if (!enumFromJson(value[key], options, parsed, why)) {
            error = std::string(key) + ": " + why;
            return false;
        }
        setter(parsed);
        return true;
    };
    if (!applyEnum("min_filter", tableOptions(reflect::enums::kTextureFilter), [&](int v) { out.setMinFilter(static_cast<TextureFilter>(v)); })) return false;
    if (!applyEnum("mag_filter", tableOptions(reflect::enums::kTextureFilter, kFramebufferFilters), [&](int v) { out.setMagFilter(static_cast<TextureFilter>(v)); })) return false;
    if (!applyEnum("wrap_u", tableOptions(reflect::enums::kTextureWrap), [&](int v) { out.setWrapU(static_cast<TextureWrap>(v)); })) return false;
    if (!applyEnum("wrap_v", tableOptions(reflect::enums::kTextureWrap), [&](int v) { out.setWrapV(static_cast<TextureWrap>(v)); })) return false;
    if (!applyEnum("alpha_border", tableOptions(reflect::enums::kTextureAlphaBorder), [&](int v) { out.setAlphaBorder(static_cast<TextureAlphaBorder>(v)); })) return false;
    if (value.contains("svg_scale")) {
        if (!value["svg_scale"].is_number() || !(value["svg_scale"].get<float>() > 0.0f)) {
            error = "svg_scale must be a number > 0.";
            return false;
        }
        out.setSvgScale(value["svg_scale"].get<float>());
    }
    return true;
}

Json McpValues::materialToJson(Project* project, SceneProject* scene, const Material& material, const std::string& linkedFile) {
    Json out = {
        {"name", material.name},
        {"baseColorFactor", toJson(material.baseColorFactor)},
        {"metallicFactor", number(material.metallicFactor)},
        {"roughnessFactor", number(material.roughnessFactor)},
        {"alphaCutoff", number(material.alphaCutoff)},
        {"foliageTransmission", number(material.foliageTransmission)},
        {"alphaMode", enumToJson((int)material.alphaMode, tableOptions(reflect::enums::kMaterialAlphaMode))},
        {"emissiveFactor", toJson(material.emissiveFactor)}
    };
    materialStochasticToJson(material, out);
    for (const MaterialTextureField& field : MATERIAL_TEXTURES) {
        const Texture& texture = material.*field.texture;
        Json json = textureToJson(project, scene, texture);
        if (!json.is_null()) out[field.name] = json;
        if (material.*field.texCoord != 0) out[field.texCoordName] = material.*field.texCoord;
    }
    // shader library: the shader and the render state always (they say how it draws), its
    // values when set
    materialShaderLibraryToJson(project, scene, material, out, false);
    // the built-in Lit's emission intensity and its extended lobes (those set)
    out["emissiveStrength"] = number(material.emissiveStrength);
    if (material.pbr != MaterialPbrExtensions()) out["pbr"] = materialPbrToJson(project, scene, material.pbr);
    if (!linkedFile.empty()) {
        out["file"] = project ? FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(project->getProjectPath() / linkedFile))
                              : FileUtils::pathToGenericUtf8(fs::path(linkedFile));
    }
    return out;
}

void McpValues::materialStochasticToJson(const Material& material, Json& out) {
    out["stochasticTiling"] = material.stochasticTiling;
    // its values while it is on, or when set away from their defaults
    static const Material defaults = Material();
    for (const StochasticMaterialValue& param : stochasticMaterialValues) {
        if (material.stochasticTiling || material.*param.field != defaults.*param.field)
            out[param.name] = number(material.*param.field);
    }
}

bool McpValues::isMaterialStochasticKey(const std::string& key) {
    return key == "stochasticTiling" || findStochasticMaterialValue(key) != nullptr;
}

bool McpValues::materialStochasticFromJson(const Json& value, Material& out, std::string& error) {
    if (value.contains("stochasticTiling")) {
        if (!value["stochasticTiling"].is_boolean()) {
            error = "stochasticTiling must be true or false.";
            return false;
        }
        out.stochasticTiling = value["stochasticTiling"].get<bool>();
    }
    for (const StochasticMaterialValue& param : stochasticMaterialValues) {
        if (!value.contains(param.name)) continue;
        if (!checkStochasticMaterialValue(param, param.name, value[param.name], error)) return false;
        out.*param.field = value[param.name].get<float>();
    }
    return true;
}

bool McpValues::materialFromJson(Project* project, SceneProject* scene, const Json& value, Material& out, std::string& error) {
    if (!value.is_object()) {
        error = "a material is an object of its fields, a .material path, or null.";
        return false;
    }
    auto number = [&](const char* key, float& target) {
        if (!value.contains(key)) return true;
        if (!value[key].is_number()) {
            error = std::string(key) + " must be a number.";
            return false;
        }
        target = value[key].get<float>();
        return true;
    };
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& key = it.key();
        bool known = key == "name" || key == "baseColorFactor" || key == "metallicFactor" || key == "roughnessFactor" ||
                     key == "alphaCutoff" || key == "foliageTransmission" || key == "alphaMode" ||
                     key == "emissiveFactor" || key == "emissiveStrength" || key == "pbr" || key == "file" ||
                     isMaterialStochasticKey(key);
        for (const MaterialTextureField& field : MATERIAL_TEXTURES) {
            if (key == field.name || key == field.texCoordName) known = true;
        }
        known = known || isMaterialShaderLibraryKey(key);
        if (!known) {
            error = "unknown material key \"" + key + "\"; valid: name, baseColorFactor, metallicFactor, roughnessFactor, "
                    "alphaCutoff, foliageTransmission, alphaMode, emissiveFactor, emissiveStrength, pbr, baseColorTexture, emissiveTexture, "
                    "metallicRoughnessTexture, occlusionTexture, normalTexture, <texture>TexCoord, stochasticTiling, "
                    "stochasticCellSize, stochasticBlendSharpness, stochasticContrast, stochasticRotation, shader, shaderParams, "
                    "shaderTextures, shaderFeatures, blendMode, cullMode, depthWrite, depthTest, depthOffsetFactor, "
                    "depthOffsetUnits, sortPriority, lighting, stencil, colorWrite, extraPass, file.";
            return false;
        }
    }
    if (value.contains("name")) {
        if (!value["name"].is_string()) {
            error = "name must be a string.";
            return false;
        }
        out.name = value["name"].get<std::string>();
    }
    if (value.contains("baseColorFactor") && !parseVector4(value["baseColorFactor"], out.baseColorFactor)) {
        error = "baseColorFactor must be [r,g,b,a] (linear, 0-1) or \"#RRGGBBAA\".";
        return false;
    }
    if (value.contains("emissiveFactor") && !parseVector3(value["emissiveFactor"], out.emissiveFactor)) {
        error = "emissiveFactor must be [r,g,b].";
        return false;
    }
    if (!number("metallicFactor", out.metallicFactor) || !number("roughnessFactor", out.roughnessFactor) ||
        !number("alphaCutoff", out.alphaCutoff) || !number("foliageTransmission", out.foliageTransmission)) {
        return false;
    }
    if (!materialStochasticFromJson(value, out, error)) {
        return false;
    }
    if (value.contains("emissiveStrength")) {
        if (!value["emissiveStrength"].is_number() || !(value["emissiveStrength"].get<float>() >= 0.0f) ||
                !std::isfinite(value["emissiveStrength"].get<float>())) {
            error = "emissiveStrength must be a number >= 0.";
            return false;
        }
        out.emissiveStrength = value["emissiveStrength"].get<float>();
    }
    if (value.contains("pbr")) {
        MaterialPbrExtensions pbr = out.pbr;
        if (!materialPbrFromJson(project, scene, value["pbr"], pbr, error)) return false;
        out.pbr = pbr;
    }
    if (value.contains("alphaMode")) {
        int mode = 0;
        std::string why;
        if (!enumFromJson(value["alphaMode"], tableOptions(reflect::enums::kMaterialAlphaMode), mode, why)) {
            error = "alphaMode: " + why;
            return false;
        }
        out.alphaMode = static_cast<MaterialAlphaMode>(mode);
    }
    for (const MaterialTextureField& field : MATERIAL_TEXTURES) {
        if (value.contains(field.name)) {
            Entity camera = NULL_ENTITY;
            std::string why;
            if (!textureFromJson(project, scene, value[field.name], out.*field.texture, camera, why)) {
                error = std::string(field.name) + ": " + why;
                return false;
            }
        }
        if (value.contains(field.texCoordName)) {
            if (!isIntegral(value[field.texCoordName])) {
                error = std::string(field.texCoordName) + " must be 0 or 1.";
                return false;
            }
            out.*field.texCoord = static_cast<int>(integralValue(value[field.texCoordName]));
        }
    }
    return materialShaderLibraryFromJson(project, scene, value, out, error);
}

Json McpValues::materialPbrToJson(Project* project, SceneProject* scene, const MaterialPbrExtensions& pbr) {
    static const MaterialPbrExtensions defaults;
    MaterialPbrExtensions& values = const_cast<MaterialPbrExtensions&>(pbr);
    Json out = Json::object();
    for (const PbrLobeInfo& lobe : MaterialPbr::getLobes()) {
        bool changed = false;
        Json fields = Json::object();
        for (const PbrField& field : MaterialPbr::getFields()) {
            if (field.lobe != lobe.lobe) continue;
            const void* value = field.ref(values);
            const void* def = field.ref(const_cast<MaterialPbrExtensions&>(defaults));
            switch (field.type) {
                case PbrFieldType::Bool:
                    fields[field.name] = *(const bool*)value;
                    changed = changed || *(const bool*)value != *(const bool*)def;
                    break;
                case PbrFieldType::Float:
                    // an infinite attenuation distance (no absorption) reads as null
                    fields[field.name] = std::isfinite(*(const float*)value) ? Json(number(*(const float*)value)) : Json(nullptr);
                    changed = changed || *(const float*)value != *(const float*)def;
                    break;
                case PbrFieldType::Vector2:
                    fields[field.name] = toJson(*(const Vector2*)value);
                    changed = changed || *(const Vector2*)value != *(const Vector2*)def;
                    break;
                case PbrFieldType::Color:
                    fields[field.name] = toJson(*(const Vector3*)value);
                    changed = changed || *(const Vector3*)value != *(const Vector3*)def;
                    break;
                case PbrFieldType::Texture:
                    fields[field.name] = textureToJson(project, scene, *(const Texture*)value);
                    changed = changed || !((const Texture*)value)->empty();
                    break;
                case PbrFieldType::TexCoord:
                    fields[field.name] = *(const int*)value;
                    changed = changed || *(const int*)value != *(const int*)def;
                    break;
            }
        }
        if (changed) out[lobe.name] = fields;
    }
    return out;
}

bool McpValues::materialPbrFromJson(Project* project, SceneProject* scene, const Json& value, MaterialPbrExtensions& out, std::string& error) {
    if (value.is_null()) {
        out = MaterialPbrExtensions();
        return true;
    }
    if (!value.is_object()) {
        error = "pbr is an object of lobes: clearcoat, sheen, anisotropy, iridescence, transmission, volume, specular, hex "
                "(each {field: value}, partial values merge), or null for none.";
        return false;
    }
    for (auto lobeIt = value.begin(); lobeIt != value.end(); ++lobeIt) {
        const PbrLobeInfo* lobe = MaterialPbr::findLobe(lobeIt.key());
        if (!lobe) {
            error = "pbr: unknown lobe "" + lobeIt.key() + ""; valid: clearcoat, sheen, anisotropy, iridescence, transmission, "
                    "volume, specular, hex.";
            return false;
        }
        if (!lobeIt.value().is_object()) {
            error = "pbr." + lobeIt.key() + " must be an object of its fields.";
            return false;
        }
        for (auto fieldIt = lobeIt.value().begin(); fieldIt != lobeIt.value().end(); ++fieldIt) {
            const PbrField* field = MaterialPbr::findField(lobe->lobe, fieldIt.key());
            const std::string path = "pbr." + lobeIt.key() + "." + fieldIt.key();
            if (!field) {
                std::string names;
                for (const PbrField& candidate : MaterialPbr::getFields()) {
                    if (candidate.lobe == lobe->lobe) names += (names.empty() ? "" : ", ") + std::string(candidate.name);
                }
                error = path + ": unknown field; " + lobeIt.key() + " has " + names + ".";
                return false;
            }
            const Json& item = fieldIt.value();
            void* target = field->ref(out);
            auto numberText = [](float number) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%g", number);
                return std::string(buffer);
            };
            auto checked = [&](float number) {
                if (!MaterialPbr::isInRange(*field, number)) {
                    const std::string range = std::isinf(field->maxValue) ? (">= " + numberText(field->minValue))
                                                                          : (numberText(field->minValue) + " to " + numberText(field->maxValue));
                    error = path + ": " + numberText(number) + " is out of range (" + MaterialPbr::getFieldLabel(*field) + ": " + range + ").";
                    return false;
                }
                return true;
            };
            switch (field->type) {
                case PbrFieldType::Bool:
                    if (!item.is_boolean()) { error = path + " must be true or false."; return false; }
                    *(bool*)target = item.get<bool>();
                    break;
                case PbrFieldType::Float:
                    // null: the attenuation distance's "no absorption"
                    if (item.is_null() && std::isinf(field->maxValue) && std::string(field->name) == "attenuationDistance") {
                        *(float*)target = INFINITY;
                        break;
                    }
                    if (!item.is_number()) { error = path + " must be a number."; return false; }
                    if (!checked(item.get<float>())) return false;
                    *(float*)target = item.get<float>();
                    break;
                case PbrFieldType::Vector2: {
                    Vector2 v = *(Vector2*)target;
                    if (!parseVector2(item, v)) { error = path + " must be [x,y]."; return false; }
                    if (!checked(v.x) || !checked(v.y)) return false;
                    *(Vector2*)target = v;
                    break;
                }
                case PbrFieldType::Color: {
                    Vector3 v = *(Vector3*)target;
                    if (!parseVector3(item, v)) { error = path + " must be [r,g,b] (linear) or \"#RRGGBB\"."; return false; }
                    if (!checked(v.x) || !checked(v.y) || !checked(v.z)) return false;
                    *(Vector3*)target = v;
                    break;
                }
                case PbrFieldType::Texture: {
                    Entity camera = NULL_ENTITY;
                    std::string why;
                    if (!textureFromJson(project, scene, item, *(Texture*)target, camera, why)) {
                        error = path + ": " + why;
                        return false;
                    }
                    break;
                }
                case PbrFieldType::TexCoord:
                    if (!isIntegral(item) || (integralValue(item) != 0 && integralValue(item) != 1)) {
                        error = path + " must be 0 or 1 (the UV set).";
                        return false;
                    }
                    *(int*)target = (int)integralValue(item);
                    break;
            }
        }
    }
    return true;
}

Json McpValues::shaderUniformsToJson(const ShaderUniformValues& values) {
    Json out = Json::object();
    for (const auto& uniform : values) out[uniform.first] = toJson(uniform.second);
    return out;
}

bool McpValues::shaderUniformsFromJson(const Json& value, ShaderUniformValues& out, std::string& error) {
    out.clear();
    if (value.is_null()) return true;
    if (!value.is_object()) {
        error = "uniforms are an object {name: [x,y,z,w]} (a number or 1-3 numbers fill the rest with 0).";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (ShaderUniforms::isReserved(it.key())) {
            error = "\"" + it.key() + "\" is set by the engine every frame (reserved).";
            return false;
        }
        float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const Json& item = it.value();
        if (item.is_number()) {
            v[0] = item.get<float>();
        } else if (item.is_array() && !item.empty() && item.size() <= 4) {
            for (size_t i = 0; i < item.size(); i++) {
                if (!item[i].is_number()) {
                    error = "uniform \"" + it.key() + "\" must be a number or up to 4 numbers.";
                    return false;
                }
                v[i] = item[i].get<float>();
            }
        } else {
            error = "uniform \"" + it.key() + "\" must be a number or up to 4 numbers.";
            return false;
        }
        out.push_back({it.key(), Vector4(v[0], v[1], v[2], v[3])});
    }
    return true;
}

// ==================================================================== YAML <-> JSON

Json McpValues::yamlToJson(const YAML::Node& node) {
    if (!node.IsDefined()) return nullptr;
    switch (node.Type()) {
        case YAML::NodeType::Null:
        case YAML::NodeType::Undefined:
            return nullptr;
        case YAML::NodeType::Scalar: {
            const std::string& text = node.Scalar();
            if (text.empty()) return text;
            if (text == "true") return true;
            if (text == "false") return false;
            if (isCanonicalInteger(text)) {
                try {
                    return std::stoll(text);
                } catch (const std::exception&) {
                    return text;
                }
            }
            if (isCanonicalDecimal(text)) {
                try {
                    return std::stod(text);
                } catch (const std::exception&) {
                    return text;
                }
            }
            return text;
        }
        case YAML::NodeType::Sequence: {
            Json out = Json::array();
            for (const YAML::Node& item : node) out.push_back(yamlToJson(item));
            return out;
        }
        case YAML::NodeType::Map: {
            Json out = Json::object();
            for (const auto& item : node) {
                out[item.first.Scalar()] = yamlToJson(item.second);
            }
            return out;
        }
    }
    return nullptr;
}

YAML::Node McpValues::jsonToYaml(const Json& value) {
    if (value.is_null()) return YAML::Node(YAML::NodeType::Null);
    if (value.is_boolean()) return YAML::Node(value.get<bool>() ? "true" : "false");
    if (value.is_number()) return YAML::Node(formatNumber(value));
    if (value.is_string()) return YAML::Node(value.get<std::string>());
    if (value.is_array()) {
        YAML::Node out(YAML::NodeType::Sequence);
        for (const Json& item : value) out.push_back(jsonToYaml(item));
        return out;
    }
    YAML::Node out(YAML::NodeType::Map);
    for (auto it = value.begin(); it != value.end(); ++it) out[it.key()] = jsonToYaml(it.value());
    return out;
}

YAML::Node McpValues::findYamlPath(const YAML::Node& root, const std::string& property) {
    if (!root || !root.IsMap()) return YAML::Node(YAML::NodeType::Undefined);
    // a flat key holding dots ("spriteInitializer.frames") first
    const YAML::Node flat = root[property];
    if (flat) return flat;
    std::vector<PathSegment> segments;
    if (!splitPath(property, segments)) return YAML::Node(YAML::NodeType::Undefined);
    // reset() rebinds; operator= would write the child's content over the parent
    YAML::Node current;
    current.reset(root);
    for (const PathSegment& segment : segments) {
        YAML::Node child = childOf(current, segment);
        if (!child) return YAML::Node(YAML::NodeType::Undefined);
        current.reset(child);
    }
    return current;
}

bool McpValues::editYamlPath(YAML::Node root, const std::string& property, const std::string& op, const Json& value,
                             long index, std::string& error, bool create) {
    YAML::Node target = findYamlPath(root, property);
    if (!target && create && root.IsMap()) {
        // a property the encoder left out (empty): make its map keys
        std::vector<PathSegment> segments;
        if (splitPath(property, segments)) {
            YAML::Node current;
            current.reset(root);
            bool made = true;
            for (const PathSegment& segment : segments) {
                if (segment.index >= 0 || !(current.IsMap() || current.IsNull())) {
                    made = false;
                    break;
                }
                if (!static_cast<const YAML::Node&>(current)[segment.key]) {
                    current[segment.key] = YAML::Node(YAML::NodeType::Null);
                }
                YAML::Node child = current[segment.key];
                current.reset(child);
            }
            if (made) target.reset(findYamlPath(root, property));
        }
    }
    if (!target) {
        error = "\"" + property + "\" is not in the component's encoding.";
        return false;
    }
    if (op == "set") {
        target = jsonToYaml(value);
        return true;
    }
    if (!target.IsSequence() && !(target.IsNull() && op == "insert")) {
        error = "\"" + property + "\" is not a list: insert/remove only work on lists.";
        return false;
    }
    const long size = target.IsSequence() ? static_cast<long>(target.size()) : 0;
    YAML::Node rebuilt(YAML::NodeType::Sequence);
    if (op == "insert") {
        const long at = index < 0 ? size : index;
        if (at > size) {
            error = "insert index " + std::to_string(at) + " is past the end of \"" + property + "\" (" +
                    std::to_string(size) + " items).";
            return false;
        }
        for (long i = 0; i <= size; i++) {
            if (i == at) rebuilt.push_back(jsonToYaml(value));
            if (i < size) rebuilt.push_back(YAML::Clone(target[static_cast<size_t>(i)]));
        }
    } else if (op == "remove") {
        if (index < 0 || index >= size) {
            error = "remove index " + std::to_string(index) + " is out of range for \"" + property + "\" (" +
                    std::to_string(size) + " items).";
            return false;
        }
        for (long i = 0; i < size; i++) {
            if (i != index) rebuilt.push_back(YAML::Clone(target[static_cast<size_t>(i)]));
        }
    } else {
        error = "unknown op \"" + op + "\" (set, insert, remove).";
        return false;
    }
    target = rebuilt;
    return true;
}

namespace {

// A list whose length the encoding also stores beside it ("colliders" / "numColliders",
// "framesRect" / "numFramesRect", "tiles" / "numTiles"...): the count follows the list
void syncListCount(YAML::Node root, const std::string& property) {
    std::vector<PathSegment> segments;
    if (!splitPath(property, segments) || segments.back().index >= 0) return;
    const std::string key = segments.back().key;
    YAML::Node parent;
    if (segments.size() == 1) {
        parent.reset(root);
    } else {
        const size_t cut = property.size() - key.size() - 1;
        parent.reset(McpValues::findYamlPath(root, property.substr(0, cut)));
    }
    if (!parent || !parent.IsMap()) return;
    const YAML::Node list = static_cast<const YAML::Node&>(parent)[key];
    if (!list || !(list.IsSequence() || list.IsNull())) return;
    std::string countKey = "num" + key;
    countKey[3] = static_cast<char>(std::toupper(static_cast<unsigned char>(countKey[3])));
    if (!static_cast<const YAML::Node&>(parent)[countKey]) return;
    parent[countKey] = std::to_string(list.IsSequence() ? list.size() : 0);
}

} // namespace

// ==================================================================== reading

Json McpValues::readYamlProperty(ReadContext& ctx, const std::string& name, bool& found) {
    found = false;
    if (!ctx.scene || !ctx.scene->scene || !ctx.hasComponent) return nullptr;
    std::string collection, key;
    if (Catalog::splitShaderEntryProperty(name, collection, key)) {
        // one shader-library value: as set, else the manifest default (textures: null)
        const PropertyData property = Catalog::findProperty(ctx.scene->scene, ctx.entity, ctx.component, collection);
        if (!property.ref) return nullptr;
        found = true;
        if (endsWith(collection, "shaderTextures")) {
            for (const auto& [uniform, texture] : *static_cast<std::vector<std::pair<std::string, Texture>>*>(property.ref)) {
                if (uniform == key) return textureToJson(ctx.project, ctx.scene, texture);
            }
            return nullptr;
        }
        const ShaderUniformValues& values = *static_cast<ShaderUniformValues*>(property.ref);
        const std::string shader = Catalog::getShaderReferenceForProperty(ctx.scene->scene, ctx.entity, ctx.component, collection);
        return toJson(ShaderLibraryValues::getParam(values, shader, key));
    }
    if (!ctx.yamlLoaded) {
        ctx.componentYaml = EntityClipboard::encodeComponent(ctx.scene->scene, ctx.entity, ctx.component);
        ctx.yamlLoaded = true;
    }
    const YAML::Node node = findYamlPath(ctx.componentYaml, name);
    if (!node) return nullptr;
    found = true;
    return yamlToJson(node);
}

Json McpValues::readProperty(ReadContext& ctx, const std::string& name, const PropertyData& property, bool readDefault) {
    void* ref = readDefault ? property.def : property.ref;
    if (property.type == PropertyType::Custom) {
        if (readDefault) return nullptr;
        if (ref && name == "shaderUniforms") {
            return shaderUniformsToJson(*static_cast<ShaderUniformValues*>(ref));
        }
        // shader library collections (a submesh material's, or a component's)
        if (ref && endsWith(name, "shaderParams")) {
            return shaderParamsToJson(*static_cast<ShaderUniformValues*>(ref));
        }
        if (ref && endsWith(name, "shaderTextures")) {
            return shaderTexturesToJson(ctx.project, ctx.scene, *static_cast<std::vector<std::pair<std::string, Texture>>*>(ref));
        }
        if (ref && endsWith(name, "shaderFeatures")) {
            return shaderFeaturesToJson(*static_cast<std::vector<std::string>*>(ref));
        }
        // phase 2: a stencil block (material, extra pass, component) and a material's extra pass
        if (ref && (name == "stencil" || endsWith(name, ".stencil"))) {
            return stencilToJson(*static_cast<MaterialStencil*>(ref));
        }
        if (ref && endsWith(name, "material.extraPass")) {
            const MaterialPass& pass = *static_cast<MaterialPass*>(ref);
            return pass == MaterialPass() ? Json(nullptr) : materialPassToJson(ctx.project, ctx.scene, pass, true);
        }
        bool found = false;
        return readYamlProperty(ctx, name, found);
    }
    if (!ref) return nullptr;
    switch (property.type) {
        case PropertyType::Bool:
            return *static_cast<bool*>(ref);
        case PropertyType::String: {
            const std::string& value = *static_cast<std::string*>(ref);
            return isAssetPathProperty(name) ? assetPathToJson(ctx.project, value) : value;
        }
        case PropertyType::Asset: {
            const std::string& value = static_cast<AssetRef*>(ref)->path;
            return isAssetPathProperty(name) ? assetPathToJson(ctx.project, value) : value;
        }
        case PropertyType::Shader:
            return static_cast<ShaderRef*>(ref)->path;
        case PropertyType::Float:
            return number(*static_cast<float*>(ref));
        case PropertyType::Double:
            return *static_cast<double*>(ref);
        case PropertyType::Vector2:
            return toJson(*static_cast<Vector2*>(ref));
        case PropertyType::Vector3:
            return toJson(*static_cast<Vector3*>(ref));
        case PropertyType::Vector4:
            return toJson(*static_cast<Vector4*>(ref));
        case PropertyType::Quat:
            return toJson(*static_cast<Quaternion*>(ref));
        case PropertyType::Int:
            // a colour write mask by its channels ("rgba", "none", "auto")
            if (name == "colorWrite" || endsWith(name, ".colorWrite")) return colorWriteToJson(*static_cast<int*>(ref));
            return *static_cast<int*>(ref);
        case PropertyType::Enum: {
            const int value = *static_cast<int*>(ref);
            const EnumOptions* options = enumOptions(ctx.component, name);
            return options ? enumToJson(value, *options) : Json(value);
        }
        case PropertyType::Ease:
            return enumToJson((int)static_cast<Ease*>(ref)->getType(), easeOptions());
        case PropertyType::UInt:
            return *static_cast<unsigned int*>(ref);
        case PropertyType::Font: {
            // the chain in use: the main font first, then its fallbacks
            const FontArray& fonts = *static_cast<FontArray*>(ref);
            size_t numFonts = 0;
            for (size_t i = 0; i < fonts.size(); i++) {
                if (!fonts[i].empty()) numFonts = i + 1;
            }
            Json list = Json::array();
            for (size_t i = 0; i < numFonts; i++) list.push_back(assetPathToJson(ctx.project, fonts[i]));
            return list;
        }
        case PropertyType::Texture:
            return textureToJson(ctx.project, ctx.scene, *static_cast<Texture*>(ref));
        case PropertyType::Entity:
            return entityToJson(ctx.scene, *static_cast<Entity*>(ref));
        case PropertyType::EntityReference:
            return entityReferenceToJson(ctx.project, ctx.scene, *static_cast<EntityReference*>(ref));
        case PropertyType::Material: {
            std::string linked;
            unsigned int submesh = 0;
            if (!readDefault && ctx.project && ctx.scene && ctx.component == ComponentType::MeshComponent &&
                submeshMaterialIndex(name, submesh)) {
                linked = ctx.project->getMaterialFilePath(ctx.scene->id, ctx.entity, submesh);
            }
            return materialToJson(ctx.project, ctx.scene, *static_cast<Material*>(ref), linked);
        }
        case PropertyType::Custom:
            break;
    }
    return nullptr;
}

Json McpValues::toJson(Project* project, const std::string& propertyName, const PropertyData& property) {
    ReadContext ctx;
    ctx.project = project;
    ctx.hasComponent = false;
    // without a component the enum name tables are unknown: Enum reads as its int
    ctx.component = ComponentType::PrefabComponent;
    return readProperty(ctx, propertyName, property);
}

// ==================================================================== writing

namespace {

// A script property of type Prefab ("scripts[N].name" whose ScriptProperty is a PrefabAsset).
// Catalog maps it to PropertyType::Custom with a PrefabAsset ref.
bool isScriptPrefabProperty(Scene* scene, Entity entity, ComponentType component, const std::string& name) {
    if (component != ComponentType::ScriptComponent || !scene || name.rfind("scripts[", 0) != 0) return false;
    const size_t close = name.find("].");
    if (close == std::string::npos) return false;
    char* end = nullptr;
    const unsigned long index = std::strtoul(name.c_str() + 8, &end, 10);
    if (end != name.c_str() + close) return false;
    const std::string field = name.substr(close + 2);
    ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
    if (!scripts || index >= scripts->scripts.size()) return false;
    for (const ScriptProperty& prop : scripts->scripts[index].properties) {
        if (prop.name == field) return prop.type == ScriptPropertyType::Prefab;
    }
    return false;
}

// null / "" = None; "path.prefab" or {"path"} (project-relative) or {"guid"(, "path")}
bool prefabAssetFromJson(Project* project, const Json& value, PrefabAsset& out, std::string& error) {
    out = PrefabAsset();
    if (value.is_null() || (value.is_string() && value.get<std::string>().empty())) return true;
    std::string path, guid;
    if (value.is_string()) {
        path = value.get<std::string>();
    } else if (value.is_object()) {
        path = value.contains("path") && value["path"].is_string() ? value["path"].get<std::string>() : "";
        guid = value.contains("guid") && value["guid"].is_string() ? value["guid"].get<std::string>() : "";
    } else {
        error = "a Prefab property takes a project-relative .prefab path, {\"guid\", \"path\"} or null.";
        return false;
    }
    if (guid.empty()) {
        fs::path absolute;
        if (path.empty() || !McpRefs::resolveProjectPath(project, path, absolute, error, true) ||
            !Util::isPrefabFile(absolute.string())) {
            error = "\"" + path + "\" is not a .prefab of the project" + (error.empty() ? "" : " (" + error + ")") +
                    "; list them with list_prefabs.";
            return false;
        }
        const fs::path relative = project->normalizeToProjectRelative(absolute);
        guid = project->getPrefabGuid(relative);
        if (guid.empty()) {
            error = "\"" + path + "\" has no prefab GUID (is it a valid .prefab?).";
            return false;
        }
        path = relative.generic_string();
    } else if (project->resolvePrefabPath(guid, path).empty()) {
        error = "No prefab has the GUID \"" + guid + "\".";
        return false;
    }
    out = PrefabAsset(guid, path);
    return true;
}

} // namespace

Command* McpValues::buildPropertyCommand(Project* project, uint32_t sceneId, Entity entity, ComponentType component,
                                         const std::string& propertyName, const PropertyData& property,
                                         const Json& value, std::string& error, std::function<void()> onChanged,
                                         const std::string* shaderReference, const std::set<std::string>* editedKeys) {
    SceneProject* scene = project ? project->getScene(sceneId) : nullptr;
    if (!scene || !scene->scene) {
        error = "The scene is not loaded.";
        return nullptr;
    }
    if (component == ComponentType::LatticeComponent &&
        (propertyName == "resolutionX" || propertyName == "resolutionY" || propertyName == "resolutionZ")) {
        // alone it would leave the offsets laid out for the old grid
        error = "the resolution changes with edit_lattice resolution: [x, y, z] (or the Inspector's Resolution), "
                "which resamples the offsets so the shape stays.";
        return nullptr;
    }
    const std::string typeName = propertyTypeName(property.type);
    auto expect = [&](const std::string& what) -> Command* {
        error = "Property " + propertyName + " is " + typeName + ": value must be " + what + ".";
        return nullptr;
    };

    switch (property.type) {
        case PropertyType::Bool:
            if (!value.is_boolean()) return expect("true or false");
            return new PropertyCmd<bool>(project, sceneId, entity, component, propertyName, value.get<bool>(), onChanged);
        case PropertyType::String:
        case PropertyType::Asset:
        case PropertyType::Shader: {
            if (!value.is_string()) return expect("a string");
            std::string text = value.get<std::string>();
            // the command of the property's own type: a path set on a reference gets its GUID
            // from the file at the path when the editor next resolves the scene's references
            const PropertyType stringType = property.type;
            auto stringCmd = [&](const std::string& name, const std::string& stored) -> Command* {
                if (stringType == PropertyType::Asset)
                    return new PropertyCmd<AssetRef>(project, sceneId, entity, component, name, AssetRef(stored), onChanged);
                if (stringType == PropertyType::Shader)
                    return new PropertyCmd<ShaderRef>(project, sceneId, entity, component, name, ShaderRef(stored), onChanged);
                return new PropertyCmd<std::string>(project, sceneId, entity, component, name, stored, onChanged);
            };
            if (component == ComponentType::ModelComponent && propertyName == "filename") {
                // a model is its file: setting it loads the file (Inspector / drag and drop)
                std::string stored;
                if (!assetPathFromJson(project, text, stored, error)) return nullptr;
                if (stored.empty()) {
                    ModelComponent* model = scene->scene->findComponent<ModelComponent>(entity);
                    if (!model || !ModelClearCmd::hasContent(*model)) {
                        return stringCmd(propertyName, std::string());
                    }
                    std::string reason;
                    if (!ModelClearCmd::canClear(project, sceneId, entity, &reason)) {
                        error = "The model can't be emptied: " + reason;
                        return nullptr;
                    }
                    return new ModelClearCmd(project, sceneId, entity);
                }
                if (!Util::isModelFile(stored) || !fs::exists(project->resolveAssetPath(stored))) {
                    error = "filename must be an existing .gltf, .glb, .obj or .fbx file in the assets directory.";
                    return nullptr;
                }
                return new ModelLoadCmd(project, sceneId, entity, stored);
            }
            if (isAssetPathProperty(propertyName) && !assetPathFromJson(project, text, text, error)) return nullptr;
            // an Asset script property is project-relative: a GUID given names its file
            if (stringType == PropertyType::Asset && propertyName.compare(0, 8, "scripts[") == 0 && Guid::isValidText(text)) {
                const fs::path found = project->getAssetDatabase().getPath(Guid::parse(text));
                if (found.empty()) {
                    error = propertyName + ": no asset has the GUID " + text + ".";
                    return nullptr;
                }
                text = FileUtils::pathToGenericUtf8(found);
            }
            // a foliage layer's FBX mesh loads from its import, created here the first time
            if (component == ComponentType::TerrainComponent && endsWith(propertyName, "meshPath") &&
                    FbxImporter::isFbxFile(text) && !project->prepareModelFile(text)) {
                error = propertyName + ": " + text + " could not be imported (the console has details).";
                return nullptr;
            }
            if (isShaderPathProperty(propertyName)) {
                // an engine library shader of the component's type, or a fork whose files exist
                std::string why;
                if (!checkShaderReference(project, text, shaderPathType(component, propertyName), why)) {
                    error = propertyName + ": " + why;
                    return nullptr;
                }
                // a material's lighting mode the shader it then draws with does not list goes
                // back to auto in the same step (as the Inspector does)
                const std::string prefix = propertyName.substr(0, propertyName.size() - 6);
                const PropertyData lighting = propertyName != "customShader" && propertyName != "customDepthShader"
                    ? Catalog::findProperty(scene->scene, entity, component, prefix + "lighting") : PropertyData();
                if (lighting.ref) {
                    const std::string drawn = !text.empty() ? text
                        : Catalog::getShaderReferenceForProperty(scene->scene, entity, component, "shaderUniforms");
                    const MaterialLighting current = static_cast<MaterialLighting>(*static_cast<int*>(lighting.ref));
                    if (!Catalog::supportsLighting(current, findManifest(drawn).get())) {
                        auto* multi = new MultiPropertyCmd();
                        multi->addCommand(std::unique_ptr<Command>(stringCmd(propertyName, text)));
                        multi->addPropertyCmd<int>(project, sceneId, entity, component, prefix + "lighting",
                                                   (int)MaterialLighting::AUTO, onChanged);
                        return multi;
                    }
                }
            }
            return stringCmd(propertyName, text);
        }
        case PropertyType::Float: {
            if (!value.is_number()) return expect("a number");
            // a material's Stochastic Tiling values, in their ranges
            const size_t field = propertyName.rfind(".material.");
            if (field != std::string::npos) {
                if (const StochasticMaterialValue* param = findStochasticMaterialValue(propertyName.substr(field + 10))) {
                    if (!checkStochasticMaterialValue(*param, propertyName, value, error)) return nullptr;
                }
            }
            return new PropertyCmd<float>(project, sceneId, entity, component, propertyName, value.get<float>(), onChanged);
        }
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
            if (!parseVector3(value, v)) return expect("[x,y,z] (or \"#RRGGBB\")");
            return new PropertyCmd<Vector3>(project, sceneId, entity, component, propertyName, v, onChanged);
        }
        case PropertyType::Vector4: {
            Vector4 v = property.ref ? *static_cast<Vector4*>(property.ref) : Vector4::ZERO;
            if (!parseVector4(value, v)) return expect("[x,y,z,w] ([r,g,b] keeps alpha; \"#RRGGBB[AA]\")");
            return new PropertyCmd<Vector4>(project, sceneId, entity, component, propertyName, v, onChanged);
        }
        case PropertyType::Quat: {
            Quaternion q = property.ref ? *static_cast<Quaternion*>(property.ref) : Quaternion::IDENTITY;
            if (!parseQuaternion(value, q)) return expect("[x,y,z,w], {\"euler\":[x,y,z]} in degrees or {\"quat\":[x,y,z,w]}");
            return new PropertyCmd<Quaternion>(project, sceneId, entity, component, propertyName, q, onChanged);
        }
        case PropertyType::Int: {
            if (propertyName == "colorWrite" || endsWith(propertyName, ".colorWrite")) {
                int mask = -1;
                std::string why;
                if (!colorWriteFromJson(value, mask, why)) {
                    error = "Property " + propertyName + ": " + why;
                    return nullptr;
                }
                return new PropertyCmd<int>(project, sceneId, entity, component, propertyName, mask, onChanged);
            }
            if (!isIntegral(value)) return expect("an integer");
            const bool stencilByte = endsWith(propertyName, "stencil.ref") || endsWith(propertyName, "stencil.readMask") ||
                                     endsWith(propertyName, "stencil.writeMask");
            if (stencilByte && (integralValue(value) < 0 || integralValue(value) > 255)) return expect("an integer 0..255");
            return new PropertyCmd<int>(project, sceneId, entity, component, propertyName,
                                        static_cast<int>(integralValue(value)), onChanged);
        }
        case PropertyType::Enum: {
            const EnumOptions* options = enumOptions(component, propertyName);
            int parsed = 0;
            if (options) {
                std::string why;
                if (!enumFromJson(value, *options, parsed, why)) {
                    error = "Property " + propertyName + " is an enum: " + why;
                    return nullptr;
                }
            } else if (isIntegral(value)) {
                parsed = static_cast<int>(integralValue(value));
            } else {
                return expect("an integer");
            }
            if (endsWith(propertyName, "].material.lighting")) {
                // only the lighting modes the submesh's shader has
                const std::string shader = shaderReference ? *shaderReference
                                         : Catalog::getShaderReferenceForProperty(scene->scene, entity, component, propertyName);
                const std::shared_ptr<const ShaderManifest> manifest = findManifest(shader);
                if (!Catalog::supportsLighting(static_cast<MaterialLighting>(parsed), manifest.get())) {
                    std::string modes = "auto";
                    for (MaterialLighting mode : Catalog::getShaderLightingModes(manifest.get())) {
                        modes += ", " + enumToJson((int)mode, tableOptions(reflect::enums::kMaterialLighting)).get<std::string>();
                    }
                    error = "Property " + propertyName + ": " + (shader.empty() ? std::string("the built-in Lit shader") : shader) +
                            " takes " + modes + ".";
                    return nullptr;
                }
            }
            return new PropertyCmd<int>(project, sceneId, entity, component, propertyName, parsed, onChanged);
        }
        case PropertyType::Ease: {
            int parsed = 0;
            std::string why;
            if (!enumFromJson(value, easeOptions(), parsed, why)) {
                error = "Property " + propertyName + " is an ease: " + why;
                return nullptr;
            }
            return new PropertyCmd<Ease>(project, sceneId, entity, component, propertyName,
                                         Ease(static_cast<EaseType>(parsed)), onChanged);
        }
        case PropertyType::UInt:
            if (!isIntegral(value) || integralValue(value) < 0) return expect("a non-negative integer");
            return new PropertyCmd<unsigned int>(project, sceneId, entity, component, propertyName,
                                                 static_cast<unsigned int>(integralValue(value)), onChanged);
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
            } else if (value.is_null()) {
                requested.push_back(std::string());
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
                if (!stored.empty() && !fs::exists(project->resolveAssetPath(stored))) {
                    error = "font file \"" + requested[i] + "\" does not exist.";
                    return nullptr;
                }
                fonts[i] = stored;
            }
            return new PropertyCmd<FontArray>(project, sceneId, entity, component, propertyName, fonts, onChanged);
        }
        case PropertyType::Texture: {
            Texture texture;
            Entity camera = NULL_ENTITY;
            std::string why;
            if (property.ref && value.is_object() && value.contains("id") && !value.contains("path") &&
                !value.contains("cube") && !value.contains("camera") && value["id"].is_string() &&
                value["id"].get<std::string>() == static_cast<Texture*>(property.ref)->getId()) {
                // a generated / embedded texture read back unchanged (font atlas, embedded data)
                return new PropertyCmd<Texture>(project, sceneId, entity, component, propertyName,
                                                *static_cast<Texture*>(property.ref), onChanged);
            }
            if (!textureFromJson(project, scene, value, texture, camera, why)) {
                error = "Property " + propertyName + " is a texture: " + why;
                return nullptr;
            }
            if (camera == NULL_ENTITY) {
                return new PropertyCmd<Texture>(project, sceneId, entity, component, propertyName, texture, onChanged);
            }
            // render to texture, as the Inspector's camera picker does it
            auto* multi = new MultiPropertyCmd();
            CameraComponent* cameraComponent = scene->scene->findComponent<CameraComponent>(camera);
            if (cameraComponent && !cameraComponent->renderToTexture) {
                multi->addPropertyCmd<bool>(project, sceneId, camera, ComponentType::CameraComponent, "renderToTexture", true);
            }
            if (scene->mainCamera == camera) {
                multi->addCommand(std::make_unique<SetMainCameraCmd>(project, sceneId, NULL_ENTITY));
            }
            multi->addPropertyCmd<Texture>(project, sceneId, entity, component, propertyName, texture, onChanged);
            return multi;
        }
        case PropertyType::Entity: {
            Entity target = NULL_ENTITY;
            std::string why;
            if (!entityFromJson(scene, value, target, why)) {
                error = "Property " + propertyName + " is a GameObject reference: " + why;
                return nullptr;
            }
            // a follower moves its own subtree, which would move its rest or lattice with it (spec §6.3)
            if (component == ComponentType::LatticeFollowerComponent && target != NULL_ENTITY &&
                (target == entity || scene->scene->isParentOf(entity, target))) {
                error = "a Lattice Follower's " + propertyName + " can't be the follower itself or a GameObject below it "
                        "(the follower would move it): pick one outside its hierarchy, e.g. an empty sibling.";
                return nullptr;
            }
            return new PropertyCmd<Entity>(project, sceneId, entity, component, propertyName, target, onChanged);
        }
        case PropertyType::EntityReference: {
            EntityReference ref;
            std::string why;
            if (!entityReferenceFromJson(project, scene, value, ref, why)) {
                error = "Property " + propertyName + " is a GameObject reference: " + why;
                return nullptr;
            }
            return new PropertyCmd<EntityReference>(project, sceneId, entity, component, propertyName, ref, onChanged);
        }
        case PropertyType::Material: {
            unsigned int submesh = 0;
            const bool linkable = component == ComponentType::MeshComponent && submeshMaterialIndex(propertyName, submesh);
            const std::string linked = linkable ? project->getMaterialFilePath(sceneId, entity, submesh) : std::string();
            std::string file;
            bool unlinkOnly = false;
            if (value.is_string()) {
                file = value.get<std::string>();
            } else if (value.is_object() && value.contains("file") && value["file"].is_string() &&
                       !value["file"].get<std::string>().empty()) {
                file = value["file"].get<std::string>();
            } else if (value.is_null()) {
                unlinkOnly = true;
            } else if (!value.is_object()) {
                return expect("an object of material fields, a .material path, or null (unlink)");
            }
            if (!file.empty()) {
                if (!linkable) return expect("an object of material fields (only Mesh submesh materials link .material files)");
                fs::path absolute;
                if (!McpRefs::resolveProjectPath(project, file, absolute, error, true)) return nullptr;
                if (!Util::isMaterialFile(absolute.string())) {
                    error = "\"" + file + "\" is not a .material file.";
                    return nullptr;
                }
                Material material;
                try {
                    material = Stream::decodeMaterial(DocumentFiles::read(absolute, DocumentKind::Material).root);
                } catch (const std::exception& e) {
                    error = std::string("Failed to read the material: ") + e.what();
                    return nullptr;
                }
                material.name = project->normalizeToProjectRelative(absolute).generic_string();
                return new LinkMaterialCmd(project, sceneId, entity, component, propertyName, submesh, material, onChanged);
            }
            if (unlinkOnly) {
                if (linked.empty()) {
                    error = "Property " + propertyName + " is not linked to a .material file (null unlinks).";
                    return nullptr;
                }
                return new UnlinkMaterialCmd(project, sceneId, component, propertyName, submesh, std::vector<Entity>{entity}, onChanged);
            }
            Material material = property.ref ? *static_cast<Material*>(property.ref) : Material();
            std::string why;
            if (!materialFromJson(project, scene, value, material, why)) {
                error = "Property " + propertyName + " is a material: " + why;
                return nullptr;
            }
            if (linked.empty()) {
                return new PropertyCmd<Material>(project, sceneId, entity, component, propertyName, material, onChanged);
            }
            // an inline edit of a linked material: it is the GameObject's own material now
            auto* multi = new MultiPropertyCmd();
            multi->addCommand(std::make_unique<UnlinkMaterialCmd>(project, sceneId, component, propertyName, submesh,
                                                                  std::vector<Entity>{entity}));
            if (material.name == linked ||
                material.name == project->normalizeToProjectRelative(project->getProjectPath() / linked).generic_string()) {
                material.name.clear();
            }
            multi->addPropertyCmd<Material>(project, sceneId, entity, component, propertyName, material, onChanged);
            return multi;
        }
        case PropertyType::Custom:
            if (isScriptPrefabProperty(scene->scene, entity, component, propertyName)) {
                PrefabAsset prefab;
                std::string why;
                if (!prefabAssetFromJson(project, value, prefab, why)) {
                    error = "Property " + propertyName + ": " + why;
                    return nullptr;
                }
                return new PropertyCmd<PrefabAsset>(project, sceneId, entity, component, propertyName, prefab, onChanged);
            }
            if (propertyName == "stencil" || endsWith(propertyName, ".stencil")) {
                // the keys given change, null resets it
                MaterialStencil stencil = property.ref ? *static_cast<MaterialStencil*>(property.ref) : MaterialStencil();
                std::string why;
                if (!stencilFromJson(value, stencil, why)) {
                    error = "Property " + propertyName + ": " + why;
                    return nullptr;
                }
                return new PropertyCmd<MaterialStencil>(project, sceneId, entity, component, propertyName, stencil, onChanged);
            }
            if (endsWith(propertyName, "material.extraPass")) {
                // merged like a material: its values follow its own shader's manifest; null removes it
                MaterialPass pass = property.ref ? *static_cast<MaterialPass*>(property.ref) : MaterialPass();
                std::string why;
                if (!materialPassFromJson(project, scene, value, pass, why)) {
                    error = "Property " + propertyName + ": " + why;
                    return nullptr;
                }
                return new PropertyCmd<MaterialPass>(project, sceneId, entity, component, propertyName, pass, onChanged);
            }
            if (propertyName == "shaderUniforms" || endsWith(propertyName, "shaderParams") ||
                endsWith(propertyName, "shaderTextures") || endsWith(propertyName, "shaderFeatures")) {
                // shader library values, checked against the manifest of the shader they feed
                const std::string shader = shaderReference ? *shaderReference
                                         : Catalog::getShaderReferenceForProperty(scene->scene, entity, component, propertyName);
                const std::shared_ptr<const ShaderManifest> manifest = findManifest(shader);
                const bool library = ShaderReference::isLibrary(shader);
                std::string why;
                Command* cmd = nullptr;
                if (endsWith(propertyName, "shaderTextures")) {
                    std::vector<std::pair<std::string, Texture>> textures;
                    if (shaderTexturesFromJson(project, scene, manifest.get(), library, value, textures, why, editedKeys)) {
                        cmd = new PropertyCmd<std::vector<std::pair<std::string, Texture>>>(project, sceneId, entity, component, propertyName, textures, onChanged);
                    }
                } else if (endsWith(propertyName, "shaderFeatures")) {
                    std::vector<std::string> features;
                    if (property.ref) features = *static_cast<std::vector<std::string>*>(property.ref);
                    if (shaderFeaturesFromJson(shader, manifest.get(), library, value, features, why)) {
                        cmd = new PropertyCmd<std::vector<std::string>>(project, sceneId, entity, component, propertyName, features, onChanged);
                    }
                } else {
                    ShaderUniformValues uniforms;
                    if (shaderParamsFromJson(manifest.get(), library, value, uniforms, why, editedKeys)) {
                        cmd = new PropertyCmd<ShaderUniformValues>(project, sceneId, entity, component, propertyName, uniforms, onChanged);
                    }
                }
                if (!cmd) error = "Property " + propertyName + ": " + why;
                return cmd;
            }
            error = "Property " + propertyName + " is custom: set it with set_properties (component encoding).";
            return nullptr;
    }
    error = "Unsupported property type.";
    return nullptr;
}

namespace {

// A FluidSimulation's "preset" pseudo-property (parseChanges): the preset's fields as changes,
// their values read from the preset as get_entity reads them
bool appendFluidPresetChanges(const Json& value, std::vector<McpValues::Change>& out, std::string& error) {
    const FluidPreset* preset = value.is_string() ? findFluidPreset(value.get<std::string>()) : nullptr;
    if (!preset) {
        std::string ids;
        for (const FluidPreset& item : getFluidPresets()) ids += (ids.empty() ? "" : ", ") + item.id;
        error = "unknown fluid preset " + value.dump() + "; the presets: " + ids + " (jasper://docs/fluid).";
        return false;
    }
    FluidSimulationComponent values = preset->values;
    const std::map<std::string, PropertyData> properties = Catalog::getProperties(ComponentType::FluidSimulationComponent, &values);
    McpValues::ReadContext ctx;
    ctx.component = ComponentType::FluidSimulationComponent;
    for (const std::string& name : preset->fields) {
        auto it = properties.find(name);
        if (it == properties.end()) continue;
        McpValues::Change change;
        change.component = ComponentType::FluidSimulationComponent;
        change.property = name;
        change.value = McpValues::readProperty(ctx, name, it->second);
        out.push_back(change);
    }
    return true;
}

bool isFluidPreset(const McpValues::Change& change) {
    return change.component == ComponentType::FluidSimulationComponent && change.property == "preset";
}

}

bool McpValues::parseChanges(const Json& changes, std::vector<Change>& out, std::string& error) {
    out.clear();
    if (changes.is_object()) {
        // {component: {property: value}}
        for (auto it = changes.begin(); it != changes.end(); ++it) {
            Change base;
            if (!McpRefs::resolveComponent(it.key(), base.component, error)) return false;
            if (!it.value().is_object()) {
                error = "values[\"" + it.key() + "\"] must be an object {property: value}.";
                return false;
            }
            // a fluid's preset first: the values named with it override its fields
            if (base.component == ComponentType::FluidSimulationComponent && it.value().contains("preset")) {
                if (!appendFluidPresetChanges(it.value()["preset"], out, error)) return false;
            }
            for (auto prop = it.value().begin(); prop != it.value().end(); ++prop) {
                Change change = base;
                change.property = prop.key();
                change.value = prop.value();
                if (isFluidPreset(change)) continue;
                out.push_back(change);
            }
        }
        return true;
    }
    if (!changes.is_array()) {
        error = "changes must be an array of {component, property, value} (or {component: {property: value}}).";
        return false;
    }
    for (size_t i = 0; i < changes.size(); i++) {
        const Json& item = changes[i];
        const std::string where = "changes[" + std::to_string(i) + "]";
        if (!item.is_object() || !item.contains("component") || !item.contains("property") ||
            !item["property"].is_string() || item["property"].get<std::string>().empty()) {
            error = where + " must be {component, property, value} (or insert: value / remove: index).";
            return false;
        }
        Change change;
        if (!McpRefs::resolveComponent(item["component"], change.component, error)) {
            error = where + ": " + error;
            return false;
        }
        change.property = item["property"].get<std::string>();
        const int ops = (item.contains("value") ? 1 : 0) + (item.contains("insert") ? 1 : 0) + (item.contains("remove") ? 1 : 0);
        if (ops != 1) {
            error = where + " needs exactly one of value, insert (with optional index) or remove (an index).";
            return false;
        }
        if (item.contains("value")) {
            change.value = item["value"];
        } else if (item.contains("insert")) {
            change.op = "insert";
            change.value = item["insert"];
            if (item.contains("index")) {
                if (!isIntegral(item["index"]) || integralValue(item["index"]) < 0) {
                    error = where + ".index must be a non-negative integer.";
                    return false;
                }
                change.index = static_cast<long>(integralValue(item["index"]));
            }
        } else {
            change.op = "remove";
            if (!isIntegral(item["remove"]) || integralValue(item["remove"]) < 0) {
                error = where + ".remove must be the index of the element to remove.";
                return false;
            }
            change.index = static_cast<long>(integralValue(item["remove"]));
        }
        if (isFluidPreset(change)) {
            if (change.op != "set") {
                error = where + ": a fluid's preset takes value: <preset id>.";
                return false;
            }
            if (!appendFluidPresetChanges(change.value, out, error)) {
                error = where + ": " + error;
                return false;
            }
            continue;
        }
        out.push_back(change);
    }
    if (out.empty()) {
        error = "changes is empty.";
        return false;
    }
    return true;
}

Json McpValues::fluidStatsToJson(Scene* scene, Entity entity) {
    if (!scene || !scene->isComponentRegistered<FluidSimulationComponent>()) return nullptr;
    FluidSimulationComponent* fluid = scene->findComponent<FluidSimulationComponent>(entity);
    if (!fluid) return nullptr;
    const std::string preset = matchFluidPreset(*fluid);
    Json out = {{"preset", preset.empty() ? Json(nullptr) : Json(preset)}};
    std::string reason;
    out["supported"] = FluidSimulator::isSupported(&reason);
    if (!reason.empty()) out["unsupported_reason"] = reason;
    // drawn now: enabled, its GameObject active and its last frame drew it (a view drawing the
    // scene, the shaders built; `error` may still name a feature the device turned off); the
    // stats below stay those of its last frame
    out["running"] = fluid->simulator != nullptr && fluid->enabled && scene->isEntityActiveInHierarchy(entity) &&
                     fluid->simulator->getStats().drawn;
    if (!fluid->simulator) return out;
    const FluidStats& stats = fluid->simulator->getStats();
    out["steps"] = stats.steps;
    out["time"] = stats.time;
    out["splats"] = stats.splats;
    out["seed"] = stats.seed;
    out["sim_size"] = Json::array({stats.simWidth, stats.simHeight});
    out["dye_size"] = Json::array({stats.dyeWidth, stats.dyeHeight});
    out["output_size"] = Json::array({stats.outputWidth, stats.outputHeight});
    out["gpu_bytes"] = stats.gpuBytes;
    out["passes_last_frame"] = stats.passesLastFrame;
    out["shading_active"] = stats.shadingActive;
    out["bloom_active"] = stats.bloomActive;
    out["sunrays_active"] = stats.sunraysActive;
    out["manual_filtering"] = stats.manualFiltering;
    out["compact_formats"] = stats.compactFormats;
    out["target_failures"] = stats.targetFailures;
    const std::string& error = fluid->simulator->getError();
    if (!error.empty()) out["error"] = error;
    return out;
}

std::string McpValues::propertyNamesText(SceneProject* scene, Entity entity, ComponentType component, size_t max) {
    std::string text;
    size_t count = 0;
    if (!scene || !scene->scene) return text;
    unsigned int usedSubmeshes = 0;
    if (MeshComponent* mesh = scene->scene->findComponent<MeshComponent>(entity)) usedSubmeshes = mesh->numSubmeshes;
    for (const auto& [name, property] : Catalog::findEntityProperties(scene->scene, entity, component)) {
        if (component == ComponentType::MeshComponent && name.compare(0, 10, "submeshes[") == 0 &&
            std::strtoul(name.c_str() + 10, nullptr, 10) >= usedSubmeshes) {
            continue;
        }
        if (count++ >= max) {
            text += ", ...";
            break;
        }
        if (!text.empty()) text += ", ";
        text += name;
    }
    return text.empty() ? "(none)" : text;
}

// Whether set_properties sets `property` as one typed value from the call's JSON (merged into its
// current value): a material, an extra pass, a stencil block, a shader library collection
static bool isWholeValueProperty(Scene* scene, Entity entity, ComponentType component, const std::string& property) {
    const PropertyData data = Catalog::findProperty(scene, entity, component, property);
    if (!data.ref) return false;
    if (data.type == PropertyType::Material) return true;
    return data.type == PropertyType::Custom &&
           (property == "stencil" || endsWith(property, ".stencil") || endsWith(property, "material.extraPass") ||
            property == "shaderUniforms" || endsWith(property, "shaderParams") || endsWith(property, "shaderTextures") ||
            endsWith(property, "shaderFeatures"));
}

namespace {

    // The numbers of a value outside [min, max] (a number, or the lanes of a vector or colour)
    bool outsideRange(const Json& value, double min, double max, double& bad) {
        auto check = [&](const Json& number) {
            if (!number.is_number()) return false;
            const double v = number.get<double>();
            if (v < min || v > max) {
                bad = v;
                return true;
            }
            return false;
        };
        if (value.is_number()) return check(value);
        if (value.is_array()) {
            for (const Json& lane : value) {
                if (check(lane)) return true;
            }
        } else if (value.is_object()) {
            for (const char* key : {"x", "y", "z", "w", "r", "g", "b", "a"}) {
                if (value.contains(key) && check(value[key])) return true;
            }
        }
        return false;
    }

    // 255 as "255", 0.5 as "0.5"
    std::string numberText(double value) {
        if (std::floor(value) == value && std::fabs(value) < 1e15) return std::to_string(static_cast<long long>(value));
        return Json(McpValues::number(static_cast<float>(value))).dump();
    }

    // "0..255", "at least 0", "at most 1"
    std::string rangeText(const reflect::FieldDesc& field) {
        const bool low = std::isfinite(field.min);
        const bool high = std::isfinite(field.max);
        if (low && high) return "in " + numberText(field.min) + ".." + numberText(field.max);
        if (low) return "at least " + numberText(field.min);
        return "at most " + numberText(field.max);
    }

}

bool McpValues::checkReflectedChanges(std::vector<Change>& changes, std::vector<std::string>& warnings, std::string& error) {
    for (Change& change : changes) {
        const reflect::ComponentDesc* desc = reflect::find(change.component);
        if (!desc) continue;
        bool viaAlias = false;
        const reflect::FieldDesc* field = reflect::findField(*desc, change.property, &viaAlias);
        if (!field) continue;
        const std::string component = McpRefs::componentName(change.component);
        if (viaAlias) {
            const std::string canonical = reflect::canonicalPath(*desc, nullptr, change.property);
            if (!canonical.empty() && canonical != change.property) {
                warnings.push_back(std::string(desc->key) + "." + change.property + " is now " + desc->key + "." + canonical);
                change.property = canonical;
            }
        }
        if (field->flags & (reflect::ReadOnly | reflect::NoMcpWrite | reflect::Derived)) {
            error = component + "." + change.property + " is read-only: the editor sets it.";
            return false;
        }
        double bad = 0.0;
        const bool numeric = field->kind == reflect::Kind::Float || field->kind == reflect::Kind::Double || field->kind == reflect::Kind::Int ||
                             field->kind == reflect::Kind::UInt || field->kind == reflect::Kind::Vector2 || field->kind == reflect::Kind::Vector3 ||
                             field->kind == reflect::Kind::Vector4;
        const Json& value = change.op == "set" || change.op == "insert" ? change.value : Json();
        // (a Clamped field takes any value: the edit clamps it, as its load does)
        if (numeric && !(field->flags & reflect::Clamped) && reflect::hasRange(*field) && outsideRange(value, field->min, field->max, bad)) {
            const double nearest = bad < field->min ? field->min : field->max;
            error = component + "." + change.property + " must be " + rangeText(*field) + " (" + numberText(bad) + " is out of range); the nearest "
                    "valid value is " + numberText(nearest) + ".";
            return false;
        }
    }
    return true;
}

bool McpValues::appendChangeCommands(Project* project, SceneProject* scene, const std::vector<Entity>& entities,
                                     const std::vector<Change>& changes, MultiPropertyCmd& multi, std::string& error) {
    if (!scene || !scene->scene) {
        error = "The scene is not loaded.";
        return false;
    }
    for (Entity entity : entities) {
        const std::string label = "GameObject " + std::to_string(entity) + " \"" + scene->scene->getEntityName(entity) + "\"";
        // components in order of first appearance
        std::vector<ComponentType> order;
        for (const Change& change : changes) {
            if (std::find(order.begin(), order.end(), change.component) == order.end()) order.push_back(change.component);
        }
        for (ComponentType component : order) {
            const std::string componentLabel = McpRefs::componentName(component);
            if (!McpRefs::hasComponent(scene, entity, component)) {
                error = label + " has no " + componentLabel + " component; add it first (add_component, or create_entity's "
                        "components).";
                return false;
            }
            // A whole value set as one command (a material, an extra pass, a stencil, a shader
            // library collection) and a path inside it in the same call: each command is built from
            // the values before the call (an entry also checked against the shader before it), so
            // whichever applies last undoes the other. Refused with the one-value form.
            for (const Change& whole : changes) {
                if (whole.component != component || whole.op != "set" || !isWholeValueProperty(scene->scene, entity, component, whole.property))
                    continue;
                for (const Change& inner : changes) {
                    if (inner.component != component || inner.property.size() <= whole.property.size() + 1 ||
                        inner.property.compare(0, whole.property.size() + 1, whole.property + ".") != 0) {
                        continue;
                    }
                    // the inner path as keys of the whole value's object
                    std::vector<std::string> keys;
                    const std::string rest = inner.property.substr(whole.property.size() + 1);
                    for (size_t start = 0;;) {
                        const size_t dot = rest.find('.', start);
                        keys.push_back(rest.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
                        if (dot == std::string::npos) break;
                        start = dot + 1;
                    }
                    Json nested = inner.op == "set" ? inner.value : Json("...");
                    for (auto key = keys.rbegin(); key != keys.rend(); ++key) nested = Json{{*key, nested}};
                    error = componentLabel + "." + whole.property + " and " + inner.property + " on " + label + " in one call: set "
                            "the whole value with the inner one in its object (" + whole.property + ": " + nested.dump() +
                            " merges), or use two set_properties calls.";
                    return false;
                }
            }
            std::vector<YamlEdit> yamlEdits;
            std::vector<std::unique_ptr<Command>> typed;
            // shader-library entry edits by collection property, in order of appearance, with
            // the entries each names (only those are checked against the shader)
            std::vector<std::pair<std::string, Json>> entryEdits;
            std::map<std::string, std::set<std::string>> entryKeys;
            ReadContext readCtx;
            readCtx.project = project;
            readCtx.scene = scene;
            readCtx.entity = entity;
            readCtx.component = component;
            // a shader set by this call: the values after it follow its manifest
            std::map<std::string, std::string> pendingShaders; // property prefix -> shader
            for (const Change& change : changes) {
                if (change.component != component || change.op != "set" || !change.value.is_string()) continue;
                if (change.property == "customShader") pendingShaders[""] = change.value.get<std::string>();
                else if (change.property.compare(0, 10, "submeshes[") == 0 && (endsWith(change.property, "].material.shader") ||
                         endsWith(change.property, "].material.extraPass.shader"))) {
                    pendingShaders[change.property.substr(0, change.property.size() - 6)] = change.value.get<std::string>();
                }
            }
            // The shader a property's values will feed once this call applies, when a change of
            // the call sets it (else null: the current one). A submesh material without its own
            // shader draws with the mesh's customShader, else the scene default (as
            // RenderSystem::getSubmeshShader); a component's empty customShader is the scene's.
            std::string pendingShader;
            auto pendingShaderFor = [&](const std::string& property) -> const std::string* {
                if (pendingShaders.empty()) return nullptr;
                const auto custom = pendingShaders.find("");
                const ShaderType type = shaderPathType(component, "customShader");
                auto sceneDefault = [&](const std::string& shader) {
                    return shader.empty() ? scene->scene->getDefaultCustomShader(type) : shader;
                };
                if (component == ComponentType::MeshComponent && property.compare(0, 10, "submeshes[") == 0) {
                    const size_t close = property.find("].material.");
                    if (close == std::string::npos) return nullptr;
                    // the extra pass's values follow its own shader only
                    if (property.compare(close, 21, "].material.extraPass.") == 0) {
                        const auto pass = pendingShaders.find(property.substr(0, close + 21));
                        if (pass == pendingShaders.end()) return nullptr;
                        pendingShader = pass->second;
                        return &pendingShader;
                    }
                    const std::string prefix = property.substr(0, close + 11);
                    const auto own = pendingShaders.find(prefix);
                    if (own != pendingShaders.end() && !own->second.empty()) {
                        pendingShader = own->second;
                        return &pendingShader;
                    }
                    if (own == pendingShaders.end()) {
                        // the material keeps its own shader: the mesh's does not matter
                        const std::string* current = Catalog::getPropertyRef<std::string>(scene->scene, entity, component, prefix + "shader");
                        if (!current || !current->empty() || custom == pendingShaders.end()) return nullptr;
                    }
                    if (custom != pendingShaders.end()) {
                        pendingShader = sceneDefault(custom->second);
                    } else {
                        // the material's shader cleared: the mesh's current one
                        pendingShader = Catalog::getShaderReferenceForProperty(scene->scene, entity, component, "shaderUniforms");
                    }
                    return &pendingShader;
                }
                if (custom == pendingShaders.end()) return nullptr;
                if (property.find('.') != std::string::npos && property.compare(0, 15, "shaderUniforms.") != 0 &&
                    property.compare(0, 15, "shaderTextures.") != 0) {
                    return nullptr;
                }
                pendingShader = sceneDefault(custom->second);
                return &pendingShader;
            };
            for (const Change& change : changes) {
                if (change.component != component) continue;
                std::string entryCollection, entryKey;
                if (change.op == "set" && Catalog::splitShaderEntryProperty(change.property, entryCollection, entryKey)) {
                    // one entry of a shader-library collection: gathered, then set as one value
                    const PropertyData collection = Catalog::findProperty(scene->scene, entity, component, entryCollection);
                    if (!collection.ref) {
                        error = "Unknown property \"" + change.property + "\" of " + componentLabel + " on " + label + ".";
                        return false;
                    }
                    auto found = std::find_if(entryEdits.begin(), entryEdits.end(), [&](const auto& e) { return e.first == entryCollection; });
                    if (found == entryEdits.end()) {
                        entryEdits.push_back({entryCollection, readProperty(readCtx, entryCollection, collection)});
                        found = std::prev(entryEdits.end());
                    }
                    Json& edited = found->second;
                    if (!edited.is_object()) edited = Json::object();
                    if (change.value.is_null()) edited.erase(entryKey);
                    else edited[entryKey] = change.value;
                    entryKeys[entryCollection].insert(entryKey);
                    continue;
                }
                const PropertyData property = Catalog::findProperty(scene->scene, entity, component, change.property);
                const bool typedProperty = property.ref && change.op == "set" &&
                    (property.type != PropertyType::Custom || change.property == "shaderUniforms" ||
                     endsWith(change.property, "shaderParams") || endsWith(change.property, "shaderTextures") ||
                     endsWith(change.property, "shaderFeatures") || change.property == "stencil" ||
                     endsWith(change.property, ".stencil") || endsWith(change.property, "material.extraPass") ||
                     isScriptPrefabProperty(scene->scene, entity, component, change.property));
                if (typedProperty) {
                    std::string why;
                    Command* cmd = buildPropertyCommand(project, scene->id, entity, component, change.property, property, change.value, why,
                                                        nullptr, pendingShaderFor(change.property));
                    if (!cmd) {
                        error = componentLabel + "." + change.property + " on " + label + ": " + why;
                        return false;
                    }
                    typed.emplace_back(cmd);
                    continue;
                }
                const bool known = property.ref && property.type == PropertyType::Custom;
                yamlEdits.push_back({change.property, change.op, change.value, change.index, known});
            }
            for (const auto& [collectionName, value] : entryEdits) {
                const PropertyData collection = Catalog::findProperty(scene->scene, entity, component, collectionName);
                std::string why;
                Command* cmd = buildPropertyCommand(project, scene->id, entity, component, collectionName, collection, value, why,
                                                    nullptr, pendingShaderFor(collectionName), &entryKeys[collectionName]);
                if (!cmd) {
                    error = componentLabel + "." + collectionName + " on " + label + ": " + why;
                    return false;
                }
                typed.emplace_back(cmd);
            }
            if (!yamlEdits.empty()) {
                YAML::Node value = YAML::Clone(EntityClipboard::encodeComponent(scene->scene, entity, component));
                for (const YamlEdit& edit : yamlEdits) {
                    std::string why;
                    if (!editYamlPath(value, edit.property, edit.op, edit.value, edit.index, why, edit.known)) {
                        if (!findYamlPath(value, edit.property)) {
                            // the nearest property name first (S08 R26)
                            std::vector<std::string> names;
                            for (const auto& [name, property] : Catalog::findEntityProperties(scene->scene, entity, component)) {
                                names.push_back(name);
                            }
                            error = "Unknown property \"" + edit.property + "\" of " + componentLabel + " on " + label +
                                    NearestNameSuffix(edit.property, names) + ". Properties: " + propertyNamesText(scene, entity, component) +
                                    " (custom ones also by their scene-file keys, see get_entity detail=\"yaml\").";
                        } else {
                            error = componentLabel + "." + edit.property + " on " + label + ": " + why;
                        }
                        return false;
                    }
                    syncListCount(value, edit.property);
                }
                auto clip = std::make_shared<ComponentClip>();
                clip->project = EntityClipboard::projectKey(project);
                clip->sceneId = scene->id;
                clip->type = component;
                clip->entity = entity;
                clip->value = value;
                // the GameObject's own references stay: every entity of the scene is "still there"
                for (Entity other : scene->entities) {
                    if (scene->scene->isEntityCreated(other)) clip->externalNames[other] = scene->scene->getEntityName(other);
                }
                multi.addCommand(std::make_unique<ComponentValuesCmd>(project, scene->id, std::vector<Entity>{entity}, component, clip));
            }
            for (auto& cmd : typed) multi.addCommand(std::move(cmd));
        }
    }
    return true;
}

// ==================================================================== commands

void McpValues::refreshTransforms(SceneProject* scene) {
    if (!scene || !scene->scene) return;
    std::shared_ptr<RenderSystem> render = scene->scene->getSystem<RenderSystem>();
    auto transforms = scene->scene->getComponentArray<Transform>();
    if (!render || !transforms) return;
    // Transform order has parents first
    for (size_t i = 0; i < transforms->size(); i++) {
        render->updateTransform(transforms->getComponentFromIndex(i), transforms->getEntity(i));
    }
}

void McpStepsCmd::refresh() {
    if (!transformProject) return;
    McpValues::refreshTransforms(transformProject->getScene(transformSceneId));
}

bool McpStepsCmd::execute() {
    if (built) {
        for (size_t i = 0; i < done.size(); i++) {
            if (!done[i]->execute()) {
                while (i > 0) {
                    done[--i]->undo();
                    refresh();
                }
                return false;
            }
            refresh();
        }
        return true;
    }
    error->clear();
    refresh();
    for (Step& step : steps) {
        std::string why;
        Command* cmd = step(why);
        if (!cmd || !cmd->execute()) {
            *error = why.empty() ? std::string("A step of the edit failed.") : why;
            delete cmd;
            undo();
            done.clear();
            return false;
        }
        done.emplace_back(cmd);
        refresh();
    }
    built = true;
    return true;
}

std::string McpStepsCmd::describe() const {
    if (done.empty()) return "Edit";
    std::string name = done.front()->describe();
    if (done.size() > 1) name += " (+" + std::to_string(done.size() - 1) + " more)";
    return name;
}

bool McpStepsCmd::isNoOp() const {
    for (const std::unique_ptr<Command>& cmd : done) {
        if (!cmd->isNoOp()) return false;
    }
    return true;
}

void McpStepsCmd::collectKeepCommands(KeepContext& context) const {
    for (const std::unique_ptr<Command>& cmd : done) context.collect(*cmd, context.source);
}

size_t McpStepsCmd::memoryBytes() const {
    size_t bytes = sizeof(*this) + steps.capacity() * sizeof(Step);
    for (const std::unique_ptr<Command>& cmd : done) bytes += cmd->memoryBytes();
    return bytes;
}

void McpStepsCmd::undo() {
    for (auto it = done.rbegin(); it != done.rend(); ++it) {
        (*it)->undo();
        refresh();
    }
}

namespace {

bool runCommand(CommandHistory* history, Command* cmd, std::string& error, const std::string& fallback) {
    std::shared_ptr<std::string> stepsError;
    if (auto* steps = dynamic_cast<McpStepsCmd*>(cmd)) stepsError = steps->errorSlot();
    const uint64_t logId = Out::lastLogId();
    cmd->setNoMerge();
    if (history->addCommand(cmd)) return true;   // deletes cmd on failure
    if (stepsError && !stepsError->empty()) {
        error = *stepsError;
        return false;
    }
    std::string logged;
    for (const LogEntry& entry : Out::getLogsSince(logId, 5, true)) {
        if (!logged.empty()) logged += " ";
        logged += entry.message;
    }
    error = !logged.empty() ? logged : (!fallback.empty() ? fallback : std::string("The edit did not apply."));
    return false;
}

} // namespace

bool jasperengine::editor::mcp::runSceneCommand(uint32_t sceneId, Command* cmd, std::string& error, const std::string& fallback) {
    return runCommand(CommandHandle::get(sceneId), cmd, error, fallback);
}

bool jasperengine::editor::mcp::runProjectCommand(Project* project, Command* cmd, std::string& error, const std::string& fallback) {
    return runCommand(project->getProjectCommandHistory(), cmd, error, fallback);
}
