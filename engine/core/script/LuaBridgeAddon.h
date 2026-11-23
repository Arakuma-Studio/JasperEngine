// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LuaBridgeAddon_h
#define LuaBridgeAddon_h

#include "lua.hpp"
#include "LuaBridge.h"
#include "Scene.h"
#include "util/CanvasScaler.h"
#include "util/TextEffects.h"
#include "Input.h"
#include "input/InputActions.h"
#include "component/CharacterCollision.h"
#include "component/TerrainComponent.h"
#include "component/WaterVolumeComponent.h"
#include "component/WaterExcluderComponent.h"
#include "component/BuoyancyComponent.h"
#include "component/SplineAnimateComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/SplineInstantiateComponent.h"
#include "spline/SplineTypes.h"
#include "FileData.h"
#include "thread/ResourceProgress.h"
#include "jobs/JobTypes.h"
#include "texture/Material.h"
#include "component/UIDocumentComponent.h"
#include "component/FluidSimulationComponent.h"
#include "particles/LegacyParticleSystem.h"
#include "component/FogComponent.h"
#include "component/SpriteBatchComponent.h"
#include "component/SpriteComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "render/shadow/ShadowTypes.h"
#include "render/LodFade.h"
#include "render/shadow/ShadowTiers.h"
#include "render/LightingTiers.h"
#include "render/OrderIndependentTransparency.h"
#include "component/CapsuleShadowCasterComponent.h"
#include "component/UISelectable.h"
#include "texture/MaterialPbr.h"
#include "device/DeviceTypes.h"
#include "debug/DebugOverlay.h"
#include "Log.h"
#include "util/StringUtils.h"
#include "component/LineRendererComponent.h"
#include "component/VideoPlayerComponent.h"
#include "camera/CameraType.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/CameraComponent.h"
#include "component/LightComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/ActionComponent.h"
#include "component/AlphaTweenComponent.h"
#include "component/CanvasGroupComponent.h"
#include "component/Joint2DComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/SpriteComponent.h"
#include "component/Joint3DComponent.h"
#include "component/ReflectionProbeComponent.h"

using namespace jasperengine;

namespace luabridge
{

    // need to specialized luabridge::Stack and use enum in arguments
    template <class T>
    struct EnumWrapper
    {
        static auto push(lua_State* L, T value) -> std::enable_if_t<std::is_enum_v<T>, luabridge::Result>
        {
            // signed: SleepTimeout's values are negative
            lua_pushinteger(L, static_cast<lua_Integer>(value));
            return {};
        }

        static auto get(lua_State* L, int index) -> std::enable_if_t<std::is_enum_v<T>, luabridge::TypeResult<T>>
        {
            return static_cast<T>(lua_tointeger(L, index));
        }

        static auto isInstance(lua_State* L, int index) -> std::enable_if_t<std::is_enum_v<T>, bool>
        {
            if (lua_type(L, index) == LUA_TNUMBER)
                return luabridge::is_integral_representable_by<std::size_t>(L, index);

            return false;
        }
    };

    // Asset path fields (AssetRef, ShaderRef) are plain path strings in Lua. Assigning one
    // gives a reference without a GUID, as assigning a path does in C++.
    template <class T>
    struct AssetPathStack
    {
        [[nodiscard]] static Result push(lua_State* L, const T& ref)
        {
            return Stack<std::string>::push(L, ref.path);
        }

        [[nodiscard]] static TypeResult<T> get(lua_State* L, int index)
        {
            auto result = Stack<std::string>::get(L, index);
            if (! result)
                return result.error();
            return T(std::move(*result));
        }

        [[nodiscard]] static bool isInstance(lua_State* L, int index)
        {
            return Stack<std::string>::isInstance(L, index);
        }
    };

    template<> struct Stack<jasperengine::AssetRef> : AssetPathStack<jasperengine::AssetRef>{};
    template<> struct Stack<jasperengine::ShaderRef> : AssetPathStack<jasperengine::ShaderRef>{};

