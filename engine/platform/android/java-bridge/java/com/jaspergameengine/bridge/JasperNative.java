package com.jaspergameengine.bridge;

import android.app.Activity;
import android.content.Intent;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

// The native bridge on Android (docs/mcp/guide/native-bridge.md): what plugin code uses
// (getActivity, runOnUiThread, runOnGameThread, emit, lifecycle listeners, pauseEngine /
// resumeEngine, registerPlugin) and what the engine calls through JNI
// (engine/platform/android/NativePlatformAndroid.cpp): has, call, callAsync, cancel, shutdown,
// runGameThreadTask. MainActivity forwards its lifecycle to the dispatch* methods.
//
// A plugin is a class with public static methods: a dotted name is a class name; a simple one
// goes through the registry the export generates
// (com.jaspergameengine.nativeplugins.JasperNativeRegistry.CLASSES: simple name -> class name),
// then the registerPlugin aliases.
public final class JasperNative {

    private static final String TAG = "JasperNative";
    private static final String REGISTRY = "com.jaspergameengine.nativeplugins.JasperNativeRegistry";
    // how long a call made before the activity finished onCreate waits for it
    private static final long READY_TIMEOUT_MS = 5000;

    private static volatile Activity activity;
    private static final CountDownLatch ready = new CountDownLatch(1);
    private static final AtomicInteger initRuns = new AtomicInteger(0);
    private static final Handler mainHandler = new Handler(Looper.getMainLooper());

    private static final Map<String, Class<?>> aliases = new ConcurrentHashMap<>();
    private static final Map<String, Class<?>> resolved = new ConcurrentHashMap<>();
    private static volatile Map<String, String> registry;

    private static final List<JasperLifecycleListener> listeners = new CopyOnWriteArrayList<>();
    private static final Map<Long, NativeCall> pending = new ConcurrentHashMap<>();
    private static final Map<Long, Runnable> gameThreadTasks = new ConcurrentHashMap<>();
    private static final AtomicLong nextTask = new AtomicLong(1);

    // "background" calls: two daemon threads
    private static final ExecutorService background = Executors.newFixedThreadPool(2, new ThreadFactory() {
        private final AtomicInteger count = new AtomicInteger(1);
        @Override public Thread newThread(Runnable r) {
            Thread thread = new Thread(r, "JasperNative-" + count.getAndIncrement());
            thread.setDaemon(true);
            return thread;
        }
    });

    private JasperNative() {}

    // ---------------------------------------------------------------- for plugin code

    // The game's activity (null before MainActivity.onCreate)
    public static Activity getActivity() {
        return activity;
    }

    // Runs `r` on the UI thread: now when called there, else posted
    public static void runOnUiThread(Runnable r) {
        if (r == null) return;
        if (Looper.myLooper() == Looper.getMainLooper()) r.run();
        else mainHandler.post(r);
    }

    // Runs `r` on the game (engine) thread at the start of the next frame's update, where Lua
    // runs (the place to touch engine state from Java)
    public static void runOnGameThread(Runnable r) {
        if (r == null) return;
        long token = nextTask.getAndIncrement();
        gameThreadTasks.put(token, r);
        try {
            nativePostToGameThread(token);
        } catch (UnsatisfiedLinkError e) {
            gameThreadTasks.remove(token);
            warn("runOnGameThread: the engine is not loaded");
        }
    }

    // Fires the Lua handlers of Native.on(plugin, event, ...) in the next frame, with `payload`
    // converted as a method's return value is (any thread)
    public static void emit(String plugin, String event, Object payload) {
        String json;
        try {
            json = NativeReflection.toJson(payload);
        } catch (NativeReflection.BridgeException e) {
            Log.e(TAG, "emit(" + plugin + ", " + event + "): the payload is dropped: " + e.getMessage());
            return;
        }
        try {
            nativeEmit(plugin, event, json);
        } catch (UnsatisfiedLinkError e) {
            warn("emit: the engine is not loaded");
        }
    }

    public static void addLifecycleListener(JasperLifecycleListener listener) {
        if (listener != null && !listeners.contains(listener)) listeners.add(listener);
    }

    public static void removeLifecycleListener(JasperLifecycleListener listener) {
        listeners.remove(listener);
    }

    // While a full-screen SDK view covers the game (an ad): the engine pauses at the next frame
    // (audio paused, Engine.onPause). Counted: as many resumeEngine as pauseEngine.
    public static void pauseEngine() {
        try {
            nativePauseEngine();
        } catch (UnsatisfiedLinkError e) {
            warn("pauseEngine: the engine is not loaded");
        }
    }

