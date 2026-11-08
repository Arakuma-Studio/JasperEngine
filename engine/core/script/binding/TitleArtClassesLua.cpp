// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's TitleArt (docs/specs/S81-title-art-runtime.md R64-R67, §6.6): the Title Art Renderer's
// API as a GameObject subclass, GameObject:getTitleArt() and TitleArtRenderMode. Bound in core,
// never under JASPERENGINE_TITLE_ART, so a script naming it keeps a Baked-only game's smallest
// player variant (R52, AC14). An operation the mode or the document cannot do raises one
// sentence naming the title, what failed, why and the fix (R65); the raising calls are plain
// lua_CFunctions that free every C++ value before lua_error (Lua is built as C: longjmp).

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Scene.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "object/TitleArt.h"
#include "subsystem/TitleArtSystem.h"

#include <optional>
#include <string>
#include <vector>

using namespace jasperengine;

namespace luabridge
{
    template<> struct Stack<TitleArtRenderMode> : EnumWrapper<TitleArtRenderMode>{};
}

namespace {

    TitleArt* titleSelf(lua_State* L){
        auto self = luabridge::get<TitleArt*>(L, 1);
        if (!self || !*self)
            luaL_argerror(L, 1, "a TitleArt (call it with ':')");
        return *self;
    }

    // the value at `index` as a parameter value (number, boolean, string, Vector3/Vector4 linear)
    bool valueAt(lua_State* L, int index, TitleArtValue& out){
        switch (lua_type(L, index)){
            case LUA_TNUMBER: out = TitleArtValue::fromNumber(lua_tonumber(L, index)); return true;
            case LUA_TBOOLEAN: out = TitleArtValue::fromBool(lua_toboolean(L, index) != 0); return true;
            case LUA_TSTRING: out = TitleArtValue::fromString(lua_tostring(L, index)); return true;
            default: break;
        }
        if (auto v4 = luabridge::get<Vector4>(L, index)){
            out = TitleArtValue::fromColor(*v4, true);
            return true;
        }
        if (auto v3 = luabridge::get<Vector3>(L, index)){
            const Vector3 color = *v3;
            out = TitleArtValue::fromColor(Vector4(color.x, color.y, color.z, 1.0f), false);
            return true;
        }
        return false;
    }

    // pushes `message` and leaves only Lua values behind: the caller returns lua_error(L)
    void pushError(lua_State* L, const std::string& message){
        lua_pushlstring(L, message.data(), message.size());
    }

    // ---- the raising calls: each does its C++ work in a scope, then raises outside it ----

    bool doSetParameter(lua_State* L, TitleArt* self, const char* name, int valueIndex){
        TitleArtValue value;
        if (!valueAt(L, valueIndex, value)){
            pushError(L, "TitleArt '" + self->getName() + "': '" + name + "' takes a number, a boolean, a string or a Vector3/Vector4; got " +
                         luaL_typename(L, valueIndex) + ".");
            return false;
        }
        std::string error;
        if (!self->setParameter(name, value, &error)){
            pushError(L, error);
            return false;
        }
        return true;
    }

    int setParameter(lua_State* L){
        TitleArt* self = titleSelf(L);
        const char* name = luaL_checkstring(L, 2);
        if (!doSetParameter(L, self, name, 3)) return lua_error(L);
        return 0;
    }

    bool pushParameter(lua_State* L, TitleArt* self, const char* name){
        TitleArtValue value;
        std::string error;
        if (!self->getParameter(name, value, &error)){
            pushError(L, error);
            return false;
        }
        switch (value.kind){
            case TitleArtValue::Kind::Number: lua_pushnumber(L, value.number); break;
            case TitleArtValue::Kind::Bool: lua_pushboolean(L, value.boolean ? 1 : 0); break;
            case TitleArtValue::Kind::String: lua_pushlstring(L, value.text.data(), value.text.size()); break;
            case TitleArtValue::Kind::Color: (void)luabridge::push(L, value.color); break;
        }
        return true;
    }

    int getParameter(lua_State* L){
        TitleArt* self = titleSelf(L);
        const char* name = luaL_checkstring(L, 2);
        if (!pushParameter(L, self, name)) return lua_error(L);
        return 1;
    }

