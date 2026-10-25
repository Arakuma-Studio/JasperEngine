// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaBinding.h"
#include <optional>
#include "log/Logger.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "SceneManager.h"
#include "PrefabManager.h"
#include "PrefabAsset.h"
#include "asset/AssetMap.h"
#include "Input.h"
#include "System.h"
#include "registry/PhysicsLayers.h"
#include "math/Ray.h"
#ifdef JASPERENGINE_PHYSICS_2D
#include "Rigidbody2D.h"
#include "Contact2D.h"
#include "Manifold2D.h"
#endif
#ifdef JASPERENGINE_PHYSICS_3D
#include "Rigidbody.h"
#include "Contact3D.h"
#include "CollideShapeResult3D.h"
#endif

#include "subsystem/ActionSystem.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UISystem.h"
#include "pool/ShaderPool.h"
#include "render/ObjectRender.h"
#include "shader/ShaderVariantCollection.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderUniforms.h"
#include "object/EntityHandle.h"

using namespace jasperengine;

namespace {
    // What Log.category(name) returns: a category's trace/debug/info/warn/error
    struct LuaLogCategory {
        LogCategory* category;
    };

    void luaLog(lua_State* L, LogLevel level, const LogCategory& category, const char* text) {
        if (!Logger::shouldLog(level, category)) return;
        LuaBinding::logFromLua(L, level, category, text ? std::string_view(text) : std::string_view("(nil)"));
    }
}

// LayerMask.getMask("Default", "Enemies") or LayerMask.getMask({"Default", "Enemies"}).
// Unity returns an int: layer 31 makes it negative, as there.
static int luaLayerMaskGetMask(lua_State* L){
    std::vector<std::string> names;
    const int top = lua_gettop(L);
    if (top == 1 && lua_istable(L, 1)){
        const lua_Integer count = (lua_Integer)lua_rawlen(L, 1);
        for (lua_Integer i = 1; i <= count; i++){
            lua_rawgeti(L, 1, i);
            if (lua_type(L, -1) != LUA_TSTRING){
                return luaL_error(L, "LayerMask.getMask: layer names must be strings");
            }
            names.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }else{
        for (int i = 1; i <= top; i++){
            names.emplace_back(luaL_checkstring(L, i));
        }
    }
    lua_pushinteger(L, (lua_Integer)(int32_t)TagManager::getMask(names));
    return 1;
}

// Physics and Physics2D. Layer masks are read as Unity's int and any integer is taken by
// its low 32 bits, so LayerMask.getMask(...), ~mask and -1 (Everything) all work.
namespace{

    uint32_t luaOptLayerMask(lua_State* L, int index, uint32_t defaultMask){
        if (lua_isnoneornil(L, index)){
            return defaultMask;
        }
        return (uint32_t)luaL_checkinteger(L, index);
    }

    lua_Integer luaMaskToInteger(uint32_t mask){
        return (lua_Integer)(int32_t)mask;
    }

    lua_Integer luaAllLayers(){ return luaMaskToInteger(Physics::ALL_LAYERS); }
    lua_Integer luaDefaultRaycastLayers(){ return luaMaskToInteger(Physics::DEFAULT_RAYCAST_LAYERS); }
    lua_Integer luaIgnoreRaycastLayer(){ return luaMaskToInteger(Physics::IGNORE_RAYCAST_LAYER); }

    template<typename PhysicsT>
    int luaIgnoreLayerCollision(lua_State* L){
        const int layer1 = (int)luaL_checkinteger(L, 1);
        const int layer2 = (int)luaL_checkinteger(L, 2);
        const bool ignore = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3) != 0;
        PhysicsT::ignoreLayerCollision(layer1, layer2, ignore);
        return 0;
    }

    template<typename PhysicsT>
    int luaGetIgnoreLayerCollision(lua_State* L){
        const int layer1 = (int)luaL_checkinteger(L, 1);
        const int layer2 = (int)luaL_checkinteger(L, 2);
        lua_pushboolean(L, PhysicsT::getIgnoreLayerCollision(layer1, layer2) ? 1 : 0);
        return 1;
    }

    template<typename PhysicsT>
    int luaGetLayerCollisionMask(lua_State* L){
        const int layer = (int)luaL_checkinteger(L, 1);
        lua_pushinteger(L, luaMaskToInteger(PhysicsT::getLayerCollisionMask(layer)));
        return 1;
    }

    template<typename PhysicsT>
    int luaSetLayerCollisionMask(lua_State* L){
        const int layer = (int)luaL_checkinteger(L, 1);
        const uint32_t mask = (uint32_t)luaL_checkinteger(L, 2);
        PhysicsT::setLayerCollisionMask(layer, mask);
        return 0;
    }

    template<typename PhysicsT>
    int luaResetLayerCollisionMatrix(lua_State* L){
        (void)L;
        PhysicsT::resetLayerCollisionMatrix();
        return 0;
    }

#if defined(JASPERENGINE_PHYSICS_2D) || defined(JASPERENGINE_PHYSICS_3D)
    Scene* luaSceneArg(lua_State* L, const char* function){
        Scene* scene = nullptr;
        if (luabridge::isInstance<Scene*>(L, 1)){
            auto result = luabridge::get<Scene*>(L, 1);
            if (result){
                scene = result.value();
            }
        }
        if (!scene){
            luaL_error(L, "%s: argument 1 must be a Scene", function);
        }
        return scene;
    }

    // get<T> raises a Lua error on a mismatch, so the type is tested first
    bool luaRayArg(lua_State* L, int index, Ray& ray){
        if (!luabridge::isInstance<Ray>(L, index)){
            return false;
        }
        auto result = luabridge::get<Ray>(L, index);
        if (!result){
            return false;
        }
        ray = result.value();
        return true;
    }

    void luaPushRayReturn(lua_State* L, const RayReturn& hit){
        if (!luabridge::push(L, hit)){
            lua_pushnil(L);
        }
    }

    void luaPushRayReturns(lua_State* L, const std::vector<RayReturn>& hits){
        lua_createtable(L, (int)hits.size(), 0);
        for (size_t i = 0; i < hits.size(); i++){
            luaPushRayReturn(L, hits[i]);
            lua_rawseti(L, -2, (lua_Integer)(i + 1));
        }
    }

    // Physics.raycast(scene, ray [, layerMask]) or (scene, origin, direction, maxDistance [, layerMask]),
    // and the 2D versions with Vector2 origin and direction
    template<typename VectorT, bool all, bool is2D>
    int luaRaycast(lua_State* L){
        const char* function = is2D ? (all ? "Physics2D.raycastAll" : "Physics2D.raycast")
                                    : (all ? "Physics.raycastAll" : "Physics.raycast");
        Scene* scene = luaSceneArg(L, function);

        Ray ray;
        uint32_t layerMask;
        if (luaRayArg(L, 2, ray)){
            layerMask = luaOptLayerMask(L, 3, Physics::DEFAULT_RAYCAST_LAYERS);
        }else{
            if (!luabridge::isInstance<VectorT>(L, 2) || !luabridge::isInstance<VectorT>(L, 3)){
                return luaL_error(L, "%s: expected (scene, ray [, layerMask]) or (scene, origin, direction, distance [, layerMask])", function);
            }
            auto origin = luabridge::get<VectorT>(L, 2);
            auto direction = luabridge::get<VectorT>(L, 3);
            if (!origin || !direction){
                return luaL_error(L, "%s: expected (scene, ray [, layerMask]) or (scene, origin, direction, distance [, layerMask])", function);
            }
            const float maxDistance = (float)luaL_checknumber(L, 4);
            layerMask = luaOptLayerMask(L, 5, Physics::DEFAULT_RAYCAST_LAYERS);

            const VectorT originValue = origin.value();
            const VectorT directionValue = direction.value();
            if constexpr (is2D){
#ifdef JASPERENGINE_PHYSICS_2D
                if (all){
                    luaPushRayReturns(L, Physics2D::raycastAll(scene, originValue, directionValue, maxDistance, layerMask));
                }else{
                    luaPushRayReturn(L, Physics2D::raycast(scene, originValue, directionValue, maxDistance, layerMask));
                }
#endif
            }else{
#ifdef JASPERENGINE_PHYSICS_3D
                if (all){
                    luaPushRayReturns(L, Physics::raycastAll(scene, originValue, directionValue, maxDistance, layerMask));
                }else{
                    luaPushRayReturn(L, Physics::raycast(scene, originValue, directionValue, maxDistance, layerMask));
                }
#endif
            }
            return 1;
        }

        if constexpr (is2D){
#ifdef JASPERENGINE_PHYSICS_2D
            if (all){
                luaPushRayReturns(L, Physics2D::raycastAll(scene, ray, layerMask));
            }else{
                luaPushRayReturn(L, Physics2D::raycast(scene, ray, layerMask));
            }
#endif
        }else{
#ifdef JASPERENGINE_PHYSICS_3D
            if (all){
                luaPushRayReturns(L, Physics::raycastAll(scene, ray, layerMask));
            }else{
                luaPushRayReturn(L, Physics::raycast(scene, ray, layerMask));
            }
#endif
        }
        return 1;
    }
#endif

}

