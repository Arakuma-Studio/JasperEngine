# RmlUi fork

| | |
|---|---|
| Upstream | https://github.com/mikke89/RmlUi |
| Base | tag `6.3`, commit `ba95ffe8bfb6370efb2cdcca927eaad4710c5413` (2026-08-22) |
| Location | `engine/libs/rmlui/` |
| Build | static, hidden visibility, `RMLUI_STATIC_LIB` (public), `RMLUI_NO_FILE_INTERFACE_DEFAULT`, no FreeType |

The build files are ours: `engine/libs/rmlui/CMakeLists.txt` (desktop, Web, Android, every
export through `engine/CMakeLists.txt`), `engine/libs/rmlui/hidden-symbols.map` (the version
script that keeps every `Rml::` symbol out of the editor's shared engine, GNU ld/lld) and the
`rmlui-ios` / `rmlui-macos` targets of `engine/workspaces/xcode/JasperEngine.xcodeproj`. The
CMake and Xcode targets compile every `.cpp` of the vendored subset, so adding or removing
upstream files only needs the Xcode file list updated.

## Vendored subset

Copied byte for byte from the upstream tag:

```
engine/libs/rmlui/
  LICENSE.txt                    (MIT)
  Include/RmlUi/Core.h
  Include/RmlUi/Debugger.h
  Include/RmlUi/Config/**
  Include/RmlUi/Core/**          (includes Containers/robin_hood.h, itlib, their LICENSE.txt)
  Include/RmlUi/Debugger/**
  Source/Core/*.{cpp,h,inl}      (Pool.inl is included by Pool.h)
  Source/Core/Elements/*.{cpp,h}
  Source/Core/Layout/*.{cpp,h}
  Source/Debugger/*.{cpp,h}      (+ Source/Debugger/LICENSE.txt: the embedded Courier Prime font, OFL)
```

## Removed from upstream

| Upstream path | Why it is not vendored |
|---|---|
| `Source/Core/FontEngineDefault/` | FreeType font engine. The engine's own `RmlFontEngine` (STBText stack: stb_truetype, HarfBuzz, SheenBidi) replaces it; `Core.cpp` includes it only under `RMLUI_FONT_ENGINE_FREETYPE`, which is never defined. |
| `Source/SVG/`, `Include/RmlUi/SVG/` | SVG plugin on LunaSVG. SVG goes through the library chosen by the phase-0 benchmark (`docs/html-ui/svg-benchmark.md`). |
| `Source/Lottie/`, `Include/RmlUi/Lottie/` | Lottie plugin on rlottie (last release 2020, a new texture per frame). Lottie goes through ThorVG. |
| `Source/Lua/`, `Include/RmlUi/Lua/`, `Include/RmlUi/Lua.h` | RmlUi's Lua bindings. The engine has its own browser-shaped Lua API (`UIDocument`, `UIElement`, `UIEvent`). |
| `Backends/` | Sample platform and renderer backends (GL, Vulkan, SDL, GLFW ...). The engine implements the interfaces on its own render abstraction (`engine/core/htmlui/`). |
| `Samples/`, `Tests/`, `Utilities/` | Demos, upstream test suites and tooling, not part of the library. |
| `CMake/`, every `CMakeLists.txt`, `CMakePresets.json` | Upstream build system; ours is `engine/libs/rmlui/CMakeLists.txt`. |
| `Dependencies/` | Third-party dependency scripts of the upstream build. |
| `Source/Debugger/.clang-format`, `readme.md`, `changelog.md`, `contributing.md` | Not source. |

## Patches

Every change to files under `engine/libs/rmlui` other than our build files above is a separate
commit prefixed `rmlui:` and listed here.

| ID | Commit | Files | Reason | Test |
|---|---|---|---|---|
| (none in phase 0) | | | | |

## Updating upstream

1. Copy the new tag over the vendored subset (same include/exclude lists).
2. Re-apply each patch above in order; drop those merged upstream.
3. Update the file list of the `rmlui-ios` / `rmlui-macos` Xcode targets if upstream added
   or removed sources.
4. Rebuild, run `jasperengine-htmlui-tests` on glcore and vulkan
   (`tools/build.sh htmlui-tests`, `HTMLUI_BACKEND=vulkan tools/build.sh htmlui-tests`),
   update this file.
