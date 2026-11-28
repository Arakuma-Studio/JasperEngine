# Build And Run on Device: Run Device list, install and launch on Android, the iOS Simulator and the browser, Patch And Run, device logs in the Console

Builds on S30 (shipping, landed), S09 (logger sinks, landed) and S28 (crash handler, landed). Sibling
of build-profiles.md: Build And Run builds the selected Build Profile or platform. Paths are relative
to the repository root.

## 1. Goal

Build And Run today starts a game only on the computer the editor runs on. A mobile developer must
leave the editor to install an APK with `adb`, start a simulator by hand, read logcat in a terminal,
and rebuild, re-sign and reinstall the whole package to try a one-line Lua change. This spec makes
Build And Run reach an Android device or emulator, an iOS Simulator and the local browser from Build
Settings, the CLI and MCP; streams the device's log into the editor Console; and adds Unity's
**Patch And Run**: a Development Build pushes only the content files that changed into the
installed app and restarts it, with no repackaging and no re-signing. Agents get the same loop
through `build_project` and `run_build`, with the log tail, crash state and a device screenshot.

## 2. Scope

**In scope**
- A **Run Device** list per platform: Android devices and running emulators (`adb devices -l`), iOS
  Simulators (`xcrun simctl list devices available -j`, macOS hosts with Xcode), the default
  browser (Web), and "This computer" (host desktop, today's behaviour).
- **Build And Run** on each of them: install (full), launch, stream the log, report exit and crash.
  Android Build And Run makes a single-ABI APK for the chosen device; Package Format still governs
  Build.
- **Patch And Run** (Development Builds, Android and iOS Simulator): push the changed content into a
  per-app development content folder and restart; the installed binary and signature are untouched.
- **Web**: an editor-hosted loopback HTTP server with the cross-origin-isolation headers, opening the
  default browser; development pages post their log lines (and a requested screenshot) back to it.
- **Device logs** in the Console (category `device`), crash detection, Stop and Screenshot.
- MCP: `get_build_info` `devices`, `build_project` `run_device` and `patch`, `run_build` `device`;
  CLI `build --run-device`; Lua `System.getBuildInfo()` fields `contentPatch` and `contentPatched`.

**Out of scope (default decisions)**
- **Physical iOS devices.** They need a signing identity, a provisioning profile and `devicectl`
  (Xcode 15+). Default: not listed in v1; the iOS row says "Physical iPhones: build the .ipa and
  install it with Xcode". A later spec adds them behind the same Run Device list.
- Starting Android emulators (AVDs) from the editor, wireless-debugging pairing UI (`adb pair` is
  run by the user), Android TV/ChromeOS specifics, serving the web build to other machines (the
  server binds loopback only, as `editor/mcp/McpSocket.h:7-8` does today).
- Remote desktop targets: the Steam Deck already has `publish deploy-deck`/`deck-smoke` (S30).
- Live reload into a running game, a remote Lua console or command channel: dev-console-for-agents.md.
- Crash report collection, symbolication and upload: shipped-game-diagnostics.md. This spec only
  detects that the run crashed and shows its log tail.
- Input injection on a device (`run_build` `input`): desktop only, as today; the error says so.
- Patching native code: a change to the player binary (a different runtime, C++ scripts in a Compile
  build, native plugins) makes a full install. Swapping a `.so` into a signed APK, as the reference
  does, is not copied (section 4).

## 3. Current state

All read from the code (not run):

- **Build And Run is host-only.** The footer enables it only when
  `m_mode == ExportMode::Desktop || (m_mode == ExportMode::Player && m_platform == hostPlatform())`
  (`editor/window/dialog/ExportWindow.cpp:1327`), with the tooltip "Build And Run needs a game for
  this computer." (`:1336`). After a build, `launchBuiltPlayer` (`:708-738`) opens a macOS `.app`
  with `open` or starts the executable with `CommandRunner::launchDetached`, which inherits no
  descriptor (`:727-734`): the game's output never reaches the Console. Web is not runnable from the
  window at all.
- **`run_build` runs a desktop player only.** Its description: "Run a desktop player for `seconds`,
  screenshot it (Linux/X11), stop it" (`editor/mcp/tools/BuildTools.cpp:2443-2446`). Input needs
  Linux/X11 (`playerInputUnsupported`, `:2094`). On Windows the tool fails "run_build is not
  implemented on Windows yet" (`:2328`). It runs only an executable whose fingerprint (resolved path,
  device/inode, size, mtime, SHA-256) the editor recorded after `build_project`
  (`builtplayers::verify`, `:2070-2085`; `editor/util/BuiltPlayers.h:9-40`).
- **No device tooling in the editor.** No file under `editor/` calls `adb`, `simctl`, `devicectl` or
  `ios-deploy` (grep). `editor/platform/AndroidTools.h:23-50` finds aapt2, zipalign, apksigner,
  java, keytool, jarsigner and bundletool, not `platform-tools/adb`. Preferences > External Tools
  has the Android SDK path (`editor/settings/SettingsRows.cpp:4144`).
- **The pieces exist in test scripts.** `tools/android-emulator-test.sh` installs with
  `adb install -r -g` (`:268`), sets `debug.jasperengine.*` properties (`:270-271`), reads logcat and
  screenshots the device; `tools/ci/ios_packaging.sh` creates, boots and installs into a simulator
  and streams `log stream` for the process (`:99-112`). Both are CI scripts, not editor features.
  `docs/shipping.md:229-230` tells users to run `xcrun simctl install` and `launch` themselves, and
  `:246` to serve a web build with `python3 tools/web/serve.py`.
