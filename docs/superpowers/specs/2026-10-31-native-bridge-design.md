# `native-bridge`: calling native platform code from Lua (design)

Status: binding spec for branch `gaps/native-bridge` (ledger #2). Read with
[AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) (shared conventions binding) and
[GAP-REPORT.md §1](../../engine-gaps/GAP-REPORT.md). Merges **after** `script-data` (JSON) and
`http` (reuses `MainThreadQueue` and `AsyncOwners` from that spec, section 6.1).

## 1. Goals

1. One Lua API, `Native`, to call native code of the running platform with JSON-compatible
   arguments and results, synchronously or asynchronously:
   Java/Kotlin static methods (Android), Objective-C/Swift class methods (iOS, macOS),
   JavaScript functions (Web), C++ functions registered by project C++ code (every platform,
   the desktop answer).
2. Native -> Lua events (`Native.on`), delivered on the main thread at the frame point of `http`.
3. A project folder convention, `plugins/native/`, whose sources, libraries, manifests and
   resources the exports pick up with no manual project editing.
4. Project settings for what SDKs need: Gradle dependencies and repositories, extra Android
   permissions and manifest XML, Info.plist keys, iOS system frameworks, CocoaPods, Swift
   packages, entitlements, deployment target, Web script includes.
5. Strong enough for a later ads/IAP wrapper written purely on top of it: activity/app lifecycle
   hooks, Android UI-thread and iOS main-thread dispatch, Activity / view controller / window
   access, engine pause/resume while a full-screen SDK view is shown.
6. Editor Play and desktop builds keep working without native code: Lua mocks
   (`<luaDir>/native_mocks/<Plugin>.lua`) and C++ plugins.
7. An MCP tool that scaffolds a plugin for all platforms at once, so agents never hand-write the
   boilerplate.

## 2. Non-goals

- Arbitrary object handles across the boundary (Unity's `AndroidJavaObject` instance calls,
  `AndroidJavaProxy`): the boundary is JSON-only; instance work is done inside a static wrapper
  method (the scaffold makes that one file). Decision 2 explains.
- C#-style P/Invoke into arbitrary dynamic libraries (`DllImport`) on desktop.
- Ads and IAP SDK wrappers themselves; migrating the existing AdMob/CrazyGames code (they stay
  as they are).
- Deep links / URL scheme registration, push notifications setup (an SDK can do them through the
  hooks and settings here, but the engine adds no feature for them).
- Running Java/ObjC plugins inside the editor (no JVM / no iOS runtime there); macOS ObjC plugins
  do run in macOS editor Play (decision 9).
- Swift plugins in CMake (non-Xcode) macOS desktop builds.
- Binary data other than strings that are valid UTF-8 (pass base64 text; documented).

## 3. Users and stories

- **Porting dev.** Unity's `AndroidJavaClass("...").CallStatic<int>("getCode")` becomes one Java
  file in `plugins/native/android/java/com/me/Device.java` with `public static int getCode()` and
  `local code, err = Native.call("Device", "getCode")`. On iOS the `DllImport("__Internal")`
  function becomes `+ (id)getCode:(NSArray*)args` in `plugins/native/ios/Device.m`.
- **SDK integrator (later ads/IAP).** Adds `com.wortise:android-sdk:1.7.1` and its Maven
  repositories in Project Settings > Native Plugins, `WortiseSDK` as a CocoaPod, writes
  `AdsBridge.java` / `AdsBridge.swift` using `JasperNative.getActivity()`, the lifecycle listener and
  `JasperNative.emit("AdsBridge", "rewarded", {...})`; Lua does
  `Native.callAsync("AdsBridge", "showRewarded", {"unit"}, function(res, err) ... end)` and
  `Native.on("AdsBridge", "rewarded", function(p) ... end)`.
- **Web dev.** Drops `plugins/native/web/poki.js` calling `JasperNative.register("Poki", {...})`
  and adds the SDK URL to `web.script_urls`.
- **Desktop dev.** A C++ file in `plugins/native/cpp/` with `JASPERENGINE_NATIVE_PLUGIN(Steam)`.
- **AI agent.** `native_plugins {action: "create", name: "Device", platforms: [...], methods:
  [{name: "getCode"}, {name: "login", async: true}]}` scaffolds all files plus a Lua mock, then
  tests the Lua side in editor Play with the mock (`eval_lua "return Native.call('Device','getCode')"`),
  sets dependencies through `set_project_settings`, and exports.

## 4. Research summary

| Engine | Mechanism | Taken / left |
|---|---|---|
| Unity Android: `AndroidJavaClass`, `AndroidJavaObject`, `AndroidJavaProxy` ([docs](https://docs.unity3d.com/Manual/android-plugins-java-code-from-c-sharp.html)); `Plugins/Android` folder, `.aar`/`.jar`/`.java`/`.kt` import, `mainTemplate.gradle`, EDM4U `*Dependencies.xml` ([EDM4U](https://github.com/googlesamples/unity-jar-resolver)) | reflection with object handles; Gradle templates; the plugin importer's per-platform checkboxes | Take: drop-in folder for sources/libs, Gradle dependencies as data (EDM4U's `androidPackage spec` + repositories), manifest merge of library manifests. Leave: object handles (error-prone global refs, threading pitfalls, hard for agents), editing Gradle templates by hand. |
| Unity iOS: `[DllImport("__Internal")]`, `Plugins/iOS` `.m/.mm/.swift`, `UnitySendMessage`, EDM4U `iosPod` ([docs](https://docs.unity3d.com/Manual/ios-native-plugin-call-functions.html)) | C ABI functions; string-based message back to a GameObject | Take: drop-in sources, pods as data, native -> script messages (`emit`). Leave: C ABI marshalling and GameObject-name targeting. |
| Unity Web: `.jslib` with `mergeInto(LibraryManager.library, {...})`, `SendMessage` ([docs](https://docs.unity3d.com/Manual/web-interacting-browser-js-to-unity.html)) | link-time JS library | Take: JS files from a plugin folder. Leave: link-time merge (plain `<script>` tags are simpler and work with any SDK). |
| Godot Android plugin v2 ([docs](https://docs.godotengine.org/en/stable/tutorials/platform/android/android_plugin.html)): `GodotPlugin` subclass, `@UsedByGodot` methods, signals, `onMainCreate`/activity callbacks; iOS plugins (`.gdip`, [docs](https://docs.godotengine.org/en/stable/tutorials/platform/ios/ios_plugin.html)); `JavaScriptBridge.eval/get_interface/create_callback` ([docs](https://docs.godotengine.org/en/stable/classes/class_javascriptbridge.html)); GDExtension | named plugin singleton + methods + signals + lifecycle | Take: plugin name -> methods, events (signals), lifecycle callbacks, JS global object access by path. Leave: export-plugin packaging (AAR + Godot editor plugin). |
| Defold native extensions ([docs](https://defold.com/manuals/extensions/)): `ext.manifest`, `manifests/android/build.gradle`, `AndroidManifest.xml`, `manifests/ios/Info.plist`, `Podfile`, merged at build | per-platform manifest fragments in the extension folder | Take: manifest/proguard fragments living next to the code (`plugins/native/android/AndroidManifest.xml`, `proguard-rules.pro`). |
| Cocos `native.reflection.callStaticMethod(class, method, signature, ...)`, `JsbBridge.sendToNative/onNative` ([docs](https://docs.cocos.com/creator/manual/en/advanced-topics/js-java-bridge.html)) | static-method reflection; string channel | Take: static methods by reflection (no registration needed on Android/iOS); a single string/JSON channel for events. Leave: JNI signature strings (inferred from the arguments instead). |
| Flutter MethodChannel / Capacitor plugins | name + method + JSON args, promise result, events | Take: the async call object with `resolve`/`reject` and error codes. |

## 5. Product decisions

1. **Plugin = a name; method = a static function.** Lua addresses `(plugin, method)`. Android:
   a class (simple name resolved through a generated registry, or a fully qualified name);
   Apple: an Objective-C class (Swift `@objc(Name)`); Web: an object registered with
   `JasperNative.register(name, obj)` or any global path (`"localStorage"`, `"navigator.share"`'s
   parent); C++: a registered plugin. No registration needed on Android/Apple: reflection.
2. **JSON-only boundary.** Arguments and results are JSON values (nil/null, boolean, number,
   string, array, object), converted with script-data's Lua<->JSON rules. Rationale: one rule set
   on four platforms, no leaked native references, trivially testable with mocks, and agents can
   reason about it. Native code that needs objects keeps them on the native side (static fields).
3. **Sync and async.** `Native.call` blocks the game thread and returns `value, errMessage,
   errCode`. `Native.callAsync` returns a `NativeCall` handle and calls back on the main thread
   with `(result, errMessage, errCode)`; the native side completes it from any thread, any time.
4. **Threads.** Sync calls run on the engine thread (Android: the game thread, attached to the JVM;
   Apple: the main thread, which is the engine thread; Web: the only thread). Async calls run on
   `options.thread`: `"main"` (default: Android UI thread via `runOnUiThread`, Apple main queue,
   Web/C++ next tick) or `"background"` (Android/Apple/C++ thread pool; Web same as main).
   Results and events always reach Lua at the `http` delivery point (before `Engine.onUpdate`).
5. **Resolution order** per call: C++ plugin registered under the name -> platform plugin
   (Java/ObjC/JS) -> Lua mock (`Native.setMock` or `<luaDir>/native_mocks/<Plugin>.lua`) ->
   error `not_found`. The same order everywhere (editor, desktop, devices): a mock never shadows
   a real implementation.
6. **Folder convention** `plugins/native/` at the project root (section 6.4), excluded from the
   assets copy.
7. **Dependencies are data** (project settings), not edited templates; plugin-folder manifest and
   proguard fragments are merged by the platform tools (Gradle manifest merger).
8. **Engine-provided native helper API** on each platform (`JasperNative` in Java and ObjC, global
   `JasperNative` in JS, `jasperengine::NativeBridge` in C++) with: activity/view controller/window
   access, thread dispatch, `emit`, lifecycle listeners, `pauseEngine`/`resumeEngine`.
9. **Desktop:** C++ plugins (all desktops, and also mobile/web), plus Objective-C from
   `plugins/native/{apple,macos}` compiled into macOS CMake builds and into the macOS editor's Play
   library. Windows/Linux: C++ only.
10. **Errors as values**, stable codes (6.3). Programming errors (non-string plugin name, args not a
    table in `callAsync`) raise Lua errors.
11. **Owner rule** (shared convention): `callAsync(..., {owner = self})` and `Native.on(..., owner)`
    drop the callback/handler when the owner is destroyed or its scene unloads; the native side
    sees `call.isCancelled() == true`. Without an owner, async calls and handlers survive scene loads
    and end at Play stop/shutdown. No warning for ownerless use (no editor-only behaviour); the docs
    and the scaffold always pass `owner = self` from scene scripts.

## 6. Design

### 6.1 Runtime C++ (`engine/core/native/`)

```cpp
namespace jasperengine {
using NativeJson = nlohmann::json;            // engine/libs/json
struct NativeError { std::string code, message; };
class JASPERENGINE_API NativeCall {           // shared state; copyable handle
public:
    uint64_t id() const; const NativeJson& args() const;
    void resolve(NativeJson value);           // any thread; first resolve/reject wins
    void reject(const std::string& code, const std::string& message);
    bool isCancelled() const; bool isDone() const;
};
class JASPERENGINE_API NativePlugin {
public:
    using Sync = std::function<NativeJson(const NativeJson& args)>;   // throw NativeError to fail
    using Async = std::function<void(NativeCall call)>;
    NativePlugin& method(const std::string& name, Sync fn);
    NativePlugin& asyncMethod(const std::string& name, Async fn);
};
class JASPERENGINE_API NativeBridge {
public:
    static NativePlugin& registerPlugin(const std::string& name);    // replaces an existing one
    static void unregisterPlugin(const std::string& name);
    static void clearCppPlugins();                                    // editor: before the Play library unloads
    static void emit(const std::string& plugin, const std::string& event, NativeJson payload); // any thread
    // the Lua side (also usable from C++ scripts)
    static NativeJson call(const std::string& plugin, const std::string& method, const NativeJson& args, NativeError& err);
    static NativeCall callAsync(const std::string& plugin, const std::string& method, NativeJson args,
                                std::function<void(const NativeJson&, const NativeError*)> done,
                                bool background = false, AsyncOwner owner = {}, double timeout = 0);
    static bool has(const std::string& plugin, const std::string& method = "");
    static void shutdown();                     // Play stop / systemShutdown: cancel calls, drop handlers
};
}
#define JASPERENGINE_NATIVE_PLUGIN(NAME) /* static registrar: void jasperNativePlugin_NAME(NativePlugin& plugin); registered at load, named NAME */
```
Usage: `JASPERENGINE_NATIVE_PLUGIN(Steam) { plugin.method("userName", [](const NativeJson&) { return NativeJson("me"); }); }`.

**Platform layer** `engine/core/native/NativePlatform.h` (internal): one implementation per platform,
compiled like the `http` backends:

```cpp
class NativePlatform {
public:
    virtual bool has(const std::string& plugin, const std::string& method) = 0;
    virtual NativeJson call(const std::string& plugin, const std::string& method, const NativeJson& args, NativeError& err) = 0;
    virtual void callAsync(uint64_t id, const std::string& plugin, const std::string& method, const NativeJson& args, bool background) = 0;
    virtual void cancel(uint64_t id) = 0;   // marks the native NativeCall cancelled
};
NativePlatform* createNativePlatform();   // null on Linux/Windows (C++ plugins and mocks only)
```
Native completions and events enter C++ through `NativeBridge::completeFromPlatform(id, json/err)`
and `NativeBridge::emit`, which post to `MainThreadQueue`. JSON crosses JNI/ObjC/JS as UTF-8 text
(`NativeJson::dump()` / `parse`), parsed on the receiving side; one serialization per hop.

| Platform | Files |
|---|---|
| Android | `engine/platform/android/NativePlatformAndroid.cpp`; Java module `engine/platform/android/java-bridge/com/jaspergameengine/bridge/{JasperNative.java, NativeCall.java, JasperLifecycleListener.java, NativeReflection.java}` |
| iOS/macOS | `engine/platform/apple/NativePlatformApple.mm`, `engine/platform/apple/JasperNative.{h,m}` (public header for plugins) |
| Web | `engine/platform/emscripten/NativePlatformWeb.cpp` (EM_JS) |
| Linux/Windows | none (`createNativePlatform` returns null) |

### 6.2 Lua API (`engine/core/script/binding/NativeClassesLua.cpp`)

| Function | Semantics |
|---|---|
| `Native.call(plugin, method, ...) -> value, errMessage, errCode` | Sync. On success `value` (nil for void/null), `nil, nil`. On failure `nil, message, code`. |
| `Native.callAsync(plugin, method [, args] [, callback] [, options]) -> NativeCall` | `args` array table (nil = no args); `callback(result, errMessage, errCode)`; `options = {thread = "main"|"background", owner = ..., timeout = seconds (0 = none, default 0)}`. |
| `Native.on(plugin, event, handler [, owner]) -> integer` | Subscribe; `handler(payload)`. Several handlers per event run in registration order. |
| `Native.off(id)` | Unsubscribe (unknown id: no-op). |
| `Native.has(plugin [, method]) -> bool` | An implementation (C++, platform or mock) exists. With `method`, that method exists (Android: a public static method of that name; Apple: a selector of 6.5's forms; Web: a function; C++: registered; mock: a function field). |
| `Native.setMock(plugin, table)` / `Native.clearMock(plugin)` | Lua mock: `table[method](...)` is called with the call's arguments; its return value(s) mean `value` or `nil, message[, code]`. For `callAsync` the result is delivered next frame; a mock function may also return `Native.pending()` and complete later with `call:resolve(v)` / `call:reject(code, msg)` (the `NativeCall` is passed as an extra last argument to async mock calls). |
| `Native.emitMock(plugin, event, payload)` | Fires `Native.on` handlers as if native code emitted it (next frame). For tests and mocks; works on every platform. |
| `Native.pending()` | Sentinel for mocks (above). |

`NativeCall` (userdata): `id`, `result`, `errorCode`, `errorMessage` (read-only), `isDone()`
(method: the coroutines awaitable protocol, so `Coroutine.waitFor(call)` works), `cancel()` (callback not called; native side sees `isCancelled()`), and for mocks
`resolve(value)`, `reject(code, message)`.

**Mock auto-load:** on the first call/has/on for a plugin with no C++/platform implementation and no
`setMock`, the engine runs `require("native_mocks." .. plugin)` once (pcall; a missing module is not
an error, other errors are logged); a table result becomes the mock. Mocks live in the Lua dir, so
they ship with builds; they only answer when nothing real exists (decision 5).

### 6.3 Error codes

`not_found` (no plugin), `no_method`, `bad_args` (an argument cannot convert to the native
parameter, or a wrong count), `exception` (native threw; message has class and text, the stack is
logged), `unsupported_type` (native returned a non-JSON value), `invalid_utf8`, `cancelled`,
`timeout`, `async_only` (Web function returned a Promise to `Native.call`), `not_ready` (Android
activity / Apple view controller not created yet), `unavailable` (the platform has no native layer
for this plugin kind; message names the platform: "Java plugins run only on Android").

### 6.4 Project folder `plugins/native/`

```
plugins/native/
  android/            java/**.java, kotlin/**.kt (any *.java/*.kt below android/ also works),
                      libs/*.aar *.jar, res/**, AndroidManifest.xml (library manifest, merged),
                      proguard-rules.pro (appended to the consumer rules)
  apple/              *.m *.mm *.h *.c *.cpp *.swift   (compiled into iOS and macOS)
  ios/                same kinds + *.framework *.xcframework *.a (iOS target only)
  macos/              same kinds (macOS target only)
  web/                *.js (each becomes a <script> before the engine script, in name order)
  cpp/                *.cpp *.h: an always-compiled C++ source root (Play library and every export)
```
- `Project::getScriptDirs()` (`editor/Project.cpp:2474`) returns the user dirs plus
  `plugins/native/cpp` when that folder exists; the Generator already compiles every source of a
  script root (`editor/Generator.cpp:993-1030`). A project whose only C++ is there still builds
  the Play library (check `Project` "has C++ scripts" logic counts script roots).
- `Exporter::copyAssets` (`editor/Exporter.cpp:2047`) skips `plugins/` (relative to the assets dir
  when the assets dir is the project root, the default `"."`, `editor/Project.h:1081`);
  `copyLua` keeps `native_mocks`.
- Project window shows the folder like any other; the Inspector shows a read-only "Native plugin
  source (Android)" header for files there.

### 6.5 Calling conventions per platform

**Android (Java/Kotlin).** Target class: if `plugin` contains a `.`, `Class.forName(plugin)`;
else the generated registry `com.jaspergameengine.nativeplugins.JasperNativeRegistry.CLASSES`
(simple name -> FQCN, generated by the export from every `package` + top-level `class`/`object`
declaration in `plugins/native/android`; duplicates are an export error), else
`JasperNative.registerPlugin(name, Class)` aliases. Method: `public static`, name = `method`,
parameter count = number of Lua args (sync), or Lua args + 1 with a trailing `NativeCall` parameter
(async; the method returns immediately and completes the call later). Kotlin: functions in an
`object` or `companion object` with `@JvmStatic`. Overloads of the same arity: the first (in
`getMethods()` order sorted by parameter type names, for determinism) whose every parameter
converts. Conversions JSON -> Java: `boolean/Boolean` (JSON bool), `int/Integer`, `long/Long`,
`short`, `byte` (JSON integer in range; a non-integral number fails), `float`, `double` and boxed
(any number), `String`/`CharSequence` (string), `JSONObject`, `JSONArray`, `Map<String,Object>`,
`List<Object>`, `Object` (natural mapping: String, Long or Double, Boolean, JSONObject, JSONArray,
null), `String[]`/`int[]`/`double[]` (JSON arrays); JSON null -> any non-primitive. Java -> JSON:
null/void, Boolean, Number (integral types stay integers), String/CharSequence, JSONObject,
JSONArray, `JSONObject.NULL`, Map, Collection, arrays, Enum (`name()`); anything else ->
`unsupported_type`. Reflection lookups cached per (class, method, arity). Implemented in
`NativeReflection.java` (pure Java, unit-testable on the JVM).

**Apple (Objective-C/Swift).** Class `NSClassFromString(plugin)`. Sync selectors tried in order:
`<method>:` (one `NSArray*` argument holding the converted Lua args), then `<method>` when there are
no args. Async: `<method>:call:` (`NSArray*`, `JasperNativeCall*`). Return `id`: `NSNull`/nil, NSNumber
(bool detected with `CFGetTypeID == CFBooleanGetTypeID()`), NSString, NSArray, NSDictionary with
string keys; else `unsupported_type`. Called through `objc_msgSend` casts; `@try/@catch` maps
`NSException` to `exception`. Swift example (documented and scaffolded):
```swift
@objc(Device) public class Device: NSObject {
    @objc public static func getCode(_ args: [Any]) -> Any? { return Bundle.main.infoDictionary?["CFBundleVersion"] }
    @objc public static func login(_ args: [Any], call: JasperNativeCall) { call.resolve(["ok": true]) }
}
```

**Web (JavaScript).** The engine defines `window.JasperNative` before plugin scripts load:
`register(name, object)`, `emit(plugin, event, payload)`, `canvas`, `pauseEngine()`,
`resumeEngine()`. Target: the registered object, else `globalThis[plugin]` resolved by dotted
path (`"navigator.clipboard"`); method `target[method]` called with `this = target`. Sync: the
return value is `JSON.stringify`'d; a returned thenable -> `async_only`. Async: `await
Promise.resolve(fn.apply(target, args))`; a rejection with `{code, message}` keeps them, any other
reason -> `exception` with `String(reason)`. Non-JSON values (functions, DOM nodes) ->
`unsupported_type`.

**C++.** `NativePlugin::method` functions receive `args` (a JSON array) and return JSON; throw
`NativeError{code, message}` to fail; other exceptions -> `exception`.

### 6.6 Native helper APIs (what plugin code uses)

**Java** `com.jaspergameengine.bridge.JasperNative` (in a tiny Gradle library module `jasperbridge`,
source dir `platform/android/java-bridge`, that both the app and the plugin module depend on):
`static Activity getActivity()`, `static void runOnUiThread(Runnable)`, `static void
runOnGameThread(Runnable)` (posts to `MainThreadQueue`: runs in the next frame on the engine thread),
`static void emit(String plugin, String event, Object payload)` (payload converted as returns are),
`static void addLifecycleListener(JasperLifecycleListener)`, `removeLifecycleListener`,
`static void pauseEngine()` / `resumeEngine()` (call `Engine::systemPause/systemResume` on the game
thread, counted: nested pauses need as many resumes; audio paused, `Engine.onPause` fires),
`static void registerPlugin(String name, Class<?> cls)`. `JasperLifecycleListener` (interface, all
default no-ops): `onCreate(Bundle)`, `onStart`, `onResume`, `onPause`, `onStop`, `onDestroy`,
`onActivityResult(int, int, Intent)`, `onRequestPermissionsResult(int, String[], int[])`,
`onNewIntent(Intent)`, `onWindowFocusChanged(boolean)`, `onConfigurationChanged(Configuration)`,
`boolean onBackPressed()` (true = consumed: the game does not get Escape). `MainActivity`
(`engine/platform/android/java/.../MainActivity.java`) forwards each of these to
`JasperNative.dispatch*`. A plugin class may also declare `public static void
onJasperInit(Activity a)`: the bridge calls it once, on the UI thread, after `MainActivity.onCreate`,
for every registry class that has it (SDK initialization).
`NativeCall`: `JSONArray getArgs()`, `resolve(Object)`, `reject(String code, String message)`,
`boolean isCancelled()`, `long getId()`.

**Objective-C** `JasperNative.h` (in `engine/platform/apple/`, on the header path of the app targets):
`+ (UIViewController*)viewController` (topmost presented, the logic of
`AdMobAdapter.rootViewController`, `engine/platform/apple/ios/AdMobAdapter.m:21`), `+ (UIWindow*)window`,
`+ (UIView*)view` (macOS: `NSViewController`/`NSWindow`/`NSView`), `+ (void)emit:(NSString*)plugin
event:(NSString*)event payload:(id)payload`, `+ (void)runOnMainThread:(dispatch_block_t)b`,
`+ (void)runOnGameThread:(dispatch_block_t)b`, `+ (void)pauseEngine`, `+ (void)resumeEngine`,
`+ (void)addLifecycleObserver:(id<JasperLifecycleObserver>)o` / remove. Protocol
`JasperLifecycleObserver` (all `@optional`): `applicationDidBecomeActive`,
`applicationWillResignActive`, `applicationDidEnterBackground`, `applicationWillEnterForeground`,
`applicationWillTerminate`, `- (BOOL)openURL:(NSURL*)url options:(NSDictionary*)o`,
`didRegisterForRemoteNotificationsWithDeviceToken:(NSData*)t`. Forwarded from
`SceneDelegate.m`/`AppDelegate.m` (`engine/platform/apple/ios/`) and the macOS app delegate.
Classes may implement `+ (void)jasperInit`: called once on the main thread after the view loads.
`JasperNativeCall`: `args`, `- (void)resolve:(id)v`, `- (void)reject:(NSString*)code message:(NSString*)m`,
`isCancelled`.

**C++**: `NativeBridge::emit`, `NativeCall`, `MainThreadQueue::post`.

### 6.7 Project settings (`editor/Project.h`, serialized in `editor/Stream.cpp` next to the platform blocks `:2757-2853`, `:3151-3237`)

New struct members (empty by default, omitted from `project.yaml` when empty):

| MCP key | Type | Export effect |
|---|---|---|
| `android.gradle_dependencies` | string[] | each `group:name:version` -> `implementation "<it>"`; any other string is inserted verbatim as a dependencies line (e.g. `implementation platform('...bom:33.0.0')`). In the `nativeplugins` module as `api`. |
| `android.gradle_repositories` | string[] (URLs) | `maven { url "<url>" }` in `allprojects.repositories` (root `build.gradle`). |
| `android.gradle_properties` | string[] (`key=value`) | appended to `gradle.properties` (replacing a key already there). |
| `android.extra_permissions` | string[] (full names) | `<uses-permission android:name="<name>"/>` (e.g. `com.google.android.gms.permission.AD_ID`). |
| `android.manifest_xml` | string | raw XML inserted inside `<manifest>` (before `<application>`). |
| `android.manifest_application_xml` | string | raw XML inserted inside `<application>`. |
| `ios.plist_entries` | object key -> string/bool/number/string[] | merged into `ios/Info.plist` (replacing existing keys). |
| `ios.plist_xml` | string | raw `<key>..</key><value>` pairs appended to the top dict (SKAdNetworkItems). |
| `ios.frameworks` | string[] | system frameworks linked (`StoreKit`, `AppTrackingTransparency`; `Name?` = weak). |
| `ios.cocoapods` | `[{name, version, source?}]` | `workspaces/xcode/Podfile` (6.8). |
| `ios.swift_packages` | `[{url, version, products[]}]` | Swift Package references in the pbxproj (6.8). |
| `ios.entitlements_xml` | string | `ios/JasperEngine.entitlements` + `CODE_SIGN_ENTITLEMENTS`. |
| `ios.deployment_target` | string, `"15.0"` | `IPHONEOS_DEPLOYMENT_TARGET` of every target (min 15.0, validated `^\d+\.\d+$`). |
| `macos.plist_entries`, `macos.frameworks` | as iOS | macOS target / CMake `-framework`. |
| `web.script_urls` | string[] | `<script src="...">` in `<head>` before plugin scripts, in order. |

The XML fragments are validated as well-formed with tinyxml2 when set (MCP: refuse; UI: red
warning); the export fails with a clear message on malformed fragments.

### 6.8 Exported-build path

**Android** (`Exporter::writeAndroidProjectSettings`, `editor/Exporter.cpp:2940`, templates under
`engine/workspaces/androidstudio`):
1. New template module `engine/workspaces/androidstudio/jasperbridge/build.gradle` (`com.android.library`,
   `namespace 'com.jaspergameengine.bridge'`, `java.srcDirs = [jasperengineRootAbs + '/platform/android/java-bridge']`,
   minSdk/compileSdk patched like the app). Always included: `settings.gradle` `include ':jasperbridge'`,
   app `implementation project(':jasperbridge')`. Engine-side Java (`MainActivity`) uses it.
2. When `plugins/native/android` exists or any `android.gradle_*` setting is set: generated module
   `workspaces/androidstudio/nativeplugins/build.gradle` (`com.android.library`, namespace
   `com.jaspergameengine.nativeplugins`, `java.srcDirs` = the copied `project/plugins/native/android`
   tree's `java`, `kotlin` and the folder itself, plus `generated/` holding `JasperNativeRegistry.java`;
   `res.srcDirs` = its `res`; `manifest.srcFile` = its `AndroidManifest.xml` or a generated empty
   `<manifest/>`; `consumerProguardFiles` = generated keep rules (`-keep class <pkg>.** { *; }` for each
   package found, `-keep class com.jaspergameengine.** { *; }`) + the folder's `proguard-rules.pro`;
   dependencies: `api project(':jasperbridge')`, the settings' deps as `api`,
   `compileOnly fileTree(dir: '<libs>', include: ['*.jar','*.aar'])`). The **app** module gets
   `implementation project(':nativeplugins')` and `implementation fileTree(dir: '<libs>', include: ['*.jar','*.aar'])`
   (local AARs must sit in the app module; AGP refuses them inside a library's AAR).
   Kotlin sources rely on AGP 9's built-in Kotlin (template AGP `9.4.1`, `engine/workspaces/androidstudio/build.gradle`);
   the compile check (section 9) proves it.
3. Manifest: `extra_permissions`, the XML fragments; the Gradle manifest merger merges the plugin
   manifest (so `tools:node="remove"` works as in Unity's game, `Assets/Plugins/Android/AndroidManifest.xml`).
4. `onJasperInit` dispatch reads the registry; nothing else to generate.
5. `MainActivity.java`: forwards lifecycle methods (adds `onStart/onStop/onDestroy/onActivityResult/
   onRequestPermissionsResult/onNewIntent/onWindowFocusChanged/onConfigurationChanged` overrides calling
   super then `JasperNative.dispatch...`), the back callback asks `JasperNative.dispatchBackPressed()` first.

**iOS/macOS** (`Exporter::writeAppleProjectSettings`, `addXcodeProjectSources`, `editor/Exporter.cpp:3155-3725`):
1. Copy `plugins/native/{apple,ios,macos}` into the export's `project/plugins/native/...`; add
   sources to `JasperEngine iOS` (apple + ios) and `JasperEngine macOS` (apple + macos) with file
   types `.m` `sourcecode.c.objc`, `.mm` `sourcecode.cpp.objcpp`, `.swift` `sourcecode.swift`, `.c`,
   `.cpp`; headers as file references only (same reasons as `addXcodeProjectSources:3526`). Reuse
   `addObject`/`addPbxListEntries`; ids from paths (deterministic re-export).
2. `.framework`/`.xcframework`/`.a`: file reference + Frameworks build phase; frameworks whose
   binary is not a static archive (first bytes `!<arch>\n`; for `.xcframework` the binary of the
   first `AvailableLibraries` entry of its `Info.plist`) are also embedded (Embed Frameworks phase,
   `CodeSignOnCopy`). `FRAMEWORK_SEARCH_PATHS`/`LIBRARY_SEARCH_PATHS` get the folder.
3. Swift present: `SWIFT_VERSION = 5.0`, `CLANG_ENABLE_MODULES = YES`, generated
   `project/plugins/native/JasperNative-Bridging-Header.h` (`#import "JasperNative.h"` + every plugin
   `.h`) as `SWIFT_OBJC_BRIDGING_HEADER`, `LD_RUNPATH_SEARCH_PATHS` includes `@executable_path/Frameworks`.
4. `JasperNative.{h,m}` and `NativePlatformApple.mm` are part of the template project (add to the
   template `project.pbxproj` app targets by hand; `.mm` engine-side into the engine targets).
5. `ios.frameworks`: `PBXFileReference` `sourceTree = SDKROOT; path = System/Library/Frameworks/X.framework`
   in the Frameworks phase (`settings = {ATTRIBUTES = (Weak, ); }` for `X?`).
6. Plist entries via the plist helpers (`replacePlistStringValue`, `:392`; add bool/number/array/dict
   writers); entitlements file + build setting.
7. CocoaPods: `workspaces/xcode/Podfile` (`platform :ios, '<deployment_target>'`, `source` lines,
   `target 'JasperEngine iOS' do use_frameworks! :linkage => :static; pod 'Name', 'version'; end`).
   The export log and a generated `workspaces/xcode/README-pods.txt` say: run `pod install`, open
   `JasperEngine.xcworkspace`. No Podfile when the list is empty (and an old one is removed).
8. Swift packages: `XCRemoteSwiftPackageReference` (`repositoryURL`, `requirement = {kind =
   upToNextMajorVersion; minimumVersion = <version>;}`), one `XCSwiftPackageProductDependency` per
   product, `packageReferences` on the project object, `packageProductDependencies` on the iOS target,
   a `PBXBuildFile` with `productRef` in its Frameworks phase.
9. `ios.deployment_target` replaces every `IPHONEOS_DEPLOYMENT_TARGET`.

**macOS CMake desktop builds and macOS editor Play** (`editor/Generator.cpp` CMake writer): when
`plugins/native/apple` or `macos` has `.m`/`.mm` files and the host is Apple, `enable_language(OBJC OBJCXX)`,
add them to the target, `-framework` for `macos.frameworks`; `JasperNative.m` comes with the engine.

**Web** (`Exporter::writeExportSettingsScript`, `editor/Exporter.cpp:2736-2830`): copy
`plugins/native/web/*.js` to `<output>/plugins/`; `apply.cmake` inserts, before `</head>` (inside the
existing `extra` string), `<script src="URL"></script>` for each `web.script_urls` entry then
`<script src="plugins/<file>"></script>` per file; the `JasperNative` global is created by
`NativePlatformWeb.cpp` from `Module.preRun`-time code (`EM_JS` init at engine start) **and** a tiny
inline `<script>window.JasperNative = window.JasperNative || {_pending: []};</script>` placed first,
so plugin files that call `register` before the module starts are queued and adopted.

**Generator/editor Play**: `NativeBridge::clearCppPlugins()` is called in
`editor::Conector::disconnect` (`editor/Conector.cpp:212`) **before** `unloadSharedLibrary`, and
`NativeBridge::shutdown()` in `Project::stop` next to `Http::shutdown()` (`editor/Project.cpp:9237`).
Exports: C++ plugins register from static initializers at startup.

### 6.9 Editor UX

- **Project Settings > Native Plugins** (new category in `ProjectSettingsWindow`): top: a summary of
  `plugins/native` (counts per platform, "Open Folder", "Create Plugin..."); sections Android,
  iOS, macOS, Web with the settings of 6.7 as editable lists (one row per entry, + / - buttons),
  multi-line fields for XML fragments with live well-formedness check, a Pods table (name, version,
  source) and a Swift packages table (URL, version, products comma-separated).
- **Create Plugin...** dialog: name (valid identifier), platforms checkboxes (Android, iOS, macOS,
  Web, C++), Android language (Java/Kotlin), Apple language (Objective-C/Swift), method list (name,
  async?) -> writes the files of 6.10 and opens the Lua mock in the code editor.
- **Assets > Create > Native Plugin...** opens the same dialog.
- **Console**: `Native` errors are values; a call to an unavailable platform plugin in editor Play
  logs once per plugin: `Native plugin "Device" has no implementation here (editor): add
  lua/native_mocks/Device.lua or Native.setMock`.

### 6.10 MCP / agent surfaces

New tool **`native_plugins`** (`editor/mcp/tools/ProjectTools.cpp` or a new `NativeTools.cpp`):
- `{action: "list"}` -> `{root, plugins: [{name, platforms: {android: [files], ios: [...], ...},
  methods: [...]}], mocks: [...], settings: {...}}` (method names parsed best-effort from sources).
- `{action: "create", name, platforms: ["android","ios","macos","web","cpp"], android_language:
  "java"|"kotlin", apple_language: "objc"|"swift", package: "com.example.plugins", methods: [{name,
  async: bool}]}` -> writes `plugins/native/android/java/<pkg path>/<Name>.java` (or `.kt`),
  `plugins/native/ios/<Name>.m` (+ `.h`) or `apple/<Name>.swift`, `plugins/native/web/<name>.js`,
  `plugins/native/cpp/<Name>Plugin.cpp`, `lua/native_mocks/<Name>.lua`; refuses to overwrite
  (`overwrite: true` to allow); returns the paths. Undo: file writes follow the existing
  `write_file` rules (not undoable; reported).
- Settings keys of 6.7 through `get/set_project_settings` with `describe` text.
- Agent guide line: "Native code: `Native.call/callAsync/on`; plugins in plugins/native; see
  jasper://docs/native-bridge". Details in `docs/mcp/guide/native-bridge.md`.
- TOOLS.md regenerated; tool budget passes (the tool description stays under ~400 chars).

### 6.11 Docs to update

New `docs/mcp/guide/native-bridge.md` (concepts, conventions per platform with full examples, the
helper APIs, lifecycle, threading, mocks, settings, export notes for Pods/SPM, ads/IAP wrapper
checklist: init on UI thread, pause engine during full-screen ads, banner via
`JasperNative.getActivity().addContentView` / `[JasperNative.viewController.view addSubview:]`
plus `Screen.setExtraInsets` from feature `safe-area`). `lua-api.md` section for `Native`.
`docs/unity-glossary.md`: `AndroidJavaClass.CallStatic` -> `Native.call`; `AndroidJavaProxy`
callbacks -> `NativeCall` / `JasperNative.emit` + `Native.on`; `[DllImport("__Internal")]` ->
Apple plugin class methods; `.jslib` -> `plugins/native/web/*.js`; `UnitySendMessage` /
`SendMessage` -> `JasperNative.emit`; `Assets/Plugins/<Platform>` -> `plugins/native/<platform>`;
EDM4U `*Dependencies.xml` -> `android.gradle_dependencies`, `ios.cocoapods`; `mainTemplate.gradle`
-> settings. README feature list.

## 7. Behaviour rules and edge cases

1. **Main thread only for Lua.** Platform callbacks never touch Lua; they post to `MainThreadQueue`.
2. **Sync re-entrancy.** A sync native call may call `JasperNative.emit` (queued, delivered next
   frame) but cannot call back into Lua synchronously.
3. **Async completion exactly once**; a second `resolve/reject` is ignored with a warning log.
   A call never completed stays pending until `timeout` (if set) or Play stop/shutdown, when native
   sees `isCancelled()` and Lua gets nothing (stop) or `timeout`.
4. **Owner destroyed** -> the call is cancelled (native `isCancelled()` true), no callback; handlers
   of `Native.on` with that owner are removed.
5. **Scene loads** don't affect ownerless calls/handlers; Play stop clears everything (C++ plugins
   from the Play library, mocks set by `setMock`, handlers, pending calls) and the mock module
   cache (`package.loaded["native_mocks.*"] = nil`).
6. **App pause.** Native events emitted while paused are queued and delivered after resume in
   order. `pauseEngine`/`resumeEngine` are reference-counted; Play stop resets the count.
7. **Android startup order.** `MainActivity.onCreate` calls `JasperNative.dispatchInit(this)` right
   after `super.onCreate` (UI thread), which runs every `onJasperInit`. The native engine (and Lua
   `init()`) runs on the game thread created by GameActivity and may start before that finished:
   any bridge call waits on a latch released when `dispatchInit` returns (at most 5 s, then the call
   fails with `not_ready`).
8. **Apple** `+jasperInit` runs from `ViewController.viewDidLoad` before `Renderer` init, so it is
   done before any Lua runs.
9. **JNI**: the game thread is attached (`NativeEngine::getJniEnv`); background-thread JNI calls
   use `ScopedJniEnv` (`JasperEngineAndroid.cpp:33-64`). `jclass`/`jmethodID` for the bridge cached
   as global refs in `NativeEngine::setupJNI` (`NativeEngine.cpp:817`); `FindClass` is never called
   from non-Java threads. Strings go through `newJavaString`/`javaStringToUtf8` (UTF-8 safe).
10. **JSON**: integers keep integer type both ways; numbers outside int64 become doubles; NaN/Inf
    -> `bad_args` (outgoing) or `unsupported_type` (incoming); cyclic Lua tables -> `bad_args`
    (script-data detects); maximum depth 64.
11. **Performance warning**: more than 200 sync calls in one frame logs a one-time warning
    (JNI/JSON overhead).
12. **Web**: everything is single-threaded; `background` is the same as `main`.
13. **R8**: plugin classes are kept by the generated rules; plugin authors using reflection on
    their own classes add rules in their `proguard-rules.pro`.

## 8. Performance and memory

Sync call cost: one JSON dump + parse per direction plus a JNI/`objc_msgSend` call (target < 50 µs on
mid-range Android for small payloads). Reflection metadata cached. No threads created except
the `background` pools (Android executor of 2 threads, Apple global queue, C++ engine
`ThreadPoolManager`).

## 9. Tests

**Unit (gtest `editor/tests/NativeBridgeTest.cpp`)** with C++ plugins and Lua mocks (Linux host):
1. C++ plugin sync method: args array round-trip (nested objects, integers vs floats, null, unicode).
2. `NativeError` thrown -> `nil, message, code`; other exception -> `exception`.
3. Async: resolve from a worker thread -> callback at the delivery point next frame, exactly once;
   double resolve ignored; reject codes kept; `timeout` option.
4. Cancel and owner destruction: no callback; `isCancelled()` true on the C++ side.
5. `emit` from a worker thread -> `Native.on` handlers in order; `Native.off`; owner removal.
6. Resolution order: C++ beats mock; mock used when nothing else; `Native.has`.
7. Mock auto-load from `native_mocks/X.lua`; `Native.pending()` + later `call:resolve`.
8. `Native.call` on Linux for an unknown plugin -> `not_found`; `Native.emitMock`.
9. `clearCppPlugins` + `shutdown` leave no handlers, no pending calls, mocks cache cleared.
10. No off-thread Lua use (`LuaBinding::getOffThreadUseCount() == 0`).

**JVM unit test** (`engine/platform/android/java-bridge/test/NativeReflectionTest.java`, run by
`./gradlew :jasperbridge:testDebugUnitTest` in the engine workspace): overload selection, every
conversion of 6.5, Kotlin-style `@JvmStatic` (compiled Java equivalent), unsupported types.

**Export tests (`tools/mcp/tests/test_native_bridge.py`, source export to a temp dir)**: a project
with `plugins/native/android/java/com/t/Device.java`, a `.kt`, `libs/x.jar`, a plugin
`AndroidManifest.xml` with `tools:node="remove"`, settings deps/repos/permissions/XML; assert
`settings.gradle` includes both modules, module gradle content, registry entries, keep rules,
manifest fragments; Apple: `.m`, `.swift`, an `.xcframework` stub, frameworks, pods, SPM: parse
with `tools/mcp/pbxproj.py`, check the target sources/frameworks/package objects and build
settings, `Podfile` content, Info.plist keys; Web: `apply.cmake` script tags and copied files;
re-export is byte-identical (deterministic ids). `native_plugins create/list` round-trip and
refuse-overwrite. Play test: Lua calls a C++ plugin from `plugins/native/cpp` and a mock;
`eval_lua` asserts values; stop/start twice (library reload re-registers, no duplicates).

**Platform compile checks**: export the test project (Android) and run
`./gradlew assembleDebug` in the exported `workspaces/androidstudio` (proves module wiring, Kotlin
built-in support, R8 config with `assembleRelease` too if time allows); the engine workspace
`assembleDebug`; Web build of the exported project with a plugin JS (emsdk). Apple: review
checklist (selector forms, `objc_msgSend` casts with correct signatures, ARC bridging, Swift
bridging header path).

## 10. Acceptance criteria

- [ ] `Native.call/callAsync/on/off/has/setMock/clearMock/emitMock/pending` and `NativeCall` exactly
      as 6.2, stubs clean.
- [ ] C++ plugins + mocks work in editor Play and Linux desktop builds; all unit tests pass.
- [ ] Android: Java and Kotlin static methods callable sync and async, events, lifecycle listener,
      `onJasperInit`, UI-thread default for async; exported project builds with Gradle.
- [ ] iOS/macOS: ObjC and Swift class methods per 6.5; frameworks, pods, SPM, plist, entitlements,
      deployment target written; reviewed.
- [ ] Web: registered and global-path calls, Promises, events, script includes.
- [ ] Project Settings > Native Plugins UI, Create Plugin dialog, `native_plugins` MCP tool,
      settings keys; docs and glossary; TOOLS.md regenerated, budget passes.

## 11. Integration points

| File | Change |
|---|---|
| `engine/core/native/*` | new (NativeBridge, NativePlatform). Add to `engine/CMakeLists.txt` `JASPERENGINE_SRCS`, run `tools/xcode_sync_engine.py`. |
| `engine/core/script/binding/NativeClassesLua.cpp`, `LuaBinding.cpp:635-651` | register `Native`. |
| `engine/platform/android/NativeEngine.{h,cpp}` (`setupJNI:817`) | bridge class/method refs, `RegisterNatives` for `nativeComplete(long, String json, String code, String msg)`, `nativeEmit(String plugin, String event, String json)`, `nativePauseEngine()`, `nativeResumeEngine()`, `nativePostToGameThread(long token)`. |
| `engine/platform/android/java/.../MainActivity.java` | lifecycle forwarding, back dispatch. |
| `engine/platform/android/java-bridge/**` | new module sources. |
| `engine/workspaces/androidstudio/{settings.gradle, app/build.gradle, jasperbridge/build.gradle}` | module wiring (template), Exporter markers. |
| `engine/platform/apple/{JasperNative.h,.m, NativePlatformApple.mm}`, `ios/{AppDelegate.m, SceneDelegate.m, ViewController.m}` | helper API, lifecycle forwarding, `jasperInit`. |
| `engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj` | new template files in targets. |
| `engine/platform/emscripten/NativePlatformWeb.cpp`, `engine/CMakeLists.txt` (Web) | JS glue. |
| `editor/Project.{h,cpp}`, `editor/Stream.cpp` | settings, `getScriptDirs` adds `plugins/native/cpp`. |
| `editor/Exporter.{h,cpp}` | 6.8 (Android modules, Apple sources/frameworks/pods/SPM/plist, Web scripts), `copyAssets` skip. |
| `editor/Generator.cpp` | macOS ObjC sources in CMake. |
| `editor/Conector.cpp:212` | `NativeBridge::clearCppPlugins()` before unload. |
| `editor/Project.cpp:9237` | `NativeBridge::shutdown()` at Play stop. |
| `editor/window/dialog/ProjectSettingsWindow.cpp` + new `NativePluginDialog.{h,cpp}` | UI. |
| `editor/mcp/tools/` | `native_plugins` tool, settings keys. |

Reuse: `MainThreadQueue`, `AsyncOwners` (http spec 6.1), `LuaJson::toJson/push` (script-data),
`ScopedJniEnv`/`newJavaString` (`JasperEngineAndroid.cpp`), pbx helpers (`Exporter.cpp:501-640`),
plist helpers (`:392-465`), `createOptionalWrapper` pattern (`MainActivity.java:52-59`).

## 12. Cross-feature dependencies

- **script-data** (required): `LuaJson::toJson` / `LuaJson::push` (`engine/core/script/LuaJson.h`,
  script-data spec 6.1: `Json.null` sentinel, array/object rules and markers, cycle detection, integer
  vs float). Native uses exactly those functions for arguments, results and event payloads.
- **http** (required): `MainThreadQueue`, `AsyncOwners`, the delivery point.
- **platform-services**: implements `System.getAppBuild`, `openURL`, locale... natively (not via
  this bridge) but reuses `JasperNative` lifecycle forwarding in `MainActivity` (focus, config
  changes); both edit `MainActivity.java`: coordinate.
- **safe-area**: `Screen.setExtraInsets` lets a banner SDK wrapper reserve space.
- **coroutines**: may make `NativeCall` awaitable (same pattern as `HttpRequest`).
- **debug-overlay**: may show pending native calls (optional).

## Implementation notes

Decisions taken while implementing (branch `gaps/native-bridge`), where the spec was silent,
ambiguous or in conflict with the code:

1. **Mocks behind C++.** The resolution order is implemented in `NativeBridge` itself, with the Lua
   layer installed as a `NativeMockProvider` (`core/native/NativePlatform.h`): C++ scripts calling
   `NativeBridge::call` get the same order (and the mocks) as Lua.
2. **A C++ plugin missing the method stops resolution** with `no_method` (it does not fall through
   to the platform plugin of the same name): one name, one implementation. Because C++ answers
   first everywhere, the scaffold wraps a C++ plugin created together with platform plugins in
   `#if !(defined(__ANDROID__) || ...)` for the chosen platforms, so it is the desktop answer.
3. **Async call to a sync method** (C++, Java, Objective-C, mock) runs the method on the requested
   thread and resolves with its return value; a sync call to an async-only method is `async_only`
   (the code the spec gave for JavaScript Promises).
4. **Mock async timing.** An async mock function runs inside `callAsync` (its result is still
   delivered next frame); a mock that returns `nil, message` without a code fails with `exception`.
   The mock module is required only when `native_mocks/<Plugin>.lua` exists (checked with the
   loader's paths), so a missing mock is never an error.
5. **Pause.** The engine now has two pause holds: the OS's (`Engine::systemPause/systemResume`,
   idempotent) and the native one (`Engine::setNativePause`, applied each frame by
   `NativeBridge::update()` from the `pauseEngine`/`resumeEngine` count, paused frames included).
   The engine is paused while either holds: one `onPause` when the first starts, one `onResume`
   when the last ends (no onResume while an ad still shows after the app came back). The OS hold
   keeps platform-services' focus order (focus lost before the pause; gaining focus while the OS
   holds resumes first, then `onFocus(true)`, even while the native hold keeps the engine paused).
   Play stop drops both holds (`resetApplicationState`). `NativeBridge::postToGameThread` (JasperNative `runOnGameThread`)
   tasks run in `update()` too, so a `resumeEngine` from one works while paused; Play stop drops
   queued tasks, and events emitted before a Play stop never reach the next Play (generation).
   On Android the natives are registered in `JNI_OnLoad` (MainActivity's `System.loadLibrary`), so
   `onJasperInit` may already emit and pause.
6. **Timeouts** are checked in `NativeBridge::update()` (real time) and delivered at the delivery
   point; the native side sees the call cancelled. A user `cancel()` after the timeout fired but
   before its delivery still suppresses the callback.
7. **`NativeCall.isDone()`** in Lua means "the callback ran or the call was cancelled" (so
   `Coroutine.waitFor` resumes with `result`/`errorCode` final); C++ `NativeCall::isDone()` is the
   native view (completed or cannot complete) and `isDelivered()` the caller's. A cancelled call's
   `errorCode` reads `"cancelled"` (no callback runs). `NativeCall` also exposes `plugin`/`method`.
8. **Unavailable plugins.** `NativeBridge::setPlatformOnlyPlugins` is filled from `plugins/native`
   by the editor at Play start and by the generated `main.cpp` per platform (`#if` blocks), so editor
   Play and every build say "Java/Kotlin plugins run only on Android" instead of `not_found`.
9. **Android layout.** The bridge module's sources are in `java-bridge/java/com/...` and its JVM test
   in `java-bridge/test/com/...` (not directly under `java-bridge/`): one directory cannot be both a
   main and a test source root. The module has its own `AndroidManifest.xml` and consumer keep rules.
   The `nativeplugins` module uses the whole `plugins/native/android` folder as its Java and Kotlin
   source root (any layout works; listing `java/` and `kotlin/` too would compile files twice).
   Java registry classes must be `public` (reflection cannot call a package-private class);
   Kotlin `internal`/`private` ones are left out (a member's `val`/`var`/`fun` ends its modifiers). `JasperLifecycleListener.onCreate` is called right
   after the plugins' `onJasperInit`, so listeners added there get it. `JasperNative.call` parses the
   arguments with its own JSON reader (integers as Long, others as Double: org.json's number rules
   differ between Android and the JVM) and writes results with its own writer (`7.0` stays a float).
10. **Apple plugins.** A plugin class adopts the `JasperNativePlugin` protocol (the marker:
    no other class of the app or system answers, so a mock still answers for a name like
    `NSString`). `+jasperInit` discovery needs no export step: `[JasperNative dispatchInit]` scans
    the app's own images (`objc_copyImageNames` / `objc_copyClassNamesForImage`, system images
    skipped) for plugin classes with their own `jasperInit` in the metaclass method list
    (`class_copyMethodList`: no class gets `+initialize` for it), once per class. It runs from
    `ViewController.viewDidLoad` (Xcode apps) and from `NativeBridge::initPlugins()` at
    `Engine::systemInit` and every editor Play start (macOS CMake builds, the macOS editor),
    picking up classes loaded since. macOS never unloads Objective-C classes: Play warns once that
    a changed Objective-C plugin needs an editor restart. Lifecycle callbacks come from the app's
    notifications; `openURL` (SceneDelegate, macOS AppDelegate) and the remote-notification token
    (AppDelegate) are forwarded by the delegates. Values cross as Foundation objects converted
    directly from/to JSON; scalar return types are read with matching `objc_msgSend` casts, and a
    method whose parameters are not objects fails with `bad_args` instead of being called.
11. **Xcode project.** Swift bridging headers are per target (`JasperNative-Bridging-Header-iOS.h` /
    `-macOS.h`). Plugin headers are no file references (a project's headers are mapped by name for
    every target: a plugin `Camera.h` would shadow the engine's): their folders are appended to
    `HEADER_SEARCH_PATHS`. The template's app targets have `$(SRCROOT)/../../platform/apple` (for
    `JasperNative.h`, which their own sources import) and `$(inherited)` first in
    `HEADER_SEARCH_PATHS` / `OTHER_LDFLAGS`, so CocoaPods' xcconfig is not overridden. Swift package
    versions are padded to x.y.z. macOS entitlements are not a setting (the spec lists iOS only).
    Plugin folders are copied without following links: links inside `plugins/native` (a
    framework's `Versions/Current`) stay links, others are left out with a warning. The CMake glob
    of an exported project skips `project/plugins/native/` (the platform folders' sources belong to
    Gradle/Xcode). A macOS CMake build defines `JASPERENGINE_MACOS_CMAKE`, so its "unavailable" list
    leaves out Swift-only Apple plugins. Every Apple export links the engine's own system frameworks
    explicitly (iOS: AVFoundation, AudioToolbox, CoreGraphics, CoreMedia, CoreVideo, Foundation,
    GameController, Metal, MetalKit, QuartzCore, UIKit; macOS: the same with AppKit and CoreAudio
    instead of UIKit), appended to the template's Frameworks phase (what it links already, such as main's
    APP_FRAMEWORKS, is kept and not repeated), merged with `ios.frameworks`/`macos.frameworks`
    (one entry per name, strong wins): the engine's C++ and Objective-C++ files import no clang
    modules, so nothing autolinks them, and an iOS export without AdMob linked AVFoundation (for
    miniaudio's AVAudioSession) only through the AdMob SDK's module. Swift packages are
    `upToNextMajorVersion` from the given version (1.1.0 resolves the newest 1.x).
12. **CocoaPods.** When a pod has a custom `source`, the CocoaPods CDN is listed too (a `source` line
    hides the trunk otherwise). Swift packages need a version and at least one product.
13. **Web.** The page stub defines `register` and `emit` (queueing them), not only `_pending`, since
    plugin scripts call them before the engine starts; it is always the first thing in `<head>`
    (before `web.head_include`). A bare plugin name is a registered object only; browser globals
    need a dotted path (`window.localStorage`, `navigator.clipboard`), so a global named like a
    plugin (`Notification`, `Audio`) never hides its mock. Async JavaScript methods get exactly the Lua
    arguments (there is no call object on the Web); top-level `undefined` is null, object members
    that are `undefined` or functions are left out (as `JSON.stringify`), anything else non-JSON is
    `unsupported_type`. Web plugin files are copied by the build's `apply.cmake` into
    `<output>/plugins/`.
14. **XML fragments** are checked by `NativePlugins::xmlFragmentError` (tinyxml2, the fragment wrapped
    in one element, so `android:`/`tools:` prefixes need no declaration); the Android manifest gets
    `xmlns:tools` when a fragment uses `tools:`.
15. **Plist values in project.yaml** are plain YAML scalars/lists; a string that would read back as
    another type (`"true"`, `"15"`, `""`) is written `{string: ...}`.
16. **Project Settings > Native Plugins** follows the window's existing OK/Cancel model (edits apply
    on OK, saved to project.yaml) like every other page: no command-history undo, as the rest of the
    window.
17. **Play library.** `Project::getBuildScriptDirs()` (user dirs + `plugins/native/cpp`) feeds the
    Generator and the Exporter; `getScriptDirs()` stays the setting. `libs/json` joined the engine
    API include directories (`NativeBridge.h` is in `JasperEngine.h`).
18. **Agent guide.** The HTTP and native lines share one bullet, and a few Lua-section lines were
    reworded shorter, to keep the merged guide under 16 KB (16376 bytes after the last main merge).

### Verification (review fixes; merged with gaps/http and main at 2e916ebb9)

- Build box 2201 (`-j5`; 2203 was overloaded): editor and `jasperengine-editor-tests` build with 0
  errors and 0 warnings (again after the `callId` rename, 956ee4b8e).
- Unit suite: 1705 passed, 10 skipped, 1 failed: `CameraShakeComponents.RegistrationAndCatalog`
  expects CameraFollowComponent last, but main's ui-scaling-text appends TextEffectComponent after
  it (main's Catalog.h and test, unchanged here). The `NativeBridgeTest` and platform-services
  cases pass, also after the rename.
- MCP e2e, full suite (`-j3`): 930 passed, 10 failed (the baseline test_animation_assets x5,
  test_lattice_scene x5), 21 skipped; test_native_bridge 4/4. Tool budget: 154 tools, 113215
  (TOTAL_LIMIT set to it). Agent guide 15365 bytes (one line under "Feature guides").
- Apple: the lead's Mac compile check of the engine's Xcode project (956ee4b8e) builds JasperEngine
  macOS and iOS with 0 errors (it found `uint64_t id` parameters hiding Objective-C's `id` in
  NativePlatformApple.mm: renamed). Exported test projects (an Objective-C and a Swift plugin in
  apple/, an iOS-only Swift plugin importing swift-collections through ios.swift_packages, plist
  entries and system frameworks per platform, a Lua caller) were handed over for an xcodebuild run.
- Android, on the host: the exported test project of `test_source_export` (a Java and a Kotlin
  plugin, libs/x.jar, a plugin manifest with tools:node="remove", Gradle dependencies, a repository,
  manifest fragments) **built with `./gradlew assembleDebug`** (AGP 9, arm64 only, 0 warnings): the
  APK (merged tree, 6m 26s) has com.t.Device, com.t.k.Store, the registry, JasperNative, JasperPlatform, AD_ID and the
  application meta-data, and no READ_PHONE_STATE. Earlier rounds: `:jasperbridge:testDebugUnitTest`
  (7 NativeReflectionTest cases), NativePlatformAndroid.cpp with the NDK clang -Wall -Wextra clean,
  a copy of the generated module layout through `assembleRelease` (R8 kept the plugin classes).
- Web: `tools/native-bridge/web-smoke/run.sh` (em++ + node, -Wall -Wextra clean): 29 checks pass
  (registered objects and dotted global paths, bare browser globals not plugins, value types,
  Promises, rejections with codes, exceptions, unsupported values, cancellation, events and
  registrations queued before the engine, pause/resume). A full Emscripten build of an exported
  project was not run.
- Apple runtime and the macOS CMake path: not run here; written against the iOS 15 /
  macOS 12 SDK APIs and reviewed (the `JasperNativePlugin` marker, metaclass method lists for
  `+jasperInit`, typed objc_msgSend casts per return type, object-only parameters, ARC, NSNumber
  booleans, @try/@catch, bridging header paths, `$(inherited)` and platform/apple in the app
  targets, pbxproj objects parsed by tools/mcp/pbxproj.py in the export test). Windows: no platform
  code (C++ plugins and mocks only).
