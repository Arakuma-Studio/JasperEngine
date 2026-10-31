// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Log.h"
#include "net/Http.h"
#include "net/HttpBackend.h"
#include "net/HttpJson.h"
#include "script/AsyncOwner.h"
#include "script/LuaJson.h"
#include "script/binding/NetClassesLua.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <cassert>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;

// =============================================================================================
// LuaRegistrySlot

LuaRegistrySlot::~LuaRegistrySlot(){
    reset();
}

void LuaRegistrySlot::set(lua_State* L, int index){
    reset();
    lua_State* main = LuaBinding::getLuaState();
    if (!L || !main) return;
    // the registry is the state's (every coroutine shares it): the reference outlives the
    // coroutine it was made in and is pushed on the main state
    lua_pushvalue(L, index);
    ref = luaL_ref(L, LUA_REGISTRYINDEX);
    state = main;
}

bool LuaRegistrySlot::push(lua_State* L) const{
    if (!isSet() || !L) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    return true;
}

void LuaRegistrySlot::reset(){
    if (state && ref != LUA_NOREF && ref != LUA_REFNIL && LuaBinding::getLuaState() == state){
        luaL_unref(state, LUA_REGISTRYINDEX, ref);
    }
    state = nullptr;
    ref = LUA_NOREF;
}

bool LuaRegistrySlot::isSet() const{
    return state && ref != LUA_NOREF && LuaBinding::getLuaState() == state;
}

// =============================================================================================
// HttpRequestLua, HttpResponseLua

uint64_t HttpRequestLua::getId() const{ return handle ? handle->id() : 0; }
std::string HttpRequestLua::getUrl() const{ return handle ? handle->url() : std::string(); }
std::string HttpRequestLua::getMethod() const{ return handle ? handle->method() : std::string(); }
HttpState HttpRequestLua::getState() const{ return handle ? handle->state() : HttpState::Cancelled; }
float HttpRequestLua::getProgress() const{ return handle ? handle->progress() : -1.0f; }
uint64_t HttpRequestLua::getBytesReceived() const{ return handle ? handle->bytesReceived() : 0; }
int64_t HttpRequestLua::getBytesTotal() const{ return handle ? handle->bytesTotal() : -1; }
bool HttpRequestLua::isDone() const{ return !handle || handle->isDone(); }
void HttpRequestLua::cancel(){ if (handle) handle->cancel(); }

