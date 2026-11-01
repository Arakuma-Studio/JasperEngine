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
| 1 | `http` | HTTP client for Lua (GET/POST/HEAD, headers, timeouts, JSON/text/binary bodies, download to file, texture from URL), all platforms | merged |
| 2 | `native-bridge` | Lua to Java (Android), Objective-C/Swift (iOS/macOS), JavaScript (Web) calls with async callbacks, plus export hooks for extra Gradle/CocoaPods/SPM dependencies and manifest/plist entries | review |
| 3 | `platform-services` | `openURL`, system language/locale, target frame rate / vsync, runtime orientation lock + iOS orientation setting + device orientation, app focus event | merged |
| 4 | `safe-area` | safe-area insets on Android/iOS (and desktop simulation), Lua API + change event, native UI safe-area anchoring, HTML UI `env(safe-area-inset-*)`, editor Game view notch simulation | impl |
| 5 | `ui-multitouch` | pointer ids through native UI and HTML UI events, per-pointer pressed state, on-screen stick/button controls | merged |
| 6 | `audio-mixer` | mixer buses with volume/mute and filters (low-pass/high-pass, ...), streamed music, PlayOneShot, fades | review |
| 7 | `physics-2d` | 2D joint parameters (wheel spring/motor, hinge limits/motor, ...), runtime-sized chain/polygon colliders (no export-only caps), 2D contact stay | merged |
| 8 | `physics-contacts` | per-object collision/trigger callbacks (enter/stay/exit) for 3D and 2D, delivered on the main thread; 3D global handlers off the job threads | merged |
| 9 | `physics-3d` | damping, centre of mass, safe body-type switching, per-collider sensor/material, collider removal, 3D joint limits/motors/springs, cone fix, ragdoll, runtime-sized collider data and world capacity | merged |
| 10 | `anim-layers` | animation layers with bone masks and blending in the Animation Player | merged |
| 11 | `anim-retarget` | humanoid retargeting of clips between different rigs | impl |
| 12 | `anim-properties` | clip property channel: active state, colors, material and UI properties | impl |
| 13 | `lines-trails` | line width (thick lines) and a Trail Renderer | merged |
| 14 | `shader-globals` | user global shader uniforms (`Shader.setGlobal*`) | merged |
| 15 | `video-mp4` | MP4/H.264 playback (platform decoders) and streamed/URL video | review |
| 16 | `coroutines` | engine-driven coroutines (wait seconds/realtime/frames/until), Invoke-style scaled timers, unscaled delta time, callbacks always on the main Lua thread | merged |
| 17 | `script-lifecycle` | onEnable/onDisable, enabling scripts, getScript/findObjectOfType, scene-wide find, event unregister | merged |
| 18 | `script-data` | JSON encode/decode, UserSettings hasKey/deleteAll/save | merged |
| 19 | `input` | key/mouse/button down/up edge queries, virtual axes and buttons (Input Manager-style), bound Touch with phases and deltas | merged |
| 20 | `persistence` | DontDestroyOnLoad (persistent objects and their sounds across scene loads) | merged |
| 21 | `ui-scaling-text` | canvas match-width-or-height scaling, per-character text effects (waves etc.), particles inside UI | review |
| 22 | `particles` | start speed along the emitter shape, multi-key curves for every over-lifetime module, Unity ParticleSystem import | merged |
| 23 | `debug-overlay` | on-device log console and stats/FPS overlay | impl |
| 24 | `camera-shake` | camera shake component and presets (MilkShake-like), camera follow | review |
| 26 | `xcode26-dav1d` | iOS/macOS builds fail on Xcode 26.6 (pre-existing on the original main): dav1d's aarch64 assembly (src/arm/64/refmvs.S ...) does not assemble; fix and verify on a macOS box | todo |
| 25 | `ui-canvas-group` | Canvas Group for the native UI: group alpha, interactable, blocks raycasts, ignore parent groups; animatable (found by the animation spec: legacy clips animate CanvasGroup alpha) | merged |

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
- 2026-10-31: `script-data` implemented (9 commits; unit 963 passed / 8 skipped; Android/Web compile checks;
  Apple/Windows code not compilable here). Independent review started.
- 2026-10-31: **`script-data` merged** (review: 2 medium + 6 low defects, all fixed with regression tests; merged tree:
  0 warnings, unit 966 passed / 8 skipped, MCP script_data/stubs/docs/play/lua_threads 20 passed). `http` started on
  top of it. `input` and `platform-services` implemented; reviews found 5 and 7 defects, being fixed. Lead product
  calls for platform-services: keep-screen-on default applies on mobile only (desktop/Web only when the game asks);
  iPad `UIRequiresFullScreen` only when orientations are restricted.