    // A character (Engine.onCharInput, UISystem:eventOnCharInput): a one-character UTF-8 string
    // in Lua, as the event handlers get it; a code point number is taken too
    template<> struct Stack<char32_t>
    {
        [[nodiscard]] static Result push(lua_State* L, char32_t value)
        {
            return Stack<std::string>::push(L, StringUtils::toUTF8(value));
        }

        [[nodiscard]] static TypeResult<char32_t> get(lua_State* L, int index)
        {
            if (lua_type(L, index) == LUA_TNUMBER)
                return static_cast<char32_t>(lua_tointeger(L, index));
            if (lua_type(L, index) != LUA_TSTRING)
                return makeErrorCode(ErrorCode::InvalidTypeCast);
            size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            bool invalid = false;
            const std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints(std::string(text, length), invalid);
            if (codepoints.size() != 1)
                return makeErrorCode(ErrorCode::InvalidTypeCast);
            return static_cast<char32_t>(codepoints[0]);
        }

        [[nodiscard]] static bool isInstance(lua_State* L, int index)
        {
            return lua_type(L, index) == LUA_TNUMBER || lua_type(L, index) == LUA_TSTRING;
        }
    };

    // Qualified: a source including this may see same-named types of other namespaces (the editor's
    // Platform and SelectionState, particles::CullingMode), and in a unity build so may every source after it
    template<> struct Stack<jasperengine::Scaling> : EnumWrapper<jasperengine::Scaling>{};
    template<> struct Stack<jasperengine::Platform> : EnumWrapper<jasperengine::Platform>{};
    template<> struct Stack<jasperengine::GraphicBackend> : EnumWrapper<jasperengine::GraphicBackend>{};
    template<> struct Stack<jasperengine::TextureStrategy> : EnumWrapper<jasperengine::TextureStrategy>{};
    template<> struct Stack<jasperengine::TextureType> : EnumWrapper<jasperengine::TextureType>{};
    template<> struct Stack<jasperengine::ColorFormat> : EnumWrapper<jasperengine::ColorFormat>{};
    template<> struct Stack<jasperengine::PrimitiveType> : EnumWrapper<jasperengine::PrimitiveType>{};
    template<> struct Stack<jasperengine::CullingMode> : EnumWrapper<jasperengine::CullingMode>{};
    template<> struct Stack<jasperengine::WindingOrder> : EnumWrapper<jasperengine::WindingOrder>{};
    template<> struct Stack<jasperengine::LodFadeMode> : EnumWrapper<jasperengine::LodFadeMode>{};
    template<> struct Stack<jasperengine::MaterialAlphaMode> : EnumWrapper<jasperengine::MaterialAlphaMode>{};
    template<> struct Stack<jasperengine::MaterialLighting> : EnumWrapper<jasperengine::MaterialLighting>{};
    template<> struct Stack<jasperengine::TerrainQualitySetting> : EnumWrapper<jasperengine::TerrainQualitySetting>{};
    template<> struct Stack<jasperengine::TerrainOffscreenShadows> : EnumWrapper<jasperengine::TerrainOffscreenShadows>{};
    template<> struct Stack<jasperengine::TerrainMacroMode> : EnumWrapper<jasperengine::TerrainMacroMode>{};
    template<> struct Stack<jasperengine::TerrainCliffProjection> : EnumWrapper<jasperengine::TerrainCliffProjection>{};
    template<> struct Stack<jasperengine::TextureFilter> : EnumWrapper<jasperengine::TextureFilter>{};
    template<> struct Stack<jasperengine::TextureWrap> : EnumWrapper<jasperengine::TextureWrap>{};
    template<> struct Stack<jasperengine::CursorType> : EnumWrapper<jasperengine::CursorType>{};
    template<> struct Stack<jasperengine::MouseMode> : EnumWrapper<jasperengine::MouseMode>{};
    template<> struct Stack<jasperengine::TouchPhase> : EnumWrapper<jasperengine::TouchPhase>{};
    template<> struct Stack<jasperengine::PointerLockState> : EnumWrapper<jasperengine::PointerLockState>{};
    template<> struct Stack<jasperengine::InputActionPhase> : EnumWrapper<jasperengine::InputActionPhase>{};
    template<> struct Stack<jasperengine::InputActionType> : EnumWrapper<jasperengine::InputActionType>{};
    template<> struct Stack<jasperengine::CharacterCollisionFlags> : EnumWrapper<jasperengine::CharacterCollisionFlags>{};
    template<> struct Stack<jasperengine::WaterVolumeWaves> : EnumWrapper<jasperengine::WaterVolumeWaves>{};
    template<> struct Stack<jasperengine::WaterExcluderShape> : EnumWrapper<jasperengine::WaterExcluderShape>{};
    template<> struct Stack<jasperengine::WaterWakeResolution> : EnumWrapper<jasperengine::WaterWakeResolution>{};
    template<> struct Stack<jasperengine::BuoyancyColumns> : EnumWrapper<jasperengine::BuoyancyColumns>{};
    template<> struct Stack<jasperengine::spline::SplineType> : EnumWrapper<jasperengine::spline::SplineType>{};
    template<> struct Stack<jasperengine::spline::TangentMode> : EnumWrapper<jasperengine::spline::TangentMode>{};
    template<> struct Stack<jasperengine::SplineExtrudeShape> : EnumWrapper<jasperengine::SplineExtrudeShape>{};
    template<> struct Stack<jasperengine::SplineWidthSource> : EnumWrapper<jasperengine::SplineWidthSource>{};
    template<> struct Stack<jasperengine::SplineCornerStyle> : EnumWrapper<jasperengine::SplineCornerStyle>{};
    template<> struct Stack<jasperengine::SplineGrade> : EnumWrapper<jasperengine::SplineGrade>{};
    template<> struct Stack<jasperengine::SplineConform> : EnumWrapper<jasperengine::SplineConform>{};
    template<> struct Stack<jasperengine::SplineCollision> : EnumWrapper<jasperengine::SplineCollision>{};
    template<> struct Stack<jasperengine::SplineInstantiateMode> : EnumWrapper<jasperengine::SplineInstantiateMode>{};
    template<> struct Stack<jasperengine::SplineSpacingFit> : EnumWrapper<jasperengine::SplineSpacingFit>{};
    template<> struct Stack<jasperengine::SplinePlantMode> : EnumWrapper<jasperengine::SplinePlantMode>{};
    template<> struct Stack<jasperengine::SplineAnimateMethod> : EnumWrapper<jasperengine::SplineAnimateMethod>{};
    template<> struct Stack<jasperengine::SplineLoop> : EnumWrapper<jasperengine::SplineLoop>{};
    template<> struct Stack<jasperengine::SplineEasing> : EnumWrapper<jasperengine::SplineEasing>{};
    template<> struct Stack<jasperengine::SplineAlignment> : EnumWrapper<jasperengine::SplineAlignment>{};
    template<> struct Stack<jasperengine::ResourceLoadState> : EnumWrapper<jasperengine::ResourceLoadState>{};
    template<> struct Stack<jasperengine::LightState> : EnumWrapper<jasperengine::LightState>{};
    template<> struct Stack<jasperengine::UIEventState> : EnumWrapper<jasperengine::UIEventState>{};
    // platform services (System, Screen, Input.deviceOrientation)
    template<> struct Stack<jasperengine::SystemLanguage> : EnumWrapper<jasperengine::SystemLanguage>{};
    template<> struct Stack<jasperengine::ScreenOrientation> : EnumWrapper<jasperengine::ScreenOrientation>{};
    template<> struct Stack<jasperengine::DeviceOrientation> : EnumWrapper<jasperengine::DeviceOrientation>{};
    template<> struct Stack<jasperengine::DeviceType> : EnumWrapper<jasperengine::DeviceType>{};
    template<> struct Stack<jasperengine::DeviceTier> : EnumWrapper<jasperengine::DeviceTier>{};
    template<> struct Stack<jasperengine::ThermalState> : EnumWrapper<jasperengine::ThermalState>{};
    template<> struct Stack<jasperengine::NetworkReachability> : EnumWrapper<jasperengine::NetworkReachability>{};
    template<> struct Stack<jasperengine::HapticFeedback> : EnumWrapper<jasperengine::HapticFeedback>{};
    template<> struct Stack<jasperengine::SleepTimeout> : EnumWrapper<jasperengine::SleepTimeout>{};

