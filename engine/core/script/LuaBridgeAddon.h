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
#include "component/LocalVolumetricFogComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "render/shadow/ShadowTypes.h"
#include "render/LodFade.h"
#include "render/shadow/ShadowTiers.h"
#include "render/LightingTiers.h"
#include "render/OrderIndependentTransparency.h"
#include "component/CapsuleShadowCasterComponent.h"
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

    template<> struct Stack<AssetRef> : AssetPathStack<AssetRef>{};
    template<> struct Stack<ShaderRef> : AssetPathStack<ShaderRef>{};

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

    template<> struct Stack<Scaling> : EnumWrapper<Scaling>{};
    template<> struct Stack<jasperengine::Platform> : EnumWrapper<jasperengine::Platform>{};  // qualified: the editor has a Platform class too
    template<> struct Stack<GraphicBackend> : EnumWrapper<GraphicBackend>{};
    template<> struct Stack<TextureStrategy> : EnumWrapper<TextureStrategy>{};
    template<> struct Stack<TextureType> : EnumWrapper<TextureType>{};
    template<> struct Stack<ColorFormat> : EnumWrapper<ColorFormat>{};
    template<> struct Stack<PrimitiveType> : EnumWrapper<PrimitiveType>{};
    template<> struct Stack<CullingMode> : EnumWrapper<CullingMode>{};
    template<> struct Stack<WindingOrder> : EnumWrapper<WindingOrder>{};
    template<> struct Stack<LodFadeMode> : EnumWrapper<LodFadeMode>{};
    template<> struct Stack<MaterialAlphaMode> : EnumWrapper<MaterialAlphaMode>{};
    template<> struct Stack<MaterialLighting> : EnumWrapper<MaterialLighting>{};
    template<> struct Stack<TextureFilter> : EnumWrapper<TextureFilter>{};
    template<> struct Stack<TextureWrap> : EnumWrapper<TextureWrap>{};
    template<> struct Stack<AdMobRating> : EnumWrapper<AdMobRating>{};
    template<> struct Stack<CursorType> : EnumWrapper<CursorType>{};
    template<> struct Stack<MouseMode> : EnumWrapper<MouseMode>{};
    template<> struct Stack<TouchPhase> : EnumWrapper<TouchPhase>{};
    template<> struct Stack<PointerLockState> : EnumWrapper<PointerLockState>{};
    template<> struct Stack<InputActionPhase> : EnumWrapper<InputActionPhase>{};
    template<> struct Stack<InputActionType> : EnumWrapper<InputActionType>{};
    template<> struct Stack<CharacterCollisionFlags> : EnumWrapper<CharacterCollisionFlags>{};
    template<> struct Stack<WaterVolumeWaves> : EnumWrapper<WaterVolumeWaves>{};
    template<> struct Stack<WaterExcluderShape> : EnumWrapper<WaterExcluderShape>{};
    template<> struct Stack<WaterWakeResolution> : EnumWrapper<WaterWakeResolution>{};
    template<> struct Stack<BuoyancyColumns> : EnumWrapper<BuoyancyColumns>{};
    template<> struct Stack<spline::SplineType> : EnumWrapper<spline::SplineType>{};
    template<> struct Stack<spline::TangentMode> : EnumWrapper<spline::TangentMode>{};
    template<> struct Stack<SplineExtrudeShape> : EnumWrapper<SplineExtrudeShape>{};
    template<> struct Stack<SplineWidthSource> : EnumWrapper<SplineWidthSource>{};
    template<> struct Stack<SplineCornerStyle> : EnumWrapper<SplineCornerStyle>{};
    template<> struct Stack<SplineGrade> : EnumWrapper<SplineGrade>{};
    template<> struct Stack<SplineConform> : EnumWrapper<SplineConform>{};
    template<> struct Stack<SplineCollision> : EnumWrapper<SplineCollision>{};
    template<> struct Stack<SplineInstantiateMode> : EnumWrapper<SplineInstantiateMode>{};
    template<> struct Stack<SplineSpacingFit> : EnumWrapper<SplineSpacingFit>{};
    template<> struct Stack<SplinePlantMode> : EnumWrapper<SplinePlantMode>{};
    template<> struct Stack<SplineAnimateMethod> : EnumWrapper<SplineAnimateMethod>{};
    template<> struct Stack<SplineLoop> : EnumWrapper<SplineLoop>{};
    template<> struct Stack<SplineEasing> : EnumWrapper<SplineEasing>{};
    template<> struct Stack<SplineAlignment> : EnumWrapper<SplineAlignment>{};
    template<> struct Stack<ResourceLoadState> : EnumWrapper<ResourceLoadState>{};
    template<> struct Stack<LightState> : EnumWrapper<LightState>{};
    template<> struct Stack<UIEventState> : EnumWrapper<UIEventState>{};
    // platform services (System, Screen, Input.deviceOrientation)
    template<> struct Stack<SystemLanguage> : EnumWrapper<SystemLanguage>{};
    template<> struct Stack<ScreenOrientation> : EnumWrapper<ScreenOrientation>{};
    template<> struct Stack<DeviceOrientation> : EnumWrapper<DeviceOrientation>{};
    template<> struct Stack<DeviceType> : EnumWrapper<DeviceType>{};
    template<> struct Stack<DeviceTier> : EnumWrapper<DeviceTier>{};
    template<> struct Stack<ThermalState> : EnumWrapper<ThermalState>{};
    template<> struct Stack<NetworkReachability> : EnumWrapper<NetworkReachability>{};
    template<> struct Stack<HapticFeedback> : EnumWrapper<HapticFeedback>{};
    template<> struct Stack<SleepTimeout> : EnumWrapper<SleepTimeout>{};

    template<> struct Stack<EntityPool> : EnumWrapper<EntityPool>{};

    template<> struct Stack<ResourceType> : EnumWrapper<ResourceType>{};
    template<> struct Stack<jobs::Lane> : EnumWrapper<jobs::Lane>{};

    template<> struct Stack<RotationOrder> : EnumWrapper<RotationOrder>{};

    template<> struct Stack<FogType> : EnumWrapper<FogType>{};
    template<> struct Stack<FogColorMode> : EnumWrapper<FogColorMode>{};
    template<> struct Stack<FogVolumeShape> : EnumWrapper<FogVolumeShape>{};
    template<> struct Stack<FogVolumeFalloff> : EnumWrapper<FogVolumeFalloff>{};
    template<> struct Stack<CloudsPreset> : EnumWrapper<CloudsPreset>{};
    template<> struct Stack<CloudsRendering> : EnumWrapper<CloudsRendering>{};
    template<> struct Stack<AtmosphereQuality> : EnumWrapper<AtmosphereQuality>{};
    template<> struct Stack<AtmosphereQualityOverride> : EnumWrapper<AtmosphereQualityOverride>{};
    template<> struct Stack<CameraType> : EnumWrapper<CameraType>{};
    template<> struct Stack<PlayMode> : EnumWrapper<PlayMode>{};
    template<> struct Stack<FrustumPlane> : EnumWrapper<FrustumPlane>{};
    template<> struct Stack<LightType> : EnumWrapper<LightType>{};
    template<> struct Stack<LightShading> : EnumWrapper<LightShading>{};
    template<> struct Stack<ShadowCaster2DShape> : EnumWrapper<ShadowCaster2DShape>{};
    template<> struct Stack<ShadowQuality> : EnumWrapper<ShadowQuality>{};
    // 3D shadows (S10)
    template<> struct Stack<ShadowType> : EnumWrapper<ShadowType>{};
    template<> struct Stack<ShadowUpdateMode> : EnumWrapper<ShadowUpdateMode>{};
    // S57's lighting
    template<> struct Stack<TransparencyMode> : EnumWrapper<TransparencyMode>{};
    template<> struct Stack<ShadingModel> : EnumWrapper<ShadingModel>{};
    template<> struct Stack<CapsuleCastShadows> : EnumWrapper<CapsuleCastShadows>{};
    template<> struct Stack<CapsuleDirection> : EnumWrapper<CapsuleDirection>{};
    template<> struct Stack<LightMode> : EnumWrapper<LightMode>{};
    template<> struct Stack<CascadeSplitMode> : EnumWrapper<CascadeSplitMode>{};
    template<> struct Stack<ShadowTier> : EnumWrapper<ShadowTier>{};
    template<> struct Stack<SceneColorScale> : EnumWrapper<SceneColorScale>{};
    template<> struct Stack<PlanarReflectionResolution> : EnumWrapper<PlanarReflectionResolution>{};
    template<> struct Stack<LitQuality> : EnumWrapper<LitQuality>{};
    template<> struct Stack<LightUnit> : EnumWrapper<LightUnit>{};
    template<> struct Stack<TonemapMode> : EnumWrapper<TonemapMode>{};
    template<> struct Stack<MonochromeMode> : EnumWrapper<MonochromeMode>{};
    template<> struct Stack<FilmGrainType> : EnumWrapper<FilmGrainType>{};
    template<> struct Stack<FilmGateMask> : EnumWrapper<FilmGateMask>{};
    template<> struct Stack<LutInputEncoding> : EnumWrapper<LutInputEncoding>{};
    template<> struct Stack<AudioSourceState> : EnumWrapper<AudioSourceState>{};
    template<> struct Stack<AudioSourceAttenuation> : EnumWrapper<AudioSourceAttenuation>{};
    template<> struct Stack<AudioLoadType> : EnumWrapper<AudioLoadType>{};
    template<> struct Stack<AudioPlayOn> : EnumWrapper<AudioPlayOn>{};

    template<> struct Stack<EaseType> : EnumWrapper<EaseType>{};
    template<> struct Stack<ActionState> : EnumWrapper<ActionState>{};

    template<> struct Stack<FileErrors> : EnumWrapper<FileErrors>{};

    template<> struct Stack<AnchorPreset> : EnumWrapper<AnchorPreset>{};
    template<> struct Stack<LayoutGroupType> : EnumWrapper<LayoutGroupType>{};
    // Canvas Groups (ui canvas group spec 7.6)
    template<> struct Stack<CanvasGroupRenderMode> : EnumWrapper<CanvasGroupRenderMode>{};
    template<> struct Stack<AlphaTweenTarget> : EnumWrapper<AlphaTweenTarget>{};
    // ui-scaling-text: the Canvas Scaler and Text Effect enums
    template<> struct Stack<UIScaleMode> : EnumWrapper<UIScaleMode>{};
    template<> struct Stack<ScreenMatchMode> : EnumWrapper<ScreenMatchMode>{};
    template<> struct Stack<PhysicalUnit> : EnumWrapper<PhysicalUnit>{};
    template<> struct Stack<TextEffectType> : EnumWrapper<TextEffectType>{};
    template<> struct Stack<TextEffectMix> : EnumWrapper<TextEffectMix>{};
    template<> struct Stack<PivotPreset> : EnumWrapper<PivotPreset>{};

    template<> struct Stack<ScrollbarType> : EnumWrapper<ScrollbarType>{};
    template<> struct Stack<ProgressbarType> : EnumWrapper<ProgressbarType>{};

    template<> struct Stack<Collider2DType> : EnumWrapper<Collider2DType>{};
    template<> struct Stack<Collider3DType> : EnumWrapper<Collider3DType>{};
    template<> struct Stack<BodyType> : EnumWrapper<BodyType>{};
    template<> struct Stack<Joint2DType> : EnumWrapper<Joint2DType>{};
    template<> struct Stack<JointLimitState2D> : EnumWrapper<JointLimitState2D>{};
    template<> struct Stack<CollisionDetectionMode2D> : EnumWrapper<CollisionDetectionMode2D>{};
    template<> struct Stack<RigidbodySleepMode2D> : EnumWrapper<RigidbodySleepMode2D>{};
    template<> struct Stack<JointAnchorSpace> : EnumWrapper<JointAnchorSpace>{};
    template<> struct Stack<JointBreakAction> : EnumWrapper<JointBreakAction>{};
    template<> struct Stack<PhysicsMaterialCombine> : EnumWrapper<PhysicsMaterialCombine>{};
    template<> struct Stack<UIDocumentRenderMode> : EnumWrapper<UIDocumentRenderMode>{};
    template<> struct Stack<FluidOutput> : EnumWrapper<FluidOutput>{};
    template<> struct Stack<VideoPlayerSource> : EnumWrapper<VideoPlayerSource>{};
    template<> struct Stack<VideoTimeUpdateMode> : EnumWrapper<VideoTimeUpdateMode>{};
    template<> struct Stack<VideoRenderMode> : EnumWrapper<VideoRenderMode>{};
    template<> struct Stack<VideoAspectRatio> : EnumWrapper<VideoAspectRatio>{};
    template<> struct Stack<VideoAudioOutputMode> : EnumWrapper<VideoAudioOutputMode>{};
    // bound enums that had none: LuaBridge pushed them as unregistered userdata (an error
    // reading probe.mode) and took no integer for them
    template<> struct Stack<ReflectionProbeMode> : EnumWrapper<ReflectionProbeMode>{};
    template<> struct Stack<ReflectionProbeUpdateMode> : EnumWrapper<ReflectionProbeUpdateMode>{};
    template<> struct Stack<ParticleEmitterShape> : EnumWrapper<ParticleEmitterShape>{};
    template<> struct Stack<RigidbodyMotionQuality> : EnumWrapper<RigidbodyMotionQuality>{};
    template<> struct Stack<Joint3DType> : EnumWrapper<Joint3DType>{};
    template<> struct Stack<ShaderType> : EnumWrapper<ShaderType>{};
    template<> struct Stack<TextureAlphaBorder> : EnumWrapper<TextureAlphaBorder>{};

    // shader library render state (Material, UI/points/lines)
    template<> struct Stack<MaterialBlendMode> : EnumWrapper<MaterialBlendMode>{};
    template<> struct Stack<MaterialCullMode> : EnumWrapper<MaterialCullMode>{};
    template<> struct Stack<MaterialDepthWrite> : EnumWrapper<MaterialDepthWrite>{};
    template<> struct Stack<MaterialDepthTest> : EnumWrapper<MaterialDepthTest>{};
    // phase 2: the extra pass's order
    template<> struct Stack<MaterialPassOrder> : EnumWrapper<MaterialPassOrder>{};
    template<> struct Stack<MaterialStencilCompare> : EnumWrapper<MaterialStencilCompare>{};
    template<> struct Stack<MaterialStencilOp> : EnumWrapper<MaterialStencilOp>{};
    template<> struct Stack<MaterialColorWriteBits> : EnumWrapper<MaterialColorWriteBits>{};
    // Line and Trail Renderers (lines and trails spec 6.6)
    template<> struct Stack<LineRendererMode> : EnumWrapper<LineRendererMode>{};
    template<> struct Stack<LineWidthUnit> : EnumWrapper<LineWidthUnit>{};
    template<> struct Stack<LineAlignment> : EnumWrapper<LineAlignment>{};
    template<> struct Stack<LineTextureMode> : EnumWrapper<LineTextureMode>{};

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
    template<> struct Stack<LogLevel> : EnumWrapper<LogLevel>{};
    template<> struct Stack<DebugOverlaySettings::Stats> : EnumWrapper<DebugOverlaySettings::Stats>{};

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