- 2026-10-31: `input` review fixes done (5 defects, regression tests; unit 989). Merged into local `main`
  (`d29e2a4be`; conflicts in the glossary, test_build.py, Engine.cpp systemPause and TOOLS.md resolved keeping both
  sides); verification running. `platform-services` review fixes done (7 defects incl. a Windows openURL drive-letter
  launch and the generated project CMake link). `script-lifecycle` and `physics-2d` implemented, in review.
  `coroutines` and `anim-layers` started.
- 2026-10-31: compute: the host (8 cores) ran at load ~28. Rented a Hetzner cpx51 box (16 vCPU, 30 GB, Ubuntu
  24.04, ~$0.44/h, 48 h limit; the dedicated-core ccx63 was refused by the account's core limit). distcc through
  the port forward was slower per file than local (12.3 s vs 8.3 s), so the box runs the merge verification
  instead (`/root/verify.sh <ref> [MCP tests|ALL]`: build, unit suite, MCP, tool budget), leaving the host to the
  agents. macOS/Windows boxes to compile-check Apple/Windows code are planned in batches at merge time.
- 2026-10-31: **`input` merged** (verified on the build box: 0 warnings, unit 1019 passed, full MCP 820 passed with
  only the 10 baseline failures + `test_autosync::test_external_property_picked_up`, which passes alone (load
  flake) + the tool budget, raised by input's measured +76 with TOOLS.md regenerated). `http` and `lines-trails`
  implemented and reviewed (http: Windows sync-handle cancel race and Apple TLS session reuse among the findings;
  lead calls: raw bodies default to application/octet-stream, redirects from remote origins to local/private
  addresses are refused). New rule: before handoff, each feature agent merges current `main` into its branch and
  resolves conflicts itself, so merges into `main` are clean.
- 2026-10-31: owner lifted the 6-agent limit (the harness caps at 20). More compute: Hetzner box #2 (cpx51, 16
  vCPU) and #3 (ccx33, 8 dedicated; both Hetzner core limits now reached), Vultr box #4 (vc2-8c-32gb; Vultr's
  monthly-fee limit reached): 48 remote cores in total. Agents build and test there with `jbox` (rsync over SSH
  through forwarded ports 2201-2204; `/usr/local/bin/jbox`, assignments in `/etc/jbox/assign`). `coroutines`
  implemented (unit 1066; 10k coroutines + 1k timers ~0.055 ms/frame), in review. Started 12 more
  implementations, several stacked on branches still in review (camera-shake on coroutines; particles and
  shader-globals on lines-trails; physics-3d on physics-2d; persistence, physics-contacts, ui-multitouch on
  script-lifecycle; safe-area, ui-scaling-text on platform-services). Queued: native-bridge and video-mp4 (on
  http, worktrees ready), anim-properties and anim-retarget (after anim-layers).
- 2026-10-31: `coroutines` review: 1 memory-safety bug (isDone() stopping the waiting coroutine), C++ repeating
  timers losing state, an O(n) deactivation scan, paused unscaled time, stop vs immediate waitFor; fixes in
  progress. `native-bridge` and `video-mp4` started (on gaps/http). Only anim-properties and anim-retarget wait.
