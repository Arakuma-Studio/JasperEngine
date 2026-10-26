// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LuaBridgeAddon_h
#define LuaBridgeAddon_h

#include "Scene.h"
#include "Input.h"
#include "input/InputActions.h"
#include "component/CharacterCollision.h"
#include "component/WaterVolumeComponent.h"
#include "component/BuoyancyComponent.h"
#include "FileData.h"
#include "thread/ResourceProgress.h"
#include "jobs/JobTypes.h"
#include "texture/Material.h"
#include "component/UIDocumentComponent.h"
#include "component/FluidSimulationComponent.h"
#include "component/FogComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/VolumetricCloudsComponent.h"
#include "render/shadow/ShadowTypes.h"
#include "render/shadow/ShadowTiers.h"

using namespace jasperengine;

namespace luabridge
{

    // need to specialized luabridge::Stack and use enum in arguments
    template <class T>
    struct EnumWrapper
    {
        static auto push(lua_State* L, T value) -> std::enable_if_t<std::is_enum_v<T>, luabridge::Result>
        {
            lua_pushinteger(L, static_cast<std::size_t>(value));
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

    template<> struct Stack<Scaling> : EnumWrapper<Scaling>{};
    template<> struct Stack<jasperengine::Platform> : EnumWrapper<jasperengine::Platform>{};  // qualified: the editor has a Platform class too
    template<> struct Stack<GraphicBackend> : EnumWrapper<GraphicBackend>{};
    template<> struct Stack<TextureStrategy> : EnumWrapper<TextureStrategy>{};
    template<> struct Stack<TextureType> : EnumWrapper<TextureType>{};
    template<> struct Stack<ColorFormat> : EnumWrapper<ColorFormat>{};
    template<> struct Stack<PrimitiveType> : EnumWrapper<PrimitiveType>{};
    template<> struct Stack<CullingMode> : EnumWrapper<CullingMode>{};
    template<> struct Stack<WindingOrder> : EnumWrapper<WindingOrder>{};
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
    template<> struct Stack<BuoyancyColumns> : EnumWrapper<BuoyancyColumns>{};
    template<> struct Stack<ResourceLoadState> : EnumWrapper<ResourceLoadState>{};
    template<> struct Stack<LightState> : EnumWrapper<LightState>{};
    template<> struct Stack<UIEventState> : EnumWrapper<UIEventState>{};

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
    template<> struct Stack<FrustumPlane> : EnumWrapper<FrustumPlane>{};
    template<> struct Stack<LightType> : EnumWrapper<LightType>{};
    template<> struct Stack<LightShading> : EnumWrapper<LightShading>{};
    template<> struct Stack<ShadowCaster2DShape> : EnumWrapper<ShadowCaster2DShape>{};
    template<> struct Stack<ShadowQuality> : EnumWrapper<ShadowQuality>{};
    // 3D shadows (S10)
    template<> struct Stack<ShadowType> : EnumWrapper<ShadowType>{};
    template<> struct Stack<ShadowUpdateMode> : EnumWrapper<ShadowUpdateMode>{};
    template<> struct Stack<CascadeSplitMode> : EnumWrapper<CascadeSplitMode>{};
    template<> struct Stack<ShadowTier> : EnumWrapper<ShadowTier>{};
    template<> struct Stack<SceneColorScale> : EnumWrapper<SceneColorScale>{};
    template<> struct Stack<LitQuality> : EnumWrapper<LitQuality>{};
    template<> struct Stack<LightUnit> : EnumWrapper<LightUnit>{};
    template<> struct Stack<TonemapMode> : EnumWrapper<TonemapMode>{};
    template<> struct Stack<MonochromeMode> : EnumWrapper<MonochromeMode>{};
    template<> struct Stack<FilmGrainType> : EnumWrapper<FilmGrainType>{};
    template<> struct Stack<FilmGateMask> : EnumWrapper<FilmGateMask>{};
    template<> struct Stack<LutInputEncoding> : EnumWrapper<LutInputEncoding>{};
    template<> struct Stack<AudioSourceState> : EnumWrapper<AudioSourceState>{};
    template<> struct Stack<AudioSourceAttenuation> : EnumWrapper<AudioSourceAttenuation>{};

    template<> struct Stack<EaseType> : EnumWrapper<EaseType>{};
    template<> struct Stack<ActionState> : EnumWrapper<ActionState>{};

    template<> struct Stack<FileErrors> : EnumWrapper<FileErrors>{};

    template<> struct Stack<AnchorPreset> : EnumWrapper<AnchorPreset>{};
    template<> struct Stack<LayoutGroupType> : EnumWrapper<LayoutGroupType>{};
    template<> struct Stack<PivotPreset> : EnumWrapper<PivotPreset>{};

    template<> struct Stack<ScrollbarType> : EnumWrapper<ScrollbarType>{};
    template<> struct Stack<ProgressbarType> : EnumWrapper<ProgressbarType>{};

    template<> struct Stack<Collider2DType> : EnumWrapper<Collider2DType>{};
    template<> struct Stack<Collider3DType> : EnumWrapper<Collider3DType>{};
    template<> struct Stack<BodyType> : EnumWrapper<BodyType>{};
    template<> struct Stack<Joint2DType> : EnumWrapper<Joint2DType>{};
    template<> struct Stack<UIDocumentRenderMode> : EnumWrapper<UIDocumentRenderMode>{};
    template<> struct Stack<FluidOutput> : EnumWrapper<FluidOutput>{};
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

            return {};
        }

        static TypeResult<Touch> get(lua_State* L, int index)
        {
            lua_getfield(L, index, "pointer");
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
            lua_pop(L, 7);
            return touch;
        }

        static bool isInstance (lua_State* L, int index)
        {
            if (lua_type (L, index) != LUA_TTABLE)
                return false;
            if (lua_getfield(L, index, "pointer") != LUA_TNUMBER)
                return false;
            if (!is_integral_representable_by<int>(L, -1))
                return false;
            if (lua_getfield(L, index, "position") != LUA_TUSERDATA)
                return false;
            if (!Stack<Vector2>::isInstance(L, -1))
                return false;

            return true;
        }
    };

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
