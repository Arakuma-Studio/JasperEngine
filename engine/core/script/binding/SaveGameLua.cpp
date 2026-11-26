// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's SaveGame module (docs/saved-data.md, docs/mcp/guide/lua-api.md "SaveGame"): slots of
// namespaces in the engine's save store (save/SaveGameStore.h), data versions and the game's
// migrations. Bound with the Lua C API, so its stub is hand-written (stubs/SaveGame.lua).
// Migrations live here, not in the store: the store keeps encoded sections, this file decodes
// them, runs the registered functions and gives the store the re-encoded result (markMigrated),
// which the files take at the next write (R9).

#include "LuaBinding.h"
#include "lua.hpp"

#include "Log.h"
#include "save/SaveGameStore.h"
#include "save/SaveValue.h"
#include "script/AsyncOwner.h"
#include "script/binding/NetClassesLua.h"

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;

#ifndef DISABLE_LUA_BINDINGS
namespace {

    using savegame::SaveError;
    using savegame::SaveErrorCode;

    const char* const MIGRATIONS_KEY = "jasperengine.savegame.migrations";
    const char* const OPTION_NAMES = "namespace, meta, requires, replaceAll, force, onComplete, owner";

    // Lua is built as C: lua_error longjmps over C++ destructors. The *Impl functions return
    // SAVE_RAISE with the message pushed once their own locals are gone; the lua_CFunction raises.
    constexpr int SAVE_RAISE = -1;

    int pushRaise(lua_State* L, const std::string& message){
        lua_pushlstring(L, message.data(), message.size());
        return SAVE_RAISE;
    }

    int saveFinish(lua_State* L, int results){
        if (results == SAVE_RAISE) return lua_error(L);
        return results;
    }

    bool isAbsent(lua_State* L, int index){
        return lua_type(L, index) == LUA_TNONE || lua_type(L, index) == LUA_TNIL;
    }

    std::string luaString(lua_State* L, int index){
        size_t n = 0;
        const char* s = lua_tolstring(L, index, &n);
        return s ? std::string(s, n) : std::string();
    }

    void setString(lua_State* L, const char* field, const std::string& value){
        lua_pushlstring(L, value.data(), value.size());
        lua_setfield(L, -2, field);
    }

    void setInteger(lua_State* L, const char* field, int64_t value){
        lua_pushinteger(L, static_cast<lua_Integer>(value));
        lua_setfield(L, -2, field);
    }

    void pushError(lua_State* L, const SaveError& error){
        lua_createtable(L, 0, 3);
        setString(L, "code", savegame::errorCodeName(error.code));
        setString(L, "message", error.message);
        if (!error.path.empty()) setString(L, "path", error.path);
    }

    // false, err
    int pushFailure(lua_State* L, const SaveError& error){
        lua_pushboolean(L, 0);
        pushError(L, error);
        return 2;
    }

    // nil, err
    int pushNilFailure(lua_State* L, const SaveError& error){
        lua_pushnil(L);
        pushError(L, error);
        return 2;
    }

    // An encoded payload as a Lua value, nil when empty or unreadable
    void pushPayload(lua_State* L, const std::string& payload){
        SaveError ignored;
        if (payload.empty() || !savegame::decodeLua(L, payload, ignored)) lua_pushnil(L);
    }

