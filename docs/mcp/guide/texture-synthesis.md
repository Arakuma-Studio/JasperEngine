# Texture synthesis

`synthesize_texture` makes textures from example images, in the editor (nothing of it ships in
games): a C++ port of Embark Studios' texture-synthesis (multiresolution stochastic synthesis,
"More Like This, Please!"). Humans have the same thing as **Window > Texture Synthesis** (also
right-click an image in the Project window > **Synthesize Texture...**); both run the same code.

## Which mode

| mode | gives | size |
|---|---|---|
| `tileable` (default) | a new texture that repeats seamlessly: floors, walls, terrain layers, materials with tiling UVs | `size` (default 1024) |
| `extend` | a new texture that looks like the examples but does not need to repeat (a larger backdrop, a variation) | `size` |
| `make_tileable` | the example itself made seamless: only a band along its edges (`border`, share of the size per edge, default 0.125) is regenerated, the rest kept | the example's |
| `inpaint` | the example with a region regenerated from the rest: remove a logo, a blemish, a visible seam, a watermark | the example's |
| `guided` | the examples rearranged to follow `target_guide` (a layout image: paths, patches, a height map); each example may have its own `guide` (e.g. its height map) | `size`, default the target guide's |

`tileable` makes a new texture; `make_tileable` keeps the photo you have and fixes only its
edges. Prefer `make_tileable` when the example is already the right content and size, `tileable`
when you want another size or more variation.

## Examples and PBR companions

- `examples`: project-relative image paths (PNG, JPEG, TGA, BMP, PSD, GIF; 8 or 16 bit). Several
  examples give the synthesis more to draw from. An entry can be an object: `sample_mask` (an
  image of the example's size: white areas may be copied, black never; keeps a crack or a logo
  out), `sampleable: false` (context only), `guide` (guided mode).
- `companions`: the other maps of a PBR set get the SAME arrangement, texel for texel, in their
  own format (a 16-bit height stays 16-bit; nothing is blended, so normal maps stay valid).
  `"auto"` (default) takes the maps next to each example that share its name up to a map word:
  `rock_albedo.png` -> `rock_normal.png`, `rock_roughness.png`, `rock_height.png`, `rock_ao.png`,
  `rock_metallic.png`, `rock_normal_gl.png`; `rock.png` -> `rock_height.png`. A map of another size
  than its example is left out with a warning (resize it first). `"none"` turns them off; an array
  names them (`{path | paths, name?, output?}`, one path per example).
- Outputs are named by the core's rule: output `rock_tile.png` + companion `rock_normal.png`
  (example `rock_albedo.png`) -> `rock_tile_normal.png`. `companions[].output` overrides a name.

## The loop: preview, then write

1. Call **without `output`**: nothing is written. The result comes back as images (the texture,
   at most 512 px; for the tileable modes a 2x2 tiling so a seam shows along the centre lines;
   the uncertainty map when many pixels matched poorly), with `metrics`, `hint` and the `seed`
   (random when you gave none). Use `quality: "draft"` and a small `size` (256-512) while
   choosing the mode, the examples and the parameters.
2. Iterate: another `seed` for another variation; `quality: "high"` for fewer seams and repeats;
   `params` for the raw parameters when a preset is not enough.
3. Call again with the **same arguments, the chosen `seed` and `output`**: the synthesis is
   deterministic (same inputs, parameters and seed give identical pixels, whatever the thread
   count), so the file is exactly the preview. The outputs (primary + companions) are one undo
   step (`undo` with scope `project`), refused when they exist unless `overwrite: true`.

Output paths must be inside the assets directory (`.png`). The texture is then a normal asset:
put it on a material (`set_properties submeshes[0].material.baseColorTexture`...), a terrain
layer, a sprite. For tiling surfaces, the library shaders' `STOCHASTIC` feature hides the
repetition of any tiling texture further (jasper://docs/shaders).

## Reading the result

- `metrics.seam_score` (tileable modes): the step across the wrap edge relative to the steps
  next to it. About 1: seamless; above 1.5: a visible seam. `hint` then suggests what to try
  (quality `high`, another seed; for `make_tileable` a wider `border`, 0.2-0.25).
- `metrics.high_uncertainty_pct`: the share of synthesized pixels whose neighbourhood matched
  the examples poorly (more than ~25 levels of 255 per channel), `mean_uncertainty` the average.
  A few percent is normal. Above ~15% the example is probably too regular or structured for this
  method (bricks, tiles, text, a grid, perspective) or too small for the output: expect broken
  structure, and look at the uncertainty image to see where.
- `assessment`: the same in plain words ("Seams: none (0.95)", "Uncertain pixels: 2%").
- `warnings`: companions left out, inputs resized, and the core's notes.

## Recipes and regenerating

Every PNG written carries its **recipe** in an iTXt chunk named `jasper.synthesis` (mode,
parameters, seed, size, the source paths and their SHA-1s, which output it is). Nothing else is
stored; it survives copies and renames, and game builds ignore it.

- `get_asset_info` on such a PNG returns `synthesis`: the recipe, `sources` with their state
  (`unchanged`, `changed`, `missing`), and `regenerate`: the `synthesize_texture` arguments that
  write it again (same seed, `overwrite: true`). `view_asset` adds a short summary.
- When a source changed (an artist repainted the example), regenerate with those arguments to
  bring the texture up to date; with unchanged sources they reproduce the file exactly.
- `jasperengine-editor check` (and `check --textures`) warns `synthesis-source-changed` /
  `synthesis-source-missing` for every synthesized texture whose sources changed or disappeared,
  and `synthesis-recipe-invalid` for a garbled recipe (docs: `docs/mcp/checks.md`); then
  `get_asset_info` returns `synthesis: {error}` instead of the recipe.
- Recipes record paths as JSON (UTF-8) text: an example, companion or output whose file name is
  not valid UTF-8 (raw Latin-1 bytes) is refused with an error saying to rename it, and such a
  companion next to an example is left out of `companions: "auto"` with a warning.
- Humans: right-click the PNG in the Project window > **Edit Synthesis...** reopens the window
  with everything restored (a notice lists changed sources); Regenerate, then Save.

## Long runs

The tool takes `wait` / `timeout_seconds` (default 120): a run that outlives the timeout returns
a `job_id`; poll `get_job` (progress, then the same result without the images) and stop it with
`cancel_job` (nothing is written). `show_in_editor: true` runs it in the Texture Synthesis window
so a watching human sees the live preview (the window's Save writes it when `output` is given).

Time grows with the output's pixels (Balanced, 8 cores): about 1 s at 512 x 512, 4-5 s at
1024 x 1024, 20 s at 2048 x 2048. `draft` is about twice as fast, `high` about twice as slow.
At most 4096 x 4096 pixels (each side 1..8192).

## Limitations

- It copies texels: it never rotates, scales or recolours them. Large structures (a single
  object, a face, perspective, lighting gradients across the example) do not survive; good
  examples are fairly uniform, top-down material photos or renders.
- Very regular patterns (bricks, tiles, planks in a grid) come out with broken rows: use a
  procedural or hand-made texture there, or `make_tileable` on a clean photo of the pattern.
- `make_tileable` and `inpaint` keep the example's size; resize the example first for another.
- Companion maps must have exactly their example's size; HDR images are not supported.
- Inpainting a region larger than about a third of the image gives repetitive fill: give more
  examples of the surroundings (extra `examples` with `sampleable: true`).
