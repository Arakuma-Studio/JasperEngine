# Sprite normal maps (`generate_sprite_normals`)

`generate_sprite_normals` makes tangent-space normal maps for 2D art: from a sprite's silhouette (rounded
volume) and its painted shading (relief), OpenGL convention (green = up the image), the sprite's pixel size and
frame layout, its alpha byte kept, flat `(128, 128, 255)` where it is transparent. It runs in the editor only
(nothing of it ships in games): a C++ port of Sprite Fusion's sprite-normals (MIT). Humans have the same thing as
**Window > Sprite Normal Map**, the Project window's right-click > **Generate Normal Map...** / **Regenerate Normal
Map** (a badge marks stale maps), and the Inspector (a wand on the Normal Texture row, **Normal Map...** under Sprite
Frames, Regenerate on a generated PNG); shells have the CLI and `check --normals` (below). All of them run the same
code, so a preview in one is the same pixels as a write in another (on one platform; another OS's maths library
may differ by 1 per channel).

## When it shows

Only 2D lights read sprite normal maps, and a Light 2D lights a normal map only when its `height` is above 0
(lights made by the editor get 64; older ones keep 0). Sprites without a normal map are lit the same at any
height. Lower `ambient_light_2d_intensity` (0.2-0.4) to see the light at all. To give a light height:

```json
{"tool": "set_properties", "arguments": {"entity": "Light 2D", "values": {"light2d": {"height": 64}}}}
```

## The loop: preview, then write

1. Call **without `output`**: nothing is written. You get a lit contact sheet (image), `metrics`, `assessment`,
   a `hint`, `warnings` with fixes and `default_output` (the files `output: true` would write).
2. Look at the sheet; adjust `preset` / `params` until the hint says "Looks fine." or you like the light.
3. Call again with **the same arguments + `output: true`** (add `apply: true` on a GameObject source): exactly the
   previewed pixels are written, as PNGs carrying their recipe, as one undo step (`undo {scope: "project"}`); apply
   is a second undo step (`undo {scope: "scene"}`). The result's `undo` lists both.
4. Check in the scene: a Light 2D near the sprite, `screenshot {view: "game"}`.

The run is a job: past `timeout_seconds` (default 120) the call returns `{job_id, status: "running"}`; poll
`get_job`, stop with `cancel_job` (a cancelled run writes nothing). Opening another project fails the job
(`project_changed`) and writes nothing.

## Arguments

| Argument | Default | Meaning |
|---|---|---|
| `source` (required) | | An image file (`.png` APNG included, `.gif`, `.jpg`, `.tga`, `.bmp`, `.psd`), or a GameObject (id, path, unique name, `{id\|name\|path}`) that is a Sprite, a Polygon or a Tilemap: its colour texture is the source, a Sprite's frames the cells |
| `scene` | selected | the scene of a GameObject source and of `apply` |
| `preset` | `default` | `soft`, `default`, `crisp`, `detailed`, `chunky`, `toon` (below) |
| `params` | `{}` | overrides: `strength`, `detail`, `smoothing`, `volume`, `radius`, `softness`, `alpha_threshold`, `profile`, `edges`, `ignore_outline`, `regions`, `quantize` (Toon steps, below); an unknown key or a value out of range is refused |
| `frames` | `"auto"` | `"auto"`, `"none"`, a grid `{tile_size: N or [w, h], columns, rows, count, offset: [x, y], padding: [x, y]}` (as `set_sprite_frames.slice`) or `{cells: [[x, y, w, h], ...]}` |
| `directx` | false | green down (Unreal); the engine reads OpenGL |
| `height` | false | also a 16-bit grey height map |
| `output` | (preview) | `true`: the default names; a `.png` path: the normal map; `{normal?, atlas?, height?, frames?}` (`frames`: `true` or a `.json` path); `false`: preview |
| `overwrite` | false | replace existing outputs (one undo puts them back); inputs are never replaced |
| `apply` | false | on write: `true` = the source GameObject, or a GameObject reference (with an image source) |
| `save_path` | | also write the contact sheet PNG there (any project folder, not one linking out of the project; replaced without `overwrite`) |
| `paint` | `[]` | up to 256 correction operations (below, "Fixing spots: paint"); one normal map per call |
| `paint_mode` | `keep` | `keep` the map's existing paint, `replace` it, or `clear` it (no `paint` then) |
| `wait` / `timeout_seconds` | true / 120 (1..3600) | job control |

