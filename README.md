# JasperEngine

**A lightweight native C++ engine for 2D and 3D games, with a visual editor and Lua scripting.**

JasperEngine is based on [Doriax](https://github.com/doriaxengine/doriax) by Eduardo Doria and contributors. See [NOTICE.md](NOTICE.md).

## Features

**Editor**

- A Unity-style editor: Hierarchy, Inspector, Project, Console, and separate Scene and Game views, with Unity's menus, toolbar, and shortcuts
- Build 2D and 3D scenes from GameObjects with sprites, tilemaps, models, cameras, lights, and reusable Prefabs
- Edit components, assets, animations, bones, and terrain without leaving the editor
- Synthesize textures from an example: extend, make tileable, inpaint, or guide them, with matching normal/roughness/height maps (Window > Texture Synthesis, the `synthesize-texture` CLI, or the `synthesize_texture` MCP tool)
- Generate normal maps for sprites from pixel art to 4096 px painted art (high-resolution sprites are inflated without creases and their relief scales with their size), sheets, GIF/APNG animations and tilesets so 2D lights give them volume: presets, a lit preview with a movable light, frames from the Sprite, recipes that know when the sprite changed, paint corrections that survive regeneration, and Toon steps (normals snapped to a few directions for crisp, cel-shaded light) (Window > Sprite Normal Map, the Project window's Generate/Regenerate Normal Map, the Inspector's Normal Map buttons, the `generate-sprite-normals` CLI, the `generate_sprite_normals` MCP tool, and `check --normals` for stale or mismatched maps; a port of Sprite Fusion's sprite-normals)
- Tune a fluid simulation from presets in the Inspector, preview it in edit mode and paint dye into it with a Scene view brush
- Place objects with edit-time physics: press G to turn Physics on and the Move, Rotate and Scale tools slide the selection along the scene and stop it against what it touches, hold Space to drop it until it rests, Shift-drag to snap it across surfaces; each gesture is one undo step (Window > Physics Placement, the `place-physics` CLI, or the `place_with_physics` MCP tool)
- Keyframe characters and objects Unity-style in the Animation window: record Scene-view and Inspector edits, edit keys in a dopesheet or curves (with Unity's key tangents), animate component properties (GameObject active, enabled checkboxes, UI and material colours, Canvas Group alpha, Rect Transform, lights, cameras, audio, sprite frames, Lua script fields), add events that call Lua, and save shared Animation Clips (`.anim`) that any hierarchy with the same child names can play; extract clips from glTF/FBX models, convert old keyframe Animations and bake pose animations to clips: see [docs/animation-clips.md](docs/animation-clips.md)
- Animate humanoid characters pose to pose: drag hands and feet in the Scene view (IK, automatic grounding, onion skin), pick a feel (snappy, cartoon, rubbery, heavy, stop-motion), humanize it, or start from 10 built-in templates (walk, run, jump, wave...): see [docs/pose-animation.md](docs/pose-animation.md)
- Bake any animation into a Vertex Animation (`.vat`, see [docs/vertex-animation.md](docs/vertex-animation.md)) that a crowd of instanced characters plays on the GPU: model clips, Animation Clips, Animation Players, pose animations and lattices, in Vertices or Bones mode with Mobile & Web or Desktop presets and a size estimate before baking; import Houdini (SideFX Labs VAT 3: soft bodies, rigid-body destruction, fluids) and OpenVAT exports, rebake when a source changes, and scatter a crowd with Create Crowd (Window > Animation > Vertex Animation Baker, the `bake-vat` CLI, or the `bake_vertex_animation` MCP tool)
- Write Lua and C++ in the integrated code editor with engine API completion
- Fork the built-in shaders or write custom post-process passes, with live viewport recompilation
- Play the game in the Game view, then build it from Build Settings as a standalone CMake project, a desktop binary, or a web build

**Engine**

- Shared ECS runtime for 2D and 3D scenes, with scene layers and serialization
- Sprites, tilemaps, and polygons, plus 2D lights, normal maps (OpenGL convention, green up; set from scripts with `setNormalTexture`, read by the built-in sprite material and `library:sprite`, correct on mirrored sprites), and 2D shadow casters
- glTF, OBJ, and FBX models with skeletal animation, morph targets, and mesh instancing (the editor converts FBX to glTF with [ufbx](https://github.com/ufbx/ufbx): meshes, materials and textures, skins, animations, blend shapes, node hierarchy, units and axes; reimported when the file or its textures change)
- PBR materials with dynamic lights and cascaded shadows, sky-driven IBL, SSAO, fog, and planar mirrors
- A library of 40 ready-made shaders (toon, matcap, water, glass, outlines, sprites, skies, post effects...), per-pixel, per-vertex or mixed lighting with any number of lights, and a mobile-first post-processing stack (tone mapping and grading in the shaders, one uber pass, quarter-resolution bloom): see [docs/shaders](docs/shaders/README.md)
- Keyframe tracks, tweens with easing, crossfade blending, and sprite-sheet animation
- Animation Clips played by the Animation Player component (Unity's legacy Animation): play, crossfade, blend and queue clips with wrap modes, clip events into Lua (`AnimationPlayer`), and read-only model clips (`hero.gltf#Walk`); clips bind by path, so one file animates every prefab, scene or character with the same rig
- Humanoid retargeting: a clip made on one humanoid rig plays on any other (other bone names, T- or A-pose, other proportions) as a humanoid clip, from a model's Animation Type: Humanoid, an Animation Player entry's Retarget, or Convert to Humanoid Clip; fingers, eyes and jaw, Mirror and Foot IK, and the Avatar Configuration window for the bone map and twist corrections: see [docs/animation-clips.md](docs/animation-clips.md#humanoid-clips-and-retargeting)
- Animation layers with weights, fades, Override or Additive blending and Avatar Masks (`.mask`: humanoid body parts and bone paths), so an arm holds a tool while the legs run; and Animator Controllers (`.controller`, Unity's Animator: parameters, states, Any State, transitions with exit times and interruption, 1D blend trees, `onStateEnter`/`onStateExit`) built in the Animator window (Window > Animation > Animator) or with the `edit_animator` MCP tool, with a validator that finds dead ends, unreachable states and parameters of the wrong type: see [docs/animation-clips.md](docs/animation-clips.md#layers-and-masks)
- Lattice deformers (free-form deformation cages) that bend, squash, twist, or wobble meshes, skinned models, and sprites on the GPU, with Scene-view point editing, presets, poses, keyframes, jiggle physics, and baking to a mesh; agents drive them through the `edit_lattice` MCP tool (see [docs/mcp/guide/lattice.md](docs/mcp/guide/lattice.md))
- Stochastic Tiling (hex-tiling) to hide texture repetition on Lit materials, terrain layers, and library shaders
- Global shader values like Unity's `Shader.SetGlobal*`: declared with defaults in Project Settings > Graphics > Shader Globals, set from Lua (`Shader.setGlobalColor`) or C++, read by any shader through its `u_vs_globals` / `u_fs_globals` block, and followed by library shader parameters bound to them in the Inspector, with no fork (see [docs/mcp/guide/shaders.md](docs/mcp/guide/shaders.md#global-values))
- Pose animations that play live on any humanoid model (the Pose Animator component and Lua `PoseAnimator`: play, go to a pose with a spring, blend by weight) or bake to a shared Animation Clip or to keyframes in the scene; poses are rig-independent joint angles, so one animation fits every humanoid rig
- Vertex Animation (VAT): baked vertex or bone animation textures played on the GPU by the Vertex Animator component, so a crowd is one instanced draw per material with no CPU pose work; every instance has its own clip, time, speed and crossfade, with clip events, sockets and a Lua API (`VertexAnimator`), inside OpenGL ES 3 / WebGL2 limits
- Thick lines and trails: Unity-style Line Renderer (polylines with width curves, colour gradients, round corners and caps, texture modes) and Trail Renderer, on every backend: see [docs/mcp/guide/lines-trails.md](docs/mcp/guide/lines-trails.md)
- Camera shake and camera follow: a Camera Shaker component (MilkShake's Shaker, with its formulas and the ported game's presets as built-ins, `.shake` preset files with a graph in the Inspector, trauma, 2D and 3D) that shakes the view without touching the camera's Transform, so scripts and followers never fight it, and a Camera Follow component (SmoothDamp damping, look-at, a 2D dead zone, bounds); Lua `CameraShake.shake("explosion")`: see [docs/camera.md](docs/camera.md)
- Unity-style particle systems (modules, curves and gradients, billboard/stretched/mesh particles, sorting) with an importer for Unity ParticleSystem prefabs and scenes
- Heightmap terrain with clipmap LOD and sculpting tools
- Video: a Video Player component (Unity's: render texture, material override, camera far/near plane, events, Lua API) and HTML UI `<video>`, with WebM AV1 + Opus decoded in software everywhere and MP4 (H.264, HEVC) through each platform's decoders (MediaCodec, AVFoundation, Media Foundation, the browser, the system FFmpeg on Linux), URL videos downloaded into a media cache and played while they download (see [docs/mcp/guide/video.md](docs/mcp/guide/video.md))
- A GPU fluid simulation component (a port of Pavel Dobryakov's WebGL-Fluid-Simulation: vorticity, bloom, sunrays) shown over the screen, on a sprite, image or mesh, or as a texture any material can use, with presets, seeded deterministic runs and a Lua API: see [docs/mcp/guide/fluid.md](docs/mcp/guide/fluid.md)
- UI toolkit with Rect Transforms, layout groups, text, images, buttons, input fields, scrollbars, and other widgets
- Canvas Groups (Unity's CanvasGroup): fade a UI subtree, grey it out or make it click-through, with fade helpers, animatable alpha and an opt-in flattened (CSS opacity) render mode
- UI scaling with a Canvas Scaler (scale with screen size, match width or height, expand/shrink, constant physical size), text outlines and shadows, per-character text effects (wave, typewriter, gradient, ...) and particles inside the UI, in the native UI and the HTML/CSS UI alike
- Unity-style input: key, mouse and gamepad edges (`Input.getKeyDown`), touches with phases, and input actions (Project Settings > Input, Unity's Input Manager axes and buttons with gravity, sensitivity and dead zones) that sum keyboard, gamepad and on-screen controls, with runtime rebinding: see [docs/mcp/guide/input.md](docs/mcp/guide/input.md)
- Multi-touch UI: every finger is its own pointer in the component UI and the HTML UI (two fingers press two buttons), Unity's EventSystem callbacks (`onPointerDown(e)`, `onBeginDrag`, `onDrop`... with a `PointerEventData`), DOM Pointer Events with pointer capture and `touch-action`, and on-screen controls (On-Screen Stick in Joystick Pack's Fixed / Floating / Dynamic modes, On-Screen Button, Touch Area for touch look) that feed the input actions: see [docs/mcp/guide/input.md](docs/mcp/guide/input.md)
- Box2D and Jolt Physics for 2D/3D rigidbodies, colliders, and joints, drawn interpolated between fixed steps
- Unity-style collision and trigger callbacks: define `onCollisionEnter` / `onTriggerEnter` (and `Stay`, `Exit`, the `2D` ones) on a Lua script or a C++ `ScriptBase`, called on the main thread after each physics step with rich, safe-to-keep collision data (contacts, normals, impulses, relative velocity); exact Enter/Stay/Exit rules for sleeping, rebuilt, disabled and destroyed bodies; per-collider callback receivers; `Physics.ignoreCollision`; the global contact events moved off Jolt's job threads
- 2D physics with Unity's joint set (wheel suspension and motors, hinge limits, sliders, springs, fixed, relative and target joints, with local anchors that work in prefabs and break forces with `onJointBreak2D`), concave polygon colliders and colliders of any number of points (no limits on device), and physics materials with Unity's combine modes. Colliders created from now on report contact events by default (`contactEvents`), as in Unity
- 3D physics with Unity's Rigidbody fields (damping, explicit mass, centre of mass and inertia, interpolation per body, sleeping, live type switching), any number of colliders per body with per-collider triggers and physics materials, Unity's joints (hinge limits from any start pose, springs and motors, sliders, soft ropes, character and configurable joints, local anchors that work in prefabs, break forces with `onJointBreak`), the Ragdoll wizard (GameObject > 3D Object > Ragdoll... or the `create_ragdoll` MCP tool) with a Ragdoll component that switches between animated and simulated, a runtime collider and joint debug draw (the Game view's Physics toggle, `Physics.debugDraw`), and a configurable world capacity (Project Settings > Physics)
- Spatial 3D audio and multithreaded asynchronous resource loading
- An Audio Mixer like Unity's (Window > Audio > Audio Mixer): groups with dB faders, mute, solo and live meters, effects (low/high/band-pass, echo, reverb, EQ, distortion, flanger, lo-fi, bass boost, duck, limiter), exposed parameters set from Lua (`AudioMixer.setFloat`, optionally remembered), snapshots with timed transitions; streamed music, `PlayOneShot` with voice limits and priorities, fades, `AudioListener.pause`; real levels on machines without an audio device: see [docs/mcp/guide/audio.md](docs/mcp/guide/audio.md)
- Platform services with Unity's names on every platform: `System.openURL`, the system language and region, `Engine.targetFrameRate` / `vSyncCount`, `Screen.orientation` and `autorotateTo*` (one Default / Allowed Orientations setting for Android and iOS), focus, pause and low-memory events, screen sleep, vibration and haptics, and app and device information; the Game view simulates phones (model, dpi, rotation) and languages: see [docs/mcp/guide/platform-services.md](docs/mcp/guide/platform-services.md)
- Safe areas and device simulation: `Screen.safeArea` (canvas units) with insets, cutouts and app extra insets (banners) on Android, iOS and the web, `Engine.onSafeAreaChanged`, a Safe Area UI component, live CSS `env(safe-area-inset-*)` in HTML UI, and phone presets with notch / Dynamic Island / punch-hole overlays in the Game view and MCP screenshots: see [docs/mcp/guide/safe-area.md](docs/mcp/guide/safe-area.md)
- HTTP(S) client for Lua and C++ (`Http.get` / `post` / `put` / `patch` / `delete` / `head`, JSON, form and binary bodies, downloads to `data://` files with progress and resume, textures from a URL), on each platform's own stack (system libcurl, WinHTTP, NSURLSession, HttpURLConnection, browser `fetch`), with callbacks on the main thread, errors as values, requests owned by scripts so they end with them, a plain-HTTP policy and the Android INTERNET permission set by the export, and an offline simulation in the Game view (see [docs/mcp/guide/networking.md](docs/mcp/guide/networking.md))
- Native code from Lua (`Native.call` / `callAsync` / `on`): Java/Kotlin on Android, Objective-C/Swift on iOS and macOS, JavaScript on the Web and C++ plugins everywhere, with JSON values, callbacks and events on the main thread, Lua mocks for editor Play, a `plugins/native` folder the exports pick up, project settings for Gradle dependencies, manifest entries, Info.plist keys, frameworks, CocoaPods and Swift packages, and an MCP tool that scaffolds a plugin for every platform (see [docs/mcp/guide/native-bridge.md](docs/mcp/guide/native-bridge.md))
- Lua and C++ scripts with Unity's MonoBehaviour lifecycle (`init` as Awake, `onEnable`, `start`, `onDisable`, `onDestroy`, application pause/focus/quit), `self.enabled`, `getScript`, scene-wide `GameObject.find*` / `findObjectOfType`, and UnityEvent-style `Event()` listeners: see [docs/mcp/guide/lua-api.md](docs/mcp/guide/lua-api.md)
- Persistent objects across scene loads: Unity's `DontDestroyOnLoad` (the whole GameObject with its scripts' state, playing sounds, physics velocities and running animations moves into a "DontDestroyOnLoad" scene shown in the Hierarchy during Play), `GameObject.persistSingleton` for the manager singleton pattern, and `SceneManager.moveGameObjectToScene` (see the Lua API guide, 2.8.1)
- Unity-style coroutines and timers for scripts: `self:startCoroutine(fn)` with `Coroutine.wait`, `waitRealtime`, `nextFrame`, `waitUntil`, `waitFor` (another coroutine, a timer, a tween, a sound, an HTTP request...), `waitForFixedUpdate` and `waitForEndOfFrame`, stopped with their GameObject and logged with their own traceback; `self:invoke` / `invokeRepeating` and `Timer.after` / `Timer.every` (scaled or unscaled; C++ `ScriptBase::invoke`); and a `Time` class with Unity's names (`Time.deltaTime`, `unscaledDeltaTime`, `fixedDeltaTime`, `timeSinceLevelLoad`...)
- Saves and data for scripts: Unity-style `PlayerPrefs` (with `hasKey`, `deleteAll`, `save`) on every platform, a `Json` module (`Json.encode` / `Json.decode` with exact array, number and null rules and errors that name the place), and atomic save files under `data://` (`File.writeText`, `Json.writeFile`), stored per user on desktop and in IndexedDB on the web (see the Lua API guide, [docs/mcp/guide/lua-api.md](docs/mcp/guide/lua-api.md))
- An on-device debug overlay in Play and in exported games (Unity's IngameDebugConsole and Graphy): a log console with level filters, search, collapse, stack traces, copy, save and commands (backquote key, three-finger hold or a popup with the error counts), an FPS / frame-time / stats panel (F3), and Lua log listeners (`Log.onMessageReceived`, `Log.getEntries`); on in development builds by default (Project Settings > Debug), or from a script with `DebugOverlay.enabled = true`: see [docs/mcp/guide/debugging.md](docs/mcp/guide/debugging.md)

**Saves of older desktop builds**: an exported desktop game now keeps its user data (`data://`, `UserSettings.xml`) in a per-user folder (`~/.local/share/<identifier>`, `%APPDATA%\<identifier>`, `~/Library/Application Support/<identifier>`, after Project Settings `application.identifier`) instead of the working directory. The first run copies an older build's `./UserSettings.xml` there; other files a game wrote next to itself are not moved.

Names follow Unity; see [docs/unity-glossary.md](docs/unity-glossary.md) for the old names and how to migrate older projects.

**Scripts in older projects**: a disabled script's `init` now runs (Unity's Awake: check
`self.enabled` there, or remove the script to keep it fully off); a script on a GameObject that
starts inactive gets its `init` on the first activation; `onDestroy` also runs on scene unload
and Play stop; disabled C++ scripts are compiled too.

**2D normal maps in older projects**:

- 2D normal maps are read in the OpenGL convention (green up), like 3D Lit materials: a map made for the old 2D behaviour lights upside down. Flip its green channel (`check --normals` reports it as `normals-green-convention`) or generate it again.
- A Light 2D's Height now shapes only normal-mapped objects (and Lit materials): plain sprites under a light with Height above 0 are no longer dimmed, so they get brighter. Lower the light's Intensity if the dimming was wanted. Light 2Ds made in the editor start at Height 64; lights made by scripts or loaded from scenes keep their value (0 makes normal maps flat).
- Mirrored objects (a negative scale) shade normal maps correctly and keep their front faces: remove `windingOrder = CW` workarounds on meshes mirrored at runtime (they are now reversed twice), and shaders branching on `gl_FrontFacing` see the front. Not covered: a mirror made only by an instance matrix, by skinning joints, by a lattice turning the mesh inside out, or baked into a Vertex Animation.
- `library:sprite` samples the material's Normal Texture under 2D lights (it was ignored), and a normal map that fails to load shades flat (it was tilted).

## Platform support

| Area | Support |
| --- | --- |
| Editor | Windows, Linux, macOS |
| Export targets | Windows, Linux, macOS, Android, iOS, HTML5 |
| Rendering backends | OpenGL/OpenGL ES, Vulkan, Metal, Direct3D 11 |
| Scripting | Lua, C++ |

OpenGL is the default editor backend on Windows and Linux, while macOS uses Metal; Vulkan can be selected on Windows or Linux at configure time.

## Building

### With Docker (Linux, Web, Android)

The only host requirement is Docker. `tools/build.sh` builds inside the images under `docker/` and writes to `build/<target>/`:

```bash
tools/build.sh linux      # editor + engine sample project
tools/build.sh web        # engine sample project, Emscripten
tools/build.sh android    # engine sample project APK
tools/build.sh unit-tests # the engine and editor unit tests
tools/build.sh shell linux  # a shell inside the Linux build image
```

The editor ends up in `build/linux/editor-install/bin/jasperengine-editor`. `docs/testing.md` lists
every test suite, how to run it and where a new test goes; `CMakePresets.json` and
`engine/CMakePresets.json` hold the standard configurations for IDEs and native builds.

### Natively

All platforms need a C++20 compiler (GCC 11, Clang 16, Apple Clang 15 from Xcode 15, or MSVC 19.30 from
Visual Studio 2022, or newer), CMake 3.27 or newer, and Python 3.

**Linux** (Ubuntu 26.04; on 22.04 get CMake from apt.kitware.com, see `docker/linux.Dockerfile`):

```bash
sudo apt-get install -y --no-install-recommends \
  build-essential cmake git ninja-build pkg-config python3 \
  libx11-dev libxcursor-dev libxi-dev libxrandr-dev \
  libgl1-mesa-dev libwayland-dev wayland-protocols \
  libdbus-1-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jasperengine-editor
./build/jasperengine-editor
```

**macOS** (Xcode Command Line Tools):

```bash
brew install cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jasperengine-editor
open build/JasperEngine.app
```

**Windows** (Visual Studio 2022+ with *Desktop development with C++* and *C++ CMake tools for Windows*), from a Developer PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target jasperengine-editor jasperengine-editor-cmd
build\Release\jasperengine-editor.exe
```

`jasperengine-editor-cmd` is the console build of the editor, used for command-line automation; it is excluded from the default build. C++ scripts played inside the editor must be built with an MSVC-compatible toolchain of the same architecture.

**Vulkan editor** (Windows or Linux): install the Vulkan SDK and add `-DGRAPHIC_BACKEND=vulkan` to the configure command, in a separate build directory.

`cmake --install build --prefix dist` creates a redistributable install tree on any platform.

## Command-line tools

```bash
jasperengine-editor export --help     # export a project to a standalone CMake project
jasperengine-editor shaders --help    # generate shaders
jasperengine-editor benchmark --help  # measure scene FPS
jasperengine-editor place-physics --help  # drop, separate or move GameObjects with physics, no window
                                          # (--decompose never or always: the same result every run)
jasperengine-editor bake-vat --help   # bake or import a Vertex Animation (.vat), no window
jasperengine-editor synthesize-texture --help  # synthesize textures from examples, no window
jasperengine-editor generate-sprite-normals --help  # normal maps for sprites, sheets and GIFs, no window
jasperengine-editor check --project . --normals    # stale or mismatched sprite normal maps (docs/mcp/checks.md)
```

## Repository layout

- `editor/` — desktop editor, MCP server for AI agents (`editor/mcp`, see `docs/mcp`), project tools, build orchestration, and export flow
- `engine/` — ECS runtime, platform layers, rendering, scripting, and project templates
- `engine/libs/shadercompiler/` — shader compilation and cross-platform translation, shared by the editor and the runtime (which compiles the shader variants a build lacks)
- `libs/` — bundled third-party dependencies
- `docker/`, `tools/` — build images, build script, rebrand and art scripts

## License

JasperEngine is available under the [MIT License](LICENSE). Bundled third-party libraries retain their respective licenses.