    public static void resumeEngine() {
        try {
            nativeResumeEngine();
        } catch (UnsatisfiedLinkError e) {
            warn("resumeEngine: the engine is not loaded");
        }
    }

    // Lua's Native.call(name, ...) reaches `cls` (a class outside the plugins folder, or
    // another name for one)
    public static void registerPlugin(String name, Class<?> cls) {
        if (name == null || cls == null) return;
        aliases.put(name, cls);
        resolved.remove(name);
    }

    // ---------------------------------------------------------------- MainActivity

    // Right after super.onCreate, on the UI thread: every plugin class's
    // `public static void onJasperInit(Activity)` (once per process), then the listeners'
    // onCreate. Releases the calls that wait for the activity.
    public static void dispatchInit(Activity a, Bundle savedInstanceState) {
        activity = a;
        if (initRuns.getAndIncrement() == 0) {
            for (String className : registry().values()) {
                Class<?> cls = loadClass(className);
                if (cls == null) continue;
                try {
                    Method init = cls.getMethod("onJasperInit", Activity.class);
                    if (!Modifier.isStatic(init.getModifiers())) continue;
                    init.invoke(null, a);
                } catch (NoSuchMethodException e) {
                    // optional
                } catch (Throwable t) {
                    Throwable cause = t.getCause() != null ? t.getCause() : t;
                    Log.e(TAG, className + ".onJasperInit failed", cause);
                }
            }
        }
        ready.countDown();
        for (JasperLifecycleListener l : listeners) l.onCreate(savedInstanceState);
    }

    public static void dispatchStart() { for (JasperLifecycleListener l : listeners) l.onStart(); }
    public static void dispatchResume() { for (JasperLifecycleListener l : listeners) l.onResume(); }
    public static void dispatchPause() { for (JasperLifecycleListener l : listeners) l.onPause(); }
    public static void dispatchStop() { for (JasperLifecycleListener l : listeners) l.onStop(); }

    public static void dispatchDestroy(Activity a) {
        for (JasperLifecycleListener l : listeners) l.onDestroy();
        if (activity == a) activity = null;
    }

    public static void dispatchActivityResult(int requestCode, int resultCode, Intent data) {
        for (JasperLifecycleListener l : listeners) l.onActivityResult(requestCode, resultCode, data);
    }