    template<> struct Stack<jasperengine::EntityPool> : EnumWrapper<jasperengine::EntityPool>{};

    template<> struct Stack<jasperengine::ResourceType> : EnumWrapper<jasperengine::ResourceType>{};
    template<> struct Stack<jasperengine::jobs::Lane> : EnumWrapper<jasperengine::jobs::Lane>{};

    template<> struct Stack<jasperengine::RotationOrder> : EnumWrapper<jasperengine::RotationOrder>{};

    template<> struct Stack<jasperengine::FogType> : EnumWrapper<jasperengine::FogType>{};
    template<> struct Stack<jasperengine::FogColorMode> : EnumWrapper<jasperengine::FogColorMode>{};
    template<> struct Stack<jasperengine::FogVolumeShape> : EnumWrapper<jasperengine::FogVolumeShape>{};
    template<> struct Stack<jasperengine::FogVolumeFalloff> : EnumWrapper<jasperengine::FogVolumeFalloff>{};
    template<> struct Stack<jasperengine::CloudsPreset> : EnumWrapper<jasperengine::CloudsPreset>{};
    template<> struct Stack<jasperengine::CloudsRendering> : EnumWrapper<jasperengine::CloudsRendering>{};
    template<> struct Stack<jasperengine::AtmosphereQuality> : EnumWrapper<jasperengine::AtmosphereQuality>{};
    template<> struct Stack<jasperengine::AtmosphereQualityOverride> : EnumWrapper<jasperengine::AtmosphereQualityOverride>{};
    template<> struct Stack<jasperengine::CameraType> : EnumWrapper<jasperengine::CameraType>{};
    template<> struct Stack<jasperengine::PlayMode> : EnumWrapper<jasperengine::PlayMode>{};
    template<> struct Stack<jasperengine::FrustumPlane> : EnumWrapper<jasperengine::FrustumPlane>{};
    template<> struct Stack<jasperengine::LightType> : EnumWrapper<jasperengine::LightType>{};
    template<> struct Stack<jasperengine::LightShading> : EnumWrapper<jasperengine::LightShading>{};
    template<> struct Stack<jasperengine::ShadowCaster2DShape> : EnumWrapper<jasperengine::ShadowCaster2DShape>{};
    template<> struct Stack<jasperengine::ShadowQuality> : EnumWrapper<jasperengine::ShadowQuality>{};
    // 3D shadows (S10)
    template<> struct Stack<jasperengine::ShadowType> : EnumWrapper<jasperengine::ShadowType>{};
    template<> struct Stack<jasperengine::ShadowUpdateMode> : EnumWrapper<jasperengine::ShadowUpdateMode>{};
    // S57's lighting
    template<> struct Stack<jasperengine::TransparencyMode> : EnumWrapper<jasperengine::TransparencyMode>{};
    template<> struct Stack<jasperengine::ShadingModel> : EnumWrapper<jasperengine::ShadingModel>{};
    template<> struct Stack<jasperengine::CapsuleCastShadows> : EnumWrapper<jasperengine::CapsuleCastShadows>{};
    template<> struct Stack<jasperengine::CapsuleDirection> : EnumWrapper<jasperengine::CapsuleDirection>{};
    template<> struct Stack<jasperengine::LightMode> : EnumWrapper<jasperengine::LightMode>{};
    template<> struct Stack<jasperengine::CascadeSplitMode> : EnumWrapper<jasperengine::CascadeSplitMode>{};
    template<> struct Stack<jasperengine::ShadowTier> : EnumWrapper<jasperengine::ShadowTier>{};
    template<> struct Stack<jasperengine::SceneColorScale> : EnumWrapper<jasperengine::SceneColorScale>{};
    template<> struct Stack<jasperengine::UIRenderMode> : EnumWrapper<jasperengine::UIRenderMode>{};
    template<> struct Stack<jasperengine::PlanarReflectionResolution> : EnumWrapper<jasperengine::PlanarReflectionResolution>{};
    template<> struct Stack<jasperengine::LitQuality> : EnumWrapper<jasperengine::LitQuality>{};
    template<> struct Stack<jasperengine::LightUnit> : EnumWrapper<jasperengine::LightUnit>{};
    template<> struct Stack<jasperengine::TonemapMode> : EnumWrapper<jasperengine::TonemapMode>{};
    template<> struct Stack<jasperengine::MonochromeMode> : EnumWrapper<jasperengine::MonochromeMode>{};
    template<> struct Stack<jasperengine::FilmGrainType> : EnumWrapper<jasperengine::FilmGrainType>{};
    template<> struct Stack<jasperengine::FilmGateMask> : EnumWrapper<jasperengine::FilmGateMask>{};
    template<> struct Stack<jasperengine::PaletteDither> : EnumWrapper<jasperengine::PaletteDither>{};
    template<> struct Stack<jasperengine::PaletteDistance> : EnumWrapper<jasperengine::PaletteDistance>{};
    template<> struct Stack<jasperengine::PalettePrecision> : EnumWrapper<jasperengine::PalettePrecision>{};
    template<> struct Stack<jasperengine::LutInputEncoding> : EnumWrapper<jasperengine::LutInputEncoding>{};
    template<> struct Stack<jasperengine::AudioSourceState> : EnumWrapper<jasperengine::AudioSourceState>{};
    template<> struct Stack<jasperengine::AudioSourceAttenuation> : EnumWrapper<jasperengine::AudioSourceAttenuation>{};
    template<> struct Stack<jasperengine::AudioLoadType> : EnumWrapper<jasperengine::AudioLoadType>{};
    template<> struct Stack<jasperengine::AudioPlayOn> : EnumWrapper<jasperengine::AudioPlayOn>{};

