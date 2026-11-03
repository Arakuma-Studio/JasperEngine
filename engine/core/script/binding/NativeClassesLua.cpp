// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Lua side of the native bridge (core/native/NativeBridge.h): the `Native` class, the
// `NativeCall` handle and the Lua mocks (Native.setMock, <luaDir>/native_mocks/<Plugin>.lua),
// the last step of the resolution order. Arguments, results and payloads cross with the
// Json module's rules (LuaJson). The guide: docs/mcp/guide/native-bridge.md.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Log.h"
#include "io/Data.h"
#include "native/NativeBridge.h"
#include "native/NativePlatform.h"
#include "script/AsyncOwner.h"
#include "script/LuaJson.h"
#include "script/binding/NetClassesLua.h"
#include "thread/MainThreadQueue.h"

#include <cassert>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;

namespace jasperengine {

    // NativeCall in Lua: what Native.callAsync returns (keeping it is optional) and what an
    // async mock function gets as its last argument
    class NativeCallLua : public std::enable_shared_from_this<NativeCallLua> {
    public:
        NativeCall call;
        LuaRegistrySlot resultValue;   // the result's Lua value, made at the first read

        uint64_t getId() const { return call.id(); }
        std::string getPlugin() const { return call.plugin(); }
        std::string getMethod() const { return call.method(); }
        // the callback ran (or the call was cancelled): Coroutine.waitFor(call) waits for it
        bool isDone() const { return call.isDelivered(); }
        // idempotent: the callback will not run; native code sees isCancelled()
        void cancel() const { call.cancel(); }
    };

}

#ifndef DISABLE_LUA_BINDINGS
namespace {

    // Lua is built as C: lua_error longjmps over C++ destructors. The *Impl functions return
    // RAISE with the message pushed once their own locals are gone; the lua_CFunction raises.
    constexpr int RAISE = -1;

    int pushRaise(lua_State* L, const std::string& message){
        lua_pushlstring(L, message.data(), message.size());
        return RAISE;
    }