Outputs must be inside the assets directory, also once symbolic links are resolved (a folder of the assets that
links outside the project is refused; the assets directory itself is a trusted root: one that is itself a link, to a
shared drive, is fine, and only links below it are checked), not in a hidden folder, end
in `.png` (`.json` for frames), and be distinct (never the source, also not through a link). Default names: `<stem>_normal.png` beside the source; for a GIF/APNG `<stem>_sheet.png` (the colour atlas)
+ `<stem>_sheet_normal.png`; `<stem>_height.png` (`<stem>_sheet_height.png`); frames JSON on request only.
The files are written beneath the assets directory without following a link planted on the way
after the check; an undo or redo of the step whose folder has since become a link out of the
assets is refused (`undo` fails naming the file: remove the link, then undo again) and the history
keeps its place.

## Presets and settings

| preset | strength | detail | smoothing | volume | for |
|---|---|---|---|---|---|
| `soft` | 1.0 | 2 | 1.0 | 1.15 | gentle, painterly art |
| `default` | 1.5 | 3 | 1.0 | 1.15 | best measured accuracy on 16-64 px sprites |
| `crisp` | 1.5 | 3 | 0.6 | 1.15 | small characters, keeps 1 px features |
| `detailed` | 1.5 | 6 | 0.6 | 1.15 | most painted detail (the upstream defaults except radius: add `radius: 4` for an exact match) |
| `chunky` | 2.5 | 4 | 0.6 | 2.0 | stylised, strong volume |
| `toon` | 2.0 | 0.5 | 2.0 | 1.3 | cel-shaded pixel art: Toon steps on (`quantize` `{directions: 8, tilts: 1, flat_deg: 15, rotation_deg: 0}`) |

All presets: `softness` 1.15, `radius` auto; only `toon` quantizes. Explicit `params` override the preset.

- `strength` 0..8: multiplies every slope. `detail` 0..8: relief from the painted shading. `smoothing` 0..8 px:
  blur of the fine shading (the broad blur is twice it). `volume` 0..8: silhouette rounding.
- `radius` `"auto"` or 0.5..512 px: how far the rounding reaches. Auto: 15% of the shorter side of each island
  (one value for all frames of an animation or sheet), at least 4; the result reports `radius_resolved`.
- `softness` 0..8 px: blur of the silhouette height. `alpha_threshold` 0..0.99 (0.03): alpha at or below is empty.
- `profile`: `rounded` (default, upstream), `dome` (a circular cap, flat top beyond the radius: coins, buttons),
  `bevel` (a straight ramp then flat: tiles, crates).
- `edges`: `transparent` (default: beyond the image is empty, borders bevel), `wrap` (seamless textures: the
  opposite edge continues), `extend` (tiles of a tileset: the border pixels continue, no bevel).
- `ignore_outline`: finds the dark outline colour and keeps it out of the relief (a pixel-art outline otherwise
  becomes a groove).
- `regions` (at most 16): `{colors: ["#rrggbb", ...], tolerance: 0..0.5 (0.03), detail: 0..8 (1), lift: -16..16
  px (0), flat: false}`; a pixel takes the first matching region. Painted highlights that should not be bumps:
  `{"colors": ["#ffffff", "#fff3c4"], "detail": 0}`. Armour plates standing out: `{"colors": ["#8a8f99"], "lift": 3}`.

## Toon steps (`quantize`)

Pixel art usually reads best lit in a few flat bands, not smooth gradients. `quantize` snaps every generated
normal to a small palette of directions, so a moving light steps across the sprite like hand-shaded cel art:

| key | range | default | meaning |
|---|---|---|---|
| `directions` | 0 (off), 4, 6, 8, 12, 16 | 0 | azimuth steps around the circle |
| `tilts` | 1..4 | 1 | tilt rings between `flat_deg` and 90° |
| `flat_deg` | 0..45 | 15 | normals tilted less face the viewer exactly (flat tops, no speckle) |
| `rotation_deg` | 0..360 (360 is 0) | 0 | turns the palette (0: one direction points right) |

