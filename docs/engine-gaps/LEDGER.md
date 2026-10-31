# Engine gaps program: ledger

Owner request (2026-10-31): implement in the engine every gap found while porting JasperDev Arcade from Unity
([GAP-REPORT.md](GAP-REPORT.md)), with the best UX for human users (editor, Inspector) and AI agents (MCP tools,
docs, Lua stubs). Networking, URL opening and a general native-code bridge are in scope; **ads and in-app purchases
are not (yet)**. Product decisions are delegated to the lead agent: do what most users and agents expect of a game
engine, Unity-familiar names first.

## Process

1. Sync with `origin/main` before starting (done 2026-10-31: already up to date at `7f48d7e8d`).
2. Research (the Unity game, this codebase, Unity and other engines) and write one spec per feature in
   `docs/superpowers/specs/2026-10-31-<feature>-design.md` (goals, requirements, API, editor/MCP surfaces, tests,
   acceptance). No separate implementation plans.
3. Implement each spec on its own branch `gaps/<feature>` in its own worktree
   (`/var/lib/orquester/workspaces/jaspersito/engine-wt/<feature>`), at most 6 agents at a time (a queue).
4. A feature is done when it is complete, built, tested and reviewed. Then merge it into `main`, merge `origin/main`,
   build and test the merged tree, and push.

Build: native Linux toolchain installed on this host (gcc 13, CMake 3.28, Ninja, ccache with
`base_dir=/var/lib/orquester/workspaces/jaspersito` shared by all worktrees, xvfb for GL tests).
`cmake -S . -B build/editor -G Ninja -DCMAKE_BUILD_TYPE=Release -DJASPERENGINE_EDITOR_TESTS=ON
-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`, then build `jasperengine-editor` and
`jasperengine-editor-tests`. The HTML UI tests need the standalone engine build (`cmake -S engine`).

## Baseline (`main` at `b32a3c578`, before any gap work)

- Editor + `jasperengine-editor-tests` build with 0 warnings (Release, ~11 min cold on 8 cores).
- Unit tests: 944 run, 936 passed, 8 skipped, 0 failed.
- MCP e2e (`tools/mcp/run-tests.sh -j3`): all pass except 10 pre-existing failures: the 5 tests of
  `test_animation_assets.py` (need the external `build/fixture-unity` project) and 5 of `test_lattice_scene.py`
  (`test_click_and_drag_one_point`, `test_box_select`, `test_falloff_drag`, `test_prefab_instance_point_override`,
  `test_2d_stacked_points`: `KeyError: 'screen_points'`).

## Features

Status: `todo` / `spec` (being written) / `specced` / `impl` (being implemented) / `review` / `merged` (on
origin/main).

