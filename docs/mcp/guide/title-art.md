# Title Art (`.titleart`) with the `title_art` group

Title Art is a document of layers (Text, Shape, Image, Vector (SVG), Group, Generator,
Adjustment) with rims, bevels, metal, glass and glow, baked to ordinary textures. Enable the tools
first: `tool_groups {"enable": ["title_art"]}`. `diff_images` is in `editor_tools`.

The tools: `list_title_library` (what exists), `get_title` (read), `edit_title` (ops, one undo
step), `render_title` (see it, with real alpha), `check_title` (measure), `export_title` (bake).
Editor Lua reaches every one through `Editor.call("edit_title", {path = "...", ops = {...}})`.

### Units and coordinates

- `%` members (rim widths, bevel sizes, char offsets) are percent of the layer's source `size`
  (a text's em size, a shape's height, an image's or SVG's larger side), so a style transfers
  across sizes. `px` members (every effect, generator and mask distance) are canvas pixels and
  scale with the render or bake scale. `edit_title` echoes a `%` change with its `px` value.
- Document space: canvas pixels, origin at the canvas centre, **+y up**. Image space (PNG rows,
  `crop`, `alpha_bounds`, check crops, frames): top-left origin, +y down. A layer at
  `transform.position [0, 300]` sits above the centre.
- Angles are clockwise on screen from +x, except light directions (`global_light.angle`,
  `bevel.light_angle`, `satin.angle`): Photoshop's "comes from", counter-clockwise, 120 = upper left.

### Ops

`edit_title {path, ops: [...]}` applies the ops to a copy, validates, and commits them as one
undo step ("Set Rim Width", "Edit Title Art (5 changes)"); any failing op applies nothing and the
error lists the valid names and the nearest one. `dry_run` validates only; `expect_rev` refuses
when the file moved on (another editor, a person). `create: {canvas: [w, h], text}` makes the
file (one Text layer, exposed parameter `text`, output `main`) and bakes it.

Addressing:
- layers: a path of names from the top level, `/`-joined, `#N` for the Nth same-named sibling
  after the first (`Logo/Title`, `Spark#2`), or `id:<8 hex>`. A bare name anywhere works when
  it is unique.
