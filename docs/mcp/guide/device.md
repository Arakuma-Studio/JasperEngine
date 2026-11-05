# Device tiers for agents

The full reference is `docs/device-tiers.md`. This page is what an agent needs to read and test it.

## What a device gets

- At startup the engine matches the GPU's renderer name against the GPU database
  (`engine/core/device/gpu_tiers.json`): PCI ids, then the longest contained name, then a family
  pattern, then a vendor default, else a heuristic from memory and cores. The result is a tier,
  Low, Medium, High or Ultra, and a one-line reason.
- A player applies that tier's overrides (Project Settings > Quality > Device Tiers).
- **The editor applies none by itself**: in edit mode and in Play without a simulation, the project's
  values render. `get_device_info` still reports what this machine detects.
- A tier only lowers quality. Its cells cap the project's or the scene's value; an empty cell
  (`null`) inherits.

## Tools (group `device`)

- `get_device_info`: `gpu`, `backend`, `vendor`, `memory_mb`, `cores`, `detected_tier`, `rule`
  (`device`, `name`, `family`, `vendor`, `heuristic`), `reason`, `tier`, `tier_applied`,
  `tier_source` (`none`, `simulated`, `set`, `detected`), `platform` (the family whose table
  applies), `thermal_state`, `thermal_response`, `thermal_response_enabled`, `recovery_seconds`,
  `low_memory_warnings`, `texture_mip_drop`, `applied_overrides` (`{setting, value | factor |
  extra_mips, source: tier | thermal | memory}`), `lod_bias`, `shadow_lod_bias`, `upload_budget_mb`,
  `deferred_uploads`, `playing`, `simulation`.
- `set_device_simulation`: `tier` (`auto` applies none), `platform` (`auto` is `mobile` while the
  Game view simulates a phone, else `desktop`), `thermal_state` (`device` is the machine's). It is
  an editor preference (not undoable), applied in Play. While playing it replies with
  `get_device_info`'s fields two frames later.

## Editing the table

`get_project_settings` / `set_project_settings`, each change one undo step (`undo` scope `project`):

| Key | Value |
|---|---|
| `quality.device_tiers.<desktop|mobile|web>.<low|medium|high|ultra>` | object of cells, merged in; `null` inherits |
| `quality.lod_bias`, `quality.shadow_lod_bias` | 0.05..8, default 1 |
| `quality.thermal_response.<family>` | bool; default on for mobile only |
| `graphics.upload_budget_mb.<family>` | 1..1024 MB; 32 desktop, 8 mobile, 6 web |

Cells: `render_scale` (0.5..1), `msaa_samples` (1, 2, 4), `shadow_quality` (`low`..`ultra`),
`shadow_distance` (m), `lod_bias`, `shadow_lod_bias` (factors), `lit_quality` (`standard`, `high`),
`texture_quality` (mips dropped, 0..3), `bloom`, `ssao`, `volumetrics` (`false` turns off; `true` is
refused), `target_frame_rate`, `max_pixel_lights`, `particle_budget`.

Example: Low on mobile at a 0.6 render scale, without MSAA, SSAO inherited:

```json
{"values": {"quality.device_tiers.mobile.low": {"render_scale": 0.6, "msaa_samples": 1, "ssao": null}}}
```

## Testing a tier in Play

1. `set_device_simulation {tier: "low", platform: "mobile"}`
2. `play {action: "start"}`, wait a few frames
3. `get_device_info`: `tier_applied` true, `applied_overrides` lists the cells
4. `get_post_processing_status`: `render_scale_applied` is the tier's scale (a new uber variant
   builds first: poll)
5. `screenshot {view: "game"}`; change the tier and compare
6. `play {action: "stop"}` ends the simulation (the toolbar keeps it for the next Play)

Thermal: `set_device_simulation {thermal_state: "serious"}` on mobile lowers the render scale by 20%
and caps the frame rate at 30; `critical` adds the tier below's shadow and post-processing cells.
Going back to `nominal` recovers one state per 30 s (`recovery_seconds`). On desktop and web the
response is off unless `quality.thermal_response.<family>` turns it on.

## Lua

```lua
Device.setTier(DeviceTier.Low)
local reason = Device.tierReason
Device.onThermalStateChanged = function(state)
    if state == ThermalState.Critical then
        Log.print("critical: " .. Device.gpuName)
    end
end
```

`Device.tier`, `Device.gpuName`, `Device.tierReason`, `Device.tierApplied`, `Device.thermalState`,
`Device.setTier`, `Device.resetTier`, `Device.onThermalStateChanged`, `Device.onLowMemory`.

## Players and builds

- `--device-tier=low` (a player's command line, `?device-tier=low` on the web) picks a tier.
- A player's log has the decision: `Device tier: <gpu>, rule <kind> (<rule>), tier <Tier>`.
- Thermal and memory changes are logged: `Thermal state: ...`, `Thermal response: ...`,
  `Low memory (warning n): ...`.