    void pushNames(lua_State* L, const std::vector<std::string>& names){
        lua_createtable(L, static_cast<int>(names.size()), 0);
        for (size_t i = 0; i < names.size(); i++){
            lua_pushlstring(L, names[i].data(), names[i].size());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
    }

    void pushSlotInfo(lua_State* L, const savegame::SlotInfo& info){
        lua_createtable(L, 0, 9);
        setString(L, "slot", info.slot);
        setInteger(L, "version", info.version);
        lua_pushnumber(L, info.savedAt);
        lua_setfield(L, -2, "savedAt");
        setInteger(L, "size", static_cast<int64_t>(info.size));
        setInteger(L, "generations", info.generations);
        pushNames(L, info.namespaces);
        lua_setfield(L, -2, "namespaces");
        pushPayload(L, info.meta);
        lua_setfield(L, -2, "meta");
        pushPayload(L, info.requirements);
        lua_setfield(L, -2, "requires");
        setString(L, "status", info.status);
    }

    void pushReadInfo(lua_State* L, const std::string& slot, const savegame::ReadInfo& info, uint32_t migratedFrom){
        lua_createtable(L, 0, 10);
        setString(L, "slot", slot);
        setInteger(L, "version", info.version);
        setInteger(L, "generation", static_cast<int64_t>(info.generation));
        lua_pushnumber(L, static_cast<double>(info.savedAtMs) / 1000.0);
        lua_setfield(L, -2, "savedAt");
        pushNames(L, info.namespaces);
        lua_setfield(L, -2, "namespaces");
        pushPayload(L, info.meta);
        lua_setfield(L, -2, "meta");
        pushPayload(L, info.requirements);
        lua_setfield(L, -2, "requires");
        if (migratedFrom) setInteger(L, "migratedFrom", migratedFrom);
        if (info.recoveredFrom) setInteger(L, "recoveredFrom", static_cast<int64_t>(info.recoveredFrom));
        if (!info.problems.empty()){
            pushNames(L, info.problems);
            lua_setfield(L, -2, "problems");
        }
    }

    // ---------------------------------------------------------------- callbacks

    struct LuaCallback {
        LuaRegistrySlot fn;
        AsyncOwner owner;
        std::string slot;
    };

    // A callback whose owner is gone (or whose Lua state was replaced) is dropped silently
    bool pushCallback(lua_State*& L, LuaCallback& callback){
        L = LuaBinding::getLuaState();
        if (!L || !callback.fn.isSet() || !AsyncOwners::alive(callback.owner)) return false;
        callback.fn.push(L);
        return true;
    }

    void runWriteCallback(LuaCallback& callback, const SaveError& error){
        lua_State* L = nullptr;
        if (pushCallback(L, callback)){
            const int top = lua_gettop(L) - 1;
            lua_pushboolean(L, error.ok() ? 1 : 0);
            if (error.ok()) lua_pushnil(L);
            else pushError(L, error);
            if (LuaBinding::pcallWithTraceback(L, 2, 0) != LUA_OK){
                Log::error("Lua error in the SaveGame onComplete of \"%s\": %s", callback.slot.c_str(),
                           LuaBinding::getLuaStackErrorString(L, -1).c_str());
            }
            lua_settop(L, top);
        }
        callback.fn.reset();
    }

    savegame::WriteCallback makeWriteCallback(lua_State* L, int fnIndex, const AsyncOwner& owner, const std::string& slot){
        if (!fnIndex) return {};
        auto callback = std::make_shared<LuaCallback>();
        callback->fn.set(L, fnIndex);
        callback->owner = owner;
        callback->slot = slot;
        return [callback](const SaveError& error){ runWriteCallback(*callback, error); };
    }

    // ---------------------------------------------------------------- migrations

    // registry[MIGRATIONS_KEY] pushed (made when missing)
    void pushMigrations(lua_State* L){
        if (lua_getfield(L, LUA_REGISTRYINDEX, MIGRATIONS_KEY) == LUA_TTABLE) return;
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, MIGRATIONS_KEY);
    }