    template<> struct Stack<jasperengine::EaseType> : EnumWrapper<jasperengine::EaseType>{};
    template<> struct Stack<jasperengine::ActionState> : EnumWrapper<jasperengine::ActionState>{};

    template<> struct Stack<jasperengine::FileErrors> : EnumWrapper<jasperengine::FileErrors>{};

    template<> struct Stack<jasperengine::AnchorPreset> : EnumWrapper<jasperengine::AnchorPreset>{};
    template<> struct Stack<jasperengine::LayoutGroupType> : EnumWrapper<jasperengine::LayoutGroupType>{};
    // Canvas Groups (ui canvas group spec 7.6)
    template<> struct Stack<jasperengine::CanvasGroupRenderMode> : EnumWrapper<jasperengine::CanvasGroupRenderMode>{};
    template<> struct Stack<jasperengine::AlphaTweenTarget> : EnumWrapper<jasperengine::AlphaTweenTarget>{};
    // ui-scaling-text: the Canvas Scaler and Text Effect enums
    template<> struct Stack<jasperengine::UIScaleMode> : EnumWrapper<jasperengine::UIScaleMode>{};
    template<> struct Stack<jasperengine::ScreenMatchMode> : EnumWrapper<jasperengine::ScreenMatchMode>{};
    template<> struct Stack<jasperengine::PhysicalUnit> : EnumWrapper<jasperengine::PhysicalUnit>{};
    template<> struct Stack<jasperengine::TextEffectType> : EnumWrapper<jasperengine::TextEffectType>{};
    template<> struct Stack<jasperengine::TextEffectMix> : EnumWrapper<jasperengine::TextEffectMix>{};
    template<> struct Stack<jasperengine::PivotPreset> : EnumWrapper<jasperengine::PivotPreset>{};
    template<> struct Stack<jasperengine::SpriteDrawMode> : EnumWrapper<jasperengine::SpriteDrawMode>{};
    template<> struct Stack<jasperengine::PixelUpscaleMode> : EnumWrapper<jasperengine::PixelUpscaleMode>{};
    template<> struct Stack<jasperengine::PixelRotation> : EnumWrapper<jasperengine::PixelRotation>{};
    // Sprite Batch and Sprite Sort Point (S52)
    template<> struct Stack<jasperengine::SpriteBatchMode> : EnumWrapper<jasperengine::SpriteBatchMode>{};
    template<> struct Stack<jasperengine::SpriteBatchSpace> : EnumWrapper<jasperengine::SpriteBatchSpace>{};
    template<> struct Stack<jasperengine::SpriteSortPoint> : EnumWrapper<jasperengine::SpriteSortPoint>{};

