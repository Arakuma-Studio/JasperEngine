# Lighting: tiles, capsule and contact shadows, OIT, hair, area lights, IES profiles, sky

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
- Area Light (a soft panel, window or screen): a `light` with `type: "area"` (`add_component` then
  `set_properties`), `areaShape` `rectangle` (`areaWidth`, `areaHeight`) or `disc` (`areaRadius`),
  in metres: the transform's scale does not size it. It lights only in front of its face, along
  `direction`. `intensity` is the face's brightness: `intensityUnit: "nits"` (or `set_light_unit`
  `nits`) for physical units; 1 m x 1 m at 1 lights like a point light of 1 far in front. `shadows:
  true` draws one 120 degree map. Lit Quality Standard approximates the highlight; High is exact
  (LTC).
- IES Profile (a real luminaire's beam): copy the `.ies` file in with `import_files`, then
  `set_properties` `iesProfile: "assets/lights/name.ies"` on a spot or point light; it turns with the
  light (vertical angle 0 along `direction`) and `intensity` stays its peak (the asset Inspector shows
  the file's peak candela). Files with `TILT=INCLUDE`, a tilt file or a photometric type other than C
  are refused, and the error names which.
- Hair: the material's `pbr.hair.enabled: true` (the Inspector's Shading Model: Hair) and its
  `primaryColor`, `primaryShift`, `primaryWidth`, `secondaryColor`, `secondaryShift`,
  `secondaryWidth`, `rotation`, `flowMap`.
- A Skybox with an HDR image (.hdr, .exr) whose sun casts no shadow: `set_scene_settings`
  `extract_sun: {remove_sun_disc: true}` creates or aligns the main Directional Light (direction,
  colour, intensity; lux when the light is in lux) and clamps the sun out of the sky's lighting.
  Refused when the sky has no distinct sun (its brightest spot under 20x its mean): place the light
  by hand.
- Ambient light by facing (sky above, ground below) instead of the sky image: `set_scene_settings`
  `ambient_mode: "gradient"` with `ambient_sky_color`, `ambient_equator_color`,
  `ambient_ground_color` (sRGB). The default `color` lights with `global_illumination_color` /
  `_intensity`, and Lit under a sky image with the image.

- Sky: a hidden sky (`skybox.visible` false, Lua `Skybox.visible`) or one on an inactive entity
  draws nothing and feeds no reflection probe; a hidden one keeps lighting the scene. It draws
  after the opaque objects (only the pixels they leave are shaded), first under a camera without
  `depthTest`. On mobile and web, `library:panoramic_sky` with the `VERTEX_MAPPING` feature
  (Per-Vertex Mapping) costs one texture read a pixel. Unity's rotations: `docs/unity-glossary.md`.

## Checking

- `get_render_stats` `lighting`: `light_mode`, `tier`, `tiles` (`active`, `lights`,
  `max_per_tile`, `average_per_tile`, `dropped_from_view`, `dropped_from_tiles`),
  `capsule_shadows` (`casters`, `capsules`), `contact_shadows` (`width`, `height` of the mask),
  `oit` (`active`, `fallback`: the device lacks half-float targets and the camera draws Sorted,
  `draws`), `passes` (`contact_shadows_ms`, `oit_accumulation_ms`; null until `get_gpu_passes`
  turns GPU timing on).
- A capsule character's moves leave `get_shadow_stats` `passes` at 0 with `capsules_only`.
- Look at it: `screenshot`.