    // The slot's namespaces brought to SaveGame.version: decoded, migrated in order and handed
    // back to the store (the files keep the old version until the next write). On success, when
    // `decoded` is set, the migrated namespace table stays pushed (nothing pushed otherwise).
    // migratedFrom: the version the data had (0: no migration ran)
    SaveError migrate(lua_State* L, const std::string& slot, savegame::SlotData& data, uint32_t& migratedFrom, bool keepTable){
        migratedFrom = 0;
        const uint32_t target = savegame::gameDataVersion();
        const uint32_t from = data.info.version;
        if (from >= target) return {};
        const int top = lua_gettop(L);
        pushMigrations(L);
        const int migrations = lua_gettop(L);
        for (uint32_t v = from; v < target; v++){
            if (lua_rawgeti(L, migrations, v) != LUA_TFUNCTION){
                lua_settop(L, top);
                return savegame::makeError(SaveErrorCode::Migration, "slot \"" + slot + "\" is version " + std::to_string(from) +
                    " and SaveGame.version is " + std::to_string(target) + ", but no migration from " + std::to_string(v) +
                    " is registered (SaveGame.addMigration(" + std::to_string(v) + ", fn))");
            }
            lua_pop(L, 1);
        }
        lua_createtable(L, 0, static_cast<int>(data.sections.size()));
        int namespaces = lua_gettop(L);
        for (const savegame::SaveSection& section : data.sections){
            SaveError error;
            if (!savegame::decodeLua(L, *section.payload, error)){
                lua_settop(L, top);
                error.message = "slot \"" + slot + "\" namespace \"" + section.name + "\": " + error.message;
                return error;
            }
            lua_setfield(L, namespaces, section.name.c_str());
        }
        for (uint32_t v = from; v < target; v++){
            lua_rawgeti(L, migrations, v);
            lua_pushvalue(L, namespaces);
            lua_createtable(L, 0, 3);
            setString(L, "slot", slot);
            setInteger(L, "fromVersion", v);
            setInteger(L, "toVersion", v + 1);
            if (LuaBinding::pcallWithTraceback(L, 2, 1) != LUA_OK){
                const std::string message = LuaBinding::getLuaStackErrorString(L, -1);
                lua_settop(L, top);
                return savegame::makeError(SaveErrorCode::Migration, "the migration from " + std::to_string(v) + " of slot \"" + slot +
                                       "\" failed: " + message);
            }
            if (!lua_istable(L, -1)){
                lua_settop(L, top);
                return savegame::makeError(SaveErrorCode::Migration, "the migration from " + std::to_string(v) + " of slot \"" + slot +
                                       "\" returned no table (return the namespaces)");
            }
            lua_replace(L, namespaces);
        }
        std::vector<savegame::SaveSection> sections;
        lua_pushnil(L);
        while (lua_next(L, namespaces) != 0){
            if (lua_type(L, -2) != LUA_TSTRING || !savegame::isValidName(lua_tostring(L, -2), savegame::MAX_SECTION_NAME)){
                const std::string key = lua_type(L, -2) == LUA_TSTRING ? luaString(L, -2) : std::string(luaL_typename(L, -2));
                lua_settop(L, top);
                return savegame::makeError(SaveErrorCode::Migration, "the migrations of slot \"" + slot + "\" returned the namespace \"" +
                                       key + "\": names use 1-32 characters of a-z, 0-9, '_' and '-'");
            }
            std::string payload;
            SaveError error;
            if (!savegame::encodeLua(L, -1, payload, error)){
                const std::string name = luaString(L, -2);
                lua_settop(L, top);
                error.message = "the migrations of slot \"" + slot + "\" namespace \"" + name + "\": " + error.message;
                return error;
            }
            sections.push_back({luaString(L, -2), std::make_shared<const std::string>(std::move(payload))});
            lua_pop(L, 1);
        }
        const SaveError error = savegame::SaveGameStore::instance().markMigrated(slot, target, sections);
        if (!error.ok()){
            lua_settop(L, top);
            return error;
        }
        data.sections = sections;
        data.info.version = target;
        data.info.namespaces.clear();
        for (const savegame::SaveSection& section : data.sections) data.info.namespaces.push_back(section.name);
        migratedFrom = from;
        if (keepTable){
            lua_pushvalue(L, namespaces);
            lua_replace(L, top + 1);
            lua_settop(L, top + 1);
        } else {
            lua_settop(L, top);
        }
        return {};
    }

