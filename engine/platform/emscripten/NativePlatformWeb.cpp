// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The native bridge's platform layer on the web (core/native/NativePlatform.h): JavaScript
// objects registered with JasperNative.register(name, object), or a global by dotted path
// ("window.localStorage", "navigator.clipboard"; a bare name is a registered object only). The page's window.JasperNative is the helper plugin
// scripts use (register, emit, canvas, pauseEngine, resumeEngine); the export puts a small
// stub of it before the plugin scripts, which queues what they do until the engine starts
// (jasper_native_init adopts it). Everything is single-threaded: "background" is the same as
// "main", an async call runs in a later microtask, never inside the engine's call.

#if defined(__EMSCRIPTEN__)

#include "Log.h"
#include "native/NativePlatform.h"

#include <emscripten.h>

#include <cstdlib>
#include <string>

using namespace jasperengine;

EM_JS_DEPS(jasper_native, "$UTF8ToString,$stringToNewUTF8");

// Makes window.JasperNative the full helper, keeping what a stub or the page put there, and
// adopts the registrations and events queued before the engine started
EM_JS(void, jasper_native_init, (), {
    var root = typeof globalThis !== "undefined" ? globalThis : window;
    var jn = root.JasperNative || {};
    if (jn._jasperReady) return;
    var queued = jn._pending || [];
    var queuedEvents = jn._pendingEvents || [];
    jn._objects = jn._objects || {};

    // JSON text of a value as plugin code returns it; throws {code, message} for what JSON
    // cannot hold (functions, symbols, BigInt, DOM nodes, Map/Set, cycles, NaN, Infinity)
    var toJson = function(value) {
        var stack = [];
        var write = function(v, depth) {
            if (depth > 64) throw { code: "unsupported_type", message: "a value nested deeper than 64" };
            if (v === null || v === undefined) return "null";
            var t = typeof v;
            if (t === "boolean") return v ? "true" : "false";
            if (t === "number") {
                if (!isFinite(v)) throw { code: "unsupported_type", message: "NaN and infinities have no JSON form" };
                return JSON.stringify(v);
            }
            if (t === "string") return JSON.stringify(v);
            if (t !== "object") throw { code: "unsupported_type", message: "a " + t + " has no JSON form" };
            if (typeof v.toJSON === "function") return write(v.toJSON(), depth);
            if (stack.indexOf(v) >= 0) throw { code: "unsupported_type", message: "a cycle has no JSON form" };
            var parts = [];
            stack.push(v);
            if (Array.isArray(v)) {
                for (var i = 0; i < v.length; i++) parts.push(write(v[i], depth + 1));
                stack.pop();
                return "[" + parts.join(",") + "]";
            }
            var kind = Object.prototype.toString.call(v);
            if (kind !== "[object Object]") {
                throw { code: "unsupported_type", message: "a " + kind.slice(8, -1) + " has no JSON form (return plain objects, arrays, strings, numbers, booleans or null)" };
            }
            var keys = Object.keys(v);
            for (var k = 0; k < keys.length; k++) {
                var item = v[keys[k]];
                // as JSON.stringify: undefined and function members are left out
                if (item === undefined || typeof item === "function") continue;
                parts.push(JSON.stringify(keys[k]) + ":" + write(item, depth + 1));
            }
            stack.pop();
            return "{" + parts.join(",") + "}";
        };
        return write(value, 0);
    };
    Module.jasperNativeToJson = toJson;

    // the registered object; else, for a dotted name ("navigator.clipboard", "window.localStorage"),
    // the global at that path; null when there is none. A bare name is never a browser global
    // (Notification, Audio, Image...): it would hide the plugin's Lua mock.
    Module.jasperNativeTarget = function(plugin) {
        if (Object.prototype.hasOwnProperty.call(jn._objects, plugin)) return jn._objects[plugin];
        if (plugin.indexOf(".") < 0) return null;
        var target = root;
        var parts = plugin.split(".");
        if (parts[0] === "window" || parts[0] === "globalThis") parts.shift();
        for (var i = 0; i < parts.length; i++) {
            if (target === null || target === undefined) return null;
            try { target = target[parts[i]]; } catch (e) { return null; }
        }
        // an object or a function: a plugin has functions to call
        return (target !== null && (typeof target === "object" || typeof target === "function")) ? target : null;
    };

    jn.register = function(name, object) {
        if (typeof name !== "string" || !object) {
            console.error("JasperNative.register(name, object): a name and an object of functions");
            return;
        }
        jn._objects[name] = object;
    };
    jn.emit = function(plugin, event, payload) {
        var text;
        try {
            text = toJson(payload);
        } catch (e) {
            console.error("JasperNative.emit(" + plugin + ", " + event + "): the payload is dropped: " + (e && e.message ? e.message : e));
            return;
        }
        var p = stringToNewUTF8(String(plugin));
        var ev = stringToNewUTF8(String(event));
        var t = stringToNewUTF8(text);
        _jasper_native_emit(p, ev, t);
        _free(p);
        _free(ev);
        _free(t);
    };
    jn.pauseEngine = function() { _jasper_native_pause(1); };
    jn.resumeEngine = function() { _jasper_native_pause(0); };
    Object.defineProperty(jn, "canvas", { configurable: true, get: function() { return Module.canvas; } });
    jn._jasperReady = true;
    delete jn._pending;
    delete jn._pendingEvents;
    root.JasperNative = jn;
    Module.jasperNativeCancelled = {};

    for (var i = 0; i < queued.length; i++) jn.register(queued[i][0], queued[i][1]);
    for (var j = 0; j < queuedEvents.length; j++) jn.emit(queuedEvents[j][0], queuedEvents[j][1], queuedEvents[j][2]);
});