- **Launch options on mobile.** Android reads the launch options from the system property
  `debug.jasperengine.args` (`engine/platform/android/NativeEngine.cpp:566-575`); iOS passes the
  process arguments to `player::launch` (`engine/platform/apple/Renderer.mm:40-53`). `--content`
  exists (`engine/core/player/LaunchOptions.h:41`). An Android player's default content root is the
  APK's `assets/game` (`engine/player/PlayerMain.cpp:63-66`), and `platformFopen` reads through the
  asset manager unless the path lies inside the app's internal data folder
  (`engine/platform/android/JasperEngineAndroid.cpp:222-238`), so a content folder in internal
  storage is readable with `fopen` (inferred).
- **No debuggable development APK.** The prebuilt manifest template has no `android:debuggable`
  (`engine/platform/android/player/AndroidManifest.template.xml:19-24`) and the packager's markers
  do not include one (`editor/platform/AndroidPackager.cpp:183-185`), so `adb shell run-as <package>`
  cannot reach a prebuilt game's private files.
- **iOS Simulator builds exist.** `--ios-simulator` builds an unsigned `.app` with
  `xcodebuild -sdk iphonesimulator ... CODE_SIGNING_ALLOWED=NO`
  (`editor/platform/IosProject.cpp:209-212`).
- **Logs.** The Android sink writes with the tag `JasperEngine`
  (`engine/core/log/sinks/AndroidLogSink.cpp:29`); the Apple sink uses `os_log` with the subsystem
  `JasperEngine` or the bundle's (`engine/core/log/sinks/AppleLogSink.cpp:14-28`). The Console keeps
  20,000 entries readable by cursor (`editor/Out.h:66-77`).
- **Players never hot-reload.** "In games nothing is watched ... players never start the service"
  (`docs/hot-reload.md:122-124`).
- **Web serving.** `tools/web/serve.py:1-11` sends COOP/COEP, the wasm MIME type and
  `Cache-Control: no-store`; the editor has loopback socket helpers for its MCP HTTP transport
  (`editor/mcp/McpSocket.h:7-31`) but no static file server.
- **CLI.** `editor/main.cpp:57-112` dispatches `export`, `check`, `build`, `players`, `publish`; there
  is no run command. `jasperengine-editor call run_build` reaches the tool (S31).

## 4. Reference

| Source | Pattern studied | Take / improve |
|---|---|---|
| craftsman-end-motor `swap_so.sh` | Incremental Android deploy: strips the freshly built `.so` (keeps the unstripped copy for crash triage), re-stores `resources.arsc` uncompressed, swaps the library into an already-signed APK, `zipalign`, re-signs with `apksigner`, verifies the alignment (`:19-38`) | Take: deploy only what changed; keep symbols out of the device copy. Improve: we never touch the signed package on a patch (content goes to a development folder), so nothing is re-signed. Anti-patterns not to repeat: the keystore path and its passwords written into the script (`:16`, `:33`), a hard-coded SDK path and build-tools version (`:7-8`), a host-only `stat -f` (`:39`). Passwords stay asked at build time (S30 R13) |
| craftsman-end-motor `tools/ios/run_sim.sh` | Pick a simulator by name or UDID, `simctl boot` + `bootstatus -b`, `install`, environment for the app through `SIMCTL_CHILD_*`, the app's data container through `get_app_container ... data`, `launch --terminate-running-process --stdout --stderr` (`:12-41`) | Take the whole sequence; the editor runs it, parses `simctl list -j` instead of `awk` over text, and passes the game's options as process arguments (the player already reads them) instead of environment flags |
| craftsman-end-motor `tools/ios/bundle_assets.sh` | `rsync -a --delete` of the content into the `.app`, idempotent (`:7-8`) | Take the idempotent mirror for the development content folder; per-platform content dropping is build-profiles.md's |
| Unity: Build Settings/Build Profiles **Run Device**, **Build And Run**, **Patch** and **Patch And Run** (Android Application Patching), Android Logcat window, WebGL Build And Run's local server | Names and behaviour: a Run Device dropdown with Refresh; Patch And Run available for Development Builds only; the browser opened on a local server | Names only, no code |

Ideas only: no code, data, assets or Minecraft-specific mechanics may be copied from
craftsman-end-motor (a port reconstructed from a retail binary). Nothing here is genre-specific.

## 5. Requirements

### Functional: devices

- **R1 Device discovery.** `editor/platform/DeviceList.{h,cpp}` returns
  `RunDevice {id, platform, kind (host|device|emulator|simulator|browser), name, state, abis[],
  osVersion, usable, why}`:
  - Android: `adb devices -l`, then for each `device`-state entry `getprop ro.product.model`,
    `ro.product.cpu.abilist` and `ro.build.version.sdk` in one `adb -s <id> shell` call. `unauthorized`
    is unusable with why "Accept the USB debugging prompt on the device"; `offline` with "Reconnect
    the device or restart adb (adb kill-server)"; an SDK below the project's `android.min_sdk` with
    "Android <n> is older than Minimum API Level <m>".
  - iOS: `xcrun simctl list devices available -j` (booted and shut down); only on a macOS host with
    a full Xcode (the existing `DEVELOPER_DIR` / Xcode lookup of S30). Elsewhere one unusable row
    "iOS Simulators need a Mac with Xcode".
  - Web: `browser` "Default browser", always usable.
  - Host desktop: `host` "This computer", when the selected platform is the host's.
  - `adb` is Preferences > External Tools > adb (`preferences.adb_path`), else
    `<Android SDK>/platform-tools/adb`, else `PATH`. Missing: one unusable row naming the SDK
    Manager package `platform-tools`.
