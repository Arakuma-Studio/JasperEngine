# Shadows

3D shadows for agents: what to set, how to check it. The full user reference (tiers, the shader
interface, migration) is `docs/rendering/shadows.md`.

## Turning shadows on

- A light casts shadows with `set_properties` on its `light` component: `shadows: true`;
  `softShadows: false` makes them hard (Unity's Shadow Type: No Shadows / Hard / Soft).
- Leave `shadowBias` and `shadowNormalBias` at 1: they are in **shadow texels**, the same at every
  resolution and cascade. Raise `shadowBias` only for visible acne (stripes on lit surfaces),
  lower it if shadows float away from their casters.
- `mapResolution` 0 takes the quality tier's size; 256 to 4096 fixes it (the tier caps it).
- Directional lights: `shadowDistance` (metres, 0: the scene's `shadow_distance`, else the
  tier's), `numShadowCascades` (0: the tier's), `cascadeSplitMode` auto/manual with
  `cascadeSplits` [a, b, c] (fractions of the distance), `shadowAngularDiameter` (degrees, softness).
- Spot and point lights: `shadowSourceRadius` (metres, softness), `automaticShadowCamera`,
  `shadowCameraNear` / `shadowCameraFar`.
- `shadowStrength` 0..1 lightens shadows.
- Meshes: `castShadows`, `receiveShadows`, and `staticShadowCaster: true` on everything that
  never moves (level geometry): their shadows are cached and only redrawn when one of them
  changes. Moving and animated meshes stay dynamic.
- `shadowUpdateMode`: auto (cached, the default), every_frame, on_demand (redraw with
  `eval_lua` `light:requestShadowUpdate()`; moving casters wait for it).

## Scene and project settings

`set_scene_settings`:

| Setting | Values |
|---|---|
| `shadows_quality` | auto (the project's tier of the platform), none (hard only), low, medium, high, ultra (PCSS contact hardening) |
| `shadow_distance` | metres, 0: the tier's (40 / 60 / 100 / 150 m) |
| `shadow_fade_fraction` | 0..1 of the distance over which shadows fade out (0.1) |
| `shadow_cascade_blend` | true / false |
| `capsule_shadows_enabled` | true / false (false): the main directional light's soft shadows from the capsules of the 4 nearest Capsule Shadow Casters, up to 32 capsule tests per lit pixel |
| `capsule_shadow_intensity` | 0..1 (1) |
| `capsule_shadow_softness` | 0..2, the penumbra's widening per metre along the light (0.25) |
| `shadows_debug_view` | none, cascades, shadow_factor, texel_density, cache, atlas (editor only, never saved, no undo step) |

`set_project_settings` `graphics.shadow_tier.<platform>` (windows, linux, macos, web, android,
ios): low, medium, high, ultra. Defaults: desktop high, web medium, mobile low.

## Checking

- `get_shadow_stats` `{scene?, camera?: "scene"|"game"}`: the effective `tier`, the atlas sizes,
  `memory_bytes` against `memory_budget_bytes`, the shadow clock `update`, and per shadowed light
  its `direction`, slots (`first_slot` -1: no slot, too many shadowed lights), resolutions,
  `cascade_splits`, `radius`, `texel_world`; per slot the cache `path` this update (skip, restore,
  static, full, uncached), `dirty_cause` (first, light, fit, static_caster, dynamic_caster,
  camera_caster, atlas, forced), and `last_render` (`path`, `dirty_cause`, `dirty_rect`: the rect it
  wrote) `frames_since_render` updates ago; `passes` and `draw_calls` by path. A still scene settles
  to every slot skip and 0 draw calls after two frames. To see what a change redrew, read `update`
  before it: a slot drew since when `update - frames_since_render` is past it.
- Look at it: `set_scene_settings shadows_debug_view: "shadow_factor"` (grey lit fraction) or
  `"cascades"`, then `screenshot view: "scene"`; set it back to `"none"`.
- `set_shadow_debug` `{fit_camera_offset?, fit_camera_euler?, freeze_cache?, force_rerender?}`
  (editor only, transient): moves only the camera the cascades are fitted to (stability tests),
  holds or redraws every slot. Reset with zeros and false.

## Pitfalls

- Shadows end at the shadow distance (a far camera clip no longer extends them): raise
  `shadow_distance` for long views.
- At most 9 projective slots (each directional cascade and each spot light takes one) and 4
  point lights cast shadows at once; the most important lights get them.
- WebGL2 and GLES3 give ultra PCSS only where a variant has a texture unit left (always with
  directional lights only); point lights never take PCSS.
- A forked mesh shader written before these shadows keeps directional and spot shadows only;
  `check_shader` warns "legacy shadow interface"; re-fork it.
