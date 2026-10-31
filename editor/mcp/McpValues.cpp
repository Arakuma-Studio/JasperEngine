// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpValues.h"
#include "particles/ParticleFields.h"

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
#include "component/AnimationPlayerComponent.h"
#include "animation/AnimationClipIO.h"
#include "animation/ModelSubClips.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UISystem.h"
#include "util/EntityPath.h"
#include "texture/TextureData.h"
#include "util/CameraTextureLink.h"
#include "util/Color.h"
#include "util/FluidTextureLink.h"
#include "component/FluidSimulationComponent.h"
#include "fluid/FluidPresets.h"
#include "fluid/FluidSimulator.h"
#include "util/EntityClipboard.h"
#include "util/FbxImporter.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "util/Util.h"

#include <algorithm>
#include <limits>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
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

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// An Animation Player's clips[N].clip: a .anim path or "<model>#<clip>"
bool isPlayerClipProperty(ComponentType component, const std::string& propertyName) {
    return component == ComponentType::AnimationPlayerComponent && propertyName.compare(0, 6, "clips[") == 0 &&
           endsWith(propertyName, "].clip");
}

// clips[N].guid: read-only over MCP (derived from the file or the AssetDatabase)
bool isPlayerGuidProperty(ComponentType component, const std::string& propertyName) {
    return component == ComponentType::AnimationPlayerComponent && propertyName.compare(0, 6, "clips[") == 0 &&
           endsWith(propertyName, "].guid");
}

// clips[N].wrap: a ClipWrapMode (Catalog: Custom, one byte), set and read by its file name
bool isPlayerWrapProperty(ComponentType component, const std::string& propertyName) {
    return component == ComponentType::AnimationPlayerComponent && propertyName.compare(0, 6, "clips[") == 0 &&
           endsWith(propertyName, "].wrap");
}

// A Vertex Animator's animation: a .vat (its GUID follows it, read-only over MCP)
bool isVertexAnimationProperty(ComponentType component, const std::string& propertyName) {
    return component == ComponentType::VertexAnimatorComponent && propertyName == "animation";
}

// String properties holding an asset reference instead of plain text: stored relative to the
// assets folder, project-relative over MCP (a model's or document's filename, a Pose Animator's
// animation, an Animation Player's clip, a Vertex Animator's animation)
// A Rigidbody 2D's or one of its colliders' physics material (a .physicsmaterial)
bool isPhysicsMaterialProperty(ComponentType component, const std::string& propertyName) {
    return (component == ComponentType::Rigidbody2DComponent || component == ComponentType::RigidbodyComponent) &&
           (propertyName == "material" || (propertyName.compare(0, 10, "colliders[") == 0 && endsWith(propertyName, "].material")));
}

