// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Fog Profiles and Fog Zones in Lua (docs/specs/S101-vapor.md R36, R43): FogProfile (load, builtin,
// get, isSet, clone; a clone's set and clear), Fog's profile, blendTo, blending, getResolved,
// clearOverride and clearOverrides, and the FogZone GameObject. Colours are sRGB here, as Fog's
// own colour properties; the files and the components hold them linear.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Log.h"
#include "Scene.h"
#include "object/environment/Fog.h"
#include "object/environment/FogZone.h"
#include "render/FogProfile.h"
#include "render/FogResolve.h"
#include "util/Color.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>

using namespace jasperengine;

namespace jasperengine {

    // A Fog Profile as Lua holds it: the key the library knows it by (a path, builtin:<id>, or
    // the runtime:<n> of a clone); only a clone can be changed
    struct LuaFogProfile {
        std::string key;
        bool clone = false;
    };

}

#ifndef DISABLE_LUA_BINDINGS
namespace jasperengine::fogluabinding {
namespace {

    using ErrorText = char[1024];

    void copyError(ErrorText& error, const std::string& text) {
        std::snprintf(error, sizeof(ErrorText), "%s", text.c_str());
    }

    std::shared_ptr<const FogProfileData> dataOf(const LuaFogProfile& self) {
        return FogProfileLibrary::get(self.key);
    }

    std::string fieldNames() {
        std::string names;
        for (size_t i = 0; i < FOG_FIELD_COUNT; i++)
            names += (names.empty() ? "" : ", ") + std::string(FogFields::name(static_cast<FogField>(i)));
        return names;
    }

    bool fieldArg(lua_State* L, int index, const char* call, FogField& field, ErrorText& error) {
        if (lua_type(L, index) != LUA_TSTRING) {
            copyError(error, std::string(call) + ": give a field name (" + fieldNames() + ")");
            return false;
        }
        if (!FogFields::find(lua_tostring(L, index), field)) {
            copyError(error, std::string(call) + ": '" + lua_tostring(L, index) + "' is not a fog look field (" + fieldNames() + ")");
            return false;
        }
        return true;
    }

    // a field of `values` onto the stack, colours in sRGB
    void pushField(lua_State* L, const FogComponent& values, FogField field) {
        switch (FogFields::info(field).kind) {
            case FogFieldKind::Float:
            case FogFieldKind::Extinction: {
                float value;
                FogFields::read(values, field, &value);
                lua_pushnumber(L, value);
                break;
            }
            case FogFieldKind::Color: {
                float value[3];
                FogFields::read(values, field, value);
                (void)luabridge::Stack<Vector3>::push(L, Color::linearTosRGB(Vector3(value[0], value[1], value[2])));
                break;
            }
            case FogFieldKind::Vector3: {
                float value[3];
                FogFields::read(values, field, value);
                (void)luabridge::Stack<Vector3>::push(L, Vector3(value[0], value[1], value[2]));
                break;
            }
            case FogFieldKind::Bool:
                lua_pushboolean(L, values.skyFill ? 1 : 0);
                break;
            case FogFieldKind::Enum:
                (void)luabridge::Stack<FogColorMode>::push(L, values.colorMode);
                break;
        }
    }

