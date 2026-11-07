# Platform lifecycle: hidden windows update without drawing, clean launches, web audio and keys, Android Gradle (design)

Date 2026-11-04. Branch `dc/platform-lifecycle`. Program: Doriax catch-up, ledger
[`docs/doriax-catchup/LEDGER.md`](../../doriax-catchup/LEDGER.md) (#17).

## 1. Goals

1. A desktop game whose window cannot be seen (minimized, unmapped, hidden by the window manager,
   fully covered on macOS) keeps running its game logic at a steady rate **without drawing or
   presenting**, on Windows, Linux and macOS (AppKit and Xcode builds), on every graphics backend:
   no stall in a present that never completes, no GPU work nobody sees. Its gamepads are suspended
   (held input released, nothing read) and come back on show.
2. One rule for the hidden update rate on every desktop platform: the game's own frame rate
   (`Engine.targetFrameRate`, or the display rate over `Engine.vSyncCount`), at most 60 Hz.
3. Games and agents can see the state: `Engine.isVisible`, `Engine.onVisibilityChanged(visible)`,
   a log line per change, `get_play_state.platform.visible`, and an MCP simulation of it.
4. The editor's Play keeps updating (without drawing) while the editor window is minimized or
   hidden, on all three desktop OSes, like an exported game; the Linux editor sees minimize by
   window managers that do not unmap (mutter); the macOS editor gets the idle and pacing rules the
   other backends have.
5. Update-only frames are safe on Metal: they never free a resource the GPU still uses.
6. `Engine::systemViewDestroyed()` is safe when no view was ever loaded (failed macOS launches,
   a second destroy): it does nothing.
7. Web games start their audio on the first key press (and on any other user gesture the browser
   counts), even when the key belongs to an IME, and retry until the audio really runs.
8. The web key map produces every engine key a browser can report: Meta (`MetaLeft`,
   `MetaRight`), the context-menu key, the ISO `<>` key, F13-F24, the right-hand modifiers, and
   the legacy `keyCode` fallback without its wrong entries.
9. The Android Gradle template and the Gradle files the export generates build without
   deprecation warnings, and CI fails when a template change brings one back.

## 2. Non-goals

- Hidden browser tabs: browsers stop `requestAnimationFrame`, and the web game already pauses
  (`Engine::systemPause`, `JasperEngineWeb::updatePause`). Unchanged; the web only reports
  `onVisibilityChanged` (D4).
- Android and iOS visibility: the OS pauses the app (`onPause`); `Engine.isVisible` stays `true`
  there (documented).
- Windows occlusion other than minimize (another virtual desktop, a window covering the game,
  `DXGI_STATUS_OCCLUDED`): Windows keeps today's `IsIconic` test. Win32 has no occlusion
  notification; a DXGI-status path is a separate change.
- X11 occlusion under a compositing window manager (a window on another workspace that stays
  mapped and has no `_NET_WM_STATE_HIDDEN`): X11 reports nothing for it. Documented limitation.
- A `runInBackground` switch (Unity's `Application.runInBackground = false`): desktops keep
  running in the background, as decided by `2026-10-31-platform-services-design.md` (non-goals).
- The editor's own gamepad readers (`BackendLinux.cpp` `pollGamepads`, the other backends'):
  unchanged. Held editor input is already released on focus loss (`Platform::forwardHostFocus`
  -> `Engine::systemFocus(false)` -> `systemFocusChanged(false)` -> `Input::releaseAll`).
- `screenshot` while the editor window is hidden does not draw offscreen: it fails at once with a
  reason (D7). Drawing editor views without the window's swapchain is a separate change.

## 3. Users and stories

- Player on Ubuntu (GNOME, mutter) minimizes a rhythm game to answer a message: the music keeps
  in time with the game clock, the fans stay quiet, restoring shows the current frame at once.
  Today (read) the game either keeps drawing unseen or blocks in `vkAcquireNextImageKHR` with an
  infinite timeout and stops updating.
- Player on macOS covers a city builder with a browser window: the simulation goes on at 60 Hz,
  the GPU is idle; uncovering resumes drawing.
- Player holding a gamepad trigger alt-tabs and minimizes: the trigger is released in the game
  at once; on restore, a trigger still held is pressed again (a fresh press, as on Windows today).
- Keyboard-only web player presses Space on the title screen: the music starts. Today it stays
  silent until the first click or touch (miniaudio unlocks on `touchend`/`click` only).
- Web player on a Mac presses Cmd+S in a game that binds `Super+S`: the game sees
  `D_KEY_LEFT_SUPER` (today: Chrome reports `MetaLeft`, the map knows only `OSLeft`, the legacy
  fallback happens to give 91; Firefox reports `keyCode` 224, which maps to nothing).
- Lua: `RegisterEngineEvent(self, "onVisibilityChanged")` and
  `function M:onVisibilityChanged(visible) self.music.volume = visible and 1 or 0.3 end`.
- Developer minimizes the editor during a long Play test: the game keeps running; restoring the
  editor shows where it got to. Today Play freezes (`EditorFrame.cpp:164`).
- Agent: `send_input {events: [{type: "app", action: "hide"}, {type: "wait", ms: 2000},
  {type: "app", action: "show"}]}` checks a game's `onVisibilityChanged` handler and that its
  timers ran while hidden; `get_play_state` reports `platform.visible` and
  `platform.hidden_update_fps`; `screenshot` on a minimized editor answers "The editor window is
  minimized or hidden..." at once instead of a 30 s timeout.
- Agent shipping Android: `tools/build.sh android` fails when a template edit uses a deprecated
  Gradle form, naming the file and line.

## 4. Findings

All by reading `main` at `b4f7816ea` (nothing run). Doriax commits are references.

### 4.1 Linux games (Doriax `277b04ca6`)

- `engine/platform/linux/JasperEngineLinux.cpp:390-404`: the loop always does `beginFrame` /
  `Engine::systemDraw` / `endFrame` and polls the pads; `processEvents` (`:260-310`) has no
  `MapNotify`, `UnmapNotify` or `PropertyNotify` case.
- `engine/platform/linux/WindowLinux.cpp:181-185`: the event mask lacks `PropertyChangeMask`, so
  `_NET_WM_STATE` changes (mutter's minimize sets `_NET_WM_STATE_HIDDEN` without unmapping) never
  arrive.
- `engine/platform/common/VulkanContext.cpp:606-607`: `vkAcquireNextImageKHR(..., UINT64_MAX, ...)`;
  a hidden window may block here forever (driver and compositor dependent), or draw unseen.
- `engine/platform/linux/GamepadLinux.h:27-30`: only `shutdown` and `poll`; no release, the pads
  are read while hidden. `Pad` has no `connected`/`name`/`guid` (`fd < 0` means no pad), so a
  closed fd cannot be told from a free slot.
- `Engine::systemUpdate` exists (`engine/core/Engine.cpp:2117-2119`, `runFrame(false)`); its only
  platform caller is Windows (`engine/platform/win/JasperEngineWin.cpp:494-508`).
- Headless runs are never mapped and must keep drawing (`JasperEngineLinux.cpp:360-364`;
  `--screenshot` and `--quit-after` count drawn frames, `Engine.cpp:2457-2466`).

### 4.2 Linux editor

- `editor/backend/BackendLinux.cpp:1207-1211` `isWindowMinimized`: `map_state != IsViewable` only,
  one `XGetWindowAttributes` round trip per call; mutter's minimize is not seen. The editor's
  windows already select `PropertyChangeMask` (`:71`).

### 4.3 macOS games (Doriax `5e472cced`, `7ba2a5cb4`, `171d0a9a6`)

- `engine/platform/mac/JasperEngineMac.mm:154-157`: a fixed 240 Hz timer; `renderFrame`
  (`:160-176`) always draws; the delegate (`:192-227`) has no `windowDidChangeOcclusionState:`.
- `engine/platform/apple/JasperEngineGameController.mm:137`: `handleState` has no suspension; no
  `setSuspended:` in the `.h` or `.mm`.
- Xcode build: `engine/platform/apple/macos/ViewController.m` has no occlusion observer and
  `Renderer.mm:86-89` `drawInMTKView` always draws. The frame-rate part of `7ba2a5cb4` is
  already here (`ViewController.m:48-51,74-78,85-90`, `Renderer.mm:138-153`).
- Failed launches (`JasperEngineMac.mm:94-97`, `:118-123`, `:126-130`) call `[NSApp terminate:]`
  before `Engine::systemViewLoaded` (`:140`); `applicationWillTerminate:` (`:229-243`) then calls
  `Engine::systemViewDestroyed()` unguarded -> `SystemRender::shutdown`
  (`engine/core/render/SystemRender.cpp:283`) -> `SokolSystem::shutdown`
  (`engine/renders/sokol/SokolSystem.cpp:642-664`) -> `sg_shutdown` with no `sg_setup`
  (`SOKOL_ASSERT(_sg.valid)`, `sokol_gfx.h:25030`). Whether that crashes or asserts depends on the
  build type; either is wrong. The web (`JasperEngineWeb.cpp:781-783`), Android
  (`NativeEngine.cpp:882-886`) and the test support (`GpuEnvironment.cpp:142-145`) each guard the
  call themselves: the rule lives in four callers instead of the engine.
- `fb5add261` (Xcode crash on minimize): already ported (no `viewDidDisappear`;
  `ViewController.m:42-46`). Skip.

### 4.4 Metal update-only frames (`171d0a9a6`)

- `Engine::runFrame(false)` still calls `SystemRender::commit()` (`Engine.cpp:2417-2420`), i.e.
  `sg_commit`. On Metal `_sg_mtl_commit` (`sokol_gfx.h:16496-16516`) garbage-collects the release
  queue by frame index (`_sg_mtl_release_resource`, `:15243-15261`: safe after
  `SG_NUM_INFLIGHT_FRAMES + 1` commits), but only `_sg_mtl_begin_pass` waits on the in-flight
  semaphore (`:16446`). Commits without passes advance the index without waiting, so a resource
  destroyed just before or during update-only frames can be freed while the GPU still reads it.
  Doriax waits once, when the window hides; update-only frames that do submit work (S32 warm
  draws, `Engine::updateGpuCreation` runs in every own frame, `Engine.cpp:2125-2133`) are not
  covered by that.
- Vulkan collects deletions behind its frame fences (`_sg_vk_acquire_frame_command_buffers`,
  `sokol_gfx.h:20940-20962`); D3D11 and GL release through the driver: no race there.

### 4.5 Windows games (shared gap noted on `71d8b27cc`)

- `engine/platform/win/JasperEngineWin.cpp:53`: `MINIMIZED_UPDATE_PERIOD = 1/60`, ignores
  `Engine.targetFrameRate` and `vSyncCount`; the deadline code (`:494-508`) is the same arithmetic
  Doriax copied to Linux and macOS. Pads are released in `WM_SIZE` (`:115-118`).

### 4.6 Editor while minimized (shared gaps noted on `71d8b27cc`)

- `editor/EditorFrame.cpp:164,182-189`: a minimized window runs no `engineRender`, so a playing
  game freezes; same on macOS (`editor/backend/BackendMac.mm:1010-1013,1055,1069`), where
  "minimized" includes "fully covered" (`occlusionState`), so covering the editor freezes Play.
- `BackendMac.mm:1081-1088`: `io.WantTextInput || ImGui::IsAnyItemActive()` counts as activity, so
  a focused text field keeps the macOS editor at full rate (EditorFrame idles it,
  `EditorFrame.cpp:100-101`); `:1115-1120` sleeps "the rest of the period" instead of
  EditorFrame's deadline pacing (`:243-253`).
- `editor/mcp/tools/ViewTools.cpp:831-848`: `screenshot` waits up to `timeout_seconds` (30) for a
  frame a hidden editor never draws.

### 4.7 Web audio (Doriax `189982697`)

- `engine/platform/emscripten/JasperEngineWeb.cpp:907-1000` `key_callback` resumes nothing;
  miniaudio unlocks on `touchend` and `click` only and removes its listeners after the first one
  even when `resume()` fails (`engine/libs/soloud/src/backend/miniaudio/miniaudio.h:42329-42352`).
- `key_callback` returns early for `keyCode == 229` (IME, soft keyboards; `:946-951`), so a
  resume placed in the key callback (Doriax's) misses those keys.
- `engine/platform/emscripten/WebVideoSource.cpp:35-60` already has a gesture list (with
  `keydown`) for the video's own `AudioContext`, not miniaudio's.

### 4.8 Web key map (Doriax `f9d271489`)

- `JasperEngineWeb.cpp:1183-1293` `jasperengine_input`: `OSLeft`, `OSRight`, `Menu` only;
  `MetaLeft`, `MetaRight`, `ContextMenu`, `IntlBackslash`, `F13`-`F24` map to 0. `BracketRight`
  is already fixed (`:1230`).
- `:1295-1395` `jasperengine_legacy_input` (used when `code` is empty, e.g. Android soft
  keyboards, `:925-931`): `8` gives `D_KEY_DELETE` (8 is Backspace), `93` gives 0 (ContextMenu),
  no `224` (Firefox Meta), no `221` (`]`), no Firefox's `59`/`61`/`173`, no `226` (`<>`), no
  `124`-`135` (F13-F24), and `16`/`17`/`18` ignore `location` (right Shift/Ctrl/Alt come out left).
- The maps live in an Emscripten-only file, so no unit test can reach them.

### 4.9 Android Gradle (Doriax `4d0862350`)

- `engine/workspaces/androidstudio/app/build.gradle:48` `multiDexEnabled true`, `:107`
  `prefab true`, `:127` `namespace '...'`; `jasperbridge/build.gradle` `namespace '...'`;
  `editor/util/NativeExportAndroid.cpp:381` writes `namespace '...'` into the generated
  `nativeplugins/build.gradle` (asserted literally by `tools/mcp/tests/test_native_bridge.py:303`).
  Gradle 9.6 deprecates the space-assignment of properties.
- `gradle.properties` keeps `android.nonFinalResIds=false` and `android.prefabVersion=2.0.0`
  (obsolete under AGP 9.4.1); `org.gradle.warning.mode=all` prints the warnings.
- The exporter's markers (`editor/Exporter.cpp:4056-4075`, `NativeExportAndroid.cpp:323-328`)
  match `compileSdk 36`, `minSdkVersion 21`, `targetSdkVersion 36`, `versionCode 1`,
  `versionName "1.0"`, `applicationId "..."`: they must change in lockstep with any line they
  match. Which lines Gradle actually reports was not run here.
- Removing `android.nonFinalResIds=false` makes the app module's R ids non-final: no `switch` or
  annotation on `R.` ids in `engine/platform/android/java` (grep); library modules
  (`jasperbridge`, `nativeplugins`) already have non-final ids.

## 5. Product decisions

- **D1. Hidden desktop windows update without drawing.** Windows, Linux and macOS games (and the
  editor's Play, D7) call `Engine::systemUpdate()` instead of drawing while hidden. Why: a game's
  clock, timers, network, music sync and save timers keep going (what players and Unity's default
  `Run In Background` expect), and nothing waits on a present that cannot complete.
- **D2. Hidden rate = the game's own rate, at most 60 Hz.**
  `PlatformRules::hiddenUpdateFps(targetFrameRate, vSyncCount, refreshRate)`: `vSyncCount > 0`:
  `min(60, refresh / vSyncCount)`; else `targetFrameRate > 0`: `min(60, targetFrameRate)`; else 60;
  clamped to `[1, 60]` (unknown refresh = 60, as `computeFramePacing`). Why: a game that asks for
  30 must not run faster unseen; 60 Hz is enough for game logic and cheap. Replaces Windows' fixed
  60 (finding 4.5).
- **D3. What "hidden" means.** Windows: `IsIconic`. Linux: unmapped, or `_NET_WM_STATE_HIDDEN`
  (mutter and other window managers minimize without unmapping). macOS: `occlusionState` without
  `NSWindowOcclusionStateVisible` (minimized, another Space, app hidden, fully covered). A
  `--headless` run never hides. Why: these are the states each OS reports; headless runs must
  render their frames.
- **D4. Visibility is engine state.** `Engine::systemVisibilityChanged(bool)`, `Engine::isVisible()`,
  `Engine::onVisibilityChanged`, Lua `Engine.isVisible` and `Engine.onVisibilityChanged(visible)`,
  log lines "Window hidden: updating at N Hz without drawing" / "Window visible: drawing again".
  Reported by the desktop games, the editor's Play (real window state and the MCP simulation) and
  the web page (`visibilitychange`, before `onPause` when hiding, after `onResume` when showing).
  Not reported on Android/iOS (their pause is the signal). `onDraw` does not run while hidden.
  Why: scripts and agents need to know frames are not drawn; Unity has no such API, so the name
  follows our `isFocused`/`onFocus` pair.
- **D5. Gamepads are suspended while hidden.** On hide: every held button goes up and every axis
  to rest (triggers -1) with events; nothing is read; Linux closes the `js` fds. On show: the pads
  are read again and what is still held arrives as new presses; Linux reopens each node and
  refuses one that now holds another pad (name or GUID differs: a disconnect, and the next scan
  connects the new pad). Why: matches Windows today (`GamepadWin::release`), no input applied to a
  game nobody watches, no stale state on return.
- **D6. Metal waits for the GPU before every update-only frame, in the engine.**
  `Engine::systemUpdate()` calls `SystemRender::waitGpuIdle()` before `runFrame(false)`: on Metal
  an empty command buffer is committed on sokol's queue and waited for; on every other backend it
  does nothing. Why: fixes the class (any update-only frame, after drawn frames or after S32 warm
  draws), once, for every caller (Windows, Linux, macOS games, the Xcode build, the editor), instead
  of Doriax's single wait at the hide moment in two platform files. Cost is Metal-only and
  hidden-only (section 8).
- **D7. The editor's Play updates while the editor is hidden.** Linux and Windows through
  `EditorFrame`, macOS through `BackendMac`'s loop: no `engineRender`, the game gets
  `Engine::systemUpdate` at the D2 rate. `screenshot` (and the other tools that wait for a drawn
  view) fail at once while the window is hidden, with a reason. `send_input {type: "app", action:
  "hide"|"show"}` simulates it while the window stays visible: the game updates without drawing,
  the Game view keeps its last frame. `editor_status.window.visible` and
  `get_play_state.platform.visible`/`hidden_update_fps` report it. Why: the editor should behave
  like the shipped game; agents need the same path to test handlers.
- **D8. Linux and macOS editor parity.** The Linux editor treats `_NET_WM_STATE_HIDDEN` as
  minimized (cached from `PropertyNotify`, no extra round trip). The macOS editor idles with a
  focused text field and paces unfocused/hidden frames by deadline, as `EditorFrame`. Why: the
  shared gaps found on `71d8b27cc`; small, in the code this feature touches.
- **D9. `systemViewDestroyed` without a view is a no-op.** In `Engine::systemViewDestroyed()`:
  `if (!viewLoaded) return;` before anything else (no pool shutdown requests, no
  `onViewDestroyed`, no `SystemRender::shutdown`). The platform callers' own guards stay (harmless).
  Why: fixes the macOS failed-launch crash and any double destroy, in one place, instead of
  Doriax's guard in the macOS delegate only.
- **D10. Web audio unlock is page-level and persistent.** A capture-phase listener on `window` for
  `keydown`, `mousedown`, `pointerdown`, `pointerup`, `touchend` and `click`, installed at
  startup, resumes every miniaudio device the engine started whose `AudioContext` is not
  `running` (`suspended`, Safari's `interrupted`). It stays installed: a gesture the browser does
  not count (Escape) leaves the context suspended and the next one retries; a context the OS
  suspends later (iOS call) comes back on the next gesture. Why: Doriax's call inside
  `key_callback` misses IME keys (the 229 early return) and keys pressed with the canvas
  unfocused, and never retries after miniaudio drops its own listeners.
- **D11. Web key map: complete, testable.** The map moves to a pure header
  (`engine/platform/emscripten/WebKeyMap.h`) with one table of `KeyboardEvent.code` names; the
  modern names are added and the old ones kept (`OSLeft`/`OSRight`/`Menu`, for older browsers,
  where Doriax replaced them); the legacy `keyCode` table is fixed and takes `location`. Why: the
  `BracketRight` bug Doriax fixed is one of a class (a table nobody can test); a unit test over the
  whole table stops the next one.
- **D12. Android Gradle: fix what Gradle and AGP report, guard it in CI.** Every deprecation the
  Android build reports for the template (`app`, `jasperbridge`, root `build.gradle`,
  `gradle.properties`, `settings.gradle`) and for the generated `nativeplugins/build.gradle` is
  fixed; the two obsolete `gradle.properties` entries go. `tools/build.sh android` fails when the
  Gradle log has a deprecation that names one of our files. Not `org.gradle.warning.mode=fail` in
  the template: a third-party plugin's deprecation would then break users' exports. Exporter
  markers change only together with a line they match. Why: exports open in Android Studio
  without warnings, and the next Gradle removal does not break exports by surprise.

Rejected: Doriax's Xcode `updateVisibility` call in `viewDidLoad` ("starts hidden", the window is
still nil there: every launch first pauses the view and starts the update timer) -> checked in
`viewDidAppear` and on each occlusion change instead. Doriax's `setSuspended:` without a started
guard (with `_slots` nil it sends trigger axis moves for eight absent pads) -> guarded.

## 6. Design

### 6.1 Engine (`engine/core`)

`Engine.h` / `Engine.cpp`, next to `systemFocus`:

```cpp
// the platform's window can (not) be seen: minimized, unmapped or hidden by the window manager,
// fully covered (macOS), a hidden page (web). Desktop platforms then call systemUpdate at
// getHiddenUpdateFps() instead of systemDraw; the web pauses as before
static void systemVisibilityChanged(bool visible);
static bool isVisible();
// PlatformRules::hiddenUpdateFps of the game's targetFrameRate, vSyncCount and the display rate
static double getHiddenUpdateFps();
static FunctionSubscribe<void(bool)> onVisibilityChanged;
```

- `static std::atomic<bool> visible` (true); `systemInit` and `resetApplicationState`
  (`Engine.cpp:1830-1838`, `:2764-2778`) set it to true without an event.
- `systemVisibilityChanged(v)`: returns when unchanged; sets it; `Log::print("Window hidden:
  updating at %g Hz without drawing", getHiddenUpdateFps())` or `"Window visible: drawing again"`;
  `onVisibilityChanged.call(v)`. No script message broadcast (no Unity counterpart; scripts use
  `RegisterEngineEvent`).
- `systemUpdate()`: `SystemRender::waitGpuIdle(); runFrame(false);` (D6).
- `systemViewDestroyed()`: `if (!viewLoaded) return;` first (D9), with a comment naming the
  failed-launch and double-destroy cases.
- `getHiddenUpdateFps()`: `PlatformRules::hiddenUpdateFps(targetFrameRate, vSyncCount,
  System::hasInstance() ? System::instance().getRefreshRate() : 0.0)`.

`engine/core/util/PlatformRules.{h,cpp}`: `JASPERENGINE_API double hiddenUpdateFps(int
targetFrameRate, int vSyncCount, double refreshRate);` (D2).

`engine/core/util/FrameLimiter.{h,cpp}`: the hidden clock is a `FrameLimiter` (deadline based,
first call due at once, no burst after a stall: `isDue`, `FrameLimiter.cpp:84-101`). Add:

```cpp
// seconds until the next frame is due (0: due now, or not started)
double secondsUntilDue() const;
// the same with the time given (tests)
bool isDueAt(double now, double fps, double tolerance = 0.002);
double secondsUntilDueAt(double now) const;
```

`isDue` and `secondsUntilDue` call the `At` forms with `now()`.

`engine/core/render/SystemRender.{h,cpp}`: `static void waitGpuIdle();` -> `SokolSystem::waitGpuIdle()`
-> `sgj_wait_gpu_idle()`, a new function in `engine/libs/sokol/sokol.cpp` (compiled as
Objective-C++ on Apple, `engine/libs/sokol/CMakeLists.txt:56`), declared in a new
`engine/libs/sokol/sokol_wait.h`: `if (!sg_isvalid()) return;` then on `SOKOL_METAL`
`id<MTLCommandBuffer> b = [(__bridge id<MTLCommandQueue>)sg_mtl_command_queue() commandBuffer];
[b commit]; [b waitUntilCompleted];`; nothing on other backends. No change to `sokol_gfx.h`; a
line in `engine/libs/sokol/PATCHES.md` lists the added function. The commit at the start of an
update-only frame is legal: no frame command buffer is open between frames.

### 6.2 Linux games

`engine/platform/linux/X11Visibility.{h,cpp}` (new; shared by games, the editor and the tests):

```cpp
// Whether an X11 top-level window can be seen: mapped and not _NET_WM_STATE_HIDDEN. Fed with the
// window's events (StructureNotifyMask | PropertyChangeMask selected on it).
class X11Visibility {
public:
    // mapped: the state to assume until the first Map/UnmapNotify (a game assumes the window
    // it asked to show is shown)
    void attach(Display* display, Window window, bool mapped);
    // MapNotify, UnmapNotify, PropertyNotify of _NET_WM_STATE on the window: true when
    // isHidden() changed
    bool handleEvent(const XEvent& event);
    bool isHidden() const;
    bool isWmHidden() const;
    // one XGetWindowProperty of _NET_WM_STATE
    static bool hasNetWmState(Display* display, Window window, Atom state);
};
```

- `WindowLinux.cpp:181-185`: add `PropertyChangeMask`. `WindowLinux::isMaximized` uses
  `X11Visibility::hasNetWmState` for both atoms (same behaviour, one helper).
- `JasperEngineLinux.cpp`: after `WindowLinux::show()` (non-headless only),
  `visibility.attach(display, handle, true)`. `processEvents` passes every event that reaches the
  `switch` to `visibility.handleEvent` (input, selection and close events are consumed earlier;
  none of them is a visibility event). New loop:

```cpp
FrameLimiter hiddenClock;
bool hidden = false;
while (!shouldClose) {
    // nothing to present while hidden: sleep until an X event or the next update
    if (hidden) waitForEvents(hiddenClock.secondsUntilDue());
    processEvents();
    if (shouldClose) break;
    const bool nowHidden = !platform.headless && visibility.isHidden();
    if (nowHidden != hidden) {
        hidden = nowHidden;
        // what is held comes up before the game hears of it
        if (hidden) { gamepads.release(); hiddenClock.reset(); }
        Engine::systemVisibilityChanged(!hidden);
    }
    if (!hidden) { /* today's poll, beginFrame, systemDraw, endFrame, frameLimiter.wait */ continue; }
    if (hiddenClock.isDue(Engine::getHiddenUpdateFps(), 0.0)) Engine::systemUpdate();
}
```

  `waitForEvents(timeout)`: returns when `timeout <= 0` or `XPending(display)`; else
  `poll({ConnectionNumber(display), POLLIN}, 1, ceil(timeout * 1000))`.
- `GamepadLinux.{h,cpp}`: `Pad` gains `bool connected`, `std::string name`, `std::string guid`;
  `connect` fills them (name through a `deviceName(fd)` helper, `JSIOCGNAME`, default "Gamepad");
  `disconnect` tests `connected`; `scan` connects only slots that are not `connected`. New
  `void release()`: for each connected pad, `setButton(..., false)` for every button and
  `setAxis(..., rest)` for every axis (rest -1 for axes 4 and 5), then close the fd, `fd = -1`,
  hats, `rawButtons` and `rawAxes` reset (so the reopen's `JS_EVENT_INIT` states arrive as new
  input). `poll` reopens `fd < 0 && connected` pads first: `reopen(id, pad)` opens the node, reads
  the name and GUID; a different pad or a missing node -> `disconnect(id)` (event; the next scan
  connects a new pad there). A seam for the tests: `explicit GamepadLinux(std::string devicePattern
  = "/dev/input/js*", std::string sysClassRoot = "/sys/class/input")`; `readGuid` and the node
  path use them.

### 6.3 Linux editor

`editor/backend/BackendLinux.cpp`: `backend->netWmStateHidden = atom("_NET_WM_STATE_HIDDEN")`;
`NativeWindow` gains `bool wmHidden` (read with `X11Visibility::hasNetWmState` when the window is
created or adopted, updated on each `PropertyNotify` of `_NET_WM_STATE` for that window in the
event switch near `:1765`); `isWindowMinimized` returns `attributes.map_state != IsViewable ||
window->wmHidden` (the map-state round trip stays as today, so viewports created but not yet
shown keep their current answer).

### 6.4 macOS games (AppKit, `engine/platform/mac/JasperEngineMac.mm`)

- `bool hidden = false;` in the anonymous namespace; `constexpr NSTimeInterval FRAME_PERIOD =
  1.0 / 240.0;` replaces the literal at `:154`.
- `- (void)startFrameTimer:(NSTimeInterval)period` (invalidates and re-adds `self.frameTimer` in
  `NSRunLoopCommonModes`); `applicationDidFinishLaunching:` ends with `[self
  startFrameTimer:FRAME_PERIOD]; [self updateVisibility];` (a window opened behind others gets no
  occlusion change).
- `- (void)windowDidChangeOcclusionState:(NSNotification*)n` (the delegate is the window's
  delegate, `:85`) -> `[self updateVisibility]`.
- `- (void)updateVisibility`: returns when `!self.frameTimer` (quitting) or
  `platformConfig().headless`; `hidden = (window.occlusionState & NSWindowOcclusionStateVisible)
  == 0` with `window = (__bridge NSWindow*)WindowMac::nativeWindow()`; on a change:
  `[JasperEngineGameController setSuspended:hidden]`, `Engine::systemVisibilityChanged(!hidden)`,
  `[self startFrameTimer:hidden ? 1.0 / Engine::getHiddenUpdateFps() : FRAME_PERIOD]`.
- `renderFrame`: the hidden branch comes before the limiter check (`:163`): GL makes its context
  current (`[gameView makeContextCurrent]`, as the drawing path), then `Engine::systemUpdate()`
  (which waits for Metal itself, D6); when `getHiddenUpdateFps()` changed since the timer started
  (a script set `targetFrameRate`), the timer restarts with the new period.
- `applicationWillTerminate:` unchanged (D9 makes it safe).

`engine/platform/apple/JasperEngineGameController.{h,mm}`: `+ (void)setSuspended:(BOOL)suspended`
(Doriax `7ba2a5cb4` end state): stores the flag; returns when unchanged or when `start` has not run
(`_slots == nil`: headless, no pads); suspending releases every button and rests every axis
(triggers -1) through `processButton` / `processAxis`; resuming calls `handleState` for each
connected slot. `handleState` returns while suspended. Connect/disconnect notifications keep
working while suspended (a pad connected while hidden gets its connect event; its state comes on
resume).

### 6.5 macOS Xcode build (`engine/platform/apple/macos/ViewController.m`, `Renderer.{h,mm}`)

- `Renderer`: `+ (void)updateGame` (`Engine::systemUpdate()`), `+ (void)visibilityChanged:(BOOL)
  visible` (`Engine::systemVisibilityChanged`), `+ (double)hiddenUpdateFps`.
- `ViewController`: ivar `NSTimer* _updateTimer`; observes
  `NSWindowDidChangeOcclusionStateNotification` (object nil; the handler ignores other windows)
  -> `updateVisibility`; `viewDidAppear` also calls `updateVisibility` (window known there).
  `updateVisibility`: returns while `self.view.window` is nil (no window yet: nothing to decide);
  `hidden = (self.view.window.occlusionState & NSWindowOcclusionStateVisible) == 0`; on a change,
  tracked in a `BOOL _hidden` ivar: `setSuspended:`, `_view.paused =
  hidden`, `[Renderer visibilityChanged:!hidden]`, start (`timerWithTimeInterval:1.0 / [Renderer
  hiddenUpdateFps]` in common modes, block `[Renderer updateGame]`) or invalidate `_updateTimer`.
- `destroyView` (`:80-83`): first `removeObserver:self`, invalidate `_updateTimer`, `_view.delegate
  = nil`, then `[_renderer destroyView]` (nothing calls into a torn-down engine; `171d0a9a6`).

### 6.6 Windows games (`engine/platform/win/JasperEngineWin.cpp`)

`MINIMIZED_UPDATE_PERIOD` and `nextUpdate` go; `FrameLimiter hiddenClock; bool hidden = false;`.
In the loop: `waitForMessage(hiddenClock.secondsUntilDue())` while hidden; after the message pump
`nowHidden = IsIconic(handle)`; a change releases the pads (moved here from `WM_SIZE`, `:116-118`,
so there is one place), resets the clock and calls `Engine::systemVisibilityChanged(!hidden)`;
hidden frames run `Engine::systemUpdate()` when `hiddenClock.isDue(Engine::getHiddenUpdateFps(),
0.0)`. Not buildable on this host: kept compiling by reading (`FrameLimiter.h` is already included
for `frameLimiter`).

### 6.7 Editor (`editor/`)

- `App` (`editor/App.{h,cpp}`):
  - `setWindowVisible(bool)` / `isWindowVisible()` (set every loop iteration by `EditorFrame::run`
    and `BackendMac`).
  - `activateRunningGame(SceneProject&) -> bool viewChanged`: the activation block extracted from
    `renderRunningScene` (`App.cpp:4075-4097`, `activateGame`, the `isPlayedSceneReplaced` swap,
    the runtime layers, `lastActivatedScene/Game`), used by `renderRunningScene` and by:
  - `bool runHiddenGameFrame()`: returns false unless a play session's scene is `PLAYING`, the
    simulation is not held and no project is loading; returns false unless
    `hiddenGameClock.isDue(Engine::getHiddenUpdateFps(), 0.0)`; runs `ScriptStatus::get().update()`
    and `ScriptReloadService::get().update()` (scripts saved meanwhile swap in at the frame
    boundary, as in `engineRender`), `activateRunningGame`, then `Engine::systemUpdate()`. No
    `Platform::setSizes`, no Scene view, no Step, no Recorder capture.
  - `bool isHiddenGameRunning() const` and `idleWaitSeconds` (`App.cpp:4695-4698`) also takes
    `hiddenGameClock.secondsUntilDue()` while a hidden game runs, so idle waits wake for it.
  - In `engineRender`'s view loop, the hosted running scene (`gameWindow->hostsScene`) calls
    `runHiddenGameFrame()` instead of `renderRunningScene` while `Platform::isGameHiddenSimulated()`.
- `EditorFrame::run` (`editor/EditorFrame.cpp`): `state.minimized` keeps its name and now means "the
  window cannot be seen" (Linux includes `wmHidden`); `app->setWindowVisible(!state.minimized)`;
  `Platform::forwardHostVisibility(!state.minimized, playSessionActive)` next to
  `forwardHostFocus`; in the `!frameReady` branch, after `processMainThreadTasks()`,
  `if (state.minimized) app->runHiddenGameFrame();`. The minimized pacing (`:243-253`) uses
  `1 / Engine::getHiddenUpdateFps()` as its period while `app->isHiddenGameRunning()`, else
  `state.framePeriod`.
- `BackendMac.mm`: the same calls with `visible` (`:1011-1013`):
  `app.setWindowVisible(visible)`, `Platform::forwardHostVisibility(visible, playSessionActive)`,
  `else { app.processMainThreadTasks(); if (!visible) app.runHiddenGameFrame(); }` at `:1069-1070`;
  activity uses `(ImGui::IsAnyItemActive() && !io.WantTextInput)` instead of
  `io.WantTextInput || ImGui::IsAnyItemActive()` (`:1085`); the sleep at `:1115-1120` becomes
  EditorFrame's deadline pacing (a `frameDeadline` advanced by the period, reset when more than a
  period off), with `(!focused || !visible)` as its condition and the hidden period while a hidden
  game runs. Not buildable here; kept compiling by reading.
- `editor/Platform.{h,cpp}`: `forwardHostVisibility(bool visible, bool playSessionActive)`:
  like `forwardHostFocus` but **not** skipped for headless editors (an Xvfb window is mapped; a
  test unmaps it on purpose); forwards `visible && !simulatedHidden` to
  `Engine::systemVisibilityChanged` on changes during a session; `setGameHiddenSimulated(bool)` /
  `isGameHiddenSimulated()`, cleared where the simulated orientation ends with the Play, and
  `Engine::resetApplicationState` (called at Play start and stop, `Project.cpp:2837,10457`) puts
  `Engine::isVisible` back to true.
- MCP:
  - `send_input` (`editor/mcp/tools/PlayTools.cpp:3430-3470`): `app` actions `hide` and `show` ->
    `Platform::setGameHiddenSimulated(true/false)`; the error text and the tool description
    (`:4364`) list them.
  - `platformJson` (`PlayTools.cpp:671-695`): `"visible": Engine::isVisible()`,
    `"hidden_update_fps": Engine::getHiddenUpdateFps()`.
  - `editor_status` (`editor/mcp/tools/CoreTools.cpp:705-740`): `"window": {"visible":
    app.isWindowVisible()}`.
  - `screenshot` / `w3::captureView` (`ViewTools.cpp`): with `!app.isWindowVisible()`, fail at
    once: "The editor window is minimized or hidden, so nothing is drawn (the game keeps updating).
    Restore the window, or run the editor with mcp --headless." A `game` capture while
    `isGameHiddenSimulated()`: "The game is hidden (send_input app hide): it updates without
    drawing. Send {type: \"app\", action: \"show\"} first." `view: "editor"` while the window is
    hidden fails with the first message.

### 6.8 Web (`engine/platform/emscripten`)

- Audio (D10): `EM_JS(void, jasper_audio_unlock_install, (), {...})` in `JasperEngineWeb.cpp`,
  called from `JasperEngineWeb::init` (`:408`) before the project starts:

```js
if (Module.jasperAudioUnlock) return;
var resume = function() {
    var ma = window.miniaudio;
    if (!ma || !ma.devices) return;
    for (var i = 0; i < ma.devices.length; ++i) {
        var d = ma.devices[i];
        // only devices the engine started; a stopped one stays stopped
        if (d && d.webaudio && d.state === ma.device_state.started &&
            d.webaudio.state !== 'running' && d.webaudio.state !== 'closed')
            d.webaudio.resume().catch(function() {});
    }
};
Module.jasperAudioUnlock = resume;
['keydown', 'mousedown', 'pointerdown', 'pointerup', 'touchend', 'click'].forEach(function(t) {
    window.addEventListener(t, resume, {capture: true, passive: true});
});
```

  Nothing in the vendored miniaudio changes. `WebVideoSource.cpp` keeps its own unlock (it also
  unmutes and plays elements).
- Visibility: `visibility_callback` (`:547-559`) calls `Engine::systemVisibilityChanged(!pageHidden)`
  when the project started: before `updatePause()` when hiding, after it when showing.
- Keys (D11): new `engine/platform/emscripten/WebKeyMap.h`, header only, no Emscripten includes:

```cpp
namespace jasperengine::web {
    struct KeyCodeName { std::string_view code; int key; };
    // every KeyboardEvent.code the engine has a key for (UI Events KeyboardEvent code values),
    // plus the names older browsers used (OSLeft, OSRight, Menu)
    inline constexpr KeyCodeName KEY_CODES[] = { ... };
    // 0: no engine key
    inline int keyFromCode(std::string_view code);
    // the legacy which/keyCode, for events without a code (Android soft keyboards, old
    // browsers); location: KeyboardEvent.location (2 = right)
    inline int keyFromKeyCode(int keyCode, int location);
}
```

  Table additions: `MetaLeft` -> `D_KEY_LEFT_SUPER`, `MetaRight` -> `D_KEY_RIGHT_SUPER`,
  `ContextMenu` -> `D_KEY_MENU`, `IntlBackslash` -> `D_KEY_WORLD_2` (GLFW's mapping of the ISO
  key), `F13`..`F24` -> `D_KEY_F13`..`D_KEY_F24`; `OSLeft`, `OSRight`, `Menu` kept. Legacy fixes:
  `8` -> `D_KEY_BACKSPACE`, `93` -> `D_KEY_MENU`, `224` -> `D_KEY_LEFT_SUPER`, `221` ->
  `D_KEY_RIGHT_BRACKET`, `59` -> `D_KEY_SEMICOLON`, `61` -> `D_KEY_EQUAL`, `173` -> `D_KEY_MINUS`,
  `226` -> `D_KEY_WORLD_2`, `225` -> `D_KEY_RIGHT_ALT` (AltGraph in Firefox), `124`..`135` ->
  `D_KEY_F13`..`D_KEY_F24`; with `location == 2`: `16`/`17`/`18`/`91`/`224` give the right-hand
  key; `13` with `location == 3` gives `D_KEY_KP_ENTER`. `key_callback` (`:925-931`) calls
  `web::keyFromCode(e->code)`, then `web::keyFromKeyCode(e->which, e->location)`, then
  `keyFromKeyCode(e->keyCode, e->location)`; `jasperengine_input` / `jasperengine_legacy_input` and
  their declarations (`JasperEngineWeb.h:79-80`) go.

### 6.9 Android (`engine/workspaces/androidstudio`, `editor/util/NativeExportAndroid.cpp`)

- The implementer builds (`tools/build.sh android`, Gradle's `--warning-mode all` is already on)
  and converts every reported line. Expected from the reading and Doriax: `app/build.gradle`
  `multiDexEnabled = true`, `prefab = true`, `namespace = 'com.jaspergameengine.jasperengine'`;
  `jasperbridge/build.gradle` `namespace = 'com.jaspergameengine.bridge'`;
  `NativeExportAndroid.cpp:381` writes `namespace = 'com.jaspergameengine.nativeplugins'`;
  `gradle.properties` loses `android.nonFinalResIds=false` and `android.prefabVersion=2.0.0`;
  the root `build.gradle`'s `clean` task (`rootProject.buildDir`) if reported
  (`layout.buildDirectory`). A reported line the exporter matches (`Exporter.cpp:4056-4075`,
  `NativeExportAndroid.cpp:323-328,363-365`) changes with its marker and replacement in the same
  commit. The final list goes into the commit message.
- `tools/ci/check_gradle_warnings.py LOG` (standard library): exit 1 listing each deprecation
  line that names a file under `engine/workspaces/androidstudio` or the generated
  `nativeplugins` module (Gradle prints `Build file '.../app/build.gradle': line 48: ...`), and
  each AGP option warning about a `gradle.properties` entry ("The option setting 'android.X=...'
  is deprecated|obsolete|experimental"); deprecations from third-party plugins are printed as
  notes, not failures. `tools/build.sh android` tees Gradle's output to `$APK_DIR/gradle.log` and
  runs the check after `assembleDebug`; `tools/players/android_player.sh` does the same.

## 7. Behaviour rules and edge cases

1. `--headless` (every desktop platform) never enters the hidden mode, whatever the window state
   (Linux: never mapped; macOS: an alpha-0 window behind others).
2. A window that starts hidden (macOS opened behind others; Linux minimized by the window manager
   at map time) gets `onVisibilityChanged(false)` within its first loop iterations, after
   `systemViewLoaded`; the game never drew, so the ready signal waits for the first drawn frame.
3. Showing again draws on the next loop iteration; the first delta is at most one hidden period;
   the visible frame limiter restarts on its own (far behind).
4. While hidden: `onUpdate`, `onFixedUpdate`, `onPostUpdate`, coroutines (also
   `waitForEndOfFrame`: `ScriptScheduler::endOfFramePhase` runs in update-only frames), timers,
   scene loading (`SceneManager::updateLoading`), audio and the network run; `onDraw`, one-shot
   scenes (thumbnails) and the ready signal wait for a drawn frame.
5. `--quit-after`, `--screenshot` and `--after-ready` count drawn frames: a hidden non-headless
   run does not advance them (documented in `docs/shipping.md`; automation uses `--headless`).
6. `Engine.targetFrameRate` / `vSyncCount` set while hidden take effect on the next update (the
   clock reads the rate each call; macOS restarts its timer).
7. Pads: held at hide -> up events (and axes to rest) before `onVisibilityChanged(false)`; still
   held at show -> down events on the first poll. Unplugged while hidden -> disconnect on show
   (Linux reopen fails; Apple's notification arrives while suspended). Another pad on the same Linux
   node -> disconnect, then a connect on the next scan.
8. Keyboard and mouse: unchanged; minimizing also takes the focus, which releases them.
9. Metal: every update-only frame starts with a GPU wait (D6), also the first after drawn frames.
10. Linux reparenting window managers may send an Unmap/Map pair at start: a short hidden period
    with two log lines; harmless.
11. Linux, compositing window manager, window on another workspace that stays mapped without
    `_NET_WM_STATE_HIDDEN`: not detected (non-goal); it draws as today.
12. Closing a hidden window (taskbar menu) wakes the `poll` (an X event) and quits normally.
13. Editor: a paused or held Play gets no hidden updates (nothing advances anyway). A Play started
    while the editor is hidden updates once it reaches `PLAYING`. MCP waits that count game
    frames (`wait`, `send_input` gaps) advance with hidden updates (`gameFrames` counts updates).
14. Editor: a Recorder session (`wantsContinuousFrames`) records drawn frames only; while hidden it
    records nothing (its frames are drawn frames).
15. Editor: `send_input app hide` while the window is visible: the Game view keeps its last frame,
    other editor views draw; `show` or Play stop ends it; a real minimize during the simulation keeps
    the game hidden until both end.
16. `Engine::systemViewDestroyed()` with no view: nothing happens (no `onViewDestroyed`, no pool
    shutdown requests, no `SystemRender::shutdown`); `systemShutdown` after it works as on the web
    before the project started.
17. Web: a key with `keyCode` 229 (IME) still unlocks audio (the listener is independent of
    `key_callback`); Escape does not count as activation in browsers, the next key does; a device the
    engine stopped is not resumed; `closed` contexts are left alone.
18. Web keys: an event with an empty `code` and `keyCode` 0 gives 0 (ignored, as today).
19. Gradle: users' native plugin sources and their dependencies' deprecations are not checked (only
    our files); an export of a project still builds when they warn.

## 8. Performance and cost

- Visible game, every platform: one `bool` compare per loop iteration. Linux: `PropertyNotify`
  events only when a property of the game window changes (title, state: rare); no per-frame X
  round trip (state cached from events). macOS: one notification per occlusion change.
- Hidden game: CPU for game logic at <= 60 Hz, the loop asleep between updates (`poll`,
  `MsgWaitForMultipleObjectsEx`, an `NSTimer`); no GPU work, no present. Metal adds one empty
  command buffer commit and wait per update (tens of microseconds on an idle GPU, <= 60 per second,
  only while hidden).
- Linux editor: no new round trip (`wmHidden` cached); the existing `XGetWindowAttributes` per
  frame stays.
- Editor while hidden and playing: the loop wakes at <= 60 Hz instead of the display rate.
- Web audio unlock: six passive listeners; each user gesture walks the miniaudio device list
  (one or two devices). Nothing per frame.
- Web key map: a table lookup per key event (no per-frame cost); the header adds nothing to
  non-web binaries (inline functions used only by the web platform file and the test).
- `Engine.onVisibilityChanged`: nothing unless subscribed. `SystemRender::waitGpuIdle` off Metal:
  a call that returns.

## 9. Compatibility

- No file format, scene, project.yaml or asset change; no migration.
- Lua: additions only (`Engine.isVisible`, `Engine.onVisibilityChanged`).
- Behaviour change, documented in `docs/mcp/guide/platform-services.md` (Upgrading): Linux and
  macOS games no longer draw while hidden, so `onDraw` stops there (Windows already behaved so);
  Windows' hidden rate follows the game's rate instead of a fixed 60 Hz.
- Android: R ids of the app module become non-final (no engine code depends on it; plugin modules
  already had them). Exports regenerate the Gradle files from the template on each export; the
  exporter's markers change with the template in one commit.
- `JasperEngineWeb::jasperengine_input` / `jasperengine_legacy_input` were static members of a
  platform class (`JasperEngineWeb.h:79-80`): removed; `grep` shows no other caller.

## 10. Tests

Red controls: each test marked **red** fails on `main` (or on the extraction commit for the key
map) and passes with the change; the implementer records the red run's command and output.

`jasperengine-core-tests` (`tools/build.sh unit-tests`, `UNIT_SUITE=core`):

- `PlatformRules.HiddenUpdateFps`: (0,0,60)->60; target 30 -> 30; target 144 -> 60; vSync 2 at
  60 Hz -> 30; vSync 1 at 144 Hz -> 60; vSync over target (vSync 2, target 120, 60 Hz) -> 30;
  unknown refresh -> 60; target 1 -> 1.
- `FrameLimiter.HiddenClock*` (time given with `isDueAt`/`secondsUntilDueAt`): due at once after
  reset; 60 Hz over one simulated second gives 60 updates; a late update is made up by the next;
  more than a period behind restarts from now (no burst); `secondsUntilDueAt` counts down to 0.
- `EngineVisibility.ChangeEventsOnceAndResets`: starts visible; `systemVisibilityChanged(false)`
  twice -> one event, `isVisible()` false, one "Window hidden" log line (LogCapture);
  `resetApplicationState()` -> visible, no event.
- **red** `EngineLifecycle.ViewDestroyedWithoutViewIsIgnored` (gtest death-test style,
  `EXPECT_EXIT` like `engine/tests/core/jobs/JobSystemTest.cpp`): in the child, if a view is loaded
  destroy it once, then subscribe a counter to `Engine::onViewDestroyed` and call
  `systemViewDestroyed()` again; exit 0 when the counter is 0. On `main` the handler runs (exit 1)
  or `sg_shutdown` asserts (abort).
- `X11Visibility.*` (Linux, the tests' Xvfb; compiled where `JASPER_TEST_SUPPORT_GPU` is on; its
  own `XOpenDisplay`, not the `WindowLinux` singleton the GPU suites use):
  `UnmapAndMapAreHiddenAndShown`, `NetWmStateHiddenIsHidden` (the test sets and clears
  `_NET_WM_STATE` = `[_NET_WM_STATE_HIDDEN]` itself, as mutter would), `OtherPropertiesChangeNothing`,
  `HasNetWmStateReadsMaximized`.
- `GamepadLinux.*` (Linux; `GamepadLinux.cpp` and `GamepadDB.cpp` added to the test support's Linux
  sources; a temporary directory with a FIFO `js0` the test keeps open for writing and fake
  `sys/class/input/js0/device/id/{bustype,vendor,product,version}` files):
  `ReleaseLetsGoOfHeldInput` (a written button press -> `Input` reports it held; `release()` ->
  released with an up event, the trigger axes at -1), `ShowReportsStillHeldAsNewPress` (after
  release, the next `poll` reopens; a press written then arrives as a down), `ReopenRefusesAnotherPad`
  (vendor file changed while released -> disconnect event).
- **red** `WebKeyMap.*` (`engine/tests/core/input/WebKeyMapTest.cpp`, includes `WebKeyMap.h`):
  `ModernMetaAndContextMenu`, `LegacyNamesStillMap`, `LegacyKeyCodes` (8, 93, 221, 224, 59, 61,
  173, 225, 226, 124-135), `LocationPicksRightModifiers`, `EveryEngineKeyHasACodeName` (every
  `D_KEY_*` except a listed few with no browser code: `D_KEY_UNKNOWN`, `D_KEY_WORLD_1`, `D_KEY_F25`),
  `NoCodeNameTwice`. Red control: commit 1 moves today's two maps unchanged into the header and adds
  the tests (the modern, legacy, location and completeness tests fail: record it); commit 2 fixes the
  table.

`jasperengine-editor-tests`:

- `PlatformServices.VisibilityEventReachesLua` (`editor/tests/PlatformServicesTest.cpp`, beside the
  `onFocus` tests): a Lua script with `RegisterEngineEvent(self, "onVisibilityChanged")` records the
  values; `Engine::systemVisibilityChanged(false)` then `(true)` -> `{false, true}`;
  `Engine.isVisible` reads them.

MCP end to end (`tools/mcp/tests/test_hidden_window.py`, `EDITOR = {"xvfb": "harness", ...}`,
`xdotool` and `xprop` from the linux image; a probe script counts `onUpdate` and `onDraw` and logs
`onVisibilityChanged`):

- **red** `test_editor_unmapped_keeps_play_updating`: Play; `xdotool windowunmap` on the editor
  window; over 1 s the update count rises by >= 30 and the draw count by 0; `get_play_state`
  `platform.visible` false, `time.frames` advancing; `editor_status.window.visible` false;
  `screenshot` fails within 2 s with the hidden message; `windowmap` -> draws resume, visible true,
  the probe logged false then true. On `main` the updates stop (game frames flat).
- **red** `test_editor_net_wm_hidden_is_hidden`: `xprop -id W -f _NET_WM_STATE 32a -set
  _NET_WM_STATE _NET_WM_STATE_HIDDEN` -> as above (draws stop, updates go on); removing the
  property -> visible. On `main` the editor keeps drawing.
- `test_send_input_hide_show`: `send_input app hide`, wait 1 s, `app show`: the probe saw false
  then true, draws stopped and updates went on in between; `screenshot view=game` during `hide`
  fails with the simulation message; Play stop leaves `Engine.isVisible` true for the next Play.
- **red** `test_prebuilt_player_hidden` (slow, `JASPER_SLOW_BUILD=1`, the run's prebuilt
  `glcore-release` player as `test_prebuilt_audio.py`; also `vulkan-release` when the build cache has
  it): the project built and run **non-headless** on the harness Xvfb; after the ready line,
  `windowunmap` -> within 2 s the log has "Window hidden: updating at 60 Hz without drawing", the
  probe's draw count stops and its update count rises ~60/s; `windowmap` -> "Window visible"; the
  same with `_NET_WM_STATE_HIDDEN`; the probe quits the game (`Engine.quit(0)`) after the second
  show; exit code 0. On `main` the draw count keeps rising while unmapped (glcore) and no line is
  logged.
- `tools/mcp/tests/test_native_bridge.py:303`: the generated namespace line's new literal.

Web gate (`WEB_PROJECT=web-keys-audio tools/build.sh web`, then `GATE=web-keys-audio tools/build.sh
web-gate`); new test project `tools/test-projects/web-keys-audio` (`main.cpp`, `lua/main.lua` that
plays a looping `assets/tone.wav` at start and prints `[gate] key <code>` on each key down) and
baseline `tools/web/gates/web-keys-audio.json`. `tools/web/browser_gate.py` learns baseline keys
`chrome_flags` (extra Chrome flags: `--autoplay-policy=user-gesture-required`, so the headless
browser blocks audio as a desktop one does), `eval_before_input` / `eval_before_input_expect`, and
`keys` (CDP `Input.dispatchKeyEvent` `rawKeyDown`/`keyUp` pairs with `code`, `key`,
`windowsVirtualKeyCode`, `location`, each followed by `after_key_seconds`, default 0.5); order: run,
`eval_before_input`, `click`s, `keys`, `eval`.

- **red** audio: `eval_before_input` = the started miniaudio contexts' states, expect `suspended`;
  first key `KeyA`; `eval` expects `running`. On `main` it stays `suspended`.
- **red** keys: `MetaLeft`, `MetaRight`, `ContextMenu`, an event with no `code` and keyCode 224, one
  with no `code` and keyCode 8 -> console lines `[gate] key 343`, `347`, `348`, `343`, `259` (ordered).

Android:

- `tools/ci/test_check_gradle_warnings.py` (tool self-test, standard library): fixture logs with a
  template DSL deprecation (fails, names file:line), an obsolete `android.*` option (fails), a
  third-party plugin deprecation (passes with a note), a clean log (passes).
- **red** the check over `tools/build.sh android` on `main` (log kept as evidence) fails listing
  the lines; on the branch it passes.

Not testable on this host (Windows, macOS AppKit and Xcode, the macOS editor): specified precisely
and kept compiling by reading the surrounding code; the implementer states it in the report.

## 11. Acceptance criteria

1. A non-headless Linux glcore prebuilt player under Xvfb, unmapped: logs "Window hidden: updating
   at 60 Hz without drawing", draws nothing, updates ~60 times per second; mapped again: logs
   "Window visible: drawing again" and draws (`test_prebuilt_player_hidden`).
2. The same with `_NET_WM_STATE_HIDDEN` set and cleared on the mapped window.
3. `--headless` runs are unaffected: `tools/ci/test_player_cli.py --only linux` and the existing
   headless MCP tests pass.
4. A hidden Linux game's pads are released with events, not read, reopened on show; another pad on
   the node disconnects (`GamepadLinux.*`).
5. The editor's Play keeps updating without drawing while the editor window is unmapped or
   `_NET_WM_STATE_HIDDEN`, and draws again when shown (`test_editor_*`).
6. `send_input app hide/show` drives `Engine.onVisibilityChanged` and the update-only path;
   `get_play_state.platform` has `visible` and `hidden_update_fps`; `editor_status.window.visible`
   exists.
7. `screenshot` on a hidden editor fails within 2 s with the hidden message.
8. `Engine::systemViewDestroyed()` with no loaded view does nothing (`EngineLifecycle.*`).
9. `PlatformRules::hiddenUpdateFps` follows D2 (`PlatformRules.HiddenUpdateFps`); Windows, Linux,
   macOS and the editor use it through `Engine::getHiddenUpdateFps()` (by reading:
   `MINIMIZED_UPDATE_PERIOD` is gone and no hidden loop has a rate constant of its own).
10. `Engine::systemUpdate` calls `SystemRender::waitGpuIdle()` first; `sgj_wait_gpu_idle` waits on
    Metal and does nothing elsewhere (reading; Metal not runnable here).
11. macOS AppKit and Xcode games implement 6.4/6.5 (occlusion -> update-only timer, pads suspended,
    `destroyView` order) and the macOS editor 6.7 (reading).
12. A web build unlocks audio on the first key press and maps Meta, ContextMenu and the legacy codes
    (gate `web-keys-audio`), and `WebKeyMap.*` passes.
13. `tools/build.sh android` passes `check_gradle_warnings.py` (no deprecation naming our files),
    and an Android export from the editor (`test_build.py`'s Android case, `test_native_bridge.py`)
    still passes.
14. Docs updated: `docs/mcp/guide/platform-services.md` (hidden windows, the agents section,
    Upgrading), `docs/mcp/guide/lua-api.md` (`isVisible`, `onVisibilityChanged`),
    `docs/unity-glossary.md` (`Application.runInBackground` row), `docs/shipping.md` (drawn frames and
    hidden windows), `docs/mcp/guide/audio.md:210` (any gesture, keys included),
    `docs/mcp/guide/input.md` (web keys), `docs/testing.md` (the gate, the Gradle check, the new
    suites); `tools/mcp/gen_tool_docs.py` and `tool_budget.py --check` pass.
15. `tools/check-changed` passes on the branch; the red runs of section 10 are recorded.

## 12. Files likely touched

- Engine: `engine/core/Engine.{h,cpp}`, `engine/core/util/PlatformRules.{h,cpp}`,
  `engine/core/util/FrameLimiter.{h,cpp}`, `engine/core/render/SystemRender.{h,cpp}`,
  `engine/renders/sokol/SokolSystem.{h,cpp}`, `engine/libs/sokol/sokol.cpp`,
  `engine/libs/sokol/sokol_wait.h` (new), `engine/libs/sokol/PATCHES.md`,
  `engine/core/script/binding/CoreClassesLua.cpp`.
- Linux: `engine/platform/linux/JasperEngineLinux.cpp`, `WindowLinux.{h,cpp}`,
  `GamepadLinux.{h,cpp}`, `X11Visibility.{h,cpp}` (new), `engine/CMakeLists.txt` (sources, unity
  skip list), `CMakeLists.txt` (editor backend sources).
- macOS/Apple: `engine/platform/mac/JasperEngineMac.mm`,
  `engine/platform/apple/JasperEngineGameController.{h,mm}`, `engine/platform/apple/Renderer.{h,mm}`,
  `engine/platform/apple/macos/ViewController.m`.
- Windows: `engine/platform/win/JasperEngineWin.cpp`.
- Web: `engine/platform/emscripten/JasperEngineWeb.{h,cpp}`, `WebKeyMap.h` (new),
  `tools/web/browser_gate.py`, `tools/web/gates/web-keys-audio.json` (new),
  `tools/test-projects/web-keys-audio/` (new).
- Editor: `editor/EditorFrame.{h,cpp}`, `editor/App.{h,cpp}` (`renderRunningScene` activation
  extracted, `engineRender`, `idleWaitSeconds`), `editor/Platform.{h,cpp}`,
  `editor/backend/BackendLinux.cpp`, `editor/backend/BackendMac.mm`,
  `editor/mcp/tools/PlayTools.cpp`, `CoreTools.cpp`, `ViewTools.cpp`, `docs/mcp/TOOLS.md`.
- Android: `engine/workspaces/androidstudio/{build.gradle,gradle.properties,app/build.gradle,
  jasperbridge/build.gradle}`, `editor/util/NativeExportAndroid.cpp`, possibly
  `editor/Exporter.cpp:4056-4075`, `tools/build.sh` (android target), `tools/players/android_player.sh`,
  `tools/ci/check_gradle_warnings.py` and its test (new).
- Tests: `engine/tests/core/{util,input,platform}/...` (new files), `engine/tests/core/CMakeLists.txt`,
  `engine/tests/support/CMakeLists.txt`, `editor/tests/PlatformServicesTest.cpp`,
  `tools/mcp/tests/test_hidden_window.py` (new), `tools/mcp/tests/test_native_bridge.py`.
- Docs: listed in AC14.

## 13. Cross-feature dependencies

- `mobile-shipping`: also edits `app/build.gradle` (AdMob dependency and `java-admob` source dir),
  `editor/Exporter.cpp`'s Android section (AdMob line removal next to the markers),
  `engine/platform/apple/*` and the Xcode project. Textual conflicts only; whichever lands second
  re-runs `check_gradle_warnings.py`.
- `build-toolchain`: exec bits on `gradlew`, Android preset backends (`app/build.gradle`
  `jasperBackend`), `tools/build.sh`. Textual conflicts in the same files.
- `scene-integrity`: changes the Play-time activation in `editor/App.cpp` (Scene view when the
  played scene is replaced, `:3936-3944`, `:4081-4089`) that this feature extracts into
  `activateRunningGame`. Land one first; the second moves its change into (or out of) the extracted
  function. Highest-risk merge of this feature.
- `ui-components` and `runtime-core`: edit `engine/core/Engine.cpp` (input dispatch, gamepad stick
  hand-back, subscriptions at Stop); separate functions. `ui-components` maps Android
  `DPAD_CENTER`, not the web map.
- `draw-perf`: edits `Engine::runFrame` and `SokolSystem` (state caching, test-only timers); this
  feature only touches `systemUpdate`, `systemViewDestroyed` and adds `SokolSystem::waitGpuIdle`.
- `water-reflections` ("waves pause with the game"): reads the shader clock latched in
  `runFrame`; update-only frames latch it too (own frames), nothing to coordinate beyond
  `Engine.cpp` merges.
- `agent-authoring`: MCP docs and `tools/mcp/budget.json`; regenerate `TOOLS.md` and re-run
  `tool_budget.py --check` after merging.
- `unicode-text`: edits `BackendLinux.cpp` (CJK fonts) and `BackendMac.mm`; separate regions.
- `export-pipeline`: `editor/Exporter.cpp` (other sections) and web outputs; no shared lines.
- No dependency on `mesh-memory`, `sprite-batching`, `ssr-probes`, `water-boats`, `scripting`,
  `project-window`, `code-editor-git`, `render-correctness`.

## 14. Deviations

1. **The Metal wait's name and declaration.** `sokol.cpp` defines `jasperengine_sg_wait_gpu_idle()` (the
   prefix of its other extension functions), declared `extern "C"` in `SokolSystem.cpp` as they are; no
   `sokol_wait.h` and no `PATCHES.md` line (that file lists changes to `sokol_gfx.h`, which is unchanged).
2. **Generated projects.** `editor/Generator.cpp` lists `linux/X11Visibility.cpp` among the platform sources
   a project built outside an export compiles (`GeneratedProject.PlatformSourcesListEveryBackendFile`
   requires every platform source there).
3. **Editor pacing while a hidden game runs.** Instead of the hidden period as the deadline loop's period,
   the loop (`EditorFrame::run`, `BackendMac`) sleeps until the hidden clock's next update
   (`App::hiddenGameSecondsUntilDue`): two independent deadlines a period apart would drop every other
   update. `runHiddenGameFrame` counts a wake-up up to 1 ms early as due.
4. **`forwardHostVisibility`** forwards `windowVisible && !simulatedHidden` every loop iteration of a
   Play session (`Engine::systemVisibilityChanged` is idempotent) instead of tracking a known state as
   `forwardHostFocus` does; Play start and stop reset the engine's state through
   `Engine::resetApplicationState`, and the simulation through `Platform::resetPlayRecords` /
   `endPlaySimulation`.
5. **A hidden activation of the game** (another scene's view drew last) marks the scene for a view
   change (`pendingResizeScene`), so the next drawn frame of the game calls `systemViewChanged`.
6. **Test files.** The Lua test is `PlatformVisibilityTest.VisibilityEventReachesLua` in its own file
   (`editor/tests/PlatformVisibilityTest.cpp`, on the Lua fixture of `ScriptDataTestSupport.h`), not in
   `PlatformServicesTest.cpp`, which has no Lua state. The player end-to-end test is its own file,
   `tools/mcp/tests/test_hidden_player.py` (`JASPER_SLOW_BUILD=1`, `PREBUILT_PLAYERS`), so the editor
   tests of `test_hidden_window.py` run without a player runtime. The core visibility and lifecycle
   tests are in `engine/tests/core/platform/EngineLifecycleTest.cpp`, the hidden rate and clock in
   `engine/tests/core/util/HiddenUpdateTest.cpp`.
7. **Lean MCP rules** (after this spec): no agent text in the guide or in
   `docs/mcp/guide/platform-services.md` beyond the new `get_play_state` fields; the `send_input`
   description gains `hide|show`, with the eval case `bad_app_hide` (`tools/mcp/eval/cases.py`), and the
   `screenshot` refusal on a hidden editor says what to do in its own error text.
8. **`WindowLinux::isMaximized`** keeps its own property read (one round trip for both atoms) rather than
   two `X11Visibility::hasNetWmState` calls.
9. **Web visibility at project start.** A page hidden while the project waited for its saved files reports
   `onVisibilityChanged(false)` when the project starts, before `updatePause()`.
10. **The Gradle guard is local.** The GitHub workflows were removed from `main` after this spec: the
    deprecation check runs where Gradle runs, in `tools/build.sh android` and
    `tools/players/android_player.sh` (both fail on a deprecation in our Gradle files), with its own test
    `tools/ci/test_check_gradle_warnings.py`; no CI job runs it.
11. **The hidden player test runs glcore only.** `test_hidden_player.py` uses the `glcore-release` runtime;
    a `vulkan-release` run is left out: it needs a second prebuilt runtime build (about as long as the
    first) for a loop the two backends share line for line (`JasperEngineLinux.cpp`: the hidden branch never
    reaches `beginFrame`, where only the Vulkan acquire differs).