bool isAssetPathProperty(ComponentType component, const std::string& propertyName) {
    return propertyName == "filename" || (component == ComponentType::PoseAnimatorComponent && propertyName == "animation") ||
           isPlayerClipProperty(component, propertyName) || isVertexAnimationProperty(component, propertyName) ||
           isPhysicsMaterialProperty(component, propertyName);
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
        case ComponentType::TrailRendererComponent: return ShaderType::LINES;
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
    static_assert(static_cast<int>(ScrollbarType::VERTICAL) == static_cast<int>(ProgressbarType::VERTICAL));
    static_assert(static_cast<int>(ScrollbarType::HORIZONTAL) == static_cast<int>(ProgressbarType::HORIZONTAL));
    static const EnumOptions options = {
        {(int)ScrollbarType::VERTICAL, "vertical", "Vertical"},
        {(int)ScrollbarType::HORIZONTAL, "horizontal", "Horizontal"}
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

const EnumOptions& cullingModeOptions() {
    static const EnumOptions options = {
        {(int)CullingMode::BACK, "back", "Back"},
        {(int)CullingMode::FRONT, "front", "Front"}
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

const EnumOptions& lineModeOptions() {
    static const EnumOptions options = {
        {(int)LineRendererMode::Segments, "segments", "Segments"},
        {(int)LineRendererMode::Polyline, "polyline", "Polyline"}
    };
    return options;
}

const EnumOptions& lineWidthUnitOptions() {
    static const EnumOptions options = {
        {(int)LineWidthUnit::World, "world", "World"},
        {(int)LineWidthUnit::Pixels, "pixels", "Pixels"}
    };
    return options;
}

const EnumOptions& lineAlignmentOptions() {
    static const EnumOptions options = {
        {(int)LineAlignment::View, "view", "View"},
        {(int)LineAlignment::TransformZ, "transform_z", "Transform Z"}
    };
    return options;
}

const EnumOptions& lineTextureModeOptions() {
    static const EnumOptions options = {
        {(int)LineTextureMode::Stretch, "stretch", "Stretch"},
        {(int)LineTextureMode::Tile, "tile", "Tile"},
        {(int)LineTextureMode::DistributePerSegment, "distribute_per_segment", "Distribute Per Segment"},
        {(int)LineTextureMode::RepeatPerSegment, "repeat_per_segment", "Repeat Per Segment"}
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

const EnumOptions& uiDocumentRenderModeOptions() {
    static const EnumOptions options = {
        {(int)UIDocumentRenderMode::SCREEN, "screen", "Screen"},
        {(int)UIDocumentRenderMode::TEXTURE, "texture", "Texture"}
    };
    return options;
}

const EnumOptions& fluidOutputOptions() {
    static const EnumOptions options = {
        {(int)FluidOutput::SCREEN, "screen", "Screen"},
        {(int)FluidOutput::MATERIAL, "material", "Material"},
        {(int)FluidOutput::TEXTURE, "texture", "Texture"}
    };
    return options;
}

// the names are the scene-file ones (Stream, spec §2.0)
const EnumOptions& latticeInterpolationOptions() {
    static const EnumOptions options = {
        {(int)LatticeInterpolation::SHARP, "sharp", "Sharp"},
        {(int)LatticeInterpolation::SMOOTH, "smooth", "Smooth"},
        {(int)LatticeInterpolation::CUBIC, "cubic", "Cubic"}
    };
    return options;
}

const EnumOptions& latticeOutsideOptions() {
    static const EnumOptions options = {
        {(int)LatticeOutside::EXTEND, "extend", "Extend"},
        {(int)LatticeOutside::FADE, "fade", "Fade"}
    };
    return options;
}

const EnumOptions& latticeJiggleAnchorOptions() {
    static const EnumOptions options = {
        {(int)LatticeJiggleAnchor::NONE, "none", "None"},
        {(int)LatticeJiggleAnchor::NEGATIVE_X, "negative_x", "-X"},
        {(int)LatticeJiggleAnchor::POSITIVE_X, "positive_x", "+X"},
        {(int)LatticeJiggleAnchor::NEGATIVE_Y, "negative_y", "-Y"},
        {(int)LatticeJiggleAnchor::POSITIVE_Y, "positive_y", "+Y"},
        {(int)LatticeJiggleAnchor::NEGATIVE_Z, "negative_z", "-Z"},
        {(int)LatticeJiggleAnchor::POSITIVE_Z, "positive_z", "+Z"}
    };
    return options;
}

const EnumOptions& latticeStageOptions() {
    static const EnumOptions options = {
        {(int)LatticeStage::BEFORE_SKINNING, "before_skinning", "Before Skinning"},
        {(int)LatticeStage::AFTER_SKINNING, "after_skinning", "After Skinning"}
    };
    return options;
}

const EnumOptions& latticeMaskOptions() {
    static const EnumOptions options = {
        {(int)LatticeMaskChannel::NONE, "none", "None"},
        {(int)LatticeMaskChannel::RED, "red", "Red"},
        {(int)LatticeMaskChannel::GREEN, "green", "Green"},
        {(int)LatticeMaskChannel::BLUE, "blue", "Blue"},
        {(int)LatticeMaskChannel::ALPHA, "alpha", "Alpha"},
        {(int)LatticeMaskChannel::INVERTED_RED, "inverted_red", "Inverted Red"},
        {(int)LatticeMaskChannel::INVERTED_GREEN, "inverted_green", "Inverted Green"},
        {(int)LatticeMaskChannel::INVERTED_BLUE, "inverted_blue", "Inverted Blue"},
        {(int)LatticeMaskChannel::INVERTED_ALPHA, "inverted_alpha", "Inverted Alpha"}
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

const EnumOptions& jointAnchorSpaceOptions() {
    static const EnumOptions options = {
        {(int)JointAnchorSpace::WORLD, "world", "World"},
        {(int)JointAnchorSpace::LOCAL, "local", "Local"}
    };
    return options;
}

const EnumOptions& jointMotionOptions() {
    static const EnumOptions options = {
        {(int)JointMotion::LOCKED, "locked", "Locked"},
        {(int)JointMotion::LIMITED, "limited", "Limited"},
        {(int)JointMotion::FREE, "free", "Free"}
    };
    return options;
}

const EnumOptions& jointSwingTypeOptions() {
    static const EnumOptions options = {
        {(int)JointSwingType::CONE, "cone", "Cone"},
        {(int)JointSwingType::PYRAMID, "pyramid", "Pyramid"}
    };
    return options;
}

const EnumOptions& rigidbodyInterpolationOptions() {
    static const EnumOptions options = {
        {(int)RigidbodyInterpolation::SCENE, "scene", "Scene"},
        {(int)RigidbodyInterpolation::NONE, "none", "None"},
        {(int)RigidbodyInterpolation::INTERPOLATE, "interpolate", "Interpolate"},
        {(int)RigidbodyInterpolation::EXTRAPOLATE, "extrapolate", "Extrapolate"}
    };
    return options;
}

const EnumOptions& jointBreakActionOptions() {
    static const EnumOptions options = {
        {(int)JointBreakAction::IGNORE, "ignore", "Ignore"},
        {(int)JointBreakAction::CALLBACK_ONLY, "callback_only", "Callback Only"},
        {(int)JointBreakAction::DISABLE, "disable", "Disable"},
        {(int)JointBreakAction::DESTROY, "destroy", "Destroy"}
    };
    return options;
}

const EnumOptions& collisionDetection2DOptions() {
    static const EnumOptions options = {
        {(int)CollisionDetectionMode2D::DISCRETE, "discrete", "Discrete"},
        {(int)CollisionDetectionMode2D::CONTINUOUS, "continuous", "Continuous"}
    };
    return options;
}

const EnumOptions& sleepMode2DOptions() {
    static const EnumOptions options = {
        {(int)RigidbodySleepMode2D::NEVER_SLEEP, "never_sleep", "Never Sleep"},
        {(int)RigidbodySleepMode2D::START_AWAKE, "start_awake", "Start Awake"},
        {(int)RigidbodySleepMode2D::START_ASLEEP, "start_asleep", "Start Asleep"}
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

const EnumOptions& canvasGroupRenderModeOptions() {
    static const EnumOptions options = {
        {(int)CanvasGroupRenderMode::PER_ELEMENT, "per_element", "Per Element"},
        {(int)CanvasGroupRenderMode::FLATTEN, "flatten", "Flatten"}
    };
    return options;
}

const EnumOptions& alphaTweenTargetOptions() {
    static const EnumOptions options = {
        {(int)AlphaTweenTarget::AUTO, "auto", "Auto"},
        {(int)AlphaTweenTarget::COLOR, "color", "Color"},
        {(int)AlphaTweenTarget::CANVAS_GROUP, "canvas_group", "Canvas Group"}
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

// Shader library render state (Material, UI/Points/LineRenderer) and lighting. NONE blends
// nothing: it reads "opaque" (the "None" label still parses)
const EnumOptions& blendModeOptions() {
    static const EnumOptions options = {
        {(int)MaterialBlendMode::AUTO, "auto", "Auto"},
        {(int)MaterialBlendMode::NONE, "opaque", "None"},
        {(int)MaterialBlendMode::ALPHA, "alpha", "Alpha"},
        {(int)MaterialBlendMode::PREMULTIPLIED, "premultiplied", "Premultiplied"},
        {(int)MaterialBlendMode::ADDITIVE, "additive", "Additive"},
        {(int)MaterialBlendMode::MULTIPLY, "multiply", "Multiply"}
    };
    return options;
}

const EnumOptions& materialCullModeOptions() {
    static const EnumOptions options = {
        {(int)MaterialCullMode::AUTO, "auto", "Auto"},
        {(int)MaterialCullMode::BACK, "back", "Back"},
        {(int)MaterialCullMode::FRONT, "front", "Front"},
        {(int)MaterialCullMode::OFF, "off", "Off"}
    };
    return options;
}

const EnumOptions& depthWriteOptions() {
    static const EnumOptions options = {
        {(int)MaterialDepthWrite::AUTO, "auto", "Auto"},
        {(int)MaterialDepthWrite::ENABLED, "on", "Enabled"},
        {(int)MaterialDepthWrite::DISABLED, "off", "Disabled"}
    };
    return options;
}

const EnumOptions& depthTestOptions() {
    static const EnumOptions options = {
        {(int)MaterialDepthTest::AUTO, "auto", "Auto"},
        {(int)MaterialDepthTest::LESS, "less", "Less"},
        {(int)MaterialDepthTest::LESS_EQUAL, "less_equal", "Less Equal"},
        {(int)MaterialDepthTest::EQUAL, "equal", "Equal"},
        {(int)MaterialDepthTest::GREATER_EQUAL, "greater_equal", "Greater Equal"},
        {(int)MaterialDepthTest::GREATER, "greater", "Greater"},
        {(int)MaterialDepthTest::NOT_EQUAL, "not_equal", "Not Equal"},
        {(int)MaterialDepthTest::ALWAYS, "always", "Always"}
    };
    return options;
}

const EnumOptions& materialLightingOptions() {
    static const EnumOptions options = {
        {(int)MaterialLighting::AUTO, "auto", "Auto"},
        {(int)MaterialLighting::PIXEL, "pixel", "Per Pixel"},
        {(int)MaterialLighting::VERTEX, "vertex", "Per Vertex"},
        {(int)MaterialLighting::MIXED, "mixed", "Mixed"},
        {(int)MaterialLighting::NONE, "none", "None"}
    };
    return options;
}

// phase 2: stencil test, stencil ops and the extra pass order
const EnumOptions& stencilCompareOptions() {
    static const EnumOptions options = {
        {(int)MaterialStencilCompare::AUTO, "auto", "Auto"},
        {(int)MaterialStencilCompare::DISABLED, "disabled", "Off"},
        {(int)MaterialStencilCompare::NEVER, "never", "Never"},
        {(int)MaterialStencilCompare::LESS, "less", "Less"},
        {(int)MaterialStencilCompare::EQUAL, "equal", "Equal"},
        {(int)MaterialStencilCompare::LESS_EQUAL, "less_equal", "Less Equal"},
        {(int)MaterialStencilCompare::GREATER, "greater", "Greater"},
        {(int)MaterialStencilCompare::NOT_EQUAL, "not_equal", "Not Equal"},
        {(int)MaterialStencilCompare::GREATER_EQUAL, "greater_equal", "Greater Equal"},
        {(int)MaterialStencilCompare::ALWAYS, "always", "Always"}
    };
    return options;
}

const EnumOptions& stencilOpOptions() {
    static const EnumOptions options = {
        {(int)MaterialStencilOp::KEEP, "keep", "Keep"},
        {(int)MaterialStencilOp::ZERO, "zero", "Zero"},
        {(int)MaterialStencilOp::REPLACE, "replace", "Replace"},
        {(int)MaterialStencilOp::INCR_CLAMP, "incr_clamp", "Increment"},
        {(int)MaterialStencilOp::DECR_CLAMP, "decr_clamp", "Decrement"},
        {(int)MaterialStencilOp::INVERT, "invert", "Invert"},
        {(int)MaterialStencilOp::INCR_WRAP, "incr_wrap", "Increment Wrap"},
        {(int)MaterialStencilOp::DECR_WRAP, "decr_wrap", "Decrement Wrap"}
    };
    return options;
}

const EnumOptions& passOrderOptions() {
    static const EnumOptions options = {
        {(int)MaterialPassOrder::AFTER, "after", "After"},
        {(int)MaterialPassOrder::BEFORE, "before", "Before"}
    };
    return options;
}

const EnumOptions& lightShadingOptions() {
    static const EnumOptions options = {
        {(int)LightShading::BOTH, "both", "Both"},
        {(int)LightShading::PIXEL, "pixel", "Per Pixel"},
        {(int)LightShading::VERTEX, "vertex", "Per Vertex"}
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

// a version 2 module enum (particles/ParticleFields): Unity's value names as snake_case
// ("ConeVolume" -> "cone_volume"), labelled with spaces ("Cone Volume")
const EnumOptions& particleEnumOptions(const particles::FieldInfo& field) {
    static std::map<const std::vector<std::string>*, std::pair<std::vector<std::string>, EnumOptions>> tables;
    auto it = tables.find(field.enumNames);
    if (it != tables.end()) return it->second.second;
    auto& entry = tables[field.enumNames];
    std::vector<std::string>& strings = entry.first;
    // names and labels kept alive next to the options that point at them
    strings.reserve(field.enumNames->size() * 2);
    for (const std::string& unity : *field.enumNames) {
        std::string snake, label;
        for (size_t i = 0; i < unity.size(); i++) {
            const char c = unity[i];
            const bool upper = std::isupper((unsigned char)c);
            const bool prevLower = i > 0 && (std::islower((unsigned char)unity[i - 1]) || std::isdigit((unsigned char)unity[i - 1]));
            const bool nextLower = i + 1 < unity.size() && std::islower((unsigned char)unity[i + 1]);
            const bool prevUpper = i > 0 && std::isupper((unsigned char)unity[i - 1]);
            if (upper && i > 0 && (prevLower || (prevUpper && nextLower))) {
                snake += '_';
                label += ' ';
            }
            snake += (char)std::tolower((unsigned char)c);
            label += c;
        }
        strings.push_back(snake);
        strings.push_back(label);
    }
    for (size_t i = 0; i < field.enumNames->size(); i++) {
        entry.second.push_back({(int)i, strings[i * 2].c_str(), strings[i * 2 + 1].c_str()});
    }
    return entry.second;
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
        case PropertyType::Curve: return "curve";
        case PropertyType::Gradient: return "gradient";
        case PropertyType::MinMaxCurve: return "min_max_curve";
        case PropertyType::MinMaxGradient: return "min_max_gradient";
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
        {"scene_color_scale", &sceneColorScaleOptions()},
        {"texture_filter", &textureFilterOptions()},
        {"framebuffer_filter", &framebufferFilterOptions()},
        {"texture_wrap", &textureWrapOptions()},
        {"texture_alpha_border", &textureAlphaBorderOptions()},
        {"material_alpha_mode", &alphaModeOptions()},
        {"material_blend_mode", &blendModeOptions()},
        {"material_cull_mode", &materialCullModeOptions()},
        {"material_depth_write", &depthWriteOptions()},
        {"material_depth_test", &depthTestOptions()},
        {"material_lighting", &materialLightingOptions()},
        {"material_stencil_compare", &stencilCompareOptions()},
        {"material_stencil_op", &stencilOpOptions()},
        {"material_pass_order", &passOrderOptions()},
        {"light_shading", &lightShadingOptions()},
        {"primitive_type", &primitiveTypeOptions()},
        {"body_type", &bodyTypeOptions()},
        {"collider2d_type", &collider2DTypeOptions()},
        {"collider3d_type", &collider3DTypeOptions()},
        {"joint_motion", &jointMotionOptions()},
        {"swing_type", &jointSwingTypeOptions()},
        {"rigidbody_interpolation", &rigidbodyInterpolationOptions()},
        {"light_type", &lightTypeOptions()},
        {"camera_type", &cameraTypeOptions()},
        {"lattice_interpolation", &latticeInterpolationOptions()},
        {"lattice_outside", &latticeOutsideOptions()},
        {"lattice_jiggle_anchor", &latticeJiggleAnchorOptions()},
        {"lattice_stage", &latticeStageOptions()},
        {"lattice_mask", &latticeMaskOptions()},
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
        case ComponentType::ProgressbarComponent:
            if (name == "type") return &orientationOptions();
            break;
        case ComponentType::SpriteComponent:
            if (name == "pivotPreset") return &pivotPresetOptions();
            break;
        case ComponentType::ActionComponent:
            if (name == "state") return &actionStateOptions();
            break;
        case ComponentType::LightComponent:
            if (name == "type") return &lightTypeOptions();
            if (name == "shading") return &lightShadingOptions();
            break;
        case ComponentType::UIComponent:
        case ComponentType::PointsComponent:
        case ComponentType::LineRendererComponent:
        case ComponentType::TrailRendererComponent:
            if (component == ComponentType::LineRendererComponent || component == ComponentType::TrailRendererComponent) {
                if (name == "mode" && component == ComponentType::LineRendererComponent) return &lineModeOptions();
                if (name == "widthUnit") return &lineWidthUnitOptions();
                if (name == "alignment") return &lineAlignmentOptions();
                if (name == "textureMode") return &lineTextureModeOptions();
            }
            if (name == "blendMode") return &blendModeOptions();
            if (name == "depthWrite") return &depthWriteOptions();
            if (name == "stencil.compare") return &stencilCompareOptions();
            if (name == "stencil.pass" || name == "stencil.fail" || name == "stencil.depthFail") return &stencilOpOptions();
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
        case ComponentType::UIDocumentComponent:
            if (name == "renderMode") return &uiDocumentRenderModeOptions();
            break;
        case ComponentType::FluidSimulationComponent:
            if (name == "output") return &fluidOutputOptions();
            break;
        case ComponentType::LatticeComponent:
            if (name == "interpolation") return &latticeInterpolationOptions();
            if (name == "outside") return &latticeOutsideOptions();
            if (name == "jiggleAnchor") return &latticeJiggleAnchorOptions();
            break;
        case ComponentType::LatticeModifierComponent:
            if (name == "bindings[].stage") return &latticeStageOptions();
            if (name == "bindings[].mask") return &latticeMaskOptions();
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
            if (name == "anchorSpace") return &jointAnchorSpaceOptions();
            if (name == "breakAction") return &jointBreakActionOptions();
            break;
        case ComponentType::Joint3DComponent:
            if (name == "type") return &joint3DTypeOptions();
            if (name == "anchorSpace") return &jointAnchorSpaceOptions();
            if (name == "breakAction") return &jointBreakActionOptions();
            if (name == "swingType") return &jointSwingTypeOptions();
            if (name == "xMotion" || name == "yMotion" || name == "zMotion" ||
                name == "angularXMotion" || name == "angularYMotion" || name == "angularZMotion") return &jointMotionOptions();
            break;
        case ComponentType::LayoutGroupComponent:
            if (name == "type") return &layoutGroupTypeOptions();
            break;
        case ComponentType::CanvasGroupComponent:
            if (name == "renderMode") return &canvasGroupRenderModeOptions();
            break;
        case ComponentType::AlphaTweenComponent:
            if (name == "alphaTarget") return &alphaTweenTargetOptions();
            break;
        case ComponentType::MeshComponent:
            if (name == "submeshes[].material.alphaMode") return &alphaModeOptions();
            if (name == "submeshes[].material.blendMode") return &blendModeOptions();
            if (name == "submeshes[].material.cullMode") return &materialCullModeOptions();
            if (name == "submeshes[].material.depthWrite") return &depthWriteOptions();
            if (name == "submeshes[].material.depthTest") return &depthTestOptions();
            if (name == "submeshes[].material.lighting") return &materialLightingOptions();
            // phase 2: the material's stencil, and its extra pass's state (the same names)
            for (const char* base : {"submeshes[].material.", "submeshes[].material.extraPass."}) {
                const size_t length = std::strlen(base);
                if (name.compare(0, length, base) != 0) continue;
                const std::string field = name.substr(length);
                if (field == "stencil.compare") return &stencilCompareOptions();
                if (field == "stencil.pass" || field == "stencil.fail" || field == "stencil.depthFail") return &stencilOpOptions();
                if (length > 21) {
                    if (field == "blendMode") return &blendModeOptions();
                    if (field == "cullMode") return &materialCullModeOptions();
                    if (field == "depthWrite") return &depthWriteOptions();
                    if (field == "depthTest") return &depthTestOptions();
                    if (field == "order") return &passOrderOptions();
                }
            }
            if (name == "submeshes[].primitiveType") return &primitiveTypeOptions();
            if (name == "cullingMode") return &cullingModeOptions();
            break;
        case ComponentType::Rigidbody2DComponent:
            if (name == "type") return &bodyTypeOptions();
            if (name == "colliders[].type") return &collider2DTypeOptions();
            if (name == "collisionDetection") return &collisionDetection2DOptions();
            if (name == "sleepingMode") return &sleepMode2DOptions();
            break;
        case ComponentType::RigidbodyComponent:
            if (name == "type") return &bodyTypeOptions();
            if (name == "motionQuality") return &motionQualityOptions();
            if (name == "colliders[].type") return &collider3DTypeOptions();
            if (name == "colliders[].source") return &collider3DSourceOptions();
            if (name == "interpolation") return &rigidbodyInterpolationOptions();
            break;
        case ComponentType::ParticleSystemComponent:
            if (name == "positionInitializer.shape") return &emitterShapeOptions();
            if (const particles::FieldInfo* field = particles::findField(name)) {
                if (field->type == particles::FieldType::Enum) return &particleEnumOptions(*field);
            }
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

bool McpValues::assetPathFromJson(Project* project, const std::string& projectRelative, std::string& stored, std::string& error) {
    if (projectRelative.empty()) {
        stored.clear();
        return true;
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

namespace {

// ---- Animation Player clip references (spec 5, 9.R41): stored relative to the assets folder
// (".anim", or "<model>#<clip>" whose model part is), project-relative over MCP; the stored
// form of an existing file is taken as well

const char* const CLIP_PATH_EXAMPLE = " (e.g. assets/anim/walk.anim, or assets/models/hero.gltf#Walk for a model's animation)";

std::string clipPathToJson(Project* project, const std::string& stored) {
    anim::SubClipReference reference;
    if (anim::parseSubClipReference(stored, reference)) {
        return anim::formatSubClipReference(McpValues::assetPathToJson(project, reference.model), reference.clip);
    }
    return McpValues::assetPathToJson(project, stored);
}

// A path as MCP passes it (project-relative), or the stored form of an existing file, to the
// stored form
bool storedAssetPathFromJson(Project* project, const std::string& text, std::string& stored, std::string& error) {
    const fs::path given = FileUtils::pathFromUtf8(text);
    std::error_code ec;
    if (!text.empty() && isSafeRelative(given) && !project->isInsideAssetsPath(project->getProjectPath() / given) &&
            fs::is_regular_file(project->resolveAssetPath(given), ec)) {
        stored = project->normalizeToAssetsRelative(project->resolveAssetPath(given)).generic_string();
        return true;
    }
    return McpValues::assetPathFromJson(project, text, stored, error);
}

bool isClipFilePath(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return endsWith(lower, ".anim");
}

// The .anim's GUID: the database's, else the file's own "guid" key
std::string clipFileGuid(Project* project, const fs::path& full) {
    std::string guid = project->getAssetDatabase().getGuid(full);
    if (guid.empty()) {
        std::ifstream file(full, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        guid = anim::readClipGuid(text);
    }
    return guid;
}

// clips[N].clip as MCP sets it: an existing .anim, or a model file and one of its animations
// (by name, or by index when the model can be read: stored by name, the canonical form).
// Empty clears the entry's clip. `guid` is the .anim's GUID (empty for sub-clips).
bool clipPathFromJson(Project* project, const std::string& text, std::string& stored, std::string& guid, std::string& error) {
    stored.clear();
    guid.clear();
    if (text.empty()) {
        return true;
    }
    std::string why;
    anim::SubClipReference reference;
    if (anim::parseSubClipReference(text, reference)) {
        std::string model;
        std::error_code ec;
        if (!storedAssetPathFromJson(project, reference.model, model, why)) {
            error = "clip: " + why + CLIP_PATH_EXAMPLE;
            return false;
        }
        if (!fs::is_regular_file(project->resolveAssetPath(model), ec)) {
            error = "clip: the model " + reference.model + " does not exist" + CLIP_PATH_EXAMPLE + ".";
            return false;
        }
        std::string clip = reference.clip;
        // checked against the model's animations when it can be read
        std::vector<anim::ModelClipInfo> clips;
        if (anim::listModelFileClips(model, clips, nullptr)) {
            auto named = std::find_if(clips.begin(), clips.end(), [&clip](const anim::ModelClipInfo& info) { return info.name == clip; });
            if (named == clips.end()) {
                const bool digits = !clip.empty() && std::all_of(clip.begin(), clip.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
                const size_t index = digits && clip.size() < 9 ? static_cast<size_t>(std::stoul(clip)) : clips.size();
                if (index >= clips.size()) {
                    std::string names;
                    for (const anim::ModelClipInfo& info : clips) names += (names.empty() ? "" : ", ") + info.name;
                    error = "clip: " + reference.model + " has no animation \"" + clip + "\" (" +
                            (names.empty() ? std::string("it has none") : "it has: " + names) + ").";
                    return false;
                }
                clip = clips[index].name;
            }
        }
        stored = anim::formatSubClipReference(model, clip);
        return true;
    }
    if (!isClipFilePath(text)) {
        error = "clip must be a .anim file or <model>#<animation>" + std::string(CLIP_PATH_EXAMPLE) + ".";
        return false;
    }
    if (!storedAssetPathFromJson(project, text, stored, why)) {
        error = "clip: " + why + CLIP_PATH_EXAMPLE;
        return false;
    }
    const fs::path full = project->resolveAssetPath(stored);
    std::error_code ec;
    if (!fs::is_regular_file(full, ec)) {
        error = "clip: " + text + " does not exist: create it first with edit_animation {clip, create: {}}" +
                CLIP_PATH_EXAMPLE + ".";
        stored.clear();
        return false;
    }
    guid = clipFileGuid(project, full);
    return true;
}

// ---- Vertex Animator animation (vertex animation spec 6): a .vat, stored relative to the
// assets folder, project-relative over MCP

const char* const VAT_PATH_EXAMPLE = ".vat file, e.g. assets/vat/hero.vat";

// The .vat's GUID: the database's, else the file's own "guid" key
std::string vatFileGuid(Project* project, const fs::path& full) {
    std::string guid = project->getAssetDatabase().getGuid(full);
    if (guid.empty()) {
        std::ifstream file(full, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const Json json = Json::parse(text, nullptr, false);
        if (json.is_object() && json.contains("guid") && json["guid"].is_string()) {
            guid = json["guid"].get<std::string>();
        }
    }
    return guid;
}

// animation as MCP sets it: an existing .vat (empty clears it); `guid` the .vat's GUID
bool vatPathFromJson(Project* project, const std::string& text, std::string& stored, std::string& guid, std::string& error) {
    stored.clear();
    guid.clear();
    if (text.empty()) {
        return true;
    }
    std::string why;
    if (!storedAssetPathFromJson(project, text, stored, why)) {
        error = "animation must be a " + std::string(VAT_PATH_EXAMPLE) + ": " + why;
        stored.clear();
        return false;
    }
    if (!Util::isVertexAnimationFile(stored)) {
        error = "animation must be a " + std::string(VAT_PATH_EXAMPLE) + " (bake one with bake_vertex_animation; the .vat.glb next to it is its Model).";
        stored.clear();
        return false;
    }
    const fs::path full = project->resolveAssetPath(stored);
    std::error_code ec;
    if (!fs::is_regular_file(full, ec)) {
        error = "animation: " + text + " does not exist: bake it first with bake_vertex_animation (a " + VAT_PATH_EXAMPLE +
                "), or list the baked ones with list_assets {type: \"vertex_animation\"}.";
        stored.clear();
        return false;
    }
    guid = vatFileGuid(project, full);
    return true;
}

// "default", "once", "loop", "ping_pong", "clamp_forever"
std::string clipWrapNames() {
    std::string names;
    for (ClipWrapMode wrap : {ClipWrapMode::Default, ClipWrapMode::Once, ClipWrapMode::Loop, ClipWrapMode::PingPong, ClipWrapMode::ClampForever}) {
        names += (names.empty() ? "" : ", ") + std::string(anim::wrapName(wrap));
    }
    return names;
}

Json playerClipsToJson(Project* project, const std::vector<AnimationPlayerClip>& clips) {
    Json list = Json::array();
    for (const AnimationPlayerClip& entry : clips) {
        Json item{{"clip", clipPathToJson(project, entry.clip)}, {"name", entry.name}, {"wrap", anim::wrapName(entry.wrap)}};
        if (!entry.guid.empty()) item["guid"] = entry.guid;
        list.push_back(item);
    }
    return list;
}

// The clip list an edit through the component's encoding left (clips set, an entry inserted,
// removed or set, a field of one). An entry naming a clip the player holds now, in its stored or
// its project-relative (read) form, is that entry: its stored text and GUID stay and it is not
// checked again (a missing clip stays missing, R8). Any other entry is checked and stored as
// clips[N].clip is. GUIDs never come from the caller: the held entry's, else the file's. Wraps
// are checked.
// A Vertex Animator's attachments as MCP writes them: [{socket, entity}] with the entity as an
// id or a path of names ("Hero/Sword"), stored as ids of the same scene
bool normalizeVertexAnimatorAttachments(SceneProject* scene, YAML::Node value, std::string& error) {
    const YAML::Node& constValue = value;
    const YAML::Node attachments = constValue["attachments"];
    if (!attachments || attachments.IsNull()) {
        value["attachments"] = YAML::Node(YAML::NodeType::Sequence);
        return true;
    }
    const char* const shape = "attachments must be a list of {socket, entity}, e.g. [{\"socket\": \"RightHand\", \"entity\": \"Hero/Sword\"}].";
    if (!attachments.IsSequence()) {
        error = shape;
        return false;
    }
    YAML::Node rebuilt(YAML::NodeType::Sequence);
    for (size_t i = 0; i < attachments.size(); i++) {
        const YAML::Node entry = attachments[i];
        if (!entry.IsMap()) {
            error = std::string("attachments[") + std::to_string(i) + "]: " + shape;
            return false;
        }
        YAML::Node out;
        const YAML::Node socket = entry["socket"];
        out["socket"] = socket && socket.IsScalar() ? socket.Scalar() : std::string();
        Entity target = NULL_ENTITY;
        const YAML::Node entity = entry["entity"];
        if (entity && entity.IsScalar() && !entity.Scalar().empty()) {
            const std::string text = entity.Scalar();
            const bool digits = std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
            std::string why;
            target = McpRefs::resolveEntity(scene, digits && text.size() < 10 ? Json(std::stoull(text)) : Json(text), why);
            if (target == NULL_ENTITY) {
                error = std::string("attachments[") + std::to_string(i) + "].entity: " + why;
                return false;
            }
        }
        out["entity"] = target;
        rebuilt.push_back(out);
    }
    value["attachments"] = rebuilt;
    return true;
}

bool normalizePlayerClips(Project* project, const AnimationPlayerComponent* current, YAML::Node value, std::string& error) {
    const YAML::Node& constValue = value;
    const YAML::Node clips = constValue["clips"];
    if (!clips || clips.IsNull()) {
        value["clips"] = YAML::Node(YAML::NodeType::Sequence);
        return true;
    }
    if (!clips.IsSequence()) {
        error = "clips must be a list of {clip, name, wrap}, e.g. [{\"clip\": \"assets/anim/walk.anim\", \"wrap\": \"loop\"}].";
        return false;
    }
    YAML::Node rebuilt(YAML::NodeType::Sequence);
    for (size_t i = 0; i < clips.size(); i++) {
        const YAML::Node item = clips[i];
        const std::string at = "clips[" + std::to_string(i) + "]";
        YAML::Node entry(YAML::NodeType::Map);
        if (item.IsScalar()) {
            entry["clip"] = item.Scalar();
        } else if (item.IsMap()) {
            for (auto it = item.begin(); it != item.end(); ++it) {
                const std::string key = it->first.as<std::string>();
                // guid: as get_entity shows it, so a list read back is taken; its value is ignored
                if (key != "clip" && key != "guid" && key != "name" && key != "wrap") {
                    error = at + ": unknown key \"" + key + "\" (an entry is {clip, name, wrap}; its guid follows the clip).";
                    return false;
                }
                if (!it->second.IsScalar() && !it->second.IsNull()) {
                    error = at + "." + key + " must be a string.";
                    return false;
                }
                if (key != "guid") entry[key] = it->second.IsScalar() ? it->second.Scalar() : std::string();
            }
        } else {
            error = at + " must be {clip, name, wrap} or a clip path" + std::string(CLIP_PATH_EXAMPLE) + ".";
            return false;
        }
        const YAML::Node& constEntry = entry;
        const std::string clip = constEntry["clip"] ? constEntry["clip"].Scalar() : std::string();
        const AnimationPlayerClip* same = nullptr;
        if (current) {
            for (const AnimationPlayerClip& held : current->clips) {
                if (held.clip == clip || (!held.clip.empty() && clipPathToJson(project, held.clip) == clip)) {
                    same = &held;
                    break;
                }
            }
        }
        std::string guid;
        if (same) {
            entry["clip"] = same->clip;
            guid = same->guid;
        } else {
            std::string stored, why;
            if (!clipPathFromJson(project, clip, stored, guid, why)) {
                error = at + "." + why;
                return false;
            }
            entry["clip"] = stored;
        }
        if (!guid.empty()) entry["guid"] = guid;
        if (constEntry["wrap"]) {
            ClipWrapMode wrap = ClipWrapMode::Default;
            const std::string name = constEntry["wrap"].Scalar();
            if (!name.empty() && !anim::wrapFromName(name, wrap)) {
                error = at + ".wrap \"" + name + "\" is not a wrap mode (" + clipWrapNames() + ").";
                return false;
            }
        }
        rebuilt.push_back(entry);
    }
    value["clips"] = rebuilt;
    return true;
}

} // namespace

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

namespace {

    Json curveNumber(float value) {
        if (std::isinf(value)) return value > 0.0f ? Json("inf") : Json("-inf");
        return McpValues::number(value);
    }

    bool curveNumberFromJson(const Json& value, float& out) {
        if (value.is_number()) {
            out = value.get<float>();
            return std::isfinite(out);
        }
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            if (text == "inf" || text == "+inf" || text == ".inf" || text == "Infinity") { out = std::numeric_limits<float>::infinity(); return true; }
            if (text == "-inf" || text == "-.inf" || text == "-Infinity") { out = -std::numeric_limits<float>::infinity(); return true; }
        }
        return false;
    }

    const char* curveWrapName(CurveWrap wrap) {
        switch (wrap) {
            case CurveWrap::Loop: return "loop";
            case CurveWrap::PingPong: return "ping_pong";
            default: return "clamp";
        }
    }

    bool curveWrapFromJson(const Json& value, CurveWrap& out) {
        if (!value.is_string()) return false;
        const std::string name = value.get<std::string>();
        if (name == "clamp") { out = CurveWrap::Clamp; return true; }
        if (name == "loop") { out = CurveWrap::Loop; return true; }
        if (name == "ping_pong") { out = CurveWrap::PingPong; return true; }
        return false;
    }

    // a colour of a gradient key: "#RRGGBB" (sRGB) or [r, g, b(, a)] (linear), as linear RGBA
    bool gradientColorFromJson(const Json& value, Vector4& linear, bool& hasAlpha) {
        hasAlpha = false;
        float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        int count = 0;
        if (parseHexColor(value, v, count)) {
            const Vector3 color = Color::sRGBToLinear(Vector3(v[0], v[1], v[2]));
            linear = Vector4(color.x, color.y, color.z, count == 4 ? v[3] : 1.0f);
            hasAlpha = count == 4;
            return true;
        }
        if (value.is_array() && (value.size() == 3 || value.size() == 4)) {
            for (size_t i = 0; i < value.size(); i++) {
                if (!value[i].is_number()) return false;
                v[i] = value[i].get<float>();
                if (!std::isfinite(v[i])) return false;
            }
            linear = Vector4(v[0], v[1], v[2], v[3]);
            hasAlpha = value.size() == 4;
            return true;
        }
        return false;
    }

}

Json McpValues::curveToJson(const AnimationCurve& curve) {
    Json keys = Json::array();
    for (const CurveKey& key : curve.keys) {
        Json item = Json::array({number(key.time), number(key.value)});
        if (key.inTangent != 0.0f || key.outTangent != 0.0f) {
            item.push_back(curveNumber(key.inTangent));
            item.push_back(curveNumber(key.outTangent));
        }
        keys.push_back(item);
    }
    Json out = Json::object();
    out["keys"] = keys;
    out["pre"] = curveWrapName(curve.preWrap);
    out["post"] = curveWrapName(curve.postWrap);
    return out;
}

bool McpValues::curveFromJson(const Json& value, AnimationCurve& out, std::string& error) {
    const char* shape = "a number (constant), [[time, value], [time, value, inTangent, outTangent], ...] or "
                        "{\"keys\": [...], \"pre\": \"clamp\"|\"loop\"|\"ping_pong\", \"post\": ...}";
    out = AnimationCurve();
    if (value.is_number()) {
        const float constant = value.get<float>();
        if (!std::isfinite(constant)) {
            error = "a constant must be a finite number.";
            return false;
        }
        out = AnimationCurve::constant(constant);
        return true;
    }
    const Json* keys = nullptr;
    if (value.is_array()) {
        keys = &value;
    } else if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key() != "keys" && it.key() != "pre" && it.key() != "post") {
                error = "unknown key \"" + it.key() + "\"; expected " + shape + ".";
                return false;
            }
        }
        if (!value.contains("keys") || !value["keys"].is_array()) {
            error = "missing \"keys\" array; expected " + std::string(shape) + ".";
            return false;
        }
        keys = &value["keys"];
        if (value.contains("pre") && !curveWrapFromJson(value["pre"], out.preWrap)) {
            error = "\"pre\" must be \"clamp\", \"loop\" or \"ping_pong\".";
            return false;
        }
        if (value.contains("post") && !curveWrapFromJson(value["post"], out.postWrap)) {
            error = "\"post\" must be \"clamp\", \"loop\" or \"ping_pong\".";
            return false;
        }
    } else {
        error = std::string("expected ") + shape + ".";
        return false;
    }
    for (size_t i = 0; i < keys->size(); i++) {
        const Json& item = (*keys)[i];
        CurveKey key;
        bool ok = item.is_array() && (item.size() == 2 || item.size() == 4) && item[0].is_number() && item[1].is_number();
        if (ok) {
            key.time = item[0].get<float>();
            key.value = item[1].get<float>();
            ok = std::isfinite(key.time) && std::isfinite(key.value);
            if (ok && item.size() == 4) {
                // a step is an infinite tangent: "inf" / "-inf"
                ok = curveNumberFromJson(item[2], key.inTangent) && curveNumberFromJson(item[3], key.outTangent);
            }
        }
        if (!ok) {
            error = "keys[" + std::to_string(i) + "] must be [time, value] or [time, value, inTangent, outTangent] "
                    "(finite numbers; a step tangent is \"inf\" / \"-inf\").";
            return false;
        }
        for (const CurveKey& existing : out.keys) {
            if (existing.time == key.time) {
                error = "keys[" + std::to_string(i) + "]: two keys at time " + std::to_string(key.time) + ".";
                return false;
            }
        }
        out.keys.push_back(key);
    }
    out.normalize();
    return true;
}

Json McpValues::gradientToJson(const Gradient& gradient) {
    Json out = Json::object();
    out["mode"] = gradient.mode == GradientMode::Fixed ? "fixed" : "blend";
    Json colors = Json::array();
    for (const GradientColorKey& key : gradient.colorKeys) {
        // the YAML form's rule: "#RRGGBB" when that is exactly the value, else linear numbers
        const YAML::Node encoded = Stream::encodeGradient(Gradient({key}, {}))["colors"][0][1];
        Json color = encoded.IsScalar() ? Json(encoded.as<std::string>())
                                        : Json::array({number(key.color.x), number(key.color.y), number(key.color.z)});
        colors.push_back(Json::array({number(key.time), color}));
    }
    out["colors"] = colors;
    Json alphas = Json::array();
    for (const GradientAlphaKey& key : gradient.alphaKeys) {
        alphas.push_back(Json::array({number(key.time), number(key.alpha)}));
    }
    out["alphas"] = alphas;
    return out;
}

bool McpValues::gradientFromJson(const Json& value, Gradient& out, std::string& error) {
    const char* shape = "{\"mode\": \"blend\"|\"fixed\", \"colors\": [[time, \"#RRGGBB\" or [r, g, b] linear], ...], "
                        "\"alphas\": [[time, alpha], ...]} (up to 8 keys each, times 0..1), or one colour for a constant gradient";
    Vector4 single;
    bool singleAlpha = false;
    if (gradientColorFromJson(value, single, singleAlpha)) {
        out = Gradient::constant(single);
        return true;
    }
    if (!value.is_object()) {
        error = std::string("expected ") + shape + ".";
        return false;
    }
    out = Gradient({}, {});
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() != "mode" && it.key() != "colors" && it.key() != "alphas") {
            error = "unknown key \"" + it.key() + "\"; expected " + shape + ".";
            return false;
        }
    }
    if (value.contains("mode")) {
        const Json& mode = value["mode"];
        if (!mode.is_string() || (mode.get<std::string>() != "blend" && mode.get<std::string>() != "fixed")) {
            error = "\"mode\" must be \"blend\" or \"fixed\".";
            return false;
        }
        out.mode = mode.get<std::string>() == "fixed" ? GradientMode::Fixed : GradientMode::Blend;
    }
    auto checkTime = [&](const Json& time, const std::string& where, float& outTime) -> bool {
        if (!time.is_number() || !std::isfinite(time.get<float>()) || time.get<float>() < 0.0f || time.get<float>() > 1.0f) {
            error = where + ": the time must be a number in 0..1.";
            return false;
        }
        outTime = time.get<float>();
        return true;
    };
    if (value.contains("colors")) {
        const Json& colors = value["colors"];
        if (!colors.is_array() || colors.size() > Gradient::MAX_KEYS) {
            error = "\"colors\" must be an array of up to 8 [time, colour] keys.";
            return false;
        }
        for (size_t i = 0; i < colors.size(); i++) {
            const std::string where = "colors[" + std::to_string(i) + "]";
            const Json& item = colors[i];
            Vector4 color;
            bool hasAlpha = false;
            float time = 0.0f;
            if (!item.is_array() || item.size() != 2) {
                error = where + " must be [time, colour].";
                return false;
            }
            if (!checkTime(item[0], where, time)) return false;
            if (!gradientColorFromJson(item[1], color, hasAlpha)) {
                error = where + ": the colour must be \"#RRGGBB\" (sRGB) or [r, g, b] (linear).";
                return false;
            }
            out.colorKeys.push_back(GradientColorKey(time, Vector3(color.x, color.y, color.z)));
        }
    }
    if (value.contains("alphas")) {
        const Json& alphas = value["alphas"];
        if (!alphas.is_array() || alphas.size() > Gradient::MAX_KEYS) {
            error = "\"alphas\" must be an array of up to 8 [time, alpha] keys.";
            return false;
        }
        for (size_t i = 0; i < alphas.size(); i++) {
            const std::string where = "alphas[" + std::to_string(i) + "]";
            const Json& item = alphas[i];
            float time = 0.0f;
            if (!item.is_array() || item.size() != 2 || !item[1].is_number() || !std::isfinite(item[1].get<float>())) {
                error = where + " must be [time, alpha] (numbers).";
                return false;
            }
            if (!checkTime(item[0], where, time)) return false;
            out.alphaKeys.push_back(GradientAlphaKey(time, item[1].get<float>()));
        }
    }
    out.normalize();
    return true;
}

Json McpValues::minMaxCurveToJson(const MinMaxCurve& curve) {
    switch (curve.mode) {
        case ParticleSystemCurveMode::Constant:
            return number(curve.constant);
        case ParticleSystemCurveMode::TwoConstants:
            return Json{{"mode", "two_constants"}, {"min", number(curve.constantMin)}, {"max", number(curve.constant)}};
        case ParticleSystemCurveMode::Curve:
            return Json{{"mode", "curve"}, {"multiplier", number(curve.constant)}, {"curve", curveToJson(curve.curve)}};
        case ParticleSystemCurveMode::TwoCurves:
            return Json{{"mode", "two_curves"}, {"multiplier", number(curve.constant)}, {"min", curveToJson(curve.curveMin)},
                        {"max", curveToJson(curve.curve)}};
    }
    return number(curve.constant);
}

bool McpValues::minMaxCurveFromJson(const Json& value, MinMaxCurve& out, std::string& error) {
    const char* shape = "a number (constant), {\"mode\": \"two_constants\", \"min\", \"max\"}, {\"mode\": \"curve\", "
                        "\"multiplier\", \"curve\"} or {\"mode\": \"two_curves\", \"multiplier\", \"min\", \"max\"} (curves: "
                        "[[time, value], ...] or {\"keys\": ...})";
    if (value.is_number()) {
        const float constant = value.get<float>();
        if (!std::isfinite(constant)) {
            error = "a constant must be a finite number.";
            return false;
        }
        out = MinMaxCurve(constant);
        return true;
    }
    if (!value.is_object() || !value.contains("mode") || !value["mode"].is_string()) {
        error = std::string("expected ") + shape + ".";
        return false;
    }
    const std::string mode = value["mode"].get<std::string>();
    auto finite = [&](const char* key, float fallback, float& result) -> bool {
        if (!value.contains(key)) {
            result = fallback;
            return true;
        }
        if (!value[key].is_number() || !std::isfinite(value[key].get<float>())) {
            error = std::string("\"") + key + "\" must be a finite number.";
            return false;
        }
        result = value[key].get<float>();
        return true;
    };
    auto allowOnly = [&](std::initializer_list<const char*> keys) -> bool {
        for (auto it = value.begin(); it != value.end(); ++it) {
            bool known = false;
            for (const char* key : keys) known = known || it.key() == key;
            if (!known) {
                error = "unknown key \"" + it.key() + "\" for mode " + mode + "; expected " + shape + ".";
                return false;
            }
        }
        return true;
    };
    if (mode == "constant") {
        float v = 0.0f;
        if (!allowOnly({"mode", "value", "constant"})) return false;
        if (!finite(value.contains("value") ? "value" : "constant", out.constant, v)) return false;
        out = MinMaxCurve(v);
        return true;
    }
    if (mode == "two_constants") {
        float lo = 0.0f, hi = 0.0f;
        if (!allowOnly({"mode", "min", "max"})) return false;
        if (!value.contains("min") || !value.contains("max")) {
            error = "two_constants needs \"min\" and \"max\".";
            return false;
        }
        if (!finite("min", 0.0f, lo) || !finite("max", 0.0f, hi)) return false;
        out = MinMaxCurve::between(lo, hi);
        return true;
    }
    if (mode == "curve" || mode == "two_curves") {
        float multiplier = 1.0f;
        if (!allowOnly({"mode", "multiplier", "curve", "min", "max"})) return false;
        if (!finite("multiplier", 1.0f, multiplier)) return false;
        std::string why;
        if (mode == "curve") {
            AnimationCurve curve;
            if (!value.contains("curve") || !curveFromJson(value["curve"], curve, why)) {
                error = "\"curve\": " + (why.empty() ? std::string("missing") : why);
                return false;
            }
            out = MinMaxCurve::fromCurve(curve, multiplier);
            return true;
        }
        AnimationCurve lo, hi;
        if (!value.contains("min") || !curveFromJson(value["min"], lo, why)) {
            error = "\"min\": " + (why.empty() ? std::string("missing") : why);
            return false;
        }
        if (!value.contains("max") || !curveFromJson(value["max"], hi, why)) {
            error = "\"max\": " + (why.empty() ? std::string("missing") : why);
            return false;
        }
        out = MinMaxCurve::between(lo, hi, multiplier);
        return true;
    }
    error = "\"mode\" must be constant, two_constants, curve or two_curves.";
    return false;
}

Json McpValues::minMaxGradientToJson(const MinMaxGradient& gradient) {
    auto color = [](const Vector4& linear) {
        const YAML::Node encoded = Stream::encodeColorValue(linear);
        if (encoded.IsScalar()) return Json(encoded.as<std::string>());
        return Json::array({number(linear.x), number(linear.y), number(linear.z), number(linear.w)});
    };
    switch (gradient.mode) {
        case ParticleSystemGradientMode::Color:
            return color(gradient.color);
        case ParticleSystemGradientMode::TwoColors:
            return Json{{"mode", "two_colors"}, {"min", color(gradient.colorMin)}, {"max", color(gradient.color)}};
        case ParticleSystemGradientMode::Gradient:
            return Json{{"mode", "gradient"}, {"gradient", gradientToJson(gradient.gradient)}};
        case ParticleSystemGradientMode::RandomColor:
            return Json{{"mode", "random_color"}, {"gradient", gradientToJson(gradient.gradient)}};
        case ParticleSystemGradientMode::TwoGradients:
            return Json{{"mode", "two_gradients"}, {"min", gradientToJson(gradient.gradientMin)}, {"max", gradientToJson(gradient.gradient)}};
    }
    return color(gradient.color);
}

bool McpValues::minMaxGradientFromJson(const Json& value, MinMaxGradient& out, std::string& error) {
    const char* shape = "a colour (\"#RRGGBB[AA]\" sRGB or [r, g, b(, a)] linear), {\"mode\": \"gradient\", \"gradient\"}, "
                        "{\"mode\": \"two_colors\", \"min\", \"max\"}, {\"mode\": \"two_gradients\", \"min\", \"max\"} or "
                        "{\"mode\": \"random_color\", \"gradient\"}";
    Vector4 single;
    bool hasAlpha = false;
    if (gradientColorFromJson(value, single, hasAlpha)) {
        out = MinMaxGradient::fromColor(single);
        return true;
    }
    if (!value.is_object() || !value.contains("mode") || !value["mode"].is_string()) {
        error = std::string("expected ") + shape + ".";
        return false;
    }
    const std::string mode = value["mode"].get<std::string>();
    std::string why;
    auto colorAt = [&](const char* key, Vector4& result) -> bool {
        bool alpha = false;
        if (!value.contains(key) || !gradientColorFromJson(value[key], result, alpha)) {
            error = std::string("\"") + key + "\" must be a colour (\"#RRGGBB[AA]\" or [r, g, b(, a)] linear).";
            return false;
        }
        return true;
    };
    auto gradientAt = [&](const char* key, Gradient& result) -> bool {
        if (!value.contains(key) || !gradientFromJson(value[key], result, why)) {
            error = std::string("\"") + key + "\": " + (why.empty() ? std::string("missing") : why);
            return false;
        }
        return true;
    };
    if (mode == "color") {
        Vector4 c;
        if (!colorAt("color", c)) return false;
        out = MinMaxGradient::fromColor(c);
        return true;
    }
    if (mode == "two_colors") {
        Vector4 lo, hi;
        if (!colorAt("min", lo) || !colorAt("max", hi)) return false;
        out = MinMaxGradient::between(lo, hi);
        return true;
    }
    if (mode == "gradient" || mode == "random_color") {
        Gradient g;
        if (!gradientAt("gradient", g)) return false;
        out = MinMaxGradient::fromGradient(g);
        if (mode == "random_color") out.mode = ParticleSystemGradientMode::RandomColor;
        return true;
    }
    if (mode == "two_gradients") {
        Gradient lo, hi;
        if (!gradientAt("min", lo) || !gradientAt("max", hi)) return false;
        out = MinMaxGradient::between(lo, hi);
        return true;
    }
    error = "\"mode\" must be color, gradient, two_colors, two_gradients or random_color.";
    return false;
}

Json McpValues::particleBurstsToJson(const std::vector<ParticleEmissionBurst>& bursts) {
    Json list = Json::array();
    for (const ParticleEmissionBurst& burst : bursts) {
        list.push_back(Json{{"time", number(burst.time)}, {"count", minMaxCurveToJson(burst.count)}, {"cycles", burst.cycles},
                            {"interval", number(burst.interval)}, {"probability", number(burst.probability)}});
    }
    return list;
}

bool McpValues::particleBurstsFromJson(const Json& value, std::vector<ParticleEmissionBurst>& out, std::string& error) {
    out.clear();
    if (!value.is_array()) {
        error = "expected [{\"time\", \"count\", \"cycles\" (0 = forever), \"interval\", \"probability\"}, ...].";
        return false;
    }
    for (size_t i = 0; i < value.size(); i++) {
        const Json& item = value[i];
        const std::string where = "bursts[" + std::to_string(i) + "]";
        if (!item.is_object()) {
            error = where + " must be {\"time\", \"count\", \"cycles\", \"interval\", \"probability\"}.";
            return false;
        }
        ParticleEmissionBurst burst;
        for (auto it = item.begin(); it != item.end(); ++it) {
            const std::string& key = it.key();
            const Json& v = it.value();
            if (key == "count") {
                std::string why;
                if (!minMaxCurveFromJson(v, burst.count, why)) {
                    error = where + ".count: " + why;
                    return false;
                }
            } else if (key == "time" || key == "interval" || key == "probability") {
                if (!v.is_number() || !std::isfinite(v.get<float>())) {
                    error = where + "." + key + " must be a number.";
                    return false;
                }
                const float number = v.get<float>();
                if (key == "time") burst.time = std::max(0.0f, number);
                if (key == "interval") burst.interval = std::max(0.0001f, number);
                if (key == "probability") burst.probability = std::min(1.0f, std::max(0.0f, number));
            } else if (key == "cycles") {
                if (!isIntegral(v) || integralValue(v) < 0) {
                    error = where + ".cycles must be an integer >= 0 (0 = forever).";
                    return false;
                }
                burst.cycles = (int)integralValue(v);
            } else {
                error = where + ": unknown key \"" + key + "\" (time, count, cycles, interval, probability).";
                return false;
            }
        }
        out.push_back(burst);
    }
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
        {"metallicFactor", number(material.metallicFactor)},
        {"roughnessFactor", number(material.roughnessFactor)},
        {"alphaCutoff", number(material.alphaCutoff)},
        {"foliageTransmission", number(material.foliageTransmission)},
        {"alphaMode", enumToJson((int)material.alphaMode, alphaModeOptions())},
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
                     key == "emissiveFactor" || key == "file" || isMaterialStochasticKey(key);
        for (const MaterialTextureField& field : MATERIAL_TEXTURES) {
            if (key == field.name || key == field.texCoordName) known = true;
        }
        known = known || isMaterialShaderLibraryKey(key);
        if (!known) {
            error = "unknown material key \"" + key + "\"; valid: name, baseColorFactor, metallicFactor, roughnessFactor, "
                    "alphaCutoff, foliageTransmission, alphaMode, emissiveFactor, baseColorTexture, emissiveTexture, "
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
    return materialShaderLibraryFromJson(project, scene, value, out, error);
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
        // an Animation Player entry's wrap has a default ("default"), like an enum
        if (readDefault) return ref && isPlayerWrapProperty(ctx.component, name) ? Json(anim::wrapName(*static_cast<ClipWrapMode*>(ref))) : Json(nullptr);
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
        // an Animation Player's clip list, its paths project-relative, and an entry's wrap
        if (ref && ctx.component == ComponentType::AnimationPlayerComponent && name == "clips") {
            return playerClipsToJson(ctx.project, *static_cast<std::vector<AnimationPlayerClip>*>(ref));
        }
        if (ref && isPlayerWrapProperty(ctx.component, name)) {
            return anim::wrapName(*static_cast<ClipWrapMode*>(ref));
        }
        // a version 2 Particle System's bursts and sprite frames
        if (ref && ctx.component == ComponentType::ParticleSystemComponent && name == "emission.bursts") {
            return particleBurstsToJson(*static_cast<std::vector<ParticleEmissionBurst>*>(ref));
        }
        if (ref && ctx.component == ComponentType::ParticleSystemComponent && name == "textureSheetAnimation.sprites") {
            return Json(*static_cast<std::vector<int>*>(ref));
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
            if (isPlayerClipProperty(ctx.component, name)) return clipPathToJson(ctx.project, value);
            return isAssetPathProperty(ctx.component, name) ? assetPathToJson(ctx.project, value) : value;
        }
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
        case PropertyType::Curve:
            return curveToJson(*static_cast<AnimationCurve*>(ref));
        case PropertyType::Gradient:
            return gradientToJson(*static_cast<Gradient*>(ref));
        case PropertyType::MinMaxCurve:
            return minMaxCurveToJson(*static_cast<MinMaxCurve*>(ref));
        case PropertyType::MinMaxGradient:
            return minMaxGradientToJson(*static_cast<MinMaxGradient*>(ref));
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

Json McpValues::toJson(Project* project, ComponentType component, const std::string& propertyName, const PropertyData& property) {
    ReadContext ctx;
    ctx.project = project;
    ctx.hasComponent = false;
    ctx.component = component;
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
                    error = "filename must be an existing .gltf, .glb, .obj or .fbx file in the assets directory.";
                    return nullptr;
                }
                return new ModelLoadCmd(project, sceneId, entity, stored);
            }
            if (component == ComponentType::PoseAnimatorComponent && propertyName == "animation") {
                // the project-relative path the pose tools use ("assets/anim/walk.poseanim"); the
                // stored form (relative to the assets folder) of an existing file is taken as well
                std::string stored;
                const fs::path given = FileUtils::pathFromUtf8(text);
                std::error_code ec;
                if (!text.empty() && isSafeRelative(given) && !project->isInsideAssetsPath(project->getProjectPath() / given) &&
                        fs::is_regular_file(project->resolveAssetPath(given), ec)) {
                    stored = project->normalizeToAssetsRelative(project->resolveAssetPath(given)).generic_string();
                } else if (!assetPathFromJson(project, text, stored, error)) {
                    error = "animation: " + error + " Pass the .poseanim as the pose tools name it (e.g. assets/anim/walk.poseanim).";
                    return nullptr;
                }
                if (!stored.empty() && !Util::isPoseAnimationFile(stored)) {
                    error = "animation must be a .poseanim file (e.g. assets/anim/walk.poseanim).";
                    return nullptr;
                }
                return new PropertyCmd<std::string>(project, sceneId, entity, component, propertyName, stored, onChanged);
            }
            if (isPlayerClipProperty(component, propertyName)) {
                // the path and the GUID it names change together: the GUID wins when the scene loads
                std::string stored, guid;
                if (!clipPathFromJson(project, text, stored, guid, error)) return nullptr;
                auto* multi = new MultiPropertyCmd();
                multi->addPropertyCmd<std::string>(project, sceneId, entity, component, propertyName, stored, onChanged);
                multi->addPropertyCmd<std::string>(project, sceneId, entity, component,
                                                   propertyName.substr(0, propertyName.size() - 5) + ".guid", guid);
                return multi;
            }
            if (isVertexAnimationProperty(component, propertyName)) {
                // the path and the GUID it names change together: the GUID wins when the scene loads
                std::string stored, guid;
                if (!vatPathFromJson(project, text, stored, guid, error)) return nullptr;
                auto* multi = new MultiPropertyCmd();
                multi->addPropertyCmd<std::string>(project, sceneId, entity, component, propertyName, stored, onChanged);
                multi->addPropertyCmd<std::string>(project, sceneId, entity, component, "guid", guid);
                return multi;
            }
            if (component == ComponentType::VertexAnimatorComponent && propertyName == "guid") {
                error = "guid is read-only: set animation, its GUID follows the file.";
                return nullptr;
            }
            if (isPlayerGuidProperty(component, propertyName)) {
                // a GUID wins over the path when the scene loads: it only ever comes from the file
                error = propertyName + " is read-only: set " + propertyName.substr(0, propertyName.size() - 5) +
                        ".clip, its GUID follows the file.";
                return nullptr;
            }
            if (isAssetPathProperty(component, propertyName) && !assetPathFromJson(project, text, text, error)) return nullptr;
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
                        multi->addPropertyCmd<std::string>(project, sceneId, entity, component, propertyName, text, onChanged);
                        multi->addPropertyCmd<int>(project, sceneId, entity, component, prefix + "lighting",
                                                   (int)MaterialLighting::AUTO, onChanged);
                        return multi;
                    }
                }
            }
            return new PropertyCmd<std::string>(project, sceneId, entity, component, propertyName, text, onChanged);
        }
        case PropertyType::Float: {
            if (!value.is_number()) return expect("a number");
            // as the scene file reads it back (Stream clamps a negative fade to none)
            if (component == ComponentType::AnimationPlayerComponent && propertyName == "defaultFadeTime" &&
                    !(value.get<double>() >= 0.0)) {
                error = "defaultFadeTime is in seconds and must be 0 or more (0.25 by default).";
                return nullptr;
            }
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
                        modes += ", " + enumToJson((int)mode, materialLightingOptions()).get<std::string>();
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
        case PropertyType::Curve: {
            AnimationCurve curve;
            std::string why;
            if (!curveFromJson(value, curve, why)) {
                error = "Property " + propertyName + " is a curve: " + why;
                return nullptr;
            }
            return new PropertyCmd<AnimationCurve>(project, sceneId, entity, component, propertyName, curve, onChanged);
        }
        case PropertyType::Gradient: {
            Gradient gradient;
            std::string why;
            if (!gradientFromJson(value, gradient, why)) {
                error = "Property " + propertyName + " is a gradient: " + why;
                return nullptr;
            }
            return new PropertyCmd<Gradient>(project, sceneId, entity, component, propertyName, gradient, onChanged);
        }
        case PropertyType::MinMaxCurve: {
            MinMaxCurve curve;
            std::string why;
            if (!minMaxCurveFromJson(value, curve, why)) {
                error = "Property " + propertyName + " is a MinMaxCurve: " + why;
                return nullptr;
            }
            return new PropertyCmd<MinMaxCurve>(project, sceneId, entity, component, propertyName, curve, onChanged);
        }
        case PropertyType::MinMaxGradient: {
            MinMaxGradient gradient;
            std::string why;
            if (!minMaxGradientFromJson(value, gradient, why)) {
                error = "Property " + propertyName + " is a MinMaxGradient: " + why;
                return nullptr;
            }
            return new PropertyCmd<MinMaxGradient>(project, sceneId, entity, component, propertyName, gradient, onChanged);
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
            if (isPlayerWrapProperty(component, propertyName)) {
                // a one-byte enum by its file name
                ClipWrapMode wrap = ClipWrapMode::Default;
                if (!value.is_string() || !anim::wrapFromName(value.get<std::string>(), wrap)) {
                    error = "Property " + propertyName + " is a wrap mode: " + clipWrapNames() + ".";
                    return nullptr;
                }
                return new PropertyCmd<ClipWrapMode>(project, sceneId, entity, component, propertyName, wrap, onChanged);
            }
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
            if (component == ComponentType::ParticleSystemComponent && propertyName == "emission.bursts") {
                std::vector<ParticleEmissionBurst> bursts;
                std::string why;
                if (!particleBurstsFromJson(value, bursts, why)) {
                    error = "Property " + propertyName + ": " + why;
                    return nullptr;
                }
                return new PropertyCmd<std::vector<ParticleEmissionBurst>>(project, sceneId, entity, component, propertyName, bursts, onChanged);
            }
            if (component == ComponentType::ParticleSystemComponent && propertyName == "textureSheetAnimation.sprites") {
                std::vector<int> frames;
                bool ok = value.is_array();
                for (size_t i = 0; ok && i < value.size(); i++) {
                    ok = isIntegral(value[i]) && integralValue(value[i]) >= 0 && integralValue(value[i]) < MAX_SPRITE_FRAMES;
                    if (ok) frames.push_back((int)integralValue(value[i]));
                }
                if (!ok) {
                    error = "Property " + propertyName + " is a list of sprite frame ids (0.." + std::to_string(MAX_SPRITE_FRAMES - 1) + ").";
                    return nullptr;
                }
                return new PropertyCmd<std::vector<int>>(project, sceneId, entity, component, propertyName, frames, onChanged);
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

// A Line or Trail Renderer's "texture": the built-in line shader's base map, stored as
// shaderTextures.u_baseTexture (one storage, LineRenderer::setTexture)
// A Line Renderer's data picks its mode: the whole `lines` list (the segment list, in the
// GameObject's space as it always was) is Segments mode in local space, the whole `positions` list
// Polyline mode; unless the same call sets mode (and, for lines, useWorldSpace) itself
void inferLineMode(std::vector<McpValues::Change>& out) {
    bool lines = false, positions = false, mode = false, worldSpace = false;
    for (const McpValues::Change& change : out) {
        if (change.component != ComponentType::LineRendererComponent) continue;
        if (change.op == "set" && change.property == "lines") lines = true;
        if (change.op == "set" && change.property == "positions") positions = true;
        if (change.property == "mode") mode = true;
        if (change.property == "useWorldSpace") worldSpace = true;
    }
    if (mode || lines == positions) return;
    McpValues::Change change;
    change.component = ComponentType::LineRendererComponent;
    change.property = "mode";
    change.value = lines ? "segments" : "polyline";
    out.push_back(change);
    if (lines && !worldSpace) {
        change.property = "useWorldSpace";
        change.value = false;
        out.push_back(change);
    }
}

void aliasLineTexture(McpValues::Change& change) {
    if ((change.component == ComponentType::LineRendererComponent || change.component == ComponentType::TrailRendererComponent) &&
            change.property == "texture") {
        change.property = std::string("shaderTextures.") + LINE_BASE_TEXTURE_UNIFORM;
    }
}

}

namespace {

// names from before a component's field was renamed: a 2D collider's "vertices" are its "points"
void normalizeChangeName(McpValues::Change& change) {
    if (change.component != ComponentType::Rigidbody2DComponent || change.property.compare(0, 10, "colliders[") != 0) return;
    const size_t at = change.property.find("].vertices");
    if (at != std::string::npos) change.property.replace(at, 10, "].points");
}

}

bool McpValues::parseChanges(const Json& changes, std::vector<Change>& out, std::string& error) {
    if (!parseChangesAsGiven(changes, out, error)) return false;
    for (Change& change : out) normalizeChangeName(change);
    return true;
}

bool McpValues::parseChangesAsGiven(const Json& changes, std::vector<Change>& out, std::string& error) {
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
                aliasLineTexture(change);
                out.push_back(change);
            }
        }
        inferLineMode(out);
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
        aliasLineTexture(change);
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
    inferLineMode(out);
    return true;
}

Json McpValues::canvasGroupStateToJson(Scene* scene, Entity entity) {
    if (!scene || !scene->isEntityCreated(entity) || !scene->findComponent<RectTransformComponent>(entity)) return nullptr;
    const CanvasGroupState state = scene->getSystem<UISystem>()->getCanvasGroupState(entity);
    if (state.group == NULL_ENTITY) return nullptr;
    Json out = {{"inherited_alpha", state.alpha}, {"interactable", state.interactable},
                {"blocks_raycasts", state.blocksRaycasts}, {"group", EntityPath::of(scene, state.group)}};
    // a group itself: whether its Flatten layer is drawn, and what the layers cost last frame
    if (const CanvasGroupComponent* group = scene->findComponent<CanvasGroupComponent>(entity)) {
        if (group->renderMode == CanvasGroupRenderMode::FLATTEN) {
            const RenderSystem::CanvasGroupRenderStats& stats = scene->getSystem<RenderSystem>()->getCanvasGroupRenderStats();
            out["flatten"] = {{"layered", group->layered}, {"layer_alpha", group->layerAlpha},
                              {"scene_layers_last_frame", stats.layers}, {"scene_layer_targets", stats.pooled},
                              {"scene_layer_targets_created", stats.targetsCreated}};
        }
    }
    return out;
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
                     isPlayerWrapProperty(component, change.property) ||
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
                            error = "Unknown property \"" + edit.property + "\" of " + componentLabel + " on " + label +
                                    ". Properties: " + propertyNamesText(scene, entity, component) +
                                    " (custom ones also by their scene-file keys, see get_entity detail=\"yaml\").";
                            // the Vertex Animator's edit-mode Preview is a runtime flag, never saved
                            // (no undo step, no scene change): its route is Lua
                            if (component == ComponentType::VertexAnimatorComponent && edit.property == "preview") {
                                error = "vertexanimator.preview is not a saved property (the Inspector's Preview toggle, a runtime flag). "
                                        "To animate it in edit mode: eval_lua {allow_edit_mode: true, code: \"object(" +
                                        std::to_string(entity) + ", 'Model'):getVertexAnimator().preview = true\"} (false to stop; "
                                        "play mode always animates).";
                            }
                        } else {
                            error = componentLabel + "." + edit.property + " on " + label + ": " + why;
                        }
                        return false;
                    }
                    syncListCount(value, edit.property);
                }
                if (component == ComponentType::AnimationPlayerComponent) {
                    std::string why;
                    if (!normalizePlayerClips(project, scene->scene->findComponent<AnimationPlayerComponent>(entity), value, why)) {
                        error = componentLabel + " on " + label + ": " + why;
                        return false;
                    }
                }
                if (component == ComponentType::VertexAnimatorComponent) {
                    std::string why;
                    if (!normalizeVertexAnimatorAttachments(scene, value, why)) {
                        error = componentLabel + " on " + label + ": " + why;
                        return false;
                    }
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
        redoFailure.clear();
        const unsigned long long mark = safewrite::scopedFailureCount();
        for (size_t i = 0; i < done.size(); i++) {
            if (!done[i]->execute()) {
                // the reason, before the rollback runs other steps
                redoFailure = done[i]->failure();
                if (redoFailure.empty()) redoFailure = safewrite::scopedFailureSince(mark);
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

void McpStepsCmd::undo() {
    for (auto it = done.rbegin(); it != done.rend(); ++it) {
        (*it)->undo();
        const std::string problem = (*it)->takeUndoProblem();
        if (!problem.empty()) undoProblem += (undoProblem.empty() ? "" : "; ") + problem;
        refresh();
    }
}

std::vector<Command::Touched> McpStepsCmd::touchedPaths(bool undo) const {
    std::vector<Touched> all;
    for (const std::unique_ptr<Command>& step : done) {
        std::vector<Touched> some = step->touchedPaths(undo);
        all.insert(all.end(), some.begin(), some.end());
    }
    return all;
}

std::string McpStepsCmd::undoRefusal() const {
    for (const std::unique_ptr<Command>& step : done) {
        std::string why = step->undoRefusal();
        if (!why.empty()) return why;
    }
    return std::string();
}

std::string McpStepsCmd::redoRefusal() const {
    for (const std::unique_ptr<Command>& step : done) {
        std::string why = step->redoRefusal();
        if (!why.empty()) return why;
    }
    return std::string();
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
