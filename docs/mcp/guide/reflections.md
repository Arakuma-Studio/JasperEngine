# Reflections

SSR and reflection probes for agents: what to set, how to check it. The user reference is
`docs/rendering/reflections.md`.

## Turning them on

- SSR: `set_scene_settings` `ssr_enabled: true` (main camera only); `ssr_intensity` (1 physical),
  `ssr_max_distance`, `ssr_thickness`, `ssr_blur`.
- A reflection probe: `create_entity` type `reflection_probe`, its `reflectionprobe` `boxSize`
  covering the room; Static captures once, Dynamic on move or on an interval.
- SSR **replaces** the sky, probe or ambient reflection where it is confident, weighed by the
  surface's occlusion (occlusion map, SSAO) and the fog. It never adds a second reflection.
- No SSR on unlit materials, planar mirrors, and roughness 0.6 and above.

## Checking it

- `set_scene_settings` `ssr_debug_mode`, then a Game view `screenshot`:
  - 7 Reflection Source: blue sky, red/green/yellow/cyan probe slot 0-3, orange a probe without a
    slot (no SSR), grey ambient, magenta SSR added only, black no SSR;
  - 8 Occlusion: what the swap is weighed by (white none);
  - 6 Environment Specular: the reflection SSR swaps out; 1 Reflection: SSR's own.
  Set it back to 0 after (it is saved with the scene).
- `get_render_stats` `ssr`: `probe_slots` (4), `probes_in_view`, `probes_without_slot`,
  `probes` [{entity, name, slot (null: none), coverage}].

## Fixing

- Too many probes in view (`probes_without_slot` > 0): a view has 4 slots, given to the probes
  covering the most screen; the others keep their probe reflection without SSR. Use fewer,
  larger probes, or accept it on small surfaces.
- A surface too shiny with SSR: check mode 7. Magenta (a lit library shader, or no lights and no
  sky) adds SSR on top; give the scene a light or a sky, or lower `ssr_intensity`.
- Reflections showing through UI or glass: transparents and the main camera's HTML UI are in the
  picture SSR composites (`docs/rendering/reflections.md`, What is approximate).
