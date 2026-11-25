# Shipping: builds, player runtimes and releases

What `build_project`, `run_build` and the `shipping` tool group do (docs/specs/S30-shipping.md).
The human version is `docs/shipping.md`; the command line is `jasperengine-editor build`,
`players` and `publish` (each with `--help`).

## Building a game

`build_project mode: player, platform: <windows|linux|macos|android|ios|web>` builds a game,
as a job. `mode: desktop` is this computer's platform and `mode: web` the web (the old names).

- **method** `auto` (default: the project's Build Settings choice): a **prebuilt player** (no
  compiler, seconds) unless the project needs native code: C++ scripts, `ProjectBuild.cmake`,
  native plugins the player cannot carry, or `method: compile` (content size never is: components have no fixed capacities). Then it compiles
  from source (minutes; this computer's platform and the web only). The result says which:
  `method_used`, `compile_reasons[]`.
- `prebuilt` fails with the reasons when the project needs native code; `compile` always compiles.
- The job has an `estimate` from submit (`seconds`, `basis` model or history; `remaining_seconds`
  in get_job). Its result's `report.fallbacks` names each downgrade (compiled instead of prebuilt,
  a bigger player variant, textures left uncompressed): tell the user.
- **development**: the development flavour (profiler, `--profile`); else release.
- **graphics_api**: `opengl`/`glcore`, `gles3`/`opengles`, `vulkan`, `d3d11`, `metal`. Each
  platform has its list (get_build_info `platforms`).
- **android_format**: `aab` (Google Play), `apk-per-abi` or `apk` (one universal APK of every ABI).
  The default is the project's (Project Settings > Android > Package Format, `aab` unless set).
  Signing: Project Settings > Android keystore with
  `JASPER_ANDROID_KS_PASS` / `JASPER_ANDROID_KEY_PASS` in the editor's environment; without a
  keystore the debug key signs it (warning `W-ANDROID-DEBUG-KEY`).
- iOS needs a Mac with Xcode; elsewhere the result is the Xcode project and a note.
- **Native plugins**: a prebuilt build carries what it can and lists it in `carried` (also
  `get_build_info` `platforms[*].carried`); the rest is a `compile_reasons` entry naming the files.
  iOS carries Objective-C/Swift plugins, SDKs, Swift packages, frameworks, Info.plist entries,
  entitlements; Web carries `plugins/native/web/*.js` and `web.script_urls`; Android carries
  `extra_permissions` and the manifest XML; C++ plugins, Java/Kotlin, Gradle dependencies, CocoaPods
  and macOS plugins compile. Warning `W-NATIVE-MOCK`: a Lua mock would answer for a plugin with no
  code on that platform. Details: `jasper://docs/native-bridge` section 8.

The result adds `player_id`, `modules` (the engine modules the game ships: Strip Engine Code
leaves out the HTML UI, video, the runtime shader compiler, 3D or 2D physics when nothing reaches
them), `module_reasons` (the first evidence of each), `content_manifest`, `executable`, `artifacts` and
`warnings[{code, message}]` (`W-MAC-UNSIGNED`, `W-ICON-NONWINDOWS`, ...). The output folder is
replaced only when the build succeeds (the previous build stays until then).