    bool doReset(lua_State* L, TitleArt* self, const char* name){
        std::string error;
        if (!self->resetParameter(name, &error)){
            pushError(L, error);
            return false;
        }
        return true;
    }

    int resetParameter(lua_State* L){
        TitleArt* self = titleSelf(L);
        const char* name = luaL_checkstring(L, 2);
        if (!doReset(L, self, name)) return lua_error(L);
        return 0;
    }

    bool doSetText(lua_State* L, TitleArt* self){
        if (lua_type(L, 2) != LUA_TSTRING && lua_type(L, 2) != LUA_TNUMBER){
            pushError(L, "TitleArt '" + self->getName() + "': 'text' is a string; got " + luaL_typename(L, 2) + ".");
            return false;
        }
        std::string error;
        if (!self->setText(lua_tostring(L, 2), &error)){
            pushError(L, error);
            return false;
        }
        return true;
    }

    int setText(lua_State* L){
        TitleArt* self = titleSelf(L);
        if (!doSetText(L, self)) return lua_error(L);
        return 0;
    }

    bool doSetColorScheme(lua_State* L, TitleArt* self){
        if (lua_type(L, 2) != LUA_TSTRING){
            pushError(L, "TitleArt '" + self->getName() + "': 'colorScheme' is a color scheme id (a string); got " + luaL_typename(L, 2) + ".");
            return false;
        }
        std::string error;
        if (!self->setColorScheme(lua_tostring(L, 2), &error)){
            pushError(L, error);
            return false;
        }
        return true;
    }

    int setColorScheme(lua_State* L){
        TitleArt* self = titleSelf(L);
        if (!doSetColorScheme(L, self)) return lua_error(L);
        return 0;
    }

    bool doSetNativeSize(lua_State* L, TitleArt* self){
        std::string error;
        if (!self->setNativeSize(&error)){
            pushError(L, error);
            return false;
        }
        return true;
    }

    int setNativeSize(lua_State* L){
        TitleArt* self = titleSelf(L);
        if (!doSetNativeSize(L, self)) return lua_error(L);
        return 0;
    }

    // R67: title and renderMode cannot change from a script
    bool doReadOnly(lua_State* L, TitleArt* self, const char* field){
        pushError(L, "TitleArt '" + self->getName() + "': '" + field + "' is read-only: a script cannot change the asset or the Render Mode (set "
                     "them in the Title Art Renderer, so the build knows every Realtime and Glyph Atlas title).");
        return false;
    }

    int setTitleReadOnly(lua_State* L){
        TitleArt* self = titleSelf(L);
        doReadOnly(L, self, "title");
        return lua_error(L);
    }

    int setModeReadOnly(lua_State* L){
        TitleArt* self = titleSelf(L);
        doReadOnly(L, self, "renderMode");
        return lua_error(L);
    }

    // TitleArt(scene, entity): an existing Title Art Renderer only (R64, R67)
    bool checkConstructible(lua_State* L, Scene* scene, Entity entity){
        if (!scene || !scene->isEntityCreated(entity) || !scene->isComponentRegistered<TitleArtRendererComponent>() ||
            !scene->findComponent<TitleArtRendererComponent>(entity)){
            const std::string name = scene && scene->isEntityCreated(entity) ? scene->getEntityName(entity) : std::to_string(entity);
            pushError(L, "TitleArt: GameObject '" + name + "' has no Title Art Renderer. Add one in the Inspector (Add Component > Rendering > "
                         "Title Art Renderer).");
            return false;
        }
        return true;
    }

    bool pushConstructed(lua_State* L){
        auto scene = luabridge::get<Scene*>(L, 2);
        const Entity entity = (Entity)luaL_checkinteger(L, 3);
        if (!checkConstructible(L, scene ? *scene : nullptr, entity)) return false;
        (void)luabridge::push(L, TitleArt(*scene, entity));
        return true;
    }

    // __call(class, scene, entity)
    int constructTitleArt(lua_State* L){
        if (!pushConstructed(L)) return lua_error(L);
        return 1;
    }

