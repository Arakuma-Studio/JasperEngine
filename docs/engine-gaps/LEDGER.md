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

## Features

Status: `todo` / `spec` (being written) / `specced` / `impl` (being implemented) / `review` / `merged` (on
origin/main).

| # | Feature (branch `gaps/<name>`) | Covers | Status |
|---|---|---|---|
| 1 | `http` | HTTP client for Lua (GET/POST/HEAD, headers, timeouts, JSON/text/binary bodies, download to file, texture from URL), all platforms | todo |
| 2 | `native-bridge` | Lua to Java (Android), Objective-C/Swift (iOS/macOS), JavaScript (Web) calls with async callbacks, plus export hooks for extra Gradle/CocoaPods/SPM dependencies and manifest/plist entries | todo |
| 3 | `platform-services` | `openURL`, system language/locale, target frame rate / vsync, runtime orientation lock + iOS orientation setting + device orientation, app focus event | todo |
| 4 | `safe-area` | safe-area insets on Android/iOS (and desktop simulation), Lua API + change event, native UI safe-area anchoring, HTML UI `env(safe-area-inset-*)`, editor Game view notch simulation | todo |
| 5 | `ui-multitouch` | pointer ids through native UI and HTML UI events, per-pointer pressed state, on-screen stick/button controls | todo |
| 6 | `audio-mixer` | mixer buses with volume/mute and filters (low-pass/high-pass, ...), streamed music, PlayOneShot, fades | todo |
| 7 | `physics-2d` | 2D joint parameters (wheel spring/motor, hinge limits/motor, ...), runtime-sized chain/polygon colliders (no export-only caps), 2D contact stay | todo |
| 8 | `physics-contacts` | per-object collision/trigger callbacks (enter/stay/exit) for 3D and 2D, delivered on the main thread; 3D global handlers off the job threads | todo |
| 9 | `physics-3d` | damping, centre of mass, safe body-type switching, per-collider sensor/material, collider removal, 3D joint limits/motors/springs, cone fix, ragdoll, runtime-sized collider data and world capacity | todo |
| 10 | `anim-layers` | animation layers with bone masks and blending in the Animation Player | todo |
| 11 | `anim-retarget` | humanoid retargeting of clips between different rigs | todo |
| 12 | `anim-properties` | clip property channel: active state, colors, material and UI properties | todo |
| 13 | `lines-trails` | line width (thick lines) and a Trail Renderer | todo |
| 14 | `shader-globals` | user global shader uniforms (`Shader.setGlobal*`) | todo |
| 15 | `video-mp4` | MP4/H.264 playback (platform decoders) and streamed/URL video | todo |
| 16 | `coroutines` | engine-driven coroutines (wait seconds/realtime/frames/until), Invoke-style scaled timers, unscaled delta time, callbacks always on the main Lua thread | todo |
| 17 | `script-lifecycle` | onEnable/onDisable, enabling scripts, getScript/findObjectOfType, scene-wide find, event unregister | todo |
| 18 | `script-data` | JSON encode/decode, UserSettings hasKey/deleteAll/save | todo |
| 19 | `input` | key/mouse/button down/up edge queries, virtual axes and buttons (Input Manager-style), bound Touch with phases and deltas | todo |
| 20 | `persistence` | DontDestroyOnLoad (persistent objects and their sounds across scene loads) | todo |
| 21 | `ui-scaling-text` | canvas match-width-or-height scaling, per-character text effects (waves etc.), particles inside UI | todo |
| 22 | `particles` | start speed along the emitter shape, multi-key curves for every over-lifetime module, Unity ParticleSystem import | todo |
| 23 | `debug-overlay` | on-device log console and stats/FPS overlay | todo |
| 24 | `camera-shake` | camera shake component and presets (MilkShake-like) | todo |

## Log

- 2026-10-30: gap analysis (four parallel audits: game catalog, scripting/platform, physics, rendering/audio/UI).
  Results in [GAP-REPORT.md](GAP-REPORT.md).
- 2026-10-31: owner asked to implement everything above. Pulled `origin/main` (no new commits). Installed the
  native toolchain and started a baseline build of `main`. Ledger and gap report written.