Per opaque pixel: tilted under `flat_deg` → `(0, 0, 1)`; else the azimuth snaps to `rotation_deg + k·360/directions`
(a tie, exactly between two directions, goes to the next direction counter-clockwise from `rotation_deg`) and the
tilt to the centre of its ring of `[flat_deg, 90)`. So every opaque pixel is one of
`1 + directions × tilts` normals. It runs after the relief, before the DirectX flip; painted pixels keep exactly
the direction painted; the height map is not quantized; flat regions stay flat. Missing keys keep the preset's
values (`{"quantize": {"directions": 8}}` is enough). Fewer directions read chunkier (4: a pyramid look); 16 is
nearly smooth. `tilts: 1` (the default) reads cleanest (two or three tones per light); 2 or more add rings tilted
70° and over at the rims (`too_steep` may warn: lower `strength`). Keep `rotation_deg` 0 for art with straight sides: a
rotation that puts a step boundary on an axis (22.5 with 8, 45 with 4) makes those sides speckle. The window's
**Toon steps** (Settings: Off / 4 / 8 / 16; Advanced: all six values, Tilt rings, Flat below, Rotation); the CLI's
`--quantize N --quantize-tilts N --quantize-flat DEG --quantize-rotation DEG`.

A cel-shaded hero, previewed, then blockier (4 steps) and written:

```json
{"source": "Hero", "preset": "toon"}
{"source": "Hero", "preset": "toon", "params": {"quantize": {"directions": 4}}, "output": true}
```

Quantized normals on any preset: `{"source": "Hero", "preset": "crisp", "params": {"quantize": {"directions": 8}}}`;
off on `toon`: `{"params": {"quantize": {"directions": 0}}}`. The recipe records `quantize` when it is on (or the
preset's is), so `regenerate` gives the same pixels.

## Sprites, sheets, animations, tiles

- `frames: "auto"`: a Sprite source uses its frames (`from: "sprite"`), a GIF/APNG its frames, else the whole
  image. Each frame is processed alone (its own islands, no bleeding across cells). `"none"`: the whole image as
  one (refused for GIF/APNG). A grid or `cells`: those rects (`from: "given"`; refused for GIF/APNG).
- A GIF/APNG becomes an atlas, frames packed row-major with `columns = ceil(sqrt(n))`. `apply` makes the Sprite
  use the colour atlas, sets its frames (`frame_0`...), its size to one frame, and adds (or updates) a Sprite
  Animation child named after the file with the file's delays as `frame_ms` (delays under 10 ms play at 100 ms,
  warned as `zero_delay`); a loop count of 1 plays once. `output.frames` writes `<atlas stem>.frames.json`.
- Tilesets: `frames: {tile_size: 16}`, `params: {edges: "extend"}`. A Tilemap source makes one map per tileset
  texture (`output: true` only) and `apply` gives each submesh its texture's map. Seamless textures:
  `params: {edges: "wrap"}`.

## Reading the result