    luabridge::LuaRef stringList(lua_State* L, const std::vector<std::string>& values){
        luabridge::LuaRef table = luabridge::newTable(L);
        for (size_t i = 0; i < values.size(); i++) table[(int)i + 1] = values[i];
        return table;
    }

    // play(name, {speed, loop, unscaled})
    bool playArgs(lua_State* L, TitleArt* self){
        const char* name = luaL_checkstring(L, 2);
        float speed = 1.0f;
        bool loop = false, unscaled = false;
        if (lua_istable(L, 3)){
            lua_getfield(L, 3, "speed");
            if (lua_isnumber(L, -1)) speed = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, 3, "loop");
            loop = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            lua_getfield(L, 3, "unscaled");
            unscaled = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
        return self->play(name, speed, loop, unscaled);
    }

    // TitleArt.setLanguage(code): an invalid tag raises (S81 §6.6's sentence form), nothing changes
    int setGlobalLanguage(lua_State* L){
        const char* code = luaL_checkstring(L, 1);
        std::string error;
        if (!TitleArt::setGlobalLanguage(code, &error)) return luaL_error(L, "TitleArt: %s.", error.c_str());
        return 0;
    }

    int play(lua_State* L){
        TitleArt* self = titleSelf(L);
        lua_pushboolean(L, playArgs(L, self) ? 1 : 0);
        return 1;
    }

}