| # | Feature (branch `gaps/<name>`) | Covers | Status |
|---|---|---|---|
| 1 | `http` | HTTP client for Lua (GET/POST/HEAD, headers, timeouts, JSON/text/binary bodies, download to file, texture from URL), all platforms | specced |
| 2 | `native-bridge` | Lua to Java (Android), Objective-C/Swift (iOS/macOS), JavaScript (Web) calls with async callbacks, plus export hooks for extra Gradle/CocoaPods/SPM dependencies and manifest/plist entries | specced |
| 3 | `platform-services` | `openURL`, system language/locale, target frame rate / vsync, runtime orientation lock + iOS orientation setting + device orientation, app focus event | impl |
| 4 | `safe-area` | safe-area insets on Android/iOS (and desktop simulation), Lua API + change event, native UI safe-area anchoring, HTML UI `env(safe-area-inset-*)`, editor Game view notch simulation | specced |
| 5 | `ui-multitouch` | pointer ids through native UI and HTML UI events, per-pointer pressed state, on-screen stick/button controls | specced |
| 6 | `audio-mixer` | mixer buses with volume/mute and filters (low-pass/high-pass, ...), streamed music, PlayOneShot, fades | specced |
| 7 | `physics-2d` | 2D joint parameters (wheel spring/motor, hinge limits/motor, ...), runtime-sized chain/polygon colliders (no export-only caps), 2D contact stay | impl |
| 8 | `physics-contacts` | per-object collision/trigger callbacks (enter/stay/exit) for 3D and 2D, delivered on the main thread; 3D global handlers off the job threads | specced |
| 9 | `physics-3d` | damping, centre of mass, safe body-type switching, per-collider sensor/material, collider removal, 3D joint limits/motors/springs, cone fix, ragdoll, runtime-sized collider data and world capacity | specced |
| 10 | `anim-layers` | animation layers with bone masks and blending in the Animation Player | specced |
| 11 | `anim-retarget` | humanoid retargeting of clips between different rigs | specced |
| 12 | `anim-properties` | clip property channel: active state, colors, material and UI properties | specced |
| 13 | `lines-trails` | line width (thick lines) and a Trail Renderer | impl |
| 14 | `shader-globals` | user global shader uniforms (`Shader.setGlobal*`) | specced |
| 15 | `video-mp4` | MP4/H.264 playback (platform decoders) and streamed/URL video | specced |
| 16 | `coroutines` | engine-driven coroutines (wait seconds/realtime/frames/until), Invoke-style scaled timers, unscaled delta time, callbacks always on the main Lua thread | specced |
| 17 | `script-lifecycle` | onEnable/onDisable, enabling scripts, getScript/findObjectOfType, scene-wide find, event unregister | impl |
| 18 | `script-data` | JSON encode/decode, UserSettings hasKey/deleteAll/save | impl |
| 19 | `input` | key/mouse/button down/up edge queries, virtual axes and buttons (Input Manager-style), bound Touch with phases and deltas | impl |
| 20 | `persistence` | DontDestroyOnLoad (persistent objects and their sounds across scene loads) | specced |
| 21 | `ui-scaling-text` | canvas match-width-or-height scaling, per-character text effects (waves etc.), particles inside UI | specced |
| 22 | `particles` | start speed along the emitter shape, multi-key curves for every over-lifetime module, Unity ParticleSystem import | specced |
| 23 | `debug-overlay` | on-device log console and stats/FPS overlay | specced |
| 24 | `camera-shake` | camera shake component and presets (MilkShake-like), camera follow | specced |
| 25 | `ui-canvas-group` | Canvas Group for the native UI: group alpha, interactable, blocks raycasts, ignore parent groups; animatable (found by the animation spec: legacy clips animate CanvasGroup alpha) | specced |

## Log

- 2026-10-30: gap analysis (four parallel audits: game catalog, scripting/platform, physics, rendering/audio/UI).
  Results in [GAP-REPORT.md](GAP-REPORT.md).
- 2026-10-31: owner asked to implement everything above. Pulled `origin/main` (no new commits). Installed the
  native toolchain and started a baseline build of `main`. Ledger and gap report written.
- 2026-10-31: Android SDK (`/opt/android-sdk`, JDK 21) and emsdk 6.0.10 (`/opt/emsdk`) installed so Android and Web
  code can be compiled here. Six spec agents started, one per feature group: platform (#1-4), scripting
  (#16-18, #20, #23), physics (#7-9), animation + camera shake (#10-12, #24), rendering (#13-15, #22),
  input/UI/audio (#19, #5, #21, #6).
- 2026-10-31: specs committed for lines-trails, shader-globals, video-mp4, particles, input, ui-multitouch,
  ui-scaling-text, audio-mixer. Baseline on `main`: unit tests 936 passed / 8 skipped / 0 failed; MCP e2e needed
  dbus-x11 and xdotool (installed); the rest of the failures need the external `build/fixture-unity` project.
  FFmpeg installed for video-mp4. Implementation started early (free slots) for `input` and `lines-trails`
  (worktrees in `../engine-wt/<feature>`).
- 2026-10-31: specs committed for http, native-bridge, platform-services, safe-area, anim-layers (with an Animator
  Controller phase), anim-retarget, anim-properties, camera-shake (with a Camera Follow phase). Added feature #25
  `ui-canvas-group`. `platform-services` implementation started.
- 2026-10-31: specs committed for script-lifecycle (Unity lifecycle incl. start/onEnable/onDisable, init on first
  activation like Awake), coroutines, script-data, persistence, debug-overlay. `script-data` implementation started.
- 2026-10-31: specs committed for physics-2d, physics-contacts, physics-3d (merge order 2d -> 3d -> contacts, contacts
  after script-lifecycle). `physics-2d` implementation started.
- 2026-10-31: all 25 specs written and committed. The Canvas Group spec found the ECS signature was a 64-bit set
  with 61 component types used; widened to 128 on `main` (`a23dfd2f7`, unit tests 936/936) before any new
  component lands, and pushed `main` (specs, ledger, fix). Added a 16 GB swap file (builds of 6 agents ran the
  machine out of memory). `script-lifecycle` implementation started.
