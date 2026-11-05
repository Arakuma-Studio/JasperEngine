# Device tiers, thermal and memory response, upload pacing

A game runs on everything from a five-year-old phone to a desktop GPU. JasperEngine identifies the
GPU at startup, gives the device a **Device Tier** (Low, Medium, High or Ultra) and applies that
tier's overrides of the project's quality settings. While the game runs it reacts to the platform's
thermal state and memory warnings, and it spreads texture and buffer uploads from loading threads
over several frames. Spec: `docs/specs/S56-device-tiers.md`. Agent guide: `jasper://docs/device`.

## How a device gets its tier

At startup (`Engine::systemViewLoaded`) the engine reads:

- the renderer name: `GL_RENDERER`, the Vulkan device name, the DXGI adapter description,
  `MTLDevice.name`, or WebGL's unmasked renderer (`WEBGL_debug_renderer_info`) on the web;
- the backend, the platform, the physical memory and the CPU core count.

It matches the name against the GPU database, `engine/core/device/gpu_tiers.json`. The rules are
tried in this order:

1. **device**: the PCI ids, where the name carries them (ANGLE's WebGL strings on Windows:
   `ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 (0x00002487) ...)`);
2. **name**: the longest database name the renderer contains between word boundaries
   (`Adreno (TM) 650`, `NVIDIA GeForce RTX 3060/PCIe/SSE2` contains `NVIDIA GeForce RTX 3060`);
3. **family**: a pattern of a GPU family (`adreno 6[3-9]\d`, `mali g7[1-9]\d`, `apple m\d`);
   software rasterizers (SwiftShader, llvmpipe, lavapipe, Microsoft Basic Render) are Low;
4. **vendor**: the vendor's default on desktops and Apple (not on the web, where Safari's
   masked "Apple GPU" says nothing about the device);
5. **heuristic**, for a GPU the database does not know: Android and iOS under 3 GB of memory are
   Low, under 6 GB Medium, otherwise High; desktops are High (Medium with 2 cores or fewer); the web
   is Medium, or Low under 4 GB of `navigator.deviceMemory`.

Detection never fails. The log says what it decided and why:

```
Device tier: Adreno (TM) 650, rule name (adreno 650), tier Medium (OpenGL ES 3 / WebGL2)
```

Names are compared normalized: lower case, `(TM)` and `(R)` dropped, every character other than a
letter or digit a space.

### The database

`tools/device/convert_forge_gpu_data.py` writes `gpu_tiers.json` from The Forge's `gpu.data`
tables (Apache-2.0: `Common_3/OS/Android/android_gpu.data`, `OS/Darwin/apple_gpu.data`,
`OS/Windows/pc_gpu.data`, `OS/Linux/steamdeck_gpu.data`) and our own rules:

- The Forge's presets are calibrated for its visibility-buffer renderer, so they are an order,
  not our tiers. The script re-maps them per platform (`REMAP`): on Android, for example, `verylow`
  becomes Low, `low` Medium and `medium` High.
- `OVERRIDES` moves single GPUs our forward renderer rates differently (the Adreno 630 to 644, the
  Apple A14 and A15, the M1 family), each with its reason in the row's `from` field.
- `FAMILIES` and `VENDOR_DEFAULTS` cover GPUs newer than the table.

```
python3 -I tools/device/convert_forge_gpu_data.py --forge /path/to/The-Forge   # convert
python3 -I tools/device/convert_forge_gpu_data.py --embed                      # after a hand edit
python3 -I tools/device/convert_forge_gpu_data.py --check                      # CI: is the embed current
```

The engine embeds the JSON (`engine/core/device/GpuTiersData.cpp`, written by `--embed`), and
parses it once at startup. The script checks its acceptance table (Adreno 530/650/740, Mali-G52/G78,
PowerVR, Apple A12/A15/M1, Intel UHD, RTX 3060, SwiftShader) before it writes anything.

## Project Settings > Quality > Device Tiers

One table per platform family (Desktop, Mobile, Web): a row per setting, a column per tier. An
empty cell (`-`) inherits the project's (or the scene's) value. Click it to override; right-click
an overridden cell to inherit again. Every edit is one undo step (Edit > Undo with the Project
Settings window focused, MCP `undo` scope `project`).

**A tier never raises quality.** The cells cap the value the project or the scene sets:

| Row | Cell | Applied as |
|---|---|---|
| Render Scale | 0.5..1 | at most (also the dynamic resolution's maximum) |
| MSAA | Off, 2x, 4x | at most |
| Shadow Quality | Low..Ultra | the shadow tier at most |
| Shadow Distance | metres | at most |
| LOD Bias | factor | multiplies the project's LOD Bias, which multiplies each mesh's |
| Shadow LOD Bias | factor | multiplies the project's Shadow LOD Bias |
| Lit Quality | Standard, High | at most |
| Texture Quality | mips dropped 0..3 | at least |
| Bloom, SSAO, Volumetrics | Off | turns the effect off |
| Target Frame Rate | fps | at most (the platform's default rate counts as above it) |
| Max Pixel Lights | lights | at most |
| Particle Budget | particles | at most |

The defaults:

| Tier | Overrides |
|---|---|
| Low | Render Scale 0.5, Shadow Quality Low, LOD Bias 0.5, no Volumetrics, no SSAO |
| Medium | Render Scale 0.85 |
| High, Ultra | none: the project's values |

A project saved before device tiers existed has these defaults, and they never change a High or
Ultra device's look: a desktop with a known GPU renders exactly as before. The table, the LOD
biases, the thermal response and the upload budget are saved in `project.yaml`'s `device` block,
only where they differ from the defaults.

The row above the table shows the tier this machine detects (its reason in the tooltip).

Next to the table:

- **LOD Bias** and **Shadow LOD Bias** (`quality.lod_bias`, `quality.shadow_lod_bias`, default 1):
  the project-wide LOD multipliers, Unity's `QualitySettings.lodBias`.
- **Thermal Response** per platform family (`quality.thermal_response.<family>`): on for Mobile,
  off for Desktop and Web.

## The tier at runtime

- A player applies the tier it detects. The generated `main.cpp` calls `Device::configure` with the
  project's settings; the prebuilt player reads them from `game.json`'s `device` object.
- `--device-tier=low` (or `--device-tier low`; `?device-tier=low` on the web) picks a tier instead.
- Lua `Device.setTier(DeviceTier.Low)` picks one for the rest of the run (a graphics menu);
  `Device.resetTier()` goes back to the detected one.
- **The editor applies no tier by itself**, so its views and tests never depend on the machine's
  GPU. Its Game view simulates one in Play (below); `Device.setTier` works in Play too.

## Game view: Device simulation

The Game view toolbar's **Device** menu: Auto, Low, Medium, High or Ultra, a platform family (Auto
is Mobile while a phone is simulated, else Desktop) and a thermal state (This Machine, Nominal,
Fair, Serious, Critical). In Play the chosen tier's overrides apply, from the chosen family's
table. The Stats overlay shows the tier in effect, the family and the thermal state. Play's end
ends the simulation; the toolbar keeps its choice for the next Play.

## Thermal response

The thermal state comes from the platform:

| Platform | Source |
|---|---|
| Android | `PowerManager.addThermalStatusListener` (API 29+), polled every 2 s with `getThermalHeadroom(10)` from API 30 (a forecast at or above 1 counts as Serious, 0.85 as Fair) |
| iOS, macOS | `NSProcessInfo.thermalState` and its change notification |
| Windows, Linux, web | none: always Nominal |

Android's statuses map to Nominal (none), Fair (light), Serious (moderate) and Critical (severe and
above).

With the family's Thermal Response on:

- **Serious**: the render scale (dynamic resolution's maximum) drops by 20%, and the frame rate is
  capped at 30 when the target is above 30 (or the platform's default).
- **Critical**: also the shadow and post-processing cells of the tier below (Shadow Quality, Shadow
  Distance, Shadow LOD Bias, Bloom, SSAO, Volumetrics). On the project's values (no tier), High's.
- **Recovery**: a state steps down one level after 30 s spent below it; heating up again restarts
  the wait. Stepping up is immediate.

The response changes only what is safe to change at runtime (scale, frame cap, shadow and post
quality), never content. Every change is logged. Lua sees the platform's state as
`Device.thermalState` and gets `Device.onThermalStateChanged(state)`.

## Memory pressure

A low-memory warning comes from Android's `onTrimMemory` (RUNNING_LOW and above, every level of a
backgrounded app but UI_HIDDEN), iOS's `didReceiveMemoryWarning`, or on the web from a heap
heuristic: the wasm heap above 90% of its maximum, or Chromium's `performance.memory` above 90% of
its limit. At the next frame the engine:

- keeps one more mip off streamed textures (for the rest of the run),
- clears the pools' unused entries: the model and texture CPU copies that can be loaded again,
  unused shaders, sounds and fonts (`Engine::clearUnusedPools`),
- fires `Device.onLowMemory` (and `Engine.onLowMemory`, as before).

## Upload pacing

Textures and buffers that loading threads create or update go through the render command queue.
**Project Settings > Graphics > Upload Budget** (`graphics.upload_budget_mb.<family>`, default 32
MB on desktops, 8 on mobile, 6 on the web) is how much of their data a frame uploads:

- the rest waits for the next frames, in order;
- a single item larger than the budget goes alone in its frame;
- what refers to a waiting resource (its views, updates, destroy) waits with it; the commands that
  mark an object loaded never overtake it, so nothing draws with a resource that does not exist yet;
- a second update of the same resource in one frame waits for the next (sokol's rule);
- the cleanups that free a load's pixels run after the upload that reads them.

A monotonic readiness token (`SystemRender::getUploadToken`, `isUploadTokenComplete`) says when
everything queued so far has run. `Engine::areScenesLoaded` and the editor's screenshots wait for
the queue to be empty. An empty queue costs nothing.

Textures the main thread creates directly (most scene loads) are not paced: they are created
in the call that needs them. sokol-gfx creates an image from all its mips at once and updates every
mip together, so a large texture is not split per mip; progressive cooked textures (S14) already
arrive small first.

## Lua

```lua
-- a graphics menu
local function onQualityPicked(choice)
    if choice == "auto" then
        Device.resetTier()
    else
        Device.setTier(choice == "low" and DeviceTier.Low or DeviceTier.High)
    end
    Log.print("Device tier: " .. tostring(Device.tier) .. " (" .. Device.tierReason .. ")")
end

Device.onThermalStateChanged = function(state)
    if state >= ThermalState.Serious then
        Log.print("the device is hot: " .. Device.gpuName)
    end
end

Device.onLowMemory = function()
    -- drop what the game caches itself
end
```

| Name | Kind | Notes |
|---|---|---|
| `Device.tier` | ro `DeviceTier` | the tier in effect: set, simulated or detected |
| `Device.gpuName` | ro | the renderer name detection read |
| `Device.tierReason` | ro | why: `"Adreno (TM) 650, rule name (adreno 650), tier Medium"` |
| `Device.tierApplied` | ro | whether a tier's overrides apply (false in the editor until simulated or set) |
| `Device.thermalState` | ro `ThermalState` | the platform's state |
| `Device.setTier(tier)`, `Device.resetTier()` | fn | a tier for the rest of the run, and back |
| `Device.onThermalStateChanged` | event | `function(state)` |
| `Device.onLowMemory` | event | after the caches were cleared |

Enums: `DeviceTier.Low`, `Medium`, `High`, `Ultra`; `ThermalState.Nominal`, `Fair`, `Serious`,
`Critical`.

## MCP

The `device` group (`tool_groups enable: ["device"]`; callable either way):

- `get_device_info`: the GPU, the detected tier and its reason, the tier in effect and where it
  comes from, the thermal and memory state, the overrides in effect, the upload queue.
- `set_device_simulation`: `tier`, `platform`, `thermal_state` of the Game view's simulation.

The table goes through `get_project_settings` / `set_project_settings`: one key per family and tier
(`quality.device_tiers.mobile.low`), whose value is an object of cells merged in (`null` inherits).

## C++

`QualityResolver` (`engine/core/device/QualityResolver.h`) is the one place the resolved values come
from: `renderScale(sceneScale)`, `msaaSamples`, `shadowTier`, `shadowDistance`, `lodBias()`,
`shadowLodBias()`, `litQuality`, `textureQuality`, `bloom`, `ssao`, `volumetrics`,
`targetFrameRate`, `maxPixelLights`, `particleBudget`, `uploadBudgetBytes()`. Each takes the
project's or the scene's value and returns it unchanged when no tier, thermal step or memory warning
applies. `getRevision()` goes up with every change. `Device` (`engine/core/device/Device.h`) holds
detection, the signals and the simulation.

The platform layers report through `Device::reportThermalState` and `Device::reportLowMemory`, from
any thread. The main thread applies them in `Device::update` at the start of the next frame.
