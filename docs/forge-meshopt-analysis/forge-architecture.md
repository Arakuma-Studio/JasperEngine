# The Forge as a reference for JasperEngine: framework architecture, and "replace or sit beside sokol-gfx?"

Scope: The Forge's framework layers against JasperEngine's sokol-gfx layer. Effects and meshlet/visibility-buffer rendering are only touched where they bear on the architecture question.

- **Clone studied:** `/tmp/forge-meshopt-research/forge/The-Forge`, shallow, commit `cd50468` (2026-08-27, "Update README.md"). Paths below are relative to it. `TF:` marks a Forge path where the context is ambiguous.
- **JasperEngine:** `/var/lib/orquester/workspaces/jaspersito/forge-meshopt` at `b4f7816ea`. `JE:` marks its paths.
- **Upstream today:** the Codeberg repository, read on 2026-11-04 through its public API and raw README.

**Evidence labels**
- **[V]** verified: I read the cited file and line, or the API response.
- **[I]** inferred: reasoning from what I read, or general platform knowledge.
- Nothing from the clone was built or run. No Forge behaviour here was observed at runtime, so a [V] means "the code says so", not "I watched it happen".
- My own scripts were kept outside the clone and only read files in it.

---

## 1. Verdict

**Do not replace sokol-gfx. Do not vendor The Forge as a sixth backend. Take a few ideas and one data table.**

1. **It has no path to three of the five required backends.** [V] It has no GL 3.3, no GLES3/WebGL2 and no D3D11. It has no HTML5 target at all.
2. **Those missing backends are the engine's defaults.** [V] They are the default on four of six export targets (`JE:engine/CMakeLists.txt:210-217`): Windows and Linux use `glcore`, Android and web use `gles3`.
3. **Forge's maintainers have ruled the same hardware out.** [V] They removed these APIs on purpose (`README.md:219`, `:263-264`).
4. **New finding, and it changes the sixth-backend sum.** [V] Upstream moved to Codeberg for release 1.64 (Aug 2026), and the public repository there is now PC-only.
   - `Common_3/Graphics` holds only `Direct3D12`, `FSL`, `GraphicsConfig.c`, `Interfaces`, `ShaderUtilities.h.fsl` and `ThirdParty`.
   - `Common_3/OS` holds only `Windows` plus shared folders.
   - The README there says: "We offer a PC licence under the Apache License Version 2.0 through this repository. We also offer commercial licenses for all other platforms like game consoles."
   - So **the Apache-licensed Vulkan, Metal and Android code in this clone is the end of the line.** Its grant is perpetual (`LICENSE:66-67`), so it stays usable. Future Adreno, Mali or iOS fixes would come only with a commercial licence, and I found no public terms for one.

The architecture-level gap is also one-sided: JasperEngine is already ahead of The Forge on async shader and pipeline creation (S32), on jobs (S06) and on test infrastructure (section 4).

### Top five to take

