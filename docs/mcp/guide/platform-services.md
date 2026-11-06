# Platform services: URLs, language, frame rate, orientation, focus, device info

What a mobile game reads from Unity's `Application`, `SystemInfo`, `Screen` and `Handheld`, with
Unity's names where the engine style allows: platform and device functions on `System`, the
display on `Screen`, frame and lifecycle state and events on `Engine`, `Input.deviceOrientation`.
Everything works in the editor's Play mode (simulated where a desktop has nothing to report) and
in exported builds on Linux, Windows, macOS, Android, iOS and the web.

## Open a URL

```lua
if not System.openURL("https://play.google.com/store/apps/details?id=" .. System.getAppIdentifier()) then
    Log.warn("could not open the store")
end
```

- Any `scheme:` URL opens with the system's handler (browser, store app, `mailto:`, `market:`).
  `javascript:`, `data:`, `file:` and `vbscript:` URLs, URLs without a scheme and URLs with
  control characters are refused: `false` and an error in the log. Never raises.
- Web: browsers block popups outside a click or key press. A blocked URL opens on the next
  click, key or touch end on the page, and the call returns `false`. Call it from a button's
  handler to open at once.
- Editor Play: the host's browser opens and the Console logs `Open URL: <url>`. Headless MCP runs
  (`--headless`, no display) only record it: `get_play_state.platform.opened_urls` lists the last
  10. Going to the background afterwards pauses the game normally (`onFocus(false)`, `onPause`).

## Language and region

| Lua | Example | Notes |
|---|---|---|
| `System.getSystemLanguage()` | `SystemLanguage.Spanish` | Unity's `Application.systemLanguage`, same names and values; Chinese is `ChineseSimplified` / `ChineseTraditional` (Hant script or TW/HK/MO) |
| `System.getLanguageCode()` | `"es"` | ISO 639, lower case |
| `System.getRegionCode()` | `"MX"` | the platform's region setting first (a Spanish speaker set to the US gives `"US"`), then the locale's region; `""` when unknown |
| `System.getLocale()` | `"es-MX"` | BCP 47 |
| `System.getPreferredLanguages()` | `{"es-MX", "en-US"}` | most preferred first |

```lua
-- Localization.cs: if (Application.systemLanguage == SystemLanguage.Spanish)
if System.getSystemLanguage() == SystemLanguage.Spanish then self:useSpanish() end
```

Editor: the Game view's Device menu > Language (or `configure_game_view {language: "es-MX"}`)
simulates another language; `"system"` is the host's.

## Frame rate and vsync (Unity rules)

| Lua | Default | Meaning |
|---|---|---|
| `Engine.targetFrameRate` | `-1` | the platform's rate: 60 on Android and iOS, every animation frame on the web, as fast as vsync lets on desktops. 0 counts as -1, at most 1000 |
| `Engine.vSyncCount` | project VSync (1 or 0) | present every Nth vblank, 0..4 |

- Android / iOS: the game runs at `targetFrameRate` (Android frame pacing, iOS
  `preferredFramesPerSecond`), capped by the display; `vSyncCount` is ignored. 120 fps on iPhone
  needs Player > iOS > High Refresh Rate.
- Desktops and editor Play: with `vSyncCount > 0` `targetFrameRate` is ignored (as Unity); with
  `vSyncCount = 0` frames are capped at `targetFrameRate`.
- Web: `targetFrameRate` skips animation frames; `vSyncCount` is ignored.
- Kept across scene loads; time scale does not change it. Project Settings > Player > Target Frame
  Rate (`player.target_frame_rate`) sets the start value without code. `Engine.framerate` keeps
  measuring real frames; the Game view's Stats show `Target: 30 fps  VSync: 0`.

```lua
-- SimpleVsync.cs
Engine.targetFrameRate = vsyncOn and 60 or -1
```

## Orientation

| Lua | Kind | Meaning |
|---|---|---|
| `Screen.orientation` | rw `ScreenOrientation` | get: how the interface is shown (never `AutoRotation`); set: a fixed orientation, or `ScreenOrientation.AutoRotation` |
| `Screen.autorotateToPortrait`, `autorotateToPortraitUpsideDown`, `autorotateToLandscapeLeft`, `autorotateToLandscapeRight` | rw bool | where auto-rotation may turn; the last one cannot be turned off (warning) |
| `Input.deviceOrientation` | ro `DeviceOrientation` | how the device is held (FaceUp/FaceDown on iOS; `Unknown` on desktops, flat Android devices) |
| `Engine.onOrientationChanged` | event `(orientation)` | after the canvas changed (`onCanvasChanged` first), once per real change |

