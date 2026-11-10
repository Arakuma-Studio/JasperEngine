// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The PixelFX player in Lua (docs/specs/S48-pixel-fx.md R24-R26, R45): PixelFX.play and
// PixelFX.set (reopening the PixelFX class TransitionClassesLua.cpp starts with glitch) and the
// PixelFXHandle they return (Coroutine.waitFor polls its isDone).
//
// Lua is compiled as C: luaL_error longjmps over C++ frames, so the functions that raise errors
// parse in helpers and raise only from frames holding nothing with a destructor.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "Log.h"
#include "animation/AnimationClipIO.h"
#include "object/EntityHandle.h"
#include "pixelart/PixelFxLua.h"
#include "pixelart/PixelFxPlayer.h"
#include "util/Color.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace jasperengine;

#ifndef DISABLE_LUA_BINDINGS
namespace jasperengine::pixelart {
namespace {

    using PixelFxError = char[512];

    bool pfxFail(PixelFxError& error, const char* function, const std::string& text){
        std::snprintf(error, sizeof(PixelFxError), "%s: %s", function, text.c_str());
        return false;
    }

    bool pfxEntityArg(lua_State* L, int index, Scene*& scene, Entity& entity, const char* function, PixelFxError& error){
        EntityHandle* handle = nullptr;
        if (luabridge::isInstance<EntityHandle>(L, index)){
            auto result = luabridge::get<EntityHandle*>(L, index);
            if (result) handle = result.value();
        }
        if (!handle || !handle->getScene())
            return pfxFail(error, function, std::string("the first argument must be a GameObject, got ") + luaL_typename(L, index));
        scene = handle->getScene();
        entity = handle->getEntity();
        return true;
    }

    // "#rrggbb[aa]" as linear
    bool pfxHexColor(const char* text, Vector4& out){
        if (!text || text[0] != '#')
            return false;
        const std::string hex(text + 1);
        if (hex.size() != 6 && hex.size() != 8)
            return false;
        for (char c : hex){
            if (!std::isxdigit((unsigned char)c))
                return false;
        }
        auto channel = [&](size_t i){ return (float)std::strtol(hex.substr(i, 2).c_str(), nullptr, 16) / 255.0f; };
        out = Color::sRGBToLinear(Vector4(channel(0), channel(2), channel(4), hex.size() == 8 ? channel(6) : 1.0f));
        return true;
    }

    // a colour: "#rrggbb" or a Vector4 (sRGB, as picked), stored linear
    bool pfxColorValue(lua_State* L, int index, Vector4& out){
        if (lua_type(L, index) == LUA_TSTRING)
            return pfxHexColor(lua_tostring(L, index), out);
        if (luabridge::isInstance<Vector4>(L, index)){
            auto value = luabridge::get<Vector4>(L, index);
            if (!value) return false;
            out = Color::sRGBToLinear(value.value());
            return true;
        }
        return false;
    }

