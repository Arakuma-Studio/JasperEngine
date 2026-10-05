// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpValues.h"

#include "McpRefs.h"

#include "Out.h"
#include "Project.h"
#include "Stream.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/LinkMaterialCmd.h"
#include "command/type/ModelClearCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "command/type/SetMainCameraCmd.h"
#include "command/type/UnlinkMaterialCmd.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/TextComponent.h"
#include "subsystem/RenderSystem.h"
#include "texture/TextureData.h"
#include "util/CameraTextureLink.h"
#include "util/EntityClipboard.h"
#include "util/Util.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>

namespace fs = std::filesystem;

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;

namespace {

// ------------------------------------------------------------------ small helpers

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

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

// The shortest decimal that reads back as the same float: 0.1f is 0.1, not 0.10000000149
double cleanFloat(float value) {
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

bool isShaderPathProperty(const std::string& propertyName) {
    return propertyName == "customShader" || propertyName == "customDepthShader";
}

bool isSafeRelative(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const fs::path& part : path.lexically_normal()) {
        if (part == "..") return false;
    }
    return true;
}

// "colliders[12].type" → "colliders[].type"
std::string stripIndices(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size(); i++) {
        out.push_back(name[i]);
        if (name[i] == '[') {
            size_t j = i + 1;
            while (j < name.size() && std::isdigit(static_cast<unsigned char>(name[j]))) j++;
            i = j - 1;
        }
    }
    return out;
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

const EnumOptions& anchorPresetOptions() {
    static const EnumOptions options = {
        {(int)AnchorPreset::NONE, "none", "None"},
        {(int)AnchorPreset::TOP_LEFT, "top_left", "Top Left"},
        {(int)AnchorPreset::TOP_RIGHT, "top_right", "Top Right"},
        {(int)AnchorPreset::BOTTOM_LEFT, "bottom_left", "Bottom Left"},
        {(int)AnchorPreset::BOTTOM_RIGHT, "bottom_right", "Bottom Right"},
        {(int)AnchorPreset::CENTER_LEFT, "center_left", "Center Left"},
        {(int)AnchorPreset::CENTER_TOP, "center_top", "Center Top"},
        {(int)AnchorPreset::CENTER_RIGHT, "center_right", "Center Right"},
        {(int)AnchorPreset::CENTER_BOTTOM, "center_bottom", "Center Bottom"},
        {(int)AnchorPreset::CENTER, "center", "Center"},
        {(int)AnchorPreset::LEFT_WIDE, "left_wide", "Left Wide"},
        {(int)AnchorPreset::TOP_WIDE, "top_wide", "Top Wide"},
        {(int)AnchorPreset::RIGHT_WIDE, "right_wide", "Right Wide"},
        {(int)AnchorPreset::BOTTOM_WIDE, "bottom_wide", "Bottom Wide"},
        {(int)AnchorPreset::VERTICAL_CENTER_WIDE, "vertical_center_wide", "Vertical Center Wide"},
        {(int)AnchorPreset::HORIZONTAL_CENTER_WIDE, "horizontal_center_wide", "Horizontal Center Wide"},
        {(int)AnchorPreset::FULL_LAYOUT, "full_layout", "Full Layout"}
    };
    return options;
}

const EnumOptions& pivotPresetOptions() {
    static const EnumOptions options = {
        {(int)PivotPreset::CENTER, "center", "Center"},
        {(int)PivotPreset::TOP_CENTER, "top_center", "Top Center"},
        {(int)PivotPreset::BOTTOM_CENTER, "bottom_center", "Bottom Center"},
        {(int)PivotPreset::LEFT_CENTER, "left_center", "Left Center"},
        {(int)PivotPreset::RIGHT_CENTER, "right_center", "Right Center"},
        {(int)PivotPreset::TOP_LEFT, "top_left", "Top Left"},
        {(int)PivotPreset::BOTTOM_LEFT, "bottom_left", "Bottom Left"},
        {(int)PivotPreset::TOP_RIGHT, "top_right", "Top Right"},
        {(int)PivotPreset::BOTTOM_RIGHT, "bottom_right", "Bottom Right"}
    };
    return options;
}

const EnumOptions& orientationOptions() {
    // ScrollbarType and ProgressbarType share their values
    static const EnumOptions options = {
        {(int)ScrollbarType::VERTICAL, "vertical", "Vertical"},
        {(int)ScrollbarType::HORIZONTAL, "horizontal", "Horizontal"}
    };
    return options;
}

const EnumOptions& progressbarTypeOptions() {
    static const EnumOptions options = {
        {(int)ProgressbarType::VERTICAL, "vertical", "Vertical"},
        {(int)ProgressbarType::HORIZONTAL, "horizontal", "Horizontal"}
    };
    return options;
}

const EnumOptions& actionStateOptions() {
    static const EnumOptions options = {
        {(int)ActionState::Running, "running", "Running"},
        {(int)ActionState::Paused, "paused", "Paused"},
        {(int)ActionState::Stopped, "stopped", "Stopped"}
    };
    return options;
}

const EnumOptions& lightTypeOptions() {
    static const EnumOptions options = {
        {(int)LightType::DIRECTIONAL, "directional", "Directional"},
        {(int)LightType::POINT, "point", "Point"},
        {(int)LightType::SPOT, "spot", "Spot"}
    };
    return options;
}

const EnumOptions& shadowCasterShapeOptions() {
    static const EnumOptions options = {
        {(int)ShadowCaster2DShape::AUTO_QUAD, "auto_quad", "Auto Quad"},
        {(int)ShadowCaster2DShape::POLYGON, "polygon", "Polygon"}
    };
    return options;
}

const EnumOptions& fogTypeOptions() {
    static const EnumOptions options = {
        {(int)FogType::LINEAR, "linear", "Linear"},
        {(int)FogType::EXPONENTIAL, "exponential", "Exponential"},
        {(int)FogType::EXPONENTIALSQUARED, "exponential_squared", "Exponential Squared"}
    };
    return options;
}

const EnumOptions& probeModeOptions() {
    static const EnumOptions options = {
        {(int)ReflectionProbeMode::STATIC, "static", "Static"},
        {(int)ReflectionProbeMode::DYNAMIC, "dynamic", "Dynamic"}
    };
    return options;
}

const EnumOptions& probeUpdateModeOptions() {
    static const EnumOptions options = {
        {(int)ReflectionProbeUpdateMode::ON_LOAD, "on_load", "On Load"},
        {(int)ReflectionProbeUpdateMode::ON_MOVE, "on_move", "On Move"},
        {(int)ReflectionProbeUpdateMode::INTERVAL, "interval", "Interval"},
        {(int)ReflectionProbeUpdateMode::MANUAL, "manual", "Manual"}
    };
    return options;
}

const EnumOptions& cameraTypeOptions() {
    static const EnumOptions options = {
        {(int)CameraType::CAMERA_ORTHO, "orthographic", "Orthographic"},
        {(int)CameraType::CAMERA_PERSPECTIVE, "perspective", "Perspective"}
    };
    return options;
}

// framebuffers have no mipmaps
const EnumOptions& framebufferFilterOptions() {
    static const EnumOptions options = {
        {(int)TextureFilter::NEAREST, "nearest", "Nearest"},
        {(int)TextureFilter::LINEAR, "linear", "Linear"}
    };
    return options;
}

const EnumOptions& textureFilterOptions() {
    static const EnumOptions options = {
        {(int)TextureFilter::NEAREST, "nearest", "Nearest"},
        {(int)TextureFilter::LINEAR, "linear", "Linear"},
        {(int)TextureFilter::NEAREST_MIPMAP_NEAREST, "nearest_mipmap_nearest", "Nearest Mipmap Nearest"},
        {(int)TextureFilter::NEAREST_MIPMAP_LINEAR, "nearest_mipmap_linear", "Nearest Mipmap Linear"},
        {(int)TextureFilter::LINEAR_MIPMAP_NEAREST, "linear_mipmap_nearest", "Linear Mipmap Nearest"},
        {(int)TextureFilter::LINEAR_MIPMAP_LINEAR, "linear_mipmap_linear", "Linear Mipmap Linear"}
    };
    return options;
}

const EnumOptions& textureWrapOptions() {
    static const EnumOptions options = {
        {(int)TextureWrap::REPEAT, "repeat", "Repeat"},
        {(int)TextureWrap::MIRRORED_REPEAT, "mirrored_repeat", "Mirrored Repeat"},
        {(int)TextureWrap::CLAMP_TO_EDGE, "clamp_to_edge", "Clamp to Edge"},
        {(int)TextureWrap::CLAMP_TO_BORDER, "clamp_to_border", "Clamp to Border"}
    };
    return options;
}

const EnumOptions& textureAlphaBorderOptions() {
    static const EnumOptions options = {
        {(int)TextureAlphaBorder::AUTO, "auto", "Auto"},
        {(int)TextureAlphaBorder::FIX, "fix", "Fix"},
        {(int)TextureAlphaBorder::KEEP, "keep", "Keep"}
    };
    return options;
}

const EnumOptions& audioStateOptions() {
    static const EnumOptions options = {
        {(int)AudioSourceState::Playing, "playing", "Playing"},
        {(int)AudioSourceState::Paused, "paused", "Paused"},
        {(int)AudioSourceState::Stopped, "stopped", "Stopped"}
    };
    return options;
}

const EnumOptions& audioAttenuationOptions() {
    static const EnumOptions options = {
        {(int)AudioSourceAttenuation::NO_ATTENUATION, "no_attenuation", "No Attenuation"},
        {(int)AudioSourceAttenuation::INVERSE_DISTANCE, "inverse_distance", "Inverse Distance"},
        {(int)AudioSourceAttenuation::LINEAR_DISTANCE, "linear_distance", "Linear Distance"},
        {(int)AudioSourceAttenuation::EXPONENTIAL_DISTANCE, "exponential_distance", "Exponential Distance"}
    };
    return options;
}

const EnumOptions& joint2DTypeOptions() {
    static const EnumOptions options = {
        {(int)Joint2DType::DISTANCE, "distance", "Distance"},
        {(int)Joint2DType::REVOLUTE, "revolute", "Revolute"},
        {(int)Joint2DType::PRISMATIC, "prismatic", "Prismatic"},
        {(int)Joint2DType::MOUSE, "mouse", "Mouse"},
        {(int)Joint2DType::WHEEL, "wheel", "Wheel"},
        {(int)Joint2DType::WELD, "weld", "Weld"},
        {(int)Joint2DType::MOTOR, "motor", "Motor"}
    };
    return options;
}

const EnumOptions& joint3DTypeOptions() {
    static const EnumOptions options = {
        {(int)Joint3DType::FIXED, "fixed", "Fixed"},
        {(int)Joint3DType::DISTANCE, "distance", "Distance"},
        {(int)Joint3DType::POINT, "point", "Point"},
        {(int)Joint3DType::HINGE, "hinge", "Hinge"},
        {(int)Joint3DType::CONE, "cone", "Cone"},
        {(int)Joint3DType::PRISMATIC, "prismatic", "Prismatic"},
        {(int)Joint3DType::SWINGTWIST, "swing_twist", "Swing Twist"},
        {(int)Joint3DType::SIXDOF, "six_dof", "Six DOF"},
        {(int)Joint3DType::PATH, "path", "Path"},
        {(int)Joint3DType::GEAR, "gear", "Gear"},
        {(int)Joint3DType::RACKANDPINON, "rack_and_pinion", "Rack and Pinion"},
        {(int)Joint3DType::PULLEY, "pulley", "Pulley"}
    };
    return options;
}

const EnumOptions& layoutGroupTypeOptions() {
    static const EnumOptions options = {
        {(int)LayoutGroupType::VERTICAL, "vertical", "Vertical"},
        {(int)LayoutGroupType::HORIZONTAL, "horizontal", "Horizontal"},
        {(int)LayoutGroupType::VERTICAL_WRAP, "vertical_wrap", "Vertical Wrap"},
        {(int)LayoutGroupType::HORIZONTAL_WRAP, "horizontal_wrap", "Horizontal Wrap"}
    };
    return options;
}

const EnumOptions& alphaModeOptions() {
    static const EnumOptions options = {
        {(int)MaterialAlphaMode::AUTO, "auto", "Auto"},
        {(int)MaterialAlphaMode::ALPHA_OPAQUE, "opaque", "Opaque"},
        {(int)MaterialAlphaMode::MASK, "mask", "Mask"},
        {(int)MaterialAlphaMode::BLEND, "blend", "Blend"}
    };
    return options;
}

const EnumOptions& primitiveTypeOptions() {
    static const EnumOptions options = {
        {(int)PrimitiveType::TRIANGLES, "triangles", "Triangles"},
        {(int)PrimitiveType::TRIANGLE_STRIP, "triangle_strip", "Triangle Strip"},
        {(int)PrimitiveType::POINTS, "points", "Points"},
        {(int)PrimitiveType::LINES, "lines", "Lines"}
    };
    return options;
}

const EnumOptions& bodyTypeOptions() {
    static const EnumOptions options = {
        {(int)BodyType::STATIC, "static", "Static"},
        {(int)BodyType::KINEMATIC, "kinematic", "Kinematic"},
        {(int)BodyType::DYNAMIC, "dynamic", "Dynamic"}
    };
    return options;
}

const EnumOptions& motionQualityOptions() {
    static const EnumOptions options = {
        {(int)RigidbodyMotionQuality::DISCRETE, "discrete", "Discrete"},
        {(int)RigidbodyMotionQuality::LINEAR_CAST, "linear_cast", "Linear Cast"}
    };
    return options;
}

const EnumOptions& collider2DTypeOptions() {
    static const EnumOptions options = {
        {(int)Collider2DType::POLYGON, "polygon", "Polygon"},
        {(int)Collider2DType::CIRCLE, "circle", "Circle"},
        {(int)Collider2DType::CAPSULE, "capsule", "Capsule"},
        {(int)Collider2DType::SEGMENT, "segment", "Segment"},
        {(int)Collider2DType::CHAIN, "chain", "Chain"}
    };
    return options;
}

const EnumOptions& collider3DTypeOptions() {
    static const EnumOptions options = {
        {(int)Collider3DType::BOX, "box", "Box"},
        {(int)Collider3DType::SPHERE, "sphere", "Sphere"},
        {(int)Collider3DType::CAPSULE, "capsule", "Capsule"},
        {(int)Collider3DType::TAPERED_CAPSULE, "tapered_capsule", "Tapered Capsule"},
        {(int)Collider3DType::CYLINDER, "cylinder", "Cylinder"},
        {(int)Collider3DType::CONVEX_HULL, "convex_hull", "Convex Hull"},
        {(int)Collider3DType::MESH, "mesh", "Mesh"},
        {(int)Collider3DType::HEIGHTFIELD, "heightfield", "Heightfield"}
    };
    return options;
}

const EnumOptions& collider3DSourceOptions() {
    static const EnumOptions options = {
        {(int)Collider3DSource::NONE, "none", "None"},
        {(int)Collider3DSource::RAW_VERTICES, "raw_vertices", "Raw Vertices"},
        {(int)Collider3DSource::RAW_MESH, "raw_mesh", "Raw Mesh"},
        {(int)Collider3DSource::ENTITY_MESH, "entity_mesh", "GameObject Mesh"},
        {(int)Collider3DSource::ENTITY_HEIGHTFIELD, "entity_heightfield", "GameObject Heightfield"}
    };
    return options;
}

const EnumOptions& emitterShapeOptions() {
    static const EnumOptions options = {
        {(int)ParticleEmitterShape::Box, "box", "Box"},
        {(int)ParticleEmitterShape::Sphere, "sphere", "Sphere"},
        {(int)ParticleEmitterShape::Hemisphere, "hemisphere", "Hemisphere"},
        {(int)ParticleEmitterShape::Circle, "circle", "Circle"},
        {(int)ParticleEmitterShape::Cone, "cone", "Cone"}
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
        {(int)ShadowQuality::NONE, "none", "None"},
        {(int)ShadowQuality::LOW, "low", "Low"},
        {(int)ShadowQuality::MEDIUM, "medium", "Medium"},
        {(int)ShadowQuality::HIGH, "high", "High"}
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
    }
    return "unknown";
}

// ==================================================================== enums

const McpValues::EnumOptions& McpValues::easeOptions() {
    static const EnumOptions options = {
        {(int)EaseType::LINEAR, "linear", "Linear"},
        {(int)EaseType::STEP, "step", "Step"},
        {(int)EaseType::QUAD_IN, "quad_in", "Quad In"},
        {(int)EaseType::QUAD_OUT, "quad_out", "Quad Out"},
        {(int)EaseType::QUAD_IN_OUT, "quad_in_out", "Quad In Out"},
        {(int)EaseType::CUBIC_IN, "cubic_in", "Cubic In"},
        {(int)EaseType::CUBIC_OUT, "cubic_out", "Cubic Out"},
        {(int)EaseType::CUBIC_IN_OUT, "cubic_in_out", "Cubic In Out"},
        {(int)EaseType::QUART_IN, "quart_in", "Quart In"},
        {(int)EaseType::QUART_OUT, "quart_out", "Quart Out"},
        {(int)EaseType::QUART_IN_OUT, "quart_in_out", "Quart In Out"},
        {(int)EaseType::QUINT_IN, "quint_in", "Quint In"},
        {(int)EaseType::QUINT_OUT, "quint_out", "Quint Out"},
        {(int)EaseType::QUINT_IN_OUT, "quint_in_out", "Quint In Out"},
        {(int)EaseType::SINE_IN, "sine_in", "Sine In"},
        {(int)EaseType::SINE_OUT, "sine_out", "Sine Out"},
        {(int)EaseType::SINE_IN_OUT, "sine_in_out", "Sine In Out"},
        {(int)EaseType::EXPO_IN, "expo_in", "Expo In"},
        {(int)EaseType::EXPO_OUT, "expo_out", "Expo Out"},
        {(int)EaseType::EXPO_IN_OUT, "expo_in_out", "Expo In Out"},
        {(int)EaseType::CIRC_IN, "circ_in", "Circ In"},
        {(int)EaseType::CIRC_OUT, "circ_out", "Circ Out"},
        {(int)EaseType::CIRC_IN_OUT, "circ_in_out", "Circ In Out"},
        {(int)EaseType::ELASTIC_IN, "elastic_in", "Elastic In"},
        {(int)EaseType::ELASTIC_OUT, "elastic_out", "Elastic Out"},
        {(int)EaseType::ELASTIC_IN_OUT, "elastic_in_out", "Elastic In Out"},
        {(int)EaseType::BACK_IN, "back_in", "Back In"},
        {(int)EaseType::BACK_OUT, "back_out", "Back Out"},
        {(int)EaseType::BACK_IN_OUT, "back_in_out", "Back In Out"},
        {(int)EaseType::BOUNCE_IN, "bounce_in", "Bounce In"},
        {(int)EaseType::BOUNCE_OUT, "bounce_out", "Bounce Out"},
        {(int)EaseType::BOUNCE_IN_OUT, "bounce_in_out", "Bounce In Out"}
    };
    return options;
}

const McpValues::EnumOptions* McpValues::enumTable(const std::string& name) {
    static const std::map<std::string, const EnumOptions*> tables = {
        {"light_state", &lightStateOptions()},
        {"shadow_quality", &shadowQualityOptions()},
        {"texture_filter", &textureFilterOptions()},
        {"framebuffer_filter", &framebufferFilterOptions()},
        {"texture_wrap", &textureWrapOptions()},
        {"texture_alpha_border", &textureAlphaBorderOptions()},
        {"material_alpha_mode", &alphaModeOptions()},
        {"primitive_type", &primitiveTypeOptions()},
        {"body_type", &bodyTypeOptions()},
        {"collider2d_type", &collider2DTypeOptions()},
        {"collider3d_type", &collider3DTypeOptions()},
        {"light_type", &lightTypeOptions()},
        {"camera_type", &cameraTypeOptions()},
        {"ease", &easeOptions()}
    };
    auto it = tables.find(name);
    return it != tables.end() ? it->second : nullptr;
}

const McpValues::EnumOptions* McpValues::enumOptions(ComponentType component, const std::string& propertyName) {
    const std::string name = stripIndices(propertyName);
    if (name == "easings[]") return &easeOptions();
    switch (component) {
        case ComponentType::RectTransformComponent:
            if (name == "anchorPreset") return &anchorPresetOptions();
            break;
        case ComponentType::PanelComponent:
            if (name == "titleAnchorPreset") return &anchorPresetOptions();
            break;
        case ComponentType::ScrollbarComponent:
            if (name == "type") return &orientationOptions();
            break;
        case ComponentType::ProgressbarComponent:
            if (name == "type") return &progressbarTypeOptions();
            break;
        case ComponentType::SpriteComponent:
            if (name == "pivotPreset") return &pivotPresetOptions();
            break;
        case ComponentType::ActionComponent:
            if (name == "state") return &actionStateOptions();
            break;
        case ComponentType::LightComponent:
            if (name == "type") return &lightTypeOptions();
            break;
        case ComponentType::ShadowCaster2DComponent:
            if (name == "shape") return &shadowCasterShapeOptions();
            break;
        case ComponentType::FogComponent:
            if (name == "type") return &fogTypeOptions();
            break;
        case ComponentType::ReflectionProbeComponent:
            if (name == "mode") return &probeModeOptions();
            if (name == "updateMode") return &probeUpdateModeOptions();
            break;
        case ComponentType::CameraComponent:
            if (name == "type") return &cameraTypeOptions();
            if (name == "framebufferFilter") return &framebufferFilterOptions();
            break;
        case ComponentType::AudioSourceComponent:
            if (name == "state") return &audioStateOptions();
            if (name == "attenuationModel") return &audioAttenuationOptions();
            break;
        case ComponentType::Joint2DComponent:
            if (name == "type") return &joint2DTypeOptions();
            break;
        case ComponentType::Joint3DComponent:
            if (name == "type") return &joint3DTypeOptions();
            break;
        case ComponentType::LayoutGroupComponent:
            if (name == "type") return &layoutGroupTypeOptions();
            break;
        case ComponentType::MeshComponent:
            if (name == "submeshes[].material.alphaMode") return &alphaModeOptions();
            if (name == "submeshes[].primitiveType") return &primitiveTypeOptions();
            break;
        case ComponentType::Rigidbody2DComponent:
            if (name == "type") return &bodyTypeOptions();
            if (name == "colliders[].type") return &collider2DTypeOptions();
            break;
        case ComponentType::RigidbodyComponent:
            if (name == "type") return &bodyTypeOptions();
            if (name == "motionQuality") return &motionQualityOptions();
            if (name == "colliders[].type") return &collider3DTypeOptions();
            if (name == "colliders[].source") return &collider3DSourceOptions();
            break;
        case ComponentType::ParticleSystemComponent:
            if (name == "positionInitializer.shape") return &emitterShapeOptions();
            break;
        default:
            break;
    }
    return nullptr;
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
        {"ortho", "orthographic"}, {"perspec", "perspective"}, {"exponentialsquared", "exponentialsquared"},
        {"alphaopaque", "opaque"}, {"none", "noattenuation"}, {"gameobjectmesh", "entitymesh"},
        {"gameobjectheightfield", "entityheightfield"}, {"sixdof", "sixdof"}, {"rackandpinon", "rackandpinion"}
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

double McpValues::number(float value) { return cleanFloat(value); }
Json McpValues::toJson(const Vector2& value) { return Json::array({cleanFloat(value.x), cleanFloat(value.y)}); }
Json McpValues::toJson(const Vector3& value) { return Json::array({cleanFloat(value.x), cleanFloat(value.y), cleanFloat(value.z)}); }
Json McpValues::toJson(const Vector4& value) {
    return Json::array({cleanFloat(value.x), cleanFloat(value.y), cleanFloat(value.z), cleanFloat(value.w)});
}
Json McpValues::toJson(const Quaternion& value) {
    return Json{{"quat", Json::array({cleanFloat(value.x), cleanFloat(value.y), cleanFloat(value.z), cleanFloat(value.w)})},
                {"euler", toJson(eulerDegrees(value))}};
}

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
        ref = value;
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
            if (texture.getSvgScale() != 1.0f) out["svg_scale"] = cleanFloat(texture.getSvgScale());
        }
    } else {
        bool anyOtherFace = false;
        for (size_t face = 1; face < 6; face++) {
            if (!texture.getPath(face).empty()) anyOtherFace = true;
        }
        if (!anyOtherFace && !texture.getPath(0).empty()) {
            out["cube"] = assetPathToJson(project, texture.getPath(0));
        } else {
            Json faces = Json::array();
            for (size_t face = 0; face < 6; face++) faces.push_back(assetPathToJson(project, texture.getPath(face)));
            out["cube"] = faces;
        }
    }
    out["min_filter"] = enumToJson((int)texture.getMinFilter(), textureFilterOptions());
    out["mag_filter"] = enumToJson((int)texture.getMagFilter(), textureFilterOptions());
    out["wrap_u"] = enumToJson((int)texture.getWrapU(), textureWrapOptions());
    out["wrap_v"] = enumToJson((int)texture.getWrapV(), textureWrapOptions());
    out["alpha_border"] = enumToJson((int)texture.getAlphaBorder(), textureAlphaBorderOptions());
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
    if (value.is_string()) {
        std::string stored;
        if (!storedPath(value, stored)) return false;
        out = Texture(stored);
        return true;
    }
    if (!value.is_object()) {
        error = "a texture is null, a project-relative path, {\"path\": ..., \"min_filter\": ...}, "
                "{\"cube\": [6 paths] or \"path\"} or {\"camera\": <entity>}.";
        return false;
    }
    static const std::set<std::string> keys = {"path", "cube", "camera", "id", "min_filter", "mag_filter", "wrap_u",
                                               "wrap_v", "alpha_border", "svg_scale"};
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!keys.count(it.key())) {
            error = "unknown texture key \"" + it.key() + "\"; valid: path, cube, camera, min_filter, mag_filter, "
                    "wrap_u, wrap_v, alpha_border, svg_scale.";
            return false;
        }
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
    if (!applyEnum("min_filter", textureFilterOptions(), [&](int v) { out.setMinFilter(static_cast<TextureFilter>(v)); })) return false;
    if (!applyEnum("mag_filter", framebufferFilterOptions(), [&](int v) { out.setMagFilter(static_cast<TextureFilter>(v)); })) return false;
    if (!applyEnum("wrap_u", textureWrapOptions(), [&](int v) { out.setWrapU(static_cast<TextureWrap>(v)); })) return false;
    if (!applyEnum("wrap_v", textureWrapOptions(), [&](int v) { out.setWrapV(static_cast<TextureWrap>(v)); })) return false;
    if (!applyEnum("alpha_border", textureAlphaBorderOptions(), [&](int v) { out.setAlphaBorder(static_cast<TextureAlphaBorder>(v)); })) return false;
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
        {"metallicFactor", cleanFloat(material.metallicFactor)},
        {"roughnessFactor", cleanFloat(material.roughnessFactor)},
        {"alphaCutoff", cleanFloat(material.alphaCutoff)},
        {"foliageTransmission", cleanFloat(material.foliageTransmission)},
        {"alphaMode", enumToJson((int)material.alphaMode, alphaModeOptions())},
        {"emissiveFactor", toJson(material.emissiveFactor)}
    };
    for (const MaterialTextureField& field : MATERIAL_TEXTURES) {
        const Texture& texture = material.*field.texture;
        Json json = textureToJson(project, scene, texture);
        if (!json.is_null()) out[field.name] = json;
        if (material.*field.texCoord != 0) out[field.texCoordName] = material.*field.texCoord;
    }
    if (!linkedFile.empty()) {
        out["file"] = project ? project->normalizeToProjectRelative(project->getProjectPath() / linkedFile).generic_string()
                              : linkedFile;
    }
    return out;
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
                     key == "emissiveFactor" || key == "file";
        for (const MaterialTextureField& field : MATERIAL_TEXTURES) {
            if (key == field.name || key == field.texCoordName) known = true;
        }
        if (!known) {
            error = "unknown material key \"" + key + "\"; valid: name, baseColorFactor, metallicFactor, roughnessFactor, "
                    "alphaCutoff, foliageTransmission, alphaMode, emissiveFactor, baseColorTexture, emissiveTexture, "
                    "metallicRoughnessTexture, occlusionTexture, normalTexture, <texture>TexCoord, file.";
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
    if (value.contains("alphaMode")) {
        int mode = 0;
        std::string why;
        if (!enumFromJson(value["alphaMode"], alphaModeOptions(), mode, why)) {
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
        bool found = false;
        Json value = readYamlProperty(ctx, name, found);
        return found ? value : Json(nullptr);
    }
    if (!ref) return nullptr;
    switch (property.type) {
        case PropertyType::Bool:
            return *static_cast<bool*>(ref);
        case PropertyType::String: {
            const std::string& value = *static_cast<std::string*>(ref);
            return isAssetPathProperty(name) ? assetPathToJson(ctx.project, value) : value;
        }
        case PropertyType::Float:
            return cleanFloat(*static_cast<float*>(ref));
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

Command* McpValues::buildPropertyCommand(Project* project, uint32_t sceneId, Entity entity, ComponentType component,
                                         const std::string& propertyName, const PropertyData& property,
                                         const Json& value, std::string& error, std::function<void()> onChanged) {
    SceneProject* scene = project ? project->getScene(sceneId) : nullptr;
    if (!scene || !scene->scene) {
        error = "The scene is not loaded.";
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
        case PropertyType::String: {
            if (!value.is_string()) return expect("a string");
            std::string text = value.get<std::string>();
            if (component == ComponentType::ModelComponent && propertyName == "filename") {
                // a model is its file: setting it loads the file (Inspector / drag and drop)
                std::string stored;
                if (!assetPathFromJson(project, text, stored, error)) return nullptr;
                if (stored.empty()) {
                    ModelComponent* model = scene->scene->findComponent<ModelComponent>(entity);
                    if (!model || !ModelClearCmd::hasContent(*model)) {
                        return new PropertyCmd<std::string>(project, sceneId, entity, component, propertyName, std::string(), onChanged);
                    }
                    std::string reason;
                    if (!ModelClearCmd::canClear(project, sceneId, entity, &reason)) {
                        error = "The model can't be emptied: " + reason;
                        return nullptr;
                    }
                    return new ModelClearCmd(project, sceneId, entity);
                }
                if (!Util::isModelFile(stored) || !fs::exists(project->resolveAssetPath(stored))) {
                    error = "filename must be an existing .gltf, .glb or .obj file in the assets directory.";
                    return nullptr;
                }
                return new ModelLoadCmd(project, sceneId, entity, stored);
            }
            if (isAssetPathProperty(propertyName) && !assetPathFromJson(project, text, text, error)) return nullptr;
            if (isShaderPathProperty(propertyName) && !text.empty()) {
                Util::CustomShaderPaths paths = Util::resolveCustomShaderPaths(text);
                if (!fs::exists(project->getProjectPath() / paths.vert) || !fs::exists(project->getProjectPath() / paths.frag)) {
                    error = propertyName + " references missing files (" + paths.vert + ", " + paths.frag +
                            "): fork the built-in shader first.";
                    return nullptr;
                }
            }
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
        case PropertyType::Int:
            if (!isIntegral(value)) return expect("an integer");
            return new PropertyCmd<int>(project, sceneId, entity, component, propertyName,
                                        static_cast<int>(integralValue(value)), onChanged);
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
                    material = Stream::decodeMaterial(YAML::LoadFile(absolute.string()));
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
            if (propertyName == "shaderUniforms") {
                ShaderUniformValues uniforms;
                std::string why;
                if (!shaderUniformsFromJson(value, uniforms, why)) {
                    error = "Property " + propertyName + ": " + why;
                    return nullptr;
                }
                return new PropertyCmd<ShaderUniformValues>(project, sceneId, entity, component, propertyName, uniforms, onChanged);
            }
            error = "Property " + propertyName + " is custom: set it with set_properties (component encoding).";
            return nullptr;
    }
    error = "Unsupported property type.";
    return nullptr;
}

bool McpValues::parseChanges(const Json& changes, std::vector<Change>& out, std::string& error) {
    out.clear();
    auto component = [&](const Json& name, ComponentType& type) {
        if (!McpRefs::resolveComponent(name, type, error)) return false;
        return true;
    };
    if (changes.is_object()) {
        // {component: {property: value}}
        for (auto it = changes.begin(); it != changes.end(); ++it) {
            Change base;
            if (!component(it.key(), base.component)) return false;
            if (!it.value().is_object()) {
                error = "values[\"" + it.key() + "\"] must be an object {property: value}.";
                return false;
            }
            for (auto prop = it.value().begin(); prop != it.value().end(); ++prop) {
                Change change = base;
                change.property = prop.key();
                change.value = prop.value();
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
        if (!component(item["component"], change.component)) {
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
        out.push_back(change);
    }
    if (out.empty()) {
        error = "changes is empty.";
        return false;
    }
    return true;
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
            std::vector<YamlEdit> yamlEdits;
            std::vector<std::unique_ptr<Command>> typed;
            for (const Change& change : changes) {
                if (change.component != component) continue;
                const PropertyData property = Catalog::findProperty(scene->scene, entity, component, change.property);
                const bool typedProperty = property.ref && change.op == "set" &&
                    (property.type != PropertyType::Custom || change.property == "shaderUniforms");
                if (typedProperty) {
                    std::string why;
                    Command* cmd = buildPropertyCommand(project, scene->id, entity, component, change.property, property, change.value, why);
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
            if (!yamlEdits.empty()) {
                YAML::Node value = YAML::Clone(EntityClipboard::encodeComponent(scene->scene, entity, component));
                for (const YamlEdit& edit : yamlEdits) {
                    std::string why;
                    if (!editYamlPath(value, edit.property, edit.op, edit.value, edit.index, why, edit.known)) {
                        if (!findYamlPath(value, edit.property)) {
                            error = "Unknown property \"" + edit.property + "\" of " + componentLabel + " on " + label +
                                    ". Properties: " + propertyNamesText(scene, entity, component) +
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
            for (auto it = done.rbegin(); it != done.rend(); ++it) {
                (*it)->undo();
                refresh();
            }
            done.clear();
            return false;
        }
        done.emplace_back(cmd);
        refresh();
    }
    built = true;
    return true;
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
