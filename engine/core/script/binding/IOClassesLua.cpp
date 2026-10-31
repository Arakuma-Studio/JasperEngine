// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "io/Data.h"
#include "io/File.h"
#include "io/FileData.h"
#include "io/FileUtils.h"
#include "io/UserSettings.h"
#include "script/LuaJson.h"
#include "util/NumberText.h"

#include <climits>
#include <string>
#include <vector>

using namespace jasperengine;

#ifndef DISABLE_LUA_BINDINGS
namespace {

    struct LuaJsonModule {};
    struct PlayerPrefsModule {};

    // Lua is built as C: lua_error longjmps over C++ destructors. The *Impl functions below
    // return RAISE with the message pushed, once their own locals are gone, and the lua_CFunction
    // raises it.
    constexpr int RAISE = -1;

    int pushRaise(lua_State* L, const std::string& message){
        lua_pushlstring(L, message.data(), message.size());
        return RAISE;
    }

    int pushFailure(lua_State* L, const std::string& message){
        lua_pushnil(L);
        lua_pushlstring(L, message.data(), message.size());
        return 2;
    }

    int finish(lua_State* L, int results){
        if (results == RAISE) return lua_error(L);
        return results;
    }

    bool isAbsent(lua_State* L, int index){
        return lua_type(L, index) == LUA_TNONE || lua_type(L, index) == LUA_TNIL;
    }

    bool stringArgument(lua_State* L, int index, std::string& out){
        if (lua_type(L, index) != LUA_TSTRING) return false;
        size_t n = 0;
        const char* s = lua_tolstring(L, index, &n);
        out.assign(s, n);
        return true;
    }

    // ---------------------------------------------------------------- Json options

    bool encodeOptions(lua_State* L, int index, const char* fn, LuaJson::EncodeOptions& out, std::string& error){
        out = LuaJson::EncodeOptions();
        if (isAbsent(L, index)) return true;
        if (!lua_istable(L, index)){
            error = std::string(fn) + ": options must be a table, got " + luaL_typename(L, index);
            return false;
        }
        index = lua_absindex(L, index);
        bool pretty = false;
        int indent = -1;
        lua_pushnil(L);
        while (lua_next(L, index) != 0){
            const std::string key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
            bool ok = true;
            if (key == "pretty"){
                ok = lua_isboolean(L, -1);
                if (ok) pretty = lua_toboolean(L, -1) != 0;
                else error = std::string(fn) + ": option 'pretty' must be a boolean";
            } else if (key == "indent"){
                ok = lua_isinteger(L, -1) && lua_tointeger(L, -1) >= 0 && lua_tointeger(L, -1) <= 64;
                if (ok) indent = static_cast<int>(lua_tointeger(L, -1));
                else error = std::string(fn) + ": option 'indent' must be an integer from 0 to 64";
            } else if (key == "invalidUtf8"){
                const std::string value = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
                if (value == "error") out.utf8 = LuaJson::InvalidUtf8::Error;
                else if (value == "replace") out.utf8 = LuaJson::InvalidUtf8::Replace;
                else if (value == "ignore") out.utf8 = LuaJson::InvalidUtf8::Ignore;
                else { ok = false; error = std::string(fn) + ": option 'invalidUtf8' must be \"error\", \"replace\" or \"ignore\""; }
            } else if (key == "emptyTable"){
                const std::string value = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
                if (value == "array") out.emptyAsObject = false;
                else if (value == "object") out.emptyAsObject = true;
                else { ok = false; error = std::string(fn) + ": option 'emptyTable' must be \"array\" or \"object\""; }
            } else {
                ok = false;
                error = std::string(fn) + ": unknown option " +
                        (key.empty() ? std::string("(not a string key)") : "'" + key + "'") +
                        " (options: pretty, indent, invalidUtf8, emptyTable)";
            }
            if (!ok){
                lua_pop(L, 2);
                return false;
            }
            lua_pop(L, 1);
        }
        out.indent = indent >= 0 ? indent : (pretty ? 2 : -1);
        return true;
    }

