// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Smoke test of the web native bridge's JavaScript side (engine/platform/emscripten/
// NativePlatformWeb.cpp) under node, without the engine: the few NativeBridge and Log
// functions it calls are stubs that record what arrives. run.sh builds and runs it.

#include "native/NativePlatform.h"
#include "Log.h"

#include <emscripten.h>
#include <emscripten/eventloop.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {
    struct Completion { NativeJson value; std::string code, message; };
    std::map<uint64_t, Completion> completions;
    std::vector<std::string> events;
    int pauses = 0;
    int failures = 0;
    NativePlatform* platform = nullptr;

    void check(bool ok, const std::string& what){
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        if (!ok) failures++;
    }

    NativeJson sync(const char* plugin, const char* method, NativeJson args, NativeError& err){
        err = NativeError();
        return platform->call(plugin, method, args, err);
    }
}

// ---- the stubs
void NativeBridge::completeFromPlatform(uint64_t id, NativeJson result, const NativeError* error){
    completions[id] = {result, error ? error->code : "", error ? error->message : ""};
}
void NativeBridge::emit(const std::string& plugin, const std::string& event, NativeJson payload){
    events.push_back(plugin + "." + event + "=" + payload.dump());
}
void NativeBridge::pauseEngine(){ pauses++; }
void NativeBridge::resumeEngine(){ pauses--; }
void Log::error(const char* text, ...){
    va_list args;
    va_start(args, text);
    std::vprintf(text, args);
    va_end(args);
    std::printf("\n");
}

EM_JS(void, pageStub, (), {
        globalThis.JasperNative = globalThis.JasperNative || {_pending: [], _pendingEvents: [],
            register: function(n, o) { this._pending.push([n, o]); },
            emit: function(p, e, d) { this._pendingEvents.push([p, e, d]); }};
});

EM_JS(void, registerPlugins, (), {
        globalThis.JasperNative.register("Poki", {
            add: function(a, b) { return a + b; },
            info: function() { return {name: "poki", list: [1, 2.5, null, "x"], nested: {ok: true}, skip: undefined}; },
            later: function() { return new Promise(function(r) { setTimeout(function() { r({sum: 5}); }, 5); }); },
            refuse: function() { return Promise.reject({code: "user_closed", message: "closed"}); },
            boom: function() { throw new Error("boom"); },
            fn: function() { return function() {}; },
            cyc: function() { var a = {}; a.self = a; return a; },
            nan: function() { return NaN; },
            seven: function() { return 7; },
            self: function() { return this === globalThis.JasperNative._objects.Poki; },
            emitLater: function() { JasperNative.emit("Poki", "adDone", {ok: true}); JasperNative.pauseEngine(); JasperNative.pauseEngine(); JasperNative.resumeEngine(); }
    });
    globalThis.Tools = { math: { twice: function(x) { return x * 2; } } };
});

EM_JS(void, emitEarly, (), {
    JasperNative.emit("Early", "ready", {n: 1});
});

EM_JS(int, hasCanvas, (), {
    return Object.getOwnPropertyDescriptor(JasperNative, "canvas") ? 1 : 0;
});

static void finish(void*){
    check(completions.count(1) && completions[1].code.empty() && completions[1].value == NativeJson{{"sum", 5}}, "async resolve (a Promise)");
    check(completions.count(2) && completions[2].code == "user_closed" && completions[2].message == "closed", "async reject with {code, message}");
    check(completions.count(3) && completions[3].code == "exception" && completions[3].message.find("boom") != std::string::npos, "async throw -> exception");
    check(completions.count(4) && completions[4].value == 7, "async plain value");
    check(completions.count(5) == 0, "a cancelled call completes into nothing");
    check(completions.count(6) && completions[6].code == "not_found", "async unknown plugin");
    check(completions.count(7) && completions[7].code == "unsupported_type", "async non-JSON result");
    check(events.size() == 2 && events[0] == "Early.ready={\"n\":1}" && events[1] == "Poki.adDone={\"ok\":true}", "events (queued before the engine, and later)");
    check(pauses == 1, "pauseEngine / resumeEngine reach the engine");
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    emscripten_force_exit(failures ? 1 : 0);
}

int main(){
    // the page: what the export's stub and a plugin script do before the engine is there
    pageStub();
    platform = createNativePlatform();
    registerPlugins();
    check(hasCanvas() == 1, "JasperNative.canvas exists");
    check(platform->has("Poki", "") && platform->has("Poki", "add") && !platform->has("Poki", "nope"), "has (registered before the engine started)");
    check(platform->has("Tools.math", "twice") && platform->has("Math", "max") && !platform->has("Nope", ""), "has (global paths)");
    check(!platform->has("name", ""), "a global that is not an object is no plugin");

    NativeError err;
    check(sync("Poki", "add", NativeJson::array({2, 3}), err) == 5 && !err.isSet(), "sync call");
    NativeJson info = sync("Poki", "info", NativeJson::array(), err);
    check(!err.isSet() && info["list"][1] == 2.5 && info["list"][2].is_null() && info["nested"]["ok"] == true && !info.contains("skip"), "sync object result");
    check(sync("Tools.math", "twice", NativeJson::array({21}), err) == 42, "dotted global path");
    check(sync("Math", "max", NativeJson::array({1, 9, 3}), err) == 9, "a browser global (Math.max)");
    check(sync("Poki", "self", NativeJson::array(), err) == true, "this is the plugin object");
    sync("Poki", "later", NativeJson::array(), err);
    check(err.code == "async_only", "a Promise to a sync call -> async_only");
    sync("Poki", "boom", NativeJson::array(), err);
    check(err.code == "exception" && err.message.find("boom") != std::string::npos, "throw -> exception");
    sync("Poki", "fn", NativeJson::array(), err);
    check(err.code == "unsupported_type", "a function result -> unsupported_type");
    sync("Poki", "cyc", NativeJson::array(), err);
    check(err.code == "unsupported_type", "a cycle -> unsupported_type");
    sync("Poki", "nan", NativeJson::array(), err);
    check(err.code == "unsupported_type", "NaN -> unsupported_type");
    sync("Poki", "nope", NativeJson::array(), err);
    check(err.code == "no_method", "no_method");
    sync("Nobody", "x", NativeJson::array(), err);
    check(err.code == "not_found", "not_found");
    check(sync("Poki", "add", NativeJson::array({"é😀", "!"}), err) == "é😀!", "UTF-8 both ways");

    platform->callAsync(1, "Poki", "later", NativeJson::array(), false);
    platform->callAsync(2, "Poki", "refuse", NativeJson::array(), false);
    platform->callAsync(3, "Poki", "boom", NativeJson::array(), true);
    platform->callAsync(4, "Poki", "seven", NativeJson::array(), false);
    platform->callAsync(5, "Poki", "seven", NativeJson::array(), false);
    platform->cancel(5);
    platform->callAsync(6, "Nobody", "x", NativeJson::array(), false);
    platform->callAsync(7, "Poki", "fn", NativeJson::array(), false);
    check(completions.empty(), "nothing completes inside callAsync");
    emitEarly();
    sync("Poki", "emitLater", NativeJson::array(), err);
    emscripten_set_timeout(finish, 100, nullptr);
    return 0;
}