    template<> struct Stack<jasperengine::ScrollbarType> : EnumWrapper<jasperengine::ScrollbarType>{};
    // ui-components: the Button's Selectable enums
    template<> struct Stack<jasperengine::ButtonTransition> : EnumWrapper<jasperengine::ButtonTransition>{};
    template<> struct Stack<jasperengine::NavigationMode> : EnumWrapper<jasperengine::NavigationMode>{};
    template<> struct Stack<jasperengine::SelectionState> : EnumWrapper<jasperengine::SelectionState>{};
    template<> struct Stack<jasperengine::MoveDirection> : EnumWrapper<jasperengine::MoveDirection>{};
    template<> struct Stack<jasperengine::ProgressbarType> : EnumWrapper<jasperengine::ProgressbarType>{};

    template<> struct Stack<jasperengine::Collider2DType> : EnumWrapper<jasperengine::Collider2DType>{};
    template<> struct Stack<jasperengine::Collider3DType> : EnumWrapper<jasperengine::Collider3DType>{};
    template<> struct Stack<jasperengine::BodyType> : EnumWrapper<jasperengine::BodyType>{};
    template<> struct Stack<jasperengine::Joint2DType> : EnumWrapper<jasperengine::Joint2DType>{};
    template<> struct Stack<jasperengine::JointLimitState2D> : EnumWrapper<jasperengine::JointLimitState2D>{};
    template<> struct Stack<jasperengine::CollisionDetectionMode2D> : EnumWrapper<jasperengine::CollisionDetectionMode2D>{};
    template<> struct Stack<jasperengine::RigidbodySleepMode2D> : EnumWrapper<jasperengine::RigidbodySleepMode2D>{};
    template<> struct Stack<jasperengine::JointAnchorSpace> : EnumWrapper<jasperengine::JointAnchorSpace>{};
    template<> struct Stack<jasperengine::JointBreakAction> : EnumWrapper<jasperengine::JointBreakAction>{};
    template<> struct Stack<jasperengine::PhysicsMaterialCombine> : EnumWrapper<jasperengine::PhysicsMaterialCombine>{};
    template<> struct Stack<jasperengine::UIDocumentRenderMode> : EnumWrapper<jasperengine::UIDocumentRenderMode>{};
    template<> struct Stack<jasperengine::FluidOutput> : EnumWrapper<jasperengine::FluidOutput>{};
    template<> struct Stack<jasperengine::VideoPlayerSource> : EnumWrapper<jasperengine::VideoPlayerSource>{};
    template<> struct Stack<jasperengine::VideoTimeUpdateMode> : EnumWrapper<jasperengine::VideoTimeUpdateMode>{};
    template<> struct Stack<jasperengine::VideoRenderMode> : EnumWrapper<jasperengine::VideoRenderMode>{};
    template<> struct Stack<jasperengine::VideoAspectRatio> : EnumWrapper<jasperengine::VideoAspectRatio>{};
    template<> struct Stack<jasperengine::VideoAudioOutputMode> : EnumWrapper<jasperengine::VideoAudioOutputMode>{};
    // bound enums that had none: LuaBridge pushed them as unregistered userdata (an error
    // reading probe.mode) and took no integer for them
    template<> struct Stack<jasperengine::ReflectionProbeMode> : EnumWrapper<jasperengine::ReflectionProbeMode>{};
    template<> struct Stack<jasperengine::ReflectionProbeUpdateMode> : EnumWrapper<jasperengine::ReflectionProbeUpdateMode>{};
    template<> struct Stack<jasperengine::ParticleEmitterShape> : EnumWrapper<jasperengine::ParticleEmitterShape>{};
    template<> struct Stack<jasperengine::RigidbodyMotionQuality> : EnumWrapper<jasperengine::RigidbodyMotionQuality>{};
    template<> struct Stack<jasperengine::Joint3DType> : EnumWrapper<jasperengine::Joint3DType>{};
    template<> struct Stack<jasperengine::ShaderType> : EnumWrapper<jasperengine::ShaderType>{};
    template<> struct Stack<jasperengine::TextureAlphaBorder> : EnumWrapper<jasperengine::TextureAlphaBorder>{};

