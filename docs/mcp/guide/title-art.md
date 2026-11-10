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

Reserved op names answer `not_available` naming the spec that adds them (looks, color schemes,
templates, library S84; animation S85; 3D S86; languages and import S87); `apply_recipe` is S83's.

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
     `options: {keep: false}` keeps no copy (one render instead of two, for batches of renders).
   - `layer` renders one layer alone, `output` an output's frame and content, `crop` [x, y, w, h]
     canvas pixels, `fallback: "rgba8"` the RGBA8 path (`"compare"`: both, `pixel_paths_diff`),
     `image: "none"` metrics only, `save_path` writes the PNG with real alpha.
   - `options: {cost: "desktop" | "mobile"}` adds the cost readout (below); other keys arrive
     with later specs, an unknown key fails listing them.
4. `check_title {path}`: PASS / WARN / FAIL per check with `measured`, `threshold`, `at` crops,
   `method` and a `fix` ({ops} for `edit_title`, or the tool to run). Apply the fix, check again.
   Checks: document_valid, assets_present, glyphs_present, exposed_targets, rim_spikes (WARN: a
   mitred rim's join at a sharp notch pokes through as a spike; fix: round joins), bake_fresh (no GPU);
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
- The editor's previews (thumbnails, the Inspector, the Designer) share 4 ms of each frame;
  `get_title`'s `previews` says how that holds: `p95_ms` of the last 200 working frames,
  `over_budget_frames`, `worst_step` (a step that alone takes longer cannot be sliced).

### Effects and generators

- Discover: `list_title_library {kind: "effects" | "generators", query?, category?}` (query
  searches names, aliases in English and Japanese, presets); `id` gives a type's params with
  their animation class (`live` plays per frame, `rerender`, `step`, `relayout`), presets with
  their params, cost class, 8-bit class and the op to paste. The full tables are
  `docs/mcp/guide/title-art-effects.md`.
- Effects transform a layer's pixels (`add_effect {layer, type, preset?, params?}`; the preset's
  values over the defaults, yours over both). Generators draw new content and are layers
  (`add_layer {type: "generator", generator}`), never effect entries.
- Rows with `entry: "recipe"` are another type's settings: `apply_recipe {layer, recipe}` adds
  them in one undo step (`long_shadow`, `shine`, `rainbow`, `vignette`, `emboss`, `deboss`,
  `icicles`, `crt`, `comic_dots`, `ben_day`). `entry: "style_setting"` rows switch a style member
  on (Dry Brush (かすれ): `set {"style.erode.enabled": true}`). "keyline" finds Outline.
- Shape effects (Distress, Drips, Ink Brush Stroke, Frost, Neon Tube, Chalk, Stitch, Contour
  Lines) read the layer's own pixels, so a glow earlier in the stack never widens them.
- A value on a param its `when` hides still applies and is written; the reply notes `inactive`.
- Cost: classes C0 (trivial) to C4 (bake only). `render_title {options: {cost: "mobile"}}`
  returns per effect `{class, passes, fre, taps, desktop_ms, mobile_ms, live}` and the shared
  distance field as its own row; mobile milliseconds are predictions. Prefer C0-C2 effects on
  titles that animate on phones; Lens Blur and long Extrudes are bake only there.
- Strobes (Glitch, Grain, Neon flicker, Scanlines flicker) animate by reseeding: their `seed` is
  a stepped live param.

### Effects and generators

- Discover with `list_title_library {kind: "effects" | "generators", query?, id?}`; `id` gives the
  params (with their animation class), the presets and the op that adds it. The full catalogue is
  `title-art-effects.md`.
- An effect changes a layer's pixels (its Effects stack). A generator draws new content and is
  its own layer: `add_layer {type: "generator", generator, preset?, follow?, place?, group?}`.
  Without `preset` the first preset applies (params, blend, layer effects). A following generator
  (Fire, Glints, Lightning...) draws from the layer `follow` names, `place` "above"/"below" it;
  `group: "auto"` wraps an Add-blended companion with its layer in "<layer> FX" so its light stays
  on that layer. `check_title`'s `fx_follow_missing` flags a generator whose layer is gone.
- Generators are seeded: change `seed` for another layout; strobes (lightning, speed lines) reseed
  by keying `seed`. Particle `time`, Glints `twinkle_phase`, Fire `phase` and Fractal Noise
  `evolution` animate without a re-layout.

### Runtime