- 2026-10-31: `physics-2d` (10 review defects fixed) merged locally (`b377f20e0`); the full MCP suite on the merged
  tree found 5 failures its own run missed (agent guide 16523 > 16384 bytes; an old-prefab-override test; an old
  scene's re-save dropping legacy cap keys): not pushed, back to its agent. Box 2202 ran out of memory and was
  replaced; all boxes got 32 GB swap and lower per-agent parallelism (-j5 / -j3, MCP -j2). New handoff rule: the
  FULL MCP suite after merging main. `script-lifecycle` (7 review defects fixed incl. a process abort on strict
  __index modules and an Inspector use-after-free) merged locally (`6ce992460`) with one ScriptMessages mechanism
  and the guide trimmed to 16369 bytes; pushed together with physics-2d once the full suite is clean.
- 2026-10-31: **`physics-2d`, `script-lifecycle` and `lines-trails` merged and pushed** (lines-trails' conflicts
  resolved by the lead: UpdateFlags_Trail moved to bit 47, both HierarchyWindow creation rules, trail clearing
  before the activation-observer early return, Xcode project resynced). Final merged tree = the tree physics-2d's
  agent verified on box 2201: unit 1175 passed / 8 skipped; full MCP 849 passed, 18 skipped, only the 10 baseline
  failures. Agent guide 16319/16384 bytes. In review / fixing: http (merging main), coroutines, platform-services
  (merging main), ui-canvas-group, physics-contacts (2 high: 2D ignoreCollision on touching bodies, double destroy
  from an Exit callback), persistence. Box 2204 (Vultr) rebooted under memory pressure; sshd/swap now survive
  reboots on all boxes.
- 2026-10-31: **`persistence` merged and pushed** (7 review defects fixed incl. two C++ use-after-frees; merged
  tree: unit 1201 passed, full MCP 851 passed with only the 10 baseline failures). `physics-3d` reviewed (2 high:
  ragdoll impulse on the switch frame, six-DOF zero-stiffness drives locking; lead calls: fix trigger-vs-CCD
  properly, create the 3D world lazily) and `particles` reviewed (4 high incl. legacy mesh particles upgraded to
  billboards, end-of-cycle bursts lost): fixes in progress.
- 2026-10-31: merged locally, not yet pushed (full verification running on box 2201): `ui-canvas-group`,
  `physics-3d` (lead resolved UpdateFlags: CanvasGroup 48, Rigidbody_Params 49, Joint3D_Params 50, Ragdoll 51),
  `http` (lead resolved the generator.configure argument lists, ViewTools options, the Game view toolbar width,
  shutdown order, the agent guide). Found after merging: Canvas Group's always-exported composite shader adds a
  shader key to every saved scene (old scenes no longer re-save byte-identically) -> back to its agent; AsyncOwners
  must re-key owners when persistence moves objects -> http agent. Reviews done: camera-shake (its merge had made
  Lua `start` a plain call again), native-bridge (critical: JasperNative.h not findable by the Xcode app targets;
  CocoaPods not linked). Rented a macOS box (Blacksmith, arm64, Xcode 26.6, ~$4.80/h, 24 h) to build the iOS and
  macOS targets at merge time: Apple code had never been compiled before.
- 2026-10-31: macOS box session (~$3.24): merged main (c7ea32946) and gaps/platform-services build the "JasperEngine
  macOS" and "JasperEngine iOS" app schemes with 0 errors once dav1d's assembly is disabled locally. That dav1d
  failure also happens on the original baseline commit: added as feature #26. The Mac was destroyed; it will be
  rented again for one batch check (native-bridge, safe-area, video-mp4, #26). http follow-up merged
  (AsyncOwners re-keyed when persistence moves objects). Lead trimmed the agent guide to 16351 bytes and set the
  measured tool budget. Not pushed yet: waiting for ui-canvas-group's fix of an old-scene re-save regression.
  platform-services is doing its final merge of main (16 files conflicted).
- 2026-10-31 23:2x: the lead's session restarted, stopping every agent and remote job. All four Linux boxes
  survived (idle). Resumed 15 agents from their transcripts (platform-services mid-merge; review fixes on
  particles, shader-globals, physics-contacts, ui-scaling-text, native-bridge, camera-shake, coroutines,
  ui-canvas-group; final suites of audio-mixer; implementations of anim-layers, debug-overlay, safe-area,
  video-mp4; the ui-multitouch review, which an API limit had stopped). Tailscale: this host had none before the
  lead installed it; it joins the owner's tailnet through a login link (no auth key) to build on the owner's Mac.
- 2026-11-01: the owner's Mac (macOS 26.2, 14 cores, Xcode 26.6 with the iOS 26.5 SDK and simulator) is reachable
  over Tailscale (`ssh mac`, user mapacho, a dedicated key; plain sshd via `tailscale nc`). Apple compile checks run
  there in `~/engine-gaps-check` (`applecheck.sh <ref>`: the iOS Simulator and macOS app schemes, nice'd, 8 jobs,
  DEVELOPER_DIR set per command, dav1d asm disabled locally per #26). The rented macOS box is no longer needed.
  ui-multitouch review: 2 high (a press released outside the canvas never ends; HTML press-on-enter buttons stay
  pressed under implicit capture), fixes in progress. video-mp4 implemented, in review. Box 2204 (Vultr) is slow
  from CPU steal; agents moved off it.
- 2026-11-01: Mac check of main (b6989373c): iOS and macOS app schemes 0 errors. shader-globals review fixes done
  (framebuffer-global lifetime, cached texture resolution, Android context loss, renames refused in Play) and merged
  locally (82890abd7). Its agent found a physics-3d merge bug on main: the 3D world capacity and the 3D physics
  switch shared the project.yaml key `physics3D` -> lead moved the capacity to `physics3DWorld` with a round-trip
  test (91e646d41). video-mp4 review: 4 confirmed high defects (exports without 3D physics fail to link; video
  render textures by raw pointer; persistent players lose their output; a regex stack overflow in the Android
  export) + platform issues; fixes in progress.
- 2026-11-01: verification of 91e646d41 (ui-canvas-group, physics-3d, http, physics3DWorld fix) on box 2201: 0
  warnings, unit 1354 passed + ConvexDecomposition.ConcurrentRequests (load flake: passes 5/5 alone, added to the
  known flakes), full MCP 862 passed with only the 10 baseline failures + test_stochastic_tiling resave (the
  ui-canvas-group regression, fix in progress). platform-services merged locally (cbf8f65a9; lead resolved
  Generator::configure taking both shaderGlobals and playerSettings). Known load flakes: test_autosync::
  test_external_property_picked_up, ConvexDecomposition.ConcurrentRequests, CanvasGroupTest.PerformanceBudgets
  (being made robust).
- 2026-11-01: rule change: agents merge main ONCE at handoff and do not chase it; the lead merges later main changes
  and verifies combined trees in batches. video-mp4 review fixes done (18 defects + spec items; unit 1436, htmlui
  3114, MCP only baseline + the known canvas resave). It found a merge-blocker on local main: physics-3d sources
  compile even with JASPERENGINE_PHYSICS_3D=OFF, which the Exporter sets for projects without 3D physics (every
  2D-only export would fail) -> physics-3d agent fixing, also checking PHYSICS_2D=OFF and both OFF. audio-mixer
  review: 2 high (live mixer edits reallocating SoLoud voice buffers -> dropouts; voice limit beyond SoLoud's 255
  -> audio-thread overflow / Windows break), fixes in progress.
- 2026-11-01: merged locally (not pushed): physics-contacts, particles, ui-canvas-group's resave fix,
  ui-multitouch, coroutines (lead resolved ScriptMessages enum/strings for particles + pointer callbacks, focus loss
  releasing input AND cancelling UI pointers with one onFocus call, Engine time statics, stub generator blocks; the
  ScriptMessages static_assert now < 64). Mac check of the batch through physics-contacts: iOS and macOS 0 errors.
  Push blocked only on the physics-3d fix for builds with 3D physics OFF. The agent guide hit 16383/16384 bytes:
  new task `guide-headroom` restructures it to ~15 KB (details to docs/mcp/guide). New task `asan-sweep`: the unit
  suite under ASan+UBSan with root-cause fixes (leads: a stack-use-after-scope in Catalog::copyPropertyValue
  from physics-2d's override migration; a heap error once in PersistenceTest).
- 2026-11-01: the physics-3d fix for builds with physics OFF merged (f873f2ce9; tools/check-physics-off.sh). The
  anim-layers agent found main did not compile: the coroutines merge had brought a second identical
  Engine::getUnscaledDeltatime -> fixed (94a22289c); lead now builds after each batch of merges before moving on.
  Final verification of the batch (Linux full suites + physics-off variants on box 2201, iOS/macOS on the owner's
  Mac) running. anim-layers implemented (both phases), in review; anim-properties and anim-retarget started on it.
- 2026-11-01: batch verification of 94a22289c (ui-canvas-group, physics-3d, http, shader-globals, platform-services,
  physics-contacts, particles, ui-multitouch, coroutines + lead fixes): Linux 0 warnings, unit 1601 passed, full MCP
  only the 10 baseline failures (+ the budget limit, corrected); physics-off builds (3d-off, 2d-off, both-off) pass.
  The Mac check found an iOS/macOS compile error: two headers named OnScreenControls.h (component UI and HTML UI)
  and a bare include that Xcode resolved to the wrong one -> include by path (4a627a9e8). guide-headroom merged
  (guide 14966 bytes); anim-layers merged (review fixes done). camera-shake, ui-scaling-text and video-mp4 conflict
  heavily with main now (they carry older copies of merged features): each does one final merge of main itself.
  Re-verifying the tip (4a627a9e8) on Linux and the Mac before pushing.
- 2026-11-01: the tip fda5c0d0f is verified and pushed. Linux (at 4a627a9e8; fda5c0d0f only changes the Xcode
  project and this brief): 0 warnings, unit 1634 passed (+2 known timing flakes), full MCP 917 passed with only the
  10 baseline failures. Owner's Mac: iOS and macOS build with 0 errors, after a second Apple-only fix: the RmlUi
  Xcode targets did not compile ui-multitouch's new Source/Core/ContextPointer.cpp (link error) -> synced with
  tools/htmlui/xcode_sync_rmlui.py; the brief now lists all four Xcode sync scripts. Pushed: http,
  platform-services, ui-multitouch, physics-contacts, physics-3d, anim-layers, shader-globals, coroutines,
  particles, ui-canvas-group (+ guide-headroom). Next batch: camera-shake (final merge done, 0ea304609),
  audio-mixer, native-bridge, then ui-scaling-text, video-mp4, safe-area as their final merges land.