    bool decodeOptions(lua_State* L, int index, const char* fn, LuaJson::DecodeOptions& out, std::string& error){
        out = LuaJson::DecodeOptions();
        if (isAbsent(L, index)) return true;
        if (!lua_istable(L, index)){
            error = std::string(fn) + ": options must be a table, got " + luaL_typename(L, index);
            return false;
        }
        index = lua_absindex(L, index);
        lua_pushnil(L);
        while (lua_next(L, index) != 0){
            const std::string key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
            bool ok = true;
            if (key == "null"){
                const std::string value = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
                if (value == "sentinel") out.nullAsNil = false;
                else if (value == "nil") out.nullAsNil = true;
                else { ok = false; error = std::string(fn) + ": option 'null' must be \"sentinel\" or \"nil\""; }
            } else if (key == "allowComments"){
                ok = lua_isboolean(L, -1);
                if (ok) out.allowComments = lua_toboolean(L, -1) != 0;
                else error = std::string(fn) + ": option 'allowComments' must be a boolean";
            } else {
                ok = false;
                error = std::string(fn) + ": unknown option " +
                        (key.empty() ? std::string("(not a string key)") : "'" + key + "'") + " (options: null, allowComments)";
            }
            if (!ok){
                lua_pop(L, 2);
                return false;
            }
            lua_pop(L, 1);
        }
        return true;
    }

    // ---------------------------------------------------------------- Json

    int jsonEncodeImpl(lua_State* L, bool tryMode){
        const char* fn = tryMode ? "Json.tryEncode" : "Json.encode";
        std::string error, out;
        LuaJson::EncodeOptions opts;
        if (!encodeOptions(L, 2, fn, opts, error)) return pushRaise(L, error);
        if (lua_gettop(L) == 0) lua_pushnil(L);
        if (!LuaJson::encode(L, 1, out, opts, error)){
            if (tryMode) return pushFailure(L, error);
            return pushRaise(L, std::string(fn) + ": " + error);
        }
        lua_pushlstring(L, out.data(), out.size());
        if (!tryMode) return 1;
        lua_pushnil(L);   // value, nil: as a failure's nil, message
        return 2;
    }

    int jsonDecodeImpl(lua_State* L, bool tryMode){
        const char* fn = tryMode ? "Json.tryDecode" : "Json.decode";
        std::string error;
        LuaJson::DecodeOptions opts;
        if (!decodeOptions(L, 2, fn, opts, error)) return pushRaise(L, error);
        if (lua_type(L, 1) != LUA_TSTRING){
            const std::string message = std::string("text must be a string, got ") + luaL_typename(L, 1);
            if (tryMode) return pushFailure(L, message);
            return pushRaise(L, std::string(fn) + ": " + message);
        }
        size_t n = 0;
        const char* text = lua_tolstring(L, 1, &n);
        if (!LuaJson::decode(L, std::string_view(text, n), opts, error)){
            if (tryMode) return pushFailure(L, error);
            return pushRaise(L, std::string(fn) + ": " + error);
        }
        if (!tryMode) return 1;
        lua_pushnil(L);   // a JSON null is Json.null, nil: nil first always means failure
        return 2;
    }

    int luaJsonEncode(lua_State* L){ return finish(L, jsonEncodeImpl(L, false)); }
    int luaJsonTryEncode(lua_State* L){ return finish(L, jsonEncodeImpl(L, true)); }
    int luaJsonDecode(lua_State* L){ return finish(L, jsonDecodeImpl(L, false)); }
    int luaJsonTryDecode(lua_State* L){ return finish(L, jsonDecodeImpl(L, true)); }

    int luaJsonIsNull(lua_State* L){
        lua_pushboolean(L, LuaJson::isNull(L, 1) ? 1 : 0);
        return 1;
    }

    int jsonMarkImpl(lua_State* L, bool array){
        if (isAbsent(L, 1)){
            lua_settop(L, 0);
            lua_newtable(L);
        } else if (!lua_istable(L, 1)){
            return pushRaise(L, std::string(array ? "Json.array" : "Json.object") + ": a table expected, got " +
                                luaL_typename(L, 1));
        }
        lua_settop(L, 1);
        if (array) LuaJson::markArray(L, 1);
        else LuaJson::markObject(L, 1);
        return 1;
    }

