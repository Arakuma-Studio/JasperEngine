// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "reflect/Enums.h"
#include "component/ActionComponent.h"
#include "component/RectTransformComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/FogComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/Joint2DComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/RigidbodyComponent.h"
#include "particles/LegacyParticleSystem.h"
#include "component/ScriptComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/LightComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/SpriteComponent.h"

#include "Project.h"
#include "component/FluidSimulationComponent.h"
#include "component/BuoyancyComponent.h"
#include "animation/ClipTypes.h"
#include "animation/AnimatorTypes.h"
#include "component/ForceFieldComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/WaterVolumeComponent.h"
#include "component/CanvasGroupComponent.h"
#include "component/LineRendererComponent.h"
#include "render/LodFade.h"
#include "component/PhysicsCommon.h"
#include "component/Rigidbody2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/AlphaTweenComponent.h"
#include "component/OnScreenControlComponents.h"
#include "render/LineRibbonBuilder.h"

#include <cctype>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::reflect;

namespace {

    // Easing names are read case-insensitively, with '-' for '_' (the AI editor actions and
    // hand-written files use both)
    std::string normalizeEaseName(std::string_view text) {
        std::string normalized;
        normalized.reserve(text.size());
        for (char ch : text) {
            normalized.push_back(ch == '-' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
        return normalized;
    }

    template<typename E>
    constexpr int v(E value) { return static_cast<int>(value); }

    constexpr const char* kFogTypeExpSquaredAliases[] = {"exponentialsquared", "exponentialSquared"};
    constexpr const char* kBlendNoneAliases[] = {"OPAQUE"};
    constexpr const char* kScriptLuaAliases[] = {"script_lua"};
    constexpr const char* kScriptCppAliases[] = {"subclass", "script_class"};

    constexpr EnumItem kActionStateItems[] = {
        {v(ActionState::Running), "Running", "Running", "ActionState::Running", "running"},
        {v(ActionState::Paused), "Paused", "Paused", "ActionState::Paused", "paused"},
        {v(ActionState::Stopped), "Stopped", "Stopped", "ActionState::Stopped", "stopped"},
    };

    constexpr EnumItem kAnchorPresetItems[] = {
        {v(AnchorPreset::NONE), "NONE", "None", "AnchorPreset::NONE", "none"},
        {v(AnchorPreset::TOP_LEFT), "TOP_LEFT", "Top Left", "AnchorPreset::TOP_LEFT", "top_left"},
        {v(AnchorPreset::TOP_RIGHT), "TOP_RIGHT", "Top Right", "AnchorPreset::TOP_RIGHT", "top_right"},
        {v(AnchorPreset::BOTTOM_LEFT), "BOTTOM_LEFT", "Bottom Left", "AnchorPreset::BOTTOM_LEFT", "bottom_left"},
        {v(AnchorPreset::BOTTOM_RIGHT), "BOTTOM_RIGHT", "Bottom Right", "AnchorPreset::BOTTOM_RIGHT", "bottom_right"},
        {v(AnchorPreset::CENTER_LEFT), "CENTER_LEFT", "Center Left", "AnchorPreset::CENTER_LEFT", "center_left"},
        {v(AnchorPreset::CENTER_TOP), "CENTER_TOP", "Center Top", "AnchorPreset::CENTER_TOP", "center_top"},
        {v(AnchorPreset::CENTER_RIGHT), "CENTER_RIGHT", "Center Right", "AnchorPreset::CENTER_RIGHT", "center_right"},
        {v(AnchorPreset::CENTER_BOTTOM), "CENTER_BOTTOM", "Center Bottom", "AnchorPreset::CENTER_BOTTOM", "center_bottom"},
        {v(AnchorPreset::CENTER), "CENTER", "Center", "AnchorPreset::CENTER", "center"},
        {v(AnchorPreset::LEFT_WIDE), "LEFT_WIDE", "Left Wide", "AnchorPreset::LEFT_WIDE", "left_wide"},
        {v(AnchorPreset::TOP_WIDE), "TOP_WIDE", "Top Wide", "AnchorPreset::TOP_WIDE", "top_wide"},
        {v(AnchorPreset::RIGHT_WIDE), "RIGHT_WIDE", "Right Wide", "AnchorPreset::RIGHT_WIDE", "right_wide"},
        {v(AnchorPreset::BOTTOM_WIDE), "BOTTOM_WIDE", "Bottom Wide", "AnchorPreset::BOTTOM_WIDE", "bottom_wide"},
        {v(AnchorPreset::VERTICAL_CENTER_WIDE), "VERTICAL_CENTER_WIDE", "Vertical Center Wide", "AnchorPreset::VERTICAL_CENTER_WIDE", "vertical_center_wide"},
        {v(AnchorPreset::HORIZONTAL_CENTER_WIDE), "HORIZONTAL_CENTER_WIDE", "Horizontal Center Wide", "AnchorPreset::HORIZONTAL_CENTER_WIDE", "horizontal_center_wide"},
        {v(AnchorPreset::FULL_LAYOUT), "FULL_LAYOUT", "Full Layout", "AnchorPreset::FULL_LAYOUT", "full_layout"},
    };

    constexpr EnumItem kAtmosphereQualityOverrideItems[] = {
        {v(AtmosphereQualityOverride::PROJECT), "project", "Project", "AtmosphereQualityOverride::PROJECT"},
        {v(AtmosphereQualityOverride::LOW), "low", "Low", "AtmosphereQualityOverride::LOW"},
        {v(AtmosphereQualityOverride::MEDIUM), "medium", "Medium", "AtmosphereQualityOverride::MEDIUM"},
    };

    constexpr EnumItem kAudioSourceAttenuationItems[] = {
        {v(AudioSourceAttenuation::NO_ATTENUATION), "no_attenuation", "No Attenuation", "AudioSourceAttenuation::NO_ATTENUATION"},
        {v(AudioSourceAttenuation::INVERSE_DISTANCE), "inverse_distance", "Inverse Distance", "AudioSourceAttenuation::INVERSE_DISTANCE"},
        {v(AudioSourceAttenuation::LINEAR_DISTANCE), "linear_distance", "Linear Distance", "AudioSourceAttenuation::LINEAR_DISTANCE"},
        {v(AudioSourceAttenuation::EXPONENTIAL_DISTANCE), "exponential_distance", "Exponential Distance", "AudioSourceAttenuation::EXPONENTIAL_DISTANCE"},
    };

    constexpr EnumItem kAudioSourceStateItems[] = {
        {v(AudioSourceState::Playing), "playing", "Playing", "AudioSourceState::Playing"},
        {v(AudioSourceState::Paused), "paused", "Paused", "AudioSourceState::Paused"},
        {v(AudioSourceState::Stopped), "stopped", "Stopped", "AudioSourceState::Stopped"},
    };

    constexpr EnumItem kBodyTypeItems[] = {
        {v(BodyType::STATIC), "static", "Static", "BodyType::STATIC"},
        {v(BodyType::KINEMATIC), "kinematic", "Kinematic", "BodyType::KINEMATIC"},
        {v(BodyType::DYNAMIC), "dynamic", "Dynamic", "BodyType::DYNAMIC"},
    };

    // the YAML and the MCP names differ (S08 Risks: unified only by a future format version)
    constexpr EnumItem kCameraTypeItems[] = {
        {v(CameraType::CAMERA_UI), "camera_ui", "UI", "CameraType::CAMERA_UI", "ui", {}, true},
        {v(CameraType::CAMERA_ORTHO), "camera_ortho", "Orthographic", "CameraType::CAMERA_ORTHO", "orthographic"},
        {v(CameraType::CAMERA_PERSPECTIVE), "camera_perspective", "Perspective", "CameraType::CAMERA_PERSPECTIVE", "perspective"},
    };
    constexpr int kCameraTypeOrder[] = {v(CameraType::CAMERA_ORTHO), v(CameraType::CAMERA_PERSPECTIVE), v(CameraType::CAMERA_UI)};

    constexpr EnumItem kCascadeSplitModeItems[] = {
        {v(CascadeSplitMode::AUTO), "auto", "Auto", "CascadeSplitMode::AUTO"},
        {v(CascadeSplitMode::MANUAL), "manual", "Manual", "CascadeSplitMode::MANUAL"},
    };

    constexpr EnumItem kCloudsPresetItems[] = {
        {v(CloudsPreset::CUSTOM), "custom", "Custom", "CloudsPreset::CUSTOM"},
        {v(CloudsPreset::SPARSE), "sparse", "Sparse", "CloudsPreset::SPARSE"},
        {v(CloudsPreset::CLOUDY), "cloudy", "Cloudy", "CloudsPreset::CLOUDY"},
        {v(CloudsPreset::OVERCAST), "overcast", "Overcast", "CloudsPreset::OVERCAST"},
        {v(CloudsPreset::STORMY), "stormy", "Stormy", "CloudsPreset::STORMY"},
    };

    constexpr EnumItem kCloudsRenderingItems[] = {
        {v(CloudsRendering::AUTO), "auto", "Automatic", "CloudsRendering::AUTO"},
        {v(CloudsRendering::SKY_MAP), "sky_map", "Sky Map", "CloudsRendering::SKY_MAP"},
        {v(CloudsRendering::SCREEN), "screen", "Screen", "CloudsRendering::SCREEN"},
    };

    constexpr EnumItem kCollider2DTypeItems[] = {
        {v(Collider2DType::POLYGON), "polygon", "Polygon", "Collider2DType::POLYGON"},
        {v(Collider2DType::CIRCLE), "circle", "Circle", "Collider2DType::CIRCLE"},
        {v(Collider2DType::CAPSULE), "capsule", "Capsule", "Collider2DType::CAPSULE"},
        {v(Collider2DType::SEGMENT), "segment", "Segment", "Collider2DType::SEGMENT"},
        {v(Collider2DType::CHAIN), "chain", "Chain", "Collider2DType::CHAIN"},
    };

    constexpr EnumItem kCollider3DSourceItems[] = {
        {v(Collider3DSource::NONE), "none", "None", "Collider3DSource::NONE"},
        {v(Collider3DSource::RAW_VERTICES), "raw_vertices", "Raw Vertices", "Collider3DSource::RAW_VERTICES"},
        {v(Collider3DSource::RAW_MESH), "raw_mesh", "Raw Mesh", "Collider3DSource::RAW_MESH"},
        {v(Collider3DSource::ENTITY_MESH), "entity_mesh", "GameObject Mesh", "Collider3DSource::ENTITY_MESH"},
        {v(Collider3DSource::ENTITY_HEIGHTFIELD), "entity_heightfield", "GameObject Heightfield", "Collider3DSource::ENTITY_HEIGHTFIELD"},
    };

    constexpr EnumItem kCollider3DTypeItems[] = {
        {v(Collider3DType::SPHERE), "sphere", "Sphere", "Collider3DType::SPHERE"},
        {v(Collider3DType::BOX), "box", "Box", "Collider3DType::BOX"},
        {v(Collider3DType::CAPSULE), "capsule", "Capsule", "Collider3DType::CAPSULE"},
        {v(Collider3DType::TAPERED_CAPSULE), "tapered_capsule", "Tapered Capsule", "Collider3DType::TAPERED_CAPSULE"},
        {v(Collider3DType::CYLINDER), "cylinder", "Cylinder", "Collider3DType::CYLINDER"},
        {v(Collider3DType::CONVEX_HULL), "convex_hull", "Convex Hull", "Collider3DType::CONVEX_HULL"},
        {v(Collider3DType::MESH), "mesh", "Mesh", "Collider3DType::MESH"},
        {v(Collider3DType::HEIGHTFIELD), "heightfield", "Heightfield", "Collider3DType::HEIGHTFIELD"},
    };
    constexpr int kCollider3DTypeOrder[] = {v(Collider3DType::BOX), v(Collider3DType::SPHERE), v(Collider3DType::CAPSULE),
        v(Collider3DType::TAPERED_CAPSULE), v(Collider3DType::CYLINDER), v(Collider3DType::CONVEX_HULL), v(Collider3DType::MESH),
        v(Collider3DType::HEIGHTFIELD)};

    constexpr EnumItem kCullingModeItems[] = {
        {v(CullingMode::BACK), "back", "Back", "CullingMode::BACK"},
        {v(CullingMode::FRONT), "front", "Front", "CullingMode::FRONT"},
    };

    constexpr EnumItem kEaseTypeItems[] = {
        {v(EaseType::LINEAR), "LINEAR", "Linear", "EaseType::LINEAR", "linear"},
        {v(EaseType::QUAD_IN), "QUAD_IN", "Quad In", "EaseType::QUAD_IN", "quad_in"},
        {v(EaseType::QUAD_OUT), "QUAD_OUT", "Quad Out", "EaseType::QUAD_OUT", "quad_out"},
        {v(EaseType::QUAD_IN_OUT), "QUAD_IN_OUT", "Quad In Out", "EaseType::QUAD_IN_OUT", "quad_in_out"},
        {v(EaseType::CUBIC_IN), "CUBIC_IN", "Cubic In", "EaseType::CUBIC_IN", "cubic_in"},
        {v(EaseType::CUBIC_OUT), "CUBIC_OUT", "Cubic Out", "EaseType::CUBIC_OUT", "cubic_out"},
        {v(EaseType::CUBIC_IN_OUT), "CUBIC_IN_OUT", "Cubic In Out", "EaseType::CUBIC_IN_OUT", "cubic_in_out"},
        {v(EaseType::QUART_IN), "QUART_IN", "Quart In", "EaseType::QUART_IN", "quart_in"},
        {v(EaseType::QUART_OUT), "QUART_OUT", "Quart Out", "EaseType::QUART_OUT", "quart_out"},
        {v(EaseType::QUART_IN_OUT), "QUART_IN_OUT", "Quart In Out", "EaseType::QUART_IN_OUT", "quart_in_out"},
        {v(EaseType::QUINT_IN), "QUINT_IN", "Quint In", "EaseType::QUINT_IN", "quint_in"},
        {v(EaseType::QUINT_OUT), "QUINT_OUT", "Quint Out", "EaseType::QUINT_OUT", "quint_out"},
        {v(EaseType::QUINT_IN_OUT), "QUINT_IN_OUT", "Quint In Out", "EaseType::QUINT_IN_OUT", "quint_in_out"},
        {v(EaseType::SINE_IN), "SINE_IN", "Sine In", "EaseType::SINE_IN", "sine_in"},
        {v(EaseType::SINE_OUT), "SINE_OUT", "Sine Out", "EaseType::SINE_OUT", "sine_out"},
        {v(EaseType::SINE_IN_OUT), "SINE_IN_OUT", "Sine In Out", "EaseType::SINE_IN_OUT", "sine_in_out"},
        {v(EaseType::EXPO_IN), "EXPO_IN", "Expo In", "EaseType::EXPO_IN", "expo_in"},
        {v(EaseType::EXPO_OUT), "EXPO_OUT", "Expo Out", "EaseType::EXPO_OUT", "expo_out"},
        {v(EaseType::EXPO_IN_OUT), "EXPO_IN_OUT", "Expo In Out", "EaseType::EXPO_IN_OUT", "expo_in_out"},
        {v(EaseType::CIRC_IN), "CIRC_IN", "Circ In", "EaseType::CIRC_IN", "circ_in"},
        {v(EaseType::CIRC_OUT), "CIRC_OUT", "Circ Out", "EaseType::CIRC_OUT", "circ_out"},
        {v(EaseType::CIRC_IN_OUT), "CIRC_IN_OUT", "Circ In Out", "EaseType::CIRC_IN_OUT", "circ_in_out"},
        {v(EaseType::ELASTIC_IN), "ELASTIC_IN", "Elastic In", "EaseType::ELASTIC_IN", "elastic_in"},
        {v(EaseType::ELASTIC_OUT), "ELASTIC_OUT", "Elastic Out", "EaseType::ELASTIC_OUT", "elastic_out"},
        {v(EaseType::ELASTIC_IN_OUT), "ELASTIC_IN_OUT", "Elastic In Out", "EaseType::ELASTIC_IN_OUT", "elastic_in_out"},
        {v(EaseType::BACK_IN), "BACK_IN", "Back In", "EaseType::BACK_IN", "back_in"},
        {v(EaseType::BACK_OUT), "BACK_OUT", "Back Out", "EaseType::BACK_OUT", "back_out"},
        {v(EaseType::BACK_IN_OUT), "BACK_IN_OUT", "Back In Out", "EaseType::BACK_IN_OUT", "back_in_out"},
        {v(EaseType::BOUNCE_IN), "BOUNCE_IN", "Bounce In", "EaseType::BOUNCE_IN", "bounce_in"},
        {v(EaseType::BOUNCE_OUT), "BOUNCE_OUT", "Bounce Out", "EaseType::BOUNCE_OUT", "bounce_out"},
        {v(EaseType::BOUNCE_IN_OUT), "BOUNCE_IN_OUT", "Bounce In Out", "EaseType::BOUNCE_IN_OUT", "bounce_in_out"},
        {v(EaseType::STEP), "STEP", "Step", "EaseType::STEP", "step"},
        {v(EaseType::CUSTOM), "CUSTOM", "Custom", "EaseType::CUSTOM", "custom"},
        {v(EaseType::CURVE), "CURVE", "Curve", "EaseType::CURVE", "curve"},
    };
    constexpr int kEaseTypeOrder[] = {v(EaseType::LINEAR), v(EaseType::STEP),
        v(EaseType::QUAD_IN), v(EaseType::QUAD_OUT), v(EaseType::QUAD_IN_OUT), v(EaseType::CUBIC_IN), v(EaseType::CUBIC_OUT),
        v(EaseType::CUBIC_IN_OUT), v(EaseType::QUART_IN), v(EaseType::QUART_OUT), v(EaseType::QUART_IN_OUT), v(EaseType::QUINT_IN),
        v(EaseType::QUINT_OUT), v(EaseType::QUINT_IN_OUT), v(EaseType::SINE_IN), v(EaseType::SINE_OUT), v(EaseType::SINE_IN_OUT),
        v(EaseType::EXPO_IN), v(EaseType::EXPO_OUT), v(EaseType::EXPO_IN_OUT), v(EaseType::CIRC_IN), v(EaseType::CIRC_OUT),
        v(EaseType::CIRC_IN_OUT), v(EaseType::ELASTIC_IN), v(EaseType::ELASTIC_OUT), v(EaseType::ELASTIC_IN_OUT), v(EaseType::BACK_IN),
        v(EaseType::BACK_OUT), v(EaseType::BACK_IN_OUT), v(EaseType::BOUNCE_IN), v(EaseType::BOUNCE_OUT), v(EaseType::BOUNCE_IN_OUT),
        v(EaseType::CURVE), v(EaseType::CUSTOM)};

    constexpr EnumItem kFogColorModeItems[] = {
        {v(FogColorMode::COLOR), "color", "Constant Color", "FogColorMode::COLOR"},
        {v(FogColorMode::SKY), "sky", "Sky Color", "FogColorMode::SKY"},
    };

    constexpr EnumItem kFogTypeItems[] = {
        {v(FogType::LINEAR), "linear", "Linear", "FogType::LINEAR"},
        {v(FogType::EXPONENTIAL), "exponential", "Exponential", "FogType::EXPONENTIAL"},
        {v(FogType::EXPONENTIALSQUARED), "exponential_squared", "Exponential Squared", "FogType::EXPONENTIALSQUARED", nullptr, kFogTypeExpSquaredAliases},
        {v(FogType::HEIGHT), "height", "Height", "FogType::HEIGHT"},
    };

    constexpr EnumItem kFogVolumeFalloffItems[] = {
        {v(FogVolumeFalloff::LINEAR), "linear", "Linear", "FogVolumeFalloff::LINEAR"},
        {v(FogVolumeFalloff::EXPONENTIAL), "exponential", "Exponential", "FogVolumeFalloff::EXPONENTIAL"},
    };

    constexpr EnumItem kFogVolumeShapeItems[] = {
        {v(FogVolumeShape::BOX), "box", "Box", "FogVolumeShape::BOX"},
        {v(FogVolumeShape::SPHERE), "sphere", "Sphere", "FogVolumeShape::SPHERE"},
    };

    constexpr EnumItem kJoint2DTypeItems[] = {
        {v(Joint2DType::DISTANCE), "distance", "Distance", "Joint2DType::DISTANCE"},
        {v(Joint2DType::REVOLUTE), "revolute", "Revolute", "Joint2DType::REVOLUTE"},
        {v(Joint2DType::PRISMATIC), "prismatic", "Prismatic", "Joint2DType::PRISMATIC"},
        {v(Joint2DType::MOUSE), "mouse", "Mouse", "Joint2DType::MOUSE"},
        {v(Joint2DType::WHEEL), "wheel", "Wheel", "Joint2DType::WHEEL"},
        {v(Joint2DType::WELD), "weld", "Weld", "Joint2DType::WELD"},
        {v(Joint2DType::MOTOR), "motor", "Motor", "Joint2DType::MOTOR"},
    };

    constexpr EnumItem kJoint3DTypeItems[] = {
        {v(Joint3DType::FIXED), "fixed", "Fixed", "Joint3DType::FIXED"},
        {v(Joint3DType::DISTANCE), "distance", "Distance", "Joint3DType::DISTANCE"},
        {v(Joint3DType::POINT), "point", "Point", "Joint3DType::POINT"},
        {v(Joint3DType::HINGE), "hinge", "Hinge", "Joint3DType::HINGE"},
        {v(Joint3DType::CONE), "cone", "Cone", "Joint3DType::CONE"},
        {v(Joint3DType::PRISMATIC), "prismatic", "Prismatic", "Joint3DType::PRISMATIC"},
        {v(Joint3DType::SWINGTWIST), "swingtwist", "Swing Twist", "Joint3DType::SWINGTWIST", "swing_twist"},
        {v(Joint3DType::SIXDOF), "sixdof", "Six DOF", "Joint3DType::SIXDOF", "six_dof"},
        {v(Joint3DType::PATH), "path", "Path", "Joint3DType::PATH"},
        {v(Joint3DType::GEAR), "gear", "Gear", "Joint3DType::GEAR"},
        {v(Joint3DType::RACKANDPINON), "rackandpinion", "Rack and Pinion", "Joint3DType::RACKANDPINON", "rack_and_pinion"},
        {v(Joint3DType::PULLEY), "pulley", "Pulley", "Joint3DType::PULLEY"},
    };

    constexpr EnumItem kLayoutGroupTypeItems[] = {
        {v(LayoutGroupType::VERTICAL), "vertical", "Vertical", "LayoutGroupType::VERTICAL"},
        {v(LayoutGroupType::HORIZONTAL), "horizontal", "Horizontal", "LayoutGroupType::HORIZONTAL"},
        {v(LayoutGroupType::VERTICAL_WRAP), "vertical_wrap", "Vertical Wrap", "LayoutGroupType::VERTICAL_WRAP"},
        {v(LayoutGroupType::HORIZONTAL_WRAP), "horizontal_wrap", "Horizontal Wrap", "LayoutGroupType::HORIZONTAL_WRAP"},
    };

    constexpr EnumItem kFluidOutputItems[] = {
        {v(FluidOutput::SCREEN), "screen", "Screen", "FluidOutput::SCREEN"},
        {v(FluidOutput::TEXTURE), "texture", "Texture", "FluidOutput::TEXTURE"},
        {v(FluidOutput::MATERIAL), "material", "Material", "FluidOutput::MATERIAL"},
    };
    constexpr int kFluidOutputOrder[] = {v(FluidOutput::SCREEN), v(FluidOutput::MATERIAL), v(FluidOutput::TEXTURE)};

    constexpr EnumItem kForceFieldShapeItems[] = {
        {v(ForceFieldShapeType::Sphere), "sphere", "Sphere", "ForceFieldShapeType::Sphere"},
        {v(ForceFieldShapeType::Box), "box", "Box", "ForceFieldShapeType::Box"},
        {v(ForceFieldShapeType::Cylinder), "cylinder", "Cylinder", "ForceFieldShapeType::Cylinder"},
        {v(ForceFieldShapeType::Infinite), "infinite", "Infinite", "ForceFieldShapeType::Infinite"},
    };

    constexpr EnumItem kForceFieldKindItems[] = {
        {v(ForceFieldKind::Directional), "directional", "Directional", "ForceFieldKind::Directional"},
        {v(ForceFieldKind::Radial), "radial", "Radial", "ForceFieldKind::Radial"},
        {v(ForceFieldKind::Vortex), "vortex", "Vortex", "ForceFieldKind::Vortex"},
        {v(ForceFieldKind::Turbulence), "turbulence", "Turbulence", "ForceFieldKind::Turbulence"},
        {v(ForceFieldKind::Drag), "drag", "Drag", "ForceFieldKind::Drag"},
    };

    // upstream's components: the tokens their codecs write (StreamUpstream.cpp)
    constexpr EnumItem kLineRendererModeItems[] = {
        {v(LineRendererMode::Segments), "segments", "Segments", "LineRendererMode::Segments"},
        {v(LineRendererMode::Polyline), "polyline", "Polyline", "LineRendererMode::Polyline"},
    };
    constexpr EnumItem kLineWidthUnitItems[] = {
        {v(LineWidthUnit::World), "world", "World", "LineWidthUnit::World"},
        {v(LineWidthUnit::Pixels), "pixels", "Pixels", "LineWidthUnit::Pixels"},
    };
    constexpr EnumItem kLineAlignmentItems[] = {
        {v(LineAlignment::View), "view", "View", "LineAlignment::View"},
        {v(LineAlignment::TransformZ), "transform_z", "Transform Z", "LineAlignment::TransformZ"},
    };
    constexpr EnumItem kLineTextureModeItems[] = {
        {v(LineTextureMode::Stretch), "stretch", "Stretch", "LineTextureMode::Stretch"},
        {v(LineTextureMode::Tile), "tile", "Tile", "LineTextureMode::Tile"},
        {v(LineTextureMode::DistributePerSegment), "distribute_per_segment", "Distribute Per Segment", "LineTextureMode::DistributePerSegment"},
        {v(LineTextureMode::RepeatPerSegment), "repeat_per_segment", "Repeat Per Segment", "LineTextureMode::RepeatPerSegment"},
    };
    constexpr const char* kCanvasGroupPerElementAliases[] = {"PER_ELEMENT"};
    constexpr const char* kCanvasGroupFlattenAliases[] = {"FLATTEN"};
    constexpr EnumItem kCanvasGroupRenderModeItems[] = {
        {v(CanvasGroupRenderMode::PER_ELEMENT), "per_element", "Per Element", "CanvasGroupRenderMode::PER_ELEMENT", nullptr, kCanvasGroupPerElementAliases},
        {v(CanvasGroupRenderMode::FLATTEN), "flatten", "Flatten", "CanvasGroupRenderMode::FLATTEN", nullptr, kCanvasGroupFlattenAliases},
    };
    constexpr EnumItem kOnScreenStickModeItems[] = {
        {v(OnScreenStickMode::FIXED), "fixed", "Fixed", "OnScreenStickMode::FIXED"},
        {v(OnScreenStickMode::FLOATING), "floating", "Floating", "OnScreenStickMode::FLOATING"},
        {v(OnScreenStickMode::DYNAMIC), "dynamic", "Dynamic", "OnScreenStickMode::DYNAMIC"},
    };
    constexpr EnumItem kOnScreenStickAxesItems[] = {
        {v(OnScreenStickAxes::BOTH), "both", "Both", "OnScreenStickAxes::BOTH"},
        {v(OnScreenStickAxes::HORIZONTAL), "horizontal", "Horizontal", "OnScreenStickAxes::HORIZONTAL"},
        {v(OnScreenStickAxes::VERTICAL), "vertical", "Vertical", "OnScreenStickAxes::VERTICAL"},
    };
    constexpr EnumItem kOnScreenVisibilityItems[] = {
        {v(OnScreenVisibility::ALWAYS), "always", "Always", "OnScreenVisibility::ALWAYS"},
        {v(OnScreenVisibility::TOUCH_DEVICES_ONLY), "touch_devices_only", "Touch Devices Only", "OnScreenVisibility::TOUCH_DEVICES_ONLY"},
    };

    // the tokens upstream's rigidbody codecs write (StreamComponents.cpp)
    constexpr EnumItem kCollisionDetection2DItems[] = {
        {v(CollisionDetectionMode2D::DISCRETE), "discrete", "Discrete", "CollisionDetectionMode2D::DISCRETE"},
        {v(CollisionDetectionMode2D::CONTINUOUS), "continuous", "Continuous", "CollisionDetectionMode2D::CONTINUOUS"},
    };
    constexpr EnumItem kSleepMode2DItems[] = {
        {v(RigidbodySleepMode2D::NEVER_SLEEP), "never_sleep", "Never Sleep", "RigidbodySleepMode2D::NEVER_SLEEP"},
        {v(RigidbodySleepMode2D::START_AWAKE), "start_awake", "Start Awake", "RigidbodySleepMode2D::START_AWAKE"},
        {v(RigidbodySleepMode2D::START_ASLEEP), "start_asleep", "Start Asleep", "RigidbodySleepMode2D::START_ASLEEP"},
    };
    constexpr EnumItem kRigidbodyInterpolationItems[] = {
        {v(RigidbodyInterpolation::SCENE), "scene", "Scene", "RigidbodyInterpolation::SCENE"},
        {v(RigidbodyInterpolation::NONE), "none", "None", "RigidbodyInterpolation::NONE"},
        {v(RigidbodyInterpolation::INTERPOLATE), "interpolate", "Interpolate", "RigidbodyInterpolation::INTERPOLATE"},
        {v(RigidbodyInterpolation::EXTRAPOLATE), "extrapolate", "Extrapolate", "RigidbodyInterpolation::EXTRAPOLATE"},
    };
    constexpr EnumItem kJointAnchorSpaceItems[] = {
        {v(JointAnchorSpace::WORLD), "world", "World", "JointAnchorSpace::WORLD"},
        {v(JointAnchorSpace::LOCAL), "local", "Local", "JointAnchorSpace::LOCAL"},
    };
    constexpr EnumItem kJointBreakActionItems[] = {
        {v(JointBreakAction::IGNORE), "ignore", "Ignore", "JointBreakAction::IGNORE"},
        {v(JointBreakAction::CALLBACK_ONLY), "callback_only", "Callback Only", "JointBreakAction::CALLBACK_ONLY"},
        {v(JointBreakAction::DISABLE), "disable", "Disable", "JointBreakAction::DISABLE"},
        {v(JointBreakAction::DESTROY), "destroy", "Destroy", "JointBreakAction::DESTROY"},
    };
    constexpr EnumItem kJointMotionItems[] = {
        {v(JointMotion::LOCKED), "locked", "Locked", "JointMotion::LOCKED"},
        {v(JointMotion::LIMITED), "limited", "Limited", "JointMotion::LIMITED"},
        {v(JointMotion::FREE), "free", "Free", "JointMotion::FREE"},
    };
    constexpr EnumItem kJointSwingTypeItems[] = {
        {v(JointSwingType::CONE), "cone", "Cone", "JointSwingType::CONE"},
        {v(JointSwingType::PYRAMID), "pyramid", "Pyramid", "JointSwingType::PYRAMID"},
    };
    // the tokens upstream's codec wrote
    constexpr EnumItem kAlphaTweenTargetItems[] = {
        {v(AlphaTweenTarget::AUTO), "AUTO", "Auto", "AlphaTweenTarget::AUTO"},
        {v(AlphaTweenTarget::COLOR), "COLOR", "Color", "AlphaTweenTarget::COLOR"},
        {v(AlphaTweenTarget::CANVAS_GROUP), "CANVAS_GROUP", "Canvas Group", "AlphaTweenTarget::CANVAS_GROUP"},
    };

    constexpr EnumItem kWaterVolumeWavesItems[] = {
        {v(WaterVolumeWaves::FROM_MATERIAL), "from_material", "From Material", "WaterVolumeWaves::FROM_MATERIAL"},
        {v(WaterVolumeWaves::NONE), "none", "None", "WaterVolumeWaves::NONE"},
    };

    // an Animation Player clip entry's wrap (anim::wrapName): default is the clip's own
    constexpr EnumItem kClipWrapModeItems[] = {
        {v(ClipWrapMode::Default), "default", "Default", "ClipWrapMode::Default"},
        {v(ClipWrapMode::Once), "once", "Once", "ClipWrapMode::Once"},
        {v(ClipWrapMode::Loop), "loop", "Loop", "ClipWrapMode::Loop"},
        {v(ClipWrapMode::PingPong), "ping_pong", "Ping Pong", "ClipWrapMode::PingPong"},
        {v(ClipWrapMode::ClampForever), "clamp_forever", "Clamp Forever", "ClipWrapMode::ClampForever"},
    };

    // an Animation Player layer's blending (upstream anim layers): "override" / "additive"
    constexpr EnumItem kAnimationLayerBlendItems[] = {
        {v(AnimationLayerBlend::Override), "override", "Override", "AnimationLayerBlend::Override"},
        {v(AnimationLayerBlend::Additive), "additive", "Additive", "AnimationLayerBlend::Additive"},
    };

    // the Animator's (S21): YAML names snake_case like the controller files
    constexpr EnumItem kAnimatorUpdateModeItems[] = {
        {v(AnimatorUpdateMode::Normal), "normal", "Normal", "AnimatorUpdateMode::Normal"},
        {v(AnimatorUpdateMode::AnimatePhysics), "animate_physics", "Animate Physics", "AnimatorUpdateMode::AnimatePhysics"},
        {v(AnimatorUpdateMode::UnscaledTime), "unscaled_time", "Unscaled Time", "AnimatorUpdateMode::UnscaledTime"},
    };
    constexpr EnumItem kAnimatorCullingModeItems[] = {
        {v(AnimatorCullingMode::AlwaysAnimate), "always_animate", "Always Animate", "AnimatorCullingMode::AlwaysAnimate"},
        {v(AnimatorCullingMode::CullUpdateTransforms), "cull_update_transforms", "Cull Update Transforms", "AnimatorCullingMode::CullUpdateTransforms"},
        {v(AnimatorCullingMode::CullCompletely), "cull_completely", "Cull Completely", "AnimatorCullingMode::CullCompletely"},
    };
    constexpr EnumItem kAnimatorWriteBonesItems[] = {
        {v(AnimatorWriteBones::All), "all", "All", "AnimatorWriteBones::All"},
        {v(AnimatorWriteBones::Exposed), "exposed", "Exposed", "AnimatorWriteBones::Exposed"},
    };
    constexpr EnumItem kAnimatorEventModeItems[] = {
        {v(AnimatorEventMode::LeaderOnly), "leader_only", "Leader Only", "AnimatorEventMode::LeaderOnly"},
        {v(AnimatorEventMode::All), "all", "All", "AnimatorEventMode::All"},
    };
    constexpr EnumItem kAnimatorRetargetItems[] = {
        {v(AnimatorRetarget::Auto), "auto", "Auto", "AnimatorRetarget::Auto"},
        {v(AnimatorRetarget::Off), "off", "Off", "AnimatorRetarget::Off"},
    };
    constexpr EnumItem kAnimatorIKTypeItems[] = {
        {v(AnimatorIKType::TwoBone), "two_bone", "Two Bone", "AnimatorIKType::TwoBone"},
        {v(AnimatorIKType::Fabrik), "fabrik", "FABRIK", "AnimatorIKType::Fabrik"},
        {v(AnimatorIKType::LookAt), "look_at", "Look At", "AnimatorIKType::LookAt"},
        {v(AnimatorIKType::FootIK), "foot_ik", "Foot IK", "AnimatorIKType::FootIK"},
        {v(AnimatorIKType::FootLock), "foot_lock", "Foot Lock", "AnimatorIKType::FootLock"},
    };

    constexpr EnumItem kBuoyancyColumnsItems[] = {
        {v(BuoyancyColumns::AUTO_2x2), "auto_2x2", "Auto 2x2", "BuoyancyColumns::AUTO_2x2"},
        {v(BuoyancyColumns::AUTO_3x3), "auto_3x3", "Auto 3x3", "BuoyancyColumns::AUTO_3x3"},
        {v(BuoyancyColumns::CUSTOM), "custom", "Custom", "BuoyancyColumns::CUSTOM"},
    };

    constexpr EnumItem kLatticeInterpolationItems[] = {
        {v(LatticeInterpolation::SHARP), "sharp", "Sharp", "LatticeInterpolation::SHARP"},
        {v(LatticeInterpolation::SMOOTH), "smooth", "Smooth", "LatticeInterpolation::SMOOTH"},
        {v(LatticeInterpolation::CUBIC), "cubic", "Cubic", "LatticeInterpolation::CUBIC"},
    };

    constexpr EnumItem kLatticeOutsideItems[] = {
        {v(LatticeOutside::EXTEND), "extend", "Extend", "LatticeOutside::EXTEND"},
        {v(LatticeOutside::FADE), "fade", "Fade", "LatticeOutside::FADE"},
    };

    constexpr EnumItem kLatticeJiggleAnchorItems[] = {
        {v(LatticeJiggleAnchor::NEGATIVE_X), "negative_x", "-X", "LatticeJiggleAnchor::NEGATIVE_X"},
        {v(LatticeJiggleAnchor::POSITIVE_X), "positive_x", "+X", "LatticeJiggleAnchor::POSITIVE_X"},
        {v(LatticeJiggleAnchor::NEGATIVE_Y), "negative_y", "-Y", "LatticeJiggleAnchor::NEGATIVE_Y"},
        {v(LatticeJiggleAnchor::POSITIVE_Y), "positive_y", "+Y", "LatticeJiggleAnchor::POSITIVE_Y"},
        {v(LatticeJiggleAnchor::NEGATIVE_Z), "negative_z", "-Z", "LatticeJiggleAnchor::NEGATIVE_Z"},
        {v(LatticeJiggleAnchor::POSITIVE_Z), "positive_z", "+Z", "LatticeJiggleAnchor::POSITIVE_Z"},
        {v(LatticeJiggleAnchor::NONE), "none", "None", "LatticeJiggleAnchor::NONE"},
    };
    constexpr int kLatticeJiggleAnchorOrder[] = {v(LatticeJiggleAnchor::NONE), v(LatticeJiggleAnchor::NEGATIVE_X),
        v(LatticeJiggleAnchor::POSITIVE_X), v(LatticeJiggleAnchor::NEGATIVE_Y), v(LatticeJiggleAnchor::POSITIVE_Y),
        v(LatticeJiggleAnchor::NEGATIVE_Z), v(LatticeJiggleAnchor::POSITIVE_Z)};

    constexpr EnumItem kLatticeStageItems[] = {
        {v(LatticeStage::BEFORE_SKINNING), "before_skinning", "Before Skinning", "LatticeStage::BEFORE_SKINNING"},
        {v(LatticeStage::AFTER_SKINNING), "after_skinning", "After Skinning", "LatticeStage::AFTER_SKINNING"},
    };

    constexpr EnumItem kLatticeMaskChannelItems[] = {
        {v(LatticeMaskChannel::RED), "red", "Red", "LatticeMaskChannel::RED"},
        {v(LatticeMaskChannel::GREEN), "green", "Green", "LatticeMaskChannel::GREEN"},
        {v(LatticeMaskChannel::BLUE), "blue", "Blue", "LatticeMaskChannel::BLUE"},
        {v(LatticeMaskChannel::ALPHA), "alpha", "Alpha", "LatticeMaskChannel::ALPHA"},
        {v(LatticeMaskChannel::INVERTED_RED), "inverted_red", "Inverted Red", "LatticeMaskChannel::INVERTED_RED"},
        {v(LatticeMaskChannel::INVERTED_GREEN), "inverted_green", "Inverted Green", "LatticeMaskChannel::INVERTED_GREEN"},
        {v(LatticeMaskChannel::INVERTED_BLUE), "inverted_blue", "Inverted Blue", "LatticeMaskChannel::INVERTED_BLUE"},
        {v(LatticeMaskChannel::INVERTED_ALPHA), "inverted_alpha", "Inverted Alpha", "LatticeMaskChannel::INVERTED_ALPHA"},
        {v(LatticeMaskChannel::NONE), "none", "None", "LatticeMaskChannel::NONE"},
    };
    constexpr int kLatticeMaskChannelOrder[] = {v(LatticeMaskChannel::NONE), v(LatticeMaskChannel::RED), v(LatticeMaskChannel::GREEN),
        v(LatticeMaskChannel::BLUE), v(LatticeMaskChannel::ALPHA), v(LatticeMaskChannel::INVERTED_RED), v(LatticeMaskChannel::INVERTED_GREEN),
        v(LatticeMaskChannel::INVERTED_BLUE), v(LatticeMaskChannel::INVERTED_ALPHA)};

    constexpr EnumItem kLightUnitItems[] = {
        {v(LightUnit::ENGINE), "engine", "Engine", "LightUnit::ENGINE"},
        {v(LightUnit::LUX), "lux", "Lux", "LightUnit::LUX"},
        {v(LightUnit::CANDELA), "candela", "Candela", "LightUnit::CANDELA"},
        {v(LightUnit::LUMEN), "lumen", "Lumen", "LightUnit::LUMEN"},
    };

    constexpr EnumItem kLightShadingItems[] = {
        {v(LightShading::BOTH), "BOTH", "Both", "LightShading::BOTH", "both"},
        {v(LightShading::PIXEL), "PIXEL", "Per Pixel", "LightShading::PIXEL", "pixel"},
        {v(LightShading::VERTEX), "VERTEX", "Per Vertex", "LightShading::VERTEX", "vertex"},
    };

    constexpr EnumItem kLodFadeModeItems[] = {
        {v(LodFadeMode::NONE), "none", "None", "LodFadeMode::NONE"},
        {v(LodFadeMode::CROSS_FADE), "cross_fade", "Cross Fade", "LodFadeMode::CROSS_FADE"},
    };

    constexpr EnumItem kLightTypeItems[] = {
        {v(LightType::DIRECTIONAL), "directional", "Directional", "LightType::DIRECTIONAL"},
        {v(LightType::POINT), "point", "Point", "LightType::POINT"},
        {v(LightType::SPOT), "spot", "Spot", "LightType::SPOT"},
    };

    constexpr EnumItem kMaterialAlphaModeItems[] = {
        {v(MaterialAlphaMode::AUTO), "AUTO", "Auto", "MaterialAlphaMode::AUTO", "auto"},
        {v(MaterialAlphaMode::ALPHA_OPAQUE), "ALPHA_OPAQUE", "Opaque", "MaterialAlphaMode::ALPHA_OPAQUE", "opaque"},
        {v(MaterialAlphaMode::MASK), "MASK", "Mask", "MaterialAlphaMode::MASK", "mask"},
        {v(MaterialAlphaMode::BLEND), "BLEND", "Blend", "MaterialAlphaMode::BLEND", "blend"},
    };

    // NONE blends nothing: MCP reads it "opaque" (the "None" label still parses)
    constexpr EnumItem kMaterialBlendModeItems[] = {
        {v(MaterialBlendMode::AUTO), "AUTO", "Auto", "MaterialBlendMode::AUTO", "auto"},
        {v(MaterialBlendMode::NONE), "NONE", "None", "MaterialBlendMode::NONE", "opaque", kBlendNoneAliases},
        {v(MaterialBlendMode::ALPHA), "ALPHA", "Alpha", "MaterialBlendMode::ALPHA", "alpha"},
        {v(MaterialBlendMode::PREMULTIPLIED), "PREMULTIPLIED", "Premultiplied", "MaterialBlendMode::PREMULTIPLIED", "premultiplied"},
        {v(MaterialBlendMode::ADDITIVE), "ADDITIVE", "Additive", "MaterialBlendMode::ADDITIVE", "additive"},
        {v(MaterialBlendMode::MULTIPLY), "MULTIPLY", "Multiply", "MaterialBlendMode::MULTIPLY", "multiply"},
    };

    constexpr EnumItem kMaterialCullModeItems[] = {
        {v(MaterialCullMode::AUTO), "AUTO", "Auto", "MaterialCullMode::AUTO", "auto"},
        {v(MaterialCullMode::BACK), "BACK", "Back", "MaterialCullMode::BACK", "back"},
        {v(MaterialCullMode::FRONT), "FRONT", "Front", "MaterialCullMode::FRONT", "front"},
        {v(MaterialCullMode::OFF), "OFF", "Off", "MaterialCullMode::OFF", "off"},
    };

    constexpr EnumItem kMaterialDepthTestItems[] = {
        {v(MaterialDepthTest::AUTO), "AUTO", "Auto", "MaterialDepthTest::AUTO", "auto"},
        {v(MaterialDepthTest::LESS), "LESS", "Less", "MaterialDepthTest::LESS", "less"},
        {v(MaterialDepthTest::LESS_EQUAL), "LESS_EQUAL", "Less Equal", "MaterialDepthTest::LESS_EQUAL", "less_equal"},
        {v(MaterialDepthTest::EQUAL), "EQUAL", "Equal", "MaterialDepthTest::EQUAL", "equal"},
        {v(MaterialDepthTest::GREATER_EQUAL), "GREATER_EQUAL", "Greater Equal", "MaterialDepthTest::GREATER_EQUAL", "greater_equal"},
        {v(MaterialDepthTest::GREATER), "GREATER", "Greater", "MaterialDepthTest::GREATER", "greater"},
        {v(MaterialDepthTest::NOT_EQUAL), "NOT_EQUAL", "Not Equal", "MaterialDepthTest::NOT_EQUAL", "not_equal"},
        {v(MaterialDepthTest::ALWAYS), "ALWAYS", "Always", "MaterialDepthTest::ALWAYS", "always"},
    };

    constexpr EnumItem kMaterialDepthWriteItems[] = {
        {v(MaterialDepthWrite::AUTO), "AUTO", "Auto", "MaterialDepthWrite::AUTO", "auto"},
        {v(MaterialDepthWrite::ENABLED), "ENABLED", "Enabled", "MaterialDepthWrite::ENABLED", "on"},
        {v(MaterialDepthWrite::DISABLED), "DISABLED", "Disabled", "MaterialDepthWrite::DISABLED", "off"},
    };

    constexpr EnumItem kMaterialLightingItems[] = {
        {v(MaterialLighting::AUTO), "AUTO", "Auto", "MaterialLighting::AUTO", "auto"},
        {v(MaterialLighting::PIXEL), "PIXEL", "Per Pixel", "MaterialLighting::PIXEL", "pixel"},
        {v(MaterialLighting::VERTEX), "VERTEX", "Per Vertex", "MaterialLighting::VERTEX", "vertex"},
        {v(MaterialLighting::MIXED), "MIXED", "Mixed", "MaterialLighting::MIXED", "mixed"},
        {v(MaterialLighting::NONE), "NONE", "None", "MaterialLighting::NONE", "none"},
    };

    constexpr EnumItem kMaterialPassOrderItems[] = {
        {v(MaterialPassOrder::BEFORE), "BEFORE", "Before", "MaterialPassOrder::BEFORE", "before"},
        {v(MaterialPassOrder::AFTER), "AFTER", "After", "MaterialPassOrder::AFTER", "after"},
    };
    constexpr int kMaterialPassOrderOrder[] = {v(MaterialPassOrder::AFTER), v(MaterialPassOrder::BEFORE)};

    constexpr EnumItem kMaterialStencilCompareItems[] = {
        {v(MaterialStencilCompare::AUTO), "AUTO", "Auto", "MaterialStencilCompare::AUTO", "auto"},
        {v(MaterialStencilCompare::DISABLED), "DISABLED", "Off", "MaterialStencilCompare::DISABLED", "disabled"},
        {v(MaterialStencilCompare::NEVER), "NEVER", "Never", "MaterialStencilCompare::NEVER", "never"},
        {v(MaterialStencilCompare::LESS), "LESS", "Less", "MaterialStencilCompare::LESS", "less"},
        {v(MaterialStencilCompare::EQUAL), "EQUAL", "Equal", "MaterialStencilCompare::EQUAL", "equal"},
        {v(MaterialStencilCompare::LESS_EQUAL), "LESS_EQUAL", "Less Equal", "MaterialStencilCompare::LESS_EQUAL", "less_equal"},
        {v(MaterialStencilCompare::GREATER), "GREATER", "Greater", "MaterialStencilCompare::GREATER", "greater"},
        {v(MaterialStencilCompare::NOT_EQUAL), "NOT_EQUAL", "Not Equal", "MaterialStencilCompare::NOT_EQUAL", "not_equal"},
        {v(MaterialStencilCompare::GREATER_EQUAL), "GREATER_EQUAL", "Greater Equal", "MaterialStencilCompare::GREATER_EQUAL", "greater_equal"},
        {v(MaterialStencilCompare::ALWAYS), "ALWAYS", "Always", "MaterialStencilCompare::ALWAYS", "always"},
    };

    constexpr EnumItem kMaterialStencilOpItems[] = {
        {v(MaterialStencilOp::KEEP), "KEEP", "Keep", "MaterialStencilOp::KEEP", "keep"},
        {v(MaterialStencilOp::ZERO), "ZERO", "Zero", "MaterialStencilOp::ZERO", "zero"},
        {v(MaterialStencilOp::REPLACE), "REPLACE", "Replace", "MaterialStencilOp::REPLACE", "replace"},
        {v(MaterialStencilOp::INCR_CLAMP), "INCR_CLAMP", "Increment", "MaterialStencilOp::INCR_CLAMP", "incr_clamp"},
        {v(MaterialStencilOp::DECR_CLAMP), "DECR_CLAMP", "Decrement", "MaterialStencilOp::DECR_CLAMP", "decr_clamp"},
        {v(MaterialStencilOp::INVERT), "INVERT", "Invert", "MaterialStencilOp::INVERT", "invert"},
        {v(MaterialStencilOp::INCR_WRAP), "INCR_WRAP", "Increment Wrap", "MaterialStencilOp::INCR_WRAP", "incr_wrap"},
        {v(MaterialStencilOp::DECR_WRAP), "DECR_WRAP", "Decrement Wrap", "MaterialStencilOp::DECR_WRAP", "decr_wrap"},
    };

    constexpr EnumItem kParticleEmitterShapeItems[] = {
        {v(ParticleEmitterShape::Box), "Box", "Box", "ParticleEmitterShape::Box", "box"},
        {v(ParticleEmitterShape::Sphere), "Sphere", "Sphere", "ParticleEmitterShape::Sphere", "sphere"},
        {v(ParticleEmitterShape::Hemisphere), "Hemisphere", "Hemisphere", "ParticleEmitterShape::Hemisphere", "hemisphere"},
        {v(ParticleEmitterShape::Circle), "Circle", "Circle", "ParticleEmitterShape::Circle", "circle"},
        {v(ParticleEmitterShape::Cone), "Cone", "Cone", "ParticleEmitterShape::Cone", "cone"},
    };

    constexpr EnumItem kPivotPresetItems[] = {
        {v(PivotPreset::CENTER), "center", "Center", "PivotPreset::CENTER"},
        {v(PivotPreset::TOP_CENTER), "top_center", "Top Center", "PivotPreset::TOP_CENTER"},
        {v(PivotPreset::BOTTOM_CENTER), "bottom_center", "Bottom Center", "PivotPreset::BOTTOM_CENTER"},
        {v(PivotPreset::LEFT_CENTER), "left_center", "Left Center", "PivotPreset::LEFT_CENTER"},
        {v(PivotPreset::RIGHT_CENTER), "right_center", "Right Center", "PivotPreset::RIGHT_CENTER"},
        {v(PivotPreset::TOP_LEFT), "top_left", "Top Left", "PivotPreset::TOP_LEFT"},
        {v(PivotPreset::BOTTOM_LEFT), "bottom_left", "Bottom Left", "PivotPreset::BOTTOM_LEFT"},
        {v(PivotPreset::TOP_RIGHT), "top_right", "Top Right", "PivotPreset::TOP_RIGHT"},
        {v(PivotPreset::BOTTOM_RIGHT), "bottom_right", "Bottom Right", "PivotPreset::BOTTOM_RIGHT"},
    };

    constexpr EnumItem kPrimitiveTypeItems[] = {
        {v(PrimitiveType::TRIANGLES), "triangles", "Triangles", "PrimitiveType::TRIANGLES"},
        {v(PrimitiveType::TRIANGLE_STRIP), "triangle_strip", "Triangle Strip", "PrimitiveType::TRIANGLE_STRIP"},
        {v(PrimitiveType::POINTS), "points", "Points", "PrimitiveType::POINTS"},
        {v(PrimitiveType::LINES), "lines", "Lines", "PrimitiveType::LINES"},
    };

    constexpr EnumItem kProgressbarTypeItems[] = {
        {v(ProgressbarType::VERTICAL), "vertical", "Vertical", "ProgressbarType::VERTICAL"},
        {v(ProgressbarType::HORIZONTAL), "horizontal", "Horizontal", "ProgressbarType::HORIZONTAL"},
    };

    constexpr EnumItem kReflectionProbeModeItems[] = {
        {v(ReflectionProbeMode::STATIC), "static", "Static", "ReflectionProbeMode::STATIC"},
        {v(ReflectionProbeMode::DYNAMIC), "dynamic", "Dynamic", "ReflectionProbeMode::DYNAMIC"},
    };

    constexpr EnumItem kReflectionProbeUpdateModeItems[] = {
        {v(ReflectionProbeUpdateMode::ON_LOAD), "on_load", "On Load", "ReflectionProbeUpdateMode::ON_LOAD"},
        {v(ReflectionProbeUpdateMode::ON_MOVE), "on_move", "On Move", "ReflectionProbeUpdateMode::ON_MOVE"},
        {v(ReflectionProbeUpdateMode::INTERVAL), "interval", "Interval", "ReflectionProbeUpdateMode::INTERVAL"},
        {v(ReflectionProbeUpdateMode::MANUAL), "manual", "Manual", "ReflectionProbeUpdateMode::MANUAL"},
    };

    constexpr EnumItem kRigidbodyMotionQualityItems[] = {
        {v(RigidbodyMotionQuality::LINEAR_CAST), "linear_cast", "Linear Cast", "RigidbodyMotionQuality::LINEAR_CAST"},
        {v(RigidbodyMotionQuality::DISCRETE), "discrete", "Discrete", "RigidbodyMotionQuality::DISCRETE"},
    };
    constexpr int kRigidbodyMotionQualityOrder[] = {v(RigidbodyMotionQuality::DISCRETE), v(RigidbodyMotionQuality::LINEAR_CAST)};

    constexpr EnumItem kScriptTypeItems[] = {
        {v(ScriptType::CPP), "cpp", "C++", "ScriptType::CPP", nullptr, kScriptCppAliases},
        {v(ScriptType::LUA), "lua", "Lua", "ScriptType::LUA", nullptr, kScriptLuaAliases},
    };

    constexpr EnumItem kScrollbarTypeItems[] = {
        {v(ScrollbarType::VERTICAL), "vertical", "Vertical", "ScrollbarType::VERTICAL"},
        {v(ScrollbarType::HORIZONTAL), "horizontal", "Horizontal", "ScrollbarType::HORIZONTAL"},
    };

    constexpr EnumItem kShadowCaster2DShapeItems[] = {
        {v(ShadowCaster2DShape::AUTO_QUAD), "autoquad", "Auto Quad", "ShadowCaster2DShape::AUTO_QUAD", "auto_quad"},
        {v(ShadowCaster2DShape::POLYGON), "polygon", "Polygon", "ShadowCaster2DShape::POLYGON"},
    };

    constexpr EnumItem kShadowUpdateModeItems[] = {
        {v(ShadowUpdateMode::AUTO), "auto", "Auto", "ShadowUpdateMode::AUTO"},
        {v(ShadowUpdateMode::EVERY_FRAME), "everyFrame", "Every Frame", "ShadowUpdateMode::EVERY_FRAME", "every_frame"},
        {v(ShadowUpdateMode::ON_DEMAND), "onDemand", "On Demand", "ShadowUpdateMode::ON_DEMAND", "on_demand"},
    };

    constexpr EnumItem kTextureAlphaBorderItems[] = {
        {v(TextureAlphaBorder::FIX), "fix", "Fix", "TextureAlphaBorder::FIX"},
        {v(TextureAlphaBorder::KEEP), "keep", "Keep", "TextureAlphaBorder::KEEP"},
        {v(TextureAlphaBorder::AUTO), "auto", "Auto", "TextureAlphaBorder::AUTO"},
    };
    constexpr int kTextureAlphaBorderOrder[] = {v(TextureAlphaBorder::AUTO), v(TextureAlphaBorder::FIX), v(TextureAlphaBorder::KEEP)};

    constexpr EnumItem kTextureFilterItems[] = {
        {v(TextureFilter::NEAREST), "nearest", "Nearest", "TextureFilter::NEAREST"},
        {v(TextureFilter::LINEAR), "linear", "Linear", "TextureFilter::LINEAR"},
        {v(TextureFilter::NEAREST_MIPMAP_NEAREST), "nearest_mipmap_nearest", "Nearest Mipmap Nearest", "TextureFilter::NEAREST_MIPMAP_NEAREST"},
        {v(TextureFilter::NEAREST_MIPMAP_LINEAR), "nearest_mipmap_linear", "Nearest Mipmap Linear", "TextureFilter::NEAREST_MIPMAP_LINEAR"},
        {v(TextureFilter::LINEAR_MIPMAP_NEAREST), "linear_mipmap_nearest", "Linear Mipmap Nearest", "TextureFilter::LINEAR_MIPMAP_NEAREST"},
        {v(TextureFilter::LINEAR_MIPMAP_LINEAR), "linear_mipmap_linear", "Linear Mipmap Linear", "TextureFilter::LINEAR_MIPMAP_LINEAR"},
    };

    constexpr EnumItem kTextureWrapItems[] = {
        {v(TextureWrap::REPEAT), "repeat", "Repeat", "TextureWrap::REPEAT"},
        {v(TextureWrap::MIRRORED_REPEAT), "mirrored_repeat", "Mirrored Repeat", "TextureWrap::MIRRORED_REPEAT"},
        {v(TextureWrap::CLAMP_TO_EDGE), "clamp_to_edge", "Clamp to Edge", "TextureWrap::CLAMP_TO_EDGE"},
        {v(TextureWrap::CLAMP_TO_BORDER), "clamp_to_border", "Clamp to Border", "TextureWrap::CLAMP_TO_BORDER"},
    };

    constexpr EnumItem kUIDocumentRenderModeItems[] = {
        {v(UIDocumentRenderMode::TEXTURE), "texture", "Texture", "UIDocumentRenderMode::TEXTURE"},
        {v(UIDocumentRenderMode::SCREEN), "screen", "Screen", "UIDocumentRenderMode::SCREEN"},
    };
    constexpr int kUIDocumentRenderModeOrder[] = {v(UIDocumentRenderMode::SCREEN), v(UIDocumentRenderMode::TEXTURE)};

    constexpr EnumItem kUIEventStateItems[] = {
        {v(UIEventState::NOT_SET), "not_set", "Not Set", "UIEventState::NOT_SET"},
        {v(UIEventState::ENABLED), "true", "Enabled", "UIEventState::ENABLED"},
        {v(UIEventState::DISABLED), "false", "Disabled", "UIEventState::DISABLED"},
    };

    constexpr EnumItem kWindingOrderItems[] = {
        {v(WindingOrder::CCW), "ccw", "Counter-Clockwise", "WindingOrder::CCW"},
        {v(WindingOrder::CW), "cw", "Clockwise", "WindingOrder::CW"},
    };

}

namespace jasperengine::editor::reflect::enums {

    const EnumDesc kActionState{"ActionState", kActionStateItems, v(ActionState::Stopped)};
    const EnumDesc kAnchorPreset{"AnchorPreset", kAnchorPresetItems, v(AnchorPreset::NONE)};
    const EnumDesc kAtmosphereQualityOverride{"AtmosphereQualityOverride", kAtmosphereQualityOverrideItems, v(AtmosphereQualityOverride::PROJECT)};
    const EnumDesc kAudioSourceAttenuation{"AudioSourceAttenuation", kAudioSourceAttenuationItems, v(AudioSourceAttenuation::NO_ATTENUATION)};
    const EnumDesc kAudioSourceState{"AudioSourceState", kAudioSourceStateItems, v(AudioSourceState::Stopped)};
    const EnumDesc kBodyType{"BodyType", kBodyTypeItems, v(BodyType::STATIC)};
    const EnumDesc kCameraType{"CameraType", kCameraTypeItems, v(CameraType::CAMERA_PERSPECTIVE), nullptr, kCameraTypeOrder};
    const EnumDesc kCascadeSplitMode{"CascadeSplitMode", kCascadeSplitModeItems, v(CascadeSplitMode::AUTO)};
    const EnumDesc kCloudsPreset{"CloudsPreset", kCloudsPresetItems, v(CloudsPreset::CLOUDY)};
    const EnumDesc kCloudsRendering{"CloudsRendering", kCloudsRenderingItems, v(CloudsRendering::AUTO)};
    const EnumDesc kCollider2DType{"Collider2DType", kCollider2DTypeItems, v(Collider2DType::POLYGON)};
    const EnumDesc kCollider3DSource{"Collider3DSource", kCollider3DSourceItems, v(Collider3DSource::NONE)};
    const EnumDesc kCollider3DType{"Collider3DType", kCollider3DTypeItems, v(Collider3DType::SPHERE), nullptr, kCollider3DTypeOrder};
    const EnumDesc kCullingMode{"CullingMode", kCullingModeItems, v(CullingMode::BACK)};
    const EnumDesc kEaseType{"EaseType", kEaseTypeItems, v(EaseType::LINEAR), &normalizeEaseName, kEaseTypeOrder};
    const EnumDesc kFogColorMode{"FogColorMode", kFogColorModeItems, v(FogColorMode::COLOR)};
    const EnumDesc kFogType{"FogType", kFogTypeItems, v(FogType::LINEAR)};
    const EnumDesc kFogVolumeFalloff{"FogVolumeFalloff", kFogVolumeFalloffItems, v(FogVolumeFalloff::LINEAR)};
    const EnumDesc kFogVolumeShape{"FogVolumeShape", kFogVolumeShapeItems, v(FogVolumeShape::BOX)};
    const EnumDesc kJoint2DType{"Joint2DType", kJoint2DTypeItems, v(Joint2DType::DISTANCE)};
    const EnumDesc kJoint3DType{"Joint3DType", kJoint3DTypeItems, v(Joint3DType::FIXED)};
    const EnumDesc kLayoutGroupType{"LayoutGroupType", kLayoutGroupTypeItems, v(LayoutGroupType::VERTICAL)};
    const EnumDesc kForceFieldShape{"ForceFieldShapeType", kForceFieldShapeItems, v(ForceFieldShapeType::Sphere)};
    const EnumDesc kForceFieldKind{"ForceFieldKind", kForceFieldKindItems, v(ForceFieldKind::Directional)};
    const EnumDesc kWaterVolumeWaves{"WaterVolumeWaves", kWaterVolumeWavesItems, v(WaterVolumeWaves::FROM_MATERIAL)};
    const EnumDesc kBuoyancyColumns{"BuoyancyColumns", kBuoyancyColumnsItems, v(BuoyancyColumns::AUTO_2x2)};
    const EnumDesc kLineRendererMode{"LineRendererMode", kLineRendererModeItems, v(LineRendererMode::Segments)};
    const EnumDesc kLineWidthUnit{"LineWidthUnit", kLineWidthUnitItems, v(LineWidthUnit::World)};
    const EnumDesc kLineAlignment{"LineAlignment", kLineAlignmentItems, v(LineAlignment::View)};
    const EnumDesc kLineTextureMode{"LineTextureMode", kLineTextureModeItems, v(LineTextureMode::Stretch)};
    const EnumDesc kCanvasGroupRenderMode{"CanvasGroupRenderMode", kCanvasGroupRenderModeItems, v(CanvasGroupRenderMode::PER_ELEMENT)};
    const EnumDesc kOnScreenStickMode{"OnScreenStickMode", kOnScreenStickModeItems, v(OnScreenStickMode::FIXED)};
    const EnumDesc kOnScreenStickAxes{"OnScreenStickAxes", kOnScreenStickAxesItems, v(OnScreenStickAxes::BOTH)};
    const EnumDesc kOnScreenVisibility{"OnScreenVisibility", kOnScreenVisibilityItems, v(OnScreenVisibility::ALWAYS)};
    const EnumDesc kJointAnchorSpace{"JointAnchorSpace", kJointAnchorSpaceItems, v(JointAnchorSpace::WORLD)};
    const EnumDesc kCollisionDetection2D{"CollisionDetectionMode2D", kCollisionDetection2DItems, v(CollisionDetectionMode2D::DISCRETE)};
    const EnumDesc kSleepMode2D{"RigidbodySleepMode2D", kSleepMode2DItems, v(RigidbodySleepMode2D::START_AWAKE)};
    const EnumDesc kRigidbodyInterpolation{"RigidbodyInterpolation", kRigidbodyInterpolationItems, v(RigidbodyInterpolation::SCENE)};
    const EnumDesc kJointBreakAction{"JointBreakAction", kJointBreakActionItems, v(JointBreakAction::DESTROY)};
    const EnumDesc kJointMotion{"JointMotion", kJointMotionItems, v(JointMotion::FREE)};
    const EnumDesc kJointSwingType{"JointSwingType", kJointSwingTypeItems, v(JointSwingType::CONE)};
    const EnumDesc kAlphaTweenTarget{"AlphaTweenTarget", kAlphaTweenTargetItems, v(AlphaTweenTarget::AUTO)};
    const EnumDesc kClipWrapMode{"ClipWrapMode", kClipWrapModeItems, v(ClipWrapMode::Default)};
    const EnumDesc kAnimationLayerBlend{"AnimationLayerBlend", kAnimationLayerBlendItems, v(AnimationLayerBlend::Override)};
    const EnumDesc kAnimatorUpdateMode{"AnimatorUpdateMode", kAnimatorUpdateModeItems, v(AnimatorUpdateMode::Normal)};
    const EnumDesc kAnimatorCullingMode{"AnimatorCullingMode", kAnimatorCullingModeItems, v(AnimatorCullingMode::AlwaysAnimate)};
    const EnumDesc kAnimatorWriteBones{"AnimatorWriteBones", kAnimatorWriteBonesItems, v(AnimatorWriteBones::All)};
    const EnumDesc kAnimatorEventMode{"AnimatorEventMode", kAnimatorEventModeItems, v(AnimatorEventMode::LeaderOnly)};
    const EnumDesc kAnimatorRetarget{"AnimatorRetarget", kAnimatorRetargetItems, v(AnimatorRetarget::Auto)};
    const EnumDesc kAnimatorIKType{"AnimatorIKType", kAnimatorIKTypeItems, v(AnimatorIKType::TwoBone)};
    const EnumDesc kFluidOutput{"FluidOutput", kFluidOutputItems, v(FluidOutput::MATERIAL), nullptr, kFluidOutputOrder};
    const EnumDesc kLatticeInterpolation{"LatticeInterpolation", kLatticeInterpolationItems, v(LatticeInterpolation::CUBIC)};
    const EnumDesc kLatticeOutside{"LatticeOutside", kLatticeOutsideItems, v(LatticeOutside::FADE)};
    const EnumDesc kLatticeJiggleAnchor{"LatticeJiggleAnchor", kLatticeJiggleAnchorItems, v(LatticeJiggleAnchor::NONE), nullptr, kLatticeJiggleAnchorOrder};
    const EnumDesc kLatticeStage{"LatticeStage", kLatticeStageItems, v(LatticeStage::AFTER_SKINNING)};
    const EnumDesc kLatticeMaskChannel{"LatticeMaskChannel", kLatticeMaskChannelItems, v(LatticeMaskChannel::NONE), nullptr, kLatticeMaskChannelOrder};
    const EnumDesc kLightUnit{"LightUnit", kLightUnitItems, v(LightUnit::ENGINE)};
    const EnumDesc kLightShading{"LightShading", kLightShadingItems, v(LightShading::BOTH)};
    const EnumDesc kLightType{"LightType", kLightTypeItems, v(LightType::DIRECTIONAL)};
    const EnumDesc kLodFadeMode{"LodFadeMode", kLodFadeModeItems, v(LodFadeMode::CROSS_FADE)};
    const EnumDesc kMaterialAlphaMode{"MaterialAlphaMode", kMaterialAlphaModeItems, v(MaterialAlphaMode::AUTO)};
    const EnumDesc kMaterialBlendMode{"MaterialBlendMode", kMaterialBlendModeItems, v(MaterialBlendMode::AUTO)};
    const EnumDesc kMaterialCullMode{"MaterialCullMode", kMaterialCullModeItems, v(MaterialCullMode::AUTO)};
    const EnumDesc kMaterialDepthTest{"MaterialDepthTest", kMaterialDepthTestItems, v(MaterialDepthTest::AUTO)};
    const EnumDesc kMaterialDepthWrite{"MaterialDepthWrite", kMaterialDepthWriteItems, v(MaterialDepthWrite::AUTO)};
    const EnumDesc kMaterialLighting{"MaterialLighting", kMaterialLightingItems, v(MaterialLighting::AUTO)};
    const EnumDesc kMaterialPassOrder{"MaterialPassOrder", kMaterialPassOrderItems, v(MaterialPassOrder::AFTER), nullptr, kMaterialPassOrderOrder};
    const EnumDesc kMaterialStencilCompare{"MaterialStencilCompare", kMaterialStencilCompareItems, v(MaterialStencilCompare::AUTO)};
    const EnumDesc kMaterialStencilOp{"MaterialStencilOp", kMaterialStencilOpItems, v(MaterialStencilOp::KEEP)};
    const EnumDesc kParticleEmitterShape{"ParticleEmitterShape", kParticleEmitterShapeItems, v(ParticleEmitterShape::Box)};
    const EnumDesc kPivotPreset{"PivotPreset", kPivotPresetItems, v(PivotPreset::BOTTOM_LEFT)};
    const EnumDesc kPrimitiveType{"PrimitiveType", kPrimitiveTypeItems, v(PrimitiveType::TRIANGLES)};
    const EnumDesc kProgressbarType{"ProgressbarType", kProgressbarTypeItems, v(ProgressbarType::HORIZONTAL)};
    const EnumDesc kReflectionProbeMode{"ReflectionProbeMode", kReflectionProbeModeItems, v(ReflectionProbeMode::STATIC)};
    const EnumDesc kReflectionProbeUpdateMode{"ReflectionProbeUpdateMode", kReflectionProbeUpdateModeItems, v(ReflectionProbeUpdateMode::ON_LOAD)};
    const EnumDesc kRigidbodyMotionQuality{"RigidbodyMotionQuality", kRigidbodyMotionQualityItems, v(RigidbodyMotionQuality::DISCRETE), nullptr,
                                           kRigidbodyMotionQualityOrder};
    const EnumDesc kScriptType{"ScriptType", kScriptTypeItems, v(ScriptType::CPP)};
    const EnumDesc kScrollbarType{"ScrollbarType", kScrollbarTypeItems, v(ScrollbarType::VERTICAL)};
    const EnumDesc kShadowCaster2DShape{"ShadowCaster2DShape", kShadowCaster2DShapeItems, v(ShadowCaster2DShape::AUTO_QUAD)};
    const EnumDesc kShadowUpdateMode{"ShadowUpdateMode", kShadowUpdateModeItems, v(ShadowUpdateMode::AUTO)};
    const EnumDesc kTextureAlphaBorder{"TextureAlphaBorder", kTextureAlphaBorderItems, v(TextureAlphaBorder::AUTO), nullptr, kTextureAlphaBorderOrder};
    const EnumDesc kTextureFilter{"TextureFilter", kTextureFilterItems, v(TextureFilter::LINEAR)};
    const EnumDesc kTextureWrap{"TextureWrap", kTextureWrapItems, v(TextureWrap::REPEAT)};
    const EnumDesc kUIDocumentRenderMode{"UIDocumentRenderMode", kUIDocumentRenderModeItems, v(UIDocumentRenderMode::SCREEN), nullptr,
                                         kUIDocumentRenderModeOrder};
    const EnumDesc kUIEventState{"UIEventState", kUIEventStateItems, v(UIEventState::NOT_SET)};
    const EnumDesc kWindingOrder{"WindingOrder", kWindingOrderItems, v(WindingOrder::CCW)};

    std::vector<const EnumItem*> offered(const EnumDesc& desc, uint64_t mask) {
        std::vector<const EnumItem*> items;
        auto add = [&](const EnumItem* item) {
            if (!item || item->hidden) return;
            if (mask && !(item->value >= 0 && item->value < 64 && (mask & bit(item->value)))) return;
            items.push_back(item);
        };
        if (desc.uiOrder.empty()) {
            for (const EnumItem& item : desc.items) add(&item);
        } else {
            for (int value : desc.uiOrder) add(enumItem(desc, value));
        }
        return items;
    }

}