bool HttpResponseLua::getOk() const{ return response && response->ok; }
int HttpResponseLua::getStatus() const{ return response ? response->status : 0; }
std::string HttpResponseLua::getErrorCode() const{ return response ? response->errorCode : std::string(); }
std::string HttpResponseLua::getError() const{ return response ? response->error : std::string(); }
std::string HttpResponseLua::getUrl() const{ return response ? response->url : std::string(); }
std::string HttpResponseLua::getMethod() const{ return response ? response->method : std::string(); }
bool HttpResponseLua::getRedirected() const{ return response && response->redirected; }
std::string HttpResponseLua::getFile() const{ return response ? response->filePath : std::string(); }
double HttpResponseLua::getElapsed() const{ return response ? response->elapsed : 0.0; }
uint64_t HttpResponseLua::getBytesReceived() const{ return response ? response->bytesReceived : 0; }

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

    // a string, a number (as Lua prints it) or a boolean as text
    bool scalarText(lua_State* L, int index, std::string& out){
        switch (lua_type(L, index)){
            case LUA_TSTRING:
                out = luaString(L, index);
                return true;
            case LUA_TNUMBER:
                if (lua_isinteger(L, index)){
                    out = std::to_string((long long)lua_tointeger(L, index));
                } else {
                    lua_pushvalue(L, index);
                    out = luaString(L, -1);
                    lua_pop(L, 1);
                }
                return true;
            case LUA_TBOOLEAN:
                out = lua_toboolean(L, index) ? "true" : "false";
                return true;
            default:
                return false;
        }
    }

    // name -> scalar pairs of a table, sorted by name (determinism)
    bool scalarPairs(lua_State* L, int index, const std::string& what, HttpHeaders& out, std::string& error){
        index = lua_absindex(L, index);
        out.clear();
        lua_pushnil(L);
        while (lua_next(L, index) != 0){
            if (lua_type(L, -2) != LUA_TSTRING){
                lua_pop(L, 2);
                error = what + " keys must be strings";
                return false;
            }
            std::string value;
            if (!scalarText(L, -1, value)){
                error = what + "." + luaString(L, -2) + " must be a string, a number or a boolean, got " + luaL_typename(L, -1);
                lua_pop(L, 2);
                return false;
            }
            out.emplace_back(luaString(L, -2), std::move(value));
            lua_pop(L, 1);
        }
        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        return true;
    }

    // {["Name"] = "value"} or {{"Name", "value"}, ...}
    bool headerList(lua_State* L, int index, HttpHeaders& out, std::string& error){
        index = lua_absindex(L, index);
        out.clear();
        lua_rawgeti(L, index, 1);
        const bool isArray = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (isArray){
            const lua_Integer n = (lua_Integer)lua_rawlen(L, index);
            for (lua_Integer i = 1; i <= n; i++){
                lua_rawgeti(L, index, i);
                const int pair = lua_gettop(L);
                std::string name, value;
                bool ok = lua_istable(L, pair);
                if (ok){
                    lua_rawgeti(L, pair, 1);
                    lua_rawgeti(L, pair, 2);
                    ok = lua_type(L, -2) == LUA_TSTRING && scalarText(L, -1, value);
                    if (ok) name = luaString(L, -2);
                    lua_pop(L, 2);
                }
                lua_pop(L, 1);
                if (!ok){
                    error = "headers[" + std::to_string((long long)i) + "] must be a {name, value} pair of strings";
                    return false;
                }
                out.emplace_back(std::move(name), std::move(value));
            }
            return true;
        }
        return scalarPairs(L, index, "headers", out, error);
    }

    std::string formBody(const HttpHeaders& fields){
        std::string text;
        for (const auto& [name, value] : fields){
            if (!text.empty()) text += '&';
            // application/x-www-form-urlencoded: spaces as '+'
            std::string n = HttpUtil::encode(name), v = HttpUtil::encode(value);
            auto plus = [](std::string s){
                std::string out;
                for (size_t i = 0; i < s.size(); i++){
                    if (s.compare(i, 3, "%20") == 0){
                        out += '+';
                        i += 2;
                    } else {
                        out += s[i];
                    }
                }
                return out;
            };
            text += plus(n) + "=" + plus(v);
        }
        return text;
    }

    bool hasHeader(const HttpHeaders& headers, const char* lowerName){
        for (const auto& header : headers){
            if (HttpUtil::lower(header.first) == lowerName) return true;
        }
        return false;
    }

    void setBody(HttpRequestDesc& desc, const std::string& bytes){
        desc.body.assign(bytes.begin(), bytes.end());
    }

    // ---------------------------------------------------------------- the requests

    enum class Call { Get, Head, Delete, Post, Put, Patch, Request, Download, Texture };

    const char* callName(Call call){
        switch (call){
            case Call::Get: return "Http.get";
            case Call::Head: return "Http.head";
            case Call::Delete: return "Http.delete";
            case Call::Post: return "Http.post";
            case Call::Put: return "Http.put";
            case Call::Patch: return "Http.patch";
            case Call::Request: return "Http.request";
            case Call::Download: return "Http.download";
            case Call::Texture: return "Http.getTexture";
        }
        return "Http";
    }

    const char* const OPTION_NAMES =
        "headers, query, body, json, form, timeout, connectTimeout, redirects, verifyTls, responseType, maxBodySize, "
        "onProgress, owner, file, resume, keepPartial (and url, method, onComplete for Http.request)";

    // the Lua side of one request: its callbacks and its Lua object
    struct CallContext {
        LuaRegistrySlot onComplete;
        LuaRegistrySlot onProgress;
        LuaRegistrySlot self;      // the HttpRequest userdata, until the request ends
        std::shared_ptr<HttpRequestLua> request;
    };

    bool pushResponseObject(lua_State* L, HttpRequestLua& request){
        if (request.responseObject.push(L)) return true;
        std::shared_ptr<HttpResponse> response = request.handle ? request.handle->sharedResponse() : nullptr;
        if (!response) return false;
        auto object = std::make_shared<HttpResponseLua>();
        object->response = response;
        if (!luabridge::push(L, object)) return false;
        request.responseObject.set(L, -1);
        return true;
    }

    void runCompletion(CallContext& context){
        lua_State* L = LuaBinding::getLuaState();
        // the delivery point is on the main thread (MainThreadQueue): no Lua anywhere else
        assert(LuaBinding::isLuaThread());
        if (L && context.onComplete.isSet()){
            const int top = lua_gettop(L);
            context.onComplete.push(L);
            if (!pushResponseObject(L, *context.request)) lua_pushnil(L);
            if (LuaBinding::pcallWithTraceback(L, 1, 0) != LUA_OK){
                Log::error("Lua error in the Http callback of %s %s: %s", context.request->getMethod().c_str(),
                           context.request->getUrl().c_str(), LuaBinding::getLuaStackErrorString(L, -1).c_str());
            }
            lua_settop(L, top);
        }
        context.onComplete.reset();
        context.onProgress.reset();
        context.self.reset();
    }

    void runProgress(CallContext& context, uint64_t received, int64_t total){
        lua_State* L = LuaBinding::getLuaState();
        assert(LuaBinding::isLuaThread());
        if (!L || !context.onProgress.isSet()) return;
        const int top = lua_gettop(L);
        context.onProgress.push(L);
        if (!context.self.push(L)) lua_pushnil(L);
        lua_pushinteger(L, (lua_Integer)received);
        lua_pushinteger(L, (lua_Integer)total);
        if (LuaBinding::pcallWithTraceback(L, 3, 0) != LUA_OK){
            Log::error("Lua error in the Http onProgress of %s %s: %s", context.request->getMethod().c_str(),
                       context.request->getUrl().c_str(), LuaBinding::getLuaStackErrorString(L, -1).c_str());
        }
        lua_settop(L, top);
    }

    std::set<std::string>& warnedOptions(){
        static std::set<std::string>* names = new std::set<std::string>();
        return *names;
    }

    // Reads `options` (the table at `index`, 0 for none) into desc; the function indices of
    // the callbacks it names go to completeIndex / progressIndex (0: none)
    int readOptions(lua_State* L, Call call, int index, HttpRequestDesc& desc, AsyncOwner& owner,
                    int& completeIndex, int& progressIndex, bool& hasBody, std::string& url, bool& hasUrl){
        const std::string fn = callName(call);
        if (index == 0) return 0;
        index = lua_absindex(L, index);
        std::string error;
        int bodyKinds = 0;
        bool wantsProgress = false;
        bool wantsComplete = false;
        bool hasContentType = false;
        std::string defaultContentType;

        // headers first: the body options look at Content-Type
        lua_pushliteral(L, "headers");
        lua_rawget(L, index);
        if (!lua_isnil(L, -1)){
            if (!lua_istable(L, -1)){
                lua_pop(L, 1);
                return pushRaise(L, fn + ": options.headers must be a table");
            }
            if (!headerList(L, -1, desc.headers, error)){
                lua_pop(L, 1);
                return pushRaise(L, fn + ": options." + error);
            }
            hasContentType = hasHeader(desc.headers, "content-type");
        }
        lua_pop(L, 1);

        lua_pushnil(L);
        while (lua_next(L, index) != 0){
            if (lua_type(L, -2) != LUA_TSTRING){
                lua_pop(L, 2);
                return pushRaise(L, fn + ": option keys must be strings");
            }
            const std::string key = luaString(L, -2);
            const int value = lua_gettop(L);
            const int type = lua_type(L, value);
            auto fail = [&](const std::string& message){
                lua_pop(L, 2);
                return pushRaise(L, fn + ": options." + key + " " + message);
            };
            if (key == "headers"){
                // read above
            } else if (key == "query"){
                if (type != LUA_TTABLE) return fail("must be a table of name = value");
                HttpHeaders query;
                if (!scalarPairs(L, value, "query", query, error)){
                    lua_pop(L, 2);
                    return pushRaise(L, fn + ": options." + error);
                }
                desc.query = std::move(query);
            } else if (key == "body" || key == "json" || key == "form"){
                if (call == Call::Get || call == Call::Head || call == Call::Texture || call == Call::Download){
                    return fail("is not allowed: a " + std::string(call == Call::Head ? "HEAD" : "GET") +
                                " request has no body (use Http.post, or Http.request with a method)");
                }
                bodyKinds++;
                if (key == "body"){
                    if (type != LUA_TSTRING) return fail("must be a string (use options.json for a table)");
                    setBody(desc, luaString(L, value));
                } else if (key == "json"){
                    nlohmann::json json;
                    if (!LuaJson::toJson(L, value, json, LuaJson::EncodeOptions(), error)){
                        lua_pop(L, 2);
                        return pushRaise(L, fn + ": options.json cannot be sent as JSON: " + error);
                    }
                    setBody(desc, json.dump());
                    defaultContentType = "application/json";
                } else {
                    if (type != LUA_TTABLE) return fail("must be a table of name = value");
                    HttpHeaders fields;
                    if (!scalarPairs(L, value, "form", fields, error)){
                        lua_pop(L, 2);
                        return pushRaise(L, fn + ": options." + error);
                    }
                    setBody(desc, formBody(fields));
                    defaultContentType = "application/x-www-form-urlencoded";
                }
            } else if (key == "timeout" || key == "connectTimeout"){
                if (type != LUA_TNUMBER) return fail("must be a number of seconds");
                const double seconds = lua_tonumber(L, value);
                if (!(seconds >= 0.0)) return fail("must be 0 or more seconds");
                if (key == "timeout") desc.timeout = seconds;
                else desc.connectTimeout = seconds;
            } else if (key == "redirects"){
                if (type == LUA_TBOOLEAN){
                    desc.maxRedirects = lua_toboolean(L, value) ? 10 : 0;
                } else if (lua_isinteger(L, value) && lua_tointeger(L, value) >= 0){
                    desc.maxRedirects = (int)std::min<lua_Integer>(lua_tointeger(L, value), 100);
                } else {
                    return fail("must be true, false or a number of hops (0 or more)");
                }
            } else if (key == "verifyTls" || key == "resume" || key == "keepPartial"){
                if (type != LUA_TBOOLEAN) return fail("must be a boolean");
                const bool on = lua_toboolean(L, value) != 0;
                if (key == "verifyTls") desc.verifyTls = on;
                else if (key == "resume") desc.resume = on;
                else desc.keepPartial = on;
            } else if (key == "responseType"){
                if (call == Call::Download || call == Call::Texture) return fail("does not apply to " + fn);
                const std::string kind = type == LUA_TSTRING ? luaString(L, value) : std::string();
                if (kind == "text") desc.responseType = HttpResponseType::Text;
                else if (kind == "binary") desc.responseType = HttpResponseType::Binary;
                else if (kind == "json") desc.responseType = HttpResponseType::Json;
                else return fail("must be \"text\", \"binary\" or \"json\" (Http.getTexture decodes images, Http.download writes files)");
            } else if (key == "maxBodySize"){
                if (!lua_isinteger(L, value) || lua_tointeger(L, value) < 0) return fail("must be a number of bytes (0: no limit)");
                desc.maxBodySize = (size_t)lua_tointeger(L, value);
            } else if (key == "onProgress"){
                if (type != LUA_TFUNCTION) return fail("must be a function(request, received, total)");
                wantsProgress = true;
            } else if (key == "owner"){
                if (!AsyncOwners::fromLua(L, value, owner, error)){
                    return fail("is not valid: " + error);
                }
            } else if (key == "file"){
                if (call == Call::Download || call == Call::Texture) return fail("does not apply to " + fn + (call == Call::Download ? " (the path is its second argument)" : ""));
                if (type != LUA_TSTRING) return fail("must be a path string (data://...)");
                desc.filePath = luaString(L, value);
                desc.responseType = HttpResponseType::File;
            } else if (call == Call::Request && key == "url"){
                if (type != LUA_TSTRING) return fail("must be a string");
                url = luaString(L, value);
                hasUrl = true;
            } else if (call == Call::Request && key == "method"){
                if (type != LUA_TSTRING) return fail("must be a string (\"GET\", \"POST\", ...)");
                std::string method = luaString(L, value);
                for (char& c : method) c = (char)std::toupper((unsigned char)c);
                if (!HttpUtil::isToken(method)) return fail("is not an HTTP method: " + method);
                desc.method = method;
            } else if (call == Call::Request && key == "onComplete"){
                if (type != LUA_TFUNCTION) return fail("must be a function(response)");
                wantsComplete = true;
            } else {
                if (warnedOptions().insert(key).second){
                    Log::warn("%s: unknown option \"%s\" is ignored (the options are %s)", fn.c_str(), key.c_str(), OPTION_NAMES);
                }
            }
            lua_pop(L, 1);
        }
        if (bodyKinds > 1){
            return pushRaise(L, fn + ": give one of options.body, options.json and options.form");
        }
        if (bodyKinds > 0) hasBody = true;
        // the callbacks stay on the stack (above the arguments) until the request is made
        if (wantsProgress){
            lua_pushliteral(L, "onProgress");
            lua_rawget(L, index);
            progressIndex = lua_gettop(L);
            desc.reportProgress = true;
        }
        if (wantsComplete){
            lua_pushliteral(L, "onComplete");
            lua_rawget(L, index);
            completeIndex = lua_gettop(L);
        }
        if (!defaultContentType.empty() && !hasContentType){
            desc.headers.emplace_back("Content-Type", defaultContentType);
        }
        return 0;
    }

    int startImpl(lua_State* L, Call call){
        const std::string fn = callName(call);
        HttpRequestDesc desc;
        desc.timeout = Http::getDefaultTimeout();
        desc.responseType = HttpResponseType::Text;
        AsyncOwner owner;
        std::string url;
        bool hasUrl = false;
        int next = 1;

        if (call == Call::Request){
            if (!lua_istable(L, 1)){
                return pushRaise(L, fn + ": the argument must be an options table with url (Http.request{url = \"https://...\", method = \"POST\"})");
            }
            if (lua_gettop(L) > 1){
                return pushRaise(L, fn + ": takes one options table (the callback is options.onComplete)");
            }
        } else {
            if (lua_type(L, 1) != LUA_TSTRING){
                return pushRaise(L, fn + ": url must be a string, got " + std::string(luaL_typename(L, 1)));
            }
            url = luaString(L, 1);
            hasUrl = true;
            next = 2;
        }

        // the body argument of post / put / patch, the path of download
        int bodyArg = 0;
        if (call == Call::Post || call == Call::Put || call == Call::Patch){
            bodyArg = next++;
            const int type = lua_type(L, bodyArg);
            if (type != LUA_TNONE && type != LUA_TNIL && type != LUA_TSTRING && type != LUA_TTABLE){
                return pushRaise(L, fn + ": body must be a string (sent as is), a table (sent as JSON) or nil, got " + std::string(luaL_typename(L, bodyArg)));
            }
        }
        if (call == Call::Download){
            if (lua_type(L, next) != LUA_TSTRING){
                return pushRaise(L, fn + ": path must be a string (data://...), got " + std::string(luaL_typename(L, next)));
            }
            desc.filePath = luaString(L, next);
            desc.responseType = HttpResponseType::File;
            next++;
        }
        if (call == Call::Texture) desc.responseType = HttpResponseType::Texture;
        switch (call){
            case Call::Head: desc.method = "HEAD"; break;
            case Call::Delete: desc.method = "DELETE"; break;
            case Call::Post: desc.method = "POST"; break;
            case Call::Put: desc.method = "PUT"; break;
            case Call::Patch: desc.method = "PATCH"; break;
            default: desc.method = "GET"; break;
        }

        int optionsIndex = 0;
        int callbackIndex = 0;
        if (call == Call::Request){
            optionsIndex = 1;
        } else {
            const int type = lua_type(L, next);
            if (type == LUA_TTABLE){
                optionsIndex = next;
                const int after = lua_type(L, next + 1);
                if (after == LUA_TFUNCTION){
                    callbackIndex = next + 1;
                } else if (after != LUA_TNONE && after != LUA_TNIL){
                    return pushRaise(L, fn + ": callback must be a function, got " + std::string(luaL_typename(L, next + 1)));
                }
            } else if (type == LUA_TFUNCTION){
                callbackIndex = next;
                if (!isAbsent(L, next + 1)){
                    return pushRaise(L, fn + ": nothing goes after the callback");
                }
            } else if (type != LUA_TNONE && type != LUA_TNIL){
                return pushRaise(L, fn + ": options must be a table, got " + std::string(luaL_typename(L, next)));
            } else if (lua_type(L, next + 1) == LUA_TFUNCTION){
                callbackIndex = next + 1;
            } else if (!isAbsent(L, next + 1)){
                return pushRaise(L, fn + ": callback must be a function, got " + std::string(luaL_typename(L, next + 1)));
            }
        }

        int progressIndex = 0;
        int completeIndex = 0;
        bool optionsBody = false;
        // readOptions keeps the callback functions it finds above the arguments
        const int result = readOptions(L, call, optionsIndex, desc, owner, completeIndex, progressIndex, optionsBody, url, hasUrl);
        if (result == RAISE) return RAISE;
        if (call == Call::Request && completeIndex) callbackIndex = completeIndex;
        if (call == Call::Request && !hasUrl){
            return pushRaise(L, fn + ": options.url is missing");
        }
        if (call == Call::Request && desc.method == "GET" && optionsBody){
            return pushRaise(L, fn + ": a GET request has no body (set method, e.g. \"POST\")");
        }
        if (call == Call::Request && desc.method == "HEAD" && optionsBody){
            return pushRaise(L, fn + ": a HEAD request has no body");
        }

        if (bodyArg && !isAbsent(L, bodyArg)){
            if (optionsBody){
                return pushRaise(L, fn + ": the body is given twice (the argument and options.body/json/form)");
            }
            if (lua_type(L, bodyArg) == LUA_TSTRING){
                setBody(desc, luaString(L, bodyArg));
            } else {
                nlohmann::json json;
                std::string error;
                if (!LuaJson::toJson(L, bodyArg, json, LuaJson::EncodeOptions(), error)){
                    return pushRaise(L, fn + ": body cannot be sent as JSON: " + error);
                }
                setBody(desc, json.dump());
                if (!hasHeader(desc.headers, "content-type")) desc.headers.emplace_back("Content-Type", "application/json");
            }
        }
        desc.url = url;

        auto context = std::make_shared<CallContext>();
        if (callbackIndex) context->onComplete.set(L, callbackIndex);
        if (progressIndex) context->onProgress.set(L, progressIndex);
        context->request = std::make_shared<HttpRequestLua>();

        HttpProgress onProgress;
        if (progressIndex){
            onProgress = [context](uint64_t received, int64_t total) { runProgress(*context, received, total); };
        }
        HttpCallback onComplete = [context](const HttpResponse&) { runCompletion(*context); };
        context->request->handle = Http::send(std::move(desc), std::move(onComplete), std::move(onProgress), owner);

        if (!luabridge::push(L, context->request)){
            lua_pushnil(L);
            return 1;
        }
        // the request's own userdata (onProgress gets it) until the request ends
        if (!context->request->isDone()) context->self.set(L, -1);
        return 1;
    }

    int luaHttpGet(lua_State* L){ return finish(L, startImpl(L, Call::Get)); }
    int luaHttpHead(lua_State* L){ return finish(L, startImpl(L, Call::Head)); }
    int luaHttpDelete(lua_State* L){ return finish(L, startImpl(L, Call::Delete)); }
    int luaHttpPost(lua_State* L){ return finish(L, startImpl(L, Call::Post)); }
    int luaHttpPut(lua_State* L){ return finish(L, startImpl(L, Call::Put)); }
    int luaHttpPatch(lua_State* L){ return finish(L, startImpl(L, Call::Patch)); }
    int luaHttpRequest(lua_State* L){ return finish(L, startImpl(L, Call::Request)); }
    int luaHttpDownload(lua_State* L){ return finish(L, startImpl(L, Call::Download)); }
    int luaHttpGetTexture(lua_State* L){ return finish(L, startImpl(L, Call::Texture)); }

    int urlTextImpl(lua_State* L, bool encode){
        if (lua_type(L, 1) != LUA_TSTRING && lua_type(L, 1) != LUA_TNUMBER){
            return pushRaise(L, std::string(encode ? "Http.encodeUrl" : "Http.decodeUrl") + ": the argument must be a string, got " + luaL_typename(L, 1));
        }
        lua_pushvalue(L, 1);
        const std::string text = luaString(L, -1);
        lua_pop(L, 1);
        const std::string out = encode ? Http::encodeUrl(text) : Http::decodeUrl(text);
        lua_pushlstring(L, out.data(), out.size());
        return 1;
    }

    int luaHttpEncodeUrl(lua_State* L){ return finish(L, urlTextImpl(L, true)); }
    int luaHttpDecodeUrl(lua_State* L){ return finish(L, urlTextImpl(L, false)); }

    // ---------------------------------------------------------------- HttpRequest

    luabridge::LuaRef requestResponse(const HttpRequestLua* self, lua_State* L){
        if (!self || !pushResponseObject(L, const_cast<HttpRequestLua&>(*self))) lua_pushnil(L);
        return luabridge::LuaRef::fromStack(L);
    }

    const char* stateName(HttpState state){
        switch (state){
            case HttpState::Queued: return "queued";
            case HttpState::Running: return "running";
            case HttpState::Done: return "done";
            case HttpState::Cancelled: return "cancelled";
        }
        return "?";
    }

    HttpResponseLua* responseOf(lua_State* L, int index){
        if (!luabridge::isInstance<HttpResponseLua>(L, index)) return nullptr;
        auto result = luabridge::get<HttpResponseLua*>(L, index);
        return result ? result.value() : nullptr;
    }

    std::string requestText(const HttpRequestLua* self){
        return "HttpRequest(" + std::to_string(self->getId()) + " " + self->getMethod() + " " + self->getUrl() + " " +
               stateName(self->getState()) + ")";
    }

    // ---------------------------------------------------------------- HttpResponse

    // pushes the body (a Lua string made once; the C++ bytes are freed then)
    void pushBody(lua_State* L, HttpResponseLua& self){
        if (self.bodyString.push(L)) return;
        std::vector<uint8_t>& body = self.response->body;
        lua_pushlstring(L, body.empty() ? "" : reinterpret_cast<const char*>(body.data()), body.size());
        self.bodyString.set(L, -1);
        std::vector<uint8_t>().swap(body);
    }

    luabridge::LuaRef responseBody(const HttpResponseLua* self, lua_State* L){
        if (!self || !self->response) lua_pushliteral(L, "");
        else pushBody(L, const_cast<HttpResponseLua&>(*self));
        return luabridge::LuaRef::fromStack(L);
    }

    luabridge::LuaRef responseHeaders(const HttpResponseLua* self, lua_State* L){
        lua_newtable(L);
        if (self && self->response){
            const int table = lua_gettop(L);
            for (const auto& [name, value] : self->response->headers){
                lua_pushlstring(L, name.data(), name.size());
                lua_rawget(L, table);
                if (lua_type(L, -1) == LUA_TSTRING){
                    // a repeated header: joined (Set-Cookie values may contain commas)
                    std::string joined = luaString(L, -1) + (name == "set-cookie" ? "\n" : ", ") + value;
                    lua_pop(L, 1);
                    lua_pushlstring(L, name.data(), name.size());
                    lua_pushlstring(L, joined.data(), joined.size());
                } else {
                    lua_pop(L, 1);
                    lua_pushlstring(L, name.data(), name.size());
                    lua_pushlstring(L, value.data(), value.size());
                }
                lua_rawset(L, table);
            }
        }
        return luabridge::LuaRef::fromStack(L);
    }

    luabridge::LuaRef responseData(const HttpResponseLua* self, lua_State* L){
        HttpResponseLua* s = const_cast<HttpResponseLua*>(self);
        if (!s || !s->response || !s->response->json){
            lua_pushnil(L);
        } else if (!s->dataValue.push(L)){
            LuaJson::push(L, s->response->json->value);
            s->dataValue.set(L, -1);
        }
        return luabridge::LuaRef::fromStack(L);
    }

    luabridge::LuaRef responseTextureData(const HttpResponseLua* self, lua_State* L){
        if (!self || !self->response || !self->response->texture){
            lua_pushnil(L);
        } else if (!luabridge::push(L, self->response->takeTexture())){
            lua_pushnil(L);
        }
        return luabridge::LuaRef::fromStack(L);
    }

    int responseJsonImpl(lua_State* L){
        HttpResponseLua* self = responseOf(L, 1);
        if (!self || !self->response){
            return pushRaise(L, "HttpResponse:json: call it with a colon (response:json())");
        }
        if (self->response->json){
            LuaJson::push(L, self->response->json->value);
            return 1;
        }
        pushBody(L, *self);
        size_t n = 0;
        const char* text = lua_tolstring(L, -1, &n);
        const std::string body(text ? text : "", n);
        lua_pop(L, 1);
        if (body.empty()){
            lua_pushnil(L);
            lua_pushliteral(L, "the body is empty");
            return 2;
        }
        std::string error;
        if (!LuaJson::decode(L, body, LuaJson::DecodeOptions(), error)){
            lua_pushnil(L);
            lua_pushlstring(L, error.data(), error.size());
            return 2;
        }
        return 1;
    }

    int luaResponseJson(lua_State* L){ return finish(L, responseJsonImpl(L)); }

    int responseHeaderImpl(lua_State* L){
        HttpResponseLua* self = responseOf(L, 1);
        if (!self || !self->response){
            return pushRaise(L, "HttpResponse:header: call it with a colon (response:header(\"content-type\"))");
        }
        if (lua_type(L, 2) != LUA_TSTRING){
            return pushRaise(L, std::string("HttpResponse:header: the name must be a string, got ") + luaL_typename(L, 2));
        }
        const std::string name = HttpUtil::lower(luaString(L, 2));
        bool found = false;
        std::string joined;
        for (const auto& [header, value] : self->response->headers){
            if (header != name) continue;
            if (found) joined += name == "set-cookie" ? "\n" : ", ";
            joined += value;
            found = true;
        }
        if (!found){
            lua_pushnil(L);
        } else {
            lua_pushlstring(L, joined.data(), joined.size());
        }
        return 1;
    }

    int luaResponseHeader(lua_State* L){ return finish(L, responseHeaderImpl(L)); }

    std::string responseText(const HttpResponseLua* self){
        if (!self->response) return "HttpResponse()";
        const HttpResponse& r = *self->response;
        const std::string what = r.status != 0 ? std::to_string(r.status) : r.errorCode;
        return "HttpResponse(" + what + " " + r.method + " " + r.url + ")";
    }

}
#endif //DISABLE_LUA_BINDINGS

