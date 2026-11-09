// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Screen transitions and the scripted glitch in Lua (docs/specs/S48-pixel-fx.md R14, R15, R19,
// R45): Transition (play, cover, reveal, loadScene, stop, its state), TransitionHandle (yieldable:
// Coroutine.waitFor polls its isDone), the TransitionKind enum and PixelFX.glitch. Kinds are
// names ("iris") or TransitionKind values; what a call leaves out comes from Project Settings >
// Graphics > Screen Transitions.
//
// Lua is compiled as C: luaL_error longjmps over C++ frames, so the functions that raise errors
// parse in helpers and raise only from frames holding nothing with a destructor.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Log.h"
#include "Scene.h"
#include "object/EntityHandle.h"
#include "pixelart/Glitch.h"
#include "pixelart/PixelColor.h"
#include "pixelart/PixelFxLua.h"
#include "pixelart/ScreenTransition.h"

#include <cstdio>
#include <string>

using namespace jasperengine;

namespace luabridge {
    template<> struct Stack<jasperengine::pixelart::TransitionKind> : EnumWrapper<jasperengine::pixelart::TransitionKind>{};
}

namespace jasperengine::pixelart {

    // the Transition table's C++ side (static members only)
    struct TransitionLua {};

}

#ifndef DISABLE_LUA_BINDINGS
namespace jasperengine::pixelart {
namespace {

    using ErrorText = char[512];

    bool fail(ErrorText& error, const char* function, const std::string& text){
        std::snprintf(error, sizeof(ErrorText), "%s: %s", function, text.c_str());
        return false;
    }

    bool kindArg(lua_State* L, int index, TransitionKind& kind, const char* function, ErrorText& error){
        if (lua_isnoneornil(L, index)){
            kind = ScreenTransition::getDefaults().kind;
            return true;
        }
        if (lua_type(L, index) == LUA_TNUMBER){
            const lua_Integer value = lua_tointeger(L, index);
            if (value < 0 || value >= kTransitionKindCount)
                return fail(error, function, "kind " + std::to_string((long long)value) + " is not a TransitionKind");
            kind = (TransitionKind)value;
            return true;
        }
        if (lua_type(L, index) == LUA_TSTRING && transitionKindFromName(lua_tostring(L, index), kind))
            return true;
        return fail(error, function, "kind must be iris, blinds, slash, pixel_dissolve, crt_off, burn_away or fade (or a TransitionKind)");
    }

    bool durationArg(lua_State* L, int index, float& seconds, const char* function, ErrorText& error){
        if (lua_isnoneornil(L, index)){
            seconds = ScreenTransition::getDefaults().duration;
            return true;
        }
        if (lua_type(L, index) != LUA_TNUMBER)
            return fail(error, function, "duration must be a number of seconds");
        seconds = (float)lua_tonumber(L, index);
        return true;
    }

    // a field by its camelCase or snake_case name, pushed (nil when neither is set)
    void getField(lua_State* L, int index, const char* camel, const char* snake){
        lua_getfield(L, index, camel);
        if (lua_isnil(L, -1) && snake){
            lua_pop(L, 1);
            lua_getfield(L, index, snake);
        }
    }

    bool numberField(lua_State* L, int index, const char* camel, const char* snake, float& out, const char* function, ErrorText& error){
        getField(L, index, camel, snake);
        bool ok = true;
        if (lua_type(L, -1) == LUA_TNUMBER) out = (float)lua_tonumber(L, -1);
        else if (!lua_isnil(L, -1)) ok = fail(error, function, std::string(camel) + " must be a number");
        lua_pop(L, 1);
        return ok;
    }

    bool intField(lua_State* L, int index, const char* camel, const char* snake, int& out, const char* function, ErrorText& error){
        float value = (float)out;
        if (!numberField(L, index, camel, snake, value, function, error)) return false;
        out = (int)value;
        return true;
    }