    bool pfxOptions(lua_State* L, int index, PixelFxOptions& options, PixelFxEffect effect, const char* function, PixelFxError& error){
        index = lua_absindex(L, index);
        if (lua_isnoneornil(L, index))
            return true;
        if (!lua_istable(L, index))
            return pfxFail(error, function, std::string("opts must be a table, got ") + luaL_typename(L, index));
        bool ok = true;

        lua_getfield(L, index, "ease");
        if (lua_type(L, -1) == LUA_TSTRING){
            if (!anim::easeFromName(lua_tostring(L, -1), options.ease))
                ok = pfxFail(error, function, std::string("opts.ease \"") + lua_tostring(L, -1) + "\" is not an ease (linear, quad_in, sine_in_out, ...)");
        }else if (lua_type(L, -1) == LUA_TNUMBER){
            options.ease = (EaseType)lua_tointeger(L, -1);
        }else if (!lua_isnil(L, -1)){
            ok = pfxFail(error, function, "opts.ease must be an ease name or an EaseType");
        }
        lua_pop(L, 1);
        if (!ok) return false;

        lua_getfield(L, index, "unscaled");
        if (lua_isboolean(L, -1)) options.unscaled = lua_toboolean(L, -1) != 0;
        else if (!lua_isnil(L, -1)) ok = pfxFail(error, function, "opts.unscaled must be a boolean");
        lua_pop(L, 1);
        if (!ok) return false;

        lua_getfield(L, index, "color");
        if (!lua_isnil(L, -1)){
            if (pfxColorValue(L, -1, options.color)) options.hasColor = true;
            else ok = pfxFail(error, function, "opts.color must be \"#rrggbb\" or a Vector4 (sRGB)");
        }
        lua_pop(L, 1);
        if (!ok) return false;

        lua_getfield(L, index, "count");
        if (lua_type(L, -1) == LUA_TNUMBER) options.count = (int)lua_tointeger(L, -1);
        else if (!lua_isnil(L, -1)) ok = pfxFail(error, function, "opts.count must be a whole number");
        lua_pop(L, 1);
        if (!ok) return false;

        lua_getfield(L, index, "ramp");
        if (lua_type(L, -1) == LUA_TSTRING) options.ramp = lua_tostring(L, -1);
        else if (!lua_isnil(L, -1)) ok = pfxFail(error, function, "opts.ramp must be a texture path or \"<palette>.palette#ramp:<name>\"");
        lua_pop(L, 1);
        if (!ok) return false;

        lua_getfield(L, index, "embers");
        if (lua_type(L, -1) == LUA_TSTRING) options.embers = lua_tostring(L, -1);
        else if (lua_isboolean(L, -1)) options.embers = lua_toboolean(L, -1) ? "ember" : "";
        else if (!lua_isnil(L, -1)) ok = pfxFail(error, function, "opts.embers must be a Pixel Particles preset name, a .particles path or true");
        lua_pop(L, 1);
        if (!ok) return false;
        if (!options.embers.empty() && effect != PixelFxEffect::DISSOLVE)
            return pfxFail(error, function, "opts.embers is for the dissolve effect");

        lua_getfield(L, index, "emberRate");
        if (lua_type(L, -1) == LUA_TNUMBER) options.emberRate = (float)lua_tonumber(L, -1);
        else if (!lua_isnil(L, -1)) ok = pfxFail(error, function, "opts.emberRate must be a number (embers per second)");
        lua_pop(L, 1);
        if (!ok) return false;

        lua_getfield(L, index, "hold");
        if (lua_type(L, -1) == LUA_TNUMBER) options.hold = (float)lua_tonumber(L, -1);
        else if (!lua_isnil(L, -1)) ok = pfxFail(error, function, "opts.hold must be a number of seconds");
        lua_pop(L, 1);
        if (!ok) return false;

        lua_getfield(L, index, "onFinished");
        if (lua_isfunction(L, -1)){
            luabridge::LuaRef callback = luabridge::LuaRef::fromStack(L, -1);
            options.onFinished = [callback](){
                auto called = callback();
                if (!called)
                    Log::error("PixelFX onFinished: %s", called.errorMessage().c_str());
            };
        }else if (!lua_isnil(L, -1)){
            ok = pfxFail(error, function, "opts.onFinished must be a function");
        }
        lua_pop(L, 1);
        return ok;
    }

    // PixelFX.play(go, effect, duration [, opts]) -> PixelFXHandle
    int luaPixelFxPlay(lua_State* L){
        PixelFxError error = {0};
        bool ok;
        {
            Scene* scene = nullptr;
            Entity entity = NULL_ENTITY;
            PixelFxEffect effect = PixelFxEffect::FLASH;
            PixelFxOptions options;
            ok = pfxEntityArg(L, 1, scene, entity, "PixelFX.play", error);
            if (ok){
                if (lua_type(L, 2) != LUA_TSTRING || !pixelFxEffectFromName(lua_tostring(L, 2), effect))
                    ok = pfxFail(error, "PixelFX.play", "the effect must be flash, fade_out, fade_in, blink, dissolve, undissolve, petrify, freeze, poison or outline_pulse");
            }
            if (ok && lua_type(L, 3) != LUA_TNUMBER)
                ok = pfxFail(error, "PixelFX.play", "the duration must be a number of seconds");
            if (ok)
                ok = pfxOptions(L, 4, options, effect, "PixelFX.play", error);
            if (ok){
                std::string why;
                PixelFxHandle handle = PixelFxPlayer::play(scene, entity, effect, (float)lua_tonumber(L, 3), options, &why);
                if (!handle.isValid())
                    ok = pfxFail(error, "PixelFX.play", why);
                else if (!luabridge::push(L, handle))
                    lua_pushnil(L);
            }
        }
        if (!ok)
            return luaL_error(L, "%s", error);
        return 1;
    }

