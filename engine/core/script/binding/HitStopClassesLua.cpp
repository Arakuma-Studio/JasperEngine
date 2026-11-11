// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// S48's game feel in Lua (docs/specs/S48-pixel-fx.md R20-R21, R45): HitStop (play, stop,
// active, remaining, unfrozenDeltaTime) and the Hit Stop Exempt component's wrapper.
//
// Lua is compiled as C: luaL_error longjmps over C++ frames, so HitStop.play does its work in a
// lambda and raises only from a frame holding nothing with a destructor.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"

#include "object/EntityHandle.h"
#include "object/HitStopExempt.h"
#include "pixelart/HitStop.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using jasperengine::pixelart::HitStop;

#ifndef DISABLE_LUA_BINDINGS
namespace {

    struct HitStopArgError{
        char text[512] = {0};

        bool set(const char* format, ...){
            va_list args;
            va_start(args, format);
            std::vsnprintf(text, sizeof(text), format, args);
            va_end(args);
            return false;
        }
    };

    // a list of GameObjects (any EntityHandle) at opts[key]
    bool luaHitStopTargets(lua_State* L, int options, const char* key, std::vector<pixelart::HitStopTarget>& out, HitStopArgError& error){
        lua_getfield(L, options, key);
        if (lua_isnil(L, -1)){
            lua_pop(L, 1);
            return true;
        }
        if (!lua_istable(L, -1)){
            lua_pop(L, 1);
            return error.set("HitStop.play: opts.%s must be a list of GameObjects", key);
        }
        const int list = lua_gettop(L);
        const lua_Integer count = (lua_Integer)lua_rawlen(L, list);
        for (lua_Integer i = 1; i <= count; i++){
            lua_rawgeti(L, list, i);
            EntityHandle* handle = nullptr;
            if (luabridge::isInstance<EntityHandle>(L, -1)){
                auto result = luabridge::get<EntityHandle*>(L, -1);
                if (result) handle = result.value();
            }
            lua_pop(L, 1);
            if (!handle || !handle->getScene()){
                lua_pop(L, 1);
                return error.set("HitStop.play: opts.%s[%d] is not a GameObject", key, (int)i);
            }
            out.push_back({handle->getScene(), handle->getEntity()});
        }
        lua_pop(L, 1);
        return true;
    }

    // HitStop.play(seconds [, {timeScale, exclude = {go...}, only = {go...},
    //                         shake = "preset" | {amplitude = px, seconds = s}}])
    int luaHitStopPlay(lua_State* L){
        HitStopArgError error;
        const bool ok = [&]() -> bool {
            if (lua_type(L, 1) != LUA_TNUMBER){
                return error.set("HitStop.play: seconds must be a number, got %s", luaL_typename(L, 1));
            }
            const float seconds = (float)lua_tonumber(L, 1);
            pixelart::HitStop::Options options;
            if (!lua_isnoneornil(L, 2)){
                if (!lua_istable(L, 2)){
                    return error.set("HitStop.play: opts must be a table, got %s", luaL_typename(L, 2));
                }
                lua_getfield(L, 2, "timeScale");
                if (lua_type(L, -1) == LUA_TNUMBER){
                    options.timeScale = (float)lua_tonumber(L, -1);
                }else if (!lua_isnil(L, -1)){
                    lua_pop(L, 1);
                    return error.set("HitStop.play: opts.timeScale must be a number (0: frozen, up to 1)");
                }
                lua_pop(L, 1);
                if (!luaHitStopTargets(L, 2, "exclude", options.exclude, error) ||
                    !luaHitStopTargets(L, 2, "only", options.only, error)){
                    return false;
                }
                lua_getfield(L, 2, "shake");
                if (lua_type(L, -1) == LUA_TSTRING){
                    options.shakePreset = lua_tostring(L, -1);
                }else if (lua_istable(L, -1)){
                    lua_getfield(L, -1, "amplitude");
                    lua_getfield(L, -2, "seconds");
                    if (lua_type(L, -2) != LUA_TNUMBER || lua_type(L, -1) != LUA_TNUMBER){
                        lua_pop(L, 3);
                        return error.set("HitStop.play: opts.shake must be a Shake Preset name or {amplitude = pixels, seconds = s}");
                    }
                    options.shakeAmplitudePx = (float)lua_tonumber(L, -2);
                    options.shakeSeconds = (float)lua_tonumber(L, -1);
                    lua_pop(L, 2);
                }else if (!lua_isnil(L, -1)){
                    lua_pop(L, 1);
                    return error.set("HitStop.play: opts.shake must be a Shake Preset name or {amplitude = pixels, seconds = s}");
                }
                lua_pop(L, 1);
            }
            pixelart::HitStop::play(seconds, options);
            return true;
        }();
        if (!ok){
            return luaL_error(L, "%s", error.text);
        }
        return 0;
    }

}
#endif

void LuaBinding::registerHitStopClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    // Hit-stop: the game freezes (or slows) for a moment on a hit, counted on unscaled time.
    // Time.deltaTime, physics, timers, actions, tweens, animations, scaled particles and shakes
    // stop; unscaled time, audio and the HTML UI clock run. Hit Stop Exempt GameObjects keep
    // running (Time.deltaTime inside their callbacks is unfrozen)
    luabridge::getGlobalNamespace(L)
        .beginClass<HitStop>("HitStop")
        // play(seconds [, opts]): opts.timeScale (0 frozen .. 1), opts.exclude = {go...} (keep
        // running), opts.only = {go...} (only these freeze, the world runs), opts.shake = a Shake
        // Preset name or {amplitude = pixels, seconds = s}, shaking through the freeze. Overlapping
        // calls keep the longer remaining time and the lower scale
        .addStaticFunction("play", &luaHitStopPlay)
        // Ends the hit-stop now
        .addStaticFunction("stop", &HitStop::stop)
        // A hit-stop is running
        .addStaticProperty("active", &HitStop::isActive)
        // Unscaled seconds left
        .addStaticProperty("remaining", &HitStop::getRemaining)
        // This frame's game delta without the hit-stop (frame time times Time.timeScale)
        .addStaticProperty("unfrozenDeltaTime", &HitStop::getUnfrozenDeltaTime)
        .endClass();

    // Keeps its GameObject (and, with includeChildren, its descendants) running through a
    // hit-stop: animations, actions, tweens, particles, shakes, follow, PixelFX effects and its
    // scripts' Time.deltaTime. Rigidbodies still run with the physics world
    luabridge::getGlobalNamespace(L)
        .deriveClass<HitStopExempt, EntityHandle>("HitStopExempt")
        .addConstructor <void (*) (Scene*, Entity)> ()
        .addProperty("enabled", &HitStopExempt::isEnabled, &HitStopExempt::setEnabled)
        .addProperty("includeChildren", &HitStopExempt::isIncludeChildren, &HitStopExempt::setIncludeChildren)
        .endClass();

#endif
}