void LuaBinding::registerNetClasses(lua_State* L){
#ifndef DISABLE_LUA_BINDINGS
    // HttpRequest.state
    luabridge::getGlobalNamespace(L)
        .beginNamespace("HttpState")
        .addVariable("Queued", HttpState::Queued)
        .addVariable("Running", HttpState::Running)
        .addVariable("Done", HttpState::Done)
        .addVariable("Cancelled", HttpState::Cancelled)
        .endNamespace();

    // The handle every Http call returns (keeping it is optional: the request runs without it)
    luabridge::getGlobalNamespace(L)
        .beginClass<HttpRequestLua>("HttpRequest")
        .addProperty("id", &HttpRequestLua::getId)
        .addProperty("url", &HttpRequestLua::getUrl)
        .addProperty("method", &HttpRequestLua::getMethod)
        .addProperty("state", &HttpRequestLua::getState)
        .addProperty("progress", &HttpRequestLua::getProgress)
        .addProperty("bytesReceived", &HttpRequestLua::getBytesReceived)
        .addProperty("bytesTotal", &HttpRequestLua::getBytesTotal)
        // the HttpResponse once the request is done or cancelled, else nil
        .addProperty("response", &requestResponse)
        .addFunction("isDone", &HttpRequestLua::isDone)
        .addFunction("cancel", &HttpRequestLua::cancel)
        .addFunction("__tostring", &requestText)
        .endClass();

    // What the callback gets (Unity's UnityWebRequest result, downloadHandler and error)
    luabridge::getGlobalNamespace(L)
        .beginClass<HttpResponseLua>("HttpResponse")
        .addProperty("ok", &HttpResponseLua::getOk)
        .addProperty("status", &HttpResponseLua::getStatus)
        .addProperty("errorCode", &HttpResponseLua::getErrorCode)
        .addProperty("error", &HttpResponseLua::getError)
        .addProperty("url", &HttpResponseLua::getUrl)
        .addProperty("method", &HttpResponseLua::getMethod)
        .addProperty("redirected", &HttpResponseLua::getRedirected)
        .addProperty("file", &HttpResponseLua::getFile)
        .addProperty("elapsed", &HttpResponseLua::getElapsed)
        .addProperty("bytesReceived", &HttpResponseLua::getBytesReceived)
        // lowercase name -> value (repeated headers joined with ", ", set-cookie with "\n"); a new table each read
        .addProperty("headers", &responseHeaders)
        // the body as a string (binary-safe; "" for HEAD, downloads and textures)
        .addProperty("body", &responseBody)
        // responseType = "json": the decoded body (Json.decode), else nil
        .addProperty("data", &responseData)
        // Http.getTexture: the decoded image (image:setTexture(response.url, response.textureData)), else nil
        .addProperty("textureData", &responseTextureData)
        .addFunction("json", &luaResponseJson)
        .addFunction("header", &luaResponseHeader)
        .addFunction("__tostring", &responseText)
        .endClass();

    // The HTTP(S) client (docs/mcp/guide/networking.md). Callbacks run on the main thread right
    // before onUpdate; errors are values (response.ok, errorCode, error)
    luabridge::getGlobalNamespace(L)
        .beginClass<Http>("Http")
        .addStaticFunction("get", &luaHttpGet)
        .addStaticFunction("head", &luaHttpHead)
        .addStaticFunction("delete", &luaHttpDelete)
        .addStaticFunction("post", &luaHttpPost)
        .addStaticFunction("put", &luaHttpPut)
        .addStaticFunction("patch", &luaHttpPatch)
        .addStaticFunction("request", &luaHttpRequest)
        .addStaticFunction("download", &luaHttpDownload)
        .addStaticFunction("getTexture", &luaHttpGetTexture)
        .addStaticFunction("cancelAll", &Http::cancelAll)
        .addStaticFunction("isAvailable", &Http::isAvailable)
        .addStaticFunction("encodeUrl", &luaHttpEncodeUrl)
        .addStaticFunction("decodeUrl", &luaHttpDecodeUrl)
        .addStaticProperty("maxConcurrentRequests", &Http::getMaxConcurrentRequests, &Http::setMaxConcurrentRequests)
        .addStaticProperty("userAgent", &Http::getUserAgent, &Http::setUserAgent)
        .addStaticProperty("timeout", &Http::getDefaultTimeout, &Http::setDefaultTimeout)
        .addStaticProperty("logFailures", &Http::getLogFailures, &Http::setLogFailures)
        .endClass();
#endif //DISABLE_LUA_BINDINGS
}