    // Pushes data, info (or nil, err) for `nameSpace` of a slot the store returned
    // The store's migrator: a write at a newer version from C++ (the dialogue save hooks) or MCP
    // (user_settings set in Play) migrates the slot's other namespaces as SaveGame.write does
    SaveError migrateForStore(const std::string& slot, savegame::SlotData& data, uint32_t version){
        lua_State* L = LuaBinding::getLuaState();
        if (!L){
            return savegame::makeError(SaveErrorCode::Migration, "slot \"" + slot + "\" is version " + std::to_string(data.info.version) +
                " and this write is version " + std::to_string(version) + ", but no game is running to migrate its other namespaces: "
                "write it in Play, or write every namespace (replaceAll)");
        }
        if (version != savegame::gameDataVersion()){
            return savegame::makeError(SaveErrorCode::Migration, "slot \"" + slot + "\" is version " + std::to_string(data.info.version) +
                " and this write is version " + std::to_string(version) + ", but the game's migrations go to SaveGame.version " +
                std::to_string(savegame::gameDataVersion()) + ": write it at that version");
        }
        uint32_t migratedFrom = 0;
        return migrate(L, slot, data, migratedFrom, false);
    }

    int pushRead(lua_State* L, const std::string& slot, const std::string& nameSpace, savegame::SlotData& data){
        const uint32_t target = savegame::gameDataVersion();
        if (data.info.version > target){
            return pushNilFailure(L, savegame::makeError(SaveErrorCode::NewerVersion, "slot \"" + slot + "\" is version " +
                std::to_string(data.info.version) + " and SaveGame.version is " + std::to_string(target) +
                ": a newer build of the game wrote it; it is left as it is"));
        }
        uint32_t migratedFrom = 0;
        const int top = lua_gettop(L);
        const SaveError error = migrate(L, slot, data, migratedFrom, true);
        if (!error.ok()) return pushNilFailure(L, error);
        if (migratedFrom){
            lua_getfield(L, top + 1, nameSpace.c_str());
            lua_remove(L, top + 1);
            if (lua_isnil(L, -1)){
                lua_pop(L, 1);
                std::string names;
                for (const std::string& name : data.info.namespaces) names += (names.empty() ? "" : ", ") + name;
                return pushNilFailure(L, savegame::makeError(SaveErrorCode::NotFound, "slot \"" + slot + "\" has no namespace \"" +
                                      nameSpace + "\" (namespaces: " + (names.empty() ? std::string("none") : names) + ")"));
            }
        } else {
            const savegame::SaveSection* found = nullptr;
            for (const savegame::SaveSection& section : data.sections) if (section.name == nameSpace) found = &section;
            if (!found){
                std::string names;
                for (const std::string& name : data.info.namespaces) names += (names.empty() ? "" : ", ") + name;
                return pushNilFailure(L, savegame::makeError(SaveErrorCode::NotFound, "slot \"" + slot + "\" has no namespace \"" +
                                      nameSpace + "\" (namespaces: " + (names.empty() ? std::string("none") : names) + ")"));
            }
            SaveError decodeError;
            if (!savegame::decodeLua(L, *found->payload, decodeError)) return pushNilFailure(L, decodeError);
        }
        pushReadInfo(L, slot, data.info, migratedFrom);
        return 2;
    }

    // ---------------------------------------------------------------- arguments

    bool slotArgument(lua_State* L, int index, const char* fn, std::string& slot, std::string& raise){
        if (lua_type(L, index) != LUA_TSTRING){
            raise = std::string("SaveGame.") + fn + ": the slot must be a string, got " + luaL_typename(L, index);
            return false;
        }
        slot = luaString(L, index);
        return true;
    }

    std::set<std::string>& warnedOptions(){
        static std::set<std::string>* names = new std::set<std::string>();
        return *names;
    }

    struct ReadOptions {
        std::string nameSpace = "main";
        AsyncOwner owner;
    };