    public static void dispatchRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        for (JasperLifecycleListener l : listeners) l.onRequestPermissionsResult(requestCode, permissions, grantResults);
    }

    public static void dispatchNewIntent(Intent intent) { for (JasperLifecycleListener l : listeners) l.onNewIntent(intent); }
    public static void dispatchWindowFocusChanged(boolean hasFocus) { for (JasperLifecycleListener l : listeners) l.onWindowFocusChanged(hasFocus); }
    public static void dispatchConfigurationChanged(Configuration c) { for (JasperLifecycleListener l : listeners) l.onConfigurationChanged(c); }

    // true when a listener consumed Back (the game then gets no Escape)
    public static boolean dispatchBackPressed() {
        for (JasperLifecycleListener l : listeners) {
            if (l.onBackPressed()) return true;
        }
        return false;
    }

    // ---------------------------------------------------------------- the engine (JNI)

    // method "": the plugin class exists
    public static boolean has(String plugin, String method) {
        Class<?> cls = findClass(plugin);
        if (cls == null) return false;
        return method == null || method.isEmpty() || NativeReflection.hasMethod(cls, method);
    }

    // On the game thread: {resultJson, errorCode, errorMessage} (code null on success)
    public static String[] call(String plugin, String method, String argsJson) {
        if (!waitReady()) {
            return new String[]{null, "not_ready", "the activity is not created yet (Native.call of " + plugin + "." + method + ")"};
        }
        Class<?> cls = findClass(plugin);
        if (cls == null) return new String[]{null, "not_found", "no class for the native plugin \"" + plugin + "\""};
        try {
            List<Object> args = NativeReflection.parseArray(argsJson);
            NativeReflection.Target target = NativeReflection.resolve(cls, plugin, method, args, false);
            Object result = NativeReflection.invoke(target, null);
            return new String[]{NativeReflection.toJson(result), null, null};
        } catch (NativeReflection.BridgeException e) {
            if ("exception".equals(e.code)) Log.e(TAG, plugin + "." + method + " threw " + e.getMessage());
            return new String[]{null, e.code, e.getMessage()};
        } catch (Throwable t) {
            Log.e(TAG, plugin + "." + method + " failed", t);
            return new String[]{null, "exception", NativeReflection.describe(t)};
        }
    }

    // Starts an async call; it completes through nativeComplete (once). background: a worker
    // thread, else the UI thread.
    public static void callAsync(final long id, final String plugin, final String method, String argsJson, boolean inBackground) {
        List<Object> args;
        try {
            args = NativeReflection.parseArray(argsJson);
        } catch (NativeReflection.BridgeException e) {
            complete(id, null, e.code, e.getMessage());
            return;
        }
        final NativeCall call = new NativeCall(id, plugin, method, args);
        pending.put(id, call);
        final List<Object> callArgs = args;
        Runnable run = new Runnable() {
            @Override public void run() {
                if (call.isCancelled()) {
                    pending.remove(id);
                    return;
                }
                if (!waitReady()) {
                    call.fail("not_ready", "the activity is not created yet (Native.callAsync of " + plugin + "." + method + ")");
                    return;
                }
                Class<?> cls = findClass(plugin);
                if (cls == null) {
                    call.fail("not_found", "no class for the native plugin \"" + plugin + "\"");
                    return;
                }
                try {
                    NativeReflection.Target target = NativeReflection.resolve(cls, plugin, method, callArgs, true);
                    Object result = NativeReflection.invoke(target, call);
                    // a plain method: its return value is the result
                    if (!target.async) call.resolve(result);
                } catch (NativeReflection.BridgeException e) {
                    if ("exception".equals(e.code)) Log.e(TAG, plugin + "." + method + " threw " + e.getMessage());
                    call.fail(e.code, e.getMessage());
                } catch (Throwable t) {
                    Log.e(TAG, plugin + "." + method + " failed", t);
                    call.fail("exception", NativeReflection.describe(t));
                }
            }
        };
        // posted, never run inside the engine's call
        if (inBackground) background.execute(run);
        else mainHandler.post(run);
    }

    public static void cancel(long id) {
        NativeCall call = pending.remove(id);
        if (call != null) call.markCancelled();
    }

    // Play stop / engine shutdown: every pending call is cancelled, queued game-thread tasks go
    public static void shutdown() {
        for (NativeCall call : pending.values()) call.markCancelled();
        pending.clear();
        gameThreadTasks.clear();
    }

    public static void runGameThreadTask(long token) {
        Runnable r = gameThreadTasks.remove(token);
        if (r == null) return;
        try {
            r.run();
        } catch (Throwable t) {
            Log.e(TAG, "runOnGameThread task failed", t);
        }
    }

    // ---------------------------------------------------------------- internals

    static void finishCall(NativeCall call, String json, String code, String message) {
        pending.remove(call.getId());
        if (call.isCancelled()) return;
        complete(call.getId(), json, code, message);
    }

    private static void complete(long id, String json, String code, String message) {
        try {
            nativeComplete(id, json, code, message);
        } catch (UnsatisfiedLinkError e) {
            warn("a native call completed without the engine loaded");
        }
    }

    static void warn(String message) {
        Log.w(TAG, message);
    }

    private static boolean waitReady() {
        try {
            return ready.await(READY_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    @SuppressWarnings("unchecked")
    private static Map<String, String> registry() {
        Map<String, String> classes = registry;
        if (classes != null) return classes;
        classes = Collections.emptyMap();
        try {
            Object value = Class.forName(REGISTRY).getField("CLASSES").get(null);
            if (value instanceof Map) classes = (Map<String, String>) value;
        } catch (ClassNotFoundException e) {
            // an export without plugins/native/android
        } catch (Throwable t) {
            Log.e(TAG, "The native plugin registry cannot be read", t);
        }
        registry = classes;
        return classes;
    }

    private static Class<?> loadClass(String name) {
        try {
            return Class.forName(name);
        } catch (ClassNotFoundException | LinkageError e) {
            return null;
        }
    }

    static Class<?> findClass(String plugin) {
        if (plugin == null || plugin.isEmpty()) return null;
        Class<?> cls = resolved.get(plugin);
        if (cls != null) return cls;
        if (plugin.indexOf('.') >= 0) {
            cls = loadClass(plugin);
        } else {
            String className = registry().get(plugin);
            if (className != null) cls = loadClass(className);
            if (cls == null) cls = aliases.get(plugin);
        }
        if (cls != null) resolved.put(plugin, cls);
        return cls;
    }

    // ---- the engine's side (registered by NativePlatformAndroid.cpp)

    private static native void nativeComplete(long id, String json, String code, String message);
    private static native void nativeEmit(String plugin, String event, String json);
    private static native void nativePauseEngine();
    private static native void nativeResumeEngine();
    private static native void nativePostToGameThread(long token);
}