    // shader library render state (Material, UI/points/lines)
    template<> struct Stack<jasperengine::MaterialBlendMode> : EnumWrapper<jasperengine::MaterialBlendMode>{};
    template<> struct Stack<jasperengine::MaterialCullMode> : EnumWrapper<jasperengine::MaterialCullMode>{};
    template<> struct Stack<jasperengine::MaterialDepthWrite> : EnumWrapper<jasperengine::MaterialDepthWrite>{};
    template<> struct Stack<jasperengine::MaterialDepthTest> : EnumWrapper<jasperengine::MaterialDepthTest>{};
    // phase 2: the extra pass's order
    template<> struct Stack<jasperengine::MaterialPassOrder> : EnumWrapper<jasperengine::MaterialPassOrder>{};
    template<> struct Stack<jasperengine::MaterialStencilCompare> : EnumWrapper<jasperengine::MaterialStencilCompare>{};
    template<> struct Stack<jasperengine::MaterialStencilOp> : EnumWrapper<jasperengine::MaterialStencilOp>{};
    template<> struct Stack<jasperengine::MaterialColorWriteBits> : EnumWrapper<jasperengine::MaterialColorWriteBits>{};
    // Line and Trail Renderers (lines and trails spec 6.6)
    template<> struct Stack<jasperengine::LineRendererMode> : EnumWrapper<jasperengine::LineRendererMode>{};
    template<> struct Stack<jasperengine::LineWidthUnit> : EnumWrapper<jasperengine::LineWidthUnit>{};
    template<> struct Stack<jasperengine::LineAlignment> : EnumWrapper<jasperengine::LineAlignment>{};
    template<> struct Stack<jasperengine::LineTextureMode> : EnumWrapper<jasperengine::LineTextureMode>{};

