# Native code: the `Native` bridge and `plugins/native`

Lua calls native code of the platform the game runs on by a **plugin name** and a **method
name**, with JSON values as arguments and results:

```lua
local code, err, errCode = Native.call("Device", "getCode")          -- sync
Native.callAsync("Ads", "showRewarded", {"unit-1"}, function(result, err, code)
    if err then Log.warn("ad failed: " .. err .. " (" .. code .. ")") return end
    Log.print("reward: " .. tostring(result.amount))
end, {owner = self})                                                  -- async
local id = Native.on("Ads", "paid", function(payload) Log.print(payload.value) end, self)
```

A plugin is, in this order (the first that exists answers, on every platform, editor included):

1. a **C++ plugin** registered with `JASPERENGINE_NATIVE_PLUGIN(Name)` (any platform: the desktop answer);
2. a **platform plugin**: a Java/Kotlin class (Android), an Objective-C/Swift class (iOS, macOS),
   a JavaScript object (Web);
3. a **Lua mock**: `Native.setMock(name, table)` or `<Lua folder>/native_mocks/<Name>.lua`.

Otherwise the call fails with `not_found`, or `unavailable` when the plugin exists in
`plugins/native` for other platforms only ("Java/Kotlin plugins run only on Android"; logged once
per plugin). A mock never shadows a real plugin, so the same scripts run in editor Play (mocks
and C++), desktop builds and devices.

The fast way to make one: `native_plugins {action: "create", name: "Device", platforms: ["android",
"ios", "web", "cpp"], methods: [{name: "getCode"}, {name: "login", async: true}]}` (or
**Assets > Create > Native Plugin...**) writes every platform's file and the Lua mock. Test the Lua
side in Play with the mock (`eval_lua "return Native.call('Device', 'getCode')"`), then export.

## Contents

1. The Lua API
2. Values, errors and threads
3. Lua mocks
4. Android (Java, Kotlin)
5. iOS and macOS (Objective-C, Swift)
6. Web (JavaScript)
7. C++ plugins
8. The `plugins/native` folder
9. Project settings (SDK dependencies)
10. Lifecycle, pausing, SDK wrappers (ads, purchases)
11. Unity and Godot equivalents

## 1. The Lua API

| Function | Semantics |
|---|---|
| `Native.call(plugin, method, ...) -> value, errMessage, errCode` | Sync (blocks the game thread). Success: `value` (nil for void/null), `nil, nil`. Failure: `nil, message, code`. |
| `Native.callAsync(plugin, method [, args] [, callback] [, options]) -> NativeCall` | `args`: an array table (nil = none; `Json.null` for a nil inside). `callback(result, errMessage, errCode)` runs once, next frame or later. `options = {thread = "main"\|"background", owner = self, timeout = seconds}`. |
| `Native.on(plugin, event, handler [, owner]) -> id` | `handler(payload)` for every native event; several handlers run in registration order. |
| `Native.off(id)` | Unsubscribes (an unknown id does nothing). |
| `Native.has(plugin [, method]) -> bool` | An implementation exists here; with `method`, that method exists. |
| `Native.setMock(plugin, table)`, `Native.clearMock(plugin)` | A Lua mock (section 3). |
| `Native.emitMock(plugin, event, payload)` | Fires the `Native.on` handlers next frame, as if native code emitted it (tests, mocks). |
| `Native.pending()` | What an async mock returns to complete its call later. |

`NativeCall` (keeping it is optional): `id`, `plugin`, `method`, `result` (after the callback; nil
on failure), `errorCode`, `errorMessage` (nil until it fails; `"cancelled"` after `cancel`),
`isDone()` (the callback ran or the call was cancelled: `Coroutine.waitFor(call)` works),
`cancel()` (the callback will not run; native code sees `isCancelled`), and for mocks
`resolve(value)` / `reject(code, message)`.

Programming errors raise Lua errors (a non-string name, `args` that is not an array table, a bad
option); everything else is a value.

**Owners.** `callAsync(..., {owner = self})` and `Native.on(..., self)` end with the script
instance, its GameObject or the Scene: no callback, and native code sees the call cancelled.
Without an owner, calls and handlers survive scene loads and end when Play stops (or the game
exits). Pass `owner = self` from per-scene scripts.