void LuaBinding::registerCoreClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginNamespace("Scaling")
        .addVariable("FITWIDTH", Scaling::FITWIDTH)
        .addVariable("FITHEIGHT", Scaling::FITHEIGHT)
        .addVariable("LETTERBOX", Scaling::LETTERBOX)
        .addVariable("CROP", Scaling::CROP)
        .addVariable("STRETCH", Scaling::STRETCH)
        .addVariable("NATIVE", Scaling::NATIVE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("Platform")
        .addVariable("MacOS", Platform::MacOS)
        .addVariable("iOS", Platform::iOS)
        .addVariable("Web", Platform::Web)
        .addVariable("Android", Platform::Android)
        .addVariable("Linux", Platform::Linux)
        .addVariable("Windows", Platform::Windows)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("GraphicBackend")
        .addVariable("GLCORE", GraphicBackend::GLCORE)
        .addVariable("GLES3", GraphicBackend::GLES3)
        .addVariable("D3D11", GraphicBackend::D3D11)
        .addVariable("METAL", GraphicBackend::METAL)
        .addVariable("WGPU", GraphicBackend::WGPU)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("TextureStrategy")
        .addVariable("FIT", TextureStrategy::FIT)
        .addVariable("RESIZE", TextureStrategy::RESIZE)
        .addVariable("NONE", TextureStrategy::NONE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("TextureType")
        .addVariable("TEXTURE_2D", TextureType::TEXTURE_2D)
        .addVariable("TEXTURE_3D", TextureType::TEXTURE_3D)
        .addVariable("TEXTURE_CUBE", TextureType::TEXTURE_CUBE)
        .addVariable("TEXTURE_ARRAY", TextureType::TEXTURE_ARRAY)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ColorFormat")
        .addVariable("RED", ColorFormat::RED)
        .addVariable("RGBA", ColorFormat::RGBA)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("PrimitiveType")
        .addVariable("TRIANGLES", PrimitiveType::TRIANGLES)
        .addVariable("TRIANGLE_STRIP", PrimitiveType::TRIANGLE_STRIP)
        .addVariable("POINTS", PrimitiveType::POINTS)
        .addVariable("LINES", PrimitiveType::LINES)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("CullingMode")
        .addVariable("BACK", CullingMode::BACK)
        .addVariable("FRONT", CullingMode::FRONT)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("WindingOrder")
        .addVariable("CCW", WindingOrder::CCW)
        .addVariable("CW", WindingOrder::CW)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialAlphaMode")
        .addVariable("AUTO", MaterialAlphaMode::AUTO)
        .addVariable("ALPHA_OPAQUE", MaterialAlphaMode::ALPHA_OPAQUE)
        .addVariable("MASK", MaterialAlphaMode::MASK)
        .addVariable("BLEND", MaterialAlphaMode::BLEND)
        .endNamespace();

    // shader library render state of a Material (and UI/points/lines blendMode, depthWrite)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialBlendMode")
        .addVariable("AUTO", MaterialBlendMode::AUTO)
        .addVariable("NONE", MaterialBlendMode::NONE)
        .addVariable("ALPHA", MaterialBlendMode::ALPHA)
        .addVariable("PREMULTIPLIED", MaterialBlendMode::PREMULTIPLIED)
        .addVariable("ADDITIVE", MaterialBlendMode::ADDITIVE)
        .addVariable("MULTIPLY", MaterialBlendMode::MULTIPLY)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialCullMode")
        .addVariable("AUTO", MaterialCullMode::AUTO)
        .addVariable("BACK", MaterialCullMode::BACK)
        .addVariable("FRONT", MaterialCullMode::FRONT)
        .addVariable("OFF", MaterialCullMode::OFF)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialDepthWrite")
        .addVariable("AUTO", MaterialDepthWrite::AUTO)
        .addVariable("ENABLED", MaterialDepthWrite::ENABLED)
        .addVariable("DISABLED", MaterialDepthWrite::DISABLED)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialDepthTest")
        .addVariable("AUTO", MaterialDepthTest::AUTO)
        .addVariable("LESS", MaterialDepthTest::LESS)
        .addVariable("LESS_EQUAL", MaterialDepthTest::LESS_EQUAL)
        .addVariable("EQUAL", MaterialDepthTest::EQUAL)
        .addVariable("GREATER_EQUAL", MaterialDepthTest::GREATER_EQUAL)
        .addVariable("GREATER", MaterialDepthTest::GREATER)
        .addVariable("NOT_EQUAL", MaterialDepthTest::NOT_EQUAL)
        .addVariable("ALWAYS", MaterialDepthTest::ALWAYS)
        .endNamespace();

    // stencil test of a Material (and UI/points/lines): AUTO takes the shader's @state,
    // DISABLED turns it off, any other compare makes the whole MaterialStencil explicit
    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialStencilCompare")
        .addVariable("AUTO", MaterialStencilCompare::AUTO)
        .addVariable("DISABLED", MaterialStencilCompare::DISABLED)
        .addVariable("NEVER", MaterialStencilCompare::NEVER)
        .addVariable("LESS", MaterialStencilCompare::LESS)
        .addVariable("EQUAL", MaterialStencilCompare::EQUAL)
        .addVariable("LESS_EQUAL", MaterialStencilCompare::LESS_EQUAL)
        .addVariable("GREATER", MaterialStencilCompare::GREATER)
        .addVariable("NOT_EQUAL", MaterialStencilCompare::NOT_EQUAL)
        .addVariable("GREATER_EQUAL", MaterialStencilCompare::GREATER_EQUAL)
        .addVariable("ALWAYS", MaterialStencilCompare::ALWAYS)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialStencilOp")
        .addVariable("KEEP", MaterialStencilOp::KEEP)
        .addVariable("ZERO", MaterialStencilOp::ZERO)
        .addVariable("REPLACE", MaterialStencilOp::REPLACE)
        .addVariable("INCR_CLAMP", MaterialStencilOp::INCR_CLAMP)
        .addVariable("DECR_CLAMP", MaterialStencilOp::DECR_CLAMP)
        .addVariable("INVERT", MaterialStencilOp::INVERT)
        .addVariable("INCR_WRAP", MaterialStencilOp::INCR_WRAP)
        .addVariable("DECR_WRAP", MaterialStencilOp::DECR_WRAP)
        .endNamespace();

    // colorWrite values: AUTO (-1), NONE (0) or a sum of R, G, B, A
    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialColorWrite")
        .addVariable("AUTO", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_AUTO)
        .addVariable("NONE", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_NONE)
        .addVariable("R", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_R)
        .addVariable("G", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_G)
        .addVariable("B", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_B)
        .addVariable("A", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_A)
        .addVariable("RGB", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_RGB)
        .addVariable("ALL", MaterialColorWriteBits::MATERIAL_COLOR_WRITE_ALL)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialLighting")
        .addVariable("AUTO", MaterialLighting::AUTO)
        .addVariable("PIXEL", MaterialLighting::PIXEL)
        .addVariable("VERTEX", MaterialLighting::VERTEX)
        .addVariable("MIXED", MaterialLighting::MIXED)
        .addVariable("NONE", MaterialLighting::NONE)
        .endNamespace();

    // shader library phase 2: Material.extraPass.order
    luabridge::getGlobalNamespace(L)
        .beginNamespace("MaterialPassOrder")
        .addVariable("AFTER", MaterialPassOrder::AFTER)
        .addVariable("BEFORE", MaterialPassOrder::BEFORE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("TextureFilter")
        .addVariable("NEAREST", TextureFilter::NEAREST)
        .addVariable("LINEAR", TextureFilter::LINEAR)
        .addVariable("NEAREST_MIPMAP_NEAREST", TextureFilter::NEAREST_MIPMAP_NEAREST)
        .addVariable("NEAREST_MIPMAP_LINEAR", TextureFilter::NEAREST_MIPMAP_LINEAR)
        .addVariable("LINEAR_MIPMAP_NEAREST", TextureFilter::LINEAR_MIPMAP_NEAREST)
        .addVariable("LINEAR_MIPMAP_LINEAR", TextureFilter::LINEAR_MIPMAP_LINEAR)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("TextureWrap")
        .addVariable("REPEAT", TextureWrap::REPEAT)
        .addVariable("MIRRORED_REPEAT", TextureWrap::MIRRORED_REPEAT)
        .addVariable("CLAMP_TO_EDGE", TextureWrap::CLAMP_TO_EDGE)
        .addVariable("CLAMP_TO_BORDER", TextureWrap::CLAMP_TO_BORDER)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("TextureAlphaBorder")
        .addVariable("AUTO", TextureAlphaBorder::AUTO)
        .addVariable("FIX", TextureAlphaBorder::FIX)
        .addVariable("KEEP", TextureAlphaBorder::KEEP)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("AdMobRating")
        .addVariable("General", AdMobRating::General)
        .addVariable("ParentalGuidance", AdMobRating::ParentalGuidance)
        .addVariable("Teen", AdMobRating::Teen)
        .addVariable("MatureAudience", AdMobRating::MatureAudience)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("CursorType")
        .addVariable("ARROW", CursorType::ARROW)
        .addVariable("IBEAM", CursorType::IBEAM)
        .addVariable("CROSSHAIR", CursorType::CROSSHAIR)
        .addVariable("POINTING_HAND", CursorType::POINTING_HAND)
        .addVariable("RESIZE_EW", CursorType::RESIZE_EW)
        .addVariable("RESIZE_NS", CursorType::RESIZE_NS)
        .addVariable("RESIZE_NWSE", CursorType::RESIZE_NWSE)
        .addVariable("RESIZE_NESW", CursorType::RESIZE_NESW)
        .addVariable("RESIZE_ALL", CursorType::RESIZE_ALL)
        .addVariable("NOT_ALLOWED", CursorType::NOT_ALLOWED)
        .addVariable("WAIT", CursorType::WAIT)
        .addVariable("PROGRESS", CursorType::PROGRESS)
        .addVariable("HELP", CursorType::HELP)
        .addVariable("GRAB", CursorType::GRAB)
        .addVariable("GRABBING", CursorType::GRABBING)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MouseMode")
        .addVariable("NORMAL", MouseMode::NORMAL)
        .addVariable("HIDDEN", MouseMode::HIDDEN)
        .addVariable("CAPTURED", MouseMode::CAPTURED)
        .addVariable("CONFINED", MouseMode::CONFINED)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ResourceLoadState")
        .addVariable("NotStarted", ResourceLoadState::NotStarted)
        .addVariable("Loading", ResourceLoadState::Loading)
        .addVariable("Finished", ResourceLoadState::Finished)
        .addVariable("Failed", ResourceLoadState::Failed)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("LightState")
        .addVariable("OFF", LightState::OFF)
        .addVariable("ON", LightState::ON)
        .addVariable("AUTO", LightState::AUTO)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ShadowQuality")
        .addVariable("NONE", ShadowQuality::NONE)
        .addVariable("LOW", ShadowQuality::LOW)
        .addVariable("MEDIUM", ShadowQuality::MEDIUM)
        .addVariable("HIGH", ShadowQuality::HIGH)
        .addVariable("AUTO", ShadowQuality::AUTO)
        .addVariable("ULTRA", ShadowQuality::ULTRA)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ShadowType")
        .addVariable("NONE", ShadowType::NONE)
        .addVariable("HARD", ShadowType::HARD)
        .addVariable("SOFT", ShadowType::SOFT)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ShadowUpdateMode")
        .addVariable("AUTO", ShadowUpdateMode::AUTO)
        .addVariable("EVERY_FRAME", ShadowUpdateMode::EVERY_FRAME)
        .addVariable("ON_DEMAND", ShadowUpdateMode::ON_DEMAND)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("CascadeSplitMode")
        .addVariable("AUTO", CascadeSplitMode::AUTO)
        .addVariable("MANUAL", CascadeSplitMode::MANUAL)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ShadowTier")
        .addVariable("LOW", ShadowTier::LOW)
        .addVariable("MEDIUM", ShadowTier::MEDIUM)
        .addVariable("HIGH", ShadowTier::HIGH)
        .addVariable("ULTRA", ShadowTier::ULTRA)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SceneColorScale")
        .addVariable("FULL", SceneColorScale::FULL)
        .addVariable("HALF", SceneColorScale::HALF)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("TonemapMode")
        .addVariable("NONE", TonemapMode::NONE)
        .addVariable("ACES", TonemapMode::ACES)
        .addVariable("NEUTRAL", TonemapMode::NEUTRAL)
        .addVariable("REINHARD", TonemapMode::REINHARD)
        .addVariable("AGX", TonemapMode::AGX)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("MonochromeMode")
        .addVariable("GRAYSCALE", MonochromeMode::GRAYSCALE)
        .addVariable("SEPIA", MonochromeMode::SEPIA)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("UIEventState")
        .addVariable("NOT_SET", UIEventState::NOT_SET)
        .addVariable("ENABLED", UIEventState::ENABLED)
        .addVariable("DISABLED", UIEventState::DISABLED)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("EntityPool")
        .addVariable("User", EntityPool::User)
        .addVariable("System", EntityPool::System)
        .endNamespace();

    // Unity's LayerMask: layer names from Project Settings > Tags and Layers
    luabridge::getGlobalNamespace(L)
        .beginClass<LayerMask>("LayerMask")
        .addStaticFunction("nameToLayer", &TagManager::nameToLayer)
        .addStaticFunction("layerToName", &TagManager::layerToName)
        .addStaticFunction("getMask", &luaLayerMaskGetMask)
        .endClass();

    // Unity's Physics and Physics2D: Layer Collision Matrix (Project Settings > Physics and
    // Physics 2D) and layer-masked ray casts. Layers are numbers 0..31 (LayerMask.nameToLayer).
    luabridge::getGlobalNamespace(L)
        .beginClass<Physics>("Physics")
        .addStaticProperty("ALL_LAYERS", &luaAllLayers)
        .addStaticProperty("DEFAULT_RAYCAST_LAYERS", &luaDefaultRaycastLayers)
        .addStaticProperty("IGNORE_RAYCAST_LAYER", &luaIgnoreRaycastLayer)
        .addStaticFunction("ignoreLayerCollision", &luaIgnoreLayerCollision<Physics>)
        .addStaticFunction("getIgnoreLayerCollision", &luaGetIgnoreLayerCollision<Physics>)
        .addStaticFunction("getLayerCollisionMask", &luaGetLayerCollisionMask<Physics>)
        .addStaticFunction("setLayerCollisionMask", &luaSetLayerCollisionMask<Physics>)
        .addStaticFunction("resetLayerCollisionMatrix", &luaResetLayerCollisionMatrix<Physics>)
#ifdef JASPERENGINE_PHYSICS_3D
        .addStaticFunction("raycast", &luaRaycast<Vector3, false, false>)
        .addStaticFunction("raycastAll", &luaRaycast<Vector3, true, false>)
#endif
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Physics2D>("Physics2D")
        .addStaticProperty("ALL_LAYERS", &luaAllLayers)
        .addStaticProperty("DEFAULT_RAYCAST_LAYERS", &luaDefaultRaycastLayers)
        .addStaticProperty("IGNORE_RAYCAST_LAYER", &luaIgnoreRaycastLayer)
        .addStaticFunction("ignoreLayerCollision", &luaIgnoreLayerCollision<Physics2D>)
        .addStaticFunction("getIgnoreLayerCollision", &luaGetIgnoreLayerCollision<Physics2D>)
        .addStaticFunction("getLayerCollisionMask", &luaGetLayerCollisionMask<Physics2D>)
        .addStaticFunction("setLayerCollisionMask", &luaSetLayerCollisionMask<Physics2D>)
        .addStaticFunction("resetLayerCollisionMatrix", &luaResetLayerCollisionMatrix<Physics2D>)
#ifdef JASPERENGINE_PHYSICS_2D
        .addStaticFunction("raycast", &luaRaycast<Vector2, false, true>)
        .addStaticFunction("raycastAll", &luaRaycast<Vector2, true, true>)
#endif
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("ShaderType")
        .addVariable("POINTS", ShaderType::POINTS)
        .addVariable("LINES", ShaderType::LINES)
        .addVariable("MESH", ShaderType::MESH)
        .addVariable("SKYBOX", ShaderType::SKYBOX)
        .addVariable("DEPTH", ShaderType::DEPTH)
        .addVariable("GBUFFER", ShaderType::GBUFFER)
        .addVariable("UI", ShaderType::UI)
        .addVariable("SSAO", ShaderType::SSAO)
        .addVariable("SSAO_BLUR", ShaderType::SSAO_BLUR)
        .addVariable("SSR", ShaderType::SSR)
        .addVariable("SSR_BLUR", ShaderType::SSR_BLUR)
        .addVariable("COMPOSITE", ShaderType::COMPOSITE)
        .addVariable("SHADOW2D", ShaderType::SHADOW2D)
        .addVariable("BLIT", ShaderType::BLIT)
        .addVariable("POSTPROCESS", ShaderType::POSTPROCESS)
        .addVariable("BLOOM", ShaderType::BLOOM)
        .addVariable("HTMLUI", ShaderType::HTMLUI)
        .addVariable("HTMLUI_GRADIENT", ShaderType::HTMLUI_GRADIENT)
        .addVariable("HTMLUI_POST", ShaderType::HTMLUI_POST)
        .addVariable("UI_EFFECT", ShaderType::UI_EFFECT)
        .addVariable("PROBE_FILTER", ShaderType::PROBE_FILTER)
        .addVariable("FLUID", ShaderType::FLUID)
        .addVariable("ATMOSPHERE_DEPTH", ShaderType::ATMOSPHERE_DEPTH)
        .addVariable("VOLUMETRIC_FOG", ShaderType::VOLUMETRIC_FOG)
        .addVariable("ATMOSPHERE_DENOISE", ShaderType::ATMOSPHERE_DENOISE)
        .addVariable("VOLUMETRIC_CLOUDS", ShaderType::VOLUMETRIC_CLOUDS)
        .addVariable("SKY_COMPOSITE", ShaderType::SKY_COMPOSITE)
        .addVariable("VOLUMETRIC_COMPOSITE", ShaderType::VOLUMETRIC_COMPOSITE)
        .endNamespace();

    // Shader variants by spec ("mesh:Uv1,Vc4"), or by type and property bits; an
    // optional last argument names a forked shader (its project-relative base path).
    // The property bits are a Lua integer taken as the 64-bit mask it holds (1 << 63
    // is negative in Lua)
    luabridge::getGlobalNamespace(L)
        .beginClass<ShaderPool>("ShaderPool")
        .addStaticFunction("warmUp",
            luabridge::overload<const std::string&>(+[](const std::string& spec) {
                ShaderKey key;
                return ShaderPool::parseShaderSpec(spec, key) && ShaderPool::warmUp(key);
            }),
            luabridge::overload<const std::string&, const std::string&>(+[](const std::string& spec, const std::string& customShader) {
                ShaderKey key;
                return ShaderPool::parseShaderSpec(spec, key, nullptr, customShader) && ShaderPool::warmUp(key);
            }),
            luabridge::overload<ShaderType, lua_Integer>(+[](ShaderType type, lua_Integer properties) {
                return ShaderPool::warmUp(type, static_cast<uint64_t>(properties));
            }),
            luabridge::overload<ShaderType, lua_Integer, const std::string&>(+[](ShaderType type, lua_Integer properties, const std::string& customShader) {
                return ShaderPool::warmUp(type, static_cast<uint64_t>(properties), ShaderPool::registerCustomShader(customShader));
            }))
        .addStaticFunction("isShaderReady",
            luabridge::overload<const std::string&>(+[](const std::string& spec) {
                ShaderKey key;
                return ShaderPool::parseShaderSpec(spec, key) && ShaderPool::getShaderState(key) == ResourceLoadState::Finished;
            }),
            luabridge::overload<ShaderType, lua_Integer>(+[](ShaderType type, lua_Integer properties) {
                return ShaderPool::isShaderReady(type, static_cast<uint64_t>(properties));
            }))
        .addStaticFunction("isShaderFailed", +[](const std::string& spec) {
            ShaderKey key;
            return !ShaderPool::parseShaderSpec(spec, key) || ShaderPool::getShaderState(key) == ResourceLoadState::Failed;
        })
        .addStaticFunction("warmUpAllShaders", &ShaderPool::warmUpAllShaders)
        .addStaticFunction("getPendingShaderCount", &ShaderPool::getPendingShaderCount)
        .addStaticFunction("isRuntimeCompilationSupported", &ShaderPool::isRuntimeCompilationSupported)
        .addStaticProperty("runtimeCompilation", &ShaderPool::isRuntimeCompilationEnabled, &ShaderPool::setRuntimeCompilationEnabled)
        .addStaticProperty("asyncCompilation", &ShaderPool::isAsyncCompilation, &ShaderPool::setAsyncCompilation)
        .endClass();

    // diagnostics: the pipelines renderable objects made and did not destroy yet
    luabridge::getGlobalNamespace(L)
        .beginClass<ObjectRender>("ObjectRender")
        .addStaticFunction("getLivePipelineCount", &ObjectRender::getLivePipelineCount)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<ShaderVariantCollection>("ShaderVariantCollection")
        .addConstructor<void (*) (void)>()
        .addFunction("add",
            luabridge::overload<ShaderVariantCollection*, const std::string&>(+[](ShaderVariantCollection* self, const std::string& spec) {
                return self->add(spec);
            }),
            luabridge::overload<ShaderVariantCollection*, const std::string&, const std::string&>(+[](ShaderVariantCollection* self, const std::string& spec, const std::string& customShader) {
                return self->add(spec, customShader);
            }),
            luabridge::overload<ShaderVariantCollection*, ShaderType, lua_Integer>(+[](ShaderVariantCollection* self, ShaderType type, lua_Integer properties) {
                return self->add(type, static_cast<uint64_t>(properties));
            }),
            luabridge::overload<ShaderVariantCollection*, ShaderType, lua_Integer, const std::string&>(+[](ShaderVariantCollection* self, ShaderType type, lua_Integer properties, const std::string& customShader) {
                return self->add(type, static_cast<uint64_t>(properties), customShader);
            }))
        .addFunction("addObject", +[](ShaderVariantCollection* self, EntityHandle* object) {
            return object ? self->addEntityVariants(object->getScene(), object->getEntity()) : (size_t)0;
        })
        .addFunction("addEntityVariants", &ShaderVariantCollection::addEntityVariants)
        .addFunction("remove",
            luabridge::overload<ShaderVariantCollection*, const std::string&>(+[](ShaderVariantCollection* self, const std::string& spec) {
                return self->remove(spec);
            }),
            luabridge::overload<ShaderVariantCollection*, const std::string&, const std::string&>(+[](ShaderVariantCollection* self, const std::string& spec, const std::string& customShader) {
                return self->remove(spec, customShader);
            }))
        .addFunction("contains",
            luabridge::overload<const ShaderVariantCollection*, const std::string&>(+[](const ShaderVariantCollection* self, const std::string& spec) {
                return self->contains(spec);
            }),
            luabridge::overload<const ShaderVariantCollection*, const std::string&, const std::string&>(+[](const ShaderVariantCollection* self, const std::string& spec, const std::string& customShader) {
                return self->contains(spec, customShader);
            }))
        .addFunction("clear", &ShaderVariantCollection::clear)
        .addProperty("variantCount", &ShaderVariantCollection::getVariantCount)
        .addFunction("warmUp", &ShaderVariantCollection::warmUp)
        .addFunction("warmUpProgressively", &ShaderVariantCollection::warmUpProgressively)
        .addProperty("isWarmedUp", &ShaderVariantCollection::isWarmedUp)
        .addProperty("warmedUpVariantCount", &ShaderVariantCollection::getWarmedUpVariantCount)
        .addProperty("failedVariantCount", &ShaderVariantCollection::getFailedVariantCount)
        .endClass();

    // The "no entity" sentinel, mirroring the C++ NULL_ENTITY macro. Scene:findEntity
    // returns this when no match is found.
    luabridge::getGlobalNamespace(L)
        .addVariable("NULL_ENTITY", static_cast<Entity>(NULL_ENTITY));

    luabridge::getGlobalNamespace(L)
        .beginClass<Engine>("Engine")

        .addStaticProperty("scene", &Engine::getScene, &Engine::setScene)
        .addStaticFunction("setScene", &Engine::setScene)
        .addStaticFunction("getScene", &Engine::getScene)
        .addStaticFunction("addSceneLayer", &Engine::addSceneLayer)
        .addStaticFunction("executeSceneOnce", &Engine::executeSceneOnce)
        .addStaticFunction("removeScene", &Engine::removeScene)
        .addStaticFunction("removeAllSceneLayers", &Engine::removeAllSceneLayers)
        .addStaticFunction("removeAllScenes", &Engine::removeAllScenes)
        .addStaticFunction("isSceneRunning", &Engine::isSceneRunning)
        .addStaticFunction("hasScenesToExecuteOnce", &Engine::hasScenesToExecuteOnce)
        .addStaticFunction("getMainScene", &Engine::getMainScene)
        .addStaticFunction("getLastScene", &Engine::getLastScene)
        .addStaticFunction("pauseGameEvents", &Engine::pauseGameEvents)
        .addStaticProperty("canvasWidth", &Engine::getCanvasWidth)
        .addStaticProperty("canvasHeight", &Engine::getCanvasHeight)
        .addStaticFunction("setCanvasSize", &Engine::setCanvasSize)
        .addStaticProperty("preferredCanvasWidth", &Engine::getPreferredCanvasWidth)
        .addStaticProperty("preferredCanvasHeight", &Engine::getPreferredCanvasHeight)
        .addStaticProperty("viewRect", &Engine::getViewRect)
        .addStaticProperty("scalingMode", &Engine::getScalingMode, &Engine::setScalingMode)
        .addStaticProperty("textureStrategy", &Engine::getTextureStrategy, &Engine::setTextureStrategy)
        .addStaticFunction("setAtmosphereQuality", &Engine::setAtmosphereQuality)
        .addStaticFunction("getAtmosphereQuality", &Engine::getAtmosphereQuality)
        .addStaticFunction("resetAtmosphereQuality", &Engine::resetAtmosphereQuality)
        .addStaticProperty("callMouseInTouchEvent", &Engine::isCallMouseInTouchEvent, &Engine::setCallMouseInTouchEvent)
        .addStaticProperty("callTouchInMouseEvent", &Engine::isCallTouchInMouseEvent, &Engine::setCallTouchInMouseEvent)
        .addStaticFunction("setCallTouchInMouseEvent", &Engine::setCallTouchInMouseEvent)
        .addStaticProperty("useDegrees", &Engine::isUseDegrees, &Engine::setUseDegrees)
        .addStaticProperty("allowEventsOutCanvas", &Engine::isAllowEventsOutCanvas, &Engine::setAllowEventsOutCanvas)
        .addStaticProperty("ignoreEventsHandledByUI", &Engine::isIgnoreEventsHandledByUI, &Engine::setIgnoreEventsHandledByUI)
        .addStaticFunction("isUIEventReceived", &Engine::isUIEventReceived)
        .addStaticFunction("wantsGamepadNavigation", &Engine::wantsGamepadNavigation)
        .addStaticProperty("updateTime", &Engine::getUpdateTime, &Engine::setUpdateTime)
        .addStaticFunction("setUpdateTimeMS", &Engine::setUpdateTimeMS)
        .addStaticProperty("interpolationAlpha", &Engine::getInterpolationAlpha)
        .addStaticProperty("mouseCursor", &Engine::getMouseCursor, &Engine::setMouseCursor)
        .addStaticProperty("mouseMode", &Engine::getMouseMode, &Engine::setMouseMode)
        .addStaticFunction("setMousePosition", &Engine::setMousePosition)
        .addStaticProperty("platform", &Engine::getPlatform)
        .addStaticProperty("graphicBackend", &Engine::getGraphicBackend)
        .addStaticProperty("openGL", &Engine::isOpenGL)
        .addStaticProperty("framerate", &Engine::getFramerate)
        .addStaticProperty("deltatime", &Engine::getDeltatime)
        .addStaticProperty("systemTime", &Engine::getSystemTime)
        .addStaticProperty("timeScale", &Engine::getTimeScale, &Engine::setTimeScale)
        .addStaticProperty("fixedDeltatime", &Engine::getFixedDeltatime, &Engine::setFixedDeltatime)
        .addStaticProperty("gameTime", &Engine::getGameTime)
        .addStaticProperty("frameCount", &Engine::getGameFrames)
        .addStaticProperty("asyncLoading", &Engine::isAsyncLoading, &Engine::setAsyncLoading)
        .addStaticFunction("startAsyncThread", &Engine::startAsyncThread)
        .addStaticFunction("commitThreadQueue", &Engine::commitThreadQueue)
        .addStaticFunction("endAsyncThread", &Engine::endAsyncThread)
        .addStaticFunction("isAsyncThread", &Engine::isAsyncThread)
        .addStaticFunction("isViewLoaded", &Engine::isViewLoaded)
        .addStaticFunction("setMaxResourceLoadingThreads", &Engine::setMaxResourceLoadingThreads)
        .addStaticFunction("getQueuedResourceCount", &Engine::getQueuedResourceCount)
        .addStaticProperty("framebuffer", &Engine::getFramebuffer, &Engine::setFramebuffer)
        .addStaticFunction("clearAllSubscriptions", &Engine::clearAllSubscriptions)
        .addStaticFunction("clearComponentSubscriptions", &Engine::clearComponentSubscriptions)

        .addStaticProperty("onViewLoaded", [] () { return &Engine::onViewLoaded; }, [] (lua_State* L) { Engine::onViewLoaded = L; })
        .addStaticProperty("onCanvasChanged", [] () { return &Engine::onViewChanged; }, [] (lua_State* L) { Engine::onViewChanged = L; })
        .addStaticProperty("onViewDestroyed", [] () { return &Engine::onViewDestroyed; }, [] (lua_State* L) { Engine::onViewDestroyed = L; })
        .addStaticProperty("onDraw", [] () { return &Engine::onDraw; }, [] (lua_State* L) { Engine::onDraw = L; })
        .addStaticProperty("onUpdate", [] () { return &Engine::onUpdate; }, [] (lua_State* L) { Engine::onUpdate = L; })
        .addStaticProperty("onFixedUpdate", [] () { return &Engine::onFixedUpdate; }, [] (lua_State* L) { Engine::onFixedUpdate = L; })
        .addStaticProperty("onPostUpdate", [] () { return &Engine::onPostUpdate; }, [] (lua_State* L) { Engine::onPostUpdate = L; })
        .addStaticProperty("onPause", [] () { return &Engine::onPause; }, [] (lua_State* L) { Engine::onPause = L; })
        .addStaticProperty("onResume", [] () { return &Engine::onResume; }, [] (lua_State* L) { Engine::onResume = L; })
        .addStaticProperty("onShutdown", [] () { return &Engine::onShutdown; }, [] (lua_State* L) { Engine::onShutdown = L; })
        .addStaticProperty("onSceneLoaded", [] () { return &Engine::onSceneLoaded; }, [] (lua_State* L) { Engine::onSceneLoaded = L; })
        .addStaticProperty("onTouchStart", [] () { return &Engine::onTouchStart; }, [] (lua_State* L) { Engine::onTouchStart = L; })
        .addStaticProperty("onTouchEnd", [] () { return &Engine::onTouchEnd; }, [] (lua_State* L) { Engine::onTouchEnd = L; })
        .addStaticProperty("onTouchMove", [] () { return &Engine::onTouchMove; }, [] (lua_State* L) { Engine::onTouchMove = L; })
        .addStaticProperty("onTouchCancel", [] () { return &Engine::onTouchCancel; }, [] (lua_State* L) { Engine::onTouchCancel = L; })
        .addStaticProperty("onMouseDown", [] () { return &Engine::onMouseDown; }, [] (lua_State* L) { Engine::onMouseDown = L; })
        .addStaticProperty("onMouseUp", [] () { return &Engine::onMouseUp; }, [] (lua_State* L) { Engine::onMouseUp = L; })
        .addStaticProperty("onMouseScroll", [] () { return &Engine::onMouseScroll; }, [] (lua_State* L) { Engine::onMouseScroll = L; })
        .addStaticProperty("onMouseMove", [] () { return &Engine::onMouseMove; }, [] (lua_State* L) { Engine::onMouseMove = L; })
        .addStaticProperty("onMouseEnter", [] () { return &Engine::onMouseEnter; }, [] (lua_State* L) { Engine::onMouseEnter = L; })
        .addStaticProperty("onMouseLeave", [] () { return &Engine::onMouseLeave; }, [] (lua_State* L) { Engine::onMouseLeave = L; })
        .addStaticProperty("onKeyDown", [] () { return &Engine::onKeyDown; }, [] (lua_State* L) { Engine::onKeyDown = L; })
        .addStaticProperty("onKeyUp", [] () { return &Engine::onKeyUp; }, [] (lua_State* L) { Engine::onKeyUp = L; })
        .addStaticProperty("onCharInput", [] () { return &Engine::onCharInput; }, [] (lua_State* L) { Engine::onCharInput = L; })
        .addStaticProperty("onGamepadConnect", [] () { return &Engine::onGamepadConnect; }, [] (lua_State* L) { Engine::onGamepadConnect = L; })
        .addStaticProperty("onGamepadDisconnect", [] () { return &Engine::onGamepadDisconnect; }, [] (lua_State* L) { Engine::onGamepadDisconnect = L; })
        .addStaticProperty("onGamepadButtonDown", [] () { return &Engine::onGamepadButtonDown; }, [] (lua_State* L) { Engine::onGamepadButtonDown = L; })
        .addStaticProperty("onGamepadButtonUp", [] () { return &Engine::onGamepadButtonUp; }, [] (lua_State* L) { Engine::onGamepadButtonUp = L; })
        .addStaticProperty("onGamepadAxisMove", [] () { return &Engine::onGamepadAxisMove; }, [] (lua_State* L) { Engine::onGamepadAxisMove = L; })

        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<PostProcessPass>("PostProcessPass")
        .addConstructor<void (*) (void)>()
        .addProperty("shader", &PostProcessPass::shader, &PostProcessPass::shader)
        .addProperty("enabled", &PostProcessPass::enabled, &PostProcessPass::enabled)
        .addFunction("setUniform",
            luabridge::overload<const std::string&, const Vector4&>(&PostProcessPass::setUniform),
            luabridge::overload<const std::string&, const Vector3&>(&PostProcessPass::setUniform),
            luabridge::overload<const std::string&, const Vector2&>(&PostProcessPass::setUniform),
            luabridge::overload<const std::string&, float>(&PostProcessPass::setUniform))
        .addFunction("getUniform", &PostProcessPass::getUniform)
        .addFunction("removeUniform", &PostProcessPass::removeUniform)
        // (phase 4) the pass's own textures by uniform, and its resolution scale
        .addFunction("setTexture", &PostProcessPass::setTexture)
        .addFunction("getTexture", &PostProcessPass::getTexture)
        .addProperty("scale", &PostProcessPass::scale, &PostProcessPass::scale)
        .endClass();

    // a copy: edit it, then assign it back (scene.postProcessing = post)
    luabridge::getGlobalNamespace(L)
        .beginClass<PostProcessingSettings>("PostProcessingSettings")
        .addConstructor<void (*) (void)>()
        .addProperty("exposure", &PostProcessingSettings::exposure, &PostProcessingSettings::exposure)
        .addProperty("tonemap", &PostProcessingSettings::tonemap, &PostProcessingSettings::tonemap)
        .addProperty("colorGradingEnabled", &PostProcessingSettings::colorGradingEnabled, &PostProcessingSettings::colorGradingEnabled)
        .addProperty("temperature", &PostProcessingSettings::temperature, &PostProcessingSettings::temperature)
        .addProperty("tint", &PostProcessingSettings::tint, &PostProcessingSettings::tint)
        .addProperty("contrast", &PostProcessingSettings::contrast, &PostProcessingSettings::contrast)
        .addProperty("saturation", &PostProcessingSettings::saturation, &PostProcessingSettings::saturation)
        .addProperty("lift", &PostProcessingSettings::lift, &PostProcessingSettings::lift)
        .addProperty("gamma", &PostProcessingSettings::gamma, &PostProcessingSettings::gamma)
        .addProperty("gain", &PostProcessingSettings::gain, &PostProcessingSettings::gain)
        .addProperty("lutTexture", &PostProcessingSettings::lutTexture, &PostProcessingSettings::lutTexture)
        .addProperty("lutContribution", &PostProcessingSettings::lutContribution, &PostProcessingSettings::lutContribution)
        .addProperty("lutSize", &PostProcessingSettings::lutSize, &PostProcessingSettings::lutSize)
        .addProperty("monochromeEnabled", &PostProcessingSettings::monochromeEnabled, &PostProcessingSettings::monochromeEnabled)
        .addProperty("monochromeMode", &PostProcessingSettings::monochromeMode, &PostProcessingSettings::monochromeMode)
        .addProperty("monochromeIntensity", &PostProcessingSettings::monochromeIntensity, &PostProcessingSettings::monochromeIntensity)
        .addProperty("vignetteEnabled", &PostProcessingSettings::vignetteEnabled, &PostProcessingSettings::vignetteEnabled)
        .addProperty("vignetteIntensity", &PostProcessingSettings::vignetteIntensity, &PostProcessingSettings::vignetteIntensity)
        .addProperty("vignetteSmoothness", &PostProcessingSettings::vignetteSmoothness, &PostProcessingSettings::vignetteSmoothness)
        .addProperty("vignetteRoundness", &PostProcessingSettings::vignetteRoundness, &PostProcessingSettings::vignetteRoundness)
        .addProperty("vignetteColor", &PostProcessingSettings::vignetteColor, &PostProcessingSettings::vignetteColor)
        .addProperty("ditherEnabled", &PostProcessingSettings::ditherEnabled, &PostProcessingSettings::ditherEnabled)
        .addProperty("fxaaEnabled", &PostProcessingSettings::fxaaEnabled, &PostProcessingSettings::fxaaEnabled)
        .addProperty("chromaticAberrationEnabled", &PostProcessingSettings::chromaticAberrationEnabled, &PostProcessingSettings::chromaticAberrationEnabled)
        .addProperty("chromaticAberrationIntensity", &PostProcessingSettings::chromaticAberrationIntensity, &PostProcessingSettings::chromaticAberrationIntensity)
        .addProperty("pixelateEnabled", &PostProcessingSettings::pixelateEnabled, &PostProcessingSettings::pixelateEnabled)
        .addProperty("pixelateSize", &PostProcessingSettings::pixelateSize, &PostProcessingSettings::pixelateSize)
        .addProperty("crtEnabled", &PostProcessingSettings::crtEnabled, &PostProcessingSettings::crtEnabled)
        .addProperty("crtScanlines", &PostProcessingSettings::crtScanlines, &PostProcessingSettings::crtScanlines)
        .addProperty("crtMask", &PostProcessingSettings::crtMask, &PostProcessingSettings::crtMask)
        .addProperty("crtCurvature", &PostProcessingSettings::crtCurvature, &PostProcessingSettings::crtCurvature)
        .addProperty("crtVignette", &PostProcessingSettings::crtVignette, &PostProcessingSettings::crtVignette)
        .addProperty("edgeDetectEnabled", &PostProcessingSettings::edgeDetectEnabled, &PostProcessingSettings::edgeDetectEnabled)
        .addProperty("edgeDetectThreshold", &PostProcessingSettings::edgeDetectThreshold, &PostProcessingSettings::edgeDetectThreshold)
        .addProperty("edgeDetectIntensity", &PostProcessingSettings::edgeDetectIntensity, &PostProcessingSettings::edgeDetectIntensity)
        .addProperty("edgeDetectColor", &PostProcessingSettings::edgeDetectColor, &PostProcessingSettings::edgeDetectColor)
        .addProperty("filmGrainEnabled", &PostProcessingSettings::filmGrainEnabled, &PostProcessingSettings::filmGrainEnabled)
        .addProperty("filmGrainIntensity", &PostProcessingSettings::filmGrainIntensity, &PostProcessingSettings::filmGrainIntensity)
        .addProperty("filmGrainResponse", &PostProcessingSettings::filmGrainResponse, &PostProcessingSettings::filmGrainResponse)
        .addProperty("sharpenEnabled", &PostProcessingSettings::sharpenEnabled, &PostProcessingSettings::sharpenEnabled)
        .addProperty("sharpenIntensity", &PostProcessingSettings::sharpenIntensity, &PostProcessingSettings::sharpenIntensity)
        .addProperty("bloomEnabled", &PostProcessingSettings::bloomEnabled, &PostProcessingSettings::bloomEnabled)
        .addProperty("bloomThreshold", &PostProcessingSettings::bloomThreshold, &PostProcessingSettings::bloomThreshold)
        .addProperty("bloomKnee", &PostProcessingSettings::bloomKnee, &PostProcessingSettings::bloomKnee)
        .addProperty("bloomIntensity", &PostProcessingSettings::bloomIntensity, &PostProcessingSettings::bloomIntensity)
        .addProperty("bloomTint", &PostProcessingSettings::bloomTint, &PostProcessingSettings::bloomTint)
        .addProperty("bloomScatter", &PostProcessingSettings::bloomScatter, &PostProcessingSettings::bloomScatter)
        .addProperty("blurEnabled", &PostProcessingSettings::blurEnabled, &PostProcessingSettings::blurEnabled)
        .addProperty("blurRadius", &PostProcessingSettings::blurRadius, &PostProcessingSettings::blurRadius)
        .addProperty("blurIntensity", &PostProcessingSettings::blurIntensity, &PostProcessingSettings::blurIntensity)
        .addProperty("renderScale", &PostProcessingSettings::renderScale, &PostProcessingSettings::renderScale)
        .addProperty("msaaSamples", &PostProcessingSettings::msaaSamples, &PostProcessingSettings::msaaSamples)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<SceneManager>("SceneManager")
        .addStaticFunction("registerScene",
            luabridge::overload<uint32_t, const std::string&, std::function<void()>>(&SceneManager::registerScene),
            luabridge::overload<uint32_t, const std::string&, std::function<void()>, std::function<void()>>(&SceneManager::registerScene))
        .addStaticFunction("loadScene", 
            luabridge::overload<uint32_t>(&SceneManager::loadScene),
            luabridge::overload<const std::string&>(&SceneManager::loadScene))
        .addStaticFunction("addChildScene",
            luabridge::overload<uint32_t>(&SceneManager::addChildScene),
            luabridge::overload<const std::string&>(&SceneManager::addChildScene))
        .addStaticFunction("removeChildScene",
            luabridge::overload<uint32_t>(&SceneManager::removeChildScene),
            luabridge::overload<const std::string&>(&SceneManager::removeChildScene))
        .addStaticFunction("getSceneId", &SceneManager::getSceneId)
        .addStaticFunction("getSceneName", &SceneManager::getSceneName)
        .addStaticFunction("getSceneNames", &SceneManager::getSceneNames)
        .addStaticProperty("sceneCount", &SceneManager::getSceneCount)
        .addStaticProperty("currentSceneId", &SceneManager::getCurrentSceneId)
        .addStaticProperty("currentSceneName", &SceneManager::getCurrentSceneName)
        .addStaticProperty("loadPending", &SceneManager::isLoadPending)
        .addStaticFunction("setLoadingScene",
            luabridge::overload<uint32_t>(&SceneManager::setLoadingScene),
            luabridge::overload<const std::string&>(&SceneManager::setLoadingScene))
        .addStaticProperty("loadingSceneId", &SceneManager::getLoadingSceneId)
        .addStaticProperty("loadingDelay", &SceneManager::getLoadingDelay, &SceneManager::setLoadingDelay)
        .addStaticProperty("loadingTimeout", &SceneManager::getLoadingTimeout, &SceneManager::setLoadingTimeout)
        .addStaticProperty("loading", &SceneManager::isLoading)
        .addStaticProperty("loadingProgress", &SceneManager::getLoadingProgress)
        .addStaticFunction("holdLoading", &SceneManager::holdLoading)
        .addStaticFunction("releaseLoading", &SceneManager::releaseLoading)
        .addStaticFunction("isCoveredByLoading", &SceneManager::isCoveredByLoading)
        .addStaticFunction("preloadScene",
            luabridge::overload<uint32_t>(&SceneManager::preloadScene),
            luabridge::overload<const std::string&>(&SceneManager::preloadScene))
        .addStaticFunction("getPreloadProgress",
            luabridge::overload<uint32_t>(&SceneManager::getPreloadProgress),
            luabridge::overload<const std::string&>(&SceneManager::getPreloadProgress))
        .addStaticFunction("cancelPreload",
            luabridge::overload<uint32_t>(&SceneManager::cancelPreload),
            luabridge::overload<const std::string&>(&SceneManager::cancelPreload))
        .addStaticFunction("clearAll", &SceneManager::clearAll)
        .addStaticFunction("setScenePtr", &SceneManager::setScenePtr)
        .addStaticFunction("getScenePtr", &SceneManager::getScenePtr)
        .addStaticFunction("removeScenePtr", &SceneManager::removeScenePtr)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<PrefabManager>("PrefabManager")
        .addStaticFunction("findPrefab", &PrefabManager::findPrefab)
        .addStaticFunction("getPrefabId", &PrefabManager::getPrefabId)
        .addStaticFunction("getPrefabName", &PrefabManager::getPrefabName)
        .addStaticFunction("getPrefabGuid", &PrefabManager::getPrefabGuid)
        .addStaticFunction("getPrefabNames", &PrefabManager::getPrefabNames)
        .addStaticProperty("prefabCount", &PrefabManager::getPrefabCount)
        .endClass();

    // What a Prefab script property holds; GameObject.instantiate takes it
    luabridge::getGlobalNamespace(L)
        .beginClass<PrefabAsset>("PrefabAsset")
        .addConstructor<void (*) (), void (*) (const std::string&, const std::string&)>()
        // Lua reads a number as a string too, so the id overload comes first
        .addStaticFunction("load",
            luabridge::overload<uint32_t>(&PrefabAsset::load),
            luabridge::overload<const std::string&>(&PrefabAsset::load))
        .addProperty("id", &PrefabAsset::getId)
        .addProperty("valid", &PrefabAsset::isValid)
        .addProperty("guid", [](const PrefabAsset* self) { return self->getGuid(); })
        .addProperty("path", [](const PrefabAsset* self) { return self->getPath(); })
        .addProperty("name", &PrefabAsset::getName)
        .addFunction("isValid", &PrefabAsset::isValid)
        .addFunction("__eq", &PrefabAsset::operator==)
        .addFunction("__tostring", [](const PrefabAsset* self) { return "PrefabAsset(" + self->getPath() + ")"; })
        .endClass();

    // Asset identity (S04): what an Asset script property holds, and Asset.ref returns
    luabridge::getGlobalNamespace(L)
        .beginClass<ScriptAssetRef>("AssetRef")
        .addProperty("guid", &ScriptAssetRef::getGuid)
        .addProperty("path", &ScriptAssetRef::getPath)
        .addProperty("type", &ScriptAssetRef::getType)
        .addProperty("valid", &ScriptAssetRef::isValid)
        .addFunction("isValid", &ScriptAssetRef::isValid)
        .addFunction("__eq", &ScriptAssetRef::operator==)
        .addFunction("__tostring", [](const ScriptAssetRef* self) { return "AssetRef(" + self->getPath() + ")"; })
        .endClass();

    // Asset.path(guid) -> string|nil, Asset.guid(path) -> string|nil,
    // Asset.exists(guidOrPath) -> bool, Asset.ref(guidOrPath) -> AssetRef
    luabridge::getGlobalNamespace(L)
        .beginNamespace("Asset")
        .addFunction("path", +[](lua_State* state) -> int {
            const char* text = luaL_checkstring(state, 1);
            std::string path;
            if (Guid::isValidText(text) && AssetMap::findPath(Guid::parse(text), path)) {
                lua_pushstring(state, path.c_str());
            } else {
                lua_pushnil(state);
            }
            return 1;
        })
        .addFunction("guid", +[](lua_State* state) -> int {
            const Guid guid = AssetMap::findGuid(luaL_checkstring(state, 1));
            if (guid.isNull()) {
                lua_pushnil(state);
            } else {
                lua_pushstring(state, guid.str().c_str());
            }
            return 1;
        })
        .addFunction("exists", +[](lua_State* state) -> int {
            lua_pushboolean(state, ScriptAssetRef::make(luaL_checkstring(state, 1)).isValid());
            return 1;
        })
        .addFunction("ref", +[](lua_State* state) -> int {
            if (!luabridge::push<ScriptAssetRef>(state, ScriptAssetRef::make(luaL_checkstring(state, 1)))) {
                lua_pushnil(state);
            }
            return 1;
        })
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void()>>("FunctionSubscribe_V")
        .addFunction("__call", &FunctionSubscribe<void()>::call)
        .addFunction("call", &FunctionSubscribe<void()>::call)
        .addFunction("add", (bool (FunctionSubscribe<void()>::*)(const std::string&, lua_State*))&FunctionSubscribe<void()>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(float)>>("FunctionSubscribe_V_F")
        .addFunction("__call", &FunctionSubscribe<void(float)>::call)
        .addFunction("call", &FunctionSubscribe<void(float)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(float)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(float)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(float,float)>>("FunctionSubscribe_V_FF")
        .addFunction("__call", &FunctionSubscribe<void(float,float)>::call)
        .addFunction("call", &FunctionSubscribe<void(float,float)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(float,float)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(float,float)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(float,float,int)>>("FunctionSubscribe_V_FFI")
        .addFunction("__call", &FunctionSubscribe<void(float,float,int)>::call)
        .addFunction("call", &FunctionSubscribe<void(float,float,int)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(float,float,int)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(float,float,int)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int)>>("FunctionSubscribe_V_I")
        .addFunction("__call", &FunctionSubscribe<void(int)>::call)
        .addFunction("call", &FunctionSubscribe<void(int)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int,int)>>("FunctionSubscribe_V_II")
        .addFunction("__call", &FunctionSubscribe<void(int,int)>::call)
        .addFunction("call", &FunctionSubscribe<void(int,int)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int,int)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int,int)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int,int,float)>>("FunctionSubscribe_V_IIF")
        .addFunction("__call", &FunctionSubscribe<void(int,int,float)>::call)
        .addFunction("call", &FunctionSubscribe<void(int,int,float)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int,int,float)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int,int,float)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int,float,float)>>("FunctionSubscribe_V_IFF")
        .addFunction("__call", &FunctionSubscribe<void(int,float,float)>::call)
        .addFunction("call", &FunctionSubscribe<void(int,float,float)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int,float,float)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int,float,float)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int,float,float,int)>>("FunctionSubscribe_V_IFFI")
        .addFunction("__call", &FunctionSubscribe<void(int,float,float,int)>::call)
        .addFunction("call", &FunctionSubscribe<void(int,float,float,int)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int,float,float,int)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int,float,float,int)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(int,bool,int)>>("FunctionSubscribe_V_IBI")
        .addFunction("__call", &FunctionSubscribe<void(int,bool,int)>::call)
        .addFunction("call", &FunctionSubscribe<void(int,bool,int)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(int,bool,int)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(int,bool,int)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(wchar_t)>>("FunctionSubscribe_V_W")
        .addFunction("__call", &FunctionSubscribe<void(wchar_t)>::call)
        .addFunction("call", &FunctionSubscribe<void(wchar_t)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(wchar_t)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(wchar_t)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<float(float)>>("FunctionSubscribe_F_F")
        .addFunction("__call", &FunctionSubscribe<float(float)>::call)
        .addFunction("call", &FunctionSubscribe<float(float)>::call)
        .addFunction("add", (bool (FunctionSubscribe<float(float)>::*)(const std::string&, lua_State*))&FunctionSubscribe<float(float)>::add)
        .endClass();

#ifdef JASPERENGINE_PHYSICS_2D
    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>>("FunctionSubscribe_V_B2ULB2UL")
        .addFunction("__call", &FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::call)
        .addFunction("call", &FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)>>("FunctionSubscribe_V_B2ULB2ULV2V2F")
        .addFunction("__call", &FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)>::call)
        .addFunction("call", &FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Vector2, Vector2, float)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>>("FunctionSubscribe_B_B2ULB2UL")
        .addFunction("__call", &FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::call)
        .addFunction("call", &FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::call)
        .addFunction("add", (bool (FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::*)(const std::string&, lua_State*))&FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)>>("FunctionSubscribe_B_B2ULB2ULM2")
        .addFunction("__call", &FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)>::call)
        .addFunction("call", &FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)>::call)
        .addFunction("add", (bool (FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)>::*)(const std::string&, lua_State*))&FunctionSubscribe<bool(Rigidbody2D, unsigned long, Rigidbody2D, unsigned long, Manifold2D)>::add)
        .endClass();
