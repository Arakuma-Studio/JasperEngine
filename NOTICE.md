# Notice

JasperEngine is a fork of [Doriax](https://github.com/doriaxengine/doriax) by Eduardo Doria and contributors, released under the MIT License.

- Fork point: Doriax `main` at commit `29348d7b4d9b03ee1f29dc184f9863f36dc12c2d` (2026-09-29), tagged `doriax-base` in this repository. `git diff doriax-base` shows every change made since.
- The full Doriax history is preserved in this repository.
- The Doriax name was replaced with JasperEngine by `tools/rebrand.py`. Per-file copyright headers (`(c) Eduardo Doria and contributors`) are kept as they were.
- Doriax's copyright notice is retained in [LICENSE](LICENSE), as the MIT License requires.

Bundled third-party libraries under `libs/`, `engine/libs/`, and `engine/libs/shadercompiler/libs/` keep their own licenses. Every one of them has a row below and a licence file in its directory; `tools/ci/check-notices.py` checks both in CI. The editor install carries this file, `LICENSE` and a `licenses/` folder with each shipped library's licence file (`<library>-LICENSE`); exported games get theirs from the export pipeline (S30).

Libraries of the engine, the editor and the tools:

| Library | Version | Location | License | Used for |
|---|---|---|---|---|
| [sokol](https://github.com/floooh/sokol) | upstream `master`, with local changes (git history) | `engine/libs/sokol/` (`LICENSE`) | zlib | graphics (sokol_gfx: GL, GLES3/WebGL2, Metal, D3D11, Vulkan), time, logging |
| [Lua](https://www.lua.org) | 5.4.7 | `engine/libs/lua/` (`LICENSE`, from `lua.h`) | MIT | the scripting runtime |
| [LuaBridge3](https://github.com/kunitoki/LuaBridge3) | 3.0.1 | `engine/libs/luabridge3/` (`LICENSE.txt`) | MIT | the C++ bindings of the Lua API |
| [JSON for Modern C++](https://github.com/nlohmann/json) | 3.11.3 | `engine/libs/json/` (`LICENSE`, from `json.hpp`) | MIT | JSON reading and writing |
| [stb](https://github.com/nothings/stb) | stb_image 2.30, stb_truetype 1.26, stb_image_write, stb_image_resize2, stb_rect_pack, stb_vorbis | `engine/libs/stb/` (`LICENSE`, from the headers) | MIT or public domain | image decoding and encoding, font rasterisation, Ogg Vorbis |
| [SoLoud](https://github.com/jarikomppa/soloud) | 202002 (`SOLOUD_VERSION`) | `engine/libs/soloud/` (`LICENSE`) | zlib/libpng (its bundled parts as listed in `LICENSE`) | audio |
| [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) | 2.0 | `engine/libs/tinyobjloader/` (`LICENSE`, from `tiny_obj_loader.h`) | MIT | OBJ model loading |
| [tinygltf](https://github.com/syoyo/tinygltf) | 2.x | `engine/libs/tinygltf/` (`LICENSE`, from `tiny_gltf.h`: MIT, plus the bundled base64 (zlib-style) and dlib URI decoding (Boost) code) | MIT | glTF model loading |
| [TinyXML-2](https://github.com/leethomason/tinyxml2) | 10.0.0 | `engine/libs/tinyxml2/` (`LICENSE.txt`) | zlib | XML (tilemaps, sprite sheets) |
| [Box2D](https://github.com/erincatto/box2d) | 3.1.0 | `engine/libs/box2d/` (`LICENSE`) | MIT | 2D physics |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | 5.2.0 | `engine/libs/joltphysics/` (`LICENSE`) | MIT | 3D physics |
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) | 14.3.0 | `engine/libs/harfbuzz/` (`COPYING`) | Old MIT | text shaping |
| [SheenBidi](https://github.com/Tehreer/SheenBidi) | 3.0 | `engine/libs/sheenbidi/` (`LICENSE`) | Apache-2.0 | the Unicode bidirectional algorithm |
| [Tracy](https://github.com/wolfpld/tracy) | 0.11.1 (the client, `public/`) | `engine/libs/tracy/` (`LICENSE`) | BSD-3-Clause | optional remote profiling of development games (`JASPERENGINE_TRACY`, off by default; never in the editor or release games) |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | 1.2 | `engine/libs/meshoptimizer/` (`LICENSE.md`) | MIT | mesh LODs and optimisation |
| [RmlUi](https://github.com/mikke89/RmlUi) | 6.3 (`ba95ffe8bfb6`), JasperEngine's fork | `engine/libs/rmlui/` (`LICENSE.txt`; Taffy's below, `LICENSE-taffy.txt`) | MIT | the HTML/CSS UI |
| [glslang](https://github.com/KhronosGroup/glslang) | 15.1.0 | `engine/libs/shadercompiler/libs/glslang/` (`LICENSE.txt`) | BSD-3-Clause and the others `LICENSE.txt` lists | GLSL to SPIR-V in the shader compiler |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | vendored (git history) | `engine/libs/shadercompiler/libs/spirv-cross/` (`LICENSE`, `LICENSES/`) | Apache-2.0 | SPIR-V to GLSL, HLSL and MSL in the shader compiler |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.8 | `libs/imgui/` (`LICENSE.txt`) | MIT | the editor's user interface |
| [Native File Dialog Extended](https://github.com/btzy/nativefiledialog-extended) | 1.3.0 | `libs/nfd/` (`LICENSE`) | zlib | the editor's file dialogs |
| [tinyexpr](https://github.com/codeplea/tinyexpr) (Lewis Van Winkle) | upstream `7ab30db449a3` | `libs/tinyexpr/` (`LICENSE`) | zlib | the expressions of the editor's numeric fields |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.8.0 | `libs/yaml-cpp/` (`LICENSE`) | MIT | the editor's scene, project and settings files |
| [googletest](https://github.com/google/googletest) | 1.17.0 (googletest and googlemock) | `libs/googletest/` (`LICENSE`, `README.jasper.md`) | BSD-3-Clause | the unit tests only: tests only, never shipped |

`engine/libs/shadercompiler/` itself (`src/`, the CMake build) is JasperEngine's own code (MIT, like the engine). Portions of the build and test tooling are adapted from [OpenEngine](https://github.com/Game-Crafters-Guild/OpenEngine) (MIT, Copyright (c) 2026 Gamecrafters Guild Limited and contributors), with that notice kept in each file: `engine/cmake/JasperSanitizers.cmake` (sanitizer flags, MSVC AddressSanitizer runtime staging), `tools/web/serve.py` and `tools/web/browser_gate.py` (the headless-Chrome web gate).

Vector graphics of the HTML/CSS UI (all MIT):

| Library | Version | Location | Used for |
|---|---|---|---|
| [ThorVG](https://github.com/thorvg/thorvg) | 1.2.0 (`caa5af1167d7`), local change: no global `operator new`/`delete` under ASan | `engine/libs/thorvg/` (`LICENSE`) | Lottie (`<lottie>`), CPU renderer |
| [LunaSVG](https://github.com/sammycage/lunasvg) | 3.5.0 (`cf3594d5232e`) | `engine/libs/lunasvg/` (`LICENSE`) | SVG (RmlUi's SVG plugin, `.svg` textures) |
| [plutovg](https://github.com/sammycage/plutovg) | 1.3.3 (bundled with LunaSVG) | `engine/libs/lunasvg/plutovg/` (`LICENSE`; its rasteriser derives from FreeType's, `source/FTL.TXT`) | LunaSVG's rasteriser |

HTML parsing of the HTML/CSS UI's compatibility layer:

| Library | Version | Location | Used for |
|---|---|---|---|
| [lexbor](https://github.com/lexbor/lexbor) | 3.1.0 (`master` at `f4cbbcd91359`, no release tag yet) | `engine/libs/lexbor/` (Apache-2.0: `LICENSE`, `NOTICE`; one local patch, `PATCHES.md`) | HTML5 parsing of `.html` documents (`engine/core/htmlui/compat/html/HtmlParser`) |

Video of the HTML/CSS UI (`<video>`: AV1 + Opus in WebM, decoded in software; FFmpeg is never linked or shipped, the editor only runs a user-installed one to convert imports):

| Library | Version | Location | License | Used for |
|---|---|---|---|---|
| [dav1d](https://code.videolan.org/videolan/dav1d) | 1.5.4 | `engine/libs/dav1d/` (`COPYING`; what is vendored: `README.jasper.md`) | BSD-2-Clause | AV1 decoding (`engine/core/htmlui/media/Av1Decoder`), C and the x86-64/aarch64/armv7 asm |
| x86inc.asm ([x264](https://code.videolan.org/videolan/x264), bundled with dav1d) | dav1d 1.5.4's copy | `engine/libs/dav1d/src/ext/x86/x86inc.asm` (licence in its header) | ISC | the macro layer of dav1d's NASM code |
| [libopus](https://opus-codec.org) | 1.6.1 (decoder sources) | `engine/libs/opus/` (`COPYING`, `README.jasper.md`) | BSD-3-Clause (with the codec's patent licences referenced in `COPYING`) | Opus audio of videos (`OpusAudioDecoder`) |
| [libwebm](https://chromium.googlesource.com/webm/libwebm) | 1.0.0.32 (`mkvparser`) | `engine/libs/libwebm/` (`LICENSE.TXT`, `PATENTS.TXT`, `README.jasper.md`) | BSD-3-Clause, plus Google's WebM patent grant | WebM/Matroska demuxing (`VideoDemuxer`) |

The video controls' icons (`engine/core/htmlui/compat/ua/video-{play,pause,volume,muted}.svg`) are [Lucide](https://github.com/lucide-icons/lucide) icons (ISC, see below). The test clips (`engine/tests/htmlui/fixtures/video/`) and the example's `menu-loop.webm` are generated by `tools/htmlui/gen_video_fixtures.py` from FFmpeg's synthetic sources.

Layout of the HTML/CSS UI:

| Code | Version | Location | License | Used for |
|---|---|---|---|---|
| [Taffy](https://github.com/DioxusLabs/taffy) (`src/compute/grid` and `src/compute/flexbox.rs`, ported to C++) | `master` at `8409f3f8c6de` | `engine/libs/rmlui/Source/Core/Layout/Grid*`, `engine/libs/rmlui/Source/Core/Layout/FlexFormattingContext.*` (`engine/libs/rmlui/LICENSE-taffy.txt`) | MIT | CSS Grid and flexbox layout in the RmlUi fork |

Test data: the html5lib tree-construction tests (MIT, `engine/tests/htmlui/fixtures/compat/html5lib/LICENSE`). Taffy's layout test fixtures (MIT, `engine/tests/htmlui/fixtures/taffy/LICENSE-taffy.txt`).

Icons of the HTML/CSS UI:

| Data | Version | Location | License | Used for |
|---|---|---|---|---|
| [Lucide](https://github.com/lucide-icons/lucide) icons | 1.53.0 (`b56741cf30c0`) | `engine/libs/lucide/` (`lucide.pack`, `LICENSE`, `VERSION`) | ISC; the icons derived from [Feather](https://github.com/feathericons/feather) (listed in `LICENSE`) MIT | built-in `data-lucide` icons of HTML documents |
| [Lucide](https://github.com/lucide-icons/lucide) icons | 1.53.0 | `docs/mcp/guide/html-ui-examples/{inventory,shop}/assets/items/*.svg` (`docs/mcp/guide/html-ui-examples/LICENSE-lucide.txt`) | ISC (as above) | the item pictures of the HTML UI example library (recoloured copies) |

UI effects of the HTML/CSS UI: the effects of `engine/shaderlib/effects/` marked `@source shaders.com <Name>` and the include library `engine/shaderlib/effects/lib/` are GLSL ports of [shaders.com](https://shaders.com/) effects and kit functions (MIT, Copyright (c) 2026 Shader Effects Inc.). The notice and the list of ported effects are in `engine/shaderlib/effects/LICENSE.shaders-com.txt` (generated by `tools/htmlui/gen_effects_docs.py`); exports that ship such an effect copy it to `licenses/shaders.com-LICENSE.txt`.

Texture compression (cooked KTX2 textures, spec S14, `docs/textures.md`). Games link only Zstandard's decoder, bcdec and, on the web, the Basis Universal transcoder; the encoders are in the editor alone:

| Library | Version | Location | License | Used for |
|---|---|---|---|---|
| [Zstandard](https://github.com/facebook/zstd) | 1.5.7 | `engine/libs/zstd/` (`LICENSE`, `README.jasper.md`) | BSD-3-Clause | the supercompression of cooked KTX2 textures: the decoder in games, the encoder in the editor's cook |
| [bcdec](https://github.com/iOrange/bcdec) | `main` at `80859ed3b7af` | `engine/libs/bcdec/` (`LICENSE`, `README.jasper.md`) | MIT or Unlicense | BC1-BC7 CPU decoding: the fallback when a GPU cannot sample a cooked BC format, editor previews |
| [Basis Universal](https://github.com/BinomialLLC/basis_universal) | 2.50 (`v2_50`) | `engine/libs/basisu/` (`LICENSE`, `NOTICE`, `LICENSES/`, `README.jasper.md`; local change: `zstd/zstd.h` forwards to the engine's Zstandard) | Apache-2.0; its encoder's bundled tinyexr BSD-3-Clause, qoi and tinydds MIT, miniz MIT, jpgd public domain (`LICENSES/`) | ETC1S / UASTC / UASTC HDR encoding of web textures, UASTC to ETC2/EAC, `.exr` reading (editor); the transcoder of web textures (web games, editor) |
| [astc-encoder](https://github.com/ARM-software/astc-encoder) | 5.7.0 | `engine/libs/astcenc/` (`LICENSE.txt`, `README.jasper.md`) | Apache-2.0 | ASTC 4x4-12x12 encoding and decoding of mobile textures (editor only) |
| [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) | `master` at `b9438627eef7` | `engine/libs/bc7enc_rdo/` (`LICENSE`, `README.jasper.md`) | MIT or Unlicense | BC1/BC3/BC4/BC5/BC7 encoding with rate-distortion optimisation of desktop textures (editor only) |

Code from [OpenEngine](https://github.com/Game-Crafters-Guild/OpenEngine) (MIT, Copyright (c) 2026 Gamecrafters Guild Limited and contributors), used by the OpenEngine port (`docs/specs/`). Each adapted file names its source in its header.

| Code | Location | Used for |
|---|---|---|
| `Engine/Modules/Types/Include/Types/NearestName.h` (copied, namespace changed) | `editor/util/NearestName.h` | "did you mean" suggestions of the scene load report (S03) |
| `Engine/Modules/NativeScripting/Include/NativeScripting/NativeScriptingABI.h` (the `GE_ToolchainFingerprint` fill, adapted and extended) | `engine/core/script/ToolchainFingerprint.h` | the toolchain fingerprint of C++ script libraries (S23) |
| `Apps/Editor/Source/ScriptEditor/SourceLocation.cpp` (the path-start scan and the `:L:C` / `(L,C)` readers, adapted and extended) | `editor/util/SourceLocation.*` | the Console's source-location parser (S23) |
| `Engine/Modules/FileWatcher` (FileWatcher, FileIdentity), `Engine/Include/Assets/ExpectedWriteLedger.h` (+ its tests), `Engine/Modules/AssetCore/.../SharedFileRead.*` (adapted, headers kept) | `engine/core/io/watch/`, `engine/tests/core/io/watch/ExpectedWriteLedgerTest.cpp` | the file watcher's rename pairing and delete hold, file identities, the self-write ledger, share-delete reads on Windows (S05) |
| `Engine/Modules/AssetCore/Source/GUID.cpp`: `GUID::Derive`, `FnvHash`, `SplitMix64` (adapted) | `engine/core/asset/Guid.cpp` (`Guid::derive`) | deterministic sub-asset GUIDs (asset identity, S04) |
| `Modules/Memory` (`Arena`, `ScratchScope`, `ObjectPool`, `RangeAllocator`, `SerialUseCheck`, `AddressSanitizer`), adapted | `engine/core/memory/` | the allocators, adapted to our naming and stats registry (S06) |
| `Modules/JobSystem` (`ParallelForCore`, `ParallelAlgorithms`, `JobCounter`, `JobChannel`, the `WorkStealingThreadPool` lane contracts), adapted | `engine/core/jobs/` | parallelFor, counters, blocking channels; the scheduler itself is our own (S06) |
| `AssetDecodeGate`, `AssetIOService`, adapted | `engine/core/jobs/BackgroundGate.*`, `engine/core/io/AssetIOService.*` | frame protection and the asset readers (S06) |
| Tests (`ParallelAlgorithmsTests`, `AssetDecodeGateTests`, `AssetIOServiceTests`, Memory tests, `MemoryBench`), ported | `engine/tests/core/` | test cases and benchmark loops (tests only, never shipped) |
| `Engine/Modules/Rendering/Source/Common/CubeLutFileParser.cpp`, `CubeLutColorDomain.cpp` and the curves and gamut matrices of `Shaders/Includes/cube_lut.glsl` (ported to C++) | `engine/core/util/CubeLut.*` | `.cube` LUT parsing, the input encoding proposal and the scene-stage colour maths (S13) |
| `Engine/Source/Engine/Rendering/DynamicResolutionController.cpp` and its header (adapted) | `engine/core/render/DynamicResolutionController.*` | the GPU law of dynamic resolution (S13) |
| `Engine/Modules/Rendering/Shaders/vhs.frag`, `vhs_interference.frag`, `vhs_signal_glitches.frag` (adapted, with the punikonta notice below) | `engine/shaderlib/includes/post_vhs.glsl` | the uber pass's VHS tape (S13) |
| `CubeLutFileParserTests.cpp` and `Tests/Fixtures/CubeLut/*.cube`, `DynamicResolutionControllerTests.cpp` (ported) | `engine/tests/core/util/CubeLutTest.cpp`, `engine/tests/core/fixtures/cubelut/`, `engine/tests/core/render/DynamicResolutionControllerTest.cpp` | test cases and fixtures (tests only, never shipped) |
| `Source/Engine/Rendering/ShadowMapRenderFeature.cpp` (`SliceBoundingSphereRadius`, `CascadeLightUp`, `GetCascadeFitTuning`), adapted | `engine/core/render/shadow/CascadeFit.*` | the cascades' bounding-sphere fit, the near-vertical light basis and the fit freeze bands (S10) |
| `PointShadowAtlasPlanner` (importance ranking, sticky slots, challenge margin and cooldown), adapted | `engine/core/render/shadow/ShadowSlotPlanner.cpp` | shadow slots by importance (S10) |
| Shader includes `shadow_receiver_plane.glsl`, `shadow_filter.glsl` (`GE_VogelDisk`), `shadow_sampling.glsl` (`GE_PoissonRotation`), `shadow_cascade_blend.glsl`, `shadow_distance_fade.glsl`, `shadow_pcss_decisions.glsl`, adapted with every depth comparison inverted | `engine/shaderlib/includes/shadow_sampling.glsl`, `engine/shaderlib/includes/shadow_pcss_decisions.glsl` | the receiver-plane bias, the Vogel filter, the cascade blend and fade, PCSS (S10) |
| `ShadowPcssDecisionTests`, ported | `engine/tests/core/render/PcssDecisionTest.cpp` | the PCSS decision tests (tests only, never shipped) |
| `Engine/Modules/Rendering/Shaders/Includes/height_fog_term.glsl` (closed-form height fog optical depth, after [SanielX/Height-Fog](https://github.com/SanielX/Height-Fog), MIT, commit `ab6f8278`), rewritten | `engine/shaderlib/includes/height_fog.glsl`, `engine/core/render/AtmosphereMath.cpp` | the height fog (S12) |
| `Engine/Modules/HeightFog/Source/HeightFogEffectPresets.cpp` (looks re-derived) | `engine/core/render/AtmospherePresets.cpp` | the Fog presets (S12) |
| `Engine/Modules/VolumetricClouds/Shaders/volumetric_clouds_march.frag` and `volumetric_clouds_worley.glsl` / `_noise.comp` (after [SebLague/Clouds](https://github.com/SebLague/Clouds), MIT, Copyright (c) 2019 Sebastian Lague), ported and extended | `engine/shaderlib/volumetric_clouds.frag`, `engine/core/render/AtmosphereNoise.cpp` | the volumetric clouds' march and CPU noise bake (S12) |
| `Engine/Modules/UI/Include/UI/Controls/VirtualWindowCore.h` (and the update reasons of `VirtualizationCoordinator.h`), copied, namespace and naming changed; its unit tests (ported) | `engine/core/htmlui/widgets/VirtualWindowCore.*`, `engine/tests/htmlui/widgets/VirtualWindowCoreTest.cpp` | the visible window, overscan and anchoring of the HTML UI's list, grid, tree and table views (S17) |
| `ListView`'s row height index (a Fenwick tree), rewritten | `engine/core/htmlui/widgets/HeightIndex.h` | variable row heights of the views (S17) |
| `Engine/Modules/UI/Source/Parsers/` (`MarkdownParser`, `MarkdownInlineParser`, `MarkdownDocument.h`, `MarkdownSyntax.h`), copied and adapted (nested lists, link and image URL hooks); `Tests/MarkdownParserTests.cpp` (ported) | `engine/core/htmlui/widgets/Markdown/`, `engine/tests/htmlui/widgets/MarkdownParserTest.cpp` | the `<markdown>` element and `data-markdown` (S17) |
| `Engine/Modules/UI`: `Interaction/TooltipOverlay` (delay, hop window, hide triggers), `Layout/DropdownPopupPlacement` and `Tests/TooltipPlacementTests.cpp`, `Interaction/DragDropManager` (typed payloads, accepted operations), `UIHotReload` matching and `Tests/UIHotReloadTests.cpp`: rules re-implemented, test cases ported | `engine/core/htmlui/widgets/{TooltipManager,PopupPlacement,DragDropSession}.*`, `engine/core/htmlui/compat/TreeReconcile.*`, `engine/tests/htmlui/widgets/{PopupPlacementTest,TreeReconcileTest}.cpp` | tooltips, popup placement, HTML5 drag and drop and the in-place hot reload of HTML documents (S17) |
| `Engine/Source/Assets/Textures/BC7TransparentEndpointRepair.cpp`, `Engine/Include/Assets/Textures/BC7TransparentEndpointRepair.h` (copied, namespace and integer types changed); `Engine/Tests/BC7TransparentEndpointRepairTests.cpp` (ported) | `editor/texture/BC7TransparentEndpointRepair.*`, `editor/tests/texture/BC7TransparentEndpointRepairTest.cpp` | transparent texels of BC7 cooks stay at alpha 0 (S14) |

The VHS tape of the uber pass (`engine/shaderlib/includes/post_vhs.glsl`, S13) is adapted from
OpenEngine's port of punikonta's Godot VHS shader ([godot-shader-crt-vhs](https://github.com/punikonta/godot-shader-crt-vhs),
revision `6ba87f95c152ec023fc07c4f77e87bdd70783afa`, `shaders/vhs.gdshader`):

```
MIT License

Copyright (c) Marcel Jovic <punikonta@protonmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The Lens and Filmic film grain and the film damage of the uber pass (`engine/shaderlib/includes/post_film.glsl`,
S13) are adapted from OpenEngine's film-simulation package (`Packages/film-simulation/Assets/Shaders/
film_simulation_grain.frag` and `film_simulation_artifacts.frag`); nothing else of that package is used (it holds
no film-stock colour data). Its licence, reproduced as it is, includes AMD's notice for the FidelityFX Lens 1.1
(SDK v1.1.3) derived grain:

```
MIT License

Copyright (c) 2026 GameEngine contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

FidelityFX Lens-derived film grain
----------------------------------

Copyright (C) 2024 Advanced Micro Devices, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