EM_JS(int, jasper_native_has, (const char* plugin, const char* method), {
    if (!Module.jasperNativeTarget) return 0;
    var target = Module.jasperNativeTarget(UTF8ToString(plugin));
    if (target === null) return 0;
    var name = UTF8ToString(method);
    if (name === "") return 1;
    try { return typeof target[name] === "function" ? 1 : 0; } catch (e) { return 0; }
});

// {"v": value} or {"c": code, "m": message}, malloc'd
EM_JS(char*, jasper_native_call, (const char* plugin, const char* method, const char* argsJson), {
    var pluginName = UTF8ToString(plugin);
    var methodName = UTF8ToString(method);
    var answer = function(text) { return stringToNewUTF8(text); };
    var fail = function(code, message) { return answer(JSON.stringify({ c: code, m: message })); };
    var target = Module.jasperNativeTarget ? Module.jasperNativeTarget(pluginName) : null;
    if (target === null) return fail("not_found", "no JavaScript object for the native plugin \"" + pluginName + "\"");
    var fn;
    try { fn = target[methodName]; } catch (e) { fn = undefined; }
    if (typeof fn !== "function") return fail("no_method", "the JavaScript plugin \"" + pluginName + "\" has no function \"" + methodName + "\"");
    var result;
    try {
        result = fn.apply(target, JSON.parse(UTF8ToString(argsJson)));
    } catch (e) {
        console.error("Native.call " + pluginName + "." + methodName + " threw:", e);
        return fail("exception", String(e));
    }
    if (result !== null && (typeof result === "object" || typeof result === "function") && typeof result.then === "function") {
        // the promise still runs; its result is lost
        if (typeof result.catch === "function") result.catch(function() {});
        return fail("async_only", pluginName + "." + methodName + " returned a Promise: use Native.callAsync");
    }
    try {
        return answer("{\"v\":" + Module.jasperNativeToJson(result) + "}");
    } catch (e) {
        return fail(e && e.code ? e.code : "unsupported_type", pluginName + "." + methodName + " returned " + (e && e.message ? e.message : String(e)));
    }
});

EM_JS(void, jasper_native_call_async, (double id, const char* plugin, const char* method, const char* argsJson), {
    var pluginName = UTF8ToString(plugin);
    var methodName = UTF8ToString(method);
    var argsText = UTF8ToString(argsJson);
    var complete = function(json, code, message) {
        if (Module.jasperNativeCancelled[id]) {
            delete Module.jasperNativeCancelled[id];
            return;
        }
        var j = json === null ? 0 : stringToNewUTF8(json);
        var c = code === null ? 0 : stringToNewUTF8(code);
        var m = message === null ? 0 : stringToNewUTF8(message);
        _jasper_native_complete(id, j, c, m);
        if (j) _free(j);
        if (c) _free(c);
        if (m) _free(m);
    };
    // a later microtask: never inside the engine's call
    Promise.resolve().then(function() {
        if (Module.jasperNativeCancelled[id]) return undefined;
        var target = Module.jasperNativeTarget ? Module.jasperNativeTarget(pluginName) : null;
        if (target === null) throw { code: "not_found", message: "no JavaScript object for the native plugin \"" + pluginName + "\"" };
        var fn = target[methodName];
        if (typeof fn !== "function") throw { code: "no_method", message: "the JavaScript plugin \"" + pluginName + "\" has no function \"" + methodName + "\"" };
        return fn.apply(target, JSON.parse(argsText));
    }).then(function(value) {
        var text;
        try {
            text = Module.jasperNativeToJson(value);
        } catch (e) {
            complete(null, "unsupported_type", pluginName + "." + methodName + " resolved with " + (e && e.message ? e.message : String(e)));
            return;
        }
        complete(text, null, null);
    }, function(reason) {
        // {code, message} keeps its code; anything else is an exception
        if (reason !== null && typeof reason === "object" && typeof reason.code === "string" && reason.code !== "") {
            complete(null, reason.code, reason.message === undefined ? "" : String(reason.message));
            return;
        }
        console.error("Native.callAsync " + pluginName + "." + methodName + " failed:", reason);
        complete(null, "exception", String(reason));
    });
});