`LandscapeLeft` is the device turned counter-clockwise from portrait (its home button or
navigation on the right): Android `SCREEN_ORIENTATION_LANDSCAPE`, iOS
`UIInterfaceOrientationLandscapeRight`, the web's `landscape-primary`.

```lua
-- AutoRotate.cs, line by line
Screen.autorotateToPortrait = false
Screen.autorotateToPortraitUpsideDown = false
Screen.autorotateToLandscapeLeft = true
Screen.autorotateToLandscapeRight = true
Screen.orientation = ScreenOrientation.LandscapeLeft
-- later, once Input.deviceOrientation == DeviceOrientation.LandscapeLeft:
Screen.orientation = ScreenOrientation.AutoRotation
```

- Player settings (Project Settings > Player > Resolution and Presentation, shared by Android and
  iOS): **Default Orientation** (`player.default_orientation`) and **Allowed Orientations for Auto
  Rotation** (`player.allowed_orientations`). They write Android's `screenOrientation` and iOS's
  `UISupportedInterfaceOrientations` (+ `~ipad`, the default first: the launch orientation).
- iPad: `UIRequiresFullScreen` is written true only when the orientations are restricted (a fixed
  default, or fewer than four allowed): iPad Split View and Slide Over ignore orientation locks, so
  a restricted game runs full screen. Allow all four orientations (with Auto Rotation) to keep
  multitasking.
- Android keeps the user's rotation lock in auto-rotation (Unity's "Auto Rotation Behavior: User").
  It cannot limit rotation to some sets (e.g. portrait upside down and landscape left only): it then
  allows all four and warns once.
- Web: browsers lock the orientation only in fullscreen (asked again on entering it). Desktops: the
  orientation follows the window's shape (wider than high is `LandscapeLeft`); requests are stored.
- Editor: with a simulated device (Game view Device menu, `configure_game_view {device}`), a request
  the device's orientation does not fit turns the simulated device, like Unity's Device Simulator;
  `Rotate` cycles the orientations the game allows. Without a device the editor behaves as a desktop.

## Focus, pause, low memory