    bool boolField(lua_State* L, int index, const char* camel, const char* snake, bool& out, const char* function, ErrorText& error){
        getField(L, index, camel, snake);
        bool ok = true;
        if (lua_isboolean(L, -1)) out = lua_toboolean(L, -1) != 0;
        else if (!lua_isnil(L, -1)) ok = fail(error, function, std::string(camel) + " must be a boolean");
        lua_pop(L, 1);
        return ok;
    }

    bool stringField(lua_State* L, int index, const char* camel, const char* snake, std::string& out, const char* function, ErrorText& error){
        getField(L, index, camel, snake);
        bool ok = true;
        if (lua_type(L, -1) == LUA_TSTRING) out = lua_tostring(L, -1);
        else if (!lua_isnil(L, -1)) ok = fail(error, function, std::string(camel) + " must be a string");
        lua_pop(L, 1);
        return ok;
    }

    // opts: nil or a table (TransitionOptions in stubs/Transition.lua); onCovered optional
    bool optionsArg(lua_State* L, int index, TransitionOptions& options, std::function<void()>* onCovered, const char* function, ErrorText& error){
        options = TransitionOptions::fromDefaults();
        index = lua_absindex(L, index);
        if (lua_isnoneornil(L, index)) return true;
        if (!lua_istable(L, index)) return fail(error, function, std::string("options must be a table, got ") + luaL_typename(L, index));

        getField(L, index, "color", nullptr);
        if (lua_type(L, -1) == LUA_TSTRING){
            Rgba8 c;
            if (!parseHex(lua_tostring(L, -1), c)){
                lua_pop(L, 1);
                return fail(error, function, "color must be \"#rrggbb\" or a Vector4 (sRGB)");
            }
            options.ink = Vector4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
        }else if (luabridge::isInstance<Vector4>(L, -1)){
            options.ink = luabridge::Stack<Vector4>::get(L, -1).value();
        }else if (!lua_isnil(L, -1)){
            lua_pop(L, 1);
            return fail(error, function, "color must be \"#rrggbb\" or a Vector4 (sRGB)");
        }
        lua_pop(L, 1);

        std::string ease, shape;
        float seed = (float)options.seed;
        if (!stringField(L, index, "palette", nullptr, options.palette, function, error) ||
            !stringField(L, index, "ramp", nullptr, options.ramp, function, error) ||
            !intField(L, index, "rim", nullptr, options.rim, function, error) ||
            !numberField(L, index, "glow", nullptr, options.glow, function, error) ||
            !intField(L, index, "pixelSize", "pixel_size", options.pixelSize, function, error) ||
            !numberField(L, index, "seed", nullptr, seed, function, error) ||
            !stringField(L, index, "ease", nullptr, ease, function, error) ||
            !numberField(L, index, "hold", nullptr, options.hold, function, error) ||
            !stringField(L, index, "shape", nullptr, shape, function, error) ||
            !numberField(L, index, "angle", nullptr, options.angle, function, error) ||
            !intField(L, index, "count", nullptr, options.count, function, error) ||
            !boolField(L, index, "vertical", nullptr, options.vertical, function, error) ||
            !intField(L, index, "cell", nullptr, options.cell, function, error) ||
            !boolField(L, index, "throughRamp", "through_ramp", options.throughRamp, function, error)){
            return false;
        }
        options.seed = (uint32_t)std::max(seed, 0.0f);
        if (!ease.empty() && !transitionEaseFromName(ease, options.ease))
            return fail(error, function, "ease must be linear, in, out or in_out");
        if (!shape.empty() && !irisShapeFromName(shape, options.shape))
            return fail(error, function, "shape must be circle, diamond or square");
        if (options.rim < 0 || options.rim > kTransitionMaxRim)
            return fail(error, function, "rim must be 0..8 art pixels");

        getField(L, index, "center", nullptr);
        if (luabridge::isInstance<Vector2>(L, -1)){
            options.center = luabridge::Stack<Vector2>::get(L, -1).value();
        }else if (lua_istable(L, -1)){
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            options.center = Vector2((float)lua_tonumber(L, -2), (float)lua_tonumber(L, -1));
            lua_pop(L, 2);
        }else if (!lua_isnil(L, -1)){
            lua_pop(L, 1);
            return fail(error, function, "center must be a Vector2 (normalized, from the top-left)");
        }
        lua_pop(L, 1);

        getField(L, index, "centerObject", "center_object");
        if (luabridge::isInstance<EntityHandle>(L, -1)){
            auto handle = luabridge::get<EntityHandle*>(L, -1);
            if (handle && handle.value()){
                options.centerScene = handle.value()->getScene();
                options.centerObject = handle.value()->getEntity();
            }
        }else if (lua_istable(L, -1)){
            // a script instance: its scene and entity
            lua_getfield(L, -1, "scene");
            lua_getfield(L, -2, "entity");
            if (lua_isinteger(L, -1) && luabridge::isInstance<Scene>(L, -2)){
                auto scene = luabridge::get<Scene*>(L, -2);
                options.centerScene = scene ? scene.value() : nullptr;
                options.centerObject = (Entity)lua_tointeger(L, -1);
            }
            lua_pop(L, 2);
        }else if (!lua_isnil(L, -1)){
            lua_pop(L, 1);
            return fail(error, function, "centerObject must be a GameObject");
        }
        lua_pop(L, 1);

        if (onCovered){
            getField(L, index, "onCovered", "on_covered");
            if (lua_isfunction(L, -1)){
                luabridge::LuaRef callback = luabridge::LuaRef::fromStack(L, -1);
                *onCovered = [callback](){
                    auto called = callback();
                    if (!called)
                        Log::error("Transition onCovered: %s", called.errorMessage().c_str());
                };
            }else if (!lua_isnil(L, -1)){
                lua_pop(L, 1);
                return fail(error, function, "onCovered must be a function");
            }
            lua_pop(L, 1);
        }
        return true;
    }