**Size.** Every result says what the build weighs: `build_bytes`, `build_categories` (models,
textures, audio, scripts, scenes, engine, other), `top_files` (the 5 largest) and `build_report`
(its `build-report.json`, the Build Report window's). A web build adds `download_bytes_gzip`, an
estimate of what a browser downloads (gzip -9 of the files the page loads). When the user names a
size limit, set it as the Download Budget: `set_project_settings`
`player.download_budget_mb.<web|mobile|desktop>` (MB; web holds the download estimate, the others
the build's bytes; 0 is off). A build over it still succeeds, with `W-BUILD-OVER-BUDGET`,
`download_budget {budget_bytes, actual_bytes, largest_categories}` and a `next` naming the tools
that inspect the largest category: follow it before shrinking anything.

A prebuilt build needs the player runtime `<platform>-<arch>-<backend>-<flavour>` (a release one
also comes in variants of fewer modules, `-ui`, `-core`, `-lite3d`, `-lite2d`: the build takes the
smallest installed one that has its modules, `W-PLAYER-VARIANT` names a smaller one to install):
`get_build_info players[]` lists them with `installed`; `install_player_runtime {id}` downloads
one (a release editor), `{archive}` installs a local `.zip`. A development editor uses local
runtimes (`tools/build.sh players`, then `JASPER_PLAYERS_DIR`). Without the runtime, `auto`
compiles when this computer can.

`get_build_info platforms.<name>.tools_missing[]` names what a platform's build lacks (Android:
build-tools, platform, JDK; the bundletool jar is downloaded by the first `aab` build, its SHA-256
pinned; iOS: a Mac with a full Xcode). An iOS device build without a team stops before signing with
`W-IOS-NO-TEAM`; a wrong Android keystore password or alias fails before any packaging and says which.

**Shader overrides** (Build Settings > Shaders): a build compiles the shader variants the scenes
need, plus the ones added by hand and minus the ones excluded. `get_build_info include_shaders`
lists every key; `edit_build_shaders` (group `shipping`) adds, excludes, removes or clears
overrides without building, one project undo step, saved to project.yaml at once (the Build
Settings window shows it while open). `build_project shaders` does the same before a build. Exclude
a variant only when nothing draws with it: an excluded shader the game needs draws nothing.

**What ships** (Project Settings > Player > Build, `set_project_settings`): `player.asset_stripping`
`ship_all` ships every asset (a project without the key); `referenced_only` (new projects) ships
what the build's scenes and project settings reference, the files Lua and C++ string literals
name, and `player.always_include` (project-relative globs, `*`, `**`). `player.exclude_from_build`
leaves its globs out under either mode. Lua scripts always ship. The result's `stripped_count`,
`stripped_bytes` and `build_report` (its `stripped[]`: path, bytes, reason) say what was left out;
a Development Build's player logs "<path> was stripped from the build" when a script loads one.
A path a script builds at runtime (`"level" .. n`) needs its files in `player.always_include`.

Every mode's result carries `warnings[{code, message}]` and `notes[]`: `W-EXPORT-FOLDER-SKIPPED`
(an earlier export inside the project, left out: move it out), `W-SCENE-UNSAVED` (a scene or
prefab with unsaved changes, or an untitled scene, shipped as the editor shows it; its file is not
written: save_scene to keep it), `W-SHADER-FORK-MISSING` (variants of a deleted
fork left out), `W-UNREADABLE-FOLDER`; a note when D3D11 shaders ship as HLSL source (built off
Windows). A failed build's message names what failed: the file a copy or write could not make, or
the first compiler error (`errors[]` lists them with file and line). `output_dir` inside the
project (or its assets or Lua folder) is refused, except below a hidden folder or `build/`. A web
build's page is `index.html`.

Before exporting: the scenes saved, the Console clean in play, the start scene right
(`edit_build_scenes`).

## Privacy manifest

Every iOS and macOS build ships one: the engine's required-reason APIs plus `set_project_settings`
`apple.privacy_tracking` (bool), `apple.privacy_tracking_domains` (host names),
`apple.privacy_collected_data` (`[{type: "DeviceID", linked, tracking, purposes:
["ThirdPartyAdvertising"]}]`), `apple.privacy_accessed_apis` (`[{type: "DiskSpace", reasons:
["E174.1"]}]`), plus the `PrivacyInfo.xcprivacy` files of static plugins and SDK bundles.
Undoable (scope project). Read the result: `native_plugins {action: "privacy_manifest", platform:
"ios"}` (`xml`, `sources`, `warnings`: `W-PRIVACY-DOMAINS`, `W-PRIVACY-ATT`). Never write
`PrivacyInfo.xcprivacy` into an export: the next build replaces it.

## Running a build

`run_build` starts a desktop build. With `quit_after` (frames), `fixed_fps`, `after_ready`
(count from "Engine ready:"), `headless` (no window) or `screenshot_frame`, the player runs
itself: it quits after the frames, writes its own frame as the screenshot (no window capture;
it works headless) and its exit code is the verdict:

| code | meaning |
|---|---|
| 0 | ran and quit |
| 2 | bad arguments |
| 3 | an error was logged (`--fail-on-error` in `args`) |
| 4 | the screenshot failed |
| 5 | the content is not for this player (rebuild it) |
| 6 | not ready before `--ready-timeout` |
| 7 | no display |

A web build reads the same options from the page's query: `index.html?quit-after=30&fixed-fps=60`.

## Devices

The loop on an Android device or emulator, an iOS Simulator (a Mac with Xcode) or the browser:

1. `get_build_info devices:true` lists the Run Devices (`id`, `usable`, `why`; `installed` with
   `patchable` / `why_not` when this project is installed there). Probing spawns adb and simctl:
   ask only when you need the list.
2. `build_project platform:"android" development:true run_device:"<id>"` (or `"auto"`, the only
   usable device; `"web"` for a web build) builds, installs or patches, and launches. A Development
   Build patches by default (`patch`): only the changed content is pushed, no reinstall
   (`device.installed: "patched"`); when it cannot, it installs in full and `next` says why.
3. `read_console categories:["device"]` reads the game's log (`[<device>] <line>`).
4. `run_build device:"<id>"` runs the recorded build on the device for `seconds` with `args`,
   `fixed_fps`, `quit_after`, `after_ready`, and returns `log`, `errors`, `crashed`, `fatal_lines`
   and the device's screenshot; `keep_running` leaves it running. `input` is desktop-only: drive a
   device's game from Lua or its arguments.

A web run gives `device.url` (a headless editor opens no browser).

## Build Report (group `shipping`)

Every export writes `build-report.json` and a copy in the project's history (10 per platform).
`get_build_report` reads it:

- A build grew, or someone asks what changed: `get_build_report {compare_to: "previous"}` (or a
  history id or a path): per category and per file changes keyed by source asset, largest first.
- A file should not ship, or you are asked why it does: `get_build_report {referenced_by: [paths]}`
  gives the chain back to project.yaml or a script. `unreferenced` means no scene, prefab,
  material, setting or script reaches it in the index; a script may still load it by a computed
  path, so ask before deleting it.
- `report` picks another report (`last` by default, a history id or a path); its `left_out` lists
  sources shipped as something else (cooked, atlas pages, baked) and why.
- Size budgets: `set_project_settings {values: {"player.size_budgets": [{platform, category,
  limit_mb}], "player.size_budget_action": "warn"|"fail"}}`. Over a limit the build warns
  `W-SIZE-BUDGET` (or fails with `fail`); the report's `budgets` has the rows.

## Build Profiles (group `shipping`)

Make a Build Profile when one project ships more than one variant: a demo, a store edition, a
white-label copy, a test build with cheats. A profile is a platform plus overrides; the project's
own settings never change. Do not rename the project, edit its Scenes In Build or flip settings
before a build and back after it.

1. `edit_build_profile {action: create, name, platform, values?, overrides?}`: `values` holds
   `product_name`, `identifier`, `version_suffix`, `icon`, `store`, `store_title`, `backend`,
   `development`, `output_dir`, `scenes` (paths or GUIDs of the project's scenes), `start_scene`,
   `defines`, `exclude_labels`, `exclude_paths`. `overrides` takes `get_project_settings` keys of
   the categories `player`, `graphics`, `quality` and the profile's platform (`android.*` for an
   Android profile); others are refused, naming the allowed ones. A refused edit writes nothing.
2. `edit_build_profile {action: get, name}`: the stored values, `resolved` (each value the build
   writes and where it comes from) and `store_findings`.
3. `build_project {profile}`: platform, Graphics API, Development Build and folder come from the
   profile; a `platform`, `graphics_api` or `development` that differs is refused. The result
   names `profile`; store limits are `warnings[]` (`W-STORE-TITLE-LENGTH`, `W-LABEL-TRUNCATED`).
4. `edit_build_profile {action: activate, name}` makes Play use the profile's name, identifier,
   version, defines and Scene List (per user, not undoable); `name: null` for none.

Scripting Define Symbols are runtime values (`System.isDefined("DEMO")`, `System.getBuildInfo()`):
the code behind them still ships. Code that must not ship goes in a folder `exclude_paths` names.
A shipped scene that uses an excluded asset fails the build naming both. Store limits: Google Play
and App Store titles 30 characters, home-screen labels warn above 12. A publishing.json target may
name `"profile"`: `publish_prepare` refuses a build of another profile, another platform, backend
or flavour, or a store title over its limit. A Build Profile is not a Device Profile
(`deviceProfile`, S56).

## Releases (group `shipping`)

`publishing.json` (project root, committed) lists the targets: `{platform, arch, backend,
flavour, buildDir, executable, depotId?, deviceProfile?: "steamdeck", requiredFiles[], profile?}`, plus
`steam{appId}` and `deck{host, user, remoteDir}`. `jasperengine-editor publish init` writes one.

- `publish_prepare {release}`: validates every target's build (architecture, exec bits, case
  collisions, symlinks, Windows names, `steam_appid.txt`, the content and notices, Android
  alignment and signature), copies it to `<releasesDir>/<release>/content/<target>`, moves debug
  symbols to `symbols/`, writes `release.json`. An existing release name is refused.
- `publish_verify {release}`: the same checks again, and every file against `release.json`
  ("Release content changed after preparation: <target>/<path>").
- `publish_steam_vdf {release, preview, branch?}`: `steampipe/app_build_<app>.vdf` and the
  depot scripts. `SetLive` on `default`/`public` is refused. Uploading is a person's step
  (`jasperengine-editor publish steam-upload`, SteamCMD's own login): no tool uploads.
- `publish_deploy_deck {release, target?, smoke?}`: the linux x64 target over ssh/scp to the
  Deck's `remoteDir/current` (checksums checked there; the build before becomes `previous`).
  `smoke` runs it there for 600 frames with `--fail-on-error`. `publish deck-rollback` restores
  the previous build.