The contact sheet: per frame (at most 8, spread evenly; `frames.sheet_note` says which) a row of colour, normal,
then lit from the top-left, top-right, bottom-right, bottom-left (the engine's 2D light formula).

| metric | means | warned when |
|---|---|---|
| `coverage_pct` | opaque pixels | |
| `flat_pct` | opaque pixels tilted under 2° | > 60 (`mostly_flat`) |
| `steep_pct` | tilted over 60° | > 25 (`too_steep`) |
| `mean_tilt_deg` | average tilt | |
| `noise_pct` | pixels disagreeing with their neighbours | > 15 (`noisy`) |
| `lean_deg` | the average normal's lean | > 8 (`leaning`) |
| `soft_edge_pct` | semi-transparent opaque pixels | > 10 (`soft_edges`) |

`assessment` says the same in words ("Shape: rounded (mean tilt 24°, 12% flat)"); `hint` is the fix of the first
warning, or "Looks fine.".

## Fixing spots: paint

A material wrong everywhere (all highlights, every outline) is a `regions` job. A few pixels or one frame (a
highlight that became a bump, an eye that became a dent, a sleeve that should face left) is a `paint` job: a
correction layer over the generation, stored in the normal map (a second iTXt chunk, `jasper.spritenormals.paint`)
and composited again on every regeneration, whatever the settings.

| op | does | extra |
|---|---|---|
| `angle` | the pixels face `direction` | `direction`: `[x, y, z]` (z > 0) or `{azimuth_deg, tilt_deg}` |
| `flatten` | the pixels face the viewer | |
| `smooth` | the box mean of the current result (same island and frame) | `radius` 1..4 |
| `erase` | removes paint (back to the generation) | |

Every op takes `where` (members intersect: `rect: [x, y, w, h]`, `pixels: [[x, y], ...]`, `colors: ["#rrggbb"]`
matched on the source with `tolerance` 0..0.5 (Oklab, 0 = exact), `frame`: a cell index, which makes `rect` and
`pixels` frame-relative) and `strength` 0..1 (coverage; a weaker op never lowers it). Pixels are of the output (the
atlas for a GIF/APNG, frames row-major as the recipe's `frames.cells`); transparent ones are skipped.
Directions are OpenGL whatever `directx`: azimuth 0 right, 90 up, 180 left, 270 down; tilt 0 faces you, at most 85.
Smooth bakes the result it smooths: re-smooth after big setting changes.

`paint_mode: keep` starts from the paint of the file the call writes (a preview: the default output), so a preview
shows exactly what writing gives and `regenerate` keeps the paint. The result's `paint` says `{mode, kept_from, ops:
[{op, selected, changed}], painted_pixels, hidden_pixels, painted_pct}`; the contact sheet outlines painted pixels in
magenta in the normal column. A kept layer of another size than the new output (the source was resized) is
`paint_size_mismatch`: `paint_mode: "clear"` drops it.

Flatten a painted highlight, then turn a sleeve left on frame 3 and write:

```json
{"source": "assets/sprites/hero.png", "paint": [{"op": "flatten", "where": {"colors": ["#ffffff"], "tolerance": 0.02}}]}
```

```json
{"source": "Hero", "output": true, "paint": [{"op": "angle", "where": {"frame": 3, "rect": [10, 12, 4, 6]},
  "direction": {"azimuth_deg": 180, "tilt_deg": 45}}]}
```

The height map is never painted. In the window: the Paint section's brushes (Angle B, Flatten N, Smooth M, Erase E,
a wand W, mirror X, Alt+click samples a direction); strokes undo with Ctrl+Z until Save writes the layer.

## Problems

Errors (`isError`, `errors: [{kind, message, fix, field?}]`; nothing is written):

| kind | means | fix |
|---|---|---|
| `invalid_source` | no such image or GameObject, not a Sprite/Polygon/Tilemap, its texture is not a file | a project image path or a Sprite GameObject |
| `invalid_settings` | unknown preset or `params` key (`quantize`'s too), wrong type, out of range | the ranges above |
| `frames_mismatch` | a grid that doesn't fit, `none`/grid/cells on a GIF/APNG, over 4096 cells | fix the grid, or `frames: "auto"` |
| `too_large` | over 64 MiB, 4096x4096 pixels, or 256 animation frames | trim or split it |
| `output_invalid` | outside assets, hidden folder, wrong extension, an input, twice, a path for several tilesets | an assets `.png` path, or `output: true` |
| `output_exists` | an output exists (also when it appeared during the run) | `overwrite: true` (one undo step puts it back) |
| `apply_needs_output` / `invalid_apply` | `apply` without `output`; `apply: true` with an image source, or not a GameObject | add `output: true`; `apply: "Hero"` |
| `not_a_sprite` / `size_mismatch` / `too_many_frames` | the apply command refused (the files are taken back) | apply to the GameObject drawing the source |
| `scene_playing` | `apply` while playing | `play {action: "stop"}`, then call again |
| `project_changed` | another project opened during the run | call again in the open project |
| `invalid_paint` | a `paint` op out of range (`field`: `paint[i].<key>`), over 256 ops, `clear` with ops, several outputs | the op table above; one call per output |
| `paint_size_mismatch` / `paint_too_large` | the kept paint has another size than the output; the layer is over 8 MiB | `paint_mode: "clear"`; paint fewer pixels |
| `write_failed` / `failed` | the disk or the generator failed | check the path; call again |

Warnings (in `warnings`, `{kind, message, fix}`): from the image `no_alpha` (no transparent pixel with edges
transparent: use `extend`/`wrap` for tiles), `mostly_flat`, `too_steep`, `noisy`, `leaning`, `soft_edges`,
`frames_touch_edges` (use `extend` when the art continues past the cell), `outline_not_found`, `region_unmatched`;
from the paint `paint_empty_selection` (an op selected nothing), `paint_layout_changed` (other frames than when it was
painted), `paint_hidden_pixels` (painted pixels now transparent), `paint_unreadable` (a damaged layer, dropped);
from the scene `no_light`, `no_light_height`, `ambient_high`, `size_mismatch`, `filter_mismatch`,
`material_linked` (save the material file), `too_many_frames`, `directx_in_engine`, `shader_ignores_normals`,
`zero_delay`.

## Using it on a GameObject

The map goes to `submeshes[0].material.normalTexture` (every submesh of a Tilemap). It must match the colour
texture's pixel size and frame layout (they share UVs). Pixel art needs the same filter on both (Nearest colour +
Linear normal smears the light): `apply` copies the colour texture's min/mag filter and wrap. Without `apply`, a
write on a GameObject source returns `apply_args` (the exact `set_properties`, and for animations
`set_sprite_frames` / `set_sprite_animation`, calls). The engine reads OpenGL green; use `directx` only to export
for Unreal; a map made for the engine's old 2D behaviour (green down) lights upside down: flip its green
(`check --normals` flags it as `normals-green-convention`). The library Sprite shader samples normal maps; mirrored sprites (negative scale, flipped frames) stay
correctly lit. In Lua: `Sprite(self.scene, self.entity):setNormalTexture("sprites/hero_normal.png")`.

## Recipes and regenerating

Every PNG written carries an iTXt chunk `jasper.spritenormals` (settings, frames, source path and SHA-1); exported
games ship the PNGs without it (and without the paint layer).
`get_asset_info` of a map shows `sprite_normals` (`source.state`: `fresh`, `changed`, `missing`, `invalid`; `stale`)
and `regenerate`: the arguments that write it again (`overwrite: true`); call the tool with them. `get_asset_info`
of a source shows `normal_map` (its `<stem>_normal.png`, stale or not). After you edit a source, the next call's
`external_changes.problems` names its maps. `check --normals` (docs/mcp/checks.md) reports:
`normals-source-changed`, `normals-source-missing`, `normals-recipe-invalid`, `normals-size-mismatch` (error),
`normals-frames-mismatch`, `normals-filter-mismatch`, `normals-directx`, `normals-green-convention`,
`normals-not-a-normal-map`, `normals-no-light-height`, `normals-paint-unreadable`, `normals-paint-size`.
With **Auto-regenerate when sources change** on (a generated PNG's Inspector), the editor rewrites a stale map itself
(no undo step); a map whose folder links out of the assets directory is not regenerated (the console says why).

## Shell

```sh
jasperengine-editor generate-sprite-normals --source art/hero.png --preset crisp
jasperengine-editor generate-sprite-normals --source art/hero.png --preset toon --quantize 4
jasperengine-editor generate-sprite-normals -s a.png -s b.png --output-dir normals --format json
jasperengine-editor generate-sprite-normals --source fx/torch.gif --frames-json --preview-out torch_sheet_preview.png
```

`--dry-run` generates and reports without writing; `--preview-out` writes the contact sheet; `--project <dir>
--sprite <GameObject>` takes a Sprite's texture and frames; `--cells-json` takes a recipe's `frames.cells`. With
`--project`, paths inside the project are taken as named, so a source reached through a link inside it is accepted
(its outputs must still be inside the assets directory once links are resolved). JSON:
`{ok, dry_run, seconds, results: [{source, kind, width, height, frames, settings, radius_resolved, metrics,
assessment, hint, warnings, outputs, written}], errors}`. Exit 0 written, 1 failed or an output exists without
`--overwrite` (nothing written), 2 usage error.

## Worked recipes

A single sprite (preview, then write and wire it):

```json
{"source": "Hero"}
{"source": "Hero", "output": true, "apply": true}
```

A sliced character sheet: slice first, then the GameObject (frames from the Sprite):

```json
{"tool": "set_sprite_frames", "arguments": {"entity": "Hero", "texture": "sprites/hero_sheet.png", "slice": {"tile_size": [32, 32]}}}
{"tool": "generate_sprite_normals", "arguments": {"source": "Hero", "preset": "crisp", "output": true, "apply": true}}
```

A GIF to an animated, lit sprite: `{"source": "assets/fx/torch.gif", "output": true, "apply": "Torch"}`.

A 16 px tileset on a Tilemap:
`{"source": "Ground", "frames": {"tile_size": 16}, "params": {"edges": "extend"}, "output": true, "apply": true}`.

Painted highlights read as bumps: `{"source": "Hero", "params": {"regions": [{"colors": ["#ffffff", "#fff3c4"], "detail": 0}]}}`.

A seamless tile: `{"source": "textures/stone.png", "params": {"edges": "wrap"}, "output": true}`.

## Common mistakes and limitations

- Light height 0: nothing changes. Linear filter on pixel-art normals: smeared light (apply copies the filter).
- A normal map from another sheet size or slicing: misplaced light (`normals-size-mismatch`, `-frames-mismatch`).
- DirectX maps: lit from the wrong side; regenerate without `directx`.
- Brightness is read as height: dark albedo becomes dents, bright paint bumps; use `regions` with `detail: 0`.
- 1 px lines get no sideways slope; parts that overlap merge (no depth between an arm and the body).
- Frames are processed independently: painted detail that moves flickers; raise `smoothing`.
- Not for UI Images (UI does not take 2D lights).