## 2. Values, errors and threads

Arguments, results and event payloads are JSON values with the `Json` module's rules (lua-api.md,
Json): tables with keys 1..n are arrays, other tables objects, integers stay integers (`7`) and
floats floats (`7.0`), `Json.null` is null inside a value, a null result or payload is `nil`.
NaN, infinities, functions, userdata, cycles and nesting deeper than 64 are `bad_args` going out
and `unsupported_type` coming back. Strings are UTF-8 (`invalid_utf8` otherwise); pass binary data
as base64 text.

| Code | Meaning |
|---|---|
| `not_found` | no C++ plugin, platform plugin or mock of that name |
| `no_method` | the plugin has no such method |
| `bad_args` | an argument cannot convert to the parameter (or the count is wrong) |
| `exception` | native code threw: the message has the class and text (the stack is logged) |
| `unsupported_type` | native code returned something JSON cannot hold |
| `invalid_utf8` | a string argument is not valid UTF-8 |
| `cancelled` | `call:cancel()`, the owner went, Play stopped (the handle shows it; no callback runs) |
| `timeout` | `options.timeout` seconds passed (real time); native code sees the call cancelled |
| `async_only` | `Native.call` of an asynchronous method or of a JavaScript function returning a Promise |
| `not_ready` | Android: the activity did not finish starting within 5 s |
| `unavailable` | the plugin exists for other platforms only |