void LuaBinding::registerTitleArtClasses(lua_State *L){
    luabridge::getGlobalNamespace(L)
        .beginNamespace("TitleArtRenderMode")
        .addVariable("BAKED", TitleArtRenderMode::BAKED)
        .addVariable("REALTIME", TitleArtRenderMode::REALTIME)
        .addVariable("GLYPH_ATLAS", TitleArtRenderMode::GLYPH_ATLAS)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .deriveClass<TitleArt, GameObject>("TitleArt")
        .addProperty("valid", &TitleArt::isValid)
        .addProperty("title", &TitleArt::getTitle, &setTitleReadOnly)
        .addProperty("renderMode", &TitleArt::getRenderMode, &setModeReadOnly)
        .addProperty("text", &TitleArt::getText, &setText)
        .addProperty("colorScheme", &TitleArt::getColorScheme, &setColorScheme)
        .addProperty("colorSchemes", +[](const TitleArt* self, lua_State* L) { return stringList(L, self->getColorSchemes()); })
        .addProperty("parameterNames", +[](const TitleArt* self, lua_State* L) { return stringList(L, self->getParameterNames()); })
        .addProperty("ready", &TitleArt::isReady)
        .addProperty("pending", &TitleArt::getPending)
        .addProperty("failed", &TitleArt::isFailed)
        .addProperty("error", &TitleArt::getError)
        .addProperty("pixelPath", &TitleArt::getPixelPath)
        .addProperty("priority", &TitleArt::getPriority, &TitleArt::setPriority)
        .addProperty("liveTime", &TitleArt::getLiveTime, &TitleArt::setLiveTime)
        .addProperty("sweepPosition", &TitleArt::getSweepPosition, &TitleArt::setSweepPosition)
        .addProperty("sweepWidth", &TitleArt::getSweepWidth, &TitleArt::setSweepWidth)
        .addProperty("sweepAngle", &TitleArt::getSweepAngle, &TitleArt::setSweepAngle)
        .addProperty("sweepSoftness", &TitleArt::getSweepSoftness, &TitleArt::setSweepSoftness)
        .addProperty("sweepColor", &TitleArt::getSweepColor, &TitleArt::setSweepColor)
        .addProperty("sweepAuto", &TitleArt::isSweepAuto, &TitleArt::setSweepAuto)
        .addProperty("glowPulse", &TitleArt::getGlowPulse, &TitleArt::setGlowPulse)
        .addProperty("glowColor", &TitleArt::getGlowColor, &TitleArt::setGlowColor)
        .addProperty("tintColor", &TitleArt::getTintColor, &TitleArt::setTintColor)
        .addProperty("tintAmount", &TitleArt::getTintAmount, &TitleArt::setTintAmount)
        .addProperty("glintAmount", &TitleArt::getGlintAmount, &TitleArt::setGlintAmount)
        .addProperty("glintSize", &TitleArt::getGlintSize, &TitleArt::setGlintSize)
        .addProperty("glintRate", &TitleArt::getGlintRate, &TitleArt::setGlintRate)
        .addProperty("glintSeed", &TitleArt::getGlintSeed, &TitleArt::setGlintSeed)
        .addProperty("glintColor", &TitleArt::getGlintColor, &TitleArt::setGlintColor)
        .addProperty("animations", +[](const TitleArt* self, lua_State* L) { return stringList(L, self->getAnimations()); })
        .addProperty("isPlaying", &TitleArt::isPlaying)
        .addProperty("onReady", [](TitleArt* self) { return &self->getOnReady(); },
                                [](TitleArt* self, lua_State* L) { self->getOnReady() = L; })
        .addProperty("onFailed", [](TitleArt* self) { return &self->getOnFailed(); },
                                 [](TitleArt* self, lua_State* L) { self->getOnFailed() = L; })
        .addProperty("onMissingCharacters", [](TitleArt* self) { return &self->getOnMissingCharacters(); },
                                            [](TitleArt* self, lua_State* L) { self->getOnMissingCharacters() = L; })
        .addProperty("onAnimationFinished", [](TitleArt* self) { return &self->getOnAnimationFinished(); },
                                            [](TitleArt* self, lua_State* L) { self->getOnAnimationFinished() = L; })
        // S87: the renderer's language ("" = Auto) and its events
        .addProperty("language", &TitleArt::getLanguage, &TitleArt::setLanguage)
        .addProperty("onLanguageReady", [](TitleArt* self) { return &self->getOnLanguageReady(); },
                                        [](TitleArt* self, lua_State* L) { self->getOnLanguageReady() = L; })
        .addFunction("getResolvedLanguage", &TitleArt::getResolvedLanguage)
        .addFunction("getLanguages", +[](const TitleArt* self, lua_State* L) { return stringList(L, self->getLanguages()); })
        .addFunction("isLanguageReady", &TitleArt::isLanguageReady)
        .addStaticFunction("setLanguage", &setGlobalLanguage)
        .addStaticFunction("getLanguage", &TitleArt::getGlobalLanguage)
        .addStaticProperty("onLanguageChanged", +[]() { return &TitleArt::getOnLanguageChanged(); })
        .addFunction("setParameter", &setParameter)
        .addFunction("getParameter", &getParameter)
        .addFunction("hasParameter", &TitleArt::hasParameter)
        .addFunction("resetParameter", &resetParameter)
        .addFunction("setColorScheme", &setColorScheme)
        .addFunction("refresh", &TitleArt::refresh)
        .addFunction("setNativeSize", &setNativeSize)
        .addFunction("play", &play)
        .addFunction("stop", &TitleArt::stop)
        .addStaticFunction("setFrameBudget", &TitleArt::setFrameBudget)
        .addStaticFunction("getFrameBudget", &TitleArt::getFrameBudget)
        .endClass();

    // TitleArt(scene, entity): checked before anything is made, so a refused call raises with
    // nothing half-constructed (the class table's __call, LuaBridge's constructor slot)
    lua_getglobal(L, "TitleArt");
    if (lua_istable(L, -1) && lua_getmetatable(L, -1)){
        lua_pushcfunction(L, &constructTitleArt);
        lua_setfield(L, -2, "__call");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    // the Title Art Renderer of the GameObject, nil when it has none (reviewed neutral, R67)
    luabridge::getGlobalNamespace(L)
        .deriveClass<GameObject, EntityHandle>("GameObject")
        .addFunction("getTitleArt", +[](GameObject* self) -> std::optional<TitleArt> {
            Scene* scene = self->getScene();
            if (!scene || !scene->isEntityCreated(self->getEntity()) || !scene->isComponentRegistered<TitleArtRendererComponent>() ||
                !scene->findComponent<TitleArtRendererComponent>(self->getEntity())){
                return std::nullopt;
            }
            return TitleArt(scene, self->getEntity());
        })
        .endClass();
}