    template <>
    struct Stack <Touch>
    {
        static Result push(lua_State* L, Touch touch)
        {
            lua_newtable(L);

            lua_pushinteger(L, touch.pointer);
            lua_setfield(L, -2, "pointer");

            if (!luabridge::push(L, touch.position))
                return makeErrorCode(ErrorCode::LuaStackOverflow);
            lua_setfield(L, -2, "position");

            // Input.TOUCH_BEGAN .. TOUCH_CANCELED
            lua_pushinteger(L, static_cast<int>(touch.phase));
            lua_setfield(L, -2, "phase");

            if (!luabridge::push(L, touch.delta))
                return makeErrorCode(ErrorCode::LuaStackOverflow);
            lua_setfield(L, -2, "delta");

            if (!luabridge::push(L, touch.startPosition))
                return makeErrorCode(ErrorCode::LuaStackOverflow);
            lua_setfield(L, -2, "startPosition");

            lua_pushnumber(L, touch.time);
            lua_setfield(L, -2, "time");

            lua_pushboolean(L, touch.began);
            lua_setfield(L, -2, "began");

            // Unity's names (TouchPhase.Began ... are the same phase values)
            lua_pushinteger(L, touch.fingerId);
            lua_setfield(L, -2, "fingerId");

            if (!luabridge::push(L, touch.deltaPosition))
                return makeErrorCode(ErrorCode::LuaStackOverflow);
            lua_setfield(L, -2, "deltaPosition");

            lua_pushnumber(L, touch.deltaTime);
            lua_setfield(L, -2, "deltaTime");

            lua_pushinteger(L, touch.tapCount);
            lua_setfield(L, -2, "tapCount");

            return {};
        }

