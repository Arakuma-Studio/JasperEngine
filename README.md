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
- Keyframe characters and objects Unity-style in the Animation window: record Scene-view and Inspector edits, edit keys in a dopesheet or curves, add events that call Lua, and save shared Animation Clips (`.anim`) that any hierarchy with the same child names can play; extract clips from glTF/FBX models, convert old keyframe Animations and bake pose animations to clips: see [docs/animation-clips.md](docs/animation-clips.md)
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
- Lattice deformers (free-form deformation cages) that bend, squash, twist, or wobble meshes, skinned models, and sprites on the GPU, with Scene-view point editing, presets, poses, keyframes, jiggle physics, and baking to a mesh; agents drive them through the `edit_lattice` MCP tool (see [docs/mcp/guide/lattice.md](docs/mcp/guide/lattice.md))
- Stochastic Tiling (hex-tiling) to hide texture repetition on Lit materials, terrain layers, and library shaders
- Pose animations that play live on any humanoid model (the Pose Animator component and Lua `PoseAnimator`: play, go to a pose with a spring, blend by weight) or bake to a shared Animation Clip or to keyframes in the scene; poses are rig-independent joint angles, so one animation fits every humanoid rig
- Vertex Animation (VAT): baked vertex or bone animation textures played on the GPU by the Vertex Animator component, so a crowd is one instanced draw per material with no CPU pose work; every instance has its own clip, time, speed and crossfade, with clip events, sockets and a Lua API (`VertexAnimator`), inside OpenGL ES 3 / WebGL2 limits
- Thick lines and trails: Unity-style Line Renderer (polylines with width curves, colour gradients, round corners and caps, texture modes) and Trail Renderer, on every backend: see [docs/mcp/guide/lines-trails.md](docs/mcp/guide/lines-trails.md)
- Unity-style particle systems (modules, curves and gradients, billboard/stretched/mesh particles, sorting) with an importer for Unity ParticleSystem prefabs and scenes
- Heightmap terrain with clipmap LOD and sculpting tools
- A GPU fluid simulation component (a port of Pavel Dobryakov's WebGL-Fluid-Simulation: vorticity, bloom, sunrays) shown over the screen, on a sprite, image or mesh, or as a texture any material can use, with presets, seeded deterministic runs and a Lua API: see [docs/mcp/guide/fluid.md](docs/mcp/guide/fluid.md)
- UI toolkit with Rect Transforms, layout groups, text, images, buttons, input fields, scrollbars, and other widgets
- Unity-style input: key, mouse and gamepad edges (`Input.getKeyDown`), touches with phases, and input actions (Project Settings > Input, Unity's Input Manager axes and buttons with gravity, sensitivity and dead zones) that sum keyboard, gamepad and on-screen controls, with runtime rebinding: see [docs/mcp/guide/input.md](docs/mcp/guide/input.md)
- Box2D and Jolt Physics for 2D/3D rigidbodies, colliders, and joints, drawn interpolated between fixed steps
- 2D physics with Unity's joint set (wheel suspension and motors, hinge limits, sliders, springs, fixed, relative and target joints, with local anchors that work in prefabs and break forces with `onJointBreak2D`), concave polygon colliders and colliders of any number of points (no limits on device), and physics materials with Unity's combine modes. Colliders created from now on report contact events by default (`contactEvents`), as in Unity
- Spatial 3D audio and multithreaded asynchronous resource loading
- Lua and C++ scripts with Unity's MonoBehaviour lifecycle (`init` as Awake, `onEnable`, `start`, `onDisable`, `onDestroy`, application pause/focus/quit), `self.enabled`, `getScript`, scene-wide `GameObject.find*` / `findObjectOfType`, and UnityEvent-style `Event()` listeners: see [docs/mcp/guide/lua-api.md](docs/mcp/guide/lua-api.md)
- Saves and data for scripts: Unity-style `PlayerPrefs` (with `hasKey`, `deleteAll`, `save`) on every platform, a `Json` module (`Json.encode` / `Json.decode` with exact array, number and null rules and errors that name the place), and atomic save files under `data://` (`File.writeText`, `Json.writeFile`), stored per user on desktop and in IndexedDB on the web (see the Lua API guide, [docs/mcp/guide/lua-api.md](docs/mcp/guide/lua-api.md))

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
tools/build.sh shell linux  # a shell inside the Linux build image
```

The editor ends up in `build/linux/editor-install/bin/jasperengine-editor`.

### Natively

All platforms need a C++17 compiler (MSVC, GCC, or Clang), CMake 3.27 or newer, and Python 3.

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

## Continuous integration

`.github/workflows/ci.yml` builds Linux, Web, and Android on every push. Windows (MSVC) and macOS/iOS run on demand — the **Run workflow** button, pull requests into `main`, `v*` tags, and releases — because those runners are billed at a multiple of Linux minutes.

## Repository layout

- `editor/` — desktop editor, MCP server for AI agents (`editor/mcp`, see `docs/mcp`), project tools, build orchestration, and export flow
- `engine/` — ECS runtime, platform layers, rendering, scripting, and project templates
- `engine/libs/shadercompiler/` — shader compilation and cross-platform translation, shared by the editor and the runtime (which compiles the shader variants a build lacks)
- `libs/` — bundled third-party dependencies
- `docker/`, `tools/` — build images, build script, rebrand and art scripts

## License

JasperEngine is available under the [MIT License](LICENSE). Bundled third-party libraries retain their respective licenses.