| Lua | Meaning |
|---|---|
| `Engine.isFocused` | Unity's `Application.isFocused`; `true` at start |
| `Engine.onFocus` `(hasFocus)` | the app is frontmost and gets input, or something covers it (a system dialog, the notification shade, another window) |
| `Engine.onPause`, `Engine.onResume` | the app goes to the background and comes back |
| `Engine.onLowMemory` | the system is short of memory (Android, iOS): free caches |
| `Device.onLowMemory` | the next frame, after the engine cleared its own caches (Android `onTrimMemory`, iOS, the web's heap heuristic; jasper://docs/device) |
| `Device.thermalState`, `Device.onThermalStateChanged` `(state)` | the platform's thermal state (`ThermalState`; Android, iOS and macOS report one) |
| script method `onApplicationFocus(focused)` / `onApplicationPause(paused)` | Unity's messages, called on every script of active GameObjects that defines them (Lua, and C++ `ScriptBase` overrides) |

Order, on every platform: leaving, `onFocus(false)` then `onPause`; coming back, `onResume` then
`onFocus(true)`. Pause and resume are idempotent (no second event, no second audio pause). A game
covered by a dialog keeps rendering on Android (as Unity); desktops never pause (they keep running
in the background) but report focus. Web: hiding the tab pauses, `focus`/`blur` of the window is
focus; CrazyGames ads and a lost WebGL context pause too (resumed once none of them holds).
A focus loss or a pause releases every held key and button (`Input.getKeyUp` / `getButtonUp` in
the next frame, as the input guide says) and a pause saves `PlayerPrefs`.

```lua
function Music:init() RegisterEngineEvent(self, "onFocus") end
function Music:onFocus(hasFocus) self.sound.volume = hasFocus and 1 or 0 end
```

## Hidden windows

| Lua | Meaning |
|---|---|
| `Engine.isVisible` | `false` while the game's window can't be seen; `true` at start (always on Android and iOS, whose pause is the signal) |
| `Engine.onVisibilityChanged` `(visible)` | the window was hidden or shown again |

A desktop game whose window can't be seen keeps running its logic **without drawing**: Windows when
minimized; Linux when unmapped or minimized by a window manager that keeps it mapped
(`_NET_WM_STATE_HIDDEN`, GNOME's mutter); macOS when its window is not visible at all (minimized,
on another Space, the app hidden, or fully covered by other windows). It then updates at the game's
own rate, at most 60 Hz: `Engine.targetFrameRate`, or the display rate divided by
`Engine.vSyncCount`. `onUpdate`, `onFixedUpdate`, coroutines, timers, audio and scene loads go on;
`onDraw` does not run and nothing reaches the GPU. Gamepads are released when the window hides (held
buttons go up, sticks and triggers rest) and are not read while hidden; a button still held when it
shows arrives as a new press. Keyboard and mouse come up through the focus loss. The log says
`Window hidden: updating at 60 Hz without drawing` and `Window visible: drawing again`.

A `--headless` run never hides. `--quit-after`, `--screenshot` and the ready signal count drawn
frames, so a hidden non-headless run does not advance them. The web pauses in a hidden tab (browsers
stop its frames) and reports `onVisibilityChanged(false)` before `onPause`, `(true)` after `onResume`.
Linux limitation: a compositing window manager that keeps a window on another workspace mapped
without `_NET_WM_STATE_HIDDEN` reports nothing, and the game draws there as before.

Editor: Play behaves the same while the editor window is minimized or hidden (macOS: also fully
covered): the game updates without drawing and its Game view keeps the last frame until the window
shows again.

```lua
function Music:init() RegisterEngineEvent(self, "onVisibilityChanged") end
function Music:onVisibilityChanged(visible) self.sound.volume = visible and 1 or 0.3 end
```

## Screen sleep

`Screen.sleepTimeout = SleepTimeout.NeverSleep` (-1) keeps the screen on; `SleepTimeout.SystemSetting`
(-2) lets it dim as the device is set to (seconds count as SystemSetting, as on Unity's mobiles).

- Android and iOS start from Player > Keep Screen On (`player.keep_screen_on`, default on: Android
  builds always kept the screen on). Android: the window flag; iOS: `idleTimerDisabled`.
- Desktops and the web start at `SystemSetting`, as Unity: the display stays awake only when the game
  sets `Screen.sleepTimeout = SleepTimeout.NeverSleep` itself (a video, a cutscene). macOS: a
  display-sleep activity; Windows: `SetThreadExecutionState`; web: the Screen Wake Lock (asked again
  when the page shows); Linux: nothing.
- Editor Play: as a desktop, or as the device when the Game view simulates one.

## Application and device information

| Lua | Unity | Notes |
|---|---|---|
| `System.getAppVersion()`, `getAppBuild()`, `getAppIdentifier()`, `getAppName()` | `Application.version`, versionCode / `CFBundleVersion`, `Application.identifier`, `Application.productName` | Android: the installed package; Apple: the bundle; elsewhere the Player settings (with the platform's overrides). `getAppBuild()` is an integer (`"1.2"` gives 1) |
| `System.getOperatingSystem()` | `SystemInfo.operatingSystem` | Unity's format: `Android OS 14 / API-34 (UP1A.231005.007/10754064)`, `iOS 17.1`, `Mac OS X 14.1.0`, `Windows 11 (10.0.22631) 64bit`, `Linux 6.8.0 Ubuntu 24.04 LTS`, `Web (Android)` |
| `System.getOperatingSystemVersion()`, `getAndroidApiLevel()` | | `"14"`, 34 (0 off Android) |
| `System.getDeviceModel()`, `getDeviceType()` | `SystemInfo.deviceModel`, `deviceType` | `"Google Pixel 8"`, `"iPhone16,1"`; `DeviceType.Handheld` / `Desktop` |
| `System.getSystemMemorySize()`, `getProcessorCount()` | `systemMemorySize`, `processorCount` | MiB (0 unknown), logical CPUs |
| `System.isEditor()` | `Application.isEditor` | true only in editor Play |
| `System.getInternetReachability()` | `Application.internetReachability` | says a network is there, not that a server answers. Android adds `ACCESS_NETWORK_STATE` to the export when a script calls it |
| `System.vibrate([ms])` | `Handheld.Vibrate` | 400 ms by default; iOS ignores the length; `false` without a vibrator. Android adds `VIBRATE` to the export when a script calls it |
| `System.hapticFeedback(HapticFeedback.X)` | (iOS feedback generators) | `Light`, `Medium`, `Heavy`, `Selection`, `Success`, `Warning`, `Error`; Android haptic constants, web vibration patterns, Force Touch trackpads |
| `Screen.dpi`, `Screen.refreshRate` | `Screen.dpi`, `Screen.currentResolution.refreshRateRatio` | 0 = unknown dpi. iOS dpi is an approximation (163 / 132 points per inch); the web measures the refresh rate over its first frames |

Device information is read once and cached; `getInternetReachability` is asked every time (cheap,
but not for every frame). The pixel size stays `System.getScreenWidth()/getScreenHeight()`.

## Agents (MCP)

- `send_input {events: [{type: "app", action: "focus_lost"}, {type: "app", action: "pause"},
  {type: "wait", ms: 500}, {type: "app", action: "resume"}]}`: the lifecycle as a device reports it
  (`focus_gained`, `low_memory` too). While "paused" the game does not update, as on a device;
  the input steps (a `resume` after a `wait`) keep going.
- `{type: "app", action: "orientation", orientation: "landscape_left"}` turns the simulated device
  (needs `configure_game_view {device}`; refused when the game allows another orientation only).
- `configure_game_view {device: "iphone_15_pro", orientation: "portrait", language: "es-MX"}`;
  `devices` in its reply lists the profiles. Simulation is editor only, never in builds; a script's
  rotation of the device ends with the Play.
- `get_play_state` while playing: `platform: {focused, paused, visible, hidden_update_fps, orientation,
  requested_orientation, target_frame_rate, vsync_count, language, system_language, device, opened_urls,
  vibrations, haptics}`.
- Project settings: `player.default_orientation`, `player.allowed_orientations`,
  `player.keep_screen_on`, `player.target_frame_rate` (`android.orientation` and
  `android.keep_screen_on` were renamed to these; older project.yaml files migrate on load).

## Upgrading: what changes when an older project is exported

Projects made before these settings migrate when opened (`android.orientation` becomes the shared
Player orientation, `android.keepScreenOn` is dropped) and save `presentation:` in project.yaml.
On the next export:

- **Android orientation.** No orientation set (`unspecified`) now exports `screenOrientation="user"`:
  portrait and both landscapes, following the user's rotation lock (tablets no longer turn upside
  down unless Portrait Upside Down is allowed). `sensorPortrait`, `sensorLandscape` and `fullSensor`
  become `userPortrait`, `userLandscape` and `fullUser`: the same orientations, now respecting the
  rotation lock (Unity's Auto Rotation Behavior: User). `portrait` and `landscape` are unchanged.
- **Android configChanges** now lists locale, uiMode, density, font scale and layout direction too:
  the activity is no longer restarted when the user changes the language or dark mode (the game
  reads `System.getLocale()` again instead).
- **Android rendering** goes on while the app is visible but not focused (a permission dialog, the
  notification shade), as Unity; `Engine.onFocus(false)` tells the game.
- **Android permissions**: `VIBRATE` and `ACCESS_NETWORK_STATE` are added when a script calls
  `System.vibrate` or `System.getInternetReachability`.
- **Android screen sleep** is unchanged: on by default (Keep Screen On); a project that had
  `android.keepScreenOn: false` keeps the screen on as before (that setting had no effect).
- **iOS orientations** follow the shared setting, migrated from the Android one: a project that had
  `android.orientation: landscape` exports a landscape-only iOS app. The default list is portrait and
  both landscapes as before, portrait now first (the launch orientation).
- **iOS iPad**: `UIRequiresFullScreen` is true for restricted orientations (the default set has three),
  so iPad Split View is off unless all four orientations are allowed.
- **Hidden desktop windows** (Linux, macOS) update without drawing, as Windows builds already did:
  `onDraw` stops while the window can't be seen. Windows' hidden rate follows the game's frame rate
  (at most 60 Hz) instead of a fixed 60 Hz.
- **Frame rate**: `Engine.targetFrameRate` starts at Player > Target Frame Rate (-1: 60 on mobile as
  before); desktop and web exports start unsynchronized only when VSync is off, as before.