    int luaJsonArray(lua_State* L){ return finish(L, jsonMarkImpl(L, true)); }
    int luaJsonObject(lua_State* L){ return finish(L, jsonMarkImpl(L, false)); }

    int jsonReadFileImpl(lua_State* L){
        std::string path, error, text;
        if (!stringArgument(L, 1, path))
            return pushRaise(L, std::string("Json.readFile: path must be a string, got ") + luaL_typename(L, 1));
        LuaJson::DecodeOptions opts;
        if (!decodeOptions(L, 2, "Json.readFile", opts, error)) return pushRaise(L, error);
        if (!FileUtils::readText(path, text, error)) return pushFailure(L, error);
        if (!LuaJson::decode(L, text, opts, error)) return pushFailure(L, path + ": " + error);
        return 1;
    }

    int jsonWriteFileImpl(lua_State* L){
        std::string path, error, text;
        if (!stringArgument(L, 1, path))
            return pushRaise(L, std::string("Json.writeFile: path must be a string, got ") + luaL_typename(L, 1));
        LuaJson::EncodeOptions opts;
        if (!encodeOptions(L, 3, "Json.writeFile", opts, error)) return pushRaise(L, error);
        if (lua_type(L, 2) == LUA_TNONE) return pushRaise(L, "Json.writeFile: a value to write expected");
        if (!LuaJson::encode(L, 2, text, opts, error)) return pushFailure(L, error);
        if (!FileUtils::writeText(path, text, error)) return pushFailure(L, error);
        lua_pushboolean(L, 1);
        return 1;
    }

    int luaJsonReadFile(lua_State* L){ return finish(L, jsonReadFileImpl(L)); }
    int luaJsonWriteFile(lua_State* L){ return finish(L, jsonWriteFileImpl(L)); }

    luabridge::LuaRef jsonNull(lua_State* L){
        LuaJson::pushNull(L);
        return luabridge::LuaRef::fromStack(L);
    }

    // ---------------------------------------------------------------- File helpers

    int fileTextImpl(lua_State* L, const char* fn, bool needsText, std::string& path, std::string& text){
        if (!stringArgument(L, 1, path))
            return pushRaise(L, std::string(fn) + ": path must be a string, got " + luaL_typename(L, 1));
        if (needsText && !stringArgument(L, 2, text))
            return pushRaise(L, std::string(fn) + ": text must be a string, got " + luaL_typename(L, 2));
        return 0;
    }

    int fileReadTextImpl(lua_State* L){
        std::string path, text, error;
        if (fileTextImpl(L, "File.readText", false, path, text) == RAISE) return RAISE;
        if (!FileUtils::readText(path, text, error)) return pushFailure(L, error);
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }

    int fileWriteTextImpl(lua_State* L, bool append){
        std::string path, text, error;
        if (fileTextImpl(L, append ? "File.appendText" : "File.writeText", true, path, text) == RAISE) return RAISE;
        const bool ok = append ? FileUtils::appendText(path, text, error) : FileUtils::writeText(path, text, error);
        if (!ok) return pushFailure(L, error);
        lua_pushboolean(L, 1);
        return 1;
    }

    int fileExistsImpl(lua_State* L){
        std::string path, text;
        if (fileTextImpl(L, "File.exists", false, path, text) == RAISE) return RAISE;
        lua_pushboolean(L, FileUtils::exists(path) ? 1 : 0);
        return 1;
    }

    int filePathOpImpl(lua_State* L, const char* fn, bool (*op)(const std::string&, std::string&)){
        std::string path, text, error;
        if (fileTextImpl(L, fn, false, path, text) == RAISE) return RAISE;
        if (!op(path, error)) return pushFailure(L, error);
        lua_pushboolean(L, 1);
        return 1;
    }

