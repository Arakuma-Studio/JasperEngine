# Sprite normal maps (`generate_sprite_normals`)

`generate_sprite_normals` makes tangent-space normal maps for 2D art, from 16 px pixel art to 4096 px painted
sprites: from a sprite's silhouette (rounded or inflated volume) and its painted shading (relief), OpenGL convention (green = up the image), the sprite's pixel size and
frame layout, its alpha byte kept, flat `(128, 128, 255)` where it is transparent. It runs in the editor only
(nothing of it ships in games): a C++ port of Sprite Fusion's sprite-normals (MIT). Humans have the same thing as
**Window > Sprite Normal Map**, the Project window's right-click > **Generate Normal Map...** / **Regenerate Normal
Map** (a badge marks stale maps), and the Inspector (a wand on the Normal Texture row, **Normal Map...** under Sprite
Frames, Regenerate on a generated PNG); shells have the CLI and `check --normals` (below). All of them run the same
code, so a preview in one is the same pixels as a write in another (on one platform; another OS's maths library
may differ by 1 per channel).

## When it shows

Lights need height > 0: only 2D lights read sprite normal maps, and a Light 2D lights a normal map only when its `height` is above 0
(new lights get 64; loaded ones keep theirs). Sprites without a normal map are lit the same at any
height. Darken a bright 2D ambient (`ambient_high`) to see the light at all. To give a light height:

```json
{"tool": "set_properties", "arguments": {"entity": "Light 2D", "values": {"light2d": {"height": 64}}}}
```

## The loop: preview, then write

1. Call **without `output`**: nothing is written. You get a lit contact sheet (image, read with `sheet_legend`),
   `metrics`, `assessment`, a `hint`, `warnings` with fixes, `default_output` and `next` (how to write it).
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
| `preset` | `default` | `soft`, `default`, `crisp`, `detailed`, `chunky`, `toon`, `painted`, `props`, `tile`, `stones` (below) |
| `params` | `{}` | overrides: `strength`, `detail`, `smoothing`, `volume`, `radius`, `softness`, `alpha_threshold`, `profile`, `edges`, `resolution_scale`, `soft_edge`, `ignore_outline`, `regions`, `quantize` (Toon steps, below), `gaps` (below), `version`; `light_steps` 0..16 (the sheet's lit columns only, below); an unknown key or a value out of range is refused |
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
| `painted` | 1.5 | 3 | 1.0 | 1.15 | painted, vector and organic art at any size: `profile` `inflate` |
| `props` | 1.5 | 3 | 0.5 | 0.7 | crates, panels, coins, buttons: `profile` `dome`, `softness` 1 (a round rim, a flat face) |
| `tile` | 2.0 | 3 | 1.0 | 1.15 | seamless textures: `edges` `wrap` (the painted relief only) |
| `stones` | 2.0 | 3 | 1.0 | 1.15 | floors and walls of stones or bricks between dark gaps: `edges` `wrap`, `profile` `dome`, `gaps` `{share: 0.3}`, `softness` 1.5 |

All presets: `softness` 1.15 (props 1, stones 1.5), `radius` auto, `profile` auto (painted inflate, props and stones dome),
`edges` transparent (tile and stones wrap), `resolution_scale` auto; only `toon` quantizes, only `stones` has gaps. Explicit `params` override the preset.
The last three were tuned on high-resolution sprites (below); the first six are the pixel-art presets, and at
128 px or less they give what they always gave (only the auto radius moved slightly).

- `strength` 0..8: multiplies every slope. `detail` 0..8: relief from the painted shading. `smoothing` 0..32 px:
  blur of the fine shading (the broad blur is twice it). `volume` 0..8: silhouette rounding. `detail`,
  `smoothing` and `softness` are for a 128 px sprite and grow with larger ones (`resolution_scale`, below).
- `radius` `"auto"` or 0.5..512 px (real pixels, never scaled): how far the rounding reaches. Auto: 30% of each
  island's thickness T (the radius of the widest circle inside it); with `dome` the smaller of T and 15% of the
  island's shorter side (a flat face inside round rims; a ring's tube stays round); at least 4, at most 512 (one value
  for all frames of an animation or sheet: from the medians of their largest islands); the result reports
  `radius_resolved`.
