# `platform-services`: URL opening, locale, frame rate, orientation, focus and device info (design)

Status: binding spec for branch `gaps/platform-services` (ledger #3). Read with
[AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) and [GAP-REPORT.md §1-2](../../engine-gaps/GAP-REPORT.md).
Shares the Game view device simulation with `safe-area` (that spec owns the simulator; this one
adds orientation, language and lifecycle simulation to it).

## 1. Goals

1. `System.openURL(url)` on every platform and in the editor.
2. System language and locale: Unity-style `SystemLanguage` enum **and** ISO codes (language,
   region, BCP 47 tag, preferred list).
3. Frame pacing: `Engine.targetFrameRate` and `Engine.vSyncCount` at runtime, Unity semantics,
   on Android (Swappy), iOS (`preferredFramesPerSecond`), desktop (swap interval + limiter), Web
   (`requestAnimationFrame` throttling) and editor Play.
4. Runtime orientation: Unity's `Screen.orientation` and `Screen.autorotateTo*`, current interface
   orientation, `Engine.onOrientationChanged`, `Input.deviceOrientation`; shared export settings
   (default + allowed orientations) for iOS **and** Android (today Info.plist always allows
   portrait, `engine/workspaces/xcode/ios/Info.plist:263-268`; Android has its own enum,
   `editor/Project.h:545-564`).
5. App focus: `Engine.onFocus(hasFocus)` + `Engine.isFocused`, consistent with `onPause/onResume`.
6. What a typical mobile game reads from Unity's `Application`/`SystemInfo`/`Screen`/`Handheld`:
   app version/build/identifier/name, OS string (Unity format, the game parses
   `SystemInfo.operatingSystem` with `Android OS (\d+)`), OS version, Android API level, device
   model and type, `isEditor`, internet reachability, vibration and haptics, screen sleep
   (`Screen.sleepTimeout`), dpi, refresh rate, low-memory event, processor count, memory size.
7. Agents can drive and observe all of it: MCP simulation of lifecycle events, simulated language
   and orientation, opened URLs reported instead of launching a browser in headless runs.

## 2. Non-goals

Battery level/status, accelerometer/gyroscope/compass (`Input.acceleration`), device unique
identifier (privacy; games make their own GUID like the game's `ArakumaAnalytics`), deep links
(`Application.deepLinkActivated`, URL schemes), runtime permission requests (`Permission.RequestUserPermission`:
doable through `native-bridge`), notifications, screen brightness, multiple displays, `runInBackground`
toggling (desktops keep running unfocused, as today), FaceUp/FaceDown on Android (no sensor fusion here).

## 3. Users and stories

- Port of `SimpleVsync.cs`: `Engine.targetFrameRate = vsyncOn and 60 or -1` (re-applying on scene
  load is harmless: the value is global and persists across loads).
- Port of `AutoRotate.cs` line by line: `Screen.autorotateToPortraitUpsideDown = false`, ...,
  `Screen.orientation = ScreenOrientation.LandscapeLeft`, `Input.deviceOrientation ==
  DeviceOrientation.LandscapeLeft`, then `Screen.orientation = ScreenOrientation.AutoRotation`.
- Port of `Localization.cs:169`: `if System.getSystemLanguage() == SystemLanguage.Spanish then`.
  PreciseLocale `GetRegion()` (`AdsManagement.cs:285`) -> `System.getRegionCode()`.
- `MobileVersionCode.GetVersionCode()` -> `System.getAppBuild()` (no plugin);
  `AndroidVer.GetDeviceAndroidVersion()` -> `System.getAndroidApiLevel()` or the Unity-format
  `System.getOperatingSystem()`.
- `CheckVersion.GoToPlaystore` -> `System.openURL(link)`.
- `MinigameSongs.OnApplicationFocus(false)` -> the per-object message `function M:onApplicationFocus(focused)`
  (feature `script-lifecycle`, called on every script that defines it) or, globally,
  `RegisterEngineEvent(self, "onFocus")`; app-open ad on resume (`AdsManagement.cs:739`) ->
  `onApplicationPause(paused)` / `onPause`/`onResume`.
- **Agent:** `send_input [{type:"app", action:"focus_lost"}, {type:"app", action:"pause"}, {type:"wait", ms:500}, {type:"app", action:"resume"}]`
  to test the app-open-ad path; `configure_game_view {device:"iphone_15", orientation:"portrait", language:"es-MX"}`
  to see the game rotate itself to landscape and pick Spanish; `get_play_state.opened_urls` to
  check the store link.

## 4. Research summary

| Topic | Unity | Others | Decision |
|---|---|---|---|
| Open URL | `Application.OpenURL` ([docs](https://docs.unity3d.com/ScriptReference/Application.OpenURL.html)) | Godot `OS.shell_open` ([docs](https://docs.godotengine.org/en/stable/classes/class_os.html#class-os-method-shell-open)), Defold `sys.open_url` | `System.openURL` returning success; Web user-gesture fallback (Unity WebGL suffers popup blocking). |
| Language | `Application.systemLanguage` enum ([docs](https://docs.unity3d.com/ScriptReference/SystemLanguage.html)); no region API (hence the game's PreciseLocale plugin) | Godot `OS.get_locale()` / `get_locale_language()`; Defold `sys.get_sys_info().language/territory/device_language` | Enum + codes + BCP 47 + preferred list. |
| Frame rate | `Application.targetFrameRate` (-1 = platform default; ignored on desktop when `QualitySettings.vSyncCount > 0`; mobile ignores vSyncCount) ([docs](https://docs.unity3d.com/ScriptReference/Application-targetFrameRate.html)), Android Optimized Frame Pacing (Swappy) | Godot `Engine.max_fps` + `DisplayServer.window_set_vsync_mode`; Unreal `t.MaxFPS`, `r.VSync` | Unity semantics exactly (porting fidelity), Swappy on Android ([frame pacing](https://developer.android.com/games/sdk/frame-pacing)). |
| Orientation | `Screen.orientation`, `Screen.autorotateTo*`, `Input.deviceOrientation`, Player *Default Orientation* + *Allowed Orientations for Auto Rotation* ([docs](https://docs.unity3d.com/ScriptReference/Screen-orientation.html)) | Godot `DisplayServer.screen_set_orientation` (enum incl. sensor variants); Defold display orientation in game.project only | Unity API names and the same two export settings, shared by Android and iOS. |
| Focus / pause | `OnApplicationFocus(bool)`, `OnApplicationPause(bool)`, `Application.isFocused` | Godot `NOTIFICATION_APPLICATION_FOCUS_IN/OUT`, `..._PAUSED/RESUMED` | `Engine.onFocus(hasFocus)` next to `onPause/onResume`, fixed ordering rules. |
| Device info | `SystemInfo.operatingSystem` ("Android OS 13 / API-33 (TP1A...)"), `deviceModel`, `deviceType`, `systemMemorySize`, `processorCount`, `Application.version/identifier/productName`, `Application.isEditor`, `Application.internetReachability`, `Handheld.Vibrate`, `Screen.sleepTimeout`, `Screen.dpi`, `Screen.currentResolution.refreshRateRatio`, `Application.lowMemory` | Godot `OS.get_model_name`, `OS.get_version`, `Input.vibrate_handheld` | Same set (minus battery), on `System`/`Screen`/`Engine`; haptics added (Godot/Unity need plugins for iOS feedback generators). |
| Device simulation | Device Simulator simulates orientation, safe area, `Screen`, `SystemInfo` ([docs](https://docs.unity3d.com/Manual/device-simulator-introduction.html)) | none built in | Game view device simulation (safe-area spec) + orientation, language, lifecycle. |

## 5. Product decisions

1. **Where things live** (engine style, Unity member names): platform/OS/app services are static
   functions on `System` (it already has `quit`, clipboard, paths); display things on a new static
   class `Screen` (Unity's name; `safe-area` adds `Screen.safeArea`); frame and lifecycle state and
   events on `Engine` (events must be `Engine.*` for `RegisterEngineEvent`, `LuaBinding.cpp:523-548`);
   `Input.deviceOrientation` on `Input` like Unity. No aliases of existing functions
   (`System.getScreenWidth()` stays the pixel width; the glossary maps Unity's `Screen.width`).
2. **Frame pacing = Unity rules** (6.4). Default `targetFrameRate = -1`: mobile 60 fps (today's
   behaviour, Swappy `SWAPPY_SWAP_60FPS`, `NativeEngine.cpp:639`), Web every animation frame,
   desktop/editor uncapped with vsync. `vSyncCount` starts from the project's VSync setting (1 or 0).
3. **Orientation export settings are shared**: `player.default_orientation` and
   `player.allowed_orientations`, replacing `android.orientation` (migrated, 6.6). Defaults:
   `auto_rotation` with `[portrait, landscape_left, landscape_right]` (what the iOS template allowed;
   Android `Unspecified` behaved like it on phones).
4. **Unity orientation names and their platform mapping are fixed** (6.5): Unity `LandscapeLeft`
   = device rotated counter-clockwise from portrait (home button / navigation on the right) =
   Android `SCREEN_ORIENTATION_LANDSCAPE` / `Surface.ROTATION_90` = iOS
   `UIInterfaceOrientationLandscapeRight` = `UIDeviceOrientationLandscapeLeft`.
5. **Android respects the user's rotation lock** in auto-rotation (`user*` orientation constants),
   as Unity's default *Auto Rotation Behavior: User* (the game's `androidAutoRotationBehavior: 1`).
6. **Focus vs pause:** focus = the app is frontmost and receives input; pause = backgrounded.
   Order when leaving: `onFocus(false)` then `onPause`; when returning: `onResume` then
   `onFocus(true)`. `Engine::systemPause/systemResume` become idempotent (no event or audio call
   when already in that state). Android keeps rendering while visible but unfocused (permission
   dialog, notification shade) like Unity; frames stop only when not visible.
7. **Screen sleep**: `Screen.sleepTimeout` (`SleepTimeout.NeverSleep` = -1, `SleepTimeout.SystemSetting`
   = -2), initial value from the new shared setting `player.keep_screen_on` (default **true**, the
   current Android behaviour: `NativeEngine.cpp:742` always sets `AWINDOW_FLAG_KEEP_SCREEN_ON`, so
   `android.keep_screen_on` had no effect when false). `android.keep_screen_on` is removed (6.6).
8. **openURL safety**: allowed schemes = any `scheme:` except `javascript:`, `data:`, `file:`,
   `vbscript:`; those return false with an error log. Web: when the popup is blocked, the URL opens
   on the next user gesture (pointer up / key up / touch end on the page) and the call returns false.
   Editor: opens the host browser, except when the editor runs headless (MCP `--headless`, no
   display): then only logged and recorded. Every call in editor Play is recorded for MCP.
9. **Simulation is editor-only and explicit** (Game view, MCP): simulated language, orientation,
   lifecycle events. Real devices are never affected; values are not saved into builds.
10. **Permissions auto-added** on Android by the export scanner from `http`
    (`ExportFeatureScan`): `VIBRATE` when `System.vibrate` is used, `ACCESS_NETWORK_STATE` when
    `System.getInternetReachability` is used.

## 6. Design

### 6.1 Runtime C++

**`System` additions** (`engine/core/System.h`), virtual with base implementations in `System.cpp`
(base = desktop-neutral defaults; every platform overrides what it can, table 6.7):

```cpp
// URLs
virtual bool openURL(const std::string& url);                 // base: false + warning
// locale
virtual std::vector<std::string> getPreferredLocales();       // BCP 47, most preferred first; base: {"en-US"}
std::string getLocale();                                      // getPreferredLocales()[0] (non-virtual)
std::string getLanguageCode();                                // "es" (lowercase, from getLocale)
std::string getRegionCode();                                  // "MX" ("" when the tag has none; virtual getRegionOverride first)
virtual std::string getRegionOverride();                      // platforms with a separate region setting (iOS/macOS NSLocale.regionCode, Android Locale.getDefault().getCountry()); base ""
SystemLanguage getSystemLanguage();                           // LocaleRules::toSystemLanguage(getLocale())
// app
virtual std::string getAppVersion(); virtual int64_t getAppBuild();
virtual std::string getAppIdentifier(); virtual std::string getAppName();
static void setApplicationInfo(const ApplicationInfo&);       // generated main / editor; base getters return it
// device
virtual std::string getOperatingSystem(); virtual std::string getOperatingSystemVersion();
virtual int getAndroidApiLevel();                             // 0 off Android
virtual std::string getDeviceModel(); virtual DeviceType getDeviceType();
virtual int getSystemMemorySize();                            // MiB, 0 unknown
int getProcessorCount();                                      // std::thread::hardware_concurrency(), 1 minimum
virtual bool isEditor();                                      // editor::Platform: true
virtual NetworkReachability getInternetReachability();
virtual bool vibrate(int milliseconds);                       // false when unsupported
virtual bool hapticFeedback(HapticFeedback type);
// display
virtual float getScreenDpi(); virtual float getRefreshRate();
virtual void setKeepScreenOn(bool keepOn);
virtual void applyOrientationPolicy(ScreenOrientation requested, uint8_t allowedMask);
virtual ScreenOrientation getScreenOrientation();             // base: from getScreenWidth/Height
virtual DeviceOrientation getDeviceOrientation();             // base: Unknown
virtual void applyFramePacing(const FramePacing& pacing);     // 6.4
```

Enums (in `System.h`, bound in `CoreClassesLua.cpp` next to `Platform`, `:254`):
- `SystemLanguage`: Unity order and names: `Afrikaans, Arabic, Basque, Belarusian, Bulgarian, Catalan,
  Chinese, Czech, Danish, Dutch, English, Estonian, Faroese, Finnish, French, German, Greek, Hebrew,
  Hungarian, Icelandic, Indonesian, Italian, Japanese, Korean, Latvian, Lithuanian, Norwegian, Polish,
  Portuguese, Romanian, Russian, SerboCroatian, Slovak, Slovenian, Spanish, Swedish, Thai, Turkish,
  Ukrainian, Vietnamese, ChineseSimplified, ChineseTraditional, Unknown, Hindi` (values 0..43 as in Unity).
- `ScreenOrientation`: `Portrait = 1, PortraitUpsideDown = 2, LandscapeLeft = 3, LandscapeRight = 4,
  AutoRotation = 5` (Unity values); `DeviceOrientation`: `Unknown = 0, Portrait, PortraitUpsideDown,
  LandscapeLeft, LandscapeRight, FaceUp, FaceDown` (Unity values).
- `DeviceType`: `Unknown, Handheld, Console, Desktop`. `NetworkReachability`: `NotReachable,
  ReachableViaCarrierDataNetwork, ReachableViaLocalAreaNetwork`. `HapticFeedback`: `Light, Medium,
  Heavy, Selection, Success, Warning, Error`. `SleepTimeout`: `NeverSleep = -1, SystemSetting = -2`.

**Pure rule functions** (testable on Linux, used by every platform): `engine/core/util/PlatformRules.{h,cpp}`:
- `SystemLanguage LocaleRules::toSystemLanguage(const std::string& bcp47)`: language subtag
  (case-insensitive, `_` accepted as `-`) -> enum by ISO 639-1/-2: `af ar eu be bg ca cs da nl en et
  fo fi fr de el he/iw hu is id/in it ja ko lv lt nb/nn/no pl pt ro ru sr/hr/bs/sh->SerboCroatian sk sl
  es sv th tr uk vi hi`; `zh`: script `Hant` or region `TW/HK/MO` -> `ChineseTraditional`, else
  `ChineseSimplified`; empty/unknown -> `Unknown`.
- `std::string LocaleRules::normalize(const std::string&)`: `es_MX.UTF-8@euro` -> `es-MX`;
  `C`/`POSIX` -> `en-US`.
- Orientation mapping (6.5) and frame pacing (6.4) functions.
- `std::string OsRules::androidOperatingSystem(release, sdkInt, display, incremental)`.

**Engine additions** (`engine/core/Engine.{h,cpp}`):
```cpp
static void setTargetFrameRate(int fps);  static int getTargetFrameRate();   // -1 default; 0 -> -1; clamp 1..1000
static void setVSyncCount(int count);     static int getVSyncCount();        // 0..4; initial from JASPERENGINE_VSYNC_ENABLED / project
static bool isFocused();
static void systemFocus(bool hasFocus);         // platforms; idempotent; fires onFocus
static void systemOrientationChanged();         // platforms after a rotation; fires onOrientationChanged if Screen::getOrientation() changed
static void systemLowMemory();
static FunctionSubscribe<void(bool)> onFocus;
static FunctionSubscribe<void(int)> onOrientationChanged;   // ScreenOrientation value
static FunctionSubscribe<void()> onLowMemory;
```
`systemPause`/`systemResume` (`Engine.cpp:1580-1590`) return early when the `paused` flag already
has the requested value. New events join `removeSubscriptionsByTag` and `clearAllSubscriptions(true)`
(`Engine.cpp:915-992`) as lifecycle events (not paused by `pauseGameEvents`). Lua binding of
`FunctionSubscribe<void(bool)>` (`FunctionSubscribe_V_B`) next to the others (`CoreClassesLua.cpp:939+`).

**Screen state** `engine/core/Screen.{h,cpp}` (static class `jasperengine::Screen`):
```cpp
static void setOrientation(ScreenOrientation o);     // fixed or AutoRotation; calls System::applyOrientationPolicy
static ScreenOrientation getOrientation();           // System::getScreenOrientation() (never AutoRotation)
static ScreenOrientation getRequestedOrientation();
static void setAutorotate(ScreenOrientation o, bool allowed); static bool getAutorotate(ScreenOrientation o);
static uint8_t getAllowedMask();                     // bits 1<<Portrait ... ; default from project
static void setSleepTimeout(int value);  static int getSleepTimeout();   // -1/-2 or seconds>0 treated as SystemSetting (Unity mobile semantics)
static float getDpi(); static float getRefreshRate();
static void initialize(ScreenOrientation defaultOrientation, uint8_t allowedMask, bool keepScreenOn); // generated main / editor Play start
```
Setting `autorotateTo*` re-applies the policy only while `requested == AutoRotation` (Unity).
Turning off the orientation currently shown while in AutoRotation rotates to the first allowed in
the order Portrait, LandscapeLeft, LandscapeRight, PortraitUpsideDown. All four false: the change is
refused, a warning logged, the flag stays true (Unity logs too).

### 6.2 Lua API

Binding in `CoreClassesLua.cpp` (`System` block `:1726-1766`, `Engine` block `:696-785`, `Input`
`:1534+`) and a new `Screen` class there.

| Lua | Type | Semantics |
|---|---|---|
| `System.openURL(url) -> bool` | fn | 5.8; never raises for a bad URL (returns false). |
| `System.getSystemLanguage() -> SystemLanguage` | fn | |
| `System.getLanguageCode() -> string` | fn | `"es"` |
| `System.getRegionCode() -> string` | fn | `"MX"`, `""` unknown |
| `System.getLocale() -> string` | fn | `"es-MX"` |
| `System.getPreferredLanguages() -> {string}` | fn | BCP 47 list |
| `System.getAppVersion()`, `getAppBuild()` (integer), `getAppIdentifier()`, `getAppName()` | fn | Android: PackageManager; iOS/macOS bundle: `CFBundleShortVersionString`, `CFBundleVersion` (integer part), bundle id, display name; else the generated application info. |
| `System.getOperatingSystem()` | fn | Unity format strings, 6.7. |
| `System.getOperatingSystemVersion()`, `getAndroidApiLevel()` | fn | |
| `System.getDeviceModel()`, `getDeviceType() -> DeviceType` | fn | |
| `System.getSystemMemorySize()`, `getProcessorCount()` | fn | MiB, count |
| `System.isEditor()` | fn | true only in editor Play |
| `System.getInternetReachability() -> NetworkReachability` | fn | like Unity: says a network exists, not that a server answers |
| `System.vibrate([ms]) -> bool` | fn | default 400 ms (Unity Handheld.Vibrate length); iOS ignores the length |
| `System.hapticFeedback(type) -> bool` | fn | |
| `Engine.targetFrameRate` | rw int | 6.4 |
| `Engine.vSyncCount` | rw int | 6.4 |
| `Engine.isFocused` | ro bool | |
| `Engine.onFocus` | event `(hasFocus)` | `RegisterEngineEvent(self, "onFocus")` |
| `Engine.onOrientationChanged` | event `(orientation)` | after the canvas changed (`onCanvasChanged` fired first) |
| `Engine.onLowMemory` | event `()` | |
| `Screen.orientation` | rw `ScreenOrientation` | get: current interface orientation (never AutoRotation); set: request |
| `Screen.autorotateToPortrait`, `autorotateToPortraitUpsideDown`, `autorotateToLandscapeLeft`, `autorotateToLandscapeRight` | rw bool | |
| `Screen.sleepTimeout` | rw int | `SleepTimeout.*` |
| `Screen.dpi`, `Screen.refreshRate` | ro number | 0 unknown dpi |
| `Input.deviceOrientation` | ro `DeviceOrientation` | |

Wrong argument types raise Lua errors (luabridge). Enum tables `SystemLanguage`, `ScreenOrientation`,
`DeviceOrientation`, `DeviceType`, `NetworkReachability`, `HapticFeedback`, `SleepTimeout` are
namespaces of integers like the existing enums.

### 6.3 Focus and lifecycle per platform

| Platform | Focus | Pause/resume | Low memory |
|---|---|---|---|
| Android | `APP_CMD_GAINED_FOCUS/LOST_FOCUS` (`NativeEngine.cpp:1382-1389`) -> `Engine::systemFocus` (game thread, synchronous). `isAnimating()` (`:1125`) becomes `mIsVisible && mHasWindow`. | `APP_CMD_PAUSE/RESUME` (existing) | `APP_CMD_LOW_MEMORY` (`:1402`) -> `systemLowMemory()` before the existing GL release. |
| iOS | `UISceneWillDeactivateNotification` -> focus false; `UISceneDidActivateNotification` -> focus true (`ViewController.m` observers). | pause on `UISceneDidEnterBackgroundNotification` (existing), resume on `UISceneWillEnterForegroundNotification` (moved from DidActivate, which with idempotency would also be harmless). | `UIApplicationDidReceiveMemoryWarningNotification`. |
| macOS (Apple backend and `platform/mac`) | `NSWindowDidBecomeKey/ResignKey` | none (desktops keep running, as today) | none |
| Windows | `WM_ACTIVATE` (`WA_INACTIVE` = false) in the window procedure (`JasperEngineWin.cpp:108` area) | none: a minimized window keeps updating through `Engine::systemUpdate` (`JasperEngineWin.cpp:403`), as today | none |
| Linux | `FocusIn/FocusOut` (`JasperEngineLinux.cpp:278-288`) | none (desktops keep running, as today) | none |
| Web | `window` `focus`/`blur` | `document.visibilitychange`: hidden -> pause, visible -> resume; combined with the CrazyGames ad callbacks (`JasperEngineWeb.cpp:117-133`) through two flags (`hidden`, `adPlaying`): pause when either becomes true, resume when both are false. | none |
| Editor Play | the editor window's OS focus (`EditorFrameState.focused`, `editor/EditorFrame.cpp`) forwarded only while a Play session is running; MCP simulation (6.8). | Editor Pause keeps calling `Engine::onPause.call()` (`editor/Project.cpp:9094`) as today; MCP `app` events call `systemPause/Resume`. | MCP `low_memory`. |

Initial state: `isFocused = true` at start (all platforms), `onFocus` fires only on changes.

### 6.4 Frame pacing

`struct FramePacing { int swapInterval; double limiterFps; int mobileFps; };`
`FramePacing PlatformRules::computeFramePacing(Platform p, int targetFrameRate, int vSyncCount, double refreshRate)`:
- **Android / iOS:** `mobileFps = targetFrameRate > 0 ? min(targetFrameRate, refreshRate) : 60`
  (clamped to `[1, refreshRate]`, refreshRate 60 when unknown); vSyncCount ignored.
  Android: `SwappyGL_setSwapIntervalNS(1e9 / mobileFps)` (Swappy picks the nearest multiple of the
  display period); iOS: `MTKView.preferredFramesPerSecond = mobileFps` (120 needs
  `ios.supports_high_refresh_rate`, already in the plist as `CADisableMinimumFrameDurationOnPhone`).
- **Desktop (Linux, Windows, macOS) and editor Play:** `vSyncCount > 0`: `swapInterval = vSyncCount`,
  `limiterFps = 0` (targetFrameRate ignored, Unity). `vSyncCount == 0`: `swapInterval = 0`,
  `limiterFps = targetFrameRate > 0 ? targetFrameRate : 0`. Backends that can't present at an
  interval > 1 (Vulkan, Metal, D3D11 can: `Present(2)`) use interval 1 + `limiterFps = refreshRate / vSyncCount`.
  macOS Metal cannot disable vsync (existing CMake warning): `swapInterval` 0 becomes 1 there.
- **Web:** `limiterFps = targetFrameRate > 0 ? targetFrameRate : 0`; vSyncCount ignored.

Application: `Engine::setTargetFrameRate/setVSyncCount` call `System::applyFramePacing(compute...)`
(also at `systemViewLoaded`). Swap interval changes: GLX `glXSwapIntervalEXT` (`JasperEngineLinux.cpp:193-198`
moved into a function), WGL `wglSwapIntervalEXT`, D3D11 present interval (`D3D11Context.cpp:234`),
Vulkan `VulkanContext::setVsync(bool)` (store in `gConfig`, recreate the swapchain at the next
frame; `VulkanContext.cpp:290`), macOS GL `NSOpenGLContextParameterSwapInterval`, Metal
`CAMetalLayer.displaySyncEnabled`. The limiter: `engine/core/util/FrameLimiter.{h,cpp}`
(`void wait(double fps)`: deadline-based; sleeps until 1 ms before the deadline then yields; a
deadline more than one period behind resets to now) called by the desktop loops right after
present (`JasperEngineLinux.cpp:363-370`, the Windows and macOS loops) and by `EditorFrame::run`
during Play (`editor/EditorFrame.cpp:119-122` uses `Engine::getVSyncCount() > 0` instead of
`project->isVSyncEnabled()` and calls the limiter when `limiterFps > 0`). Web: `renderLoop`
(`JasperEngineWeb.cpp:605`) returns early without `systemDraw` while `now - lastDraw < 1000/fps - 2 ms`.
Initial `vSyncCount`: `JASPERENGINE_VSYNC_ENABLED` (exports) / `project->isVSyncEnabled()` (editor Play
start sets `Engine::setVSyncCount`). `Engine.framerate` keeps measuring real frames.

### 6.5 Orientation

Bits: `1 << Portrait | 1 << PortraitUpsideDown | 1 << LandscapeLeft | 1 << LandscapeRight`.

**Android** (`PlatformRules::androidRequestedOrientation(requested, mask) -> int` ActivityInfo constant):
fixed: Portrait `SCREEN_ORIENTATION_PORTRAIT` (1), PortraitUpsideDown `REVERSE_PORTRAIT` (9),
LandscapeLeft `LANDSCAPE` (0), LandscapeRight `REVERSE_LANDSCAPE` (8). AutoRotation by mask:
single orientation -> its fixed constant; {P, PU} -> `USER_PORTRAIT` (12); {LL, LR} ->
`USER_LANDSCAPE` (11); {P, LL, LR} -> `USER` (2); all four -> `FULL_USER` (13); any other set ->
`FULL_USER` + one warning ("Android cannot restrict auto-rotation to this set"). Applied with a new
`MainActivity.setOrientationPolicy(int)` (UI thread, `setRequestedOrientation`) through a cached
`jmethodID` (`NativeEngine::setupJNI`, `:817`).
Current orientation: `MainActivity.getDisplayRotation()` (0/90/180/270 from `Display.getRotation()`)
and `isNaturalPortrait()` (rotation 0/180 with height > width, or 90/270 with width > height);
`PlatformRules::orientationFromRotation(rotation, naturalPortrait)`: `r' = naturalPortrait ? r : (r + 90) % 360`;
0 Portrait, 90 LandscapeLeft, 180 PortraitUpsideDown, 270 LandscapeRight. Queried on surface size
change (`NativeEngine::doFrame`, `:1090-1097`) and `APP_CMD_CONFIG_CHANGED`; cached.
Device orientation: Java `OrientationEventListener` in `MainActivity` (enabled in `onResume`,
disabled in `onPause`), degrees with natural-orientation adjustment `d' = naturalPortrait ? d : (d + 90) % 360`,
buckets with 20 degree hysteresis: around 0 Portrait, 90 LandscapeRight, 180 PortraitUpsideDown,
270 LandscapeLeft; `ORIENTATION_UNKNOWN` (flat) -> `Unknown`; stored in a `volatile int` read via JNI.
Manifest `configChanges` (`Exporter.cpp:3093`) becomes
`mcc|mnc|locale|touchscreen|keyboard|keyboardHidden|navigation|orientation|screenLayout|uiMode|screenSize|smallestScreenSize|fontScale|layoutDirection|density`
(so rotation and locale changes never recreate the activity) and `android:screenOrientation` the
manifest name of `androidRequestedOrientation(default, allowed)` (`portrait`, `reversePortrait`,
`landscape`, `reverseLandscape`, `userPortrait`, `userLandscape`, `user`, `fullUser`).

**iOS** (`ViewController.m`): `- (UIInterfaceOrientationMask)supportedInterfaceOrientations`
returns the mask from the engine state (fixed request -> only that one; AutoRotation -> allowed
mask); `shouldAutorotate` YES. Mapping (decision 4): Portrait `UIInterfaceOrientationMaskPortrait`,
PortraitUpsideDown `...PortraitUpsideDown`, LandscapeLeft `...LandscapeRight`, LandscapeRight
`...LandscapeLeft`. Applying a change: iOS 16+: `[self setNeedsUpdateOfSupportedInterfaceOrientations]`
and `[windowScene requestGeometryUpdateWithPreferences:[[UIWindowSceneGeometryPreferencesIOS alloc]
initWithInterfaceOrientations:mask] errorHandler:^(NSError* e){ log }]`; iOS 15: `[UIViewController
attemptRotationToDeviceOrientation]`, and for a fixed request whose orientation is not current,
`[[UIDevice currentDevice] setValue:@(UIInterfaceOrientation...) forKey:@"orientation"]` before it
(the long-standing approach; guarded by `@available(iOS 16, *)` checks). Current orientation:
`windowScene.interfaceOrientation` mapped back. `viewWillTransitionToSize:withTransitionCoordinator:`
completion -> `Engine::systemOrientationChanged()`. Device orientation:
`beginGeneratingDeviceOrientationNotifications` + `UIDevice.orientation` (names equal Unity's
`DeviceOrientation`, FaceUp/FaceDown included). The runtime mask is intersected with the bundle's
`UISupportedInterfaceOrientations`; asking for an orientation the export did not list logs one
warning ("not in player.allowed_orientations of this build").
Info.plist (`Exporter::writeAppleProjectSettings`): `UISupportedInterfaceOrientations` and
`UISupportedInterfaceOrientations~ipad` = allowed set with the default orientation first (fixed
default: only that one); `UIRequiresFullScreen` = true unless all four are allowed (iPad
multitasking ignores orientation locks otherwise; export log says so).

**Web:** current orientation from `screen.orientation.type` (`portrait-primary` Portrait,
`portrait-secondary` PortraitUpsideDown, `landscape-primary` LandscapeLeft, `landscape-secondary`
LandscapeRight), else from the canvas size; requests call `screen.orientation.lock(type)`
(`portrait`/`landscape`/specific/`any`), which browsers allow only in fullscreen: rejections logged at
verbose level. Change: `screen.orientation` `change` event.
**Desktop:** orientation from the window size (`width >= height` LandscapeLeft else Portrait),
requests stored only; `onOrientationChanged` when a resize flips it. Device orientation `Unknown`.
**Editor Play:** from the Game view (device simulation of `safe-area`): with a device profile,
the simulated orientation; a request (or an autorotate change) that excludes the simulated
orientation rotates the simulated device to the first allowed one (like Unity's Device Simulator)
and fires the event; without a profile, as desktop.

### 6.6 Project settings and migration

| Key (MCP) | Type, default | Notes |
|---|---|---|
| `player.default_orientation` | `"portrait"|"portrait_upside_down"|"landscape_left"|"landscape_right"|"auto_rotation"`, `auto_rotation` | |
| `player.allowed_orientations` | subset of the four, `["portrait","landscape_left","landscape_right"]` | must be non-empty when default is `auto_rotation`; a fixed default is added to the set automatically |
| `player.keep_screen_on` | bool, true | initial `Screen.sleepTimeout` |
| `quality.vsync` | (existing VSync setting, unchanged) | initial `vSyncCount` (1 or 0) |
| `player.target_frame_rate` | int, -1 | initial `Engine.targetFrameRate` (so a project can set 30 without code) |

`ApplicationSettings`/new `PresentationSettings` struct in `editor/Project.h`; `editor/Stream.cpp`
writes `presentation: {defaultOrientation, allowedOrientations, keepScreenOn, targetFrameRate}`
(only non-defaults). **Migration** on load (`Stream.cpp:3205+`): an `android.orientation` value maps
to the new keys when the project has no `presentation.defaultOrientation`: Portrait -> portrait;
Landscape -> landscape_left; SensorPortrait -> auto [portrait, portrait_upside_down];
SensorLandscape -> auto [landscape_left, landscape_right]; FullSensor -> auto all four; Unspecified
-> defaults. `android.keepScreenOn` is read and dropped (the new default keeps the screen on, which
is what builds did). Both fields leave `AndroidProjectSettings`, its UI rows
(`ProjectSettingsWindow.cpp:1918`, `:1932-1965`) and MCP keys (`android.orientation`,
`android.keep_screen_on` -> `set_project_settings` answers "renamed: use player.default_orientation /
player.allowed_orientations / player.keep_screen_on"). The Exporter's `keepScreenOn` patch of
`MainActivity.java` (`Exporter.cpp:3137-3146`) is removed; `NativeEngine::gameLoop` stops adding
`AWINDOW_FLAG_KEEP_SCREEN_ON` (`NativeEngine.cpp:742`); `Screen::initialize` applies it.

**Generator** (`editor/Generator.cpp:2301` area): emits
`Screen::initialize(ScreenOrientation::X, mask, keepOn); Engine::setTargetFrameRate(n);
System::setApplicationInfo({name, identifier, version, build});` (desktop/web values: the shared
player values with the platform overrides of the target: Linux/Windows/macOS/Web names). Editor Play:
`Project::finalizeStart` (`editor/Project.cpp:5636`) calls the same with the project values.

### 6.7 Per-platform implementation matrix

| API | Android (JNI to `MainActivity`/`JasperPlatform.java`) | iOS / macOS (`JasperEngineApple.mm`, `SystemMac.mm`) | Windows (`SystemWin.cpp`) | Linux (`SystemLinux.cpp`) | Web (`JasperEngineWeb.cpp`, EM_JS) | Editor (`editor/Platform.cpp`) |
|---|---|---|---|---|---|---|
| openURL | `Intent.ACTION_VIEW` + `FLAG_ACTIVITY_NEW_TASK` on UI thread; `ActivityNotFoundException` -> false (true when started) | `[UIApplication openURL:options:completionHandler:]` (main thread; returns `canOpenURL`-independent true when the call was made with a valid NSURL) / `[NSWorkspace openURL:]` | `ShellExecuteW(nullptr, L"open", ...)` > 32 | `posix_spawnp("xdg-open", url)` no shell, child reaped on a detached thread | `window.open(url, "_blank", "noopener")`; null -> deferred to next gesture, return false | host desktop implementation unless headless; always recorded |
| preferred locales | `LocaleList.getDefault()` (API 24+) else `Locale.getDefault()`, `toLanguageTag()` | `[NSLocale preferredLanguages]`; region override `[NSLocale currentLocale].countryCode` | `GetUserPreferredUILanguages` (MUI_LANGUAGE_NAME); region `GetUserDefaultGeoName` (fallback locale name) | `LANGUAGE` (colon list), `LC_ALL`, `LC_MESSAGES`, `LANG`, normalized | `navigator.languages` | host, or the simulated language |
| app info | `PackageManager.getPackageInfo` (`getLongVersionCode()` API 28+ else `versionCode`), `getApplicationLabel` | main bundle `infoDictionary` | generated info | generated info | generated info | project settings (shared player values) |
| OS string | `"Android OS " + RELEASE + " / API-" + SDK_INT + " (" + Build.DISPLAY + "/" + VERSION.INCREMENTAL + ")"` | `systemName + " " + systemVersion` / `"Mac OS X " + operatingSystemVersionString numbers` | `"Windows " + (build >= 22000 ? "11" : "10") + " (" + major.minor.build + ") 64bit"` via `RtlGetVersion` | `"Linux " + uname.release + " " + os-release PRETTY_NAME` | `"Web (" + platform + ")"` from `navigator.userAgentData.platform` or `navigator.platform` | host |
| OS version | `RELEASE` | `systemVersion` | `major.minor.build` | os-release `VERSION_ID` else `uname.release` | `""` | host |
| device model | `MANUFACTURER + " " + MODEL` | `utsname.machine` (`iPhone15,2`) / `sysctl hw.model` | registry `HARDWARE\DESCRIPTION\System\BIOS\SystemProductName`, else `"PC"` | `/sys/devices/virtual/dmi/id/product_name`, else `"PC"` | `"Browser"` | host, or the simulated device's model |
| device type | Handheld | Handheld / Desktop | Desktop | Desktop | Handheld when `isTouchDevice()`, else Desktop | Desktop (Handheld with a device profile) |
| memory MiB | `ActivityManager.MemoryInfo.totalMem` | `NSProcessInfo.physicalMemory` | `GlobalMemoryStatusEx` | `sysconf(_SC_PHYS_PAGES)*pagesize` | `navigator.deviceMemory*1024` or 0 | host |
| reachability | `ConnectivityManager` active network + capabilities (`TRANSPORT_CELLULAR` -> carrier, other connected -> LAN), needs ACCESS_NETWORK_STATE | `nw_path_monitor` (Network.framework; started on first call, status cached; first call before an update -> LAN if `SCNetworkReachability` flags say reachable) | `INetworkListManager::GetConnectivity` (COM) | `getifaddrs`: an `IFF_UP|IFF_RUNNING` non-loopback interface with an address -> LAN | `navigator.onLine` | host; `NotReachable` when the Game view network is offline (`http`) |
| vibrate | `VibratorManager` (31+)/`Vibrator`, `VibrationEffect.createOneShot` (26+) | `AudioServicesPlaySystemSound(kSystemSoundID_Vibrate)` (iOS), false (macOS) | false | false | `navigator.vibrate(ms)` | false + recorded |
| haptics | `View.performHapticFeedback` on UI thread: Light/Selection `CLOCK_TICK`, Medium `VIRTUAL_KEY`, Heavy `LONG_PRESS`, Success `CONFIRM` (30+, else `VIRTUAL_KEY`), Warning/Error `REJECT` (30+, else `LONG_PRESS`) | `UIImpactFeedbackGenerator` light/medium/heavy, `UISelectionFeedbackGenerator`, `UINotificationFeedbackGenerator` (iOS); `NSHapticFeedbackManager` (macOS, trackpad) | false | false | `navigator.vibrate(10/20/40/10/[20,40,20]...)` approximations | false + recorded |
| keep screen on | `GameActivity_setWindowFlags(add/remove AWINDOW_FLAG_KEEP_SCREEN_ON)` | `idleTimerDisabled` / `IOPMAssertionCreateWithName(kIOPMAssertionTypeNoDisplaySleep)` | `SetThreadExecutionState(ES_CONTINUOUS|ES_DISPLAY_REQUIRED)` | no-op | `navigator.wakeLock.request("screen")`, re-acquired on `visibilitychange` | no-op |
| dpi | `DisplayMetrics.densityDpi` | `UIScreen.scale * (iPad ? 132 : 163)` (approximation, documented) / `CGDisplayScreenSize` | `GetDpiForWindow` | `Xft.dpi` resource, else X screen mm, else 96 | `devicePixelRatio * 96` | host, or the device profile's dpi |
| refresh rate | `Display.getRefreshRate()` | `UIScreen.maximumFramesPerSecond` / `NSScreen.maximumFramesPerSecond` | `EnumDisplaySettingsW` `dmDisplayFrequency` | XRandR current mode, else 60 | median rAF interval over the first 30 frames, 60 before | host |

Android Java side: new `engine/platform/android/java/com/jaspergameengine/jasperengine/JasperPlatform.java`
(static helpers taking the activity) to keep `MainActivity` small; method ids cached in `JniData`
(`NativeEngine.h:17-57`); calls from the game thread through `NativeEngine::getJniEnv()`.

### 6.8 Editor UX and simulation

- **Project Settings > Player > Resolution and Presentation** (new section): Default Orientation
  combo; Allowed Orientations for Auto Rotation (4 checkboxes, disabled unless Auto Rotation);
  Keep Screen On; Target Frame Rate (int, -1 = platform default). Android and iOS pages show a
  read-only line "Orientation: from Player > Resolution and Presentation".
- **Game view**: the device simulation menu of `safe-area` gains **Language** (System, or a list:
  en-US, es-ES, es-MX, pt-BR, fr-FR, de-DE, it-IT, ja-JP, ko-KR, zh-Hans, zh-Hant, ru-RU, ar-SA,
  hi-IN, plus "Custom...") and **Rotate** (portrait/landscape of the profile); the Stats overlay
  (`GameWindow::drawStats`) adds `Target: 60 fps  VSync: 1`.
- **Console**: in editor Play, `System.openURL` logs `Open URL: <url>` (info).

### 6.9 MCP / agent surfaces

- `send_input` (`editor/mcp/tools/PlayTools.cpp`, the event switch at `:2439+`): new event
  `{type: "app", action: "pause"|"resume"|"focus_lost"|"focus_gained"|"low_memory"|"orientation", orientation?}`
  calling `Engine::systemPause/Resume/Focus/LowMemory` or rotating the simulated device. Documented
  in the tool description (budget!).
- `configure_game_view`: `language` (BCP 47 or `"system"`), `orientation` (`"portrait"|"landscape_left"|...`,
  needs a device profile, see safe-area).
- `get_play_state`: `"platform": {"focused", "paused", "orientation", "target_frame_rate",
  "vsync_count", "language", "opened_urls": [last 10], "vibrations": n}`.
- Project settings keys of 6.6; removed keys answer with the rename hint.
- Guide: one line in `McpInstructions.cpp`; details in `docs/mcp/guide/platform-services.md`.

### 6.10 Docs

New `docs/mcp/guide/platform-services.md`; `lua-api.md` (2.1 Engine rows, 2.5 System rows, new
`Screen`, 2.2 `Input.deviceOrientation`, 1.3 events table: `onFocus`, `onOrientationChanged`,
`onLowMemory`; enums section 5); `docs/unity-glossary.md` rows: `Application.OpenURL` ->
`System.openURL`, `Application.systemLanguage` -> `System.getSystemLanguage()`,
`Application.targetFrameRate` -> `Engine.targetFrameRate`, `QualitySettings.vSyncCount` ->
`Engine.vSyncCount`, `Screen.orientation/autorotateTo*/sleepTimeout/dpi` -> `Screen.*`,
`Screen.width/height` -> `System.getScreenWidth/Height()` (pixels) or `Engine.canvasWidth/Height`,
`Input.deviceOrientation` -> same, `OnApplicationFocus` -> the `onApplicationFocus` script message
(script-lifecycle) or the `onFocus` engine event, `OnApplicationPause(true/false)` ->
`onApplicationPause` or `onPause`/`onResume`, `Application.isFocused` -> `Engine.isFocused`, `Application.lowMemory` -> `onLowMemory`,
`Application.version/identifier/productName` -> `System.getAppVersion/getAppIdentifier/getAppName`,
Android `versionCode` / iOS `CFBundleVersion` -> `System.getAppBuild()`, `SystemInfo.operatingSystem/
deviceModel/deviceType/systemMemorySize/processorCount` -> `System.get*`, `Application.isEditor` ->
`System.isEditor()`, `Application.internetReachability` -> `System.getInternetReachability()`,
`Handheld.Vibrate` -> `System.vibrate`, Player *Default Orientation* / *Allowed Orientations* ->
`player.default_orientation` / `player.allowed_orientations`; README.

## 7. Behaviour rules and edge cases

1. All APIs are main-thread Lua calls; platform callbacks (rotation, focus, memory) happen on the
   engine thread on every platform (Android: `handleCommand` runs on the game thread; Apple: main
   thread; Web: single thread; desktop: the loop thread). No queueing needed; events fire
   synchronously at the platform callback, between frames.
2. `targetFrameRate` and `vSyncCount` persist across scene loads; Play stop restores the
   project values (next `finalizeStart`).
3. Time scale does not affect pacing.
4. `Screen.orientation` set in `init()` of exported builds applies before the first frame (the
   policy is stored and applied at `systemViewLoaded` too).
5. Rotation sequence: platform resizes -> `systemViewChanged` (`onCanvasChanged`) ->
   `systemOrientationChanged` (`onOrientationChanged(new)`), once per actual change.
6. `openURL` while the app goes to background: the engine pauses normally (`onFocus(false)`,
   `onPause`).
7. Idempotent pause/resume: duplicates from platforms (iOS DidActivate after a Control Center pull)
   no longer resume twice or resume audio that was not paused.
8. Simulated values (editor) reset at Play stop to System/no simulation.
9. `getAppBuild()` returns the integer value; a non-numeric `CFBundleVersion` part ("1.2") returns
   the first integer (1); unknown -> 0.
10. `System.getRegionCode()` prefers the platform region setting (a Spanish-language user in the US
    returns "US"), then the locale tag's region.

## 8. Performance

Device info queries are cached after the first call except reachability (cheap platform queries,
<0.1 ms; documented "don't call per frame"). Orientation queries only on resize/config change.

## 9. Tests

**Unit (gtest, `editor/tests/PlatformServicesTest.cpp`)**:
1. `LocaleRules::toSystemLanguage`: `es-MX` Spanish, `pt_BR.UTF-8` Portuguese, `zh-Hant-TW` and `zh-HK`
   ChineseTraditional, `zh-CN`/`zh-Hans`/`zh` ChineseSimplified, `nb-NO` Norwegian, `sr-Latn` and
   `hr` SerboCroatian, `iw` Hebrew, `in` Indonesian, `hi-IN` Hindi, `C` English, `""`/`xx` Unknown;
   enum integer values equal Unity's (table check).
2. `normalize`, language/region extraction, region override precedence.
3. `androidRequestedOrientation` for every mask (16) and fixed values; `orientationFromRotation` for
   natural portrait and landscape; iOS mask mapping (LandscapeLeft -> MaskLandscapeRight).
4. `computeFramePacing` table: Android target -1/30/60/120@60Hz, desktop vsync 1 target 30 (ignored),
   vsync 0 target 30 (limiter 30), vsync 2 on Vulkan (interval 1, limiter refresh/2), Web 30.
5. `FrameLimiter`: 20 frames at 50 fps take 0.4 s +/- 10% (wall clock, generous bound).
6. `Engine::systemPause` twice -> one `onPause`; focus events only on change; order rules via a
   test System.
7. `Screen` autorotate rules: all-false refused; turning off the current orientation picks the next
   allowed; settings applied only in AutoRotation; `applyOrientationPolicy` calls recorded.
8. `OsRules::androidOperatingSystem` produces `Android OS 14 / API-34 (X/Y)` and the game's regex
   `Android OS (\d+)` matches it.
9. Stream migration: old `android.orientation` values and `keepScreenOn` load into the new keys;
   save writes only `presentation`.
10. Lua stubs list the new functions, enums and events (`tools/mcp/tests/test_stubs.py`).

**MCP e2e (`tools/mcp/tests/test_platform_services.py`)**: Lua registers `onFocus/onPause/onResume`
and logs them; `send_input` app events produce the documented order; `System.openURL` recorded in
`get_play_state.opened_urls` and not launched (headless); `Engine.targetFrameRate = 30` measured
via `Engine.framerate` average within 25-35 during `wait seconds=2` with vsync off; language
simulation `es-MX` -> `System.getSystemLanguage() == SystemLanguage.Spanish`, region `MX`;
orientation simulation with an iPhone profile: script sets `Screen.orientation =
ScreenOrientation.LandscapeLeft` -> Game view landscape, `onOrientationChanged` fired, screenshot
is landscape; project settings round trip + rename hints.

**Export (test_build.py)**: manifest `screenOrientation` and `configChanges`, VIBRATE /
ACCESS_NETWORK_STATE auto, no KEEP_SCREEN_ON patch; iOS plist orientation arrays (default first,
`~ipad`), `UIRequiresFullScreen`.

**Compile checks**: Android `assembleDebug` (Java helpers, JNI ids), Web build (EM_JS, visibility,
wake lock), Apple/Windows by review (APIs listed with their availability guards).

## 10. Acceptance criteria

- [ ] Every function, property, enum and event of 6.2 bound with the given names; stubs clean.
- [ ] Unit tests 1-10 and the MCP e2e pass; Android and Web compile.
- [ ] Orientation: settings shared by Android/iOS, migration done, Android constants and iOS masks per
      6.5, iPad full-screen rule, runtime requests work in editor simulation.
- [ ] Frame pacing per 6.4 on every platform and in editor Play.
- [ ] Focus/pause ordering and idempotency on every platform; Android renders while visible.
- [ ] `Screen.sleepTimeout`; `android.keep_screen_on` removal does not change exported behaviour.
- [ ] MCP: `send_input` app events, `configure_game_view` language/orientation, `get_play_state.platform`.
- [ ] Docs, glossary, README, TOOLS.md regenerated within budget.

## 11. Integration points

| File | Change |
|---|---|
| `engine/core/System.{h,cpp}` | virtuals, enums, application info. |
| `engine/core/Screen.{h,cpp}`, `engine/core/util/{PlatformRules,FrameLimiter}.{h,cpp}` | new; `JASPERENGINE_SRCS` + `tools/xcode_sync_engine.py`. |
| `engine/core/Engine.{h,cpp}` (`:1580-1590`, `:915-992`, `:1196`) | pacing, focus, orientation, low memory, idempotent pause. |
| `engine/core/script/binding/CoreClassesLua.cpp` | bindings (`:254`, `:696-785`, `:939+`, `:1534+`, `:1726+`). |
| `engine/platform/android/{NativeEngine.cpp,JasperEngineAndroid.{h,cpp}}`, `java/.../MainActivity.java`, new `JasperPlatform.java` | 6.3, 6.5, 6.7. |
| `engine/platform/apple/{JasperEngineApple.{h,mm},Renderer.mm}`, `ios/ViewController.m` | 6.3, 6.5, 6.7, `preferredFramesPerSecond`. |
| `engine/platform/mac/{SystemMac,WindowMac,JasperEngineMac}.mm` | focus, pacing, info. |
| `engine/platform/win/{SystemWin,WindowWin,JasperEngineWin,D3D11Context}.cpp`, `engine/platform/common/VulkanContext.{h,cpp}` | focus, pacing, info. |
| `engine/platform/linux/{SystemLinux,JasperEngineLinux}.cpp` | focus, pacing, info. |
| `engine/platform/emscripten/JasperEngineWeb.{h,cpp}` | focus/visibility, throttling, info, wake lock, openURL. |
| `editor/Platform.{h,cpp}` | editor implementations + simulation + recording. |
| `editor/EditorFrame.cpp:119-122` | Play pacing. |
| `editor/Project.{h,cpp}` (`finalizeStart :5636`), `editor/Stream.cpp` | settings, migration, Play start. |
| `editor/Generator.cpp:2301` | generated init. |
| `editor/Exporter.cpp` (`:284-296`, `:3093-3097`, `:3137-3146`, `:3155+`) | manifest, plist, permission scan. |
| `editor/window/dialog/ProjectSettingsWindow.cpp`, `editor/window/GameWindow.cpp` | UI. |
| `editor/mcp/tools/{PlayTools,ViewTools,ProjectTools}.cpp` | MCP. |
| `engine/workspaces/xcode/ios/Info.plist` | template orientation keys stay; the exporter rewrites them. |

## 12. Cross-feature dependencies

- **script-lifecycle**: its spec (6.6) also introduces `Engine::systemFocus(bool)` to broadcast the
  per-object `onApplicationFocus` message, and `onApplicationPause` from `systemPause/systemResume`.
  There is one `Engine::systemFocus`: whichever feature merges first adds it; it must (a) be
  idempotent, (b) update `Engine::isFocused`, (c) fire `Engine::onFocus`, (d) broadcast
  `onApplicationFocus`. The ordering rules of decision 6 apply to both the event and the messages
  (focus-lost message before the pause message). Platform calls (table 6.3) belong to this feature;
  script-lifecycle adds only the Android one if it merges first.

- **safe-area**: owns the Game view device simulation (profiles, rotation) that this spec's
  orientation and language simulation extend; `Screen` class is created here and extended there
  (merge order: whichever lands first creates `engine/core/Screen.{h,cpp}`; the other extends it).
- **http**: `ExportFeatureScan` (permissions), Game view offline state used by reachability.
- **native-bridge**: both edit `MainActivity.java` (lifecycle forwarding vs orientation/haptics
  helpers) and `ViewController.m`: keep the forwarding hooks of native-bridge intact.
- **coroutines**: unscaled timers for `InternetChecker`-style polling; `onFocus` is a plain event.
- **audio-mixer**: may react to `onFocus` (the game mutes on focus loss) — no coupling needed.

## Implementation notes

Decisions taken while implementing (branch `gaps/platform-services`), where the spec was silent,
ambiguous or did not fit the code:

1. **`openURL` checks first, then the platform.** `System::openURL` is non-virtual: it applies the
   URL rules of decision 8 (`UrlRules::check`, scheme required, `javascript:`/`data:`/`file:`/
   `vbscript:` and control characters refused, an error logged) on every platform, then calls the
   protected virtual `platformOpenURL` (the engine's `platformFopen`/`platformLog` naming). The spec's
   `virtual bool openURL` would have let a platform skip the check.
2. **Desktop host code is shared.** `engine/platform/common/HostServices.h` with one implementation
   per desktop OS (`linux/HostServicesLinux.cpp`, `win/HostServicesWin.cpp`, `mac/HostServicesMac.mm`)
   serves both the desktop `System`s and the editor's `Platform` ("host" in table 6.7), so Play and
   a desktop build report the same OS string, model, locales, dpi and refresh rate. The Xcode macOS
   app (`JasperEngineApple.mm`) has its own Foundation/AppKit code (the Xcode project lists its sources
   by hand). Linux reads XRandR through `dlopen` (games need not link it); Windows links `ole32`,
   `uuid`, `advapi32`.
3. **Apple reachability without a framework.** `nw_path_monitor` needs Network.framework, which the
   hand-kept Xcode project does not link; `getifaddrs` gives the same answer Unity's does (a routable
   address on `en*` = local area network, on `pdp_ip*` = carrier) with no project change. macOS keeps
   the display awake with `NSProcessInfo beginActivityWithOptions:NSActivityIdleDisplaySleepDisabled`
   instead of an IOKit power assertion (same effect, no IOKit).
4. **iOS orientations: the app delegate allows all.** `application:supportedInterfaceOrientationsForWindow:`
   returns every orientation and the view controller returns Screen's mask, so the Info.plist list
   (Player settings, default first) only chooses the launch orientation, as in Unity: a project with a
   fixed default can still switch to `AutoRotation` at runtime. The spec's "intersect with the bundle's
   orientations and warn" would have broken that common pattern (`AutoRotate.cs`). `UIRequiresFullScreen`
   is set unless all four are allowed, as specified.
5. **Focus/pause order is enforced in `Engine`, not by each platform.** `systemPause()` on a focused
   app calls `systemFocus(false)` first, `systemFocus(true)` on a paused app calls `systemResume()`
   first, and both are idempotent. Every platform therefore produces decision 6's order whatever order
   its OS reports (Android's `APP_CMD_PAUSE`/`LOST_FOCUS` order varies by version; iOS sends
   `DidActivate` after `WillEnterForeground`). iOS resumes on `UISceneWillEnterForegroundNotification`
   and gains focus on `DidActivate` (6.3).
6. **Script messages `onApplicationFocus` / `onApplicationPause`.** The shared `Engine::systemFocus`
   (section 12) must broadcast `onApplicationFocus`; until `script-lifecycle` lands its `ScriptRuntime`
   broadcast, `Engine.cpp` has one small `broadcastApplicationMessage` that calls the method by name on
   the Lua scripts of the running scenes' active GameObjects (the existing `callLuaScriptMethod` path),
   for focus and pause alike, message before the engine event. `script-lifecycle` replaces that one
   function with its broadcast (C++ scripts included) when it merges.
7. **`systemOrientationChanged` runs at the end of every `systemViewChanged`.** That gives rule 7.5's
   order (`onCanvasChanged` then `onOrientationChanged`) on every platform with one call site; platforms
   call it directly only for a half turn, which changes no size (Android polls the display rotation a
   `DisplayListener` keeps, iOS calls it after the rotation transition, the web on
   `screen.orientation`'s `change`, the editor every frame). The first orientation seen is a baseline,
   not an event.
8. **Frame pacing details.** `System::getMaxSwapInterval()` (GL and D3D11: 4; Vulkan, Metal, the
   editor: 1) and `System::canDisableVSync()` (false for the macOS MTKView app) feed
   `computeFramePacing`, so "backends that can't present at an interval > 1" is a property of each
   platform's System, not a backend table. `Engine::getFramePacing()` is what the loops read for the
   limiter. The macOS Xcode app and iOS apply pacing as the MTKView's `preferredFramesPerSecond`; the
   native macOS backend skips frames of its 240 Hz timer (`FrameLimiter::isDue`), like the web's
   `requestAnimationFrame`. An unfocused editor never runs a Play limiter faster than its unfocused rate.
9. **Editor Play applies the Player settings before scripts run**, not in `finalizeStart` (which runs
   after the scripts' `init`): `Project::applyPresentationSettings()` is called where Play applies the
   canvas settings, so a script's `Engine.targetFrameRate = 30` in `init()` is not overwritten, as in an
   export whose generated `init()` sets them before the first scene loads. Play stop resets focus,
   pause, the frame rate and the simulated rotation (`Engine::resetApplicationState`,
   `Platform::endPlaySimulation`).
10. **Device simulation shared with `safe-area`.** This branch adds `editor/window/DeviceProfiles.{h,cpp}`
    (the safe-area spec's preset table, insets and cutouts included, plus each device's model, dpi and
    whether it turns upside down) and `engine/core/math/EdgeInsets.h` (header only; safe-area binds it to
    Lua), the Game view's Device menu (device list, Language submenu with Custom..., a Rotate button) and
    `GameViewSettings.device/orientation/language`. The overlay drawing, the Custom device, the insets
    pushed to `Screen` and the screenshot overlay are safe-area's. Simulated values: the device and
    language are Game view preferences (kept like the aspect, so a chosen phone stays chosen); what the
    game does to them (a script's rotation, simulated focus/pause) ends with the Play (rule 7.8).
11. **Rotate and MCP rotations stay within what the game allows.** The simulated device shows one
    orientation (no separate "device held sideways, screen locked" state), so Rotate, `configure_game_view
    orientation` and `send_input {type: "app", action: "orientation"}` refuse an orientation the playing
    game excludes, with the allowed list in the message. iPhone profiles never turn upside down.
12. **Headless openURL answers true.** In an MCP `--headless` run (or Linux without a display) the URL is
    recorded and `System.openURL` returns true, as a device would after handing the URL to a browser, so
    game flows behave as on device; `opened_urls[].launched` is false.
13. **`HapticFeedback`'s C++ enumerators** are `NotificationSuccess/Warning/Error` (X11's `Success`
    macro breaks any enumerator of that name in files that include Xlib); Lua keeps `HapticFeedback.Success`,
    `Warning`, `Error`.
14. **Not done here, by dependency.** The editor's "network offline" reachability belongs to `http`
    (its Game view network toggle is not merged); `get_play_state.platform` reports reachability-free
    state. The permission scan is a small name scan in `Exporter.cpp` (`ExportFeatureScan` is `http`'s and
    not merged): `System.vibrate` adds `VIBRATE`, `getInternetReachability` adds `ACCESS_NETWORK_STATE`,
    logged in the export log; `http` can fold it into its scanner.
15. **Project Settings has no undo.** The Project Settings dialog applies its edits on OK and Cancel drops
    them (the existing pattern of every other Player setting); the Game view settings are editor
    preferences, not undoable, as the aspect and scale were.
16. **Editor focus.** The editor window's OS focus reaches the game only as changes during a Play and
    never in headless MCP runs, so a Play always starts focused (Xvfb gives no focus to anything).
17. **A simulated pause stops the updates, not the tools.** `send_input {type: "app", action: "pause"}`
    is `Engine::systemPause` itself: the game stops updating (as on a device; the engine's frame skips
    its update while paused) and its audio pauses, while `send_input` keeps counting the editor's frames
    so a later `resume` step runs; `wait` stalled on a paused app says so.