    enum class Call { PLAY, COVER, REVEAL };

    // play / cover / reveal(kind?, duration?, opts?)
    int luaTransitionCall(lua_State* L, Call call, const char* function){
        ErrorText error;
        bool ok;
        {
            TransitionKind kind;
            float seconds = 0.0f;
            TransitionOptions options;
            std::function<void()> onCovered;
            ok = kindArg(L, 1, kind, function, error) && durationArg(L, 2, seconds, function, error) &&
                 optionsArg(L, 3, options, call == Call::REVEAL ? nullptr : &onCovered, function, error);
            if (ok){
                TransitionHandle handle;
                if (call == Call::PLAY) handle = ScreenTransition::play(kind, seconds, options, std::move(onCovered));
                else if (call == Call::COVER) handle = ScreenTransition::cover(kind, seconds, options, std::move(onCovered));
                else handle = ScreenTransition::reveal(kind, seconds, options);
                if (!luabridge::push(L, handle)) lua_pushnil(L);
            }
        }
        if (!ok) return luaL_error(L, "%s", error);
        return 1;
    }

    int luaTransitionPlay(lua_State* L){
        return luaTransitionCall(L, Call::PLAY, "Transition.play");
    }

    int luaTransitionCover(lua_State* L){
        return luaTransitionCall(L, Call::COVER, "Transition.cover");
    }

    int luaTransitionReveal(lua_State* L){
        return luaTransitionCall(L, Call::REVEAL, "Transition.reveal");
    }

    // loadScene(scene, kind?, duration?, opts?)
    int luaTransitionLoadScene(lua_State* L){
        const char* function = "Transition.loadScene";
        ErrorText error;
        bool ok;
        {
            TransitionKind kind;
            float seconds = 0.0f;
            TransitionOptions options;
            std::function<void()> onCovered;
            ok = lua_type(L, 1) == LUA_TSTRING ? true : fail(error, function, "scene must be a scene name");
            ok = ok && kindArg(L, 2, kind, function, error) && durationArg(L, 3, seconds, function, error) &&
                 optionsArg(L, 4, options, &onCovered, function, error);
            if (ok){
                const TransitionHandle handle = ScreenTransition::loadScene(lua_tostring(L, 1), kind, seconds, options, std::move(onCovered));
                if (!luabridge::push(L, handle)) lua_pushnil(L);
            }
        }
        if (!ok) return luaL_error(L, "%s", error);
        return 1;
    }