A title goes into a game as a **Title Art Renderer** (`add_component {component: "titleart"}`, key
`titleart`) on an Image (UI) or a Sprite (world); with neither, the scene's kind is added in the
same step. It lends the companion its image; the companion's saved texture never changes. Fields
with `set_properties {component: "titleart"}`: `title`, `renderMode` (`baked`, `realtime`,
`glyph_atlas`), one property per exposed parameter (`overrides.text`, `overrides.glow_amount`; a
prefab variant may override one alone), the Live Effects (`sweepEnabled`, `sweepPosition`,
`glowPulse`, `tintAmount`, `glintsEnabled`... and `liveTime`, their clock).

- **Baked** (default): the bake, at an Image's cost, no engine code. Overrides do not apply.
- **Realtime**: re-renders when its text or a parameter changes, spread over frames within the
  frame budget (2 ms mobile and web, 4 ms desktop, 8 ms in Edit mode; per S56 platform group);
  the old image stays until the new one is ready. Builds add the `titleart` module.
- **Glyph Atlas**: for text that changes every frame (counters). Each glyph renders once with the
  look into atlas pages (`get_title`'s `glyph_atlas.pages`); a change then costs a compose of the
  plates and cells, no render. Effects that read neighbours (warp, distort, hull plates, masks)
  do not apply per glyph: `check_title {checks: ["glyph_atlas"]}` lists them with fix ops; 3D
  text, or no text layer exposed as `text`, falls back to Realtime.
- Live Effects (any mode, one draw, no extra pass): Sweep (a band at `sweepPosition` 0..1, 0 and
  1 off the frame; key it in a clip), Glow Pulse (needs a glow output), Tint, Glints (stars at the
  bake's highlight anchors). Nothing reads engine time: `--fixed-fps` runs are identical.

The Play loop: `play` → `eval_lua` (change) → `get_title {entity, wait_ready: true}` →
`screenshot {view: "game"}`. `get_title {entity}` returns `ready`, `pending` (queued, hidden,
shaders, rendering n/m, failed), `overrides`, `effective`, `live`, and `particles` (four
`create_particles` calls: Sparkle, Embers, Confetti, Coin Shower). `render_title {entity}` renders
the instance's state (`options.overrides` tries values, nothing saved); `options.rerender` renders
the live instance again.

Clip events (no script needed): `titleArtSetText(text)`, `titleArtSetParameter(name, value)`,
`titleArtSetColorScheme(id)`, `titleArtPlay(name)`; a first `path` argument addresses a child as
clip tracks do. Keyable rows: `titleart.sweepPosition`, `glowPulse`, `liveTime`, the colours and
numeric `overrides.<name>`.

```lua
local t = self.gameObject:getTitleArt()
if t ~= nil and t.renderMode ~= TitleArtRenderMode.BAKED then
    t.text = "JACKPOT"
    t:setParameter("glow_amount", 0.8)
    t.onReady:add("hud", function() Log.print("title ready") end)
end
```

Lua takes Exposed names only (never document paths) and raises sentences that say the fix: `text`
on a Baked title, an unknown name (with "did you mean"), a wrong type. `title` and `renderMode`
are read-only.

**Characters**: a build ships only the glyphs a Realtime or Glyph Atlas title can show (its
document's Characters set: presets Digits, Latin, Latin Extended, Kana, Joyo Kanji, Custom, a
Custom File, the document's own text; plus the component's Additional Characters and literal text
of shipped `titleArtSetText` events). Fonts are subset; a font that forbids embedding (OS/2
fsType) stops the export unless its import acknowledges a licence. In Play, characters outside
the set draw as nothing and `onMissingCharacters` fires, as in the build.

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
### 3D

A Text, Shape, Vector or Image layer becomes a lit solid with `{"op":"set_dimension","layer":"Logo","dimension":"3d"}`: the first
switch derives `style_3d` from the 2D style (the face fill as the face material, each enabled rim a band, the side at half the
face's value), so switching back to 2D restores the 2D look exactly. Edit it with `set` (`3d.` means `style_3d.`, `band#0.` and
`band:<id>.` mean a band): `3d.depth`, `3d.bevel.width` / `profile` (flat, chamfer, round, cove, step, inset, tube, custom),
`3d.bevel_position` (outside, center, inside), `3d.mode` (extrude, inflate, orb), `3d.skew`, `band#0.grow`, `band#0.offset`,
`transform.rotation_y`, `chars.2.tilt_x` (a tilted, `dz`-moved or `rig` letter becomes a rigid part that never remeshes). Document
values live in `scene_3d` (`scene_3d.environment.preset`, `scene_3d.tone_mapping`, `scene_3d.supersample`); a layer overrides them
value by value in `3d.lighting`, a material's environment in `3d.face.environment`.

- Ops: `add_band` / `remove_band` / `move_band` `{layer, band, index?, values?, preset?}` (presets bright_ring, dark_step, plate,
  tube_rim), `apply_material {layer, part: face|bevel|side|band#n, preset}` (gold, chrome, sapphire... `list_title_library
  {kind: materials_3d}`), `fit_to_tier {layer, tier}`.
- Environments (`list_title_library {kind: environments}`): the procedural studios (studio, strips, backlit, under, sunset, neon,
  gold, night) build in the editor; a photo environment without its HDRI renders the studio and reports `environment_missing`.
  Tone maps: none, aces_filmic, agx, neutral, punch (punch keeps gold saturated).
- `render_title {options: {quality: "low"}}` renders 3D layers as that Realtime tier would; every result with a 3D layer carries
  `stats_3d` (triangles, vertices, solids, build_ms, k, environment_face and per tier `{fits, over}`); `get_title` adds each 3D
  layer's effective style and lighting with the source of every value (`document`, `layer`, `part`).
- `options.debug_view` shows why a look reads wrong: `normals` (the shading normal as colour) and `environment` (the
  reflected environment, unweighted) with no tone map; `ao` (the occlusion alone), `glint` (the glint light alone), `ink`
  (the render without the glint) and `wireframe` (the mesh's edges).
- Budgets per layer: High 150k triangles / 8 solids, Medium 60k / 5, Low 30k / 3 (Android and HTML5 default to Low). Check them
  with `check_title {checks: ["3d"]}` (`3d_tier_budget.<tier>` FAILs come with a `fit_to_tier` fix; also `3d_z_fighting`,
  `3d_environment_missing`, `3d_light_limit`, `3d_mesh_fallback`).
- Bake a 3D title for anything static (an image costs nothing at run time); `export_title {format: "glb"}` writes the solids
  with their materials (`KHR_materials_*`) beside the title for use as a model.

### Library: Looks, Color Schemes, Templates

Three independent axes; each states what it keeps:
- `apply_look {layer, look, apply_font: false, keep_color_scheme: true}` restyles one layer: replaces
  its style (face, rims or 3D bands, bevel), effects, part effects and the generator layers its look
  made; keeps text, size, transform, masks, blend, warp, font (the look's font pair with
  `apply_font`); a 3D look switches the layer to 3D. Px values scale with the layer's size.
- `apply_color_scheme {layer?, color_scheme}` recolours by role (face, dark keyline, accent rims,
  dropped base, glows, shadows, light colours); every width, bevel and effect stays. Without
  `layer` the whole title. Twice changes nothing.
- `apply_template {template, keep_text: true, language: en|ja}` replaces the document's content;
  keeps the file, GUID, name and non-default outputs; your exposed texts go into its placeholders.
- `edit_title {path, create: {template | look, text?, language?}}` makes a new document from one.

Refs: built-ins `<family>/<slug>` (`metal/classic-gold`; schemes flat: `gold`), your project's
`project/<slug>`, your personal ones `user/<slug>`. A typo returns the three closest ids.

Tiers of `list_title_library`: no args (counts), `kind: looks | color_schemes | templates` with
`query` (en/ja/romaji), `family`, `dimension`, `tags` (rows with a ready `use` op), `id` (one item:
structure, keep/replace contract, font pair, requirements), `sheet {text | path + layer, ids?}`
(the items rendered on your own text or layer, numbered as the rows). Pick by seeing: render a
sheet on your own text before applying. Items with `available: false` need a type this build
lacks. Pack fonts list under `kind: fonts` (`pack: true`): a look names a pair `{latin, ja}`, the
first apply copies the face (with its OFL.txt) into `assets/TitleArt/Fonts/`.

Save your own: `save_look {layer, name, scope: project | user}`, `save_color_scheme {layer?, name}`,
`save_template {name, placeholders?}` (expose the title's text first), `copy_library_item {ref, to}`,
`delete_library_item {ref}` (the last two need no `path`). The whole catalogue by family:
`jasper://docs/title-library`. `check_title`'s `style_lint.*` checks report a layer's look; passing
checks is not approval: show the render to a person.
