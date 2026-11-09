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
| [LuaBridge3](https://github.com/kunitoki/LuaBridge3) | 3.0.1, with the size changes of docs/specs/S34-player-size.md D6 (marked "JasperEngine (S34") | `engine/libs/luabridge3/` (`LICENSE.txt`) | MIT | the C++ bindings of the Lua API |
| [JSON for Modern C++](https://github.com/nlohmann/json) | 3.11.3 | `engine/libs/json/` (`LICENSE`, from `json.hpp`) | MIT | JSON reading and writing |
| [stb](https://github.com/nothings/stb) | stb_image 2.30, stb_truetype 1.26, stb_image_write, stb_image_resize2, stb_rect_pack | `engine/libs/stb/` (`LICENSE`, from the headers) | MIT or public domain | image decoding and encoding, font rasterisation |
| [SoLoud](https://github.com/jarikomppa/soloud) | 202002 (`SOLOUD_VERSION`) | `engine/libs/soloud/` (`LICENSE`) | zlib/libpng (its bundled parts as listed in `LICENSE`) | audio (Ogg Vorbis through its bundled stb_vorbis 1.22, public domain or MIT as in the file) |
| [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) | 2.0 | `engine/libs/tinyobjloader/` (`LICENSE`, from `tiny_obj_loader.h`) | MIT | OBJ model loading |
| [tinygltf](https://github.com/syoyo/tinygltf) | 2.x, with the buffer changes of docs/specs/S53-model-import-cook.md R1 (marked "JasperEngine patch") | `engine/libs/tinygltf/` (`LICENSE`, from `tiny_gltf.h`: MIT, plus the bundled base64 (zlib-style) and dlib URI decoding (Boost) code) | MIT | glTF model loading |
| [TinyXML-2](https://github.com/leethomason/tinyxml2) | 10.0.0 | `engine/libs/tinyxml2/` (`LICENSE.txt`) | zlib | XML (tilemaps, sprite sheets) |
| [Box2D](https://github.com/erincatto/box2d) | 3.1.0 | `engine/libs/box2d/` (`LICENSE`) | MIT | 2D physics |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | 5.2.0 | `engine/libs/joltphysics/` (`LICENSE`) | MIT | 3D physics |
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) | 14.3.0 | `engine/libs/harfbuzz/` (`COPYING`) | Old MIT | text shaping |
| [SheenBidi](https://github.com/Tehreer/SheenBidi) | 3.0 | `engine/libs/sheenbidi/` (`LICENSE`) | Apache-2.0 | the Unicode bidirectional algorithm |
| [Tracy](https://github.com/wolfpld/tracy) | 0.11.1 (the client, `public/`) | `engine/libs/tracy/` (`LICENSE`) | BSD-3-Clause | optional remote profiling of development games (`JASPERENGINE_TRACY`, off by default; never in the editor or release games) |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | 1.3 | `engine/libs/meshoptimizer/` (`LICENSE.md`) | MIT | mesh LODs and optimisation |
| [msdfgen](https://github.com/Chlumsky/msdfgen) | 1.13.0 (`master` at `1c106ed81178`), `core/` only; local change: `core/base.h` includes the hand-written `msdfgen-config.h` | `engine/libs/msdfgen/` (`LICENSE.txt`) | MIT | the HTML UI's MTSDF glyph atlas (scaling text, GPU text effects) |
| [recastnavigation](https://github.com/recastnavigation/recastnavigation) (Recast, Detour, DetourCrowd, DetourTileCache, DebugUtils) | 1.6.0 (`v1.6.0`, `6dc1667f5803`) | `engine/libs/recastnavigation/` (`License.txt`, `README.jasper.md`; five local patches (`PATCHES.md`): the crowd's avoidance priority, upstream fixes `9147d09`, `599fd0f`, `9f4ce64`, and the proximity grid's unsigned hash) | zlib | navigation meshes, path queries, crowds and carving obstacles (`engine/core/navigation/`); games built with `JASPERENGINE_NAVIGATION` off leave it out |
| [RmlUi](https://github.com/mikke89/RmlUi) | 6.3 (`ba95ffe8bfb6`), JasperEngine's fork | `engine/libs/rmlui/` (`LICENSE.txt`; Taffy's below, `LICENSE-taffy.txt`) | MIT | the HTML/CSS UI |
| [glslang](https://github.com/KhronosGroup/glslang) | 15.1.0 | `engine/libs/shadercompiler/libs/glslang/` (`LICENSE.txt`) | BSD-3-Clause and the others `LICENSE.txt` lists | GLSL to SPIR-V in the shader compiler |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | vendored (git history) | `engine/libs/shadercompiler/libs/spirv-cross/` (`LICENSE`, `LICENSES/`) | Apache-2.0 | SPIR-V to GLSL, HLSL and MSL in the shader compiler |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.8 | `libs/imgui/` (`LICENSE.txt`) | MIT | the editor's user interface |
| [Native File Dialog Extended](https://github.com/btzy/nativefiledialog-extended) | 1.3.0 | `libs/nfd/` (`LICENSE`) | zlib | the editor's file dialogs |
| [tinyexpr](https://github.com/codeplea/tinyexpr) (Lewis Van Winkle) | upstream `7ab30db449a3` | `libs/tinyexpr/` (`LICENSE`) | zlib | the expressions of the editor's numeric fields |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.8.0 | `libs/yaml-cpp/` (`LICENSE`) | MIT | the editor's scene, project and settings files |
| [googletest](https://github.com/google/googletest) | 1.17.0 (googletest and googlemock) | `libs/googletest/` (`LICENSE`, `README.jasper.md`) | BSD-3-Clause | the unit tests only: tests only, never shipped |

`engine/libs/shadercompiler/` itself (`src/`, the CMake build) is JasperEngine's own code (MIT, like the engine). Portions of the build and test tooling are adapted from [OpenEngine](https://github.com/Game-Crafters-Guild/OpenEngine) (MIT, Copyright (c) 2026 Gamecrafters Guild Limited and contributors), with that notice kept in each file: `engine/cmake/JasperSanitizers.cmake` (sanitizer flags, MSVC AddressSanitizer runtime staging), `tools/web/serve.py` and `tools/web/browser_gate.py` (the headless-Chrome web gate). The rounded-rectangle distance, pixel footprint, box-filter coverage, `erf` and Gaussian edge functions of the HTML UI's shaders (`engine/shaderlib/includes/htmlui_sdf.glsl`) are OpenEngine's (`Engine/Modules/Rendering/Shaders/UI/sdf_functions.glsl`), with its notice in that file.

Vector graphics of the HTML/CSS UI (all MIT):

| Library | Version | Location | Used for |
|---|---|---|---|
| [ThorVG](https://github.com/thorvg/thorvg) | 1.2.0 (`caa5af1167d7`) | `engine/libs/thorvg/` (`LICENSE`; one local patch: its global `operator new`/`delete` replacement removed, with the one allocation that relied on it (`rleRender`) made a `new`, noted in `CMakeLists.txt`) | Lottie (`<lottie>`), CPU renderer |
| [LunaSVG](https://github.com/sammycage/lunasvg) | 3.5.0 (`cf3594d5232e`) | `engine/libs/lunasvg/` (`LICENSE`) | SVG (RmlUi's SVG plugin, `.svg` textures) |
| [plutovg](https://github.com/sammycage/plutovg) | 1.3.3 (bundled with LunaSVG) | `engine/libs/lunasvg/plutovg/` (`LICENSE`; its rasteriser derives from FreeType's, `source/FTL.TXT`) | LunaSVG's rasteriser |

HTML parsing of the HTML/CSS UI's compatibility layer:

| Library | Version | Location | Used for |
|---|---|---|---|
| [lexbor](https://github.com/lexbor/lexbor) | 3.1.0 (`master` at `f4cbbcd91359`, no release tag yet) | `engine/libs/lexbor/` (Apache-2.0: `LICENSE`, `NOTICE`; one local patch, `PATCHES.md`) | HTML5 parsing of `.html` documents (`engine/core/htmlui/compat/html/HtmlParser`) |

Video of the HTML/CSS UI (`<video>`: AV1 + Opus in WebM, decoded in software; FFmpeg is never linked or shipped, the editor only runs a user-installed one to convert imports):

| Library | Version | Location | License | Used for |
|---|---|---|---|---|
| [dav1d](https://code.videolan.org/videolan/dav1d) | 1.5.4 | `engine/libs/dav1d/` (`COPYING`; what is vendored: `README.jasper.md`) | BSD-2-Clause | AV1 decoding (`engine/core/media/Av1Decoder`), C and the x86-64/aarch64/armv7 asm |
| [FFmpeg](https://ffmpeg.org) public headers | 4.4.5, 5.1.6, 6.1.2, 7.1.1, 8.0 | `engine/libs/ffmpeg-headers/`, one folder per avcodec major (`COPYING.LGPLv2.1`; what is vendored: `README.jasper.md`) | LGPL-2.1-or-later | declarations only: the Linux video backend, in the Linux editor and every Linux game, is compiled against these headers and loads the system's FFmpeg libraries with `dlopen` (no FFmpeg library is linked or shipped) |
| x86inc.asm ([x264](https://code.videolan.org/videolan/x264), bundled with dav1d) | dav1d 1.5.4's copy | `engine/libs/dav1d/src/ext/x86/x86inc.asm` (licence in its header) | ISC | the macro layer of dav1d's NASM code |
| [libopus](https://opus-codec.org) | 1.6.1 (decoder sources) | `engine/libs/opus/` (`COPYING`, `README.jasper.md`) | BSD-3-Clause (with the codec's patent licences referenced in `COPYING`) | Opus audio of videos (`OpusAudioDecoder`) |
| [libwebm](https://chromium.googlesource.com/webm/libwebm) | 1.0.0.32 (`mkvparser`) | `engine/libs/libwebm/` (`LICENSE.TXT`, `PATENTS.TXT`, `README.jasper.md`) | BSD-3-Clause, plus Google's WebM patent grant | WebM/Matroska demuxing (`VideoDemuxer`) |

The video controls' icons (`engine/core/htmlui/compat/ua/video-{play,pause,volume,muted}.svg`) are [Lucide](https://github.com/lucide-icons/lucide) icons (ISC, see below). The test clips (`engine/tests/htmlui/fixtures/video/`) and the example's `menu-loop.webm` are generated by `tools/htmlui/gen_video_fixtures.py` from FFmpeg's synthetic sources.

Procedural Material nodes of the editor (docs/specs/S90-procedural-material.md; the editor only, never in a game):

| Library | Version | Location | License | Used for |
|---|---|---|---|---|
| [Material Maker](https://github.com/RodZill4/material-maker) node library, as data | `7d6daae`, converted by `tools/procmat/import_mm_nodes.py` | `editor/procmat/nodes/` (`LICENSE`) | MIT (Rodolphe Suescun and contributors), with the notices below | the node definitions of a procedural material: GLSL and metadata, not its code generator or host |

These nodes carry their own notices, listed in each descriptor's `notices` and in that `LICENSE`: `fbm2`, `fbm4`, `sdarrow`, `sdellipse`, `sdstairs`, `sdstar`, `sdtunnel`, `voronoi`, `voronoi2` (Inigo Quilez, MIT); `anisotropic_kuwahara`, `classic_kuwahara`, `generalized_kuwahara` (Garrett Gunnell, MIT); `symmetric_nearest_neighbor` (fiveko.com, MIT); `denoiser` (Michele Morrone, BSD 2-Clause). The nodes' `rand` functions are ours (S90 R10) and use the mixing steps of MurmurHash3 (Austin Appleby, public domain). Nodes whose licence does not allow it are never imported; `tools/procmat/import_mm_nodes.py` holds the list and fails on an attribution comment it does not know.

Layout of the HTML/CSS UI:

| Code | Version | Location | License | Used for |
|---|---|---|---|---|
| [Taffy](https://github.com/DioxusLabs/taffy) (`src/compute/grid` and `src/compute/flexbox.rs`, ported to C++) | `master` at `8409f3f8c6de` | `engine/libs/rmlui/Source/Core/Layout/Grid*`, `engine/libs/rmlui/Source/Core/Layout/FlexFormattingContext.*` (`engine/libs/rmlui/LICENSE-taffy.txt`) | MIT | CSS Grid and flexbox layout in the RmlUi fork |
| [libebur128](https://github.com/jiixyj/libebur128) (the five numeric design constants of the K-weighting filters) | `master` | `engine/core/audio/AudioMeter.cpp` | MIT | BS.1770-4 K-weighting at any sample rate |

Sounds: the editor embeds 40 CC0 clips by Kenney (`editor/resources/sound-library/essentials/`, each pack's licence in `licenses/`) and installs a CC0 starter library of 927 clips by Kenney and Freesound users next to itself (`editor/resources/sound-library/library/`, licences in `licenses/`, per clip author, URL and licence in `manifest.json`). No player carries them: a game holds only the clips a project imported, credited in its `licenses/THIRD-PARTY-AUDIO.txt`. The editor also compiles SoLoud's sfxr (`engine/libs/soloud/src/audiosource/sfxr/`, part of SoLoud, zlib/libpng) for placeholder sounds; players do not.

The layered placeholder presets (`generate_sound`, `editor/audio/SoundDesign.cpp`): idea from earshot (MIT), reimplemented; no code of it is here.

Test data: the html5lib tree-construction tests (MIT, `engine/tests/htmlui/fixtures/compat/html5lib/LICENSE`). Taffy's layout test fixtures (MIT, `engine/tests/htmlui/fixtures/taffy/LICENSE-taffy.txt`). The audio tests' Ogg Vorbis clip is Kenney's "Sci-Fi Sounds" 1.0 (CC0, `engine/tests/core/fixtures/audio/License-kenney.txt`); their FLAC and MP3 sweeps are FFmpeg encodes of a synthetic source (tests only, never shipped).

Icons of the HTML/CSS UI:

| Data | Version | Location | License | Used for |
|---|---|---|---|---|
| [Lucide](https://github.com/lucide-icons/lucide) icons | 1.53.0 (`b56741cf30c0`) | `engine/libs/lucide/` (`lucide.pack`, `LICENSE`, `VERSION`) | ISC; the icons derived from [Feather](https://github.com/feathericons/feather) (listed in `LICENSE`) MIT | built-in `data-lucide` icons of HTML documents |
| [Lucide](https://github.com/lucide-icons/lucide) icons | 1.53.0 | `docs/mcp/guide/html-ui-examples/{inventory,shop}/assets/items/*.svg` (`docs/mcp/guide/html-ui-examples/LICENSE-lucide.txt`) | ISC (as above) | the item pictures of the HTML UI example library (recoloured copies) |

Shader code ported from:

| Code | Version | Location | License | Used for |
|---|---|---|---|---|
| [hextile-demo](https://github.com/mmikk/hextile-demo) (`hextiling.h`, ported to GLSL), the reference code of Morten S. Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 11(2), 2022 | `master` at `43c3ed7e18e1` | `engine/shaderlib/includes/stochastic.glsl` (`engine/shaderlib/includes/LICENSE.hextiling.txt`) | MIT | Stochastic Tiling of terrain layers and of the library shaders' `STOCHASTIC` feature |
| [spryte](https://github.com/debuffjp/spryte) (the logic of `src/draw.ts` `Math.round` of an unrotated sprite's translation, `src/webgl2.ts` `COPY_VERT` / `EACH_VERT` snapping, `src/panels.ts` `snapEdges`; rewritten in C++ and GLSL, with a tie bias of our own) | the research clone of 2026-11-04 | `engine/core/pixelart/PixelSnap.*`, `engine/shaderlib/includes/pixel_snap.glsl`, the tilemap edges of `engine/core/subsystem/MeshSystem.cpp` (`engine/core/pixelart/LICENSE.spryte.txt`) | MIT | the pixel-snap rule of the Pixel Perfect Camera (S41) |

UI effects of the HTML/CSS UI: the effects of `engine/shaderlib/effects/` marked `@source shaders.com <Name>` and the include library `engine/shaderlib/effects/lib/` are GLSL ports of [shaders.com](https://shaders.com/) effects and kit functions (MIT, Copyright (c) 2026 Shader Effects Inc.). The notice and the list of ported effects are in `engine/shaderlib/effects/LICENSE.shaders-com.txt` (generated by `tools/htmlui/gen_effects_docs.py`); exports that ship such an effect copy it to `licenses/shaders.com-LICENSE.txt`.

Fluid simulation: the FluidSimulation component and the HTML UI `fluid` effect port [WebGL-Fluid-Simulation](https://github.com/PavelDoGreat/WebGL-Fluid-Simulation) by Pavel Dobryakov (MIT, Copyright (c) 2017 Pavel Dobryakov): his solver and look kernels (`engine/shaderlib/effects/lib/fam_fluid_solver.glsl`, `engine/shaderlib/effects/lib/fam_fluid_look.glsl`; these two are his, not shaders.com's), the native passes on them (`engine/shaderlib/fluid.vert`, `engine/shaderlib/fluid.frag`) and his simulation loop, defaults and splats (`engine/core/fluid/`). His blue-noise dithering texture (`LDR_LLL1_0.png`, whose licence is not stated) was not copied: the dither is procedural. The notice is `engine/shaderlib/effects/LICENSE.webgl-fluid-simulation.txt`; exports that run the fluid (a FluidSimulation component in an exported scene or prefab, or the `fluid` effect) copy it to `licenses/webgl-fluid-simulation-LICENSE.txt`.

Texture synthesis and sprite normal maps of the editor (editor-only: never part of an exported game):

| Code | Version | Location | License | Used for |
|---|---|---|---|---|
| [texture-synthesis](https://github.com/EmbarkStudios/texture-synthesis) by Embark Studios (`lib/src`, ported to C++) | 0.8.2 (`22825ec4ad25`) | `editor/texsynth/` (`editor/texsynth/LICENSE-texture-synthesis.txt`) | MIT OR Apache-2.0 (used under MIT) | `jasperengine-editor synthesize-texture`: new, tileable, inpainted and guided textures from examples |
| [spritefusion-sprite-normals](https://github.com/Hugo-Dz/spritefusion-sprite-normals) by Hugo Duprez (`src`, ported to C++) | 0.1.0 (`d14b067`) | `editor/spritenormals/` (`editor/spritenormals/LICENSE-spritefusion-sprite-normals.txt`; C++ port of the normal-map generator) | MIT | Window > Sprite Normal Map, `jasperengine-editor generate-sprite-normals` and the `generate_sprite_normals` MCP tool: normal maps for 2D sprites, sheets, animations and tilesets |

Aseprite import of the editor (S44, [docs/aseprite.md](docs/aseprite.md); editor-only: an exported game ships the import's sheet, pages and clips, never the `.aseprite` codec):

| Code | Version | Location | License | Used for |
|---|---|---|---|---|
| [Aseprite](https://github.com/aseprite/aseprite) Document Library (`src/doc/blend_funcs.cpp`, `src/doc/render_plan.cpp`, `src/doc/blend_internals.h`), Render Library (`src/render/render.cpp`) and Document IO Library (`src/dio/aseprite_decoder.cpp`, `aseprite_encoder.cpp`: the chunk walk, chunk order and user data ownership), ported to C++ | `main` (v1.3.x) | `editor/pixelart/aseprite/AsepriteBlend.*`, `AsepriteRender.cpp`, `AsepriteRead.cpp`, `AsepriteWrite.cpp` (`editor/pixelart/aseprite/LICENSE-aseprite-libraries.txt`) | MIT | the blend modes and the composition of a frame as Aseprite draws it; reading and writing `.aseprite` files |
| [RectangleBinPack](https://github.com/juj/RectangleBinPack) by Jukka Jylänki (`MaxRectsBinPack`, ported to C++, rotation removed) | `master` | `editor/sprite/SpritePacker.cpp` (licence in its header) | public domain | packing sprite frames into pages |

Test data of the Aseprite import (tests only, never shipped): the sprites of Aseprite's `tests/sprites` (MIT, `editor/tests/fixtures/aseprite/aseprite-tests/LICENSE.txt`, `UPSTREAM-README.md`), the test files of [asefile](https://github.com/alpine-alpaca/asefile) (MIT, `editor/tests/fixtures/aseprite/asefile/LICENSE`) with their Aseprite PNG exports, and the sprites of [tenten](https://github.com/debuffjp/tenten) by debuff (`b9a57fb01ec3`) (MIT, `editor/tests/fixtures/aseprite/tenten/LICENSE`), whose `src/aseprite.ts` was also read for the chunk walk and palette precedence (no code copied). `editor/tests/fixtures/aseprite/README.md` lists every file and what it covers.

FBX import of the editor (editor-only: an exported game ships the converted `.glb`, never ufbx):

| Library | Version | Location | License | Used for |
|---|---|---|---|---|
| [ufbx](https://github.com/ufbx/ufbx) | 0.23.1 (as vendored by Doriax `8e8dde5d6`) | `libs/ufbx/` (`LICENSE`) | MIT OR Unlicense (public domain), used under MIT | reading `.fbx` files, converted to `.glb` by `editor/util/FbxImporter` and `GlbBuilder` |

EXR reading of the editor (editor-only: the Vertex Animation importers re-pack EXR textures into `.vat.glb` payloads, exported games never read EXR):

| Library | Version | Location | License | Used for |
|---|---|---|---|---|
| [tinyexr](https://github.com/syoyo/tinyexr) (v1: `tinyexr.h` with the two headers it includes, `exr_reader.hh` and `streamreader.hh`; it contains OpenEXR-derived code under the same BSD-3-Clause terms, noted in its header) | v3.2.0 release (`6f470c9ab24b`); local patch: the Huffman encoder's bit buffer (`outputBits`, `outputCode`, `sendCode`, `hufPackEncTable`, `hufEncode`) is `unsigned long long` as in OpenEXR (signed, its shifts past the top bit were undefined behaviour) | `libs/tinyexr/` (`LICENSE`; configured in `CMakeLists.txt`: deflate through the engine's stb_image/stb_image_write zlib, not miniz or zlib) | BSD-3-Clause | reading and writing `.exr` images (`editor/util/FloatImage`) |

Vertex Animation (`.vat`, [docs/vertex-animation.md](docs/vertex-animation.md)): no third-party code is copied. The formats and formulas below were reimplemented from their sources and are credited here:

| Source | License | What was taken | Where |
|---|---|---|---|
| [SideFX Labs](https://github.com/sideeffects/SideFXLabs): the Houdini Vertex Animation Textures ROP 3.0 and its Unity Shader Graphs (`VAT_SoftBodyDeformation`, `VAT_RigidBodyDynamics`, `VAT_DynamicRemeshing`) | BSD | the VAT 3 format and its decoding: the flags in the bounds' digits, texel addressing, LDR remaps, Two Position Textures, smallest-three rigid quaternions, the precomputed-angle slerp, smooth trajectories, compressed spheremap normals, the fluid lookup table; also the quaternion tangent frame | the Houdini importer (`editor/vat/import/HoudiniVat3.cpp`, decoded on the CPU at import) |
| G4ND44/Godot_VAT3 (GitHub) | MIT | a second reference for the VAT 3 decoding | `editor/vat/import/HoudiniVat3.cpp` |
| OpenVAT's engine decoders (OpenVAT-Engine_Tools) | MIT | the OpenVAT file layout (per-channel remap, packed normals, `remap_info.json` animations); the A/B crossfade computed on the GPU from one clock | `editor/vat/import/OpenVat.cpp`, `engine/shaderlib/includes/vat.glsl`. The OpenVAT Blender add-on (GPL) is not used |
| keijiro/HdrpVatExample (GitHub) | Unlicense | integer-addressed `texelFetch` decoding; the shrinking of rigid pieces that lerped matrices cause, which Bones mode avoids (quaternion nlerp) | `engine/shaderlib/includes/vat.glsl` |
| maxartz15/VertexAnimation (GitHub) | MIT | a per-vertex index attribute that survives reordering and is shared by LODs (`_VAT_INDEX`); mesh bounds over every frame | the baker (`editor/vat/`), `engine/core/animation/VertexAnimation` |
| fuqunaga/VatBaker (GitHub) | MIT | frame timing: loop clips sampled without a duplicate end frame and interpolated from the last frame back to the first | the baker, `engine/core/animation/VertexAnimation` |
| sandwichpuissant/Unity-VAT (GitHub) | Unlicense | positions stored as offsets from the rest pose | the baker, `engine/shaderlib/includes/vat.glsl` |
| sugi-cho/Animation-Texture-Baker (GitHub) | MIT | a random start time per instance | `engine/core/subsystem/VertexAnimatorSystem` |

Bonjour-Interactive-Lab/Unity3D-VATUtils was read as a VAT 3 decoding reference only (its repository licence is GPL-3.0 while its `package.json` says MIT): none of its code is copied, and the math it implements is SideFX Labs'. The SideFX "VAT in Unity" tutorial exports used by the importer tests are not in the repository: the tests read them from outside it when present and skip otherwise.

Convex decomposition of Physics Placement (edit-time placement in the Scene view and for agents):

| Library | Version | Location | License | Used for |
|---|---|---|---|---|
| [V-HACD](https://github.com/kmammou/v-hacd) by Khaled Mamou (version 4 by John W. Ratcliff) | 4.1.0 (`master` at `f900e4236149`, the single header `include/VHACD.h`) | `engine/libs/vhacd/` (`LICENSE`, `README.jasper.md`; three local fixes for its multithreaded mode, `jasper.patch`) | BSD-3-Clause | splitting concave meshes into convex hulls (`engine/core/physics/ConvexDecomposition`), the editor on desktop only, never exported games (`JASPERENGINE_CONVEX_DECOMPOSITION`) |

Texture compression (cooked KTX2 textures, spec S14, `docs/textures.md`). Games link only Zstandard's decoder, bcdec and, on the web, the Basis Universal transcoder; the encoders are in the editor alone:

| Library | Version | Location | License | Used for |
|---|---|---|---|---|
| [Zstandard](https://github.com/facebook/zstd) | 1.5.7 | `engine/libs/zstd/` (`LICENSE`, `README.jasper.md`) | BSD-3-Clause | the supercompression of cooked KTX2 textures: the decoder in games, the encoder in the editor's cook |
| [xxHash](https://github.com/Cyan4973/xxHash) | 0.8.2 (Zstandard 1.5.7's copy) | `engine/libs/xxhash/` (`LICENSE`, `README.jasper.md`; local change: Zstandard's prelude removed) | BSD (dual BSD / GPLv2) | content hashes of shared GPU buffers and mesh LOD keys (XXH3) |
| [bcdec](https://github.com/iOrange/bcdec) | `main` at `80859ed3b7af` | `engine/libs/bcdec/` (`LICENSE`, `README.jasper.md`) | MIT or Unlicense | BC1-BC7 CPU decoding: the fallback when a GPU cannot sample a cooked BC format, editor previews |
| [Basis Universal](https://github.com/BinomialLLC/basis_universal) | 2.50 (`v2_50`) | `engine/libs/basisu/` (`LICENSE`, `NOTICE`, `LICENSES/`, `README.jasper.md`; local change: `zstd/zstd.h` forwards to the engine's Zstandard) | Apache-2.0; its encoder's bundled tinyexr BSD-3-Clause, qoi and tinydds MIT, miniz MIT, jpgd public domain (`LICENSES/`) | ETC1S / UASTC / UASTC HDR encoding of web textures, UASTC to ETC2/EAC, `.exr` reading (editor); the transcoder of web textures (web games, editor) |
| [astc-encoder](https://github.com/ARM-software/astc-encoder) | 5.7.0 | `engine/libs/astcenc/` (`LICENSE.txt`, `README.jasper.md`) | Apache-2.0 | ASTC 4x4-12x12 encoding and decoding of mobile textures (editor only) |
| [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) | `master` at `b9438627eef7` | `engine/libs/bc7enc_rdo/` (`LICENSE`, `README.jasper.md`) | MIT or Unlicense | BC1/BC3/BC4/BC5/BC7 encoding with rate-distortion optimisation of desktop textures (editor only) |

Shipping of the editor (S30, `docs/shipping.md`; editor-only: never part of a game):

| Library | Version | Location | License | Used for |
|---|---|---|---|---|
| [miniz](https://github.com/richgel999/miniz) | 3.0.2 | `libs/miniz/` (`LICENSE`, `README.jasper.md`) | MIT | zip reading and writing: player runtime archives, APK and AAB assembly, the Project Hub's zip import (S27) |
| [bundletool](https://github.com/google/bundletool) | 1.17.2 (`bundletool-all-1.17.2.jar`, SHA-256 `2d4ad908faea64047c1cc9cb747e6aa667c6ab192e09607bd16b67246a8cd6ae`) | not vendored: downloaded by the editor into its cache on the first Android App Bundle build (`editor/platform/AndroidTools.cpp`) and checked against that hash | Apache-2.0 | building `.aab` files from a prebuilt Android player |

The code editor's line diff (`editor/util/LineDiff.cpp`, from Doriax) follows the clean-up passes of [Visual Studio Code](https://github.com/microsoft/vscode)'s line diff (`defaultLinesDiffComputer`: joining and sliding changes over repeated lines, better boundaries, joining across short matches; MIT, Copyright (c) Microsoft Corporation), so its change markers land where VS Code's do; the file names that origin.

Code from [OpenEngine](https://github.com/Game-Crafters-Guild/OpenEngine) (MIT, Copyright (c) 2026 Gamecrafters Guild Limited and contributors), used by the OpenEngine port (`docs/specs/`). Each adapted file names its source in its header.

| Code | Location | Used for |
|---|---|---|
| `Apps/Editor/Source/Diagnostics/MainThreadHangWatchdog.{h,cpp}` (the heartbeat armed by the first beat, the poll, the re-report interval, the "recovered after" line, the shutdown window, the job-occupancy section; adapted) and `Tests/Editor/MainThreadHangWatchdogTests.cpp` (ported cases) | `engine/core/diag/HangWatchdog.*`, `engine/tests/core/diag/HangWatchdogTest.cpp` | the main-thread hang watchdog (S28) |
| `Engine/Modules/Types/Include/Types/NearestName.h` (copied, namespace changed) | `engine/core/util/NearestName.h` | "did you mean" suggestions of the scene load report (S03), graph issues and particle effect patches (BF23) |
| `Engine/Modules/NativeScripting/Include/NativeScripting/NativeScriptingABI.h` (the `GE_ToolchainFingerprint` fill, adapted and extended) | `engine/core/script/ToolchainFingerprint.h` | the toolchain fingerprint of C++ script libraries (S23) |
| `Apps/Editor/Source/ScriptEditor/SourceLocation.cpp` (the path-start scan and the `:L:C` / `(L,C)` readers, adapted and extended) | `editor/util/SourceLocation.*` | the Console's source-location parser (S23) |
| `Engine/Modules/FileWatcher` (FileWatcher, FileIdentity), `Engine/Include/Assets/ExpectedWriteLedger.h` (+ its tests), `Engine/Modules/AssetCore/.../SharedFileRead.*` (adapted, headers kept) | `engine/core/io/watch/`, `engine/tests/core/io/watch/ExpectedWriteLedgerTest.cpp` | the file watcher's rename pairing and delete hold, file identities, the self-write ledger, share-delete reads on Windows (S05) |
| `Tools/Scripts/publish_game.py` (`validate_payload`, `binary_target`, `tree_manifest`, `vdf_string`, the SteamPipe scripts, `prepare`/`verify_release`, the Deck deploy's verify-then-swap; adapted, rewritten in C++) and `Tools/Web/export_web_player.py` (the player manifest idea) | `editor/publish/` (`Publishing`, `PayloadValidator`, `BinaryInfo`, `TreeManifest`) | releases, Steam and Steam Deck publishing (S30) |
| `Engine/Modules/AssetCore/Source/GUID.cpp`: `GUID::Derive`, `FnvHash`, `SplitMix64` (adapted) | `engine/core/asset/Guid.cpp` (`Guid::derive`) | deterministic sub-asset GUIDs (asset identity, S04) |
| `Modules/Memory` (`Arena`, `ScratchScope`, `ObjectPool`, `RangeAllocator`, `SerialUseCheck`, `AddressSanitizer`), adapted | `engine/core/memory/` | the allocators, adapted to our naming and stats registry (S06) |
| `Modules/JobSystem` (`ParallelForCore`, `ParallelAlgorithms`, `JobCounter`, `JobChannel`, the `WorkStealingThreadPool` lane contracts), adapted | `engine/core/jobs/` | parallelFor, counters, blocking channels; the scheduler itself is our own (S06) |
| `AssetDecodeGate`, `AssetIOService`, adapted | `engine/core/jobs/BackgroundGate.*`, `engine/core/io/AssetIOService.*` | frame protection and the asset readers (S06) |
| Tests (`ParallelAlgorithmsTests`, `AssetDecodeGateTests`, `AssetIOServiceTests`, Memory tests, `MemoryBench`), ported | `engine/tests/core/` | test cases and benchmark loops (tests only, never shipped) |
| `Engine/Modules/Rendering/Shaders/Includes/specular_aa.glsl`, adapted | `engine/shaderlib/includes/specular_aa.glsl` | geometric specular anti-aliasing of the High Lit Quality (S11) |
| `Engine/Modules/Rendering/Shaders/Includes/iridescence.glsl`, adapted | `engine/shaderlib/includes/iridescence.glsl` | the thin-film Fresnel of KHR_materials_iridescence (S11) |
| `Engine/Modules/Rendering/Shaders/Includes/terrain_material_albedo.glsl` (the colour-slotted hex lattice), adapted with an integer hash | `engine/shaderlib/includes/hex_tiling.glsl` | hex tiling of materials and terrain layers (S11) |
| `Engine/Modules/Rendering/Shaders/Includes/standard_pbr.glsl` (anisotropic GGX, Charlie visibility, the sheen albedo fit), adapted | `engine/shaderlib/includes/pbr_ext.glsl` | the extended lobes of the built-in Lit (S11) |
| `Engine/Include/Components/Rendering/LightPhotometry.h` (black body fit, lumen to candela), adapted | `engine/core/util/LightPhotometry.h` | photometric light units and colour temperature (S11) |
| `Engine/Modules/Rendering/Source/Common/CubeLutFileParser.cpp`, `CubeLutColorDomain.cpp` and the curves and gamut matrices of `Shaders/Includes/cube_lut.glsl` (ported to C++) | `engine/core/util/CubeLut.*` | `.cube` LUT parsing, the input encoding proposal and the scene-stage colour maths (S13) |
| `Engine/Modules/Pathfinding/Source/` (`DetourNavMap.cpp`: the Recast config derived from one cell size, tile validation before `addTile`; `ObstacleAvoidance.cpp`: the dtCrowd setup and avoidance presets; `GridMap.cpp`, `HexGridHelpers.cpp`, `AStarSolver.cpp`, `AStarSolverPool.cpp`, `PathSmoother.cpp`: flat grid cells, hex neighbour tables, generation-stamped A* scratch, the solver pool, line-of-sight smoothing), `Engine/Modules/PathfindingECS/Source/NavigationSourceCollector.cpp` (what a bake collects) and `NavigationMeshBake.cpp` (tile jobs owning their input), adapted | `engine/core/navigation/` | navmesh bakes, crowds and grid A* (S19); the hex point lookup is exact cube rounding instead of theirs |
| `Engine/Source/Engine/Rendering/DynamicResolutionController.cpp` and its header (adapted) | `engine/core/render/DynamicResolutionController.*` | the GPU law of dynamic resolution (S13) |
| `Engine/Modules/Spline` (SplineTypes, SplineData, SplineEvaluator, SplineUtility), `Engine/Modules/SplineGeometry` (SplineStation, SplineVertex, SplineProfile, SplineStripBuilder, SplineCorner, SplineEndTaper, SplineStep, SplineFillField, SplineFillMesher), `Engine/Modules/SplineLayout` (TileLayout, FenceLayout, SpanMitre, CenterlineSampling, PieceBasis, PieceAxis), `Engine/Include/Components/Spline/SplineStationJitter.h`, `Engine/Modules/TerrainECS/.../TerrainRoutePolyline.*` and the stroke algorithms of `Apps/Editor/Source/SceneView/SplineTool.cpp`, adapted to our math and right-handed frame, with their tests ported; the field names and defaults of `Engine/Include/Components/Spline/{SplineExtrude,SplineWall,SplinePlacement,SplineFence}.h` | `engine/core/spline/`, `engine/tests/core/spline/`, `engine/core/component/Spline*Component.h`, `editor/util/TerrainSplineBake.*`, `editor/spline/` | splines and what is built on them (S18); every export links them and ships `licenses/OpenEngine-LICENSE.txt` (`engine/core/spline/LICENSE.OpenEngine.txt`) |
| `Engine/Modules/Rendering/Shaders/vhs.frag`, `vhs_interference.frag`, `vhs_signal_glitches.frag` (adapted, with the punikonta notice below) | `engine/shaderlib/includes/post_vhs.glsl` | the uber pass's VHS tape (S13) |
| `CubeLutFileParserTests.cpp` and `Tests/Fixtures/CubeLut/*.cube`, `DynamicResolutionControllerTests.cpp` (ported) | `engine/tests/core/util/CubeLutTest.cpp`, `engine/tests/core/fixtures/cubelut/`, `engine/tests/core/render/DynamicResolutionControllerTest.cpp` | test cases and fixtures (tests only, never shipped) |
| `Source/Engine/Rendering/ShadowMapRenderFeature.cpp` (`SliceBoundingSphereRadius`, `CascadeLightUp`, `GetCascadeFitTuning`), adapted | `engine/core/render/shadow/CascadeFit.*` | the cascades' bounding-sphere fit, the near-vertical light basis and the fit freeze bands (S10) |
| `PointShadowAtlasPlanner` (importance ranking, sticky slots, challenge margin and cooldown), adapted | `engine/core/render/shadow/ShadowSlotPlanner.cpp` | shadow slots by importance (S10) |
| Shader includes `shadow_receiver_plane.glsl`, `shadow_filter.glsl` (`GE_VogelDisk`), `shadow_sampling.glsl` (`GE_PoissonRotation`), `shadow_cascade_blend.glsl`, `shadow_distance_fade.glsl`, `shadow_pcss_decisions.glsl`, adapted with every depth comparison inverted | `engine/shaderlib/includes/shadow_sampling.glsl`, `engine/shaderlib/includes/shadow_pcss_decisions.glsl` | the receiver-plane bias, the Vogel filter, the cascade blend and fade, PCSS (S10) |
| `ShadowPcssDecisionTests`, ported | `engine/tests/core/render/PcssDecisionTest.cpp` | the PCSS decision tests (tests only, never shipped) |
| `Engine/Modules/Rendering/Shaders/Includes/height_fog_term.glsl` (closed-form height fog optical depth, after [SanielX/Height-Fog](https://github.com/SanielX/Height-Fog), MIT, commit `ab6f8278`), rewritten | `engine/shaderlib/includes/height_fog.glsl`, `engine/core/render/AtmosphereMath.cpp` | the height fog (S12) |
| `Apps/Editor/Source/UI/UniversalSearchProvider.cpp` (`NormalizeForSearch`, `DamerauDistance`, `SubsequenceScore`, `FuzzyScore` tiers, `KindPriority`, `ParseQuery`), ported | `editor/search/SearchScorer.*`, `editor/search/SearchEngine.*` | the one fuzzy scorer and the Search palette's ranking and prefixes (S25) |
| `Apps/Editor/Source/Search/UniversalSearchController.cpp` (snapshot on open, scoring off the main thread with a version counter), adapted | `editor/search/SearchPalette.*` | the Search palette (Ctrl+K) (S25) |
| `Apps/Editor/Include/Editor/Shortcuts/EditorShortcuts.h` (the catalog's columns, `FindShortcutConflict`, the capture latch `SetShortcutCaptureActive` / `NoteShortcutCaptureConsumedKey`), adapted | `editor/shortcuts/CommandCatalog.*`, `editor/shortcuts/ShortcutDispatcher.*` | the command catalog, conflicts and rebinding capture (S25) |
| `EditorApplication_LayoutPresets.cpp` (unique names on import, the prompt before leaving a modified layout), UX rules | `editor/layout/` | window layout presets (S25) |
| `Apps/Editor/Source/Editor/Settings/EditorSettingsRegistry.cpp` (`RegisterCategory`'s refusal of unwired rows) | `editor/settings/SettingsRegistry.*` | the settings registry (S25) |
| `Source/UI/EditorTags.cpp` (the nine tag colours) | `editor/organisation/AssetLabels.cpp` | the default asset label colours (S25) |
| `Include/UI/SmartFolder/SmartFolder.h` (filter kinds, AND/OR) | `editor/organisation/SavedSearchRows.*` | the structured editor of saved searches (S25) |
| `BookmarksPanel` (kinds, drag in, reorder, drop to navigate), layout | `editor/window/BookmarksWindow.*` | the Bookmarks window (S25) |
| `Apps/Editor/Source/Panels/AssetsPanel.cpp` (type-ahead: 750 ms repeat window, A-Z/0-9; 16 px Ctrl+wheel size steps) | `editor/window/ProjectWindowSearch.cpp` | the Project window's type-ahead and item sizes (S25) |
| `Engine/Modules/HeightFog/Source/HeightFogEffectPresets.cpp` (looks re-derived) | `engine/core/render/AtmospherePresets.cpp` | the Fog presets (S12) |
| `Engine/Modules/VolumetricClouds/Shaders/volumetric_clouds_march.frag` and `volumetric_clouds_worley.glsl` / `_noise.comp` (after [SebLague/Clouds](https://github.com/SebLague/Clouds), MIT, Copyright (c) 2019 Sebastian Lague), ported and extended | `engine/shaderlib/volumetric_clouds.frag`, `engine/core/render/AtmosphereNoise.cpp` | the volumetric clouds' march and CPU noise bake (S12) |
| `Engine/Modules/UI/Include/UI/Controls/VirtualWindowCore.h` (and the update reasons of `VirtualizationCoordinator.h`), copied, namespace and naming changed; its unit tests (ported) | `engine/core/htmlui/widgets/VirtualWindowCore.*`, `engine/tests/htmlui/widgets/VirtualWindowCoreTest.cpp` | the visible window, overscan and anchoring of the HTML UI's list, grid, tree and table views (S17) |
| `ListView`'s row height index (a Fenwick tree), rewritten | `engine/core/htmlui/widgets/HeightIndex.h` | variable row heights of the views (S17) |
| `Engine/Modules/UI/Source/Parsers/` (`MarkdownParser`, `MarkdownInlineParser`, `MarkdownDocument.h`, `MarkdownSyntax.h`), copied and adapted (nested lists, link and image URL hooks); `Tests/MarkdownParserTests.cpp` (ported) | `engine/core/htmlui/widgets/Markdown/`, `engine/tests/htmlui/widgets/MarkdownParserTest.cpp` | the `<markdown>` element and `data-markdown` (S17) |
| `Engine/Modules/UI`: `Interaction/TooltipOverlay` (delay, hop window, hide triggers), `Layout/DropdownPopupPlacement` and `Tests/TooltipPlacementTests.cpp`, `Interaction/DragDropManager` (typed payloads, accepted operations), `UIHotReload` matching and `Tests/UIHotReloadTests.cpp`: rules re-implemented, test cases ported | `engine/core/htmlui/widgets/{TooltipManager,PopupPlacement,DragDropSession}.*`, `engine/core/htmlui/compat/TreeReconcile.*`, `engine/tests/htmlui/widgets/{PopupPlacementTest,TreeReconcileTest}.cpp` | tooltips, popup placement, HTML5 drag and drop and the in-place hot reload of HTML documents (S17) |
| `Engine/Modules/MeshPicking/Source/MeshBvh.cpp` and `Include/MeshPicking/{MeshBvh.h,PickTypes.h}` (binned-SAH build, the CWBVH8 collapse with 8-bit quantized child boxes, the AVX2/NEON/scalar 8-child slab test, Möller-Trumbore, brute force below 256 triangles, serialization), adapted: runtime SIMD dispatch, culled faces, all-hits, refit | `engine/core/picking/MeshBvh.*`, `engine/core/picking/MeshBvhSimd*`, `engine/core/picking/PickTypes.h` | triangle-exact picking and MeshRaycast (S26) |
| `Engine/Modules/SceneBvh/{Include/SceneBvh/BinnedSahSplit.h,Source/BinnedSahSplit.cpp}`, adapted (the three axes binned in one pass) | `engine/core/picking/BinnedSah.*` | the SAH split of both BVHs (S26) |
| `Engine/Modules/Scene/Source/{TlasBuilder,TlasRefit,TlasTraverse}.cpp` (the binned build, bottom-up refit and ray/frustum traversal over entity bounds), adapted without sectors or async requests; `Apps/Editor/Source/Picking/MeshBvhCache.cpp` (coalescing, invalidation, the pending policy) and `MeshBvhDiskFormat` (the header checks), adapted | `engine/core/picking/{SceneBvh,PickCache}.*`, `editor/picking/BvhDiskCache.*` | the scene's pick BVH, the shared mesh trees and their disk cache (S26) |
| `Apps/Editor/Source/SceneViewController.cpp` (`PickViaClickThrough`, Ctrl+wheel depth cycling, the drop order of `SceneView/SceneViewDropPlacement.cpp`), `Picking/PickRootResolver.cpp` (the candidate list), the behaviour and constants of `UI/Controls/Widgets/CameraBookmarksWidget.cpp`, `Panels/SceneCameraSettingsPopup.cpp`, `UI/Controls/Widgets/SceneViewRulerOverlay.cpp` (the 1-2-5 major step), `Panels/HierarchyPanel.cpp` (the lock toggle): rules re-implemented on ImGui | `editor/picking/ScenePicker.*`, `editor/picking/IconPicker.*`, `editor/sceneview/`, `editor/window/HierarchyVisibilityGutter.cpp` | the Scene view's click flow, camera bookmarks, Scene Camera settings, rulers, hide and pick lock (S26) |
| `Tests/MeshBvhTests.cpp`, `Tests/SceneTlasTests.cpp` (cases ported) | `engine/tests/core/picking/` | test cases (tests only, never shipped) |
| `Engine/Modules/Noise/Include/Noise/GradientCurl3D.h` (copied, vector type and namespace changed) and its 4-wide port; `Tests/GradientCurl3DTests.cpp` (reference samples, ported) | `engine/core/math/noise/GradientCurl3D.h`, `GradientCurl3DSimd.h`, `CurlNoiseBatch.*`, `engine/tests/core/particles/CurlNoiseTest.cpp` | the particle Noise module's curl noise (S20) |
| `Engine/Modules/Rendering/Shaders/Includes/particle_six_way.glsl` (the three map layouts and the squared-direction response), adapted | `engine/shaderlib/includes/particles_fs.glsl` | six-way smoke lighting (S20) |
| `Engine/Modules/Particles` (`ParticleChannels`, `ParticleRuntime`, `ParticleStackCompiler` with `ContinuousEmissionPeak` and the structure comparison, `ParticleStackValidation`, `ParticleValue` with `RemapParticleInput`, the processors, `ParticleTrailStore`, `ParticleExtraction`): designs and rules followed, rewritten for SoA SIMD kernels | `engine/core/particles/` | the particle system (S20); each file names what it follows |
| `Engine/Source/Assets/Textures/BC7TransparentEndpointRepair.cpp`, `Engine/Include/Assets/Textures/BC7TransparentEndpointRepair.h` (copied, namespace and integer types changed); `Engine/Tests/BC7TransparentEndpointRepairTests.cpp` (ported) | `editor/texture/BC7TransparentEndpointRepair.*`, `editor/tests/texture/BC7TransparentEndpointRepairTest.cpp` | transparent texels of BC7 cooks stay at alpha 0 (S14) |
| `Engine/Modules/VCSIntegration`: `VCSCommandExecutor.cpp` (argument quoting on Windows, the concurrent pipe drain, kill on timeout), `VCSStatusPoller` (the cooldown and wake coalescing), `VCSPathKey.h` (lexical repository-relative keys), adapted; `Tests/VCSCommandExecutorTests.cpp`, `VCSStatusPollerTests.cpp` (cases ported) | `editor/vcs/ProcessExec.*`, `editor/vcs/VcsStatusPoller.*`, `editor/vcs/VcsPathKey.h`, `editor/tests/vcs/ProcessExecTest.cpp`, `editor/tests/vcs/VcsStatusPollerTest.cpp` | the version control process runner, status poller and path keys (S29) |
| `Packages/git-vcs/Editor/GitIntegration.cpp`, `Packages/svn-vcs/Editor/SVNIntegration.cpp` (the command sets), followed and rewritten (argv only, porcelain v2, XML) | `editor/vcs/GitProvider.*`, `editor/vcs/SvnProvider.*` | the Git (with LFS locks) and Subversion providers (S29) |
| `Apps/Editor/Source/VersionControl/`: `SceneDiff` (the result model), `SceneDiffVcs` (the baseline memo), `InspectorVcsController` (the baseline tooltip and revert items), `Ui/VcsCommitDialog`, `Ui/VcsLogViewer` (layouts), and `Apps/Editor/Include/UndoRedo/VcsRevertCommand.h`, followed and rewritten for our YAML, commands and ImGui; `Tests/Editor/SceneDiffTests.cpp` (cases ported to our YAML) | `editor/vcs/scene/SceneDiff.*`, `editor/vcs/SceneDiffTracker.*`, `editor/window/InspectorVcs.cpp`, `editor/command/type/RevertFilesCmd.*`, `editor/command/type/RevertToBaselineCmd.*`, `editor/window/vcs/VcsCommitWindow.*`, `editor/window/vcs/VcsHistoryWindow.*`, `editor/tests/vcs/SceneDiffTest.cpp` | the semantic scene diff, Revert to Baseline, the undoable revert and the Commit and History windows (S29) |
| `Apps/Editor/Source/Projects/ProjectCatalog.cpp` (`IsValidCatalogId`, `IsSafeRelativePath`, copied with its notice) and the template manifest's fields (`Apps/Editor/ProjectTemplates/manifest.json`: id, name, description, category, order, accentColor, thumbnail, startScene) | `editor/hub/HubPaths.*`, `editor/hub/TemplateCatalog.*` | the Project Hub's ids, safe paths and project templates (S27) |
| `Apps/Editor/Source/Projects/ProjectAcquisition.cpp` (the Git lookup, the argv-only shallow clone and its progress) and `ZipArchive` (the entry checks before extraction), adapted | `editor/hub/ProjectImporter.*` | importing a project from a zip or a Git URL (S27) |
| `Apps/Editor/Source/Panels/ColorPicker.cpp` (the callback triad, the palette model: rows of 19, at most 4 rows, a spectrum row), `Apps/Editor/Source/UI/Controls/CurveField.cpp` (opening on click) and `CurvePresets.cpp` (presets fitted to the range, "My Preset N"), followed | `editor/widget/ColorPickerWindow.*`, `editor/widget/SwatchLibrary.*`, `editor/widget/CurveField.*`, `editor/widget/CurvePresets.*` | the Color window, swatches, curve fields and presets (S27) |
| `Engine/Modules/Platform/Source/Window.cpp` (the screen reads of the colour sampler: `XGetImage`, `BitBlt` of the desktop DC, `CGWindowListCreateImage`), adapted to one 9x9 block per frame (`NSColorSampler` on macOS) | `editor/platform/ScreenColor*` | the eyedropper outside the editor's views (S27) |
| `Apps/Editor/Source/MovieRecorderController.cpp` (resolution and bitrate presets), `GameViewMovieCapture.cpp` (the bounded frame queues) and `VideoWriterFFmpeg.cpp` (the H.264 encoder preference list, the raw-RGBA pipe), adapted | `editor/recorder/` | the Recorder (S27); FFmpeg itself is the user's program, never linked or shipped |

The extended lobes' formulas (`engine/shaderlib/includes/pbr_ext.glsl`) follow the [Khronos glTF-Sample-Renderer](https://github.com/KhronosGroup/glTF-Sample-Renderer) shaders (Apache-2.0, Copyright The Khronos Group Inc.), as `includes/pbr.glsl`, `brdf.glsl` and `ibl.glsl` already do.

Test assets (never shipped: `engine/tests/` stays out of the editor install and of exports): the [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) models of `engine/tests/render/assets/khronos/`, each with its `LICENSE.md` and `metadata.json`. The CC-BY-4.0 ones are credited here:

| Model | Location | License | Credit |
|---|---|---|---|
| ClearCoatTest | `engine/tests/render/assets/khronos/ClearCoatTest/` (a copy in the test fixture `tools/fixtures/pbr-extended-export/assets/models/ClearCoatTest/`) | CC-BY-4.0 | Ed Mackey, © 2020 Analytical Graphics, Inc. |
| AnisotropyRotationTest | `engine/tests/render/assets/khronos/AnisotropyRotationTest/` | CC-BY-4.0 | Ed Mackey, © 2023 Analytical Graphics, Inc. |
| TransmissionRoughnessTest | `engine/tests/render/assets/khronos/TransmissionRoughnessTest/` | CC-BY-4.0 | Ed Mackey, © 2021 Analytical Graphics, Inc. |
| AttenuationTest | `engine/tests/render/assets/khronos/AttenuationTest/` | CC-BY-4.0 | Ed Mackey, © 2021 Analytical Graphics, Inc. |
| SpecularTest | `engine/tests/render/assets/khronos/SpecularTest/` | CC-BY-4.0 | Ed Mackey, © 2021 Analytical Graphics, Inc. |
| EmissiveStrengthTest | `engine/tests/render/assets/khronos/EmissiveStrengthTest/` | CC-BY-4.0 | Ed Mackey, © 2022 AGI |
| SheenTestGrid, AnisotropyStrengthTest, IridescenceDielectricSpheres, IridescenceMetallicSpheres, TransmissionTest, IORTestGrid, MetalRoughSpheresNoTextures | `engine/tests/render/assets/khronos/` | CC0-1.0 | Eric Chadwick, Ed Mackey, Khronos, UX3D, Adobe, Krill Gavrilov (their `metadata.json`) |

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

## Built-in palettes of the editor (S43)

The editor's built-in palettes (`editor/resources/palettes/*.palette`, editor-only templates: a game
holds only the copies a project makes of them) are colour data, credited here and in each file's
`author` and `source` fields:

| Palette | Author | Source | Licence or terms |
|---|---|---|---|
| The 27 ramps `fire` … `night` (`src/core/color.js` `PAL`), and `pixelfx-effects`, which holds them all | pixelfx | [pixelfx](https://github.com/patch-gg/pixelfx) 0.3.0 (`a84159688af3`) | MIT, Copyright (c) 2026 Patch |
| The `hot`, `fire_cooling` and `smoke_thin` ramps of `pixelfx-effects` (`src/core/sim.js:13-16`) | pixelfx, adapted from Sprite Fusion's pixel art VFX | [pixelfx](https://github.com/patch-gg/pixelfx) 0.3.0 | MIT, Copyright (c) 2026 Patch and Copyright (c) 2026 Hugo Duprez |
| `game-boy` (Game Boy (DMG)) | Nintendo (hardware) | [Lospec: Original Gameboy](https://lospec.com/palette-list/original-gameboy) | hardware colours; free to use, credited |
| `pico-8` | Lexaloffle Games | [Lospec: PICO-8](https://lospec.com/palette-list/pico-8) | free to use, credited |
| `cga` | IBM (hardware) | [Lospec: Color Graphics Adapter](https://lospec.com/palette-list/color-graphics-adapter) | hardware colours; free to use, credited |
| `ega` | IBM (hardware) | [Lospec: Enhanced Graphics Adapter](https://lospec.com/palette-list/enhanced-graphics-adapter) | hardware colours; free to use, credited |
| `commodore-64` (Pepto) | Philip "Pepto" Timmermann | [Commodore VIC-II Color Analysis (2001)](https://www.pepto.de/projects/colorvic/2001/) | free to use, credited |
| `zx-spectrum` | Sinclair Research (hardware) | [Lospec: ZX Spectrum](https://lospec.com/palette-list/zx-spectrum) | hardware colours; free to use, credited |
| `sweetie-16` | GrafxKid | [Lospec: Sweetie 16](https://lospec.com/palette-list/sweetie-16) | free to use, credited |
| `endesga-32` | ENDESGA | [Lospec: Endesga 32](https://lospec.com/palette-list/endesga-32) | free to use, credited |
| `resurrect-64` | Kerrie Lake | [Lospec: Resurrect 64](https://lospec.com/palette-list/resurrect-64) | free to use, credited |
| `aap-64` | Adigun A. Polack | [Lospec: AAP-64](https://lospec.com/palette-list/aap-64) | free to use, credited |

The agent sprite drawing tools (`editor/pixelart/draw/`, docs/specs/S47-agent-sprite-drawing.md) take their design from
[tenten](https://github.com/debuffjp/tenten) by debuff (MIT) and port these algorithms, keeping the headers in the sources:
[Pixelorama](https://github.com/Orama-Interactive/Pixelorama) (MIT, Copyright (c) 2019-present Orama Interactive and contributors): the
pixel-perfect line (`src/Classes/Drawers.gd`), the scanline flood fill (`src/Classes/FloodFillObject.gd`, from Allegro 4.2.1's `flood.c` by
Shawn Hargreaves), Zingl's ellipse and rounded rectangles (`src/Autoload/DrawingAlgos.gd`), the outline neighbourhoods
(`src/Shaders/Effects/OutlineInline.gdshaderinc`), hue-shift shading (`src/Tools/DesignTools/Shading.gd`) and the rotxel and nearest
rotations; [cleanEdge](https://torcado.com/cleanEdge/) by torcado (MIT, Copyright (c) 2022 torcado) and
[OmniScale](https://github.com/libretro/glsl-shaders) by Lior Halphon (MIT, Copyright (c) 2015-2016 Lior Halphon, no-AA version by deakcor),
ported to the CPU as the rotations of `edit_sprite`.

Test data: the Endesga 32 files under `editor/tests/fixtures/palettes/` (`lospec-endesga-32.*`) and
the recorded Lospec answers under `tools/mcp/tests/fixtures/lospec-mirror/` are Lospec's own
downloads of the palettes above. The palette importers follow GIMP's `gimppalette-load.c` layout as
[Pixelorama](https://github.com/Orama-Interactive/Pixelorama) reads it (`src/Autoload/Palettes.gd`),
and the ramp generator's Hue Shift mode ports Pixelorama's `src/Tools/DesignTools/Shading.gd`
(MIT, Copyright (c) 2019-present Orama Interactive and contributors) with Godot's `Color` semantics
(MIT, Copyright (c) 2014-present Godot Engine contributors).

## Pixel-art colour code of the engine and editor (S43)

Ported, each file keeping the original notice next to ours:

| Code | Taken from | Licence |
|---|---|---|
| OKLab conversions (`engine/core/pixelart/PixelColor.cpp`, `engine/shaderlib/includes/palette.glsl`) | Björn Ottosson, [A perceptual color space for image processing](https://bottosson.github.io/posts/oklab/) (2020), his reference matrices | public domain / MIT |
| CIELAB D65, CIEDE2000 and the cached nearest-colour matcher (`engine/core/pixelart/PixelColor.cpp`); Apply Palette's lost-colour groups (`editor/pixelart/ApplyPalette.cpp`) | [ritmo](https://github.com/typeonce-dev/ritmo) `packages/tools/src/pixel/core/color.ts`, `lost.ts` | MIT, Copyright (c) 2026 Sandro Maglione |
| `fitRamp` and `rampAt` (`engine/core/pixelart/PixelColor.cpp`) | [pixelfx](https://github.com/patch-gg/pixelfx) `src/core/color.js` `fit` and `at` | MIT, Copyright (c) 2026 Patch |
| The ordered pair-mix dither (`PaletteMatcher::ditherPair`, Palette Quantize's table) | Joel Yliluoma, "Arbitrary-palette positional dithering algorithm" (the algorithm, no code) | — |

The CIEDE2000 test pairs (`engine/tests/core/pixelart/PixelColorTest.cpp`) are Sharma, Wu and Dalal's
published data (2005).

## Pixel particles (S49)

Ported or copied, each file keeping the original notices next to ours:

| Code or data | Taken from | Licence |
|---|---|---|
| The pixel shapes (Cross, Streak, Puff, Shard, Chunk, Glow: `engine/core/particles/ParticlePixel.cpp`, `engine/shaderlib/includes/particles_pixel.glsl`), the dithered fade, the presets' constants (`editor/pixelart/particles/*.particles`), and the closed-form flight and seamless cycle of Analytic particles (`engine/core/particles/AnalyticParticles.cpp`, `engine/shaderlib/includes/particles_analytic_vs.glsl`) | [pixelfx](https://github.com/patch-gg/pixelfx) 0.3.0 `src/core/sim.js`, `painter.js`, `particles.js`; `sim.js` adapts Sprite Fusion's [pixel art VFX](https://www.spritefusion.com/pixel-art-vfx) | MIT, Copyright (c) 2026 Patch and Copyright (c) 2026 Hugo Duprez |

## Ideas re-implemented, no code copied

[ozz-animation](https://github.com/guillaumeblanc/ozz-animation) (MIT, 0.17.0 `744eb9d`, (c) Guillaume Blanc): `editor/animation/ClipReduction.*` re-implements the idea of its `AnimationOptimizer` and `decimate.h` (a hierarchical error metric and Ramer-Douglas-Peucker decimation), with the changes of `docs/specs/S77-clip-quality.md` D5. No ozz source ships here.