    int readOptions(lua_State* L, int index, const char* fn, ReadOptions& out){
        if (isAbsent(L, index)) return 0;
        if (!lua_istable(L, index)) return pushRaise(L, std::string("SaveGame.") + fn + ": options must be a table, got " + luaL_typename(L, index));
        index = lua_absindex(L, index);
        lua_pushnil(L);
        while (lua_next(L, index) != 0){
            const std::string key = lua_type(L, -2) == LUA_TSTRING ? luaString(L, -2) : std::string();
            if (key == "namespace"){
                if (lua_type(L, -1) != LUA_TSTRING) return pushRaise(L, std::string("SaveGame.") + fn + ": option 'namespace' must be a string");
                out.nameSpace = luaString(L, -1);
            } else if (key == "owner"){
                std::string error;
                if (!AsyncOwners::fromLua(L, lua_gettop(L), out.owner, error)) return pushRaise(L, std::string("SaveGame.") + fn + ": " + error);
            } else if (warnedOptions().insert(key).second){
                Log::warn("SaveGame.%s: unknown option \"%s\" is ignored (the options are namespace, owner)", fn, key.c_str());
            }
            lua_pop(L, 1);
        }
        return 0;
    }

    // ---------------------------------------------------------------- SaveGame.*

    int writeImpl(lua_State* L){
        std::string slot, raise;
        if (!slotArgument(L, 1, "write", slot, raise)) return pushRaise(L, raise);
        if (lua_gettop(L) < 2) lua_settop(L, 2);
        savegame::WriteOptions options;
        options.version = savegame::gameDataVersion();
        std::string nameSpace = "main";
        int completeIndex = 0;
        AsyncOwner owner;
        if (!isAbsent(L, 3)){
            if (!lua_istable(L, 3)) return pushRaise(L, std::string("SaveGame.write: options must be a table, got ") + luaL_typename(L, 3));
            lua_pushnil(L);
            while (lua_next(L, 3) != 0){
                const std::string key = lua_type(L, -2) == LUA_TSTRING ? luaString(L, -2) : std::string();
                const int value = lua_gettop(L);
                const int type = lua_type(L, value);
                if (key == "namespace"){
                    if (type != LUA_TSTRING) return pushRaise(L, "SaveGame.write: option 'namespace' must be a string");
                    nameSpace = luaString(L, value);
                } else if (key == "meta"){
                    if (type != LUA_TTABLE) return pushRaise(L, "SaveGame.write: option 'meta' must be a table");
                    SaveError error;
                    if (!savegame::encodeLua(L, value, options.meta, error)){
                        error.message = "meta: " + error.message;
                        lua_settop(L, 3);
                        return pushFailure(L, error);
                    }
                    options.hasMeta = true;
                } else if (key == "requires"){
                    if (type != LUA_TTABLE) return pushRaise(L, "SaveGame.write: option 'requires' must be a table {name = version}");
                    lua_pushnil(L);
                    while (lua_next(L, value) != 0){
                        if (lua_type(L, -2) != LUA_TSTRING || !lua_isinteger(L, -1))
                            return pushRaise(L, "SaveGame.write: option 'requires' maps content names to integer versions ({dlc1 = 2})");
                        lua_pop(L, 1);
                    }
                    SaveError error;
                    if (!savegame::encodeLua(L, value, options.requirements, error)) return pushFailure(L, error);
                    options.hasRequirements = true;
                } else if (key == "replaceAll"){
                    if (type != LUA_TBOOLEAN) return pushRaise(L, "SaveGame.write: option 'replaceAll' must be a boolean");
                    options.replaceAll = lua_toboolean(L, value) != 0;
                } else if (key == "force"){
                    if (type != LUA_TBOOLEAN) return pushRaise(L, "SaveGame.write: option 'force' must be a boolean");
                    options.force = lua_toboolean(L, value) != 0;
                } else if (key == "onComplete"){
                    if (type != LUA_TFUNCTION) return pushRaise(L, "SaveGame.write: option 'onComplete' must be a function(ok, err)");
                    completeIndex = 1;
                } else if (key == "owner"){
                    std::string error;
                    if (!AsyncOwners::fromLua(L, value, owner, error)) return pushRaise(L, "SaveGame.write: " + error);
                } else if (warnedOptions().insert(key).second){
                    Log::warn("SaveGame.write: unknown option \"%s\" is ignored (the options are %s)", key.c_str(), OPTION_NAMES);
                }
                lua_pop(L, 1);
            }
            if (completeIndex){
                lua_getfield(L, 3, "onComplete");
                completeIndex = lua_gettop(L);
            }
        }

        // the value as it is now (later changes to the table do not reach the save: R4)
        std::string payload;
        SaveError error;
        const auto started = std::chrono::steady_clock::now();
        if (!savegame::encodeLua(L, 2, payload, error)) return pushFailure(L, error);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (ms > 8.0 && warnedOptions().insert("slow:" + nameSpace).second){
            Log::warn("SaveGame: encoding \"%s\" took %.0f ms; split it into namespaces", nameSpace.c_str(), ms);
        }

        savegame::SaveGameStore& store = savegame::SaveGameStore::instance();
        // the slot's other namespaces are brought to this version first, so a write never mixes
        // data of two versions
        savegame::SlotData data;
        error = savegame::isValidName(slot, savegame::MAX_SLOT_NAME) ? store.readSlot(slot, data) : SaveError();
        if (error.ok() && !data.sections.empty()){
            uint32_t migratedFrom = 0;
            error = migrate(L, slot, data, migratedFrom, false);
            if (!error.ok()) return pushFailure(L, error);
        }
        error = store.write(slot, nameSpace, std::move(payload), options, makeWriteCallback(L, completeIndex, owner, slot));
        if (!error.ok()) return pushFailure(L, error);
        lua_pushboolean(L, 1);
        return 1;
    }

