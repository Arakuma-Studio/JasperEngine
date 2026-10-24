// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LuaBridgeAddon_h
#define LuaBridgeAddon_h

#include "Scene.h"
#include "Input.h"
#include "FileData.h"
#include "thread/ResourceProgress.h"
#include "jobs/JobTypes.h"
#include "texture/Material.h"
#include "component/UIDocumentComponent.h"
#include "component/FogComponent.h"
#include "component/LocalVolumetricFogComponent.h"
#include "component/VolumetricCloudsComponent.h"

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
    template<> struct Stack<SceneColorScale> : EnumWrapper<SceneColorScale>{};
    template<> struct Stack<TonemapMode> : EnumWrapper<TonemapMode>{};
    template<> struct Stack<MonochromeMode> : EnumWrapper<MonochromeMode>{};
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
            return Signature(lua_tostring(L, index));
        }

        static bool isInstance (lua_State* L, int index)
        {
            return lua_type(L, index) == LUA_TSTRING;
        }
    };
}

#endif /* LuaBridgeAddon_h */
