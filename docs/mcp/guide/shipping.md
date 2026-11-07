# Shipping: builds, player runtimes and releases

What `build_project`, `run_build` and the `shipping` tool group do (docs/specs/S30-shipping.md).
The human version is `docs/shipping.md`; the command line is `jasperengine-editor build`,
`players` and `publish` (each with `--help`).

## Building a game

`build_project mode: player, platform: <windows|linux|macos|android|ios|web>` builds a game,
as a job. `mode: desktop` is this computer's platform and `mode: web` the web (the old names).

- **method** `auto` (default: the project's Build Settings choice): a **prebuilt player** (no
  compiler, seconds) unless the project needs native code: C++ scripts, `ProjectBuild.cmake`,
  AdMob, or `method: compile` (content size never is: components have no fixed capacities). Then it compiles
  from source (minutes; this computer's platform and the web only). The result says which:
  `method_used`, `compile_reasons[]`.
- `prebuilt` fails with the reasons when the project needs native code; `compile` always compiles.
- **development**: the development flavour (profiler, `--profile`); else release.
- **graphics_api**: `opengl`/`glcore`, `gles3`/`opengles`, `vulkan`, `d3d11`, `metal`. Each
  platform has its list (get_build_info `platforms`).
- **android_format**: `aab` (Google Play), `apk-per-abi` or `apk` (one universal APK of every ABI).
  The default is the project's (Project Settings > Android > Package Format, `aab` unless set).
  Signing: Project Settings > Android keystore with
  `JASPER_ANDROID_KS_PASS` / `JASPER_ANDROID_KEY_PASS` in the editor's environment; without a
  keystore the debug key signs it (warning `W-ANDROID-DEBUG-KEY`).
- iOS needs a Mac with Xcode; elsewhere the result is the Xcode project and a note.

The result adds `player_id`, `modules` (the engine modules the game ships: Strip Engine Code
leaves out the HTML UI, video, the runtime shader compiler, 3D or 2D physics when nothing reaches
them), `module_reasons` (the first evidence of each), `content_manifest`, `executable`, `artifacts` and
`warnings[{code, message}]` (`W-MAC-UNSIGNED`, `W-ICON-NONWINDOWS`, ...). The output folder is
replaced only when the build succeeds (the previous build stays until then).

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

Every mode's result carries `warnings[{code, message}]` and `notes[]`: `W-EXPORT-FOLDER-SKIPPED`
(an earlier export inside the project, left out: move it out), `W-SCENE-NOT-SAVED` (an untitled
scene shipped as it is: save_scene with a path), `W-SHADER-FORK-MISSING` (variants of a deleted
fork left out), `W-UNREADABLE-FOLDER`; a note when D3D11 shaders ship as HLSL source (built off
Windows). A failed build's message names what failed: the file a copy or write could not make, or
the first compiler error (`errors[]` lists them with file and line). `output_dir` inside the
project (or its assets or Lua folder) is refused, except below a hidden folder or `build/`. A web
build's page is `index.html`.

Before exporting: the scenes saved, the Console clean in play, the start scene right
(`edit_build_scenes`).

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

## Releases (group `shipping`)

`publishing.json` (project root, committed) lists the targets: `{platform, arch, backend,
flavour, buildDir, executable, depotId?, deviceProfile?: "steamdeck", requiredFiles[]}`, plus
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