    int fileListImpl(lua_State* L){
        std::string path, text, error;
        if (fileTextImpl(L, "File.list", false, path, text) == RAISE) return RAISE;
        std::vector<std::string> names;
        if (!FileUtils::list(path, names, error)) return pushFailure(L, error);
        lua_createtable(L, static_cast<int>(names.size()), 0);
        for (size_t i = 0; i < names.size(); i++){
            lua_pushlstring(L, names[i].data(), names[i].size());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        return 1;
    }

    int luaFileReadText(lua_State* L){ return finish(L, fileReadTextImpl(L)); }
    int luaFileWriteText(lua_State* L){ return finish(L, fileWriteTextImpl(L, false)); }
    int luaFileAppendText(lua_State* L){ return finish(L, fileWriteTextImpl(L, true)); }
    int luaFileExists(lua_State* L){ return finish(L, fileExistsImpl(L)); }
    int luaFileRemove(lua_State* L){ return finish(L, filePathOpImpl(L, "File.remove", &FileUtils::remove)); }
    int luaFileMakeDirs(lua_State* L){ return finish(L, filePathOpImpl(L, "File.makeDirs", &FileUtils::makeDirs)); }
    int luaFileList(lua_State* L){ return finish(L, fileListImpl(L)); }

    // ---------------------------------------------------------------- PlayerPrefs

    int prefsKeyImpl(lua_State* L, const char* fn, std::string& key){
        if (!stringArgument(L, 1, key) || key.empty())
            return pushRaise(L, std::string("PlayerPrefs.") + fn + ": key must be a non-empty string, got " +
                                (lua_type(L, 1) == LUA_TSTRING ? "\"\"" : luaL_typename(L, 1)));
        return 0;
    }

    // An integer, or a float with an integer value (6 / 2), within the 32-bit int range
    int prefsIntImpl(lua_State* L, int index, const char* fn, int& out){
        int isInteger = 0;
        const lua_Integer value = lua_type(L, index) == LUA_TNUMBER ? lua_tointegerx(L, index, &isInteger) : 0;
        if (!isInteger){
            const std::string got = lua_type(L, index) == LUA_TNUMBER ? NumberText::formatDouble(lua_tonumber(L, index))
                                                                      : std::string(luaL_typename(L, index));
            return pushRaise(L, std::string("PlayerPrefs.") + fn + ": integer expected, got " + got);
        }
        if (value < INT_MIN || value > INT_MAX)
            return pushRaise(L, std::string("PlayerPrefs.") + fn + ": " + std::to_string(value) +
                                " is outside the 32-bit int range (UserSettings.setLongForKey stores larger ones)");
        out = static_cast<int>(value);
        return 0;
    }

    int prefsGetIntImpl(lua_State* L){
        std::string key;
        int fallback = 0;
        if (prefsKeyImpl(L, "getInt", key) == RAISE) return RAISE;
        if (!isAbsent(L, 2) && prefsIntImpl(L, 2, "getInt", fallback) == RAISE) return RAISE;
        lua_pushinteger(L, UserSettings::getIntegerForKey(key.c_str(), fallback));
        return 1;
    }

    int prefsSetIntImpl(lua_State* L){
        std::string key;
        int value = 0;
        if (prefsKeyImpl(L, "setInt", key) == RAISE) return RAISE;
        if (prefsIntImpl(L, 2, "setInt", value) == RAISE) return RAISE;
        UserSettings::setIntegerForKey(key.c_str(), value);
        return 0;
    }

    int prefsGetFloatImpl(lua_State* L){
        std::string key;
        float fallback = 0.0f;
        if (prefsKeyImpl(L, "getFloat", key) == RAISE) return RAISE;
        if (!isAbsent(L, 2)){
            if (lua_type(L, 2) != LUA_TNUMBER)
                return pushRaise(L, std::string("PlayerPrefs.getFloat: the default must be a number, got ") + luaL_typename(L, 2));
            fallback = static_cast<float>(lua_tonumber(L, 2));
        }
        lua_pushnumber(L, static_cast<lua_Number>(UserSettings::getFloatForKey(key.c_str(), fallback)));
        return 1;
    }

    int prefsSetFloatImpl(lua_State* L){
        std::string key;
        if (prefsKeyImpl(L, "setFloat", key) == RAISE) return RAISE;
        if (lua_type(L, 2) != LUA_TNUMBER)
            return pushRaise(L, std::string("PlayerPrefs.setFloat: number expected, got ") + luaL_typename(L, 2));
        UserSettings::setFloatForKey(key.c_str(), static_cast<float>(lua_tonumber(L, 2)));
        return 0;
    }

    int prefsGetStringImpl(lua_State* L){
        std::string key, fallback;
        if (prefsKeyImpl(L, "getString", key) == RAISE) return RAISE;
        if (!isAbsent(L, 2) && !stringArgument(L, 2, fallback))
            return pushRaise(L, std::string("PlayerPrefs.getString: the default must be a string, got ") + luaL_typename(L, 2));
        const std::string value = UserSettings::getStringForKey(key.c_str(), fallback);
        lua_pushlstring(L, value.data(), value.size());
        return 1;
    }

    int prefsSetStringImpl(lua_State* L){
        std::string key, value;
        if (prefsKeyImpl(L, "setString", key) == RAISE) return RAISE;
        if (!stringArgument(L, 2, value))
            return pushRaise(L, std::string("PlayerPrefs.setString: string expected, got ") + luaL_typename(L, 2));
        UserSettings::setStringForKey(key.c_str(), value);
        return 0;
    }

    int luaPrefsGetInt(lua_State* L){ return finish(L, prefsGetIntImpl(L)); }
    int luaPrefsSetInt(lua_State* L){ return finish(L, prefsSetIntImpl(L)); }
    int luaPrefsGetFloat(lua_State* L){ return finish(L, prefsGetFloatImpl(L)); }
    int luaPrefsSetFloat(lua_State* L){ return finish(L, prefsSetFloatImpl(L)); }
    int luaPrefsGetString(lua_State* L){ return finish(L, prefsGetStringImpl(L)); }
    int luaPrefsSetString(lua_State* L){ return finish(L, prefsSetStringImpl(L)); }

}
#endif //DISABLE_LUA_BINDINGS

void LuaBinding::registerIOClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginNamespace("FileErrors")
        .addVariable("FILEDATA_OK", FileErrors::FILEDATA_OK)
        .addVariable("INVALID_PARAMETER", FileErrors::INVALID_PARAMETER)
        .addVariable("FILE_NOT_FOUND", FileErrors::FILE_NOT_FOUND)
        .addVariable("OUT_OF_MEMORY", FileErrors::OUT_OF_MEMORY)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<FileData>("FileData")
        .addStaticFunction("newFile", 
            luabridge::overload<bool>(&FileData::newFile),
            luabridge::overload<const char*, bool>(&FileData::newFile))
        .addStaticFunction("getBaseDir", &FileData::getBaseDir)
        .addStaticFunction("getFilePathExtension", &FileData::getFilePathExtension)
        .addStaticFunction("getSystemPath", &FileData::getSystemPath)
        .addFunction("read8", &FileData::read8)
        .addFunction("read16", &FileData::read16)
        .addFunction("read32", &FileData::read32)
        .addFunction("eof", &FileData::eof)
        .addFunction("length", &FileData::length)
        .addFunction("seek", &FileData::seek)
        .addFunction("pos", &FileData::pos)
        .addFunction("readString", 
            luabridge::overload<>(&FileData::readString),
            luabridge::overload<unsigned int>(&FileData::readString))
        .addFunction("writeString", &FileData::writeString)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<File, FileData>("File")
        .addConstructor<void(), void(const char*, bool)> ()
        .addFunction("open", &File::open)
        .addFunction("flush", &File::flush)
        .addFunction("close", &File::close)
        // whole-file helpers for saves and data files (FileUtils: data://, asset://, lua://)
        .addStaticFunction("readText", &luaFileReadText)
        .addStaticFunction("writeText", &luaFileWriteText)
        .addStaticFunction("appendText", &luaFileAppendText)
        .addStaticFunction("exists", &luaFileExists)
        .addStaticFunction("remove", &luaFileRemove)
        .addStaticFunction("makeDirs", &luaFileMakeDirs)
        .addStaticFunction("list", &luaFileList)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Data, FileData>("Data")
        .addConstructor<void(), void(const char*)> ()
        .addFunction("open", (unsigned int(Data::*)(const char *))&Data::open)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<UserSettings>("UserSettings")
        .addStaticFunction("getBoolForKey", 
            luabridge::overload<const char*>(&UserSettings::getBoolForKey),
            luabridge::overload<const char*, bool>(&UserSettings::getBoolForKey))
        .addStaticFunction("getIntegerForKey", 
            luabridge::overload<const char*>(&UserSettings::getIntegerForKey),
            luabridge::overload<const char *, int>(&UserSettings::getIntegerForKey))
        .addStaticFunction("getLongForKey", 
            luabridge::overload<const char*>(&UserSettings::getLongForKey),
            luabridge::overload<const char *, long>(&UserSettings::getLongForKey))
        .addStaticFunction("getFloatForKey", 
            luabridge::overload<const char*>(&UserSettings::getFloatForKey),
            luabridge::overload<const char *, float>(&UserSettings::getFloatForKey))
        .addStaticFunction("getDoubleForKey", 
            luabridge::overload<const char*>(&UserSettings::getDoubleForKey),
            luabridge::overload<const char *, double>(&UserSettings::getDoubleForKey))
        .addStaticFunction("getStringForKey", 
            luabridge::overload<const char*>(&UserSettings::getStringForKey),
            luabridge::overload<const char *, const std::string&>(&UserSettings::getStringForKey))
        .addStaticFunction("getDataForKey", 
            luabridge::overload<const char*>(&UserSettings::getDataForKey),
            luabridge::overload<const char *, const Data&>(&UserSettings::getDataForKey))
        .addStaticFunction("setBoolForKey", &UserSettings::setBoolForKey)
        .addStaticFunction("setIntegerForKey", &UserSettings::setIntegerForKey)
        .addStaticFunction("setLongForKey", &UserSettings::setLongForKey)
        .addStaticFunction("setFloatForKey", &UserSettings::setFloatForKey)
        .addStaticFunction("setDoubleForKey", &UserSettings::setDoubleForKey)
        .addStaticFunction("setStringForKey", &UserSettings::setStringForKey)
        .addStaticFunction("setDataForKey", &UserSettings::setDataForKey)
        .addStaticFunction("removeKey", &UserSettings::removeKey)
        .addStaticFunction("hasKey", &UserSettings::hasKey)
        .addStaticFunction("deleteKey", &UserSettings::deleteKey)
        .addStaticFunction("deleteAll", &UserSettings::deleteAll)
        .addStaticFunction("save", &UserSettings::save)
        .endClass();