    // PixelFX.set(go, name, value)
    int luaPixelFxSet(lua_State* L){
        PixelFxError error = {0};
        bool ok;
        {
            Scene* scene = nullptr;
            Entity entity = NULL_ENTITY;
            ok = pfxEntityArg(L, 1, scene, entity, "PixelFX.set", error);
            if (ok && lua_type(L, 2) != LUA_TSTRING)
                ok = pfxFail(error, "PixelFX.set", "the name must be a string (flash, fade, dissolve, ramp or a library:sprite property)");
            Vector4 value;
            if (ok){
                if (lua_type(L, 3) == LUA_TNUMBER || lua_isboolean(L, 3)){
                    value = Vector4(lua_isboolean(L, 3) ? (lua_toboolean(L, 3) ? 1.0f : 0.0f) : (float)lua_tonumber(L, 3), 0.0f, 0.0f, 0.0f);
                }else if (luabridge::isInstance<Vector4>(L, 3)){
                    value = luabridge::get<Vector4>(L, 3).value();
                }else if (luabridge::isInstance<Vector3>(L, 3)){
                    const Vector3 v = luabridge::get<Vector3>(L, 3).value();
                    value = Vector4(v.x, v.y, v.z, 0.0f);
                }else if (luabridge::isInstance<Vector2>(L, 3)){
                    const Vector2 v = luabridge::get<Vector2>(L, 3).value();
                    value = Vector4(v.x, v.y, 0.0f, 0.0f);
                }else{
                    ok = pfxFail(error, "PixelFX.set", std::string("the value must be a number or a vector, got ") + luaL_typename(L, 3));
                }
            }
            if (ok){
                std::string why;
                if (!PixelFxPlayer::set(scene, entity, lua_tostring(L, 2), value, &why))
                    ok = pfxFail(error, "PixelFX.set", why);
            }
        }
        if (!ok)
            return luaL_error(L, "%s", error);
        return 0;
    }

    luabridge::LuaRef pfxGetOnFinished(const PixelFxHandle* self, lua_State* L){
        std::shared_ptr<void>& data = const_cast<PixelFxHandle*>(self)->scriptData();
        if (data)
            return *std::static_pointer_cast<luabridge::LuaRef>(data);
        return luabridge::LuaRef(L);
    }

    // handle.onFinished = function() ... end (nil removes it)
    void pfxSetOnFinished(PixelFxHandle* self, lua_State* L){
        luabridge::LuaRef callback = luabridge::LuaRef::fromStack(L, 2);
        if (callback.isNil()){
            self->scriptData().reset();
            self->setOnFinished(nullptr);
            return;
        }
        if (!callback.isFunction()){
            Log::error("PixelFXHandle.onFinished must be a function or nil, got %s", luaL_typename(L, 2));
            return;
        }
        self->scriptData() = std::make_shared<luabridge::LuaRef>(callback);
        self->setOnFinished([callback](){
            auto called = callback();
            if (!called)
                Log::error("PixelFX onFinished: %s", called.errorMessage().c_str());
        });
    }

    int pfxActiveCount(){
        return (int)PixelFxPlayer::getActiveCount();
    }

}
}
#endif

void LuaBinding::registerPixelFxClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    using namespace jasperengine::pixelart;

    // A playing sprite effect (PixelFX.play); Coroutine.waitFor(handle) waits for it
    luabridge::getGlobalNamespace(L)
        .beginClass<PixelFxHandle>("PixelFXHandle")
        // True once it ended by itself, was stopped or replaced, or its GameObject went
        .addFunction("isDone", &PixelFxHandle::isDone)
        // Ends it now, its values back at their neutral 0 (onFinished is not called)
        .addFunction("stop", &PixelFxHandle::stop)
        // 0..1 of its duration; 0 until the material's variant with the effect's feature draws
        .addProperty("progress", &PixelFxHandle::getProgress)
        // False for the handle of a call that played nothing
        .addProperty("valid", &PixelFxHandle::isValid)
        // A function called once when it ends by itself
        .addProperty("onFinished", &pfxGetOnFinished, &pfxSetOnFinished)
        .endClass();

    // Sprite effects over time on a GameObject's own library:sprite material: flash, dithered
    // fades and blinks, dissolve (with embers), petrify, freeze and poison, an outline pulse
    luabridge::getGlobalNamespace(L)
        .beginClass<PixelFxLua>("PixelFX")
        .addStaticFunction("play", &luaPixelFxPlay)
        .addStaticFunction("set", &luaPixelFxSet)
        // The effects still playing
        .addStaticProperty("activeCount", &pfxActiveCount)
        .endClass();
#endif
}