    std::string phaseName(){
        switch (ScreenTransition::getPhase()){
            case TransitionPhase::COVER: return "cover";
            case TransitionPhase::HOLD: return "hold";
            case TransitionPhase::REVEAL: return "reveal";
            case TransitionPhase::NONE: break;
        }
        return "none";
    }

    std::string kindName(){
        return transitionKindName(ScreenTransition::getKind());
    }

    void glitch(float seconds, float intensity){
        Glitch::burst(seconds, intensity);
    }

}
}
#endif

void LuaBinding::registerTransitionClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    using namespace jasperengine::pixelart;

    // The screen transitions' kinds (Transition.play's first argument; a name works as well)
    luabridge::getGlobalNamespace(L)
        .beginNamespace("TransitionKind")
        .addVariable("IRIS", TransitionKind::IRIS)
        .addVariable("BLINDS", TransitionKind::BLINDS)
        .addVariable("SLASH", TransitionKind::SLASH)
        .addVariable("PIXEL_DISSOLVE", TransitionKind::PIXEL_DISSOLVE)
        .addVariable("CRT_OFF", TransitionKind::CRT_OFF)
        .addVariable("BURN_AWAY", TransitionKind::BURN_AWAY)
        .addVariable("FADE", TransitionKind::FADE)
        .endNamespace();

    // A running screen transition (Transition.play, cover, reveal, loadScene); Coroutine.waitFor(handle) waits for it
    luabridge::getGlobalNamespace(L)
        .beginClass<TransitionHandle>("TransitionHandle")
        // True once it ended (or another transition replaced it); cover() ends when covered
        .addFunction("isDone", &TransitionHandle::isDone)
        // Ends it now if it is still the running one: the screen is uncovered at once
        .addFunction("stop", &TransitionHandle::stop)
        // 0..1 of its run (1 once done)
        .addProperty("progress", &TransitionHandle::getProgress)
        // False for the handle of a call that did nothing (an unknown scene in loadScene)
        .addProperty("valid", &TransitionHandle::isValid)
        .endClass();

    // Screen transitions over the whole frame (the HUD and the loading scene too): hard art-pixel
    // edges, rims from a palette ramp, unscaled time; they survive scene changes
    luabridge::getGlobalNamespace(L)
        .beginClass<TransitionLua>("Transition")
        .addStaticFunction("play", &luaTransitionPlay)
        .addStaticFunction("cover", &luaTransitionCover)
        .addStaticFunction("reveal", &luaTransitionReveal)
        .addStaticFunction("loadScene", &luaTransitionLoadScene)
        // Ends the running transition at once (the screen uncovered)
        .addStaticFunction("stop", +[](){ ScreenTransition::stop(); })
        // A transition is running (or holds the screen covered)
        .addStaticProperty("active", &ScreenTransition::isActive)
        // The screen is fully covered
        .addStaticProperty("covered", &ScreenTransition::isCovered)
        // How much of the screen the transition covers now, 0..1
        .addStaticProperty("coverage", &ScreenTransition::getCoverage)
        // "none", "cover", "hold" or "reveal"
        .addStaticProperty("phase", &phaseName)
        // The running (or last) transition's kind name
        .addStaticProperty("kind", &kindName)
        .endClass();

    // Pixel FX: the scripted glitch burst (the sprite effects are PixelFX.play)
    luabridge::getGlobalNamespace(L)
        .beginClass<PixelFxLua>("PixelFX")
        // Raises the main scene's Glitch to `intensity` (0..1) for `seconds` of unscaled time, without saving it; only where its post-processing has Glitch enabled
        .addStaticFunction("glitch", &glitch)
        .endClass();
#endif
}
