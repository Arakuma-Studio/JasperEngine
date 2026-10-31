// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NETCLASSESLUA_H
#define NETCLASSESLUA_H

// The Lua objects of the HTTP client (NetClassesLua.cpp): HttpRequest (what Http.get & co.
// return; keeping it is optional) and HttpResponse (what the callback gets). C++ code uses
// net/Http.h directly.

#include "net/Http.h"

#include <cstdint>
#include <memory>
#include <string>

typedef struct lua_State lua_State;

namespace jasperengine {

    // A registry reference in the engine's Lua state, released on the main thread (never into
    // a state that is gone)
    class LuaRegistrySlot {
    public:
        LuaRegistrySlot() = default;
        ~LuaRegistrySlot();
        LuaRegistrySlot(const LuaRegistrySlot&) = delete;
        LuaRegistrySlot& operator=(const LuaRegistrySlot&) = delete;

        // keeps the value at `index` of L (any thread of the engine's state)
        void set(lua_State* L, int index);
        // pushes it on L; false (nothing pushed) when empty
        bool push(lua_State* L) const;
        void reset();
        bool isSet() const;

    private:
        lua_State* state = nullptr;   // the main state
        int ref = -2;                 // LUA_NOREF
    };

    class HttpResponseLua;

    class HttpRequestLua : public std::enable_shared_from_this<HttpRequestLua> {
    public:
        std::shared_ptr<HttpRequestHandle> handle;
        // the response's Lua object, made once (the callback's argument is `request.response`)
        LuaRegistrySlot responseObject;

        // unique per process, from 1
        uint64_t getId() const;
        // as sent, the query appended
        std::string getUrl() const;
        std::string getMethod() const;
        HttpState getState() const;
        // 0..1, -1 while the size is unknown
        float getProgress() const;
        uint64_t getBytesReceived() const;
        // -1 while unknown
        int64_t getBytesTotal() const;
        // Done or Cancelled (Coroutine.waitFor(request) waits for it)
        bool isDone() const;
        // Idempotent; a queued or running request's callback runs with errorCode "cancelled"
        void cancel();
    };

    class HttpResponseLua : public std::enable_shared_from_this<HttpResponseLua> {
    public:
        std::shared_ptr<HttpResponse> response;
        LuaRegistrySlot bodyString;   // the body, moved into a Lua string at its first read
        LuaRegistrySlot dataValue;    // responseType "json": the decoded body

        // true only for a response with status 200-299 read whole
        bool getOk() const;
        // the HTTP status; 0 when no response came (timeout, dns, ...)
        int getStatus() const;
        // "" when ok; http, timeout, dns, connect, tls, network, cancelled, offline,
        // insecure_http, too_many_redirects, redirect_blocked, invalid_url, file, decode,
        // too_large, unsupported
        std::string getErrorCode() const;
        // "" when ok: what failed and the URL ("timeout after 10 s: https://...")
        std::string getError() const;
        // the final URL (after redirects)
        std::string getUrl() const;
        // the request's method
        std::string getMethod() const;
        // a redirect was followed
        bool getRedirected() const;
        // where a download was written (the path as given), "" otherwise
        std::string getFile() const;
        // seconds from the start to the end
        double getElapsed() const;
        uint64_t getBytesReceived() const;
    };

}

#endif // NETCLASSESLUA_H
