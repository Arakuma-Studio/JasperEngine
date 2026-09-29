# JasperEngine

**A lightweight native C++ engine for 2D and 3D games, with a visual editor and Lua scripting.**

JasperEngine is based on [Doriax](https://github.com/doriaxengine/doriax) by Eduardo Doria and contributors. See [NOTICE.md](NOTICE.md).

## Features

**Editor**

- Build 2D and 3D scenes with sprites, tilemaps, models, cameras, lights, and reusable entities
- Edit component properties, project resources, animations, bones, and terrain without leaving the editor
- Write Lua and C++ in the integrated code editor with engine API completion
- Fork the built-in shaders or write custom post-process passes, with live viewport recompilation
- Run the game in play mode, then export it as a standalone CMake project, a desktop binary, or a web build

**Engine**

- Shared ECS runtime for 2D and 3D scenes, with scene layers and serialization
- Sprites, tilemaps, and polygons, plus 2D lights, normal maps, and occluder shadows
- GLTF and OBJ models with skeletal animation, morph targets, and mesh instancing
- PBR materials with dynamic lights and cascaded shadows, sky-driven IBL, SSAO, fog, and planar mirrors
- Keyframe tracks, runtime actions with easing, crossfade blending, and sprite-sheet animation
- Particle systems and heightmap terrain with clipmap LOD and sculpting tools
- UI toolkit with anchors, containers, text, images, buttons, scrollbars, and other widgets
- Box2D and Jolt Physics for 2D/3D bodies, shapes, and joints
- Spatial 3D audio and multithreaded asynchronous resource loading

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
  libdbus-1-dev libcurl4-openssl-dev

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
```

## Continuous integration

`.github/workflows/ci.yml` builds Linux, Web, and Android on every push. Windows (MSVC) and macOS/iOS run on demand — the **Run workflow** button, pull requests into `main`, `v*` tags, and releases — because those runners are billed at a multiple of Linux minutes.

## Repository layout

- `editor/` — desktop editor, AI assistant, project tools, build orchestration, and export flow
- `engine/` — ECS runtime, platform layers, rendering, scripting, and project templates
- `shadercompiler/` — shader compilation and cross-platform translation
- `libs/` — bundled third-party dependencies
- `docker/`, `tools/` — build images, build script, rebrand and art scripts

## License

JasperEngine is available under the [MIT License](LICENSE). Bundled third-party libraries retain their respective licenses.