    int finish(lua_State* L, int results){
        if (results == RAISE) return lua_error(L);
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

    // Native.pending(): its address is the sentinel
    char pendingSentinel = 0;

    bool isPending(lua_State* L, int index){
        return lua_type(L, index) == LUA_TLIGHTUSERDATA && lua_touserdata(L, index) == &pendingSentinel;
    }

    // a value from Lua as JSON; false with the error code and message
    bool toNative(lua_State* L, int index, NativeJson& out, std::string& code, std::string& message){
        std::string error;
        if (LuaJson::toJson(L, index, out, LuaJson::EncodeOptions(), error)) return true;
        code = error.rfind("invalid UTF-8", 0) == 0 ? "invalid_utf8" : "bad_args";
        message = error;
        return false;
    }

    // a result or payload: a top-level null is nil (nested ones are Json.null)
    void pushNative(lua_State* L, const NativeJson& value){
        if (value.is_null()) lua_pushnil(L);
        else LuaJson::push(L, value);
    }

    NativeCallLua* callOf(lua_State* L, int index){
        if (!luabridge::isInstance<NativeCallLua>(L, index)) return nullptr;
        auto result = luabridge::get<NativeCallLua*>(L, index);
        return result ? result.value() : nullptr;
    }

    // pushes a new NativeCall userdata for `call`
    bool pushCall(lua_State* L, const NativeCall& call){
        auto object = std::make_shared<NativeCallLua>();
        object->call = call;
        if (!luabridge::push(L, object)){
            lua_pushnil(L);
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------- the mocks

    class LuaMocks : public NativeMockProvider {
    public:
        std::map<std::string, std::unique_ptr<LuaRegistrySlot>> mocks;   // plugin -> table
        std::set<std::string> loadTried;                                 // auto-load done (or failed)

        void set(lua_State* L, const std::string& plugin, int index){
            auto slot = std::make_unique<LuaRegistrySlot>();
            slot->set(L, index);
            mocks[plugin] = std::move(slot);
        }

        void clear(const std::string& plugin){
            mocks.erase(plugin);
        }

        // pushes the mock table of `plugin` (loads native_mocks/<plugin>.lua the first time);
        // false (nothing pushed) when there is none
        bool pushMock(lua_State* L, const std::string& plugin){
            auto it = mocks.find(plugin);
            if (it != mocks.end() && it->second->push(L)) return true;
            if (it != mocks.end()) mocks.erase(it);
            if (!loadTried.insert(plugin).second) return false;
            if (!autoLoad(L, plugin)) return false;
            it = mocks.find(plugin);
            return it != mocks.end() && it->second->push(L);
        }

        // require("native_mocks.<plugin>") when the file exists; a table result is the mock
        bool autoLoad(lua_State* L, const std::string& plugin){
            if (!isModuleName(plugin) || !mockFileExists(plugin)) return false;
            const int top = lua_gettop(L);
            const std::string module = "native_mocks." + plugin;
            lua_getglobal(L, "require");
            lua_pushlstring(L, module.data(), module.size());
            if (LuaBinding::pcallWithTraceback(L, 1, 1) != LUA_OK){
                Log::error("Native: loading the mock %s failed: %s", module.c_str(), LuaBinding::getLuaStackErrorString(L, -1).c_str());
                lua_settop(L, top);
                return false;
            }
            bool made = false;
            if (lua_istable(L, -1)){
                // the module may have called Native.setMock itself: that one stays
                if (mocks.find(plugin) == mocks.end()) set(L, plugin, -1);
                made = true;
            } else if (mocks.find(plugin) != mocks.end()){
                made = true;
            } else {
                Log::warn("Native: the mock %s returned %s, not a table of functions; it is ignored", module.c_str(), luaL_typename(L, -1));
            }
            lua_settop(L, top);
            return made;
        }

        static bool isModuleName(const std::string& plugin){
            if (plugin.empty()) return false;
            for (char c : plugin){
                if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
            }
            return true;
        }

        // the paths LuaBinding::moduleLoader looks at
        static bool mockFileExists(const std::string& plugin){
            for (const std::string& path : {"lua://native_mocks/" + plugin + ".lua", "lua://lua/native_mocks/" + plugin + ".lua"}){
                Data data;
                data.open(path.c_str());
                if (data.getMemPtr() != nullptr) return true;
            }
            return false;
        }

        bool has(const std::string& plugin, const std::string& method) override{
            lua_State* L = LuaBinding::getLuaState();
            if (!L) return false;
            const int top = lua_gettop(L);
            bool found = pushMock(L, plugin);
            if (found && !method.empty()){
                lua_pushlstring(L, method.data(), method.size());
                lua_gettable(L, -2);
                found = lua_isfunction(L, -1);
            }
            lua_settop(L, top);
            return found;
        }

        // pushes the mock function of plugin.method; false with err (nothing pushed) when none
        bool pushFunction(lua_State* L, const std::string& plugin, const std::string& method, NativeError& err){
            const int top = lua_gettop(L);
            if (!pushMock(L, plugin)){
                err = NativeError("not_found", "the Lua mock of \"" + plugin + "\" is gone");
                return false;
            }
            lua_pushlstring(L, method.data(), method.size());
            lua_gettable(L, -2);
            if (!lua_isfunction(L, -1)){
                lua_settop(L, top);
                err = NativeError("no_method", "the Lua mock of \"" + plugin + "\" has no function \"" + method + "\"");
                return false;
            }
            lua_remove(L, -2);
            return true;
        }

        // pushes the elements of a JSON array; their count
        static int pushArgs(lua_State* L, const NativeJson& args){
            int n = 0;
            if (!args.is_array()) return 0;
            luaL_checkstack(L, (int)args.size() + 2, "too many native call arguments");
            for (const NativeJson& arg : args){
                pushNative(L, arg);
                n++;
            }
            return n;
        }

        // reads the results of a mock function (from `first` to the top): value, or nil +
        // message [+ code]; false with err
        static bool readResults(lua_State* L, int first, const std::string& plugin, const std::string& method,
                                NativeJson& value, NativeError& err){
            const int top = lua_gettop(L);
            if (first > top || lua_isnil(L, first)){
                if (first + 1 <= top && lua_type(L, first + 1) == LUA_TSTRING){
                    std::string code = (first + 2 <= top && lua_type(L, first + 2) == LUA_TSTRING) ? luaString(L, first + 2) : std::string();
                    err = NativeError(code.empty() ? "exception" : code, luaString(L, first + 1));
                    return false;
                }
                value = nullptr;
                return true;
            }
            std::string code, message;
            if (!toNative(L, first, value, code, message)){
                err = NativeError("unsupported_type", "the Lua mock " + plugin + "." + method + " returned a value JSON cannot hold: " + message);
                return false;
            }
            return true;
        }

        NativeJson call(const std::string& plugin, const std::string& method, const NativeJson& args, NativeError& err) override{
            lua_State* L = LuaBinding::getLuaState();
            assert(LuaBinding::isLuaThread());
            if (!L){
                err = NativeError("not_found", "no Lua state");
                return nullptr;
            }
            const int top = lua_gettop(L);
            if (!pushFunction(L, plugin, method, err)) return nullptr;
            const int nargs = pushArgs(L, args);
            if (LuaBinding::pcallWithTraceback(L, nargs, LUA_MULTRET) != LUA_OK){
                err = NativeError("exception", "the Lua mock " + plugin + "." + method + " failed: " + LuaBinding::getLuaStackErrorString(L, -1));
                lua_settop(L, top);
                return nullptr;
            }
            NativeJson value;
            if (top + 1 <= lua_gettop(L) && isPending(L, top + 1)){
                err = NativeError("async_only", plugin + "." + method + " (Lua mock) returned Native.pending(): it is asynchronous, use Native.callAsync");
                lua_settop(L, top);
                return nullptr;
            }
            const bool ok = readResults(L, top + 1, plugin, method, value, err);
            lua_settop(L, top);
            return ok ? value : NativeJson();
        }

        void callAsync(const NativeCall& call) override{
            lua_State* L = LuaBinding::getLuaState();
            if (!L){
                call.reject("not_found", "no Lua state");
                return;
            }
            const int top = lua_gettop(L);
            NativeError err;
            if (!pushFunction(L, call.plugin(), call.method(), err)){
                call.reject(err.code, err.message);
                return;
            }
            const int nargs = pushArgs(L, call.args());
            pushCall(L, call);
            // the mock runs now; its result reaches the callback in the next frame, as a
            // platform's would
            if (LuaBinding::pcallWithTraceback(L, nargs + 1, LUA_MULTRET) != LUA_OK){
                const std::string message = LuaBinding::getLuaStackErrorString(L, -1);
                lua_settop(L, top);
                call.reject("exception", "the Lua mock " + call.plugin() + "." + call.method() + " failed: " + message);
                return;
            }
            if (top + 1 <= lua_gettop(L) && isPending(L, top + 1)){
                // completed later with call:resolve / call:reject
                lua_settop(L, top);
                return;
            }
            NativeJson value;
            const bool ok = readResults(L, top + 1, call.plugin(), call.method(), value, err);
            lua_settop(L, top);
            if (call.isDone()) return;   // the mock completed it itself and returned something
            if (ok) call.resolve(std::move(value));
            else call.reject(err.code, err.message);
        }

        void reset() override{
            mocks.clear();
            loadTried.clear();
            // the mock modules load again (scripts may have changed)
            lua_State* L = LuaBinding::getLuaState();
            if (!L) return;
            const int top = lua_gettop(L);
            lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
            if (lua_istable(L, -1)){
                std::vector<std::string> names;
                lua_pushnil(L);
                while (lua_next(L, -2) != 0){
                    if (lua_type(L, -2) == LUA_TSTRING){
                        const std::string name = luaString(L, -2);
                        if (name.rfind("native_mocks.", 0) == 0) names.push_back(name);
                    }
                    lua_pop(L, 1);
                }
                for (const std::string& name : names){
                    lua_pushnil(L);
                    lua_setfield(L, -2, name.c_str());
                }
            }
            lua_settop(L, top);
        }
    };

    LuaMocks& luaMocks(){
        static LuaMocks* mocks = new LuaMocks();
        return *mocks;
    }

    // ---------------------------------------------------------------- Native

    // plugin and method names: strings (a programming error otherwise)
    int readNames(lua_State* L, const char* fn, std::string& plugin, std::string& method, bool methodOptional){
        if (lua_type(L, 1) != LUA_TSTRING){
            return pushRaise(L, std::string(fn) + ": the plugin name must be a string, got " + luaL_typename(L, 1));
        }
        plugin = luaString(L, 1);
        if (methodOptional && isAbsent(L, 2)) return 0;
        if (lua_type(L, 2) != LUA_TSTRING){
            return pushRaise(L, std::string(fn) + ": the method name must be a string, got " + luaL_typename(L, 2));
        }
        method = luaString(L, 2);
        return 0;
    }

    int pushFailure(lua_State* L, const NativeError& err){
        lua_pushnil(L);
        lua_pushlstring(L, err.message.data(), err.message.size());
        lua_pushlstring(L, err.code.data(), err.code.size());
        return 3;
    }

    int callImpl(lua_State* L){
        std::string plugin, method;
        if (readNames(L, "Native.call", plugin, method, false) == RAISE) return RAISE;
        NativeJson args = NativeJson::array();
        const int top = lua_gettop(L);
        for (int i = 3; i <= top; i++){
            NativeJson value;
            std::string code, message;
            if (!toNative(L, i, value, code, message)){
                return pushFailure(L, NativeError(code, "argument " + std::to_string(i - 2) + " of " + plugin + "." + method + ": " + message));
            }
            args.push_back(std::move(value));
        }
        NativeError err;
        NativeJson result = NativeBridge::call(plugin, method, args, err);
        if (err.isSet()) return pushFailure(L, err);
        pushNative(L, result);
        lua_pushnil(L);
        lua_pushnil(L);
        return 3;
    }

    int luaNativeCall(lua_State* L){ return finish(L, callImpl(L)); }

    const char* const ASYNC_OPTIONS = "thread (\"main\" or \"background\"), owner, timeout";

    std::set<std::string>& warnedOptions(){
        static std::set<std::string>* names = new std::set<std::string>();
        return *names;
    }

    // the Lua side of an async call: its callback
    struct AsyncContext {
        LuaRegistrySlot callback;
        std::string plugin, method;
    };

    void runCallback(AsyncContext& context, const NativeJson& result, const NativeError* error){
        lua_State* L = LuaBinding::getLuaState();
        // the delivery point is on the main thread (MainThreadQueue): no Lua anywhere else
        assert(LuaBinding::isLuaThread());
        if (L && context.callback.isSet()){
            const int top = lua_gettop(L);
            context.callback.push(L);
            if (error){
                lua_pushnil(L);
                lua_pushlstring(L, error->message.data(), error->message.size());
                lua_pushlstring(L, error->code.data(), error->code.size());
            } else {
                pushNative(L, result);
                lua_pushnil(L);
                lua_pushnil(L);
            }
            if (LuaBinding::pcallWithTraceback(L, 3, 0) != LUA_OK){
                Log::error("Lua error in the Native.callAsync callback of %s.%s: %s", context.plugin.c_str(), context.method.c_str(),
                           LuaBinding::getLuaStackErrorString(L, -1).c_str());
            }
            lua_settop(L, top);
        }
        context.callback.reset();
    }

    int callAsyncImpl(lua_State* L){
        const char* fn = "Native.callAsync";
        std::string plugin, method;
        if (readNames(L, fn, plugin, method, false) == RAISE) return RAISE;

        // (plugin, method [, args] [, callback] [, options]); args may be left out before a callback
        int next = 3;
        int argsIndex = 0;
        if (lua_type(L, next) == LUA_TTABLE){
            argsIndex = next++;
        } else if (lua_type(L, next) == LUA_TNIL){
            next++;
        } else if (lua_type(L, next) != LUA_TFUNCTION && lua_type(L, next) != LUA_TNONE){
            return pushRaise(L, std::string(fn) + ": args must be an array table ({1, \"two\"}) or nil, got " + luaL_typename(L, next));
        }
        int callbackIndex = 0;
        if (lua_type(L, next) == LUA_TFUNCTION){
            callbackIndex = next++;
        } else if (lua_type(L, next) == LUA_TNIL){
            next++;
        } else if (lua_type(L, next) != LUA_TTABLE && lua_type(L, next) != LUA_TNONE){
            return pushRaise(L, std::string(fn) + ": callback must be a function(result, errMessage, errCode), got " + luaL_typename(L, next));
        }
        int optionsIndex = 0;
        if (lua_type(L, next) == LUA_TTABLE){
            optionsIndex = next++;
        } else if (!isAbsent(L, next)){
            return pushRaise(L, std::string(fn) + ": options must be a table {" + ASYNC_OPTIONS + "}, got " + luaL_typename(L, next));
        }
        if (!isAbsent(L, next)){
            return pushRaise(L, std::string(fn) + ": too many arguments (plugin, method, args, callback, options)");
        }

        bool background = false;
        double timeout = 0.0;
        AsyncOwner owner;
        if (optionsIndex){
            lua_pushnil(L);
            while (lua_next(L, optionsIndex) != 0){
                const int value = lua_gettop(L);
                const std::string key = lua_type(L, -2) == LUA_TSTRING ? luaString(L, -2) : std::string();
                std::string error;
                if (key == "thread"){
                    const std::string thread = lua_type(L, value) == LUA_TSTRING ? luaString(L, value) : std::string();
                    if (thread != "main" && thread != "background"){
                        return pushRaise(L, std::string(fn) + ": options.thread must be \"main\" or \"background\"");
                    }
                    background = thread == "background";
                } else if (key == "timeout"){
                    if (lua_type(L, value) != LUA_TNUMBER || !(lua_tonumber(L, value) >= 0.0)){
                        return pushRaise(L, std::string(fn) + ": options.timeout must be 0 or more seconds (0: none)");
                    }
                    timeout = lua_tonumber(L, value);
                } else if (key == "owner"){
                    if (!AsyncOwners::fromLua(L, value, owner, error)){
                        return pushRaise(L, std::string(fn) + ": options.owner is not valid: " + error);
                    }
                } else if (warnedOptions().insert(key).second){
                    Log::warn("%s: unknown option \"%s\" is ignored (the options are %s)", fn, key.c_str(), ASYNC_OPTIONS);
                }
                lua_pop(L, 1);
            }
        }

        NativeJson args = NativeJson::array();
        std::string convertCode, convertError;
        bool converted = true;
        if (argsIndex){
            converted = toNative(L, argsIndex, args, convertCode, convertError);
            if (converted && !args.is_array()){
                return pushRaise(L, std::string(fn) + ": args must be an array table ({1, \"two\"}; Json.null for a nil inside), "
                                 "got a table with other keys");
            }
        }

        auto context = std::make_shared<AsyncContext>();
        context->plugin = plugin;
        context->method = method;
        if (callbackIndex) context->callback.set(L, callbackIndex);
        NativeBridge::Completion done = [context](const NativeJson& result, const NativeError* error) {
            runCallback(*context, result, error);
        };

        NativeCall call;
        if (!converted){
            // an argument JSON cannot hold: the callback says so, next frame (errors are values)
            const NativeError error(convertCode, "the arguments of " + plugin + "." + method + ": " + convertError);
            call = NativeBridgeInternal::failedCall(plugin, method, std::move(done), owner, error);
        } else {
            call = NativeBridge::callAsync(plugin, method, std::move(args), std::move(done), background, owner, timeout);
        }
        pushCall(L, call);
        return 1;
    }

    int luaNativeCallAsync(lua_State* L){ return finish(L, callAsyncImpl(L)); }

    int onImpl(lua_State* L){
        std::string plugin, event;
        if (lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING){
            return pushRaise(L, "Native.on: the plugin and event names must be strings (Native.on(\"Ads\", \"rewarded\", function(payload) end))");
        }
        plugin = luaString(L, 1);
        event = luaString(L, 2);
        if (lua_type(L, 3) != LUA_TFUNCTION){
            return pushRaise(L, std::string("Native.on: the handler must be a function(payload), got ") + luaL_typename(L, 3));
        }
        AsyncOwner owner;
        std::string error;
        if (!AsyncOwners::fromLua(L, 4, owner, error)){
            return pushRaise(L, "Native.on: the owner is not valid: " + error);
        }
        // a mock module may be what emits this plugin's events
        NativeBridge::has(plugin);
        auto handler = std::make_shared<LuaRegistrySlot>();
        handler->set(L, 3);
        const std::string where = plugin + "." + event;
        const uint64_t id = NativeBridge::on(plugin, event, [handler, where](const NativeJson& payload) {
            lua_State* state = LuaBinding::getLuaState();
            assert(LuaBinding::isLuaThread());
            if (!state || !handler->isSet()) return;
            const int top = lua_gettop(state);
            handler->push(state);
            pushNative(state, payload);
            if (LuaBinding::pcallWithTraceback(state, 1, 0) != LUA_OK){
                Log::error("Lua error in the Native.on handler of %s: %s", where.c_str(), LuaBinding::getLuaStackErrorString(state, -1).c_str());
            }
            lua_settop(state, top);
        }, owner);
        lua_pushinteger(L, (lua_Integer)id);
        return 1;
    }

    int luaNativeOn(lua_State* L){ return finish(L, onImpl(L)); }

    int offImpl(lua_State* L){
        if (!lua_isinteger(L, 1)){
            if (isAbsent(L, 1)) return 0;
            return pushRaise(L, std::string("Native.off: the id must be the integer Native.on returned, got ") + luaL_typename(L, 1));
        }
        NativeBridge::off((uint64_t)lua_tointeger(L, 1));
        return 0;
    }

    int luaNativeOff(lua_State* L){ return finish(L, offImpl(L)); }

    int hasImpl(lua_State* L){
        std::string plugin, method;
        if (readNames(L, "Native.has", plugin, method, true) == RAISE) return RAISE;
        lua_pushboolean(L, NativeBridge::has(plugin, method));
        return 1;
    }

    int luaNativeHas(lua_State* L){ return finish(L, hasImpl(L)); }

    int setMockImpl(lua_State* L){
        if (lua_type(L, 1) != LUA_TSTRING){
            return pushRaise(L, std::string("Native.setMock: the plugin name must be a string, got ") + luaL_typename(L, 1));
        }
        if (!lua_istable(L, 2)){
            return pushRaise(L, std::string("Native.setMock: the mock must be a table of functions ({getCode = function() return 7 end}), got ") +
                             luaL_typename(L, 2));
        }
        luaMocks().set(L, luaString(L, 1), 2);
        return 0;
    }

    int luaNativeSetMock(lua_State* L){ return finish(L, setMockImpl(L)); }

    int clearMockImpl(lua_State* L){
        if (lua_type(L, 1) != LUA_TSTRING){
            return pushRaise(L, std::string("Native.clearMock: the plugin name must be a string, got ") + luaL_typename(L, 1));
        }
        luaMocks().clear(luaString(L, 1));
        return 0;
    }

    int luaNativeClearMock(lua_State* L){ return finish(L, clearMockImpl(L)); }

    int emitMockImpl(lua_State* L){
        if (lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING){
            return pushRaise(L, "Native.emitMock: the plugin and event names must be strings (Native.emitMock(\"Ads\", \"rewarded\", {amount = 1}))");
        }
        NativeJson payload;
        std::string code, message;
        if (!toNative(L, 3, payload, code, message)){
            return pushRaise(L, "Native.emitMock: the payload cannot be sent as JSON: " + message);
        }
        NativeBridge::emit(luaString(L, 1), luaString(L, 2), std::move(payload));
        return 0;
    }

    int luaNativeEmitMock(lua_State* L){ return finish(L, emitMockImpl(L)); }

    int luaNativePending(lua_State* L){
        lua_pushlightuserdata(L, &pendingSentinel);
        return 1;
    }

    // ---------------------------------------------------------------- NativeCall

    luabridge::LuaRef callResult(const NativeCallLua* self, lua_State* L){
        NativeCallLua* s = const_cast<NativeCallLua*>(self);
        if (!s || !s->call.isDelivered() || s->call.error().isSet() || s->call.result().is_null()){
            lua_pushnil(L);
        } else if (!s->resultValue.push(L)){
            LuaJson::push(L, s->call.result());
            s->resultValue.set(L, -1);
        }
        return luabridge::LuaRef::fromStack(L);
    }

    luabridge::LuaRef callErrorCode(const NativeCallLua* self, lua_State* L){
        if (self && self->call.isDelivered() && self->call.error().isSet()) {
            const std::string& code = self->call.error().code;
            lua_pushlstring(L, code.data(), code.size());
        } else {
            lua_pushnil(L);
        }
        return luabridge::LuaRef::fromStack(L);
    }

    luabridge::LuaRef callErrorMessage(const NativeCallLua* self, lua_State* L){
        if (self && self->call.isDelivered() && self->call.error().isSet()) {
            const std::string& message = self->call.error().message;
            lua_pushlstring(L, message.data(), message.size());
        } else {
            lua_pushnil(L);
        }
        return luabridge::LuaRef::fromStack(L);
    }

    int callResolveImpl(lua_State* L){
        NativeCallLua* self = callOf(L, 1);
        if (!self) return pushRaise(L, "NativeCall:resolve: call it with a colon (call:resolve(value))");
        NativeJson value;
        std::string code, message;
        if (!isAbsent(L, 2) && !toNative(L, 2, value, code, message)){
            return pushRaise(L, "NativeCall:resolve: the value cannot be sent as JSON: " + message);
        }
        lua_pushboolean(L, self->call.resolve(std::move(value)));
        return 1;
    }

    int luaCallResolve(lua_State* L){ return finish(L, callResolveImpl(L)); }

    int callRejectImpl(lua_State* L){
        NativeCallLua* self = callOf(L, 1);
        if (!self) return pushRaise(L, "NativeCall:reject: call it with a colon (call:reject(code, message))");
        if (lua_type(L, 2) != LUA_TSTRING){
            return pushRaise(L, std::string("NativeCall:reject: the code must be a string (\"exception\", \"cancelled\", ...), got ") + luaL_typename(L, 2));
        }
        const std::string message = lua_type(L, 3) == LUA_TSTRING ? luaString(L, 3) : luaString(L, 2);
        lua_pushboolean(L, self->call.reject(luaString(L, 2), message));
        return 1;
    }

    int luaCallReject(lua_State* L){ return finish(L, callRejectImpl(L)); }

    std::string callText(const NativeCallLua* self){
        std::string state = "pending";
        if (self->call.isDelivered()) state = self->call.error().isSet() ? self->call.error().code : "done";
        return "NativeCall(" + std::to_string(self->getId()) + " " + self->getPlugin() + "." + self->getMethod() + " " + state + ")";
    }

}
#endif //DISABLE_LUA_BINDINGS

void LuaBinding::registerNativeClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    NativeBridgeInternal::setMockProvider(&luaMocks());

    // What Native.callAsync returns (keeping it is optional: the call runs without it)
    luabridge::getGlobalNamespace(L)
        .beginClass<NativeCallLua>("NativeCall")
        .addProperty("id", &NativeCallLua::getId)
        .addProperty("plugin", &NativeCallLua::getPlugin)
        .addProperty("method", &NativeCallLua::getMethod)
        // the result once the callback ran (nil before, on failure, or for null)
        .addProperty("result", &callResult)
        // nil, or the error code once it failed ("timeout", "not_found", ...)
        .addProperty("errorCode", &callErrorCode)
        .addProperty("errorMessage", &callErrorMessage)
        .addFunction("isDone", &NativeCallLua::isDone)
        .addFunction("cancel", &NativeCallLua::cancel)
        // mocks: complete a call their function returned Native.pending() for
        .addFunction("resolve", &luaCallResolve)
        .addFunction("reject", &luaCallReject)
        .addFunction("__tostring", &callText)
        .endClass();

    // Native code of the platform the game runs on (docs/mcp/guide/native-bridge.md): C++
    // plugins, Java/Kotlin (Android), Objective-C/Swift (iOS, macOS), JavaScript (Web), Lua
    // mocks. Errors are values; callbacks and events run on the main thread before onUpdate.
    luabridge::getGlobalNamespace(L)
        .beginClass<NativeBridge>("Native")
        .addStaticFunction("call", &luaNativeCall)
        .addStaticFunction("callAsync", &luaNativeCallAsync)
        .addStaticFunction("on", &luaNativeOn)
        .addStaticFunction("off", &luaNativeOff)
        .addStaticFunction("has", &luaNativeHas)
        .addStaticFunction("setMock", &luaNativeSetMock)
        .addStaticFunction("clearMock", &luaNativeClearMock)
        .addStaticFunction("emitMock", &luaNativeEmitMock)
        .addStaticFunction("pending", &luaNativePending)
        .endClass();
#endif //DISABLE_LUA_BINDINGS
}