- fields: dot paths with `[i]` (`style.rims[1].width`, `chars.3.rotation`,
  `style.face.fill.gradient.stops[0].color`). Shorthands: `face.` (style.face.), `rim#0.` (the
  innermost rim) or `rim:<id>.`, `fx:outer_glow.` / `fx:outer_glow#1.` (an effect's params),
  `rim#1/fx:outer_glow.` (a part's effect), `mask#0.`, `gen.` (generator params), `adj.`.
  Document members omit `layer`: `canvas.width`, `global_light.angle`, `outputs[0].scale`.
- colours: `"#rrggbb"`, `"#rgb"` or `[r, g, b]` 0..1; alpha is always a separate member.

The ops of this build (later specs add theirs to the same tool):

```json
{"op": "set", "layer": "Title", "values": {"text": "JACKPOT", "size": 240, "face.fill.color": "#ffd34d"}}
{"op": "set_text", "layer": "Title", "text": "確定", "font": {"path": "TitleArt/Fonts/DelaGothicOne.ttf"}}
{"op": "add_layer", "type": "shape", "name": "Plate", "before": "Title", "values": {"shape": "ribbon", "size": 260}}
{"op": "add_layer", "type": "generator", "generator": "fill"}
{"op": "remove_layer", "layer": "Plate"}            {"op": "duplicate_layer", "layer": "Title"}
{"op": "move_layer", "layer": "Plate", "after": "Title"}
{"op": "group", "layers": ["Title", "Plate"], "name": "Logo"}   {"op": "ungroup", "layer": "Logo"}
{"op": "add_rim", "layer": "Title", "values": {"width": 6, "material.fill.color": "#2a1200"}}
{"op": "remove_rim", "layer": "Title", "rim": "rim#0"}   {"op": "move_rim", "layer": "Title", "rim": "rim#2", "index": 0}
{"op": "add_effect", "layer": "Title", "type": "outer_glow", "params": {"size": 60}}
{"op": "remove_effect", "layer": "Title", "effect": "fx:outer_glow"}   {"op": "move_effect", "layer": "Title", "effect": "fx:drop_shadow", "index": 0}
{"op": "add_mask", "layer": "Title", "type": "gradient", "values": {"angle": 90}}   {"op": "remove_mask", "layer": "Title", "mask": "mask#0"}
{"op": "expose", "name": "subtitle", "type": "text", "targets": [{"layer": "id:3fa0b2c1", "field": "text"}]}   {"op": "unexpose", "name": "subtitle"}
{"op": "link_material", "layer": "Title", "material": "style.rims[0].material", "link": "gold"}   {"op": "unlink_material", ...}
```

Reserved op names answer `not_available` naming the spec that adds them (recipes S83; looks,
color schemes, templates, library S84; animation S85; 3D S86; languages and import S87).

### The loop: render, check, fix

1. `get_title` (detail `overrides` shows only values that differ from defaults; `fields` reads
   exact values: `["style.rims[*].width"]`).
2. `edit_title` with ops; add `render: true` (or `{max_width, backgrounds, crop}`) to see the
   result in the same call.
3. `render_title {path, backgrounds: ["checker", "black", "white"]}`: real alpha on each
   background plus `alpha_bounds` (canvas pixels, with margins), `coverage_pct`, `flat`.
   - What-if without touching the file: `ops` (one variant) or `try: [{label, ops}]` (up to 9
     cells, labelled in `cells`).
   - `compare: "rev:<n>"` (the last 16 renders of each document are kept) or a PNG path gives
     `diff {changed_pct, max, mean, bbox}`: a no-op is 0, a colour change is > 0.
   - `layer` renders one layer alone, `output` an output's frame and content, `crop` [x, y, w, h]
     canvas pixels, `fallback: "rgba8"` the RGBA8 path (`"compare"`: both, `pixel_paths_diff`),
     `image: "none"` metrics only, `save_path` writes the PNG with real alpha.
   - `options` keys arrive with later specs; an unknown key fails listing them.
4. `check_title {path}`: PASS / WARN / FAIL per check with `measured`, `threshold`, `at` crops,
   `method` and a `fix` ({ops} for `edit_title`, or the tool to run). Apply the fix, check again.
   Checks: document_valid, assets_present, glyphs_present, exposed_targets, bake_fresh (no GPU);
   not_flat, not_clipped, alpha_bounds, legibility_contrast, counter_survival, noop_effects,
   clipped_highlights, halo, deterministic, fallback_parity, golden (`baseline`: a PNG or
   `rev:<n>`; `save_baseline` writes one). Passing is not aesthetic approval: show the render to
   a person.

### Baking, outputs and freshness

- Outputs (`outputs[i]`): `content` all, layers, ink, glow or mask; `scale`; `trim` with
  `padding`; `round_to` (4 for block-compressed textures); `bit_depth` 8 or 16; `alpha_bleed`;
  `max_size`. Configure them with `edit_title` (`set {"outputs[0].scale": 2}`).
- `logo.titleart` bakes `logo.titleart.png` (main), `logo.titleart.<id>.png` and the sidecar
  `logo.titleart.bake.json` (frame, origin, outputs, alpha bounds). Every output shares one trimmed
  frame, so layered parts register exactly. The PNGs are ordinary textures: place them like any
  image; renaming or moving the document moves them.
- Each PNG carries a `jasper.titleart` recipe: it is **fresh** when the document text, the hashes
  of the fonts and images it reads, and the bake version match; else stale (with what changed),
  missing or invalid. `get_title`, `get_asset_info` (the document or the PNG) show it.
- `export_title {path}` bakes (a job; fresh outputs are skipped unless `force`); `dry_run`
  reports files, sizes, bytes and VRAM. Auto Bake (Project Settings > Title Art) rebakes outputs
  that exist 500 ms after an edit; `edit_title {bake: "now"}` bakes before replying.
- Exports ship the PNGs, never the `.titleart` (unless a Realtime title needs it); stale outputs
  are rebaked first (title_art.stale_bakes_on_export: rebake, warn or fail).
- Headless: `jasperengine-editor check --titles` (no GPU) reports `title-*` problems with the
  fix command; `jasperengine-editor call export_title --project <p> path=<doc>` bakes.

### Common mistakes

- Pixel sizes in `%` members (a rim `width` of 40 is 40 % of the text size, very thick).
- y down in document positions (y is up there; only image-space results are y down).
- Editing the PNGs: they are derived files; edit the document and bake.
- Reading `check_title` PASS as "it looks good": always look at the render.

### Title Designer

The window a person designs in (Window > Title Designer) edits through the same ops. To show a
person a variant, use `render_title` `try`; to leave it on their screen, `focus_window` with
`select`.
- `focus_window {window: "title_designer", title_designer: {...}}` keys: `path` (open it),
  `select` (`"Title"`, `"Title/rim#1"`, `"Title/fx:outer_glow"`, `""` = document), `panel`
  (layers, properties, library, history), `search` (types into Find a setting), `library`
  `{tab, family, query}`, `preview_as` (realtime, baked, or a registered mode), `background`
  (checker, black, white, gray, color, image, game_scene, in_context), `device`
  `{profile, orientation, display_height}`, `zoom` (number or `"fit"`). The result's `state`
  has `path, selection, panel, zoom, preview_as, busy, staged, preview_active, last_render_ms, rect`,
  `library {provider: starter|registered, shown}` and `notices` (`cjk_font_missing`: the person's
  editor shows this title's CJK text as boxes; say so).
- `screenshot {view: "title_designer"}`: the window as a person sees it.
- `get_title` adds `designer {selection, staged, preview_active, view}` while it is open.