EM_JS(void, jasper_native_cancel, (double id), {
    if (Module.jasperNativeCancelled) Module.jasperNativeCancelled[id] = true;
});

EM_JS(void, jasper_native_shutdown, (), {
    // the promises still running complete into nothing (the engine dropped their calls)
    if (Module.jasperNativeCancelled) Module.jasperNativeCancelled = {};
});

namespace {

    bool parseJson(const char* text, NativeJson& out){
        out = NativeJson::parse(text ? text : "", nullptr, false);
        return !out.is_discarded();
    }

    class WebNativePlatform : public NativePlatform {
    public:
        WebNativePlatform(){
            jasper_native_init();
        }

        const char* name() const override{ return "Web"; }

        bool has(const std::string& plugin, const std::string& method) override{
            return jasper_native_has(plugin.c_str(), method.c_str()) != 0;
        }

        NativeJson call(const std::string& plugin, const std::string& method, const NativeJson& args, NativeError& err) override{
            char* text = jasper_native_call(plugin.c_str(), method.c_str(), args.dump().c_str());
            NativeJson envelope;
            const bool ok = parseJson(text, envelope);
            std::free(text);
            if (!ok || !envelope.is_object()){
                err = NativeError("unsupported_type", plugin + "." + method + " returned text that is not JSON");
                return nullptr;
            }
            if (envelope.contains("c")){
                err = NativeError(envelope.value("c", std::string("exception")), envelope.value("m", std::string()));
                return nullptr;
            }
            return envelope.contains("v") ? envelope["v"] : NativeJson();
        }

        void callAsync(uint64_t id, const std::string& plugin, const std::string& method, const NativeJson& args, bool) override{
            jasper_native_call_async((double)id, plugin.c_str(), method.c_str(), args.dump().c_str());
        }

        void cancel(uint64_t id) override{
            jasper_native_cancel((double)id);
        }

        void shutdown() override{
            jasper_native_shutdown();
        }
    };

    // window.JasperNative becomes the full helper when the engine's code starts, so plugin
    // events and registrations work before the first Native call
    struct WebInit {
        WebInit(){ jasper_native_init(); }
    } webInit;

}

extern "C" {

EMSCRIPTEN_KEEPALIVE void jasper_native_complete(double id, const char* json, const char* code, const char* message){
    if (code && code[0]){
        const NativeError err(code, message ? message : "");
        NativeBridge::completeFromPlatform((uint64_t)id, nullptr, &err);
        return;
    }
    NativeJson value;
    if (!parseJson(json, value)){
        const NativeError err("unsupported_type", "the JavaScript result is not JSON");
        NativeBridge::completeFromPlatform((uint64_t)id, nullptr, &err);
        return;
    }
    NativeBridge::completeFromPlatform((uint64_t)id, std::move(value), nullptr);
}

EMSCRIPTEN_KEEPALIVE void jasper_native_emit(const char* plugin, const char* event, const char* json){
    NativeJson payload;
    if (!parseJson(json, payload)){
        Log::error("Native: an event of %s is not JSON; it is dropped", plugin ? plugin : "?");
        return;
    }
    NativeBridge::emit(plugin ? plugin : "", event ? event : "", std::move(payload));
}

EMSCRIPTEN_KEEPALIVE void jasper_native_pause(int pause){
    if (pause) NativeBridge::pauseEngine();
    else NativeBridge::resumeEngine();
}

}

NativePlatform* jasperengine::createNativePlatform(){
    return new WebNativePlatform();
}

#endif