    // the value at `index` into values.<field>
    bool readField(lua_State* L, int index, FogComponent& values, FogField field, const char* call, ErrorText& error) {
        const FogFieldKind kind = FogFields::info(field).kind;
        const std::string name = FogFields::name(field);
        switch (kind) {
            case FogFieldKind::Float:
            case FogFieldKind::Extinction:
                if (lua_type(L, index) != LUA_TNUMBER) break;
                {
                    const float value = (float)lua_tonumber(L, index);
                    FogFields::write(values, field, &value);
                }
                return true;
            case FogFieldKind::Color:
            case FogFieldKind::Vector3:
                if (!luabridge::isInstance<Vector3>(L, index)) break;
                {
                    Vector3 value = luabridge::Stack<Vector3>::get(L, index).value();
                    if (kind == FogFieldKind::Color) value = Color::sRGBToLinear(value);
                    const float in[3] = {value.x, value.y, value.z};
                    FogFields::write(values, field, in);
                }
                return true;
            case FogFieldKind::Bool:
                if (lua_type(L, index) != LUA_TBOOLEAN) break;
                values.skyFill = lua_toboolean(L, index) != 0;
                return true;
            case FogFieldKind::Enum:
                if (lua_type(L, index) == LUA_TNUMBER) {
                    const float value = (float)lua_tointeger(L, index);
                    FogFields::write(values, field, &value);
                    return true;
                }
                if (lua_type(L, index) == LUA_TSTRING && FogFields::findColorMode(lua_tostring(L, index), values.colorMode)) return true;
                break;
        }
        const char* expected = kind == FogFieldKind::Color ? "a Vector3 colour" : kind == FogFieldKind::Vector3 ? "a Vector3"
                             : kind == FogFieldKind::Bool ? "a boolean" : kind == FogFieldKind::Enum ? "a FogColorMode" : "a number";
        copyError(error, std::string(call) + ": " + name + " takes " + expected);
        return false;
    }

    std::optional<LuaFogProfile> loadProfile(const std::string& path) {
        std::shared_ptr<const FogProfileData> data = FogProfileLibrary::get(path);
        if (!data) {
            std::string reason;
            FogProfileLibrary::isMissing(path, &reason);
            Log::error("FogProfile.load(\"%s\"): %s", path.c_str(), reason.empty() ? "it does not read" : reason.c_str());
            return std::nullopt;
        }
        return LuaFogProfile{path, false};
    }

    std::optional<LuaFogProfile> builtinProfile(const std::string& id) {
        const FogProfileBuiltin* builtin = FogProfiles::findBuiltin(id);
        if (!builtin) {
            std::string ids;
            for (const FogProfileBuiltin& entry : FogProfiles::builtins()) ids += (ids.empty() ? "" : ", ") + std::string(entry.id);
            Log::error("FogProfile.builtin(\"%s\"): no such built-in (%s)", id.c_str(), ids.c_str());
            return std::nullopt;
        }
        return LuaFogProfile{FogProfiles::builtinKey(builtin->id), false};
    }

    LuaFogProfile cloneProfile(const LuaFogProfile* self) {
        std::shared_ptr<const FogProfileData> data = dataOf(*self);
        auto copy = std::make_shared<FogProfileData>(data ? *data : FogProfileData());
        return LuaFogProfile{FogProfileLibrary::addRuntime(copy), true};
    }

    int profileGet(lua_State* L) {
        ErrorText error;
        bool ok = false;
        {
            const LuaFogProfile* self = luabridge::detail::Userdata::get<LuaFogProfile>(L, 1, true);
            FogField field;
            if (!self) copyError(error, "FogProfile:get(field): call it on a FogProfile");
            else if (fieldArg(L, 2, "FogProfile:get", field, error)) {
                std::shared_ptr<const FogProfileData> data = dataOf(*self);
                if (data && data->isSet(field)) pushField(L, data->values, field);
                else lua_pushnil(L);
                ok = true;
            }
        }
        if (!ok) return luaL_error(L, "%s", error);
        return 1;
    }

    int profileIsSet(lua_State* L) {
        ErrorText error;
        bool ok = false;
        {
            const LuaFogProfile* self = luabridge::detail::Userdata::get<LuaFogProfile>(L, 1, true);
            FogField field;
            if (!self) copyError(error, "FogProfile:isSet(field): call it on a FogProfile");
            else if (fieldArg(L, 2, "FogProfile:isSet", field, error)) {
                std::shared_ptr<const FogProfileData> data = dataOf(*self);
                lua_pushboolean(L, data && data->isSet(field) ? 1 : 0);
                ok = true;
            }
        }
        if (!ok) return luaL_error(L, "%s", error);
        return 1;
    }

