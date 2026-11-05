# Shipping games

How a JasperEngine project becomes a game on every platform, and how a build becomes a release
(docs/specs/S30-shipping.md). Agents: `jasper://docs/shipping` (docs/mcp/guide/shipping.md).

## Two ways to build

**Build Settings > Build** (or `jasperengine-editor build`) makes a game for Windows, Linux,
macOS, Android, iOS or the web. It picks one of two methods:

- **Prebuilt player** (no compiler needed). The editor cooks the project's scenes and prefabs
  into a binary form (`.jscn`, `.jpfb`), writes `game.json` and `content-manifest.json`, and puts
  them next to a ready-made player executable, the *player runtime* of that platform. Seconds,
  with no C++ toolchain, Android NDK, Gradle or emsdk.
- **Compile from source**. The editor generates the C++ project and compiles the engine with
  the game, as before. It is needed when the project has
  - C++ scripts,
  - a `ProjectBuild.cmake`,
  - capacities above the player's (Project Settings > Scene Limits: `MAX_BONES` above 128, ...),
  - Google Mobile Ads (AdMob),
  - or when Build Settings > Build Method says Compile.

  Compiling makes builds for this computer's platform and for the web; other platforms are
  built from the exported source project (Export source, then Android Studio, Xcode or CMake).

The **Build Method** row of Build Settings shows the choice and why ("Compile from source: C++
scripts (Player.cpp)"); its combo forces Auto, Prebuilt or Compile (`export.method` in
project.yaml). Both methods write the same `content-manifest.json` and `licenses/`, so releases
treat them alike.

A build replaces its output folder only once it is complete: it is made in
`<folder>.staging-<pid>` and swapped in at the end, so a failed or cancelled build leaves the
previous one as it was.

## Player runtimes

A runtime is named `<platform>-<arch>-<backend>-<flavour>`, for example
`linux-x64-glcore-release` or `web-wasm32-gles3-development`. The flavour follows
**Development Build** (profiler and `--profile`). The editor install ships the runtimes of its
own platform and the web; Build Settings downloads the others ("Download (NN MB)"), checked
against the SHA-256 values built into that editor release. Preferences > External Tools lists
the installed ones, with Remove and Install from file.

| Platform | Backends | Arch | What the build makes |
|---|---|---|---|
| Windows | d3d11, glcore, vulkan | x64 | `<Game>.exe` (its icon and version resource patched in), `content/` |
| Linux | glcore, gles3, vulkan | x64 | `<Game>` (executable), `<Game>.desktop`, the content |
| macOS | metal, glcore | universal | `<Game>.app` (Info.plist, icon, signed when `codesign` or `rcodesign` is set up; warning `W-MAC-UNSIGNED` otherwise) |
| Android | gles3 | universal (arm64-v8a, armeabi-v7a, x86, x86_64) | `<Game>.aab`, `<Game>-<abi>.apk` or `<Game>.apk`, signed |
| iOS | metal | arm64 | the Xcode project; on a Mac with Xcode the `.ipa` (or a simulator `.app`) |
| Web | gles3 (WebGL 2), single-thread and pthreads | wasm32 | `index.html`, the two players, `content.jpak`, `coi-serviceworker.js` |

From the command line:

```
jasperengine-editor players list
jasperengine-editor players download linux-x64-vulkan-release
jasperengine-editor players install jasperengine-player-<id>-<version>.zip
jasperengine-editor players remove <id>
```

An engine checkout builds its own runtimes: `tools/build.sh players` (Linux; `PLAYER_PLATFORM=web`
and `PLAYER_PLATFORM=android` for those), into `build/players/`; point the editor at them with
`JASPER_PLAYERS_DIR=build/players` or `--players-dir`. A runtime is locked to the editor version
it was built with.

## Player size: Strip Engine Code