#endif

#ifdef JASPERENGINE_PHYSICS_3D
    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(Rigidbody)>>("FunctionSubscribe_V_B3")
        .addFunction("__call", &FunctionSubscribe<void(Rigidbody)>::call)
        .addFunction("call", &FunctionSubscribe<void(Rigidbody)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(Rigidbody)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(Rigidbody)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>>("FunctionSubscribe_V_B3B3C3")
        .addFunction("__call", &FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>::call)
        .addFunction("call", &FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(Rigidbody, Rigidbody, Contact3D)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>>("FunctionSubscribe_V_B3B3ULUL")
        .addFunction("__call", &FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>::call)
        .addFunction("call", &FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>::call)
        .addFunction("add", (bool (FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>::*)(const std::string&, lua_State*))&FunctionSubscribe<void(Rigidbody, Rigidbody, unsigned long, unsigned long)>::add)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>>("FunctionSubscribe_B_B3B3V3CR3")
        .addFunction("__call", &FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>::call)
        .addFunction("call", &FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>::call)
        .addFunction("add", (bool (FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>::*)(const std::string&, lua_State*))&FunctionSubscribe<bool(Rigidbody, Rigidbody, Vector3, CollideShapeResult3D)>::add)
        .endClass();
#endif

    luabridge::getGlobalNamespace(L)
        .beginClass<Texture>("Texture")
        .addConstructor <void (*) (void), void (*) (const std::string&), void (*) (const std::string&, TextureData), void (*) (Framebuffer*)> ()
        .addFunction("setPath", &Texture::setPath)
        .addFunction("setData", &Texture::setData)
        .addFunction("setId", &Texture::setId)
        .addFunction("setCubePath", &Texture::setCubePath)
        .addFunction("setCubePaths", &Texture::setCubePaths)
        .addFunction("setFramebuffer", &Texture::setFramebuffer)
        .addFunction("load", &Texture::load)
        .addFunction("destroy", &Texture::destroy)
        .addFunction("getRender", &Texture::getRender)
        .addFunction("getPath", &Texture::getPath)
        .addFunction("getData", &Texture::getData)
        .addFunction("getId", &Texture::getId)
        .addFunction("getNumFaces", &Texture::getNumFaces)
        .addFunction("getType", &Texture::getType)
        .addFunction("isCubeMap", &Texture::isCubeMap)
        .addFunction("getWidth", &Texture::getWidth)
        .addFunction("getHeight", &Texture::getHeight)
        .addFunction("isTransparent", &Texture::isTransparent)
        .addProperty("releaseDataAfterLoad", &Texture::isReleaseDataAfterLoad, &Texture::setReleaseDataAfterLoad)
        .addFunction("releaseData", &Texture::releaseData)
        .addFunction("empty", &Texture::empty)
        .addFunction("isFramebuffer", &Texture::isFramebuffer)
        .addFunction("isFramebufferOutdated", &Texture::isFramebufferOutdated)
        .addProperty("minFilter", &Texture::getMinFilter, &Texture::setMinFilter)
        .addProperty("magFilter", &Texture::getMagFilter, &Texture::setMagFilter)
        .addProperty("wrapU", &Texture::getWrapU, &Texture::setWrapU)
        .addProperty("wrapV", &Texture::getWrapV, &Texture::setWrapV)
        .addProperty("svgScale", &Texture::getSvgScale, &Texture::setSvgScale)
        .addProperty("alphaBorder", &Texture::getAlphaBorder, &Texture::setAlphaBorder)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<TextureData>("TextureData")
        .addConstructor<
            void(), 
            void(int, int, unsigned int, ColorFormat, int, void*), 
            void(const char*)>()
        .addFunction("loadTextureFromFile", &TextureData::loadTextureFromFile)
        .addFunction("releaseImageData", &TextureData::releaseImageData)
        .addFunction("hasAlpha", &TextureData::hasAlpha)
        .addFunction("resizePowerOfTwo", &TextureData::resizePowerOfTwo)
        .addFunction("fitPowerOfTwo", &TextureData::fitPowerOfTwo)
        .addFunction("resize", &TextureData::resize)
        .addFunction("crop", &TextureData::crop)
        .addFunction("fitSize", &TextureData::fitSize)
        .addFunction("flipVertical", &TextureData::flipVertical)
        .addFunction("getColorComponent", &TextureData::getColorComponent)
        .addProperty("dataOwned", &TextureData::getDataOwned, &TextureData::setDataOwned)
        .addProperty("svgScale", &TextureData::getSVGScale, &TextureData::setSVGScale)
        .addFunction("getWidth", &TextureData::getWidth)
        .addFunction("getHeight", &TextureData::getHeight)
        .addFunction("getOriginalWidth", &TextureData::getOriginalWidth)
        .addFunction("getOriginalHeight", &TextureData::getOriginalHeight)
        .addFunction("getSize", &TextureData::getSize)
        .addFunction("getColorFormat", &TextureData::getColorFormat)
        .addFunction("getChannels", &TextureData::getChannels)
        .addFunction("getData", &TextureData::getData)
        .addFunction("isTransparent", &TextureData::isTransparent)
        .addFunction("getMinNearestPowerOfTwo", &TextureData::getMinNearestPowerOfTwo)
        .endClass();

    luabridge::getGlobalNamespace(L)
        // A load result, read-only on purpose
        .beginClass<TextureLoadResult>("TextureLoadResult")
        .addConstructor<void()>()
        .addProperty("id", &TextureLoadResult::id)
        .addProperty("state", &TextureLoadResult::state)
        .addProperty("errorMessage", &TextureLoadResult::errorMessage)
        .addProperty("data", &TextureLoadResult::data)
        .addFunction("__tostring", [] (const TextureLoadResult& result) {
            return "TextureLoadResult(id: " + result.id + ", state: " + std::to_string(static_cast<int>(result.state)) + ")";
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        // the stencil state of a Material (material.stencil.compare = ... edits the material's
        // own) or of a UI/points/lines object (a copy: set it back)
        .beginClass<MaterialStencil>("MaterialStencil")
        .addConstructor<void()>()
        .addProperty("compare", &MaterialStencil::compare, &MaterialStencil::compare)
        .addProperty("ref", &MaterialStencil::ref, &MaterialStencil::ref)
        .addProperty("readMask", &MaterialStencil::readMask, &MaterialStencil::readMask)
        .addProperty("writeMask", &MaterialStencil::writeMask, &MaterialStencil::writeMask)
        .addProperty("pass", &MaterialStencil::pass, &MaterialStencil::pass)
        .addProperty("fail", &MaterialStencil::fail, &MaterialStencil::fail)
        .addProperty("depthFail", &MaterialStencil::depthFail, &MaterialStencil::depthFail)
        .endClass();

    luabridge::getGlobalNamespace(L)
        // A Material's extra draw (material.extraPass, edited in place on the Material copy,
        // then mesh:setMaterial(m)): its own shader, parameters, textures, features and state.
        // An empty shader means no extra pass.
        .beginClass<MaterialPass>("MaterialPass")
        .addConstructor<void (*) (void)>()
        .addProperty("shader", &MaterialPass::shader, &MaterialPass::shader)
        .addProperty("blendMode", &MaterialPass::blendMode, &MaterialPass::blendMode)
        .addProperty("cullMode", &MaterialPass::cullMode, &MaterialPass::cullMode)
        .addProperty("depthWrite", &MaterialPass::depthWrite, &MaterialPass::depthWrite)
        .addProperty("depthTest", &MaterialPass::depthTest, &MaterialPass::depthTest)
        .addProperty("depthOffsetFactor", &MaterialPass::depthOffsetFactor, &MaterialPass::depthOffsetFactor)
        .addProperty("depthOffsetUnits", &MaterialPass::depthOffsetUnits, &MaterialPass::depthOffsetUnits)
        .addProperty("stencil", &MaterialPass::stencil, &MaterialPass::stencil)
        // MaterialColorWrite.AUTO (-1), NONE (0) or a sum of R, G, B, A
        .addProperty("colorWrite", &MaterialPass::colorWrite, &MaterialPass::colorWrite)
        .addProperty("order", &MaterialPass::order, &MaterialPass::order)
        // a @property value of the pass's shader (colours linear); unset reads the manifest default
        .addFunction("setShaderParam",
            luabridge::overload<MaterialPass*, const std::string&, const Vector4&>(+[](MaterialPass* self, const std::string& name, const Vector4& value) {
                ShaderUniforms::set(self->shaderParams, name, value);
            }),
            luabridge::overload<MaterialPass*, const std::string&, const Vector3&>(+[](MaterialPass* self, const std::string& name, const Vector3& value) {
                // a colour given as (r, g, b) is opaque
                ShaderUniforms::set(self->shaderParams, name, LuaBinding::getShaderParamValue(self->shader, name, value));
            }),
            luabridge::overload<MaterialPass*, const std::string&, const Vector2&>(+[](MaterialPass* self, const std::string& name, const Vector2& value) {
                ShaderUniforms::set(self->shaderParams, name, Vector4(value.x, value.y, 0.0f, 0.0f));
            }),
            luabridge::overload<MaterialPass*, const std::string&, float>(+[](MaterialPass* self, const std::string& name, float value) {
                ShaderUniforms::set(self->shaderParams, name, Vector4(value, 0.0f, 0.0f, 0.0f));
            }))
        .addFunction("getShaderParam", +[](const MaterialPass* self, const std::string& name) -> Vector4 {
            return ShaderLibraryValues::getParam(self->shaderParams, self->shader, name);
        })
        .addFunction("removeShaderParam", +[](MaterialPass* self, const std::string& name) -> bool {
            return ShaderUniforms::remove(self->shaderParams, name);
        })
        // the texture of a @texture uniform by path ("" removes it: the manifest default binds)
        .addFunction("setShaderTexture", +[](MaterialPass* self, const std::string& name, const std::string& path) {
            ShaderLibraryValues::setTexture(self->shaderTextures, name, path);
        })
        // a cube map (a @texture ... cube uniform): one image of the six faces, or six images
        .addFunction("setShaderCubeTexture",
            luabridge::overload<MaterialPass*, const std::string&, const std::string&>(+[](MaterialPass* self, const std::string& name, const std::string& path) {
                ShaderLibraryValues::setCubeTexture(self->shaderTextures, name, path);
            }),
            luabridge::overload<MaterialPass*, const std::string&, const std::string&, const std::string&, const std::string&,
                                const std::string&, const std::string&, const std::string&>(+[](MaterialPass* self, const std::string& name,
                    const std::string& front, const std::string& back, const std::string& left, const std::string& right, const std::string& up, const std::string& down) {
                ShaderLibraryValues::setCubeTextureFaces(self->shaderTextures, name, front, back, left, right, up, down);
            }))
        .addFunction("getShaderTexture", +[](const MaterialPass* self, const std::string& name) -> std::string {
            return ShaderLibraryValues::getTexture(self->shaderTextures, name);
        })
        // a @feature on or off, or a @keywords value ("GROUP_VALUE") chosen
        .addFunction("setShaderFeature", +[](MaterialPass* self, const std::string& define, bool enabled) {
            ShaderLibraryValues::setFeature(self->shaderFeatures, self->shader, define, enabled);
        })
        .addFunction("isShaderFeatureEnabled", +[](const MaterialPass* self, const std::string& define) -> bool {
            return ShaderLibraryValues::isFeatureEnabled(self->shaderFeatures, self->shader, define);
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        // Mesh::getMaterial hands back a copy: the values are written on it, then the copy is
        // set back (m.roughnessFactor = 0.2; mesh:setMaterial(m)). Textures stay read-only.
        .beginClass<Material>("Material")
        .addProperty("baseColorFactor", &Material::baseColorFactor, &Material::baseColorFactor)
        .addProperty("metallicFactor", &Material::metallicFactor, &Material::metallicFactor)
        .addProperty("roughnessFactor", &Material::roughnessFactor, &Material::roughnessFactor)
        .addProperty("alphaMode", &Material::alphaMode, &Material::alphaMode)
        .addProperty("lighting", &Material::lighting, &Material::lighting)
        .addProperty("alphaCutoff", &Material::alphaCutoff, &Material::alphaCutoff)
        .addProperty("foliageTransmission", &Material::foliageTransmission, &Material::foliageTransmission)
        .addProperty("emissiveFactor", &Material::emissiveFactor, &Material::emissiveFactor)
        // Stochastic Tiling of the built-in Lit's maps (no repetition); the four values shape it
        .addProperty("stochasticTiling", &Material::stochasticTiling, &Material::stochasticTiling)
        .addProperty("stochasticCellSize", &Material::stochasticCellSize, &Material::stochasticCellSize)
        .addProperty("stochasticBlendSharpness", &Material::stochasticBlendSharpness, &Material::stochasticBlendSharpness)
        .addProperty("stochasticContrast", &Material::stochasticContrast, &Material::stochasticContrast)
        .addProperty("stochasticRotation", &Material::stochasticRotation, &Material::stochasticRotation)
        .addProperty("baseColorTexture", &Material::baseColorTexture)
        .addProperty("emissiveTexture", &Material::emissiveTexture)
        .addProperty("metallicRoughnessTexture", &Material::metallicRoughnessTexture)
        .addProperty("occlusionTexture", &Material::occlusionTexture)
        .addProperty("normalTexture", &Material::normalTexture)
        // shader library, writable on the copy getMaterial gives (then setMaterial it):
        // shader is "" (built-in Lit), "library:<name>" or a project fork
        .addProperty("shader", &Material::shader, &Material::shader)
        .addProperty("blendMode", &Material::blendMode, &Material::blendMode)
        .addProperty("cullMode", &Material::cullMode, &Material::cullMode)
        .addProperty("depthWrite", &Material::depthWrite, &Material::depthWrite)
        .addProperty("depthTest", &Material::depthTest, &Material::depthTest)
        .addProperty("depthOffsetFactor", &Material::depthOffsetFactor, &Material::depthOffsetFactor)
        .addProperty("depthOffsetUnits", &Material::depthOffsetUnits, &Material::depthOffsetUnits)
        .addProperty("sortPriority", &Material::sortPriority, &Material::sortPriority)
        .addProperty("stencil", &Material::stencil, &Material::stencil)
        .addProperty("colorWrite", &Material::colorWrite, &Material::colorWrite)
        // one optional extra draw of the submesh (outlines, masks), edited in place:
        // m.extraPass.shader = "library:outline"; mesh:setMaterial(m)
        .addProperty("extraPass",
            // the material's own pass (made on first access): m.extraPass.order = ... edits it
            +[](const Material* self) -> MaterialPass* { return &const_cast<Material*>(self)->extraPass.edit(); },
            +[](Material* self, const MaterialPass& pass) { self->extraPass = pass; })
        // a @property value of the shader (colours linear); unset reads the manifest default.
        // The shader is the material's, else the one its submesh draws with (a copy from
        // Mesh:getMaterial remembers it: the mesh's customShader or the scene default).
        .addFunction("setShaderParam",
            luabridge::overload<Material*, const std::string&, const Vector4&>(+[](Material* self, const std::string& name, const Vector4& value) {
                ShaderUniforms::set(self->shaderParams, name, value);
            }),
            luabridge::overload<Material*, const std::string&, const Vector3&, lua_State*>(+[](Material* self, const std::string& name, const Vector3& value, lua_State* L) {
                // a colour given as (r, g, b) is opaque
                const std::string shader = self->shader.empty() ? LuaBinding::getMaterialDrawnShader(L, 1) : self->shader.path;
                ShaderUniforms::set(self->shaderParams, name, LuaBinding::getShaderParamValue(shader, name, value));
            }),
            luabridge::overload<Material*, const std::string&, const Vector2&>(+[](Material* self, const std::string& name, const Vector2& value) {
                ShaderUniforms::set(self->shaderParams, name, Vector4(value.x, value.y, 0.0f, 0.0f));
            }),
            luabridge::overload<Material*, const std::string&, float>(+[](Material* self, const std::string& name, float value) {
                ShaderUniforms::set(self->shaderParams, name, Vector4(value, 0.0f, 0.0f, 0.0f));
            }))
        .addFunction("getShaderParam", +[](const Material* self, const std::string& name, lua_State* L) -> Vector4 {
            const std::string shader = self->shader.empty() ? LuaBinding::getMaterialDrawnShader(L, 1) : self->shader.path;
            return ShaderLibraryValues::getParam(self->shaderParams, shader, name);
        })
        .addFunction("removeShaderParam", +[](Material* self, const std::string& name) -> bool {
            return ShaderUniforms::remove(self->shaderParams, name);
        })
        // the texture of a @texture uniform by path ("" removes it: the manifest default binds)
        .addFunction("setShaderTexture", +[](Material* self, const std::string& name, const std::string& path) {
            ShaderLibraryValues::setTexture(self->shaderTextures, name, path);
        })
        // a cube map (a @texture ... cube uniform): one image of the six faces, or six images
        .addFunction("setShaderCubeTexture",
            luabridge::overload<Material*, const std::string&, const std::string&>(+[](Material* self, const std::string& name, const std::string& path) {
                ShaderLibraryValues::setCubeTexture(self->shaderTextures, name, path);
            }),
            luabridge::overload<Material*, const std::string&, const std::string&, const std::string&, const std::string&,
                                const std::string&, const std::string&, const std::string&>(+[](Material* self, const std::string& name,
                    const std::string& front, const std::string& back, const std::string& left, const std::string& right, const std::string& up, const std::string& down) {
                ShaderLibraryValues::setCubeTextureFaces(self->shaderTextures, name, front, back, left, right, up, down);
            }))
        .addFunction("getShaderTexture", +[](const Material* self, const std::string& name) -> std::string {
            return ShaderLibraryValues::getTexture(self->shaderTextures, name);
        })
        // a @feature on or off, or a @keywords value ("GROUP_VALUE") chosen
        .addFunction("setShaderFeature", +[](Material* self, const std::string& define, bool enabled, lua_State* L) {
            const std::string shader = self->shader.empty() ? LuaBinding::getMaterialDrawnShader(L, 1) : self->shader.path;
            ShaderLibraryValues::setFeature(self->shaderFeatures, shader, define, enabled);
        })
        .addFunction("isShaderFeatureEnabled", +[](const Material* self, const std::string& define, lua_State* L) -> bool {
            const std::string shader = self->shader.empty() ? LuaBinding::getMaterialDrawnShader(L, 1) : self->shader.path;
            return ShaderLibraryValues::isFeatureEnabled(self->shaderFeatures, shader, define);
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Framebuffer>("Framebuffer")
        .addConstructor <void (*) (void)> ()
        .addFunction("create", luabridge::overload<>(&Framebuffer::create))
        .addFunction("destroy", &Framebuffer::destroy)
        .addFunction("isCreated", &Framebuffer::isCreated)
        .addFunction("getRender", &Framebuffer::getRender)
        .addFunction("getVersion", &Framebuffer::getVersion)
        .addFunction("create", luabridge::overload<>(&Framebuffer::create))
        .addFunction("width", &Framebuffer::getWidth, &Framebuffer::setWidth)
        .addFunction("height", &Framebuffer::getHeight, &Framebuffer::setHeight)
        .addProperty("minFilter", &Framebuffer::getMinFilter, &Framebuffer::setMinFilter)
        .addProperty("magFilter", &Framebuffer::getMagFilter, &Framebuffer::setMagFilter)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<TextureRender>("TextureRender")
        .addConstructor <void (*) (void)> ()
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<FramebufferRender>("FramebufferRender")
        .addConstructor <void (*) (void)> ()
        .addFunction("createFramebuffer", luabridge::overload<TextureType, int, int, TextureFilter, TextureFilter, TextureWrap, TextureWrap, bool>(&FramebufferRender::createFramebuffer))
        .addFunction("destroyFramebuffer", &FramebufferRender::destroyFramebuffer)
        .addFunction("isCreated", &FramebufferRender::isCreated)
        .addFunction("getColorTexture", &FramebufferRender::getColorTexture)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<EntityRegistry>("EntityRegistry")
        .addConstructor <void (*) (void), void (*) (EntityPool)> ()
        .addFunction("createEntity", &EntityRegistry::createEntity)
        .addFunction("createUserEntity", &EntityRegistry::createUserEntity)
        .addFunction("createSystemEntity", &EntityRegistry::createSystemEntity)
        .addFunction("recreateEntity", &EntityRegistry::recreateEntity)
        .addFunction("isEntityCreated", &EntityRegistry::isEntityCreated)
        .addFunction("destroyEntity", &EntityRegistry::destroyEntity)
        .addFunction("setEntityName", &EntityRegistry::setEntityName)
        .addFunction("getEntityName", &EntityRegistry::getEntityName)
        .addFunction("findEntity",
            luabridge::constOverload<const std::string&>(&EntityRegistry::findEntity),
            luabridge::constOverload<const std::string&, Entity>(&EntityRegistry::findEntity))
        .addFunction("getSignature", &EntityRegistry::getSignature)
        .addFunction("setEntityActive", &EntityRegistry::setEntityActive)
        .addFunction("isEntityActive", &EntityRegistry::isEntityActive)
        .addFunction("isEntityActiveInHierarchy", &EntityRegistry::isEntityActiveInHierarchy)
        // scripts get Unity's check; loaders and generated scenes call the C++ setter directly
        .addFunction("setEntityTag", [] (EntityRegistry* self, Entity entity, const std::string& tag) {
            if (!TagManager::isTagDefined(tag)){
                Log::error("Tag: %s is not defined.", tag.c_str());
                return;
            }
            self->setEntityTag(entity, tag);
        })
        .addFunction("getEntityTag", &EntityRegistry::getEntityTag)
        .addFunction("setEntityLayer", &EntityRegistry::setEntityLayer)
        .addFunction("getEntityLayer", &EntityRegistry::getEntityLayer)
        .addFunction("findEntityByTag", &EntityRegistry::findEntityByTag)
        .addFunction("findEntitiesByTag", &EntityRegistry::findEntitiesByTag)
        // Unity-style names: GameObject.FindWithTag / FindGameObjectsWithTag
        .addFunction("findByTag", &EntityRegistry::findEntityByTag)
        .addFunction("findAllByTag", &EntityRegistry::findEntitiesByTag)
        .addFunction("addEntityChild", &EntityRegistry::addEntityChild)
        .addFunction("moveChildToIndex", &EntityRegistry::moveChildToIndex)
        .addFunction("moveChildToTop", &EntityRegistry::moveChildToTop)
        .addFunction("moveChildUp", &EntityRegistry::moveChildUp)
        .addFunction("moveChildDown", &EntityRegistry::moveChildDown)
        .addFunction("moveChildToBottom", &EntityRegistry::moveChildToBottom)
        .addFunction("findOldestParent", &EntityRegistry::findOldestParent)
        .addFunction("isParentOf", &EntityRegistry::isParentOf)
        .addFunction("findBranchLastIndex", &EntityRegistry::findBranchLastIndex)
        .addFunction("setDefaultEntityPool", &EntityRegistry::setDefaultEntityPool)
        .addFunction("getDefaultEntityPool", &EntityRegistry::getDefaultEntityPool)
        .addFunction("getLastEntity", &EntityRegistry::getLastEntity)
        .addFunction("getEntityList", &EntityRegistry::getEntityList)
        .addFunction("clear", &EntityRegistry::clear)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Scene, EntityRegistry>("Scene")
        .addConstructor <void (*) (void), void (*) (EntityPool)> ()
        .addFunction("load", &Scene::load)
        .addFunction("destroy", &Scene::destroy)
        .addFunction("draw", &Scene::draw)
        .addFunction("update", &Scene::update)
        .addFunction("updateCameraSize", &Scene::updateCameraSize)
        .addProperty("camera", (Entity(Scene::*)()const)&Scene::getCamera, (void(Scene::*)(Entity))&Scene::setCamera)
        .addFunction("setCamera", 
            luabridge::overload<Camera*>(&Scene::setCamera),
            luabridge::overload<Entity>(&Scene::setCamera))
        .addProperty("backgroundColor", &Scene::getBackgroundColor, (void (Scene::*)(Vector4))&Scene::setBackgroundColor)
        .addFunction("setBackgroundColor", 
            luabridge::overload<float, float, float>(&Scene::setBackgroundColor),
            luabridge::overload<float, float, float, float>(&Scene::setBackgroundColor))
        .addProperty("shadowQuality", &Scene::getShadowQuality, &Scene::setShadowQuality)
        .addProperty("shadowDistance", &Scene::getShadowDistance, &Scene::setShadowDistance)
        .addProperty("shadowFadeFraction", &Scene::getShadowFadeFraction, &Scene::setShadowFadeFraction)
        .addProperty("shadowCascadeBlend", &Scene::isShadowCascadeBlend, &Scene::setShadowCascadeBlend)
        .addProperty("effectiveShadowTier", &Scene::getEffectiveShadowTier)
        .addProperty("lightState", &Scene::getLightState, &Scene::setLightState)
        .addProperty("globalIlluminationColor", &Scene::getGlobalIlluminationColor, (void (Scene::*)(Vector3))&Scene::setGlobalIllumination)
        .addProperty("globalIlluminationIntensity", &Scene::getGlobalIlluminationIntensity, (void (Scene::*)(float))&Scene::setGlobalIllumination)
        .addFunction("getGlobalIlluminationColorLinear", &Scene::getGlobalIlluminationColorLinear)
        .addFunction("setGlobalIllumination", (void (Scene::*)(float, Vector3))&Scene::setGlobalIllumination)
        .addProperty("maxLightsPerObject", &Scene::getMaxLightsPerObject, &Scene::setMaxLightsPerObject)
        .addProperty("ambientLight2DColor", &Scene::getAmbientLight2DColor, (void (Scene::*)(Vector3))&Scene::setAmbientLight2D)
        .addProperty("ambientLight2DIntensity", &Scene::getAmbientLight2DIntensity, (void (Scene::*)(float))&Scene::setAmbientLight2D)
        .addFunction("getAmbientLight2DColorLinear", &Scene::getAmbientLight2DColorLinear)
        .addFunction("setAmbientLight2D", (void (Scene::*)(float, Vector3))&Scene::setAmbientLight2D)
        .addProperty("shadow2DQuality", &Scene::getShadow2DQuality, &Scene::setShadow2DQuality)
        .addProperty("gravity2D", &Scene::getGravity2D, (void (Scene::*)(Vector2))&Scene::setGravity2D)
        .addFunction("setGravity2D",
            luabridge::overload<Vector2>(&Scene::setGravity2D),
            luabridge::overload<float, float>(&Scene::setGravity2D))
        .addProperty("gravity3D", &Scene::getGravity3D, (void (Scene::*)(Vector3))&Scene::setGravity3D)
        .addFunction("setGravity3D",
            luabridge::overload<Vector3>(&Scene::setGravity3D),
            luabridge::overload<float, float, float>(&Scene::setGravity3D))
        .addProperty("physicsInterpolation", &Scene::isPhysicsInterpolation, &Scene::setPhysicsInterpolation)
        .addProperty("ssaoEnabled", &Scene::isSSAOEnabled, &Scene::setSSAOEnabled)
        .addProperty("ssaoRadius", &Scene::getSSAORadius, &Scene::setSSAORadius)
        .addProperty("ssaoIntensity", &Scene::getSSAOIntensity, &Scene::setSSAOIntensity)
        .addProperty("ssaoBias", &Scene::getSSAOBias, &Scene::setSSAOBias)
        .addProperty("meshLodEnabled", &Scene::isMeshLodEnabled, &Scene::setMeshLodEnabled)
        .addProperty("meshLodThreshold", &Scene::getMeshLodThreshold, &Scene::setMeshLodThreshold)
        .addProperty("depthPrepassEnabled", &Scene::isDepthPrepassEnabled, &Scene::setDepthPrepassEnabled)
        .addProperty("sceneColorScale", &Scene::getSceneColorScale, &Scene::setSceneColorScale)
        // a copy (edit it and assign it back, which applies it)
        .addProperty("postProcessing", [] (const Scene* self) { return PostProcessingSettings(self->getPostProcessing()); }, &Scene::setPostProcessing)
        .addProperty("ssaoDebug", &Scene::isSSAODebug, &Scene::setSSAODebug)
        .addProperty("ssrEnabled", &Scene::isSSREnabled, &Scene::setSSREnabled)
        .addProperty("ssrMaxDistance", &Scene::getSSRMaxDistance, &Scene::setSSRMaxDistance)
        .addProperty("ssrThickness", &Scene::getSSRThickness, &Scene::setSSRThickness)
        .addProperty("ssrMaxSteps", &Scene::getSSRMaxSteps, &Scene::setSSRMaxSteps)
        .addProperty("ssrIntensity", &Scene::getSSRIntensity, &Scene::setSSRIntensity)
        .addProperty("ssrBlur", &Scene::getSSRBlur, &Scene::setSSRBlur)
        .addProperty("ssrDebugMode", &Scene::getSSRDebugMode, &Scene::setSSRDebugMode)
        .addProperty("fixedResolutionEnabled", &Scene::isFixedResolutionEnabled, &Scene::setFixedResolutionEnabled)
        .addProperty("fixedResolutionWidth", &Scene::getFixedResolutionWidth, &Scene::setFixedResolutionWidth)
        .addProperty("fixedResolutionHeight", &Scene::getFixedResolutionHeight, &Scene::setFixedResolutionHeight)
        .addProperty("fixedResolutionFilter", &Scene::getFixedResolutionFilter, &Scene::setFixedResolutionFilter)
        .addFunction("setFixedResolutionSize", &Scene::setFixedResolutionSize)
        .addProperty("defaultMeshShader", &Scene::getDefaultMeshShader, &Scene::setDefaultMeshShader)
        .addProperty("defaultUIShader", &Scene::getDefaultUIShader, &Scene::setDefaultUIShader)
        .addProperty("defaultSkyShader", &Scene::getDefaultSkyShader, &Scene::setDefaultSkyShader)
        .addProperty("defaultPointsShader", &Scene::getDefaultPointsShader, &Scene::setDefaultPointsShader)
        .addProperty("defaultLinesShader", &Scene::getDefaultLinesShader, &Scene::setDefaultLinesShader)
        .addProperty("postProcessPasses", &Scene::getPostProcessPasses, &Scene::setPostProcessPasses)
        .addFunction("setPostProcessUniform",
            luabridge::overload<unsigned int, const std::string&, const Vector4&>(&Scene::setPostProcessUniform),
            luabridge::overload<unsigned int, const std::string&, const Vector3&>(&Scene::setPostProcessUniform),
            luabridge::overload<unsigned int, const std::string&, const Vector2&>(&Scene::setPostProcessUniform),
            luabridge::overload<unsigned int, const std::string&, float>(&Scene::setPostProcessUniform))
        .addFunction("getPostProcessUniform", &Scene::getPostProcessUniform)
        .addFunction("removePostProcessUniform", &Scene::removePostProcessUniform)
        .addFunction("setPostProcessPassEnabled", &Scene::setPostProcessPassEnabled)
        .addFunction("isPostProcessPassEnabled", &Scene::isPostProcessPassEnabled)
        .addFunction("canReceiveUIEvents", &Scene::canReceiveUIEvents)
        .addProperty("enableUIEvents", &Scene::getEnableUIEvents, (void (Scene::*)(UIEventState))&Scene::setEnableUIEvents)
        .addFunction("getActionSystem", [] (Scene* self, lua_State* L) { return self->getSystem<ActionSystem>().get(); })
        .addFunction("getAudioSystem", [] (Scene* self, lua_State* L) { return self->getSystem<AudioSystem>().get(); })
        .addFunction("getMeshSystem", [] (Scene* self, lua_State* L) { return self->getSystem<MeshSystem>().get(); })
        .addFunction("getPhysicsSystem", [] (Scene* self, lua_State* L) { return self->getSystem<PhysicsSystem>().get(); })
        .addFunction("getRenderSystem", [] (Scene* self, lua_State* L) { return self->getSystem<RenderSystem>().get(); })
        .addFunction("getUISystem", [] (Scene* self, lua_State* L) { return self->getSystem<UISystem>().get(); })
        .endClass();

    // Log.*: category lua, with the calling chunk and line. debug is compiled out of release
    // builds as before; Log.category(name) gives a category's own trace...error
    luabridge::getGlobalNamespace(L)
        .beginClass<Log>("Log")
        .addStaticFunction("print", [] (const char* text, lua_State* L) { luaLog(L, LogLevel::Info, logCategoryLua(), text); })
        .addStaticFunction("info", [] (const char* text, lua_State* L) { luaLog(L, LogLevel::Info, logCategoryLua(), text); })
        .addStaticFunction("verbose", [] (const char* text, lua_State* L) { luaLog(L, LogLevel::Trace, logCategoryLua(), text); })
        .addStaticFunction("debug", [] (const char* text, lua_State* L) {
        #ifndef NDEBUG
            luaLog(L, LogLevel::Debug, logCategoryLua(), text);
        #else
            (void)text; (void)L;
        #endif
        })
        .addStaticFunction("warn", [] (const char* text, lua_State* L) { luaLog(L, LogLevel::Warn, logCategoryLua(), text); })
        .addStaticFunction("error", [] (const char* text, lua_State* L) { luaLog(L, LogLevel::Error, logCategoryLua(), text); })
        // false (and a warning) for an unknown level name
        .addStaticFunction("setLevel", [] (const std::string& level, std::optional<std::string> category, lua_State* L) {
            LogLevel value;
            if (!parseLogLevel(level.c_str(), value)) {
                luaLog(L, LogLevel::Warn, logCategoryLua(),
                       ("Log.setLevel: unknown level '" + level + "' (trace, debug, info, warn, error, fatal, off)").c_str());
                return false;
            }
            Logger::setCategoryLevel(category ? Logger::registerCategory(*category, LogLevel::Info) : logCategoryLua(), value);
            return true;
        })
        .addStaticFunction("getLevel", [] (std::optional<std::string> category) -> std::string {
            const LogCategory* found = category ? Logger::findCategory(*category) : &logCategoryLua();
            return logLevelName(found ? Logger::categoryLevel(*found) : Logger::defaultLevel());
        })
        .addStaticFunction("category", [] (const std::string& name) {
            return LuaLogCategory{&Logger::registerCategory(name, LogLevel::Info)};
        })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<LuaLogCategory>("LogCategory")
        .addProperty("name", [] (const LuaLogCategory* self) { return std::string(self->category->name); })
        .addFunction("trace", [] (LuaLogCategory* self, const char* text, lua_State* L) { luaLog(L, LogLevel::Trace, *self->category, text); })
        .addFunction("debug", [] (LuaLogCategory* self, const char* text, lua_State* L) { luaLog(L, LogLevel::Debug, *self->category, text); })
        .addFunction("info", [] (LuaLogCategory* self, const char* text, lua_State* L) { luaLog(L, LogLevel::Info, *self->category, text); })
        .addFunction("warn", [] (LuaLogCategory* self, const char* text, lua_State* L) { luaLog(L, LogLevel::Warn, *self->category, text); })
        .addFunction("error", [] (LuaLogCategory* self, const char* text, lua_State* L) { luaLog(L, LogLevel::Error, *self->category, text); })
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Input>("Input")
        .addStaticProperty("MODIFIER_SHIFT", [] () -> int { return D_MODIFIER_SHIFT; })
        .addStaticProperty("MODIFIER_CONTROL", [] () -> int { return D_MODIFIER_CONTROL; })
        .addStaticProperty("MODIFIER_ALT", [] () -> int { return D_MODIFIER_ALT; })
        .addStaticProperty("MODIFIER_SUPER", [] () -> int { return D_MODIFIER_SUPER; })
        .addStaticProperty("MODIFIER_CAPS_LOCK", [] () -> int { return D_MODIFIER_CAPS_LOCK; })
        .addStaticProperty("MODIFIER_NUM_LOCK", [] () -> int { return D_MODIFIER_NUM_LOCK; })
        .addStaticProperty("KEY_UNKNOWN", [] () -> int { return D_KEY_UNKNOWN; })
        .addStaticProperty("KEY_SPACE", [] () -> int { return D_KEY_SPACE; })
        .addStaticProperty("KEY_APOSTROPHE", [] () -> int { return D_KEY_APOSTROPHE; })
        .addStaticProperty("KEY_COMMA", [] () -> int { return D_KEY_COMMA; })
        .addStaticProperty("KEY_MINUS", [] () -> int { return D_KEY_MINUS; })
        .addStaticProperty("KEY_PERIOD", [] () -> int { return D_KEY_PERIOD; })
        .addStaticProperty("KEY_SLASH", [] () -> int { return D_KEY_SLASH; })
        .addStaticProperty("KEY_0", [] () -> int { return D_KEY_0; })
        .addStaticProperty("KEY_1", [] () -> int { return D_KEY_1; })
        .addStaticProperty("KEY_2", [] () -> int { return D_KEY_2; })
        .addStaticProperty("KEY_3", [] () -> int { return D_KEY_3; })
        .addStaticProperty("KEY_4", [] () -> int { return D_KEY_4; })
        .addStaticProperty("KEY_5", [] () -> int { return D_KEY_5; })
        .addStaticProperty("KEY_6", [] () -> int { return D_KEY_6; })
        .addStaticProperty("KEY_7", [] () -> int { return D_KEY_7; })
        .addStaticProperty("KEY_8", [] () -> int { return D_KEY_8; })
        .addStaticProperty("KEY_9", [] () -> int { return D_KEY_9; })
        .addStaticProperty("KEY_SEMICOLON", [] () -> int { return D_KEY_SEMICOLON; })
        .addStaticProperty("KEY_EQUAL", [] () -> int { return D_KEY_EQUAL; })
        .addStaticProperty("KEY_A", [] () -> int { return D_KEY_A; })
        .addStaticProperty("KEY_B", [] () -> int { return D_KEY_B; })
        .addStaticProperty("KEY_C", [] () -> int { return D_KEY_C; })
        .addStaticProperty("KEY_D", [] () -> int { return D_KEY_D; })
        .addStaticProperty("KEY_E", [] () -> int { return D_KEY_E; })
        .addStaticProperty("KEY_F", [] () -> int { return D_KEY_F; })
        .addStaticProperty("KEY_G", [] () -> int { return D_KEY_G; })
        .addStaticProperty("KEY_H", [] () -> int { return D_KEY_H; })
        .addStaticProperty("KEY_I", [] () -> int { return D_KEY_I; })
        .addStaticProperty("KEY_J", [] () -> int { return D_KEY_J; })
        .addStaticProperty("KEY_K", [] () -> int { return D_KEY_K; })
        .addStaticProperty("KEY_L", [] () -> int { return D_KEY_L; })
        .addStaticProperty("KEY_M", [] () -> int { return D_KEY_M; })
        .addStaticProperty("KEY_N", [] () -> int { return D_KEY_N; })
        .addStaticProperty("KEY_O", [] () -> int { return D_KEY_O; })
        .addStaticProperty("KEY_P", [] () -> int { return D_KEY_P; })
        .addStaticProperty("KEY_Q", [] () -> int { return D_KEY_Q; })
        .addStaticProperty("KEY_R", [] () -> int { return D_KEY_R; })
        .addStaticProperty("KEY_S", [] () -> int { return D_KEY_S; })
        .addStaticProperty("KEY_T", [] () -> int { return D_KEY_T; })
        .addStaticProperty("KEY_U", [] () -> int { return D_KEY_U; })
        .addStaticProperty("KEY_V", [] () -> int { return D_KEY_V; })
        .addStaticProperty("KEY_W", [] () -> int { return D_KEY_W; })
        .addStaticProperty("KEY_X", [] () -> int { return D_KEY_X; })
        .addStaticProperty("KEY_Y", [] () -> int { return D_KEY_Y; })
        .addStaticProperty("KEY_Z", [] () -> int { return D_KEY_Z; })
        .addStaticProperty("KEY_LEFT_BRACKET", [] () -> int { return D_KEY_LEFT_BRACKET; })
        .addStaticProperty("KEY_BACKSLASH", [] () -> int { return D_KEY_BACKSLASH; })
        .addStaticProperty("KEY_RIGHT_BRACKET", [] () -> int { return D_KEY_RIGHT_BRACKET; })
        .addStaticProperty("KEY_GRAVE_ACCENT", [] () -> int { return D_KEY_GRAVE_ACCENT; })
        .addStaticProperty("KEY_WORLD_1", [] () -> int { return D_KEY_WORLD_1; })
        .addStaticProperty("KEY_WORLD_2", [] () -> int { return D_KEY_WORLD_2; })
        .addStaticProperty("KEY_ESCAPE", [] () -> int { return D_KEY_ESCAPE; })
        .addStaticProperty("KEY_ENTER", [] () -> int { return D_KEY_ENTER; })
        .addStaticProperty("KEY_TAB", [] () -> int { return D_KEY_TAB; })
        .addStaticProperty("KEY_BACKSPACE", [] () -> int { return D_KEY_BACKSPACE; })
        .addStaticProperty("KEY_INSERT", [] () -> int { return D_KEY_INSERT; })
        .addStaticProperty("KEY_DELETE", [] () -> int { return D_KEY_DELETE; })
        .addStaticProperty("KEY_RIGHT", [] () -> int { return D_KEY_RIGHT; })
        .addStaticProperty("KEY_LEFT", [] () -> int { return D_KEY_LEFT; })
        .addStaticProperty("KEY_DOWN", [] () -> int { return D_KEY_DOWN; })
        .addStaticProperty("KEY_UP", [] () -> int { return D_KEY_UP; })
        .addStaticProperty("KEY_PAGE_UP", [] () -> int { return D_KEY_PAGE_UP; })
        .addStaticProperty("KEY_PAGE_DOWN", [] () -> int { return D_KEY_PAGE_DOWN; })
        .addStaticProperty("KEY_HOME", [] () -> int { return D_KEY_HOME; })
        .addStaticProperty("KEY_END", [] () -> int { return D_KEY_END; })
        .addStaticProperty("KEY_CAPS_LOCK", [] () -> int { return D_KEY_CAPS_LOCK; })
        .addStaticProperty("KEY_SCROLL_LOCK", [] () -> int { return D_KEY_SCROLL_LOCK; })
        .addStaticProperty("KEY_NUM_LOCK", [] () -> int { return D_KEY_NUM_LOCK; })
        .addStaticProperty("KEY_PRINT_SCREEN", [] () -> int { return D_KEY_PRINT_SCREEN; })
        .addStaticProperty("KEY_PAUSE", [] () -> int { return D_KEY_PAUSE; })
        .addStaticProperty("KEY_F1", [] () -> int { return D_KEY_F1; })
        .addStaticProperty("KEY_F2", [] () -> int { return D_KEY_F2; })
        .addStaticProperty("KEY_F3", [] () -> int { return D_KEY_F3; })
        .addStaticProperty("KEY_F4", [] () -> int { return D_KEY_F4; })
        .addStaticProperty("KEY_F5", [] () -> int { return D_KEY_F5; })
        .addStaticProperty("KEY_F6", [] () -> int { return D_KEY_F6; })
        .addStaticProperty("KEY_F7", [] () -> int { return D_KEY_F7; })
        .addStaticProperty("KEY_F8", [] () -> int { return D_KEY_F8; })
        .addStaticProperty("KEY_F9", [] () -> int { return D_KEY_F9; })
        .addStaticProperty("KEY_F10", [] () -> int { return D_KEY_F10; })
        .addStaticProperty("KEY_F11", [] () -> int { return D_KEY_F11; })
        .addStaticProperty("KEY_F12", [] () -> int { return D_KEY_F12; })
        .addStaticProperty("KEY_F13", [] () -> int { return D_KEY_F13; })
        .addStaticProperty("KEY_F14", [] () -> int { return D_KEY_F14; })
        .addStaticProperty("KEY_F15", [] () -> int { return D_KEY_F15; })
        .addStaticProperty("KEY_F16", [] () -> int { return D_KEY_F16; })
        .addStaticProperty("KEY_F17", [] () -> int { return D_KEY_F17; })
        .addStaticProperty("KEY_F18", [] () -> int { return D_KEY_F18; })
        .addStaticProperty("KEY_F19", [] () -> int { return D_KEY_F19; })
        .addStaticProperty("KEY_F20", [] () -> int { return D_KEY_F20; })
        .addStaticProperty("KEY_F21", [] () -> int { return D_KEY_F21; })
        .addStaticProperty("KEY_F22", [] () -> int { return D_KEY_F22; })
        .addStaticProperty("KEY_F23", [] () -> int { return D_KEY_F23; })
        .addStaticProperty("KEY_F24", [] () -> int { return D_KEY_F24; })
        .addStaticProperty("KEY_F25", [] () -> int { return D_KEY_F25; })
        .addStaticProperty("KEY_KP_0", [] () -> int { return D_KEY_KP_0; })
        .addStaticProperty("KEY_KP_1", [] () -> int { return D_KEY_KP_1; })
        .addStaticProperty("KEY_KP_2", [] () -> int { return D_KEY_KP_2; })
        .addStaticProperty("KEY_KP_3", [] () -> int { return D_KEY_KP_3; })
        .addStaticProperty("KEY_KP_4", [] () -> int { return D_KEY_KP_4; })
        .addStaticProperty("KEY_KP_5", [] () -> int { return D_KEY_KP_5; })
        .addStaticProperty("KEY_KP_6", [] () -> int { return D_KEY_KP_6; })
        .addStaticProperty("KEY_KP_7", [] () -> int { return D_KEY_KP_7; })
        .addStaticProperty("KEY_KP_8", [] () -> int { return D_KEY_KP_8; })
        .addStaticProperty("KEY_KP_9", [] () -> int { return D_KEY_KP_9; })
        .addStaticProperty("KEY_KP_DECIMAL", [] () -> int { return D_KEY_KP_DECIMAL; })
        .addStaticProperty("KEY_KP_DIVIDE", [] () -> int { return D_KEY_KP_DIVIDE; })
        .addStaticProperty("KEY_KP_MULTIPLY", [] () -> int { return D_KEY_KP_MULTIPLY; })
        .addStaticProperty("KEY_KP_SUBTRACT", [] () -> int { return D_KEY_KP_SUBTRACT; })
        .addStaticProperty("KEY_KP_ADD", [] () -> int { return D_KEY_KP_ADD; })
        .addStaticProperty("KEY_KP_ENTER", [] () -> int { return D_KEY_KP_ENTER; })
        .addStaticProperty("KEY_KP_EQUAL", [] () -> int { return D_KEY_KP_EQUAL; })
        .addStaticProperty("KEY_LEFT_SHIFT", [] () -> int { return D_KEY_LEFT_SHIFT; })
        .addStaticProperty("KEY_LEFT_CONTROL", [] () -> int { return D_KEY_LEFT_CONTROL; })
        .addStaticProperty("KEY_LEFT_ALT", [] () -> int { return D_KEY_LEFT_ALT; })
        .addStaticProperty("KEY_LEFT_SUPER", [] () -> int { return D_KEY_LEFT_SUPER; })
        .addStaticProperty("KEY_RIGHT_SHIFT", [] () -> int { return D_KEY_RIGHT_SHIFT; })
        .addStaticProperty("KEY_RIGHT_CONTROL", [] () -> int { return D_KEY_RIGHT_CONTROL; })
        .addStaticProperty("KEY_RIGHT_ALT", [] () -> int { return D_KEY_RIGHT_ALT; })
        .addStaticProperty("KEY_RIGHT_SUPER", [] () -> int { return D_KEY_RIGHT_SUPER; })
        .addStaticProperty("KEY_MENU", [] () -> int { return D_KEY_MENU; })
        .addStaticProperty("KEY_LAST", [] () -> int { return D_KEY_LAST; })

        .addStaticProperty("MOUSE_BUTTON_1", [] () -> int { return D_MOUSE_BUTTON_1; })
        .addStaticProperty("MOUSE_BUTTON_2", [] () -> int { return D_MOUSE_BUTTON_2; })
        .addStaticProperty("MOUSE_BUTTON_3", [] () -> int { return D_MOUSE_BUTTON_3; })
        .addStaticProperty("MOUSE_BUTTON_4", [] () -> int { return D_MOUSE_BUTTON_4; })
        .addStaticProperty("MOUSE_BUTTON_5", [] () -> int { return D_MOUSE_BUTTON_5; })
        .addStaticProperty("MOUSE_BUTTON_6", [] () -> int { return D_MOUSE_BUTTON_6; })
        .addStaticProperty("MOUSE_BUTTON_7", [] () -> int { return D_MOUSE_BUTTON_7; })
        .addStaticProperty("MOUSE_BUTTON_8", [] () -> int { return D_MOUSE_BUTTON_8; })
        .addStaticProperty("MOUSE_BUTTON_LAST", [] () -> int { return D_MOUSE_BUTTON_LAST; })
        .addStaticProperty("MOUSE_BUTTON_LEFT", [] () -> int { return D_MOUSE_BUTTON_LEFT; })
        .addStaticProperty("MOUSE_BUTTON_RIGHT", [] () -> int { return D_MOUSE_BUTTON_RIGHT; })
        .addStaticProperty("MOUSE_BUTTON_MIDDLE", [] () -> int { return D_MOUSE_BUTTON_MIDDLE; })

        .addStaticProperty("GAMEPAD_BUTTON_A", [] () -> int { return D_GAMEPAD_BUTTON_A; })
        .addStaticProperty("GAMEPAD_BUTTON_B", [] () -> int { return D_GAMEPAD_BUTTON_B; })
        .addStaticProperty("GAMEPAD_BUTTON_X", [] () -> int { return D_GAMEPAD_BUTTON_X; })
        .addStaticProperty("GAMEPAD_BUTTON_Y", [] () -> int { return D_GAMEPAD_BUTTON_Y; })
        .addStaticProperty("GAMEPAD_BUTTON_LEFT_BUMPER", [] () -> int { return D_GAMEPAD_BUTTON_LEFT_BUMPER; })
        .addStaticProperty("GAMEPAD_BUTTON_RIGHT_BUMPER", [] () -> int { return D_GAMEPAD_BUTTON_RIGHT_BUMPER; })
        .addStaticProperty("GAMEPAD_BUTTON_BACK", [] () -> int { return D_GAMEPAD_BUTTON_BACK; })
        .addStaticProperty("GAMEPAD_BUTTON_START", [] () -> int { return D_GAMEPAD_BUTTON_START; })
        .addStaticProperty("GAMEPAD_BUTTON_GUIDE", [] () -> int { return D_GAMEPAD_BUTTON_GUIDE; })
        .addStaticProperty("GAMEPAD_BUTTON_LEFT_THUMB", [] () -> int { return D_GAMEPAD_BUTTON_LEFT_THUMB; })
        .addStaticProperty("GAMEPAD_BUTTON_RIGHT_THUMB", [] () -> int { return D_GAMEPAD_BUTTON_RIGHT_THUMB; })
        .addStaticProperty("GAMEPAD_BUTTON_DPAD_UP", [] () -> int { return D_GAMEPAD_BUTTON_DPAD_UP; })
        .addStaticProperty("GAMEPAD_BUTTON_DPAD_RIGHT", [] () -> int { return D_GAMEPAD_BUTTON_DPAD_RIGHT; })
        .addStaticProperty("GAMEPAD_BUTTON_DPAD_DOWN", [] () -> int { return D_GAMEPAD_BUTTON_DPAD_DOWN; })
        .addStaticProperty("GAMEPAD_BUTTON_DPAD_LEFT", [] () -> int { return D_GAMEPAD_BUTTON_DPAD_LEFT; })
        .addStaticProperty("GAMEPAD_BUTTON_LAST", [] () -> int { return D_GAMEPAD_BUTTON_LAST; })
        .addStaticProperty("GAMEPAD_BUTTON_CROSS", [] () -> int { return D_GAMEPAD_BUTTON_CROSS; })
        .addStaticProperty("GAMEPAD_BUTTON_CIRCLE", [] () -> int { return D_GAMEPAD_BUTTON_CIRCLE; })
        .addStaticProperty("GAMEPAD_BUTTON_SQUARE", [] () -> int { return D_GAMEPAD_BUTTON_SQUARE; })
        .addStaticProperty("GAMEPAD_BUTTON_TRIANGLE", [] () -> int { return D_GAMEPAD_BUTTON_TRIANGLE; })

        .addStaticProperty("GAMEPAD_AXIS_LEFT_X", [] () -> int { return D_GAMEPAD_AXIS_LEFT_X; })
        .addStaticProperty("GAMEPAD_AXIS_LEFT_Y", [] () -> int { return D_GAMEPAD_AXIS_LEFT_Y; })
        .addStaticProperty("GAMEPAD_AXIS_RIGHT_X", [] () -> int { return D_GAMEPAD_AXIS_RIGHT_X; })
        .addStaticProperty("GAMEPAD_AXIS_RIGHT_Y", [] () -> int { return D_GAMEPAD_AXIS_RIGHT_Y; })
        .addStaticProperty("GAMEPAD_AXIS_LEFT_TRIGGER", [] () -> int { return D_GAMEPAD_AXIS_LEFT_TRIGGER; })
        .addStaticProperty("GAMEPAD_AXIS_RIGHT_TRIGGER", [] () -> int { return D_GAMEPAD_AXIS_RIGHT_TRIGGER; })
        .addStaticProperty("GAMEPAD_AXIS_LAST", [] () -> int { return D_GAMEPAD_AXIS_LAST; })

        .addStaticFunction("isKeyPressed", &Input::isKeyPressed)
        .addStaticFunction("isMousePressed", &Input::isMousePressed)
        .addStaticFunction("isTouch", &Input::isTouch)
        .addStaticFunction("isMouseEntered", &Input::isMouseEntered)
        .addStaticFunction("getMousePosition", &Input::getMousePosition)
        .addStaticFunction("getMouseScroll", &Input::getMouseScroll)
        .addStaticFunction("getTouchPosition", &Input::getTouchPosition)
        .addStaticFunction("getTouches", &Input::getTouches)
        .addStaticFunction("numTouches", &Input::numTouches)
        .addStaticFunction("isGamepadConnected", &Input::isGamepadConnected)
        .addStaticFunction("getGamepadName", &Input::getGamepadName)
        .addStaticFunction("isGamepadButtonPressed", &Input::isGamepadButtonPressed)
        .addStaticFunction("getGamepadAxis", &Input::getGamepadAxis)
        .addStaticFunction("numGamepads", &Input::numGamepads)
        .addStaticFunction("getGamepadId", &Input::getGamepadId)
        .addStaticFunction("getModifiers", &Input::getModifiers)
        .addStaticFunction("findTouchIndex", &Input::findTouchIndex)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<System>("System")
        .addStaticFunction("getScreenWidth", [] () { return System::instance().getScreenWidth(); })
        .addStaticFunction("getScreenHeight", [] () { return System::instance().getScreenHeight(); })
        .addStaticFunction("showVirtualKeyboard", [] (std::wstring text) { System::instance().showVirtualKeyboard(text); })
        .addStaticFunction("hideVirtualKeyboard", [] () { System::instance().hideVirtualKeyboard(); })
        .addStaticFunction("setClipboardText", [] (const std::string& text) { System::instance().setClipboardText(text); })
        .addStaticFunction("getClipboardText", [] () { return System::instance().getClipboardText(); })
        .addStaticFunction("isTouchDevice", [] () { return System::instance().isTouchDevice(); })
        .addStaticFunction("isFullscreen", [] () { return System::instance().isFullscreen(); })
        .addStaticFunction("requestFullscreen", [] () { System::instance().requestFullscreen(); })
        .addStaticFunction("exitFullscreen", [] () { System::instance().exitFullscreen(); })
        .addStaticFunction("isWindowMaximized", [] () { return System::instance().isWindowMaximized(); })
        .addStaticFunction("maximizeWindow", [] () { System::instance().maximizeWindow(); })
        .addStaticFunction("restoreWindow", [] () { System::instance().restoreWindow(); })
        .addStaticFunction("setWindowSize", [] (int width, int height) { System::instance().setWindowSize(width, height); })
        .addStaticFunction("isWindowResizable", [] () { return System::instance().isWindowResizable(); })
        .addStaticFunction("setWindowResizable", [] (bool resizable) { System::instance().setWindowResizable(resizable); })
        .addStaticFunction("setWindowTitle", [] (const std::string& title) { System::instance().setWindowTitle(title); })
        .addStaticFunction("quit", [] () { System::instance().quit(); })
        .addStaticFunction("getDirSeparator", [] () { return System::instance().getDirSeparator(); })
        .addStaticFunction("getAssetPath", [] () { return System::instance().getAssetPath(); })
        .addStaticFunction("getUserDataPath", [] () { return System::instance().getUserDataPath(); })
        .addStaticFunction("getLuaPath", [] () { return System::instance().getLuaPath(); })
        .addStaticFunction("getShaderPath", [] () { return System::instance().getShaderPath(); })

        //UserSettings not need here

        .addStaticFunction("initializeAdMob", [] () { return System::instance().initializeAdMob(); })
        .addStaticFunction("setMaxAdContentRating", [] (AdMobRating rating) { return System::instance().setMaxAdContentRating(rating); })
        .addStaticFunction("loadInterstitialAd", [] (const std::string& adUnitID) { return System::instance().loadInterstitialAd(adUnitID); })
        .addStaticFunction("isInterstitialAdLoaded", [] () { return System::instance().isInterstitialAdLoaded(); })
        .addStaticFunction("showInterstitialAd", [] () { return System::instance().showInterstitialAd(); })

        .addStaticFunction("initializeCrazyGamesSDK", [] () { return System::instance().initializeCrazyGamesSDK(); })
        .addStaticFunction("showCrazyGamesAd", [] (std::string type) { return System::instance().showCrazyGamesAd(type); })
        .addStaticFunction("happytimeCrazyGames", [] () { return System::instance().happytimeCrazyGames(); })
        .addStaticFunction("gameplayStartCrazyGames", [] () { return System::instance().gameplayStartCrazyGames(); })
        .addStaticFunction("gameplayStopCrazyGames", [] () { return System::instance().gameplayStopCrazyGames(); })
        .addStaticFunction("loadingStartCrazyGames", [] () { return System::instance().loadingStartCrazyGames(); })
        .addStaticFunction("loadingStopCrazyGames", [] () { return System::instance().loadingStopCrazyGames(); })
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}