    // a loaded or built-in profile is shared by every Fog using it: only clones change (§6.1)
    std::shared_ptr<FogProfileData> writable(const LuaFogProfile* self, const char* call, ErrorText& error) {
        if (!self) {
            copyError(error, std::string(call) + ": call it on a FogProfile");
            return nullptr;
        }
        if (!self->clone) {
            copyError(error, std::string(call) + ": '" + self->key + "' is read-only (every Fog using it shares it); "
                      "change a copy: local p = profile:clone()");
            return nullptr;
        }
        std::shared_ptr<FogProfileData> data = FogProfileLibrary::getRuntime(self->key);
        if (!data) copyError(error, std::string(call) + ": this clone is gone (Play stopped)");
        return data;
    }

    int profileSet(lua_State* L) {
        ErrorText error;
        bool ok = false;
        {
            const LuaFogProfile* self = luabridge::detail::Userdata::get<LuaFogProfile>(L, 1, true);
            std::shared_ptr<FogProfileData> data = writable(self, "FogProfile:set", error);
            FogField field;
            if (data && fieldArg(L, 2, "FogProfile:set", field, error)) {
                FogComponent values = data->values;
                if (readField(L, 3, values, field, "FogProfile:set", error)) {
                    data->setFrom(values, field);
                    ok = true;
                }
            }
        }
        if (!ok) return luaL_error(L, "%s", error);
        return 0;
    }

    int profileClear(lua_State* L) {
        ErrorText error;
        bool ok = false;
        {
            const LuaFogProfile* self = luabridge::detail::Userdata::get<LuaFogProfile>(L, 1, true);
            std::shared_ptr<FogProfileData> data = writable(self, "FogProfile:clear", error);
            FogField field;
            if (data && fieldArg(L, 2, "FogProfile:clear", field, error)) {
                data->clear(field);
                ok = true;
            }
        }
        if (!ok) return luaL_error(L, "%s", error);
        return 0;
    }

    // nil, a FogProfile or a path (assets-relative .fogprofile, builtin:<id>) at `index`
    bool profileKeyArg(lua_State* L, int index, const char* call, std::string& key, ErrorText& error) {
        if (lua_isnoneornil(L, index)) {
            key.clear();
            return true;
        }
        if (lua_type(L, index) == LUA_TSTRING) {
            key = lua_tostring(L, index);
            return true;
        }
        if (luabridge::isInstance<LuaFogProfile>(L, index)) {
            key = luabridge::Stack<LuaFogProfile>::get(L, index).value().key;
            return true;
        }
        copyError(error, std::string(call) + ": give a FogProfile (FogProfile.load, FogProfile.builtin, profile:clone()), a path or nil");
        return false;
    }

    std::optional<LuaFogProfile> profileOfKey(const std::string& key) {
        if (key.empty()) return std::nullopt;
        return LuaFogProfile{key, FogProfileLibrary::isRuntimeKey(key)};
    }

    std::optional<LuaFogProfile> fogProfile(const Fog* fog) {
        return profileOfKey(fog->getProfile());
    }

    int setFogProfile(lua_State* L) {
        ErrorText error;
        bool ok = false;
        {
            Fog* fog = luabridge::detail::Userdata::get<Fog>(L, 1, false);
            std::string key;
            if (!fog) copyError(error, "Fog.profile: set it on a Fog");
            else if (profileKeyArg(L, 2, "Fog.profile", key, error)) {
                fog->setProfile(key);
                ok = true;
            }
        }
        if (!ok) return luaL_error(L, "%s", error);
        return 0;
    }

    int fogBlendTo(lua_State* L) {
        ErrorText error;
        bool ok = false;
        {
            Fog* fog = luabridge::detail::Userdata::get<Fog>(L, 1, false);
            std::string key;
            if (!fog) copyError(error, "Fog:blendTo(profile, seconds): call it on a Fog");
            else if (lua_type(L, 3) != LUA_TNUMBER) copyError(error, "Fog:blendTo(profile, seconds): seconds is a number (game time)");
            else if (profileKeyArg(L, 2, "Fog:blendTo", key, error)) {
                fog->blendTo(key, (float)lua_tonumber(L, 3));
                ok = true;
            }
        }
        if (!ok) return luaL_error(L, "%s", error);
        return 0;
    }