- `softness` 0..32 px: blur of the silhouette height. `alpha_threshold` 0..0.99 (0.03): alpha at or below is empty.
- `profile`: `auto` (default: `rounded` for islands up to 128 px, `inflate` above), `rounded` (upstream: keeps
  rising toward the middle, a crease along it), `dome` (a circular cap, flat top beyond the radius: coins, buttons,
  crates), `bevel` (a straight ramp then flat: tiles), `inflate` (the shape blown up like a balloon: every part as
  round as it is wide, no crease; the result's `profile_resolved` lists what auto chose).
- `resolution_scale`: `auto` (default) multiplies `smoothing`, `softness` and `detail` by k = max(1, 2T / 128),
  T the island's thickness (2T: the width of its widest part; for an animation or sheet the median of the frames'
  largest islands; an opaque `wrap` / `extend` tile has none: its shorter side), so the same art at 4x the size gets
  the same light; `off` takes them as given, in pixels. The result reports `scale_range` [min k, max k].
- `edges`: `transparent` (default: beyond the image is empty, borders bevel), `wrap` (seamless textures: the
  opposite edge continues), `extend` (tiles of a tileset: the border pixels continue, no bevel).
- `ignore_outline`: finds the dark outline colour and keeps it out of the relief (a pixel-art outline otherwise
  becomes a groove).
- `regions` (at most 16): `{colors: ["#rrggbb", ...], tolerance: 0..0.5 (0.03), detail: 0..8 (1), lift: -16..16
  px (0), flat: false}`; a pixel takes the first matching region. Painted highlights that should not be bumps:
  `{"colors": ["#ffffff", "#fff3c4"], "detail": 0}`. Armour plates standing out: `{"colors": ["#8a8f99"], "lift": 3}`.
- `soft_edge`: `auto` (default: above 128 px, soft pixels reaching the outside are outside the shape, below) or
  `solid` (every visible pixel is shape, as before: translucent art such as ghosts, smoke or glass with a thin rim).
  Only the silhouette changes; an image without soft pixels gives the same bytes.
- `version`: 2 (default); 1 regenerates a map made before high-resolution support with its old rules (the
  `regenerate` arguments of such a map carry it).

## High-resolution sprites