**Threads.** Lua runs on the main thread only. `Native.call` runs the method on the engine
thread (Android: the game thread, attached to the JVM; Apple and Web: the main thread).
`Native.callAsync` runs it on `options.thread`: `"main"` (default: the Android UI thread, the
Apple main queue, the next frame on Web and for C++) or `"background"` (Android: two worker
threads; Apple: a global queue; C++: the engine's thread pool; Web: same as main). Native code
completes the call from any thread at any time, exactly once (a second completion is ignored with
a warning). Results and events always reach Lua on the main thread at the delivery point of
`Http`, right before `Engine.onUpdate`; never inside the `callAsync` call. While the engine is
paused (app in the background, `pauseEngine`), events and results wait and arrive after the
resume, in order. A sync method may emit events (delivered next frame) but cannot call back into
Lua. More than 200 `Native.call` in one frame logs a warning (each one converts JSON and, on
Android, crosses JNI).

An async call to a plain (sync) method works too: its return value resolves the call, on the
thread asked for.

## 3. Lua mocks

A mock answers where no real implementation exists: editor Play, Linux/Windows builds, and any
platform that lacks the plugin. Put it in `<Lua folder>/native_mocks/<Plugin>.lua` (loaded with
`require` the first time the plugin is called, checked with `Native.has` or subscribed to with
`Native.on`; a missing file is no error) or set one with `Native.setMock`:

```lua
-- lua/native_mocks/Device.lua
local Device = {}
function Device.getCode() return 42 end                       -- Native.call -> 42
function Device.fail() return nil, "no device", "not_ready" end   -- nil, message [, code]
-- async: the Lua arguments first, the NativeCall last
function Device.login(user, call)
    Device.pendingLogin = call      -- complete it any time later: call:resolve({token = "test-" .. user})
    return Native.pending()
end
function Device.quick(call) return {ok = true} end            -- a plain return: delivered next frame
return Device
```

A mock's value goes through JSON like a real result, so it behaves the same. A Lua error in a mock
is `exception`. `Native.emitMock("Ads", "paid", {value = 1})` fakes an event. Mocks ship with
builds (they are Lua files) but never answer where a real plugin exists. Play stop drops
`Native.setMock` mocks and the loaded mock modules (they load again at the next Play).

## 4. Android (Java, Kotlin)

Put sources anywhere below `plugins/native/android` (`java/`, `kotlin/` by convention). A plugin is
a class with **public static** methods; Lua addresses it by its simple name (the export generates
a registry from every `package` + top-level class/object; two classes with one simple name are an
export error) or by its full name (`"com.me.Device"`), or by an alias
(`JasperNative.registerPlugin("Name", Cls.class)`).

```java
package com.me.plugins;

import android.app.Activity;
import com.jaspergameengine.bridge.JasperNative;
import com.jaspergameengine.bridge.NativeCall;
import org.json.JSONObject;

public class Device {
    // optional: once, on the UI thread, right after the activity's onCreate (SDK init)
    public static void onJasperInit(Activity activity) { }

    public static int getCode() { return BuildConfig.VERSION_CODE; }   // Native.call("Device", "getCode")
    public static String greet(String name, int times) { return "hi " + name; }   // Native.call("Device", "greet", "Ann", 2)

    // async: the Lua arguments plus a trailing NativeCall; complete it later, from any thread
    public static void login(String user, final NativeCall call) {
        JasperNative.runOnUiThread(() -> {
            try {
                JSONObject result = new JSONObject().put("token", "abc");
                call.resolve(result);
            } catch (Exception e) {
                call.reject("exception", e.toString());
            }
        });
    }
}
```

Kotlin: functions of an `object` (or a `companion object`) marked `@JvmStatic`:

```kotlin
package com.me.plugins
import com.jaspergameengine.bridge.NativeCall
object Store {
    @JvmStatic fun price(sku: String): Double = 0.99
    @JvmStatic fun buy(sku: String, call: NativeCall) { call.resolve(mapOf("ok" to true)) }
}
```

**Method choice.** The name, then the parameter count (sync: one per Lua argument; async: plus the
trailing `NativeCall`). Overloads of one count: the first, in the order of their parameter type
names, whose every parameter converts. **Conversions** JSON -> Java: `boolean/Boolean` (a
boolean), `int/long/short/byte` and boxes (an integral number in range), `float/double` and boxes
(any number), `String/CharSequence`, `JSONObject`, `JSONArray`, `Map<String,Object>`,
`List<Object>`, `Object` (String, Long or Double, Boolean, JSONObject, JSONArray, null),
`String[] int[] long[] double[] boolean[]`; null fits any non-primitive. Java -> JSON: null/void,
Boolean, Number (integral types stay integers), String/CharSequence/Character, JSONObject,
JSONArray, `JSONObject.NULL`, Map, Collection, arrays, an Enum (its name); anything else is
`unsupported_type`. Lookups are cached per (class, method, arity).

**`JasperNative`** (`com.jaspergameengine.bridge`, the `jasperbridge` Gradle module):
`getActivity()`, `runOnUiThread(Runnable)`, `runOnGameThread(Runnable)` (the start of the next frame, on
the engine thread, paused frames too), `emit(plugin, event, payload)` (payload converted as a return value), `pauseEngine()` /
`resumeEngine()` (counted), `addLifecycleListener` / `removeLifecycleListener`,
`registerPlugin(name, class)`. **`NativeCall`**: `getArgs()` (JSONArray), `resolve(value)`,
`reject(code, message)`, `isCancelled()`, `getId()`. **`JasperLifecycleListener`** (default
methods): `onCreate(Bundle)` (right after the plugins' `onJasperInit`), `onStart`, `onResume`,
`onPause`, `onStop`, `onDestroy`, `onActivityResult`, `onRequestPermissionsResult`, `onNewIntent`,
`onWindowFocusChanged`, `onConfigurationChanged`, `boolean onBackPressed()` (true: consumed, the
game gets no Escape).

**Startup.** The engine (and Lua's `init`) runs on the game thread and may start before
`MainActivity.onCreate` finished: a bridge call waits for it (at most 5 s, then `not_ready`).

**Files of the folder.** `libs/*.aar *.jar` (linked into the app), `res/**` (Android resources),
`AndroidManifest.xml` (a library manifest merged by Gradle: `tools:node="remove"` works, no
`package` attribute), `proguard-rules.pro` (added to the keep rules; plugin classes are already
kept: add rules only for your own reflection).

## 5. iOS and macOS (Objective-C, Swift)

`plugins/native/apple/` goes into both apps, `ios/` into iOS only (also `.framework`,
`.xcframework`, `.a` SDKs), `macos/` into macOS only. A plugin is a class adopting the
`JasperNativePlugin` protocol (Swift: `@objc(Name)` and `JasperNativePlugin`) whose **class
methods** Lua calls; no other class answers a `Native.call` (so a mock still answers for a name
like `NSString`):

```objc
// plugins/native/ios/Device.m (+ Device.h: @interface Device : NSObject <JasperNativePlugin>)
#import "Device.h"
@implementation Device
+ (void)jasperInit { }                                   // optional: once, on the main thread, when the game view loads
+ (id)getCode:(NSArray*)args { return @42; }             // Native.call("Device", "getCode", ...): args are the Lua arguments
+ (id)version { return NSBundle.mainBundle.infoDictionary[@"CFBundleVersion"]; }   // no arguments: the bare form works too
+ (void)login:(NSArray*)args call:(JasperNativeCall*)call {   // Native.callAsync
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        [call resolve:@{@"token": @"abc"}];              // or [call reject:@"user_cancelled" message:@"closed"]
    });
}
@end
```

```swift
// plugins/native/apple/Store.swift
@objc(Store) public class Store: NSObject, JasperNativePlugin {
    @objc public static func price(_ args: [Any]) -> Any? { return 0.99 }
    @objc public static func buy(_ args: [Any], call: JasperNativeCall) { call.resolve(["ok": true]) }
}
```

Selectors tried: `method:` (one `NSArray*` of the arguments), then `method` with no arguments;
async `method:call:`. Values: `NSNull`/nil, `NSNumber` (`@YES`/`@NO` are booleans), `NSString`,
`NSArray`, `NSDictionary` with string keys; scalar returns (BOOL, NSInteger, double) work too.
An `NSException` is `exception`.

**`JasperNative.h`**: `JasperNative.viewController` (the topmost presented one: present SDK
screens on it), `.window`, `.view` (the game's view; macOS: NSViewController/NSWindow/NSView),
`emit:event:payload:`, `runOnMainThread:`, `runOnGameThread:`, `pauseEngine` / `resumeEngine`,
`addLifecycleObserver:` / `removeLifecycleObserver:` with the protocol `JasperLifecycleObserver`
(all optional: `applicationDidBecomeActive`, `applicationWillResignActive`,
`applicationDidEnterBackground`, `applicationWillEnterForeground`, `applicationWillTerminate`,
`openURL:options:`, `didRegisterForRemoteNotificationsWithDeviceToken:`). `JasperNativeCall`:
`args`, `resolve:`, `reject:message:`, `isCancelled`, `callId`. Swift sees all of it through the
export's bridging header (with every plugin `.h`).

`+jasperInit` runs once for every plugin class of the app's own images that implements it,
before the engine starts (found from the classes' method lists: no class is initialized for it).
macOS: Objective-C plugins of `apple/` and `macos/` are compiled into CMake desktop builds and the
macOS editor's Play library too (Swift only in the Xcode project), and their SDKs linked (desktop
builds carry the dynamic frameworks in `Frameworks/` and resource bundles beside the executable); there `+jasperInit` runs at the
engine's start and at each Play for classes not run yet. macOS never unloads Objective-C classes:
after changing an Objective-C plugin, restart the editor (Play warns once).

## 6. Web (JavaScript)

Each `plugins/native/web/*.js` becomes a `<script src="plugins/<file>">` before the engine (in name
order, after the `web.script_urls` SDKs). Register an object, or call any global by path:

```js
// plugins/native/web/poki.js
JasperNative.register("Poki", {
    init: function () { return PokiSDK.init().then(() => true); },   // Native.callAsync: Promises are awaited
    gameplayStart: function () { PokiSDK.gameplayStart(); },        // Native.call
    rewarded: async function () {
        JasperNative.pauseEngine();
        try { return await PokiSDK.rewardedBreak(); } finally { JasperNative.resumeEngine(); }
    }
});
```

Lua: `Native.call("Poki", "gameplayStart")`, `Native.callAsync("Poki", "rewarded", nil, cb)`, or a
global by a dotted path: `Native.call("window.localStorage", "getItem", "k")`,
`Native.callAsync("navigator.clipboard", "writeText", {"hi"})`. A bare name is only a registered
object (never a browser global like `Notification`, so a plugin's Lua mock still answers). The function runs with `this` = the object. Sync: the return value as JSON
(a Promise is `async_only`). Async: the awaited value; a rejection with `{code, message}` keeps
them, any other reason is `exception`. Functions, DOM nodes, Map/Set, cycles, NaN are
`unsupported_type`. `JasperNative`: `register(name, object)`, `emit(plugin, event, payload)`,
`canvas`, `pauseEngine()`, `resumeEngine()`; scripts may call `register`/`emit` before the engine
starts (queued). Everything is single-threaded: `background` is `main`.

## 7. C++ plugins

`plugins/native/cpp/*.cpp` is a C++ source root of the Play library and every export (no Script
component needed); a project whose only C++ is there still builds the Play library.

```cpp
#include "JasperEngine.h"
using namespace jasperengine;

JASPERENGINE_NATIVE_PLUGIN(Steam) {
    plugin.method("userName", [](const NativeJson& args) -> NativeJson { return "me"; });
    plugin.method("add", [](const NativeJson& args) -> NativeJson {
        if (args.size() != 2) throw NativeError("bad_args", "add takes two numbers");
        return args[0].get<double>() + args[1].get<double>();
    });
    plugin.asyncMethod("login", [](NativeCall call) {
        std::thread([call]() { call.resolve({{"ok", true}}); }).detach();   // any thread
    });
}
```

A C++ plugin answers first on every platform, so one that shares a name with Java/ObjC/JS
plugins shadows them: the scaffold wraps it in `#if !(defined(__ANDROID__) || ...)` for the
platforms that have their own. C++ scripts can call `NativeBridge::call/callAsync/on/emit` too
(`core/native/NativeBridge.h`). In the editor the plugins go with the Play library at Play stop
and register again at the next Play.

## 8. The `plugins/native` folder

```
plugins/native/
  android/   *.java *.kt (any layout), libs/*.aar *.jar, res/**, AndroidManifest.xml, proguard-rules.pro
  apple/     *.m *.mm *.h *.c *.cpp *.swift   (iOS and macOS)
  ios/       the same + *.framework *.xcframework *.a   (iOS)
  macos/     the same   (macOS)
  web/       *.js
  cpp/       *.cpp *.h   (a C++ source root everywhere)
```

At the project root; never copied as assets. `native_plugins {action: "list"}` lists plugins
(methods parsed from the sources), Lua mocks and the native settings. Project Settings > Native
Plugins shows the counts, opens the folder and creates plugins.

**Export.** Android: a `nativeplugins` Gradle module (the folder's sources, `res`, manifest, the
generated `JasperNativeRegistry` and keep rules, the dependency settings as `api`), on top of the
always-present `jasperbridge` module; the app depends on both and links `libs/`. iOS/macOS: the
sources join the app targets of the Xcode project; SDK frameworks are linked (and embedded
when they are not static archives), with their search paths; Swift turns on `SWIFT_VERSION 5.0`
and a generated bridging header. Web: the scripts and the stub that queues registrations. A
re-export with the same inputs writes the same files.

**Prebuilt builds** (no compiler) carry what they can and name the rest as a reason to compile
(docs/shipping.md, "Native plugins in prebuilt builds"): iOS adds the Objective-C/Swift plugins,
SDKs, Swift packages, frameworks, Info.plist entries and entitlements to the player's Xcode
project; Web adds the scripts; Android adds `extra_permissions` and the manifest XML. C++ plugins,
Java/Kotlin, AARs, Gradle dependencies, CocoaPods and macOS plugins make the build compile (on
Android and iOS: Export source). `build_project` and `get_build_info` list both (`carried`,
`compile_reasons`). A mock that would answer in a shipped game for a plugin with no code on its
platform is the build warning `W-NATIVE-MOCK`, and the game logs it once.

## 9. Project settings (SDK dependencies)

Project Settings > Native Plugins, or `set_project_settings` (`get_project_settings {keys: [...],
describe: true}` explains each):

| Key | Export effect |
|---|---|
| `android.gradle_dependencies` | `"group:name:version"` -> `api "..."` in the nativeplugins module; other text is a dependencies line as is (`implementation platform('com.google.firebase:firebase-bom:33.0.0')`) |
| `android.gradle_repositories` | `maven { url "..." }` in `allprojects.repositories` |
| `android.gradle_properties` | `key=value` lines of `gradle.properties` (replacing a key) |
| `android.extra_permissions` | `<uses-permission android:name="...">` by full name |
| `android.manifest_xml` / `android.manifest_application_xml` | raw XML inside `<manifest>` (before `<application>`) / inside `<application>`; `tools:` is declared when used |
| `ios.plist_entries` / `macos.plist_entries` | Info.plist keys -> string, boolean, number or string list (replacing) |
| `ios.plist_xml` | raw `<key>..</key><value>` pairs appended (dictionaries, `SKAdNetworkItems`) |
| `ios.frameworks` / `macos.frameworks` | system frameworks linked (`"StoreKit"`; `"AppTrackingTransparency?"` = weak), besides the engine's own, which every export links (AVFoundation, AudioToolbox, CoreMedia, CoreVideo, Metal, MetalKit, QuartzCore, UIKit/AppKit...) |
| `ios.cocoapods` | `[{name, version, source}]`: `workspaces/xcode/Podfile` (static frameworks); run `pod install` there and open `JasperEngine.xcworkspace` (README-pods.txt says so) |
| `ios.swift_packages` | `[{url, version, products}]`: package references (up to the next major version), products linked |
| `ios.entitlements_xml` | the pairs of `ios/JasperEngine.entitlements` (`CODE_SIGN_ENTITLEMENTS`) |
| `ios.deployment_target` | `IPHONEOS_DEPLOYMENT_TARGET` of every target (15.0 or later) |
| `web.script_urls` | `<script src>` in `<head>`, before the plugin scripts |
| `apple.privacy_*` | the Apple privacy manifest's additions (tracking, tracking domains, collected data, required-reason APIs); with the `PrivacyInfo.xcprivacy` files of static plugins in `apple/`, `ios/`, `macos/` (`native_plugins {action: "privacy_manifest"}` shows the result) |

XML fragments are checked when set (MCP refuses, the window shows a red line) and again at export.
Android's INTERNET permission is not implied by native plugins: set Internet Access to Require if
an SDK needs the network.

## 10. Lifecycle, pausing, SDK wrappers (ads, purchases)

A wrapper of an ads or purchases SDK is written on top of this bridge alone:

- **Init on the UI thread**: Android `onJasperInit(Activity)`, Apple `+jasperInit`, Web the script.
- **Calls**: `Native.callAsync("Ads", "showRewarded", {unit}, cb, {owner = self, timeout = 60})`;
  the method shows the ad on the UI thread and completes the call when it closes.
- **Events**: `JasperNative.emit("Ads", "paid", {value = ...})` -> `Native.on("Ads", "paid", fn, self)`.
- **Pause while a full-screen ad shows**: `JasperNative.pauseEngine()` before, `resumeEngine()`
  after (counted; audio pauses and `Engine.onPause` / `onResume` fire once; the OS's own pause is
  a separate hold: the engine runs again only when both let go; Play stop resets it).
- **Banners**: Android `JasperNative.getActivity().addContentView(view, params)` on the UI thread;
  Apple `[JasperNative.view addSubview:banner]`; then `Screen.setExtraInsets` (feature safe-area)
  so the game's UI keeps clear of it.
- **Lifecycle**: Android `JasperLifecycleListener` (resume, activity results, Back), Apple
  `JasperLifecycleObserver` (active/background, URLs).
- **Dependencies**: `android.gradle_dependencies` + `android.gradle_repositories`,
  `android.manifest_application_xml` for an SDK's `<meta-data>` (an app ID),
  `android.extra_permissions` (`com.google.android.gms.permission.AD_ID`), `ios.cocoapods` or
  `ios.swift_packages`, `ios.plist_entries` (`NSUserTrackingUsageDescription`,
  `GADApplicationIdentifier`), `ios.plist_xml` (`SKAdNetworkItems`), `ios.frameworks`.
- **Editor**: a Lua mock that answers the same calls (resolve after a delay, emit fake events).
- **Privacy**: an ads SDK tracks: `apple.privacy_tracking` true, its domains in
  `apple.privacy_tracking_domains`, what it collects in `apple.privacy_collected_data`, and
  `NSUserTrackingUsageDescription` in `ios.plist_entries`. An SDK framework that is dynamic carries
  its own manifest.

### Worked example: interstitial ads with Google Mobile Ads

The engine has no ads SDK of its own; this plugin is all a game needs (take the SDK versions from
Google's release notes). The same shape fits any ads or purchases SDK.

Lua, the same on both platforms (and in Play, with the mock):

```lua
local ads = {}

function ads:init()
    Native.callAsync("Ads", "start", nil, function(result, err, code)
        if err then Log.warn("ads did not start: " .. err .. " (" .. code .. ")") end
    end, {owner = self})
end

function ads:showBetweenLevels(unitId, done)
    Native.callAsync("Ads", "showInterstitial", {unitId}, function(result, err, code)
        if err then Log.warn("no ad: " .. err) end
        done()                                   -- the game goes on either way
    end, {owner = self, timeout = 60})
end

return ads
```

Android, `plugins/native/android/java/com/me/ads/Ads.java` (the consent form first, as Google
requires in the EEA and the UK; the engine pauses while the ad covers the game):

```java
package com.me.ads;

import android.app.Activity;
import com.google.android.gms.ads.AdError;
import com.google.android.gms.ads.AdRequest;
import com.google.android.gms.ads.FullScreenContentCallback;
import com.google.android.gms.ads.LoadAdError;
import com.google.android.gms.ads.MobileAds;
import com.google.android.gms.ads.interstitial.InterstitialAd;
import com.google.android.gms.ads.interstitial.InterstitialAdLoadCallback;
import com.google.android.ump.ConsentRequestParameters;
import com.google.android.ump.UserMessagingPlatform;
import com.jaspergameengine.bridge.JasperNative;
import com.jaspergameengine.bridge.NativeCall;

public class Ads {
    public static void start(final NativeCall call) {
        JasperNative.runOnUiThread(() -> {
            Activity activity = JasperNative.getActivity();
            UserMessagingPlatform.getConsentInformation(activity).requestConsentInfoUpdate(activity,
                new ConsentRequestParameters.Builder().build(),
                () -> UserMessagingPlatform.loadAndShowConsentFormIfRequired(activity, formError -> {
                    MobileAds.initialize(activity, status -> call.resolve(true));
                }),
                error -> call.reject("consent", error.getMessage()));
        });
    }

    public static void showInterstitial(String unitId, final NativeCall call) {
        JasperNative.runOnUiThread(() -> {
            Activity activity = JasperNative.getActivity();
            InterstitialAd.load(activity, unitId, new AdRequest.Builder().build(), new InterstitialAdLoadCallback() {
                @Override public void onAdLoaded(InterstitialAd ad) {
                    ad.setFullScreenContentCallback(new FullScreenContentCallback() {
                        @Override public void onAdDismissedFullScreenContent() { JasperNative.resumeEngine(); call.resolve(true); }
                        @Override public void onAdFailedToShowFullScreenContent(AdError e) { JasperNative.resumeEngine(); call.reject("show", e.getMessage()); }
                    });
                    JasperNative.pauseEngine();
                    ad.show(activity);
                }
                @Override public void onAdFailedToLoad(LoadAdError e) { call.reject("no_fill", e.getMessage()); }
            });
        });
    }
}
```

Its settings: `android.gradle_dependencies` `["com.google.android.gms:play-services-ads:<version>"]`,
`android.manifest_application_xml` `<meta-data android:name="com.google.android.gms.ads.APPLICATION_ID"
android:value="ca-app-pub-...~..."/>` (the App ID from the AdMob console; Google's sample App ID
serves test ads only), `android.extra_permissions` `["com.google.android.gms.permission.AD_ID"]`,
and Internet Access set to Require. Java makes the Android build compile: Export source, then
Android Studio.

iOS, `plugins/native/ios/Ads.m` (+ `Ads.h`: `@interface Ads : NSObject <JasperNativePlugin>`):

```objc
#import "Ads.h"
@import GoogleMobileAds;
@import UserMessagingPlatform;

@interface AdsDelegate : NSObject <GADFullScreenContentDelegate>
@property(nonatomic, strong) JasperNativeCall* call;
@property(nonatomic, strong) GADInterstitialAd* ad;
@end
@implementation AdsDelegate
- (void)adDidDismissFullScreenContent:(id<GADFullScreenPresentingAd>)ad { [JasperNative resumeEngine]; [self.call resolve:@YES]; }
- (void)ad:(id<GADFullScreenPresentingAd>)ad didFailToPresentFullScreenContentWithError:(NSError*)error {
    [JasperNative resumeEngine];
    [self.call reject:@"show" message:error.localizedDescription];
}
@end

static AdsDelegate* current;

@implementation Ads
+ (void)start:(NSArray*)args call:(JasperNativeCall*)call {
    [UMPConsentInformation.sharedInstance requestConsentInfoUpdateWithParameters:[UMPRequestParameters new]
        completionHandler:^(NSError* error) {
            [UMPConsentForm loadAndPresentIfRequiredFromViewController:JasperNative.viewController completionHandler:^(NSError* formError) {
                [GADMobileAds.sharedInstance startWithCompletionHandler:^(GADInitializationStatus* status) { [call resolve:@YES]; }];
            }];
        }];
}
+ (void)showInterstitial:(NSArray*)args call:(JasperNativeCall*)call {
    [GADInterstitialAd loadWithAdUnitID:args[0] request:[GADRequest request]
        completionHandler:^(GADInterstitialAd* ad, NSError* error) {
            if (error) { [call reject:@"no_fill" message:error.localizedDescription]; return; }
            current = [AdsDelegate new];
            current.call = call;
            current.ad = ad;
            ad.fullScreenContentDelegate = current;
            [JasperNative pauseEngine];
            [ad presentFromRootViewController:JasperNative.viewController];
        }];
}
@end
```

Its settings: `ios.swift_packages` `[{url: "https://github.com/googleads/swift-package-manager-google-mobile-ads",
version: "<version>", products: ["GoogleMobileAds"]}]` (the package brings the consent SDK too),
`ios.plist_entries` `{"GADApplicationIdentifier": "ca-app-pub-...~...", "NSUserTrackingUsageDescription":
"..."}`, `ios.plist_xml` with Google's `SKAdNetworkItems` list, and the privacy manifest:
`apple.privacy_tracking` true plus the data the SDK collects (`apple.privacy_collected_data`,
from Google's data disclosure). Google's frameworks are dynamic and carry their own manifests. The
prebuilt iOS build carries all of it.

The mock, `lua/native_mocks/Ads.lua`, for Play and desktop builds:

```lua
local Ads = {}
function Ads.start(call) return true end
function Ads.showInterstitial(unitId, call) return true end
return Ads
```

## 11. Unity and Godot equivalents

| Unity / Godot | Here |
|---|---|
| `new AndroidJavaClass("com.me.Device").CallStatic<int>("getCode")` | `Native.call("Device", "getCode")` (or `"com.me.Device"`) |
| `AndroidJavaProxy` callbacks | a `NativeCall` (one result) or `JasperNative.emit` + `Native.on` (many) |
| `AndroidJavaObject` instances | not across the boundary: keep objects in static fields of the plugin |
| `[DllImport("__Internal")]` (iOS) | an Objective-C/Swift class method |
| `.jslib` + `SendMessage` (Web) | `plugins/native/web/*.js` + `JasperNative.emit` |
| `UnitySendMessage("Obj", "Method", "msg")` | `JasperNative.emit(plugin, event, payload)` |
| `Assets/Plugins/Android`, `Plugins/iOS` | `plugins/native/android`, `plugins/native/ios` |
| EDM4U `*Dependencies.xml` (androidPackage, iosPod) | `android.gradle_dependencies`, `ios.cocoapods` |
| `mainTemplate.gradle`, `AndroidManifest.xml` edits | `android.gradle_*`, `android.manifest_xml` settings, the plugin folder's manifest |
| Godot `Engine.get_singleton("Plugin").method()`, signals | `Native.call("Plugin", "method")`, `Native.on` |
| Godot `JavaScriptBridge.eval` / `get_interface` | `Native.call("globalThing.path", "fn", ...)` |