        static TypeResult<Touch> get(lua_State* L, int index)
        {
            index = lua_absindex(L, index);
            lua_getfield(L, index, "pointer");
            // a table with Unity's fingerId only
            if (lua_type(L, -1) != LUA_TNUMBER){
                lua_pop(L, 1);
                lua_getfield(L, index, "fingerId");
            }
            if (lua_type(L, -1) != LUA_TNUMBER)
                return makeErrorCode(ErrorCode::InvalidTypeCast);
            if (! is_integral_representable_by<int>(L, -1))
                return makeErrorCode(ErrorCode::IntegerDoesntFitIntoLuaInteger);
            int pointer = lua_tointeger(L, -1);

            lua_getfield(L, index, "position");
            auto result = luabridge::get<Vector2>(L, -1);
            if (! result)
                return result.error();
            Vector2 position = *result;

            Touch touch = {pointer, position};
            // the frame fields, when the table has them (a Touch from getTouch)
            lua_getfield(L, index, "phase");
            if (lua_type(L, -1) == LUA_TNUMBER)
                touch.phase = static_cast<TouchPhase>(lua_tointeger(L, -1));
            lua_getfield(L, index, "delta");
            if (auto delta = luabridge::get<Vector2>(L, -1))
                touch.delta = *delta;
            lua_getfield(L, index, "startPosition");
            if (auto start = luabridge::get<Vector2>(L, -1))
                touch.startPosition = *start;
            lua_getfield(L, index, "time");
            if (lua_type(L, -1) == LUA_TNUMBER)
                touch.time = (float)lua_tonumber(L, -1);
            lua_getfield(L, index, "began");
            touch.began = lua_toboolean(L, -1) != 0;
            touch.fingerId = pointer;
            lua_getfield(L, index, "deltaPosition");
            if (auto delta = luabridge::get<Vector2>(L, -1))
                touch.deltaPosition = *delta;
            else
                touch.deltaPosition = touch.delta;
            lua_getfield(L, index, "deltaTime");
            if (lua_type(L, -1) == LUA_TNUMBER)
                touch.deltaTime = (float)lua_tonumber(L, -1);
            lua_getfield(L, index, "tapCount");
            if (lua_type(L, -1) == LUA_TNUMBER)
                touch.tapCount = (int)lua_tointeger(L, -1);
            lua_pop(L, 10);
            return touch;
        }

        static bool isInstance (lua_State* L, int index)
        {
            if (lua_type (L, index) != LUA_TTABLE)
                return false;
            index = lua_absindex(L, index);
            const int top = lua_gettop(L);
            // a pointer (S22) or a fingerId (Unity), and a position
            bool ok = lua_getfield(L, index, "pointer") == LUA_TNUMBER;
            if (!ok){
                lua_pop(L, 1);
                ok = lua_getfield(L, index, "fingerId") == LUA_TNUMBER;
            }
            ok = ok && is_integral_representable_by<int>(L, -1);
            ok = ok && lua_getfield(L, index, "position") == LUA_TUSERDATA && Stack<Vector2>::isInstance(L, -1);
            lua_settop(L, top);
            return ok;
        }
    };

    // feature debug-overlay: LogLevel.INFO .. ERROR, DebugOverlay.STATS_*
    template<> struct Stack<jasperengine::LogLevel> : EnumWrapper<jasperengine::LogLevel>{};
    template<> struct Stack<jasperengine::DebugOverlaySettings::Stats> : EnumWrapper<jasperengine::DebugOverlaySettings::Stats>{};

    // Entity type is not necessary here because it already exists Stack<unsigned int>

    template <>
    struct Stack <Signature>
    {
        static Result push(lua_State* L, Signature sig)
        {
            lua_pushstring(L, sig.to_string().c_str());
            return {};
        }

        static TypeResult<Signature> get(lua_State* L, int index)
        {
            if (lua_type(L, index) != LUA_TSTRING)
                return makeErrorCode(ErrorCode::InvalidTypeCast);
            // up to 256 '0'/'1' characters, MSB first (a 64-character string sets the low bits)
            Signature signature;
            if (!Signature::fromString(lua_tostring(L, index), signature))
                return makeErrorCode(ErrorCode::InvalidTypeCast);
            return signature;
        }

        static bool isInstance (lua_State* L, int index)
        {
            return lua_type(L, index) == LUA_TSTRING;
        }
    };
}

#endif /* LuaBridgeAddon_h */