| # | What | Verdict | Effort [I] |
|---|---|---|---|
| 1 | GPU profile system: gpu.data-style tier table, gpu.cfg-style rules, per-tier scene resolution and settings | COPY the data, REWRITE the parser | ~400-600 lines plus editor, Lua and MCP surface |
| 2 | Thermal and memory-pressure signals, wired into dynamic resolution and frame-rate caps | ADOPT-IDEA (Forge only displays them; JE can act on them) | small |
| 3 | Real-device test farm (Forge's BrowserStack hooks), plus a benchmark driven by Android intent arguments | ADOPT-IDEA | CI spend, small code |
| 4 | Upload pacing: per-frame upload budget and sync tokens from Resource Loader | ADOPT-IDEA | medium |
| 5 | On-device shader hot reload: `adb reverse` plus a host compile server | ADOPT-IDEA | medium |

Runner-up: a `VK_EXT_device_fault` report on Vulkan device loss, which S28 explicitly left open.

---

## 2. Platform and backend matrix

### 2.1 What exists in this clone

| Target / API | The Forge at `cd50468` | JasperEngine | Evidence |
|---|---|---|---|
| OpenGL 3.3 core | **No** | Yes, default on Windows and Linux | [V] No matching directory anywhere; backend selection in `Common_3/Graphics/GraphicsConfig.h:50-75` has no GL branch |
| GLES3 | **No** (GLES2 existed and was dropped) | Yes, default on Android | [V] `README.md:263-264` |
| WebGL2 / HTML5 | **No** | Yes, default on web | [V] A word-bounded search for `emscripten`, `WEBGL` and `WEBGPU` finds nothing relevant in own code |
| D3D11 | **No**, removed | Yes | [V] `README.md:219`: "We removed DirectX 11 support". The leftovers are `FSL utils.py:34`, `generators/d3d.py:32,143` and `GraphicsConfig.h:171`; `compilers.py:32-44` has no D3D11 compiler entry |
| D3D12 | Yes, Windows only | No | [V] `GraphicsConfig.h:59`, `Direct3D12/Direct3D12.c` |
| Vulkan on Windows | Not officially supported | Optional | [V] `README.md:260`; `GraphicsConfig.h:52` gates it behind `FORGE_EXPLICIT_RENDERER_API` |
| Vulkan on Linux | Steam Deck first; stand-alone Linux removed | Optional | [V] `README.md:556`; `Application/Config.h:272` maps `__linux__` to `STEAMDECK` |
| Vulkan on Android | Yes, Vulkan 1.1, **arm64 only** | Not built; Android is GLES3 | [V] `GraphicsConfig.h:66-67`, `VulkanConfig.h:63`, `OS/Android/Packaging/app/build.gradle:72,85` |
| Metal | Yes | Yes | [V] `Metal/MetalRenderer.mm` |
| Consoles | Commercial only, not in the repo | No | [V] `README.md:17,29,270` |

Note on the search: a naive grep for `GLES` matches `TRIANGLES`, so the absence claim rests on the word-bounded search, not the naive one.

**Runtime API switching is gone too.** [V] `README.md:263`: "we removed the switching functionality". Forge is one API per platform at compile time, which is the same shape as JE's `GRAPHIC_BACKEND`.

### 2.2 Floors

| Floor | The Forge | JasperEngine | Effect if Forge replaced sokol |
|---|---|---|---|
| Android API | 26 in `Examples_3/Build_Props/VS/AGDEVersions.Android.props:5`; README says Pie (`README.md:6`); Codeberg README says Android 10 | 21 (`JE:editor/Project.h:744`) | Loses API 21-25 or more |
| Android ABIs | `arm64-v8a` only | arm64, armeabi-v7a, x86, x86_64 (`JE:engine/workspaces/androidstudio/app/build.gradle:51-54`) | Loses three ABIs, including the x86_64 ABI the emulator gate can build (`JE:tools/build.sh:66`) |
| Android target SDK | 31 (`build.gradle:10`) | 36 (`JE:editor/Exporter.cpp:4057`) | Packaging needs updating |
| iOS | 14.1 (`Metal/MetalAvailabilityMacros.h`) | 15.0 (`JE:engine/CMakeLists.txt:855`) | **No loss** |
| macOS | 11 (`README.md:10`) | 12.0 (`JE:engine/CMakeLists.txt:643`) | **No loss** |

Side note: `S06-jobs-memory.md` still quotes older Apple floors (macOS 10.15, iOS 12/13). The CMake values above are the current ones.

### 2.3 The count you asked for

**If The Forge replaced sokol:**

- **Backends lost: 3 of 5** (GL 3.3 core, GLES3/WebGL2, D3D11). Metal and Vulkan survive, and Vulkan would no longer be officially supported on Windows. D3D12 would be added as an unwanted new backend. [V]
- **Export targets lost outright: 1 of 6** (HTML5). [V]
- **Export targets degraded: 3 of 6.** [V]
  - Android: arm64 and Vulkan 1.1 only, so every GLES-only phone and all 32-bit and x86 builds go.
  - Windows: D3D12 only.
  - Linux: Steam Deck is the supported Vulkan target.
- **Targets that stay whole: macOS and iOS.** [V]
- **Current default backend removed on 4 of 6 targets.** [V]
- **Using only currently published source, 5 of 6 targets have no public code** (only Windows remains on Codeberg). [V] The frozen GitHub snapshot is the only Apache route for the others.
- **Test infrastructure breaks** [I from `JE:docs/testing.md`]: goldens are glcore on llvmpipe, the web gate is WebGL2 on SwiftShader, and the Android render gate is GLES. Each would need replacing.

### 2.4 Project state

- [V] `README.md:76`: "Release 1.64 / Continued on https://codeberg.org/The-Forge".
- [V] Codeberg repository created 2026-08-14, 12 commits, last updated 2026-08-31, `LICENSE.txt` is plain Apache-2.0 with no console wording.
- [V] Release 1.64 replaced Dear ImGui with Nuklear.
- [V] Release notes state cuts to fit a small team (`README.md:555`, "So we cut down functional or unit tests").
- [I] The shallow clone cannot show which release the GitHub code corresponds to. The last release section with content is 1.63, March 2025, so assume it is about that old or slightly newer.

---

## 3. Architecture and size

### 3.1 Own code, excluding `ThirdParty` [V, measured with `wc -l`]

| Layer | Lines |
|---|---|
| `Common_3/Graphics` | 35,055 |
| `Common_3/OS` | 24,173 |
| `Common_3/Application` | 19,777 |
| `Common_3/Tools` | 17,155 |
| `Common_3/Utilities` | 9,507 |
| `Common_3/Resources` | 7,625 |
| `Common_3/Game` | 2,515 |
| `Common_3/Renderer` | 2,164 |
| **Total** | **117,971** |

| Backend file | Lines |
|---|---|
| `Graphics/Vulkan/Vulkan.c` | 9,627 (plus `VulkanRaytracing.c` 493) |
| `Graphics/Direct3D12/Direct3D12.c` | 6,650 (plus `Direct3D12Raytracing.c` 526, hooks 1,269) |
| `Graphics/Metal/MetalRenderer.mm` | 6,004 (plus raytracing 570, allocator 621, caps 503) |
| `Graphics/Interfaces/IGraphics.h` | 2,961 |

For comparison [V]: `JE:engine/libs/sokol/sokol_gfx.h` is 27,045 lines holding GL, GLES, Metal, D3D11, Vulkan and WebGPU, with `sokol_async_impl.h` 968 and `sokol_gpu_timer_impl.h` 1,177 beside it. JE's backend layer `engine/renders/sokol/*.cpp` is 8,649 lines.

Repository weight [V]: 480 MB, of which about 442 MB is `ThirdParty` (Tools 270 MB, Graphics 131 MB, OS 27 MB). It ships prebuilt `dxc`, `glslangValidator`, an embedded Python 3.6, `7z.exe` and `wget.exe`.

### 3.2 Why it does not coexist with JE's build [V unless marked]

- **No CMake.** There are zero `CMakeLists.txt` files in the tree. The build is Visual Studio, Xcode and CodeLite projects plus AGDE for Android. JE builds through CMake inside Docker images.
- **Hostile macros.**
  - `Utilities/Interfaces/IMemory.h:144-166` redefines `malloc`, `free`, `new` and `delete` as `static_assert(false, ...)`.
  - `Graphics/FSL/fsl_srt.h:34-35` has `#define float4x4 mat4` and `#define uint uint32_t`.
  - [I] Any C++20 translation unit that includes these beside the STL needs careful ordering.
- **Global unprefixed C symbols.** `initRenderer`, `addShader`, `addPipeline`, `cmdDraw` and `queueSubmit` (`IGraphics.h:2824-2904`). JE's namespacing does not protect against them.
- **Duplicate vendored libraries.** Dear ImGui, Lua 5.3.5, stb, meshoptimizer, lz4, zstd, hidapi. JE vendors its own ImGui, stb, meshoptimizer and zstd, and Lua 5.4.7 (`JE:NOTICE.md`). [I] Linking both risks ODR clashes.
- **It wants the OS layer.** `OS/*/…Base` owns the main loop, the window and input. `GraphicsConfig.cpp` reads `gpu.data` through Forge's own filesystem resource directories. [I] Skipping `IApp` is possible but unsupported in practice.
- **Language standard.** C99 and C++11 (`README.md:1700-1702`). JE is C++20.
- **Non-permissive files under `ThirdParty/OpenSource`.**
  - `Graphics/ThirdParty/OpenSource/nvapi/nvapi.h:15` says "PROPRIETARY and CONFIDENTIAL to NVIDIA".
  - `Direct3d12Agility/LICENSE.txt` is Microsoft software licence terms.
  - `Utilities/ThirdParty/OpenSource/FluidStudios/MemoryManager/mmgr.c:19-23` has custom terms: credit required, modified redistribution must document where to get the original, and "may not use this software to directly or indirectly cause harm". [I] That is not an OSI licence.
  - Whole-tree vendoring would have to audit and strip these.

---

## 4. Subsystem verdicts

Key: **REPLACE**, **ADOPT-IDEA**, **COPY-CODE**, **SKIP**. Entanglement figures are the transitive closure of `#include "..."` measured by my script; numbers split own code from third party.

| Subsystem | Verdict |
|---|---|
| `IGraphics` and backends vs sokol-gfx | **SKIP** (no REPLACE) |
| Sixth backend | **SKIP** |
| Resource Loader | **ADOPT-IDEA** |
| FSL translator | **SKIP** |
| Pipeline cache and PSO | **SKIP**, one micro-idea |
| Memory allocators | **SKIP** |
| Threading | **SKIP** |
| ECS (flecs) and Ozz | **SKIP** |
| GPU config system | **COPY data, ADOPT-IDEA for code** |
| Swappy | **SKIP** (JE is already there) |
| Thermal and memory state | **ADOPT-IDEA** |
| Variable-rate shading | **SKIP** |
| AssetPipeline | **SKIP** |
| BunyArchive | **SKIP** |
| Shader ReloadServer | **ADOPT-IDEA** |
| Code hot reload (cr) | **SKIP** |
| Test and screenshot automation | **ADOPT-IDEA** (device farm only) |
| GPU crash diagnostics | **ADOPT-IDEA** (runner-up) |

### 4.1 IGraphics and backends: SKIP

**Shape** [V, `IGraphics.h:2824-2904`]. Vulkan/D3D12-style explicit API: `initRenderer`, `initQueue`, `addSwapChain`, `addPipeline`, `addDescriptorSet`, `cmdBindDescriptorSet`, `cmdResourceBarrier`, `acquireNextImage`, `queueSubmit`, `queuePresent`. The caller owns barriers, queues, semaphores and fences.

sokol-gfx's model is immediate-style (`sg_apply_pipeline`, `sg_apply_bindings`, passes). Those are not close, and JE's whole render layer is written against the second.

**The one real capability gap** [V]. sokol's Vulkan backend needs Vulkan 1.3 and `VK_EXT_descriptor_buffer` (`JE:engine/platform/common/VulkanContext.cpp:199`, `JE:engine/CMakeLists.txt:315`), so JE has no Android Vulkan. Forge's runs on Vulkan 1.1. That is the strongest argument for Forge. It does not carry the decision, because JE's Android path is GLES3 by design, GLES3 reaches devices Vulkan 1.1 cannot, and no JE feature requires Vulkan.

### 4.2 "Vendor The Forge as a sixth backend": not viable

A straight answer, with the costs itemised. It is technically possible, but it costs more than it returns:

1. **No backend seam exists.** [V] `JE:engine/core/render/ObjectRender.h:20` holds `SokolObject backend;` and `ShaderRender.h:24` holds `SokolShader backend;` by value. 52 files touch `sokol_gfx.h` or `sg_*` calls. A sixth backend first means introducing a compile-time seam through the render layer.
2. **Different binding model.** [V] Forge expects root signatures and descriptor sets (`IGraphics.h:1953-1969`, `IResourceLoader.h:473-481`), and shader binaries compiled with FSL's conventions (`BinaryShaderDesc`, `IGraphics.h:1742-1775`). [I] JE's SPIR-V-to-sokol reflection (`JE:engine/libs/shadercompiler/src/spirvcross.cc`) targets a different slot model, so JE's cross-compile path would need a second output or FSL would need to be adopted.
3. **It would duplicate S32, S15 and S14 work.** [V] JE has already patched into sokol: async creation (`sokol_async_impl.h`), GPU timers (`sokol_gpu_timer_impl.h`), ASTC and 16-bit formats, and a pool lock (`JE:engine/libs/sokol/PATCHES.md`). [I] Each would need a Forge equivalent.
4. **It covers no target JE lacks, except consoles, and those need a licence anyway.** [V] Windows gets D3D12 instead of D3D11, which is not a requirement. [I] Android Vulkan is the only possible gain.
5. **Frozen code.** [V] The non-PC backends are no longer published, so JE would maintain 16,000+ lines of Vulkan and Metal alone or pay for updates.
6. **Violates a stated value.** [V] "Every runtime feature works on all five graphics backends" (`JE:CLAUDE.md`). A sixth backend would permanently grow the test matrix, and every feature would have to work on it too.

### 4.3 Consoles

- [V] The GitHub README says console platforms "are only available for accredited developers on request" and "you need a license from us" (`README.md:17`, `:29`), with commercial licences from release 1.59 (`README.md:270`). The Apache `LICENSE` file itself has no console clause (`LICENSE`, grep for console, PlayStation, Xbox, Nintendo: no hits).
- [V] Console code is not in the clone. References such as `../../../PS4/Common_3/...` (`Graphics/FSL/fsl_srt.h`) point at directories that do not exist here; 9 such references in `Graphics` and `Utilities`.
- [V] sokol has no console backend and JE has no console target.
- **Realistic path?** [I] Not today. It needs platform-holder accreditation for each console, a Forge commercial licence on undisclosed terms, the backend seam from 4.2, and an FSL or equivalent shader path. That is a funded project with a signed customer behind it, not a roadmap item. If it ever happens, buy it then, because nothing here needs doing in advance. Do not design for it now.

### 4.4 Resource Loader: ADOPT-IDEA

**What it is** [V, `Resources/ResourceLoader/ResourceLoader.cpp`, 4,383 lines; API in `Interfaces/IResourceLoader.h`].

- A streamer thread (`streamerThreadFunc`, `:1991`) with a single-threaded mode (`:2007`, `:2126`).
- Copy engines on a transfer queue, with an upload engine on the graphics queue (`:2212-2221`).
- Default staging of 8 MB × 2 sets (`:465`).
- UMA fast paths that write directly into mapped buffers (`:1529`, `:1582`).
- Monotonic `SyncToken`s with `isTokenCompleted`, `waitForToken` and the "submitted vs completed" distinction (`IResourceLoader.h`).
- Large resources get split across flushes instead of failing ("will perform multiple copies/flushes").
- **Not provided:** streaming. "Resource Streaming (not provided)" (`README.md:37`).

**Entanglement.** Direct includes 19. Closure 100 files: 22 own files (11,998 lines), 78 third-party files (147,525 lines). Bound to `Cmd`, `Queue`, barriers and `Renderer`. **Cannot be copied.**

**The fit for JE** [V/I]. Transfer queues and UMA mapping are not reachable through sokol-gfx, and not on GL at all.

The portable part is the discipline:
- [V] `JE:engine/renders/sokol/SokolCmdQueue.cpp:80,125` runs `MAKE_IMAGE` and `UPDATE_IMAGE` with no byte budget in that file. S32's `mainThreadBudgetMs` covers shaders only.
- [I] So a burst of large cooked textures can still cost a frame on GLES, which is the mobile backend.
- **Take:** a per-frame upload byte budget in `SokolCmdQueue` (the same shape as S32 R5), and a monotonic readiness token that `areScenesLoaded` and screenshots can wait on.
- **Cost when off:** zero. **Backends:** all, since it sits above sokol.
- **Caveat** [I]: sokol uploads an image's initial data in one call, so a single huge texture cannot be split without creating it empty and updating per-mip. A real benchmark on a mid-range phone should come first.

### 4.5 FSL shader translator: SKIP

**What it is** [V]. A Python textual translator: 3,408 lines across `fsl.py`, `compilers.py`, `utils.py` and `generators/{d3d,vulkan,metal}.py`, plus about 120 KB of macro includes. HLSL-like source with `STRUCT`, `DATA`, `ROOT_SIGNATURE`, `VS_MAIN` macros (`Examples_3/Unit_Tests/src/01_Transformations/Shaders/FSL/basic.vert.fsl`), and `.list` files naming stages and feature variants (`shaders.list`).

**Why it cannot replace JE's pipeline:**

| | FSL | JE (`engine/libs/shadercompiler`, `engine/core/shader`) |
|---|---|---|
| Source language | HLSL-like superset | GLSL |
| Outputs | Vulkan, D3D12, Metal, console languages | GLSL 300 es, desktop GLSL, HLSL 5, MSL 2.1 (iOS, macOS), SPIR-V (`JE:engine/shaders/*.h`) |
| GL / GLES / WebGL / D3D11 | **None** (`compilers.py:32-44`) | All |
| Compilers | External binaries: `dxc`, `glslangValidator`, Apple `metal` via `xcrun` (`compilers.py:32-44,62-63`) | In-process glslang and SPIRV-Cross |
| When it runs | Offline at build | Editor job and at runtime (`RuntimeShaderCompiler`) |
| Metal from a Linux host | [I] No, requires Apple's compiler | Yes, ships MSL source |

JE's model of runtime compile and MSL source is what lets it export without a Mac for the shader step. FSL gives that up.

**The one interesting idea** is the Shader Resource Table: a header shared by C++ and shaders that declares bindings by update frequency (`Graphics/FSL/fsl_srt.h`, `vulkan_srt.h`). [I] JE already derives this from reflection, so no action.

### 4.6 Pipeline cache and PSO vs S32: SKIP

JE is well ahead. Comparison against `JE:docs/specs/S32-async-shaders.md` and `JE:engine/renders/sokol/SokolAsyncCreate.h`:

| Topic | The Forge | JasperEngine S32 |
|---|---|---|
| Creation thread | Caller's thread, synchronous. `vkCreateGraphicsPipelines` at `Vulkan.c:7408`; `newRenderPipelineStateWithDescriptor` at `MetalRenderer.mm:3546`. Samples build everything in `addPipelines()` at load (`Visibility_Buffer.cpp:2376`) | Worker thread or completion handlers, with a per-frame budget (R5) |
| Stand-ins | None found | R8 |
| Warm-up list | None found | Recorded in Play, exported, replayed (R10-R12) |
| Interning | None found | R3, `SokolPipelineCache::acquire` |
| Vulkan disk cache | App-named raw file (`ResourceLoader.cpp:4214-4265`, `Visibility_Buffer.cpp:515`), validated only by the driver | Per-device path, header with magic, vendor, device, UUID, checksum, atomic write (R13) |
| D3D12 | `ID3D12PipelineLibrary` (`Direct3D12.c:5003-5016`) | Not applicable |
| Metal | `addPipelineCache` is an empty stub (`MetalRenderer.mm:3655-3659`); no binary archives | Completion-handler APIs (R16) |
| GL | None | Worker context, `KHR_parallel_shader_compile`, deferred (R14) |

"None found" means my searches of the backends and samples turned up nothing, not that I read every line.

**Micro-idea:** `PIPELINE_CACHE_FLAG_EXTERNALLY_SYNCHRONIZED` (`IGraphics.h:2123`, `Vulkan.c:2720-2722`). [I] S32 creates pipelines on one worker thread, so that thread could skip the driver's cache lock. Only worth doing if a profile shows it; R22's main-thread budget does not depend on it.

### 4.7 Memory allocators vs S06: SKIP

- **CPU** [V]: `tf_malloc` and friends route through `mmgr` when `ENABLE_MEMORY_TRACKING` is on (`MemoryTracking/MemoryTracking.c`, set for `!NDEBUG` at `Application/Config.h`), and are thin wrappers otherwise. Global hooks are explicitly out of S06's scope, and `mmgr`'s licence is a problem (section 3.2). JE already has `engine/core/memory/{Arena,ScratchScope,ObjectPool,RangeAllocator,MemoryStats}`.
- **GPU** [V]: Vulkan Memory Allocator (MIT) and D3D12MA, with VMA also adapted for Metal heaps (`Graphics/Metal/MetalMemoryAllocatorImpl.h`, 621 lines, included into VMA's header at `:620`).
- **JE today** [V]: sokol's Vulkan backend does one `vkAllocateMemory` per resource (`sokol_gfx.h:19521-19614`).
- [I] VMA would be the textbook fix, but JE's Vulkan is desktop-only (1.3), where allocation limits matter less. If it ever matters, vendor VMA from upstream, not from Forge. Not worth a sokol patch today.

### 4.8 Threading vs S06: SKIP

[V] `Utilities/Threading/ThreadSystem.c` (402 lines) is one mutex-protected FIFO with two condition variables (`:43-60`, with the comment "TODO optimize queued task IO" at `:55`). No stealing, priorities or lanes. It does have a useful "dummy mode" with `threadCount = 0` for single-threaded operation (`ThreadSystem.h`).

JE's `engine/core/jobs` has work stealing, priorities, a background gate and nested-safe `parallelFor` (`JE:docs/specs/S06-jobs-memory.md`), including a single-thread web mode. JE is strictly ahead. Closure: 15 files, for a smaller and worse scheduler.

### 4.9 ECS and animation: SKIP

- **flecs** [V]: included in a single place in own code: `Examples_3/Unit_Tests/src/19_CodeHotReload/19_CodeHotReload_Main.cpp:26`. README's "Fast Entity Component System" (`README.md:50`) is a sample dependency, not an engine integration. S07 is committed to JE's own ECS.
- **Ozz** [V]: wrapped by `Resources/AnimationSystem/Animation/*` (1,454 lines of `.cpp` wrapper), with the skeleton batcher tied to Forge's renderer. S21 specifies JE's own animation stack. Ozz itself is MIT if ever wanted, from upstream.

### 4.10 GPU config system: COPY the data, REWRITE the code. This is the best thing in the tree for JE.

**What it is** [V, `Graphics/GraphicsConfig.cpp`, 2,380 lines].

Two text formats:

- **`gpu.data`**, per platform, with vendor list, default preset and a `vendor; device; preset; #name` table (`OS/Android/android_gpu.data:8-26`).

| File | Entries [V, counted] |
|---|---|
| `OS/Windows/pc_gpu.data` | 677 |
| `OS/Darwin/apple_gpu.data` | 314 |
| `OS/Android/android_gpu.data` | 103 |
| `OS/Linux/steamdeck_gpu.data` | 1 |

- **`gpu.cfg`**, per application, with sections parsed at `GraphicsConfig.cpp:1049-1252`:
  - `BEGIN_GPU_SELECTION`: rules for choosing among several GPUs.
  - `BEGIN_DRIVER_REJECTION`: `vendor; DriverVersion <= x; reason`, evaluated at `:2048-2106`.
  - `BEGIN_GPU_SETTINGS`: `property; condition, condition; value`, to override capabilities.
  - `BEGIN_RESOLUTION_SETTINGS`: `SceneResolutions; Native; 540p; ...` and `sceneres; target == android, GpuPresetLevel <= high; 1;`, used by `getGPUCfgSceneResolution` (`:2332`).
  - `BEGIN_USER_SETTINGS`: named app settings by rule, e.g. `DisableGodRays; GpuPresetLevel <= 3; 1;`.
  - `BEGIN_TEXTURE_FORMAT`: per-GPU format capability overrides (`:1101`, `:1549`).

The property table is data-driven through a macro with read and write lambdas (`:110`, table `:166-267`), which keeps rules declarative.

**Why JE wants it** [V]. JE has no device-to-quality mapping:
- `grep` for Adreno, Mali and PowerVR in `engine/core`, `engine/platform` and `engine/renders` finds only one comment, in `SokolAsyncCreate.cpp:269`.
- `AtmosphereQuality` is per platform group (`JE:engine/core/render/AtmosphereQuality.h:12-14`), not per device.
- S32 already hard-codes per-driver decisions that could be rules: warm draws off on GLES until an Adreno or Mali run, and shared-context risk on old Mali and PowerVR (`S32-async-shaders.md:627`, `:721`, `:882`).
- A device-specific budget is already part of the specs (`S13-post-fx.md:238`, Mali-G57 / Adreno 610 class).

**Reality check on the data.** It is less of a treasure than advertised:

1. **The workaround database is thin** [V]. Across all 18 shipped `gpu.cfg` files, the driver rules reduce to about ten: Intel dynamic rendering off, Mali tessellation off, Xclipse 920 transfer queue, a Mali-G78-class device-memory-report crash (`DeviceID == 0x92020010`), Mali-G76 wave ops, Steam Deck `gl_PrimitiveID`, Adreno storage buffer size, 4096 rigs causing device loss on low-end Android. Almost all are Vulkan or compute. **Few apply to JE's GLES3.** Mali tessellation does not apply either, since sokol-gfx exposes no tessellation stages [I].
2. **Android presets are calibrated for Forge's visibility buffer** [V/I]. `android_gpu.data:132,129,104` rates Adreno 640 and 630 `verylow` and 740 `medium`, with no `high` entry for any phone GPU. Treat it as relative ordering, not as JE quality levels.
3. **It is stale** [V]. Newest Adreno is 750 (`:107`), newest Mali is G720 (`:94`). No Adreno 8xx.
4. **Android IDs are Vulkan IDs** [V]. The file cites `vulkan.gpuinfo.org` (`:6`). JE's Android has no Vulkan, so **vendor and device IDs are not available to it.**

**The fix for point 4** [V/I]. Key on the name. The human name is in the row (`#Qualcomm; Adreno (TM) 610`, and `Apple; Apple A14 GPU` in `apple_gpu.data`), and Forge itself does a name lookup on Apple because Metal exposes no IDs (`MetalRenderer.mm:1661-1666`). JE already has `sgj_device_name()` on every backend (`JE:engine/libs/sokol/sokol_gpu_timer_impl.h:1059-1085`: `GL_RENDERER`, Vulkan device name, DXGI description, `MTLDevice.name`).

[I] `GL_RENDERER` on Adreno and Mali reads like those names. On the web, `GL_RENDERER` is masked: JE needs `WEBGL_debug_renderer_info` (absent today, [V] by grep), and Safari reports a generic "Apple GPU". So the web needs a fallback tier, or a short GPU benchmark at startup, and the design must not assume a name is always available.

**Do not copy these** [V]:
- **Fallback bug.** `getGPUPresetLevel` applies the default preset inside `#if defined(ENABLE_GRAPHICS_RUNTIME_CHECK)` (`GraphicsConfig.cpp:2129-2136`), which exists only in debug (`GraphicsConfig.h:125`, undefined for profile at `:148`). An unlisted GPU in a release build reads as `GPU_PRESET_NONE`, which `Vulkan.c:4347`, `Direct3D12.c:2700` and `MetalRenderer.mm:2417` treat as unsupported and return a NULL renderer, with the misleading "Office Preset" message. I read this path but did not run it, and a `gpupresetlevel` rule in `gpu.cfg` could mask it. Either way, JE must never refuse to start on an unknown GPU, because new phones ship constantly. **Unknown means a default tier, always.**
- Fixed-size global rule arrays (`:98-100`; `:490-498`, with a comment about 72,776 bytes).
- Rejecting drivers. JE supports old devices; warn and pick a safe rule instead.

**Entanglement of `GraphicsConfig.cpp`.** C++11 with lambdas and `decltype`, so it would compile as C++20.
- 8 direct includes, closure 92 files: 18 own (8,668 lines), 74 third party (140,023 lines), through `IGraphics.h`.
- 158 lines reference `GpuDesc`, `pGpu` or `pRenderer`; 59 `LOGF`; 19 `tf_*` allocation calls; also `stb_ds`, `bstrlib`, `tinyimageformat`, `IFileSystem`.
- Parser and rule logic are about half the file; the rest is enum and property glue. **Too entangled to copy.** Upstream has since rewritten it as `GraphicsConfig.c` (Codeberg, [V]).
- Rewriting is short work, and a clean rewrite from the format description carries no copyright baggage.

**Suggested JE shape** [I].
- `engine/core/render/GpuProfile`: data file(s) with `name-pattern; tier` rows, loaded once at startup, one string match, no per-frame cost when no rules exist.
- Project-level rules in the project's settings: tier to `renderScale`, shadow tier, `AtmosphereQuality`, MSAA, warm-draw flag.
- `JE:CLAUDE.md` also requires: editor UI (Project Settings > Quality, with a preview of "which tier this device gets"), Lua (`Device.gpuTier`), MCP (`get_gpu_profile`, and a way to force a tier so agents can test), undoable edits, defaults so old projects load unchanged.

### 4.11 Swappy: SKIP (JE already does more on its backend)

- [V] Forge integrates SwappyVk on Android Vulkan only: `GFX_ENABLE_SWAPPY` (`VulkanConfig.h:119-120`), device extensions (`Vulkan.c:3753-3775`), init and interval (`:5211-5226`), present (`:8979-8983`).
- [V] JE integrates SwappyGL on GLES: init (`JE:engine/platform/android/NativeEngine.cpp:664-667`), a fallback for devices Swappy refuses (`:1491-1507`), `targetFrameRate` (`JasperEngineAndroid.cpp:667-673`), via the `androidx.games:games-frame-pacing:2.0.0` AAR (`JE:engine/workspaces/androidstudio/app/build.gradle:135`).
- Nothing to take. The Forge's prebuilt `libswappy_static.a` is arm64 only, and JE pulls four ABIs from Maven.

**Side finding for JE** [V]: `NOTICE.md` has no row for `games-frame-pacing` or `androidx` (grep: no hits). The AAR is linked into exported Android games. I did not verify how the repository's notice gate treats Maven dependencies, so this needs a look by someone who owns `NOTICE.md`.

### 4.12 Thermal and memory state: ADOPT-IDEA

**What Forge has** [V].
- Android thermal: `PowerManager.addThermalStatusListener`, API 29+ (`OS/Android/Packaging/app/src/main/java/ForgeBaseActivity.java:102-115`), reported through JNI (`OS/Android/AndroidBase.cpp:120-126`).
- iOS thermal: `NSProcessInfoThermalState` (`OS/Darwin/iOSBase.mm:415-442`), including the note that `thermalState` must be read once to receive notifications (`:534-537`).
- API: `getThermalStatus()` (`OS/Interfaces/IOperatingSystem.h:284-301,361`).
- Android memory: Google's Memory Advice library from AGDK (`AndroidBase.cpp:165-192`, `IOperatingSystem.h:429-455`), which includes a `.tflite` model in its assets.

**What it does with them** [V]. **Nothing except display them.** The only consumers are `AndroidBase.cpp` and `iOSBase.mm` (which set and log) and the profiler overlay (`Application/Profiler/ProfilerBase.cpp:1426-1460`). "Thermal handling" in The Forge is telemetry.

**What JE has** [V]. No thermal signal at all: the only `thermal` hits are terrain erosion, an HTML UI effect comment and a note in S06 (`JE:docs/specs/S06-jobs-memory.md:407`). No Memory Advice, and no `onTrimMemory` or `didReceiveMemoryWarning` hits under `engine/platform` or `engine/core`. It does have `DynamicResolutionController` with a target and `maxScale`, plus `Engine.targetFrameRate`.

**Take and go further** [I].
- `Device.thermalState` and `memoryState`, in Lua and MCP, plus a project option: at `SEVERE`, lower DRS `maxScale` and cap the frame rate, with hysteresis; at memory `CRITICAL`, drop caches and mips.
- Use the listener approach. The code is about 15 lines; rewrite it rather than copy. Take Memory Advice from Maven if at all (Apache-2.0, `agdk/LICENSE`), and weigh its model asset against S34's player-size goals. `onTrimMemory` is the zero-size alternative.
- Desktop and web return `NOT_SUPPORTED` and cost nothing.
- **Verification** [I]: Android thermal cannot be exercised on the emulator gate without `adb shell cmd thermalservice override-status`, which should work for a test, but check it. Mark device behaviour unverified until a phone run.

### 4.13 Variable-rate shading: SKIP

[V] Software VRS only: 4×MSAA targets with a stencil-based rate map, and no hardware VRS API in `IGraphics.h` (the only mention at `:2341` is a shader target comment; `Vulkan.c` has one match). It "Relies on programmable sample locations support, not widely supported on Android devices" (`README.md:526`, `:619`), with Android listed as Galaxy S23 and up, and on Apple "doesn't give expected performance benefits" (`README.md:624`). It was retired as a sample and folded into `15a_VisibilityBufferOIT` (`README.md:549`).

[I] sokol has no sample-location API, and GL 3.3, GLES3, WebGL2 and D3D11 at this level have no portable equivalent, so it fails the no-fallback rule. JE's S13 dynamic resolution is the portable answer. No JE spec mentions VRS ([V] by grep).

### 4.14 Tools

**Requested names that do not exist here** [V]: `BunnyLOD` and `SpirvTools` have **zero** file or text matches in this tree. The nearest name is `BunyArchive`, unrelated to LOD. `Tools/ThirdParty/OpenSource` holds only `ISPCTextureCompressor` and `meshoptimizer`. If you remember them from older Forge versions, they are gone.

| Tool | Lines | Verdict | Why |
|---|---|---|---|
| `AssetPipeline` | 2,998 + 1,536 + 248 | **SKIP** | glTF to Forge's binary formats, ozz cooking, meshoptimizer flags, ISPC textures. JE's S14 is ahead (basisu, astcenc, bc7enc_rdo, deterministic cook cache). Closure 126 files. Forge's own notes say Basis was removed for lack of a use (`README.md:550`), which has no bearing on JE's choice |
| `BunyArchive` | 2,344 + 941 | **SKIP** | LZ4 or zstd block archive with a hash table and parallel writer (`Buny.h`). JE has `engine/core/io/ResourcePack.*` and S30's `.jpak`. Tied to `IToolFileSystem`, closure 22 files. A worthwhile read only if pack format work reopens |
| `ForgeMaterialCompiler` | 340 | **SKIP** | Forge's material format |
| `Network` | 407 | **SKIP** | Small socket wrapper; closure 11 files. Not worth a dependency |
| `TinyImageFormat` | (third party, 4,209-line query header) | **SKIP** | MIT, clean. JE has `sg_pixel_format` and its own tables; a format library adds nothing |

### 4.15 Hot reload

**Shader ReloadServer: ADOPT-IDEA** [V, `Tools/ReloadServer/ReloadClient.cpp:25-48` describes the flow; `ReloadServer.py`].

1. A host daemon caches the compile arguments.
2. The app on the device connects to it (port 6543).
3. The host recompiles and returns the changed binaries.
4. On Android it runs `adb reverse tcp:PORT tcp:PORT` (`ReloadServer.py:430-433`, `ReloadClient.cpp:336`), so **no network setup is needed on a phone.**
5. On error, the host sends the message and the app keeps running.

This is a real gap for JE [V]: S05 sends mobile and web to the Null watcher backend and defers players ("A dev-player flag may come later with S30", `S05-file-watcher-hot-reload.md:35-36`). Iterating on a shader on a real Adreno today means re-exporting.

[I] JE's version can be simpler than Forge's, because JE has a runtime compiler: send GLSL or the built `ShaderData` from the editor over one socket, and let `ShaderPool` swap it through the generation counters S05 already uses. **Development builds only, nothing in shipped players.**

Code is not worth copying: `ReloadClient.cpp` pulls in UI and scripting, with a closure of 55 files. `adb reverse` is the idea to keep.

**Code hot reload (19_CodeHotReload, `cr.h`): SKIP** [V]. Desktop only (`IGamePlugin.h`: `FORGE_CODE_HOT_RELOAD 0` on Android, iOS and consoles). A plugin DLL plus a callback struct to work around static linking, and its own comments call this an example limitation. S23 already covers Lua hot reload in Play and C++ script reload. `cr` is MIT if a crash-protection reference is wanted.

### 4.16 Test and screenshot automation: ADOPT one idea

**What Forge has** [V]:
- A compile-time `AUTOMATED_TESTING` mode: exit after 240 frames at a fixed 0.0167 s step (`Application/Config.h:353-358`, `OS/Windows/WindowsBase.cpp:548-551`).
- Lua test scripts, run first and last (`WindowsBase.cpp:189-196`, `:312-319`; `Common_3/Scripts/Test_*.lua`), that cycle options and request screenshots.
- Screenshots via a compute copy (`Application/Screenshot/Screenshot.cpp`, closure 104 files).
- `BROWSERSTACK_TESTING` for real-phone runs (`AndroidBase.cpp:250,269`, and `README.md:214`).
- Android benchmark arguments read from the launch intent (`getBenchmarkArguments`, `AndroidBase.cpp`).
- **No comparison tool in the repo:** searching scripts for PSNR, SSIM or image diff finds nothing, so that part lives outside it.

**What JE has** [V, `JE:docs/testing.md`]: golden images with a ledger, a web gate, an Android emulator render gate, smoke tests, MCP-driven tests, a declared-run gate. JE is far ahead on everything except one point.

**The gap** [V]. Real GPUs. Unverified items sit in the specs for exactly this reason:
- `S13-post-fx.md:463`: "no Mali-G57 / Adreno 610 class phone is available".
- `S32-async-shaders.md:884`: Android EGL on Adreno pending.

**Take** [I]: a BrowserStack-class device farm job that installs an APK, drives the existing render gate, and returns screenshots and S15 GPU timings. A small matrix (one Adreno 6xx, one Mali-G5x/G7x, one PowerVR) would close real open items and would also feed the tiers in 4.10. Combine with the intent-argument trick so a run needs no custom build. It should run on landing or nightly, not per commit, because of cost.

### 4.17 GPU crash diagnostics: ADOPT-IDEA (runner-up)

[V] Forge reports `VK_EXT_device_fault` on device loss (`Vulkan.c:769-830`), `VK_EXT_device_memory_report` in debug (`:683-740`), and has DRED on D3D12 (`IGraphics.h:2715`) and `cmdWriteMarker` breadcrumbs (`IGraphics.h:2949`).

[V] S28 lists "GPU device-loss handling (separate future work)" (`S28-robustness.md:30`).

[I] `device_fault` is about 60 lines of plain Vulkan and would land in JE's crash report folders; write it fresh. It is low priority only because JE's Vulkan is desktop-only and mobile is GLES, where nothing equivalent exists.

---

## 5. Comparison of the sokol layer

**JE's local sokol changes** [V, `engine/libs/sokol/PATCHES.md`; `sokol_gfx.h` has 50 lines marked `JasperEngine:`]:
- ASTC 5×5 to 12×12, RGB565, RGBA4 (S14).
- Async-creation hooks (S32): adopting a GL program, a `VkPipelineCache`, Metal completion hooks.
- A resource-pool lock.
- GPU timer hooks (S15), the lavapipe descriptor size, GL texture max level, the GL depth-stencil invalidate fix.
- Engine-side backend code in `sokol_async_impl.h` and `sokol_gpu_timer_impl.h`.

**What this means.** JE has already extended sokol through a small, well-documented patch surface to get most of what would otherwise tempt it towards Forge: async creation, GPU timers, device names. Each new Forge-inspired idea in section 1 sits **above** sokol and needs no new sokol patches, apart from a web query for `WEBGL_debug_renderer_info` (which is platform code).

---

## 6. Licences

### 6.1 Apache-2.0 obligations if anything is copied [V, `LICENSE`]

- §2 (`:66-67`): perpetual, irrevocable copyright grant. It survives upstream's move for the code at this commit.
- §3 (`:73-82`): patent grant, which terminates if you sue claiming the work infringes.
- §4 (`:89+`):
  - Give recipients a copy of the licence.
  - Modified files must carry a prominent notice that you changed them.
  - Keep copyright, patent, trademark and attribution notices in source.
  - Include any NOTICE file. **The Forge has no root NOTICE** ([V] `find` hits only `D3D12MemoryAllocator/NOTICES.txt`), so this clause has nothing to carry.
- §6 (`:138`): no trademark grant. "The Forge Framework™" appears only as attribution; do not name features after it.
- Compatible with JE's MIT engine: JE already ships Apache-2.0 code (SheenBidi, astc-encoder, SPIRV-Cross; `JE:NOTICE.md:28,35,142`).
- **In JE:** a `NOTICE.md` row, the licence text beside the file, and the header style used in `JE:engine/core/render/DynamicResolutionController.h:3-4` ("Portions ported from ...").
- `.data` files carry no header ([V] `android_gpu.data:1`), so the `NOTICE.md` row and a modified-file note are what carry attribution.
- Ideas and formats reimplemented from a description carry no copying obligation [I, not legal advice]; a courtesy credit is still sensible.

### 6.2 Third-party licences for what is recommended

| Item | Licence | Evidence | Recommended source |
|---|---|---|---|
| `gpu.data` tables | Apache-2.0 as part of The Forge. Rows draw on third-party sources (`technical.city`, Intel `gpudetect`, `vulkan.gpuinfo.org`, `android_gpu.data:4-6`). [I] IDs and names are facts, but the `technical.city` ranking licence is unchecked | [V] for the citation | Copy with attribution; re-rate for JE anyway, which reduces reliance on rankings |
| AGDK Memory Advice / Swappy | Apache-2.0 | [V] `OS/ThirdParty/OpenSource/agdk/LICENSE`, `swappyVk.h:2-3` | Maven AAR, not Forge's arm64-only static libs |
| VMA (only if ever needed) | MIT | [V] `VulkanMemoryAllocator/LICENSE.txt` | Upstream |
| `cr.h`, tinyimageformat, tinyktx, flecs, ozz, lz4, zstd | MIT or BSD-type | [V] for `cr/LICENSE`, `tinyimageformat_base.h:40`, `tinyktx.h:1,2452`. [I] for the others, from file presence | Not recommended, listed for completeness |

**Do not import:** `mmgr` (custom terms), `nvapi` (proprietary header), `Direct3d12Agility` (Microsoft terms). `bstrlib` is BSD/GPL dual (`bstrlib.h:2-3`); it is fine under BSD but has no use in JE.

**Do-not-copy list.** The list in `JE:docs/specs/README.md` names OpenEngine's items, not The Forge's. [I] Its reasoning (GPU-driven culling and so on) also rules out Forge's visibility buffer, TVB 2.0 and compute particles as primary paths. That is an engineering limit, not a licence issue.

---

## 7. What is uncertain

- **Commercial terms for non-PC platforms** are not public. I could only read the README sentences. Whether a licence is cheap would change the "frozen code" cost but not the backend gap.
- **The snapshot's exact release** cannot be established from a shallow clone.
- **Nothing was run.** The preset fallback bug in 4.10 comes from reading the code, including the `#if` scope and the callers. Rule ordering could mask it in practice.
- **`GL_RENDERER` strings on real Adreno and Mali drivers** matching the table's names is from general knowledge. It needs a real-device log. JE's startup log (`sgj_device_name`, f8d2aa88c) will show this cheaply on the first farm run.
- **"None found" in 4.6** means searches, not a full read.
- **Effort figures** are estimates.
- **Codeberg state** is as read on 2026-11-04 and may change.

## 8. Files

- This report: `/tmp/forge-meshopt-research/reports/forge-architecture.md`
- Include-closure script: `/tmp/forge-meshopt-research/scripts/incl.py`
- Copy of the Codeberg README and API responses: `/tmp/forge-meshopt-research/codeberg-readme/`

No JasperEngine files were changed.

## Appendix: include closures [V, measured by `incl.py`]

Unresolved includes (console and absent paths) were skipped. Own means outside `ThirdParty`.

| File | Self | Direct | Closure | Own (lines) | Third party (lines) |
|---|---|---|---|---|---|
| `Graphics/GraphicsConfig.cpp` | 2,380 | 8 | 92 | 18 (8,668) | 74 (140,023) |
| `Resources/ResourceLoader/ResourceLoader.cpp` | 4,383 | 19 | 100 | 22 (11,998) | 78 (147,525) |
| `Utilities/Threading/ThreadSystem.c` | 402 | 6 | 15 | 12 (3,178) | 3 (3,068) |
| `Utilities/MemoryTracking/MemoryTracking.c` | 249 | 5 | 15 | 9 (2,636) | 6 (2,980) |
| `Tools/BunyArchive/Buny.c` | 2,344 | 11 | 22 | 15 (5,570) | 7 (7,546) |
| `Tools/ReloadServer/ReloadClient.cpp` | 722 | 9 | 55 | 19 (4,460) | 36 (55,691) |
| `Tools/Network/Network.c` | 407 | 2 | 11 | 9 (2,667) | 2 (641) |
| `Tools/AssetPipeline/src/AssetPipeline.cpp` | 2,998 | 21 | 126 | 25 (11,313) | 101 (157,085) |
| `Application/Screenshot/Screenshot.cpp` | 396 | 11 | 104 | 29 (9,317) | 75 (141,773) |
| `OS/Android/AndroidBase.cpp` | 999 | 15 | 104 | 31 (9,498) | 73 (135,814) |
| `OS/Darwin/iOSBase.mm` | 850 | 16 | 104 | 31 (9,349) | 73 (135,814) |
| `Graphics/Vulkan/Vulkan.c` | 9,627 | 20 | 71 | 20 (16,290) | 51 (129,770) |
| `Graphics/Metal/MetalRenderer.mm` | 6,004 | 15 | 97 | 19 (10,454) | 78 (160,171) |
| `Graphics/Direct3D12/Direct3D12.c` | 6,650 | 16 | 67 | 20 (13,541) | 47 (109,745) |
| `Graphics/Metal/MetalMemoryAllocatorImpl.h` | 621 | 1 | 13 | 9 (3,008) | 4 (18,063) |
| `Utilities/RingBuffer.h` | 244 | 4 | 91 | 19 (7,164) | 72 (133,387) |
| `Graphics/Interfaces/IGraphics.h` | 2,962 | 7 | 53 | 14 (6,036) | 39 (80,764) |
| `Resources/ResourceLoader/TextureContainers.h` | 303 | 10 | 59 | 15 (6,339) | 44 (92,475) |
| `tinyimageformat_query.h` | 4,209 | 2 | 11 | 7 (2,177) | 4 (5,373) |

Reading: including `IGraphics.h` alone drags in 53 files. Everything that touches a GPU type pulls most of the tree, which is the concrete meaning of "entangled" throughout this report.
