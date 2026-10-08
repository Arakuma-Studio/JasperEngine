# RmlUi fork

| | |
|---|---|
| Upstream | https://github.com/mikke89/RmlUi |
| Base | tag `6.3`, commit `ba95ffe8bfb6370efb2cdcca927eaad4710c5413` (2026-08-22) |
| Location | `engine/libs/rmlui/` |
| Build | static, hidden visibility, `RMLUI_STATIC_LIB` (public), `RMLUI_NO_FILE_INTERFACE_DEFAULT`, `RMLUI_SVG_PLUGIN` (links `lunasvg` privately), no FreeType; `Source/Debugger` only with `JASPERENGINE_HTMLUI_DEBUGGER` (editor, Debug builds, tests) |

The build files are ours: `engine/libs/rmlui/CMakeLists.txt` (desktop, Web, Android, every
export through `engine/CMakeLists.txt`), `engine/libs/rmlui/hidden-symbols.map` (the version
script that keeps every `Rml::`, `tvg::` and `lunasvg::` symbol out of the editor's shared engine, GNU ld/lld) and the
`rmlui-ios` / `rmlui-macos` targets of `engine/workspaces/xcode/JasperEngine.xcodeproj`. The
CMake and Xcode targets compile every `.cpp` of the vendored subset (CMake: `Source/Debugger`
only with `JASPERENGINE_HTMLUI_DEBUGGER`; Xcode always, and the Debug configurations define it),
so adding or removing upstream files only needs the Xcode file list updated.

## Vendored subset

Copied byte for byte from the upstream tag:

```
engine/libs/rmlui/
  LICENSE.txt                    (MIT)
  Include/RmlUi/Core.h
  Include/RmlUi/Debugger.h
  Include/RmlUi/Config/**
  Include/RmlUi/Core/**          (includes Containers/robin_hood.h, itlib, and the shared Containers/LICENSE.txt)
  Include/RmlUi/Debugger/**
  Source/Core/*.{cpp,h,inl}      (Pool.inl is included by Pool.h)
  Source/Core/Elements/*.{cpp,h}
  Source/Core/Layout/*.{cpp,h}
  Source/Debugger/*.{cpp,h}      (+ Source/Debugger/LICENSE.txt: the embedded Courier Prime font, OFL)
  Include/RmlUi/SVG/ElementSVG.h  (phase 1, p1-vector)
  Source/SVG/*.{cpp,h}           (phase 1, p1-vector: the SVG plugin, unchanged, on LunaSVG)
```

The SVG plugin is compiled into the `rmlui` target with `RMLUI_SVG_PLUGIN` defined, so
`Rml::Initialise` registers it as upstream does (`Source/Core/Core.cpp`); it links
`engine/libs/lunasvg` (LunaSVG 3.5.0 + plutovg, the version the phase-0 benchmark measured). The
engine registers its built-in fonts with LunaSVG (`engine/core/util/LunaSVGRuntime`), which is
built without system-font discovery and without file access (`<image href>` takes `data:` URIs).

## Removed from upstream

| Upstream path | Why it is not vendored |
|---|---|
| `Source/Core/FontEngineDefault/` | FreeType font engine. The engine's own `RmlFontEngine` (STBText stack: stb_truetype, HarfBuzz, SheenBidi) replaces it; `Core.cpp` includes it only under `RMLUI_FONT_ENGINE_FREETYPE`, which is never defined. |
| `Source/Lottie/`, `Include/RmlUi/Lottie/` | Lottie plugin on rlottie (last release 2020, a new texture per frame). The engine's own `<lottie>` element (`engine/core/htmlui/vector/`) runs on ThorVG. |
| `Source/Lua/`, `Include/RmlUi/Lua/`, `Include/RmlUi/Lua.h` | RmlUi's Lua bindings. The engine has its own browser-shaped Lua API (`UIDocument`, `UIElement`, `UIEvent`). |
| `Backends/` | Sample platform and renderer backends (GL, Vulkan, SDL, GLFW ...). The engine implements the interfaces on its own render abstraction (`engine/core/htmlui/`). |
| `Samples/`, `Tests/`, `Utilities/` | Demos, upstream test suites and tooling, not part of the library. |
| `CMake/`, every `CMakeLists.txt`, `CMakePresets.json` | Upstream build system; ours is `engine/libs/rmlui/CMakeLists.txt`. |
| `Dependencies/` | Third-party dependency scripts of the upstream build. |
| `Source/Debugger/.clang-format`, `readme.md`, `changelog.md`, `contributing.md`, `.appveyor.yml`, `.clang-format`, `.editorconfig`, `.gitignore`, `.github/` | Not source. |

## Patches

Every change to files under `engine/libs/rmlui` other than our build files above is a separate
commit prefixed `rmlui:` and listed here.

| ID | Commit | Files | Reason | Test |
|---|---|---|---|---|
| — | `ae671dec` | `Include/**`, `Source/Core/**`, `Source/Debugger/**`, `LICENSE.txt` | `rmlui: vendor RmlUi 6.3 (Core, Debugger)`: the phase-0 vendoring commit, byte for byte upstream. | phase-0 suites |
| P1-1 | `bf32a608` `rmlui: Event::PreventDefault` | `Include/RmlUi/Core/Event.h`, `Source/Core/Event.cpp`, `Source/Core/EventDispatcher.{h,cpp}`, `Include/RmlUi/Core/Element.h`, `Source/Core/Element.cpp`, `Include/RmlUi/Core/Context.h`, `Source/Core/Context.cpp`, `Source/Core/Elements/WidgetTextInput.cpp` | DOM `preventDefault()` for the Lua/C++ element API (phase 1 §3.14 patch 1): `Event::PreventDefault`/`IsDefaultPrevented`; the dispatcher skips the elements' default actions of a prevented event and reports it as consumed (`DispatchEvent` returns false; new `Element::DispatchEvent(EventId, params, bool* default_prevented)`); so `Context::ProcessKeyDown/Up/TextInput` and `ProcessMouseWheel` return "consumed" (a prevented wheel does not scroll; a prevented mousedown starts no drag and no double click); the text input produced by a prevented key down is dropped until the next key down or key up; `WidgetTextInput` ignores key downs/text inputs an ancestor's capture listener prevented | `engine/tests/htmlui/TestScriptEvents.cpp` (`PreventDefaultKeepsCheckboxUnchecked`, `PreventDefaultOnKeyDown`, `PreventDefaultOnWheelStopsScrolling`) |
| — | `e580f8d7` | `Source/SVG/**`, `Include/RmlUi/SVG/**` | `rmlui: vendor SVG plugin (6.3)`: a vendoring commit, the files are byte for byte upstream. The planned rasteriser patch (`rmlui: SVG plugin rasterises with ThorVG`, phase 1 §3.10.2) is **not made**: the phase-0 benchmark chose LunaSVG for SVG (`docs/html-ui/svg-benchmark.md`), which is the plugin's upstream rasteriser. | `TestVectorSvg.cpp` (`SvgPluginTest`), goldens `p1/svg_file`, `p1/svg_inline` |

## Updating upstream

1. Copy the new tag over the vendored subset (same include/exclude lists).
2. Re-apply each patch above in order; drop those merged upstream.
3. Update the file list of the `rmlui-ios` / `rmlui-macos` Xcode targets if upstream added
   or removed sources.
4. Rebuild, run `jasperengine-htmlui-tests` on glcore and vulkan
   (`tools/build.sh htmlui-tests`, `HTMLUI_BACKEND=vulkan tools/build.sh htmlui-tests`),
   update this file.