    // Unity's PlayerPrefs: the same keys as UserSettings (getInt reads what setIntegerForKey
    // wrote). Read a key with the type it was written with.
    luabridge::getGlobalNamespace(L)
        .beginClass<PlayerPrefsModule>("PlayerPrefs")
        .addStaticFunction("getInt", &luaPrefsGetInt)
        .addStaticFunction("setInt", &luaPrefsSetInt)
        .addStaticFunction("getFloat", &luaPrefsGetFloat)
        .addStaticFunction("setFloat", &luaPrefsSetFloat)
        .addStaticFunction("getString", &luaPrefsGetString)
        .addStaticFunction("setString", &luaPrefsSetString)
        .addStaticFunction("hasKey", &UserSettings::hasKey)
        .addStaticFunction("deleteKey", &UserSettings::deleteKey)
        .addStaticFunction("deleteAll", &UserSettings::deleteAll)
        .addStaticFunction("save", &UserSettings::save)
        .endClass();

    // JSON <-> Lua: encode/decode, files, the null sentinel and the array/object markers (the
    // rules: docs/mcp/guide/lua-api.md, "Json")
    luabridge::getGlobalNamespace(L)
        .beginClass<LuaJsonModule>("Json")
        .addStaticFunction("encode", &luaJsonEncode)
        .addStaticFunction("decode", &luaJsonDecode)
        .addStaticFunction("tryEncode", &luaJsonTryEncode)
        .addStaticFunction("tryDecode", &luaJsonTryDecode)
        .addStaticFunction("isNull", &luaJsonIsNull)
        .addStaticFunction("array", &luaJsonArray)
        .addStaticFunction("object", &luaJsonObject)
        .addStaticFunction("readFile", &luaJsonReadFile)
        .addStaticFunction("writeFile", &luaJsonWriteFile)
        // The JSON null: a unique value (tostring "Json.null") that keeps array lengths and object keys
        .addStaticProperty("null", &jsonNull)
        .endClass();

#endif //DISABLE_LUA_BINDINGS
}