Above 128 px (an island's shorter side: the gate) the generator works differently, so painted 256-4096 px art
gets the light pixel art gets:

- **Resolution scale.** `smoothing`, `softness` and `detail` grow with the sprite's thickness (x 2T / 128): a
  1024 px disc smooths over 8 px and lifts its relief 8 times, a 1024 px ring with a 64 px tube not at all. Without
  it the painted relief all but vanished (a 2048 px stone tile came out flat) and the silhouette's creases stayed
  1 px sharp.
- **Inflate.** `profile: auto` inflates islands above 128 px: the height is the square root of the Poisson
  solution inside the shape, so a disc is a hemisphere, an arm or a bottle neck is as round as it is thin, and
  there is no crease down the middle (the `rounded` profile's ridge, an X across a square crate) nor fans from a
  wobbly outline. Measured on round shapes: 4.4° mean error against 12.5° before.
- **Thickness radius.** Auto radius is 30% of each island's thickness, not 15% of its box: rings, limbs and
  diagonal shapes get a radius that fits them (`dome`: the thickness, at most 15% of the shorter side).
- **Soft edges.** Pixels under alpha 0.5 that reach the transparent outside (a glow, a feathered rim) are outside
  the shape: they keep their alpha and their normal, but do not inflate the sprite. Semi-transparent pixels inside
  it (glass, a translucent belly) stay part of the shape. The outside is the transparency that reaches the image's or
  frame's border, and any transparent hole deeper than max(2 px, 5% of the shorter side of the island around it,
  whatever `edges`) (a ring's middle:
  its inner glow behaves like the outer one; a pinhole or a small hole in glass does not count). With `wrap` only the
  depth counts, so a tile gives the same map wherever it is cut. A sprite that is translucent all over (a ghost, a
  bubble: the band would take over 90% of it) keeps its whole shape. When the band takes most of a sprite and it
  comes out `mostly_flat`, the hint says so: pass `soft_edge: "solid"`.
- **Creases are measured.** `crease_pct` (below) warns `creased` with the fix (`profile: inflate` / `dome`, or
  `softness`); painted pixels are not counted, so painting over a crease clears it and a painted patch is no crease.
  When the paint (and the blur's reach around it) covers over half of what would be measured, `crease_pct` is `null`
  ("Creases not measured: paint covers most of the sprite") rather than 0. `roughness_deg` of the whole map warns `streaky` only past every
  good result of the evaluation (the painted relief is rough by design).

Long thin art is gated by its shorter side: a 1024x96 sword is pixel art to the generator (rounded, no scaling).
To give it the high-resolution treatment pass `profile: "inflate"` (or the `painted` preset), and for more
smoothing or softness `resolution_scale: "off"` with the pixel values you want (e.g. `smoothing: 4, softness: 4`).

Pick by the art: `default` (auto: inflate) or `painted` for characters, creatures, potions and plants; `props`
for flat-faced objects and rings (crates, panels, signs, coins, hoops: the rounded box measured 1.4° against 25°
with default, a torus 1.8°);
`tile` for seamless textures; `params: {profile: "rounded"}` for faceted gems or art that wants the ridge.
Generation takes about 0.1 s at 1024 px and 0.4 s at 2048 px (8 threads; a large island runs on every thread).

```json
{"source": "assets/characters/knight_2048.png"}
{"source": "assets/props/crate.png", "preset": "props", "output": true}
{"source": "assets/ground/stone_2048.png", "preset": "tile", "output": true}
{"source": "assets/fx/orb.png", "params": {"profile": "inflate", "softness": 3}}
```

The same pixels as before high-resolution support: `{"params": {"version": 1}}` (CLI `--settings-version 1`): the
old rules exactly (bbox auto radius, no scaling, no outer band, `auto` as `rounded`). A map made before keeps its
look when regenerated (its recipe is version 1); `check --normals` names those whose sprite is over 128 px
(`normals-before-hires`), and `{"params": {"version": 2}}` on its `regenerate` arguments updates it.

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

## Stone floors: gaps

A tile of stones or bricks between dark gaps comes out flat with `tile` (an opaque tile has no silhouette):
use `preset: "stones"`. `gaps: {share: 0..0.9}` (default 0, off) makes the darkest `share` of each frame's pixels
(their lightness, slightly blurred) gaps: outside the shape, so every stone gets its own rim and `profile`,
`radius` and `edges` work per stone. Gap pixels keep their alpha and get normals. Raise `share` when stones
merge, lower it when stones lose their dark edges; the result's warning `gaps_unmatched` (a flat or evenly lit
image: nothing darker to cut) names the fix. A bright-grout tile is not for gaps.

```json
{"source": "assets/ground/stone_floor.png", "preset": "stones"}
{"source": "assets/ground/bricks.png", "preset": "stones", "params": {"gaps": {"share": 0.2}}, "output": true}
```

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
then lit from the top-left, top-right, bottom-right, bottom-left (the engine's 2D light formula, 34° above the
sprite); `sheet_legend` says the same. `params.light_steps: N` lights those columns in N bands as a scene's Light
Steps does (default: the target scene's Light Steps); `sheet_legend.light_steps` is the count used.

| metric | means | warned when |
|---|---|---|
| `coverage_pct` | opaque pixels | |
| `flat_pct` | opaque pixels tilted under 2° | > 60 (`mostly_flat`) |
| `steep_pct` | tilted over 60° | > 25 (`too_steep`) |
| `mean_tilt_deg` | average tilt | |
| `noise_pct` | pixels disagreeing with their neighbours | > 15 (`noisy`) |
| `lean_deg` | the average normal's lean | > 8 (`leaning`) |
| `soft_edge_pct` | semi-transparent opaque pixels | > 10 (`soft_edges`; not above 128 px, where soft rims are outside the shape) |
| `crease_pct` | above 128 px: inner pixels on a crease: the silhouette's normal (the painted relief left out) over 8° from its blur; painted pixels and 3x the blur's sigma around them not counted | > 2 (`creased`) |
| `roughness_deg` | above 128 px: mean angle between the map's normal (relief and paint included) and its blur | > 13 (`streaky`, when not creased) |

Both are measured over the inner pixels (deeper than min(8k, half the frame's largest depth) inside the alpha-0.5
silhouette) against a blur over 4k px, k = max(1, 2T / 128) of the largest island; they are 0 for sprites of 128 px
or less (pixel art's 1-2 px creases are its look). Creases: the new defaults 0%, the `rounded` profile above 128 px
3.6-29%. Roughness is mostly the painted relief (the new defaults 0.2-8.9°, the `tile` preset on brushed cobbles
12.1°), so `streaky` is kept for maps rougher than any good result: brushwork turned into bumps by strong,
unsmoothed relief (`detail` 8 with `smoothing` 0.1-0.3 measured 14-21°). The `detailed` preset can warn on flat-shaded
art above 128 px (a flat-coloured character and a drawn crate measured 14.4-14.6°: every outline becomes a ridge);
`default` or `painted` suit such art.

`assessment` says the same in words ("Shape: auto -> inflate (mean tilt 42°, 0% flat)", "Roughness: 0.4° (creases
0.0%)"); `hint` is the fix of the first warning, or "Looks fine.". The result also has `scale_range` and
`profile_resolved`.

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
transparent: use `extend`/`wrap` for tiles, `stones` for stones between dark gaps), `mostly_flat` (above 128 px
with most of the sprite soft: try `soft_edge: "solid"`), `too_steep`, `noisy`, `boxy` (a sprite filling its box,
a crate or panel, on `rounded`/`inflate`: follow its fix, `preset: "props"` or `profile: "bevel"` with the radius
it names), `creased`, `streaky`, `leaning`, `soft_edges`, `frames_touch_edges` (use `extend` when the art continues
past the cell), `outline_not_found`, `region_unmatched`, `gaps_unmatched`;
from the paint `paint_empty_selection` (an op selected nothing), `paint_layout_changed` (other frames than when it was
painted), `paint_hidden_pixels` (painted pixels now transparent), `paint_unreadable` (a damaged layer, dropped);
from the scene `no_light`, `no_light_height`, `light_low` (raise the lights it names), `ambient_high`,
`size_mismatch`, `filter_mismatch`, `material_linked` (save the material file), `too_many_frames`, `directx_in_engine`, `shader_ignores_normals`,
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

Every PNG written carries an iTXt chunk `jasper.spritenormals` (settings, frames, source path and SHA-1; recipe
`version` 2 records `profile` and `resolution_scale` too, and a version 1 recipe regenerates with version 1); exported
games ship the PNGs without it (and without the paint layer).
`get_asset_info` of a map shows `sprite_normals` (`source.state`: `fresh`, `changed`, `missing`, `invalid`; `stale`)
and `regenerate`: the arguments that write it again (`overwrite: true`); call the tool with them. `get_asset_info`
of a source shows `normal_map` (its `<stem>_normal.png`, stale or not). After you edit a source, the next call's
`external_changes.problems` names its maps. `check --normals` (docs/mcp/checks.md) reports:
`normals-source-changed`, `normals-source-missing`, `normals-recipe-invalid`, `normals-size-mismatch` (error),
`normals-frames-mismatch`, `normals-filter-mismatch`, `normals-directx`, `normals-green-convention`,
`normals-not-a-normal-map`, `normals-no-light-height`, `normals-paint-unreadable`, `normals-paint-size`,
`normals-before-hires` (a map over 128 px made before high-resolution support: regenerate with `params.version` 2).
With **Auto-regenerate when sources change** on (a generated PNG's Inspector), the editor rewrites a stale map itself
(no undo step); a map whose folder links out of the assets directory is not regenerated (the console says why).

## Shell

```sh
jasperengine-editor generate-sprite-normals --source art/hero.png --preset crisp
jasperengine-editor generate-sprite-normals --source art/hero.png --preset toon --quantize 4
jasperengine-editor generate-sprite-normals -s a.png -s b.png --output-dir normals --format json
jasperengine-editor generate-sprite-normals --source fx/torch.gif --frames-json --preview-out torch_sheet_preview.png
jasperengine-editor generate-sprite-normals --source art/crate_1024.png --preset props
jasperengine-editor generate-sprite-normals --source art/hero_2048.png --profile inflate --resolution-scale off --smoothing 16
```

`--profile`, `--resolution-scale auto|off`, `--soft-edge auto|solid` and `--settings-version 1|2` are the `params` of
the same names.

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
  Above 128 px the relief is scaled up with the sprite, so dark outlines read as grooves: `ignore_outline`.
- Faceted gems and crystals inflate into pillows above 128 px: `params: {profile: "rounded"}` keeps the ridge.
- `props` on a character (dome over thin limbs) creases: `creased` says so; use `default` or `painted`.
- A 1024x96 sword or a long banner counts as pixel art (shorter side 96): see "High-resolution sprites" to force
  inflate or more smoothing.
- 1 px lines get no sideways slope; parts that overlap merge (no depth between an arm and the body).
- Frames are processed independently: painted detail that moves flickers; raise `smoothing`.
- Not for UI Images (UI does not take 2D lights).