- **R2 Probing cost.** Discovery runs on a background job, never per frame: when Build Settings
  shows Android or iOS, on **Refresh**, and every 3 s while it stays visible; each probe has a 2 s
  timeout per process. `get_build_info devices: true` probes with the same timeout and reuses a
  result younger than 2 s.
- **R3 Build And Run (full install).** After a successful build for the selected Run Device:
  - **Android**: Build And Run builds a single APK of the device's first ABI the build selects
    (`ro.product.cpu.abilist` ∩ Project Settings > Android > Architectures; none in common fails
    naming both lists) into the build's output folder, whatever Package Format says.
    `adb -s <id> install -r -t <apk>` (no `-g`: permissions are asked as on a player's phone); on
    `INSTALL_FAILED_UPDATE_INCOMPATIBLE` (another signature) the error says "uninstall the app from the
    device first (adb uninstall <package>), or sign with the same keystore" and never uninstalls by
    itself (it would delete the player's saves). Then `run-as <package> rm -rf files/je-dev-content`
    (development builds), then launch with `am start -W -n <package>/com.jaspergameengine.jasperengine.MainActivity`.
  - **iOS Simulator**: a shut-down simulator is booted (`simctl boot`, `bootstatus -b`) and, unless
    the editor is headless, `open -a Simulator` shows it; the build is the simulator `.app`
    (S30 `--ios-simulator` is implied); `simctl install`, the development folder (R7) removed, then
    `simctl launch --terminate-running-process <udid> <bundle id> <options>`.
  - **Web**: R10.
  - **Host**: today's `launchBuiltPlayer`, plus R12's log capture.
- **R4 Game options.** The run's player options (`--fixed-fps`, `--scene`, `--device-tier`, the
  game's own arguments after `--`) reach every target: Android through `debug.jasperengine.args`
  set right before the launch and cleared once the game logs "Engine ready:" or after 30 s;
  iOS as process arguments; Web as the URL query; host as arguments.
- **R5 Install records.** Each install or patch writes the device's record in
  `.jasperengine/user/devices.yaml` (through `io::writeFileAtomic`): device id, package or bundle id,
  the installed package's SHA-256, the player binary's identity (prebuilt: player id and
  `playerSha256` of the content manifest; compiled: the SHA-256 of the game library/executable), and
  the content manifest of what the device runs (per file `sha256` and `size`), plus the patch
  counter. A record whose package no longer answers (`pm path` empty, `simctl get_app_container`
  fails) is dropped.

### Functional: Patch And Run

- **R6 When it patches.** Patch And Run (and `patch: true`) is offered when:
  - the build is a Development Build;
  - the target is Android or the iOS Simulator;
  - the device has a record (R5) whose binary identity equals the new build's;
  - and, on Android, the installed package is debuggable (R9).

  Otherwise it does a full Build And Run and says why ("the player binary changed: C++ scripts were
  recompiled", "no install recorded on Pixel 7"). A patch skips packaging (no APK, no Xcode build,
  no signing): it cooks the content as a build does, into the build cache.
- **R7 Development content folder.** A development player, at start, uses
  `<internal data>/je-dev-content/` (Android: the app's files folder; iOS:
  `Library/je-dev-content` of the data container) as its content root when that folder holds a
  `content-manifest.json` whose `playerId` and `playerSha256` (or compiled binary hash) match the
  running binary. A mismatch logs one warning ("development content in je-dev-content is for
  another player; using the packaged content") and uses the packaged content. Release players never
  look: the check is compiled only under `JASPERENGINE_DEVELOPMENT_BUILD`
  (`engine/core/Engine.h:825`). An explicit `--content` wins over the folder.
- **R8 What a patch pushes.** The first patch mirrors the whole cooked content into the folder; later
  patches push only files whose SHA-256 differs from the device record, delete the files the new
  manifest no longer lists, and write `content-manifest.json` last (so a broken transfer leaves the
  old manifest, which no longer matches the files' hashes: the player's `--verify-content` path
  refuses it and falls back to packaged content with the warning above).
  - Android: one `adb push` of a staging folder to `/data/local/tmp/jasper/<package>/patch-<n>/`,
    then `adb shell run-as <package> sh -c 'cp -R /data/local/tmp/jasper/<package>/patch-<n>/. files/je-dev-content/ && rm ...'`
    for the deletions, then the temporary folder is removed.
  - iOS Simulator: the data container is a host folder: files are written directly with
    `io::writeFileAtomic`.
  - Then the app is stopped (`am force-stop` / `simctl terminate`) and launched as in R3.
  - The content manifest gets `patch: <n>` and `patchedAt` (ISO 8601 UTC); the player exposes them
    (R27).
- **R9 Debuggable development APKs.** The Android manifest template gains the marker
  `{{DEBUGGABLE}}`, filled with `android:debuggable="true"` for Development Builds and empty
  otherwise. A player runtime whose template lacks the marker builds as today and Patch And Run is
  unavailable with why "the Android player runtime predates Patch And Run: update it". The payload
  validator (S30 R30) refuses a `release`-flavour Android target whose package is debuggable
  (`aapt2 dump badging` reports `application-debuggable`), and so does `publish prepare`.

### Functional: Web, logs, crashes, stop

- **R10 Web Build And Run.** `editor/platform/WebServe.{h,cpp}`: a static file server on
  `127.0.0.1:<first free port from 8600>` serving the build's output folder, with the headers of
  `tools/web/serve.py` (COOP `same-origin`, COEP `require-corp`, `application/wasm`,
  `Cache-Control: no-store`), GET and HEAD only, no directory listing, paths confined to the folder
  (a `..` or symlink escape is a 404). It starts on the first Web Build And Run and stops when the
  project closes. The browser opens `http://127.0.0.1:<port>/index.html?editor-port=<port>&<options>`
  through `HostServices::openURL`; a headless editor opens nothing and the result gives the URL.
- **R11 Web log channel.** With `editor-port` in its URL, a development page's loader posts each log
  line (batched, at most every 100 ms) to `POST /__jasper/log`, and on `screenshot` uploads the PNG
  to `POST /__jasper/screenshot`. The server accepts these from its own origin only, at most
  1 MB per log request and 16 MB per screenshot. Release pages carry no reporter.
- **R12 Device log in the Console.** While a run is active its log streams into the Console with
  category `device`, the message prefixed by the device name ("[Pixel 7] ..."), and levels mapped
  (logcat V/D/I/W/E/F, `os_log` levels, the web reporter's levels, a desktop player's stderr):
  - Android: `adb -s <id> logcat -v threadtime --pid=<pid>` (pid from `pidof`), and on API < 24
    the same stream filtered by pid in the editor; the tags `AndroidRuntime`, `DEBUG`, `libc` and
    `crash_dump` of the same pid are kept for crashes.
  - iOS Simulator: `simctl spawn <udid> log stream --style compact --level debug --predicate
    'process == "<executable>"'`, plus the launch's stdout/stderr.
  - Web: R11. Host: the player's stdout/stderr through a pipe instead of the detached launch.
  - The stream ends when the app exits or Stop is pressed; one line says how ("[Pixel 7] exited
    (code 0)", "... crashed: Fatal signal 11 (SIGSEGV)", "... stopped").
- **R13 Crash detection.** A run is `crashed` when Android logs `FATAL EXCEPTION` or `Fatal signal`
  for its pid, the process disappears without the engine's shutdown line, the simulator process
  ends by a signal, the web reporter posts the S28 web crash overlay's report, or a desktop player
  ends by a signal. The S28 crash report on the device is out of scope (shipped-game-diagnostics.md);
  the run reports `fatal_lines` from the log.
- **R14 Stop and Screenshot.** The Console's device strip (R19) has **Stop** (force-stop /
  terminate / asks the page to quit through the web channel / kills the host player it started)
  and **Screenshot** (`adb exec-out screencap -p`, `simctl io <udid> screenshot`, the web page's
  upload, and for the host the capture `run_build` uses today), saved as `Recordings/Device/<device>-<UTC time>.png` (`Recordings` is not an asset
  folder: `editor/Project.cpp:5989-5991`).
- **R15 Only recorded builds.** A device run installs or patches only a package the editor recorded
  right after building it (`builtplayers::record` extended to `.apk`, `.aab` is not installable,
  and the simulator `.app` directory hashed as a tree), verified as `run_build` verifies an
  executable today. Content pushed by a patch is the build cache's, never an argument.

### Platform & backends

- **R16 Hosts.** Android Build And Run and Patch And Run on Linux, Windows and macOS hosts; iOS
  Simulator on macOS with Xcode; Web and host desktop everywhere. Every process is started with an
  argument vector (`CommandRunner`), never through a shell; quoting inside `adb shell` commands uses
  one escaping helper tested with spaces, quotes and non-ASCII package-relative paths.
- **R17 Game side.** R7 is the only runtime change: one directory check at start in a development
  player, none in a release player, on every backend. The web reporter is page JavaScript only.

### Performance

- **Budgets** (mobile and web first; the CPU numbers are for the CI runner, 4 cores):
  - Patch diff: ≤ 200 ms for 10,000 content files when unchanged (hashes cached by path, size and
    mtime in the build cache).
  - Patch And Run of a one-file change (≤ 1 MB) on the CI emulator: from the end of cooking to the
    game's "Engine ready:" ≤ 15 s; on a USB device the push itself is bounded by adb throughput.
  - Log streaming: ≤ 1 ms of main-thread time per frame at 2,000 lines/s (lines are batched off the
    main thread and handed to `Out` once per frame); above 5,000 lines/s lines are dropped and one
    line per second says how many.
  - Discovery: 0 processes while Build Settings is closed and no run is active; ≤ 1 probe per 3 s
    otherwise.
  - Release players: 0 added work; development players: one `stat` at start.

### Editor UX (human)

- **R18 Build Settings.**
  - A **Run Device** row (Android, iOS, Web; the host shows "This computer", read-only), with a combo
    "<name> (<id>) — <state>", unusable entries greyed with their why as tooltip, and a **Refresh**
    button.
  - **Build And Run** is enabled for a usable Run Device of the selected platform; its tooltip names
    the reason when not ("No Android device: connect one with USB debugging on, then Refresh").
  - With **Development Build** on and a patchable target (R6), the footer shows **Patch And Run**
    between Build and Build And Run; disabled with its reason when R6 fails. Unity's **Patch** (push
    without launching) is not offered: a patch without a restart has no effect (players do not
    reload).
  - The selection is per user (`.jasperengine/user/devices.yaml`), not a project edit, so it has no
    undo (as the selected platform).
- **R19 Console.** While a run is active, a strip above the entries shows the device, its state
  (Installing, Pushing 3 files (2.1 MB), Running, Exited, Crashed), **Stop** and **Screenshot**. The
  Console's category filter lists `device`. Clicking a crashed run's "crashed" line selects its first
  fatal line.
- **R20 Preferences.** External Tools > Shipping gains **adb** (path, with a check mark or "Not
  found: install Android SDK Platform-Tools").

### Agent UX (MCP, CLI, Lua)

- **R21 `get_build_info`** gains `devices` (boolean, default false; probing spawns processes) and,
  with it, `devices[{id, platform, kind, name, state, usable, why?, abi?, os_version?, installed?:
  {package, patch, patchable, why_not?}}]`.
- **R22 `build_project`** gains:
  - `run_device` (string: a device id, `auto` = the only usable device of the platform, or `web`):
    after the build, install or patch and launch, then return (the GUI's Build And Run).
  - `patch` (boolean, default true when `development`): patch when R6 allows; else a full install
    and `next` says why.
  - The result adds `device {id, name, installed: full|patched, pushed_files, pushed_bytes,
    launch_ms, url?}` and `next` "read_console categories [\"device\"] for its log; run_build device
    to watch it".
- **R23 `run_build`** gains `device` (same values). With it, the tool runs the build recorded for
  that platform on the device: installs or patches when the device's record differs, launches with
  `args`, `fixed_fps`, `quit_after` and `after_ready` (R4), waits `seconds`, captures
  `screenshot`/`screenshots_at` with the device's capture, stops the app unless `keep_running` (new,
  boolean), and returns the same fields as a desktop run (`log_tail`, `errors`, `exit_code`,
  `crashed`, `fatal_lines`) plus `device`. `input`, `space`, `gap_ms` and `input_delay_seconds`
  with `device` fail "input is desktop-only (Linux/X11); on a device drive the game from Lua or its
  arguments". `path` with `device` must name the recorded package.
- **R24 Errors name the fix** ("no Android device is connected: connect one with USB debugging on
  (get_build_info devices:true lists them)", "Pixel 7 is unauthorized: accept the USB debugging
  prompt on the device", "iOS Simulators need macOS with Xcode").
- **R25 CLI.** `jasperengine-editor build ... --run-device <id|auto|web> [--no-patch]` does Build And
  Run headlessly and prints `device:`, `installed:` and `url:` lines (or JSON); the log is followed
  until the app exits or `--run-seconds S` (default 0: return after the launch). Device lists and
  runs otherwise go through `call get_build_info --devices` and `call run_build --device`. No new
  subcommand.
- **R26 Guide.** `docs/mcp/guide/shipping.md` gets a "Devices" section (the loop: `get_build_info
  devices:true`, `build_project run_device`, `read_console categories:["device"]`, `run_build
  device`); no line in the always-sent guide unless an eval case needs one.
- **R27 Lua.** `System.getBuildInfo()` (owned by build-profiles.md, whichever lands first adds the
  stub fragment `engine/core/script/binding/stubs/BuildInfo.lua`) gains `contentPatch` (integer, 0
  for packaged content) and `contentPatched` (boolean), so a game's debug overlay can show which
  patch runs. In the editor's Play both are 0/false.

### Data & compatibility

- **R28** New per-user file `.jasperengine/user/devices.yaml` (`formatVersion: 1`, not committed);
  an unreadable or newer file is ignored with one warning and rewritten. New preference
  `preferences.adb_path` (default ""). No `project.yaml`, scene or prefab change: no version bump.
- **R29** `content-manifest.json` gains optional `patch` and `patchedAt` (absent = 0 / none). Older
  players ignore them; the content format version is not bumped (a patch is development only and
  read only by players that implement R7).
- **R30** The manifest template marker (R9) ships with the player runtimes of the editor version
  that adds it (runtimes are locked to their editor version, `docs/shipping.md:129-131`).

## 6. Design constraints & decisions

- **D1 Patch = content only, never the package.** The reference re-signs an APK per change; we
  leave the signed package alone and redirect a development player to a private content folder.
  This keeps the keystore out of the loop (no password prompt on a patch), works the same on the
  simulator, and costs nothing in release players.
- **D2 Full mirror, not an overlay.** The development folder holds the complete content, so the
  player needs no two-root file lookup. The first patch pays one full push; later patches push the
  difference. Default chosen over an overlay because `platformFopen` and `getSystemPath` stay
  untouched.
- **D3 `run-as` over world-readable paths.** Writing into the app's own storage needs a debuggable
  package (R9); `/sdcard/Android/data` was rejected (scoped storage changes per Android version,
  and a release player would have to look there).
- **D4 One device runner per platform.**

  ```cpp
  // editor/platform/DeviceRunner.h
  namespace jasperengine::editor::platform {
      struct DeviceRunRequest {
          RunDevice device;
          std::filesystem::path package;      // .apk, simulator .app, web output folder, executable
          std::filesystem::path contentDir;   // the build cache's cooked content (patches)
          std::vector<std::string> playerArgs;
          bool patch = false;
      };
      struct DeviceRunState { std::string phase; bool running = false, crashed = false;
                              std::optional<int> exitCode; std::vector<std::string> fatalLines; };
      class DeviceRunner {   // AndroidRunner, SimulatorRunner, WebRunner, HostRunner
      public:
          virtual bool install(const DeviceRunRequest&, std::string& error) = 0;
          virtual bool patch(const DeviceRunRequest&, PatchStats& stats, std::string& error) = 0;
          virtual bool launch(const DeviceRunRequest&, std::string& error) = 0;
          virtual void stop() = 0;
          virtual bool screenshot(std::vector<uint8_t>& png, std::string& error) = 0;
          virtual DeviceRunState state() const = 0;   // thread-safe; the log goes to Out
      };
      std::unique_ptr<DeviceRunner> makeRunner(const RunDevice& device, CommandRunner& runner);
  }
  ```

  The GUI, `build_project`, `run_build` and the CLI share it; one active run per device, a second
  request for the same device stops the first.
- **D5 Long work is a job.** Install, patch and launch run in the build's job (MCP `McpJobs`); the log
  pump is a background thread per run that hands batches to `Out` on the main thread.
- **D6 Patch content source.** A patch cooks with the same exporter steps as a build up to the
  content manifest (`editor/ExporterPrebuilt.cpp:436-521`), then stops before packaging; the manifest's
  player identity is the installed one (R6 guarantees equality).
- **D7 Fakes for tests.** Every device command goes through `CommandRunner` with the tool path from
  R1, so tests put stand-in `adb` and `xcrun` scripts first on `PATH`, as `test_publish.py` does for
  `ssh`/`scp` (`tools/mcp/tests/test_publish.py:55-66`).
- **D8 Names.** Run Device, Build And Run, Patch And Run, Refresh: Unity's. The Console category is
  `device`; the MCP parameter `run_device` on `build_project` and `device` on `run_build` (the tool
  already runs something).

## 7. Acceptance criteria

- **AC1 Discovery parsing.** `editor/tests/platform/DeviceListTest.cpp` (a new folder, as the "Where a
  test goes" table puts `editor/platform/X.cpp`'s test; the target globs it):
  `DeviceList.ParsesAdbDevicesLong` (device, emulator, unauthorized, offline, a model with spaces),
  `DeviceList.ParsesSimctlJson` (booted/shutdown, unavailable runtimes left out),
  `DeviceList.FlagsAnApiBelowMinimumSdk`, `DeviceList.MissingAdbIsOneUnusableRow`. Fail without:
  no parser exists.
- **AC2 Shell quoting.** `DeviceCommands.QuotesRunAsPathsWithSpacesAndQuotes` checks the argument
  vectors and the `sh -c` strings for `files/je-dev-content/assets/My "Hero".png`. Fail without:
  no helper.
- **AC3 Android full install and patch (fakes).** `tools/mcp/tests/test_device_run.py` (`CI = True`).
  The Android build uses a fake runtime (`fake_runtime`, `tools/mcp/tests/test_build_method.py:193`)
  and stand-in SDK tools (`aapt2`, `zipalign`, `apksigner`, `java`) that write a placeholder package,
  since the packaging itself is covered by the `AndroidPackage.*` unit tests; a stand-in `adb`
  records its calls and serves canned `devices -l`, `getprop`, `pm path`, `pidof` and `logcat`
  output:
  - `test_build_and_run_android_installs_and_launches`: `build_project platform android development
    run_device "emulator-5554"` → the record shows `install -r -t`, `run-as ... rm -rf
    files/je-dev-content`, `am start -W -n <package>/...MainActivity`; result `installed: "full"`.
  - `test_patch_and_run_pushes_only_changed_files`: change one Lua file, build again → no `install`,
    one pushed content file plus `content-manifest.json`, `force-stop` then `am start`; result
    `installed: "patched"`, `pushed_files: 2`.
  - `test_patch_falls_back_to_full_when_the_player_changed`: switch the backend's runtime id in the
    fake record → full install and `next` names the reason.
  Fail without: `run_device` is an unknown argument.
- **AC4 Only recorded packages.** `test_device_run.py::test_run_build_device_refuses_unrecorded_packages`:
  `run_build device:"emulator-5554" path:<other.apk>` fails naming "the package this project built".
- **AC5 Logs, crash, screenshot (fakes).** `test_device_run.py::test_run_build_device_reports_crash`:
  the fake logcat emits a `Fatal signal 11` line for the pid → `crashed: true`, the line in
  `fatal_lines`, `read_console categories ["device"]` holds "[<model>]" lines; `screenshot: true`
  returns the fake `screencap` PNG as an image.
- **AC6 Development content folder (engine).** `engine/tests/core/player/DevelopmentContentTest.cpp`:
  `DevelopmentContent.UsedWhenManifestMatchesThePlayer`, `DevelopmentContent.IgnoredForAnotherPlayer`
  (warning logged once, packaged content used), `DevelopmentContent.ExplicitContentOptionWins`. A
  release-flavour build of the test leaves the check out: `DevelopmentContent.ReleaseNeverLooks`
  asserts the function is a constant false when `JASPERENGINE_DEVELOPMENT_BUILD` is 0. Fail without:
  the folder is never read.
- **AC7 Debuggable only in development.** Extending the existing files:
  `editor/tests/export/BuildMethodTest.cpp` `AndroidPackage.DebuggableOnlyInDevelopmentBuilds` (beside
  `AndroidPackage.CarriesNativeManifestSettings`, `:358`), and `editor/tests/publish/PublishingTest.cpp`
  `PayloadValidator.RefusesADebuggableReleaseApk` (beside `AndroidPackagesMustBeSigned`, `:262`) using a
  fake `aapt2` output. Fail without: no marker, no finding.
- **AC8 Web serving.** `test_device_run.py::test_web_build_and_run_serves_with_isolation`: `build_project
  platform web run_device "web"` on a headless editor returns `device.url`; a GET of it has COOP/COEP,
  `application/wasm` for the `.wasm`, `no-store`; `GET /../project.yaml` is 404; a `POST
  /__jasper/log` with two lines puts them in `read_console categories ["device"]`. Fail without: no
  server.
- **AC9 Web gate.** `tools/web/browser_gate.py --editor-served` (the gate's Chromium) loads a
  development build through the editor's server and checks the page's "Engine ready:" line arrives in
  the Console through the reporter. Fail without: the page has no reporter.
- **AC10 Real emulator.** The Android emulator lane (`docs/testing.md` "Android emulator") runs
  `tools/ci/android_patch_run.sh`: prebuilt Development APK of `tools/test-projects/core-utilities`
  with `jasperengine-editor build --run-device emulator-5554`, then a Lua string edit and the same
  command again; checks logcat has the new string, `pm path` reports the same APK SHA-256 (no
  reinstall), and the patch step's time ≤ 15 s (reported through `perfSlack()` in the script's
  summary, not asserted raw). Fail without: the second run reinstalls or shows the old string.
- **AC11 iOS Simulator.** `tools/ci/ios_packaging.sh` (macOS, on demand) gains a step using
  `jasperengine-editor build --platform ios --development --run-device <udid>` and then a patch run,
  checking the new log line in `log stream` and that `simctl install` ran once.
- **AC12 Log pump budget.** `editor/tests/platform/DeviceLogPumpTest.cpp`:
  `DeviceLogPump.BatchesLinesOncePerFrame` (10,000 lines in one second reach `Out` in ≤ 60 batches)
  and `DeviceLogPump.DropsAboveTheRateAndSaysHowMany`. Main-thread time per batch is reported
  through `perfSlack()`.
- **AC13 Editor UX.** `test_device_run.py::test_build_settings_run_device_row` drives Build Settings
  through `EditorModal` (`find_ui_item`): the Run Device combo lists the fake device, Patch And Run
  appears only with Development Build on. A screenshot from a scratch copy of a fixture project with
  a real emulator connected ships with the PR (Build Settings with Run Device and Patch And Run; the
  Console's device strip while running).
- **AC14 Docs and budget.** `docs/shipping.md` gets "Running on a device" (Android, Simulator, Web,
  Patch And Run, the development content folder, `adb` lookup); `docs/mcp/guide/shipping.md` its
  Devices section; `docs/mcp/TOOLS.md` regenerated; `tool_budget.py --check tools/mcp/budget.json`
  passes with core under its limit (R23's text replaces part of `run_build`'s description rather than
  adding to it).

## 8. Dependencies & conflicts

- **Depends on** S30 (prebuilt packaging, `--ios-simulator`, payload validator, BuiltPlayers), S09
  (Android and Apple log sinks), S28 (web crash overlay, crash exit detection).
- **build-profiles.md**: Build And Run builds the selected profile; a profile's platform picks the
  Run Device list; `System.getBuildInfo()` and its stub fragment are shared (R27).
- **dev-console-for-agents.md**: may add a command channel to a running game; it should reuse this
  spec's device runner (D4) to reach the device and forward its port (`adb forward`), not start a
  second process manager.
- **shipped-game-diagnostics.md**: collects S28 reports from devices and symbolicates; this spec
  only flags `crashed` and keeps `fatal_lines`.
- **project-test-runner.md**, **input-recorder-and-input-scripts.md**,
  **release-performance-and-gpu-quirks.md**: can run on a device through `run_build device` and the
  game's arguments; input injection on devices stays theirs to add.
- **Merge hotspots**: `editor/window/dialog/ExportWindow.cpp` (footer, rows; build-profiles.md edits
  the same window), `editor/mcp/tools/BuildTools.cpp` (`run_build`, `build_project`),
  `editor/platform/AndroidPackager.cpp` and the manifest template (S67 touches packaging tests),
  `engine/player/PlayerMain.cpp` (content root), `tools/ci/ios_packaging.sh`,
  `tools/android-emulator-test.sh` lane wiring, `editor/settings/SettingsRows.cpp` (adb preference).

## 9. Risks & defaults

- **`run-as` reading `/data/local/tmp`** (assumed from how Android Studio deploys; not verified here).
  Default: R8's push-then-copy. Fallback decided now: stream each file through
  `adb exec-in run-as <package> sh -c 'cat > <path>'` when `cp` is denied; AC10 shows which one the
  emulator needed.
- **OEM devices without `run-as` or with broken `pidof`.** Default: Patch And Run unavailable with why
  "this device's run-as is not usable"; logcat falls back to filtering by package in the editor.
- **Global `debug.jasperengine.args`.** Shared by every JasperEngine app on the device. Default: set
  right before launch, cleared after "Engine ready:" or 30 s (R4), so a later launcher start runs
  without them.
- **A patch transfer interrupted.** Default: the manifest goes last (R8); the next patch compares
  against the device's record, which is written only after the device confirms the copy.
- **Saves survive reinstall** (`install -r`). Intended: a player's saves are kept; the development
  folder is cleared on every full install so stale content never outlives its package.
- **Port 8600 taken.** Default: the next free port up to 8699; none free fails naming the range.
- **Physical iOS later.** The Run Device model has `kind` and `usable/why`, so adding devices with
  `devicectl` is additive.

## Deviations

- **Android screenshots** (R14) are `adb shell screencap -p <file>` then `adb pull`, not
  `adb exec-out screencap -p`: the editor reads a tool's output as text lines (`CommandRunner`).
- **No engine shutdown line exists** (R13): a process that disappears without a fatal line in its
  log is reported as exited (code unknown), not crashed; adding a shutdown line would have been a
  second runtime change (R17). iOS Simulator apps are not the editor's children, so their signal is
  not known either: a Simulator run is crashed by its fatal lines.
- **Player binary identity** (R5, R6, R7): a prebuilt Android or iOS runtime names a folder as its
  executable, so `playerSha256` in their content manifests stays empty, as before. The device record
  keeps the runtime id and a hash of the runtime's `executables` map; the player compares the
  development folder's `playerId` with its own runtime id and its `playerSha256` with the packaged
  content's (which was built for the running binary). Android and iOS builds are always prebuilt
  (a Compile build of them is the source export), so the "compiled binary hash" case does not arise.
- **AC3** `test_patch_and_run_pushes_only_changed_files`: the first patch after a full install
  mirrors the whole content, as R8 says (the folder starts empty); the test checks that, then
  changes one Lua file and checks `pushed_files: 2` on the next patch.
- **AC10** builds `tools/fixtures/tween-curve` (an editor project): `tools/test-projects/core-utilities`
  is an engine test project (`main.cpp`), not one the editor builds. The script times the whole
  second command (cook, push, restart), an upper bound of the spec's end-of-cook-to-ready time.
- **R11** the reporter goes into the pages of prebuilt Development web builds (the editor writes the
  page); a Compile web build's page (its CMake shell) has none. A web screenshot is the canvas read
  with `toBlob` right after a frame.
- **R9** the payload check reads `aapt2 dump badging` of APKs; a bundle is not checked for
  `debuggable` (Google Play refuses one itself). A Debug build type's APK is debuggable too.
- **R21** `get_build_info devices:true` probes every platform's devices (Android, iOS, the browser,
  this computer), not only the selected one: the call has no platform argument.
- **R25** a web Build And Run from the CLI serves the page until interrupted when `--run-seconds` is
  not given (the server lives in the CLI's process).
- **Risks** the `run-as` fallback through `adb exec-in ... cat` is not implemented: a device whose
  `run-as` cannot copy the pushed patch fails the patch with "this device's run-as is not usable",
  the risk section's default for such devices.
- **AC9, AC13 (integration pass).** AC9 ran in the Linux image: an editor on Xvfb with its HTTP MCP
  server, `build_project {mode: "web", development: true, run_device: "web"}` through
  `jasperengine-editor call`, then `browser_gate.py --editor-served` (its ready line read back from
  `read_console categories ["device"]`). AC13's screenshot is of that run: the Console's device strip
  with the Default browser device Running, its URL, Stop and Screenshot. No emulator was attached to
  that editor (the emulator image has no X server for the editor's window).
- **AC11 (Mac pass).** `tools/ci/ios_packaging.sh development` ran in full on an arm64 Mac (macOS 26.2,
  Xcode 26.6, an iPhone 17 simulator on iOS 26.5) and passed: the simulator build ran and showed its scene,
  the device build stopped before signing (W-IOS-NO-TEAM), the team build failed with "Xcode has no account
  for the team", the unsigned archive was made and `-exportArchive` stopped at "No profiles" (no developer
  account), `publish prepare` passed, and step 6 installed in full, then patched (`installed: patched`,
  `simctl install` once) with the patched script's line in `log stream`.
- **R12 Simulator log, one copy.** The engine writes each line to the unified log and to the app's
  stdout/stderr, and the run reads both, so every line showed twice. The unified log's copy goes to the
  Console, with its level (the engine's warnings are its default type) and its category; a stdout or
  stderr line waits 3 s for its unified copy and goes on alone only when none came (a `printf` past
  the engine's log, or no `log stream`). A Patch And Run names the process from the installed app
  (`simctl get_app_container ... app`): its request has no package, and its `log stream` used to
  match no process. Stopping the `log stream` no longer warns "Failed to send
  SIGTERM to process group": the second SIGTERM of a stop found the process exited and not yet
  reaped, a group macOS refuses to signal (EPERM).
