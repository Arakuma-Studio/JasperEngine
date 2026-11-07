# Lighting: tiles, capsule and contact shadows, OIT, hair

For agents: what to set and how to check it. The full reference is `docs/rendering/lighting.md`.

## Setting

- Light Mode: `set_project_settings` `graphics.light_mode` automatic (default: Tiled on the Medium
  tier and above, Per Object on Low), per_object, tiled. Tiled draws every point and spot light per
  pixel from screen tiles (up to 32 a tile, 256 a view); a scene of directional lights alone stays
  per object.
- Capsule shadows: add a `capsuleshadowcaster` component to the character's root
  (`add_component`), give it `capsules` with `set_properties` or run `eval_lua`
  `CapsuleShadowCaster(scene, entity):fitToSkeleton()` on a humanoid; `castShadows`:
  `capsules_only` takes its meshes out of the shadow maps. Turn them on with `set_scene_settings`
  `capsule_shadows_enabled: true` (`capsule_shadow_intensity`, `capsule_shadow_softness`). The main
  directional light needs `shadows: true`.
- Contact shadows: `set_properties` on a directional or spot `light` with `shadows: true`:
  `contactShadows: true`, `contactShadowLength` (metres), `contactShadowThickness` (metres),
  `contactShadowSampleCount` 8..16. Off on the Low tier.
- Order Independent transparency: `set_properties` on the `camera`:
  `transparencyMode: "order_independent"`, `oitWeightScale`. Only alpha-blended materials take it.
- Hair: the material's `pbr.hair.enabled: true` (the Inspector's Shading Model: Hair) and its
  `primaryColor`, `primaryShift`, `primaryWidth`, `secondaryColor`, `secondaryShift`,
  `secondaryWidth`, `rotation`, `flowMap`.

## Checking

- `get_render_stats` `lighting`: `light_mode`, `tier`, `tiles` (`active`, `lights`,
  `max_per_tile`, `average_per_tile`, `dropped_from_view`, `dropped_from_tiles`),
  `capsule_shadows` (`casters`, `capsules`), `contact_shadows` (`width`, `height` of the mask),
  `oit` (`active`, `fallback`: the device lacks half-float targets and the camera draws Sorted,
  `draws`), `passes` (`contact_shadows_ms`, `oit_accumulation_ms`; null until `get_gpu_passes`
  turns GPU timing on).
- A capsule character's moves leave `get_shadow_stats` `passes` at 0 with `capsules_only`.
- Look at it: `screenshot`.