    int readImpl(lua_State* L){
        std::string slot, raise;
        if (!slotArgument(L, 1, "read", slot, raise)) return pushRaise(L, raise);
        ReadOptions options;
        if (readOptions(L, 2, "read", options) == SAVE_RAISE) return SAVE_RAISE;
        savegame::SlotData data;
        const SaveError error = savegame::SaveGameStore::instance().readSlot(slot, data);
        if (!error.ok()) return pushNilFailure(L, error);
        if (!savegame::isValidName(options.nameSpace, savegame::MAX_SECTION_NAME)){
            return pushNilFailure(L, savegame::makeError(SaveErrorCode::Name, "Namespace names use 1-32 characters of a-z, 0-9, '_' and '-' (got \"" +
                                  options.nameSpace + "\")"));
        }
        return pushRead(L, slot, options.nameSpace, data);
    }

    struct ReadCallback {
        LuaRegistrySlot fn;
        AsyncOwner owner;
        std::string slot;
        std::string nameSpace;
    };

    int readAsyncImpl(lua_State* L){
        std::string slot, raise;
        if (!slotArgument(L, 1, "readAsync", slot, raise)) return pushRaise(L, raise);
        if (lua_type(L, 2) != LUA_TFUNCTION) return pushRaise(L, std::string("SaveGame.readAsync: the callback must be a function(data, info), got ") + luaL_typename(L, 2));
        ReadOptions options;
        if (readOptions(L, 3, "readAsync", options) == SAVE_RAISE) return SAVE_RAISE;
        auto callback = std::make_shared<ReadCallback>();
        callback->fn.set(L, 2);
        callback->owner = options.owner;
        callback->slot = slot;
        callback->nameSpace = options.nameSpace;
        savegame::SaveGameStore::instance().readSlotAsync(slot, [callback](const SaveError& error, const savegame::SlotData& loaded){
            lua_State* state = LuaBinding::getLuaState();
            if (state && callback->fn.isSet() && AsyncOwners::alive(callback->owner)){
                const int top = lua_gettop(state);
                callback->fn.push(state);
                int results;
                if (!error.ok()) results = pushNilFailure(state, error);
                else {
                    savegame::SlotData data = loaded;
                    results = pushRead(state, callback->slot, callback->nameSpace, data);
                }
                if (LuaBinding::pcallWithTraceback(state, results, 0) != LUA_OK){
                    Log::error("Lua error in the SaveGame.readAsync callback of \"%s\": %s", callback->slot.c_str(),
                               LuaBinding::getLuaStackErrorString(state, -1).c_str());
                }
                lua_settop(state, top);
            }
            callback->fn.reset();
        });
        return 0;
    }

