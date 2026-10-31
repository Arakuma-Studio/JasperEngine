package com.jaspergameengine.bridge;

import org.json.JSONArray;

import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

// One Native.callAsync from Lua, as the async method gets it (its trailing parameter). Complete
// it once, from any thread, at any time: the first resolve/reject wins, later ones are ignored
// with a warning. The result reaches the Lua callback in the next frame, on the game thread.
public final class NativeCall {

    private final long id;
    private final String plugin;
    private final String method;
    private final List<Object> args;
    private final AtomicBoolean done = new AtomicBoolean(false);
    private volatile boolean cancelled = false;

    NativeCall(long id, String plugin, String method, List<Object> args) {
        this.id = id;
        this.plugin = plugin;
        this.method = method;
        this.args = args;
    }

    public long getId() {
        return id;
    }

    // the Lua arguments (a new array each time)
    public JSONArray getArgs() {
        return NativeReflection.toJSONArray(args);
    }

    // The Lua side no longer waits (it cancelled, its owner went, the call timed out or the
    // game stopped): the work may stop, a result would be dropped
    public boolean isCancelled() {
        return cancelled;
    }

    // `value` as a method's return value would be (null, Boolean, Number, String, JSONObject,
    // JSONArray, Map, List, arrays)
    public void resolve(Object value) {
        if (!done.compareAndSet(false, true)) {
            JasperNative.warn("NativeCall " + plugin + "." + method + " (" + id + ") was completed twice; the second result is ignored");
            return;
        }
        String json;
        try {
            json = NativeReflection.toJson(value);
        } catch (NativeReflection.BridgeException e) {
            JasperNative.finishCall(this, null, e.code, plugin + "." + method + " resolved with " + e.getMessage());
            return;
        }
        JasperNative.finishCall(this, json, null, null);
    }

    // code: "exception", "cancelled", "not_ready" or your own ("user_cancelled", ...)
    public void reject(String code, String message) {
        if (!done.compareAndSet(false, true)) {
            JasperNative.warn("NativeCall " + plugin + "." + method + " (" + id + ") was completed twice; the second result is ignored");
            return;
        }
        JasperNative.finishCall(this, null, code == null || code.isEmpty() ? "exception" : code, message == null ? "" : message);
    }

    // ---- the bridge

    boolean isDone() {
        return done.get();
    }

    // a failure the bridge itself reports (an exception thrown by the method): ignored when
    // the method completed the call before
    void fail(String code, String message) {
        if (done.compareAndSet(false, true)) JasperNative.finishCall(this, null, code, message);
    }

    void markCancelled() {
        cancelled = true;
    }

    String where() {
        return plugin + "." + method;
    }
}