    int fogGetResolved(lua_State* L) {
        ErrorText error;
        bool ok = false;
        {
            Fog* fog = luabridge::detail::Userdata::get<Fog>(L, 1, false);
            FogField field;
            if (!fog) copyError(error, "Fog:getResolved(field): call it on a Fog");
            else if (fieldArg(L, 2, "Fog:getResolved", field, error)) {
                pushField(L, fog->getResolved(), field);
                ok = true;
            }
        }
        if (!ok) return luaL_error(L, "%s", error);
        return 1;
    }

    int fogClearOverride(lua_State* L) {
        ErrorText error;
        bool ok = false;
        {
            Fog* fog = luabridge::detail::Userdata::get<Fog>(L, 1, false);
            FogField field;
            if (!fog) copyError(error, "Fog:clearOverride(field): call it on a Fog");
            else if (fieldArg(L, 2, "Fog:clearOverride", field, error)) {
                fog->clearOverride(FogFields::name(field));
                ok = true;
            }
        }
        if (!ok) return luaL_error(L, "%s", error);
        return 0;
    }

    std::optional<LuaFogProfile> zoneProfile(const FogZone* zone) {
        return profileOfKey(zone->getProfile());
    }

    int setZoneProfile(lua_State* L) {
        ErrorText error;
        bool ok = false;
        {
            FogZone* zone = luabridge::detail::Userdata::get<FogZone>(L, 1, false);
            std::string key;
            if (!zone) copyError(error, "FogZone.profile: set it on a FogZone");
            else if (profileKeyArg(L, 2, "FogZone.profile", key, error)) {
                zone->setProfile(key);
                ok = true;
            }
        }
        if (!ok) return luaL_error(L, "%s", error);
        return 0;
    }

}
}
#endif // DISABLE_LUA_BINDINGS

void LuaBinding::registerFogClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    using namespace jasperengine::fogluabinding;

    luabridge::getGlobalNamespace(L)
        .beginClass<LuaFogProfile>("FogProfile")
        .addStaticFunction("load", &loadProfile)
        .addStaticFunction("builtin", &builtinProfile)
        .addProperty("key", +[](const LuaFogProfile* self) -> std::string { return self->key; })
        .addProperty("name", +[](const LuaFogProfile* self) -> std::string {
            std::shared_ptr<const FogProfileData> data = dataOf(*self);
            return data ? data->name : std::string();
        })
        .addProperty("description", +[](const LuaFogProfile* self) -> std::string {
            std::shared_ptr<const FogProfileData> data = dataOf(*self);
            return data ? data->description : std::string();
        })
        .addProperty("readOnly", +[](const LuaFogProfile* self) -> bool { return !self->clone; })
        .addFunction("get", &profileGet)
        .addFunction("isSet", &profileIsSet)
        .addFunction("clone", &cloneProfile)
        .addFunction("set", &profileSet)
        .addFunction("clear", &profileClear)
        .endClass();

    // after the GameObject classes, which register Fog
    luabridge::getGlobalNamespace(L)
        .beginClass<Fog>("Fog")
        .addProperty("profile", &fogProfile, &setFogProfile)
        .addProperty("blending", &Fog::isBlending)
        .addFunction("blendTo", &fogBlendTo)
        .addFunction("getResolved", &fogGetResolved)
        .addFunction("clearOverride", &fogClearOverride)
        .addFunction("clearOverrides", &Fog::clearOverrides)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<FogZone, GameObject>("FogZone")
        .addConstructor <void (*) (Scene*), void (*) (Scene*, Entity)> ()
        .addProperty("shape", &FogZone::getShape, &FogZone::setShape)
        .addProperty("size", &FogZone::getSize, (void(FogZone::*)(Vector3))&FogZone::setSize)
        .addFunction("setSize", (void(FogZone::*)(const float, const float, const float))&FogZone::setSize)
        .addProperty("blendDistance", &FogZone::getBlendDistance, &FogZone::setBlendDistance)
        .addProperty("priority", &FogZone::getPriority, &FogZone::setPriority)
        .addProperty("weight", &FogZone::getWeight, &FogZone::setWeight)
        .addProperty("profile", &zoneProfile, &setZoneProfile)
        .addProperty("currentWeight", &FogZone::getCurrentWeight)
        .endClass();
#endif
}