A game ships only the engine modules it can use (docs/specs/S34-player-size.md). The optional
modules are the HTML UI (UI Documents, their Lua API, anime, widgets, effects, on-screen
controls), its `<video>`, the runtime shader compiler, 3D physics (Jolt), 2D physics (Box2D) and
navigation meshes. The build finds what the project reaches:

- a UI Document, Rigidbody, joint, character controller or Buoyancy in a scene or prefab, or a
  Spline Extrude with collision;
- a Lua script naming a module's API (`UIDocument`, `anime`, `getRigidbody`, `Joint3DType`...),
  in code or in a string; a member's name counts after `.` or `:` or in a string, not as a local
  or a table key (`local volume`, `{duration = 2}`); comments do not count. In a project with
  the HTML UI, a member named like the video API (`self.duration`, `"volume"`) keeps video;
- a script property of one of their types;
- a `<video>` element in a document, or a script naming video, with the HTML UI;
- the runtime shader compiler: a script that can change what an object draws with (makes a mesh,
  a material or a light, changes a camera's target, the post-processing...), beyond what the
  build already cooks (moving, spawning prefabs, playing sounds and animations, physics, the
  light capacity, shadow and lit quality are all cooked);
- a dynamic access that can reach anything (`_G`, `_ENV`, `load`, `debug`, `package`, a
  `require` of a computed name), or C++ scripts: every module.

A prebuilt build then uses the smallest player variant with those modules: `full`, `ui` (no
video), `core` (no HTML UI), `lite3d` (3D physics and navigation only) or `lite2d` (2D physics and
navigation only); a release runtime's id ends with the variant (`linux-x64-glcore-release-lite3d`).
When a variant of fewer modules than the one it uses would fit but is not installed, the build
warns `W-PLAYER-VARIANT` with the id to download. A Compile build leaves the same modules out of
what it compiles (a source export and a project with C++ scripts keep every module their settings
allow). The player checks the modules its content needs when it starts (exit 5 when one is
missing), and so does the build before it ships.

The Console, the CLI (`modules:`, `left_out:`, `module:` lines) and `build_project`
(`modules`, `module_reasons`) say what was kept and why. A script that builds an engine name at
runtime from pieces (`obj["get" .. "Rigidbody"]`) is the one use the build cannot see: turn
**Project Settings > Player > Strip Engine Code** off for such a project (every module its
settings allow ships).

## Platforms

- **Android** needs the Android SDK's build-tools (35 or newer: aapt2, zipalign, apksigner), the
  target platform's `android.jar` and a JDK (keytool, jarsigner); an App Bundle also needs
  bundletool 1.17.2, which the first bundle build downloads from Google's GitHub releases into the
  user's cache (`jasperengine/tools/`). Its SHA-256 is pinned: a file that differs is not kept. A
  broken download is retried and resumed; offline, the build fails with the URL and where to put
  the file by hand. No NDK, CMake or Gradle. Set the SDK and JDK in Preferences > External Tools
  when they are not in `ANDROID_HOME` / `JAVA_HOME`.

  Signing: without a keystore the debug key (`~/.android/debug.keystore`, made when missing) signs
  the build, fine for testing and refused by stores (a release build warns `W-ANDROID-DEBUG-KEY`).
  A release keystore is Project Settings > Android > Keystore and Key Alias, RSA or EC, JKS or
  PKCS12; its passwords are asked when building (or `JASPER_ANDROID_KS_PASS` and
  `JASPER_ANDROID_KEY_PASS`), go to the tools through owner-only files, and are never saved or
  logged. The key is checked before any packaging: a wrong store password, a wrong key password
  and an unknown alias each fail at once and say which.

  A source export is an Android Studio project (`workspaces/androidstudio`, `./gradlew assembleDebug
  bundleDebug`). Gradle signs it with the debug key; set up a release key in Android Studio.

  Project Settings > Android > Package Format picks what Build makes:
  - **App Bundle** (`aab`, the default) is what Google Play takes. Play gives each device only its
    own ABI's libraries.
  - **One APK per ABI** (`apk-per-abi`) makes `<Game>-arm64-v8a.apk` and so on, for stores and
    sites outside Play. Give each device the APK of its ABI.
  - **Universal APK** (`apk`) is one installable file with every selected ABI's libraries. Every
    device downloads all of them, so choose it only on purpose.

  Build Settings shows each choice's download size, from the installed runtime's libraries.
  Unselect the ABIs a game does not need under Architectures (arm64-v8a covers nearly every
  device sold since 2017).
- **iOS** needs a Mac with a full Xcode (the Command Line Tools have no `xcodebuild`). The build
  uses `DEVELOPER_DIR` when set, else the active developer directory, else the Xcode in
  `/Applications` (the Mac's `xcode-select` setting is not changed). On other computers, or
  without Xcode, the build is the Xcode project ("Open on a Mac with Xcode"). Project Settings >
  iOS holds the team id and the export method (development, ad-hoc, app-store).
  - `--ios-simulator` (`ios_simulator` in `build_project`): an unsigned `.app` for the simulator,
    no account needed. `xcrun simctl install` and `launch` run it.
  - For a device, no Team ID: the project is compiled and linked for a device and stops before
    signing, with the warning `W-IOS-NO-TEAM`; the `.app` installs on nothing, and the Xcode
    project is there to open.
  - For a device, with a Team ID: `xcodebuild archive` and `-exportArchive` make the `.ipa`. This
    needs the team's account in Xcode > Settings > Accounts; without it the build fails and says
    so. What `tools/ci/ios_packaging.sh` runs is on the simulator, the device build up to signing
    and an unsigned archive; a signed `.ipa` has not been made in any test, as that needs an
    Apple developer account.
- **macOS**: the bundle is signed ad hoc by `codesign` on a Mac, or by `rcodesign` anywhere
  (Preferences > External Tools); an unsigned arm64 build does not start on Apple silicon.
- **Web**: the page runs the pthreads player when the page is cross-origin isolated (COOP/COEP
  headers, `_headers` for hosts that read it). On a host that sends no such headers it installs
  `coi-serviceworker.js` once, reloads, and runs the pthreads player; when the browser does not
  let the worker isolate the page, it runs the single-thread player. Project Settings > Player >
  Threading > Web: Multithreaded decides whether the pthreads player ships at all. Serve the
  folder over HTTP (`python3 tools/web/serve.py --root <folder>`): `file://` does not load
  WebAssembly.

## Running a player from scripts and CI

Every player takes the same options (the web from its URL query, `?quit-after=30&fixed-fps=60`):

| Option | Effect |
|---|---|
| `--headless` | no window (Linux still needs an X server: Xvfb in CI) |
| `--quit-after N` | quit after N frames |
| `--after-ready` | count the frames from "Engine ready:" (every scene loaded, no shader being made, no texture loading) |
| `--fixed-fps F` | a fixed time step of 1/F seconds |
| `--screenshot file.png` / `--screenshot-frame N` | write that frame (the last one by default); a frame drawn with shaders still being made or textures still loading waits for them (the game held), up to 120 s, then exits 4 naming them |
| `--fail-on-error` | exit 3 when an error is logged |
| `--ready-timeout S` | exit 6 when not ready after S seconds |
| `--resolution WxH`, `--scene <name\|id>`, `--content <dir>`, `--verify-content`, `--silent-audio`, `--log-level` | |
| `--audio-capture <dir>` | A desktop development player writes one audio capture session into `<dir>/<id>/`, on the deterministic driver; any other player exits 2. |
| `-- <args>` | the game's own arguments (Lua `System.args`) |

Exit codes: 0 done, 2 bad arguments, 3 an error was logged, 4 the screenshot failed, 5 the content
is not for this player, 6 not ready in time, 7 no display.

```
xvfb-run ./MyGame --headless --after-ready --quit-after 30 --fixed-fps 60 --screenshot game.png --fail-on-error
```

## Releases

`publishing.json` at the project root (committed; Build Settings > Publish edits it) names the
builds of a release:

```json
{
  "schemaVersion": 1,
  "releasesDir": "releases",
  "targets": {
    "linux": {"platform": "linux", "arch": "x64", "backend": "vulkan", "flavour": "release",
              "buildDir": "../MyGame-linux", "executable": "MyGame", "depotId": "1001",
              "deviceProfile": "steamdeck", "requiredFiles": []},
    "windows": {"platform": "windows", "arch": "x64", "backend": "d3d11", "flavour": "release",
                "buildDir": "../MyGame-windows", "executable": "MyGame.exe", "depotId": "1002"}
  },
  "steam": {"appId": "480", "buildDescription": ""},
  "deck": {"host": "steamdeck.local", "user": "deck", "remoteDir": "Games/MyGame"}
}
```

```
jasperengine-editor publish init                      # a starting publishing.json
jasperengine-editor publish prepare --release 1.0.0   # validate, copy, split symbols, release.json
jasperengine-editor publish verify 1.0.0              # nothing changed since prepare
jasperengine-editor publish steam-vdf 1.0.0 --preview
jasperengine-editor publish steam-upload 1.0.0 --user <build account> [--branch beta]
jasperengine-editor publish deck-deploy 1.0.0 --target linux
jasperengine-editor publish deck-smoke
jasperengine-editor publish deck-rollback
```

- **prepare** checks every build (each binary's format and architecture, exec bits, paths that
  differ only in case, symlinks that escape, Windows-reserved names and long paths,
  `steam_appid.txt`, the content the manifest lists, the notices, Android library alignment and
  `apksigner verify`, unsigned macOS arm64 code), copies it into
  `<releasesDir>/<release>/content/<target>`, moves debug symbols to `symbols/<target>` (`.pdb`,
  `.dSYM`, and on Linux the DWARF split off with `objcopy`/`strip` when binutils is installed),
  checks again and writes `release.json`. A release name is used once.

  Release builds of either method already ship stripped. A prebuilt player's symbols come from
  its runtime (`symbols/<target>/player`). A compiled build's link splits its own off; the build
  keeps them under `.jasperengine/symbols/` and prepare copies them to `symbols/<target>/game`.
- **verify** repeats the checks and compares every file with `release.json`.
- **steam-upload** verifies, writes the SteamPipe scripts and runs
  `steamcmd +login <user> +run_app_build <script> +quit`. SteamCMD keeps its own login session
  (log in once interactively, Steam Guard included); the editor never sees credentials. `SetLive`
  is for beta branches: promote public builds in Steamworks.
- **deck-deploy** copies the linux x64 target to the Deck (any ssh host: enable SSH in the
  Deck's desktop mode and add your key) into `<remoteDir>/<release>-<target>.incoming`, checks its
  checksums there, then makes it `current/` and keeps the build before as `previous/`.
  **deck-smoke** runs `current/<executable> --quit-after 600 --fail-on-error` there and returns
  its exit code and log tail. Verify prints the newest `GLIBC_x.y` the build needs
  (`glibc_floor`), which the Deck's runtime must provide.

## Notices

Every build has `licenses/<library>-LICENSE` for each library its player links and
`THIRD-PARTY-NOTICES.txt` with their NOTICE.md entries. `engine/notices.json` says which games
link each library (always, or with a player feature such as physics3d); `tools/ci/check-notices.py`
keeps it complete.

## What is tested where

The Linux and web prebuilt paths are built and run by `tools/build.sh players`, the player CLI test
and the web gate; Android's prebuilt APK runs in the emulator check. `tools/build.sh players` does
not build Windows, macOS or iOS runtimes, and nothing else in this repository does; Steam uploads and real
Steam Deck deploys are tested with stand-ins only (a SteamCMD-shaped script, ssh to a local
directory).