    int listImpl(lua_State* L){
        const std::vector<savegame::SlotInfo> slots = savegame::SaveGameStore::instance().list();
        lua_createtable(L, static_cast<int>(slots.size()), 0);
        for (size_t i = 0; i < slots.size(); i++){
            pushSlotInfo(L, slots[i]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    int infoImpl(lua_State* L){
        std::string slot, raise;
        if (!slotArgument(L, 1, "info", slot, raise)) return pushRaise(L, raise);
        savegame::SlotInfo info;
        const SaveError error = savegame::SaveGameStore::instance().info(slot, info);
        if (!error.ok()) return pushNilFailure(L, error);
        pushSlotInfo(L, info);
        return 1;
    }

    int existsImpl(lua_State* L){
        std::string slot, raise;
        if (!slotArgument(L, 1, "exists", slot, raise)) return pushRaise(L, raise);
        lua_pushboolean(L, savegame::SaveGameStore::instance().exists(slot) ? 1 : 0);
        return 1;
    }

    int deleteImpl(lua_State* L){
        std::string slot, raise;
        if (!slotArgument(L, 1, "delete", slot, raise)) return pushRaise(L, raise);
        const SaveError error = savegame::SaveGameStore::instance().remove(slot);
        if (!error.ok()) return pushFailure(L, error);
        lua_pushboolean(L, 1);
        return 1;
    }

    int flushImpl(lua_State* L){
        std::string slot;
        if (!isAbsent(L, 1)){
            std::string raise;
            if (!slotArgument(L, 1, "flush", slot, raise)) return pushRaise(L, raise);
        }
        const SaveError error = savegame::SaveGameStore::instance().flush(slot, std::chrono::seconds(2), "SaveGame.flush returned");
        if (!error.ok()) return pushFailure(L, error);
        lua_pushboolean(L, 1);
        return 1;
    }

    int getBackupsImpl(lua_State* L){
        std::string slot, raise;
        if (!slotArgument(L, 1, "getBackups", slot, raise)) return pushRaise(L, raise);
        std::vector<savegame::BackupInfo> backups;
        const SaveError error = savegame::SaveGameStore::instance().backups(slot, backups);
        if (!error.ok()) return pushNilFailure(L, error);
        lua_createtable(L, static_cast<int>(backups.size()), 0);
        for (size_t i = 0; i < backups.size(); i++){
            const savegame::BackupInfo& backup = backups[i];
            lua_createtable(L, 0, 6);
            setInteger(L, "generation", static_cast<int64_t>(backup.generation));
            lua_pushnumber(L, backup.savedAt);
            lua_setfield(L, -2, "savedAt");
            setInteger(L, "version", backup.version);
            setInteger(L, "size", static_cast<int64_t>(backup.size));
            lua_pushboolean(L, backup.valid ? 1 : 0);
            lua_setfield(L, -2, "valid");
            if (!backup.problem.empty()) setString(L, "problem", backup.problem);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    int restoreBackupImpl(lua_State* L){
        std::string slot, raise;
        if (!slotArgument(L, 1, "restoreBackup", slot, raise)) return pushRaise(L, raise);
        if (!lua_isinteger(L, 2) || lua_tointeger(L, 2) < 1)
            return pushRaise(L, "SaveGame.restoreBackup: the generation must be an integer >= 1 (SaveGame.getBackups lists them)");
        if (!isAbsent(L, 3) && lua_type(L, 3) != LUA_TFUNCTION)
            return pushRaise(L, "SaveGame.restoreBackup: onComplete must be a function(ok, err)");
        const uint64_t generation = static_cast<uint64_t>(lua_tointeger(L, 2));
        const SaveError error = savegame::SaveGameStore::instance().restore(slot, generation,
            makeWriteCallback(L, isAbsent(L, 3) ? 0 : 3, AsyncOwner(), slot));
        if (!error.ok()) return pushFailure(L, error);
        lua_pushboolean(L, 1);
        return 1;
    }

    int unloadImpl(lua_State* L){
        std::string slot;
        if (!isAbsent(L, 1)){
            std::string raise;
            if (!slotArgument(L, 1, "unload", slot, raise)) return pushRaise(L, raise);
        }
        savegame::SaveGameStore::instance().unload(slot);
        return 0;
    }

    int storageImpl(lua_State* L){
        const savegame::StorageInfo info = savegame::SaveGameStore::instance().storage();
        lua_createtable(L, 0, 3);
        setInteger(L, "used", info.used);
        setInteger(L, "quota", info.quota);
        setInteger(L, "free", info.free);
        return 1;
    }

    int addMigrationImpl(lua_State* L){
        if (!lua_isinteger(L, 1) || lua_tointeger(L, 1) < 1)
            return pushRaise(L, "SaveGame.addMigration: fromVersion must be an integer >= 1");
        if (lua_type(L, 2) != LUA_TFUNCTION)
            return pushRaise(L, "SaveGame.addMigration: the migration must be a function(namespaces, info) returning the namespaces");
        pushMigrations(L);
        lua_pushvalue(L, 2);
        lua_rawseti(L, -2, lua_tointeger(L, 1));
        lua_pop(L, 1);
        return 0;
    }

    int luaWrite(lua_State* L){ return saveFinish(L, writeImpl(L)); }
    int luaRead(lua_State* L){ return saveFinish(L, readImpl(L)); }
    int luaReadAsync(lua_State* L){ return saveFinish(L, readAsyncImpl(L)); }
    int luaList(lua_State* L){ return saveFinish(L, listImpl(L)); }
    int luaInfo(lua_State* L){ return saveFinish(L, infoImpl(L)); }
    int luaExists(lua_State* L){ return saveFinish(L, existsImpl(L)); }
    int luaDelete(lua_State* L){ return saveFinish(L, deleteImpl(L)); }
    int luaFlush(lua_State* L){ return saveFinish(L, flushImpl(L)); }
    int luaGetBackups(lua_State* L){ return saveFinish(L, getBackupsImpl(L)); }
    int luaRestoreBackup(lua_State* L){ return saveFinish(L, restoreBackupImpl(L)); }
    int luaUnload(lua_State* L){ return saveFinish(L, unloadImpl(L)); }
    int luaStorage(lua_State* L){ return saveFinish(L, storageImpl(L)); }
    int luaAddMigration(lua_State* L){ return saveFinish(L, addMigrationImpl(L)); }

    // SaveGame.version lives in the engine (the store and the tools read it)
    int luaIndex(lua_State* L){
        if (lua_type(L, 2) == LUA_TSTRING && std::string(lua_tostring(L, 2)) == "version"){
            lua_pushinteger(L, static_cast<lua_Integer>(savegame::gameDataVersion()));
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    int luaNewIndex(lua_State* L){
        if (lua_type(L, 2) == LUA_TSTRING && std::string(lua_tostring(L, 2)) == "version"){
            if (!lua_isinteger(L, 3) || lua_tointeger(L, 3) < 1 || lua_tointeger(L, 3) > 0x7FFFFFFF)
                return luaL_error(L, "SaveGame.version must be an integer >= 1");
            savegame::setGameDataVersion(static_cast<uint32_t>(lua_tointeger(L, 3)));
            return 0;
        }
        lua_rawset(L, 1);
        return 0;
    }

}
#endif //DISABLE_LUA_BINDINGS

void LuaBinding::registerSaveGameClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    static const luaL_Reg functions[] = {
        {"write", luaWrite},
        {"read", luaRead},
        {"readAsync", luaReadAsync},
        {"list", luaList},
        {"info", luaInfo},
        {"exists", luaExists},
        {"delete", luaDelete},
        {"flush", luaFlush},
        {"getBackups", luaGetBackups},
        {"restoreBackup", luaRestoreBackup},
        {"unload", luaUnload},
        {"storage", luaStorage},
        {"addMigration", luaAddMigration},
        {nullptr, nullptr},
    };
    // a new state (Play started again) starts at version 1 with no migrations
    savegame::setGameDataVersion(1);
    savegame::setMigrator(migrateForStore);
    luaL_newlib(L, functions);
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, luaIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, luaNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "SaveGame");
#endif //DISABLE_LUA_BINDINGS
}
