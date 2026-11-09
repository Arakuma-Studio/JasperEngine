# Pixel art cleanup

Turn AI or loose art into real pixel art, or check art that already is, for agents. Tools (group
`pixel_art`): fix_pixel_art, lint_pixel_art. User docs: `docs/pixel-art-cleanup.md`.

## The loop

1. `fix_pixel_art {source}` previews and writes nothing. Read the verdict, the guards and the
   image (`compare`: the source with its grid beside the result).
2. Look at the compare image before writing. A verdict is a measurement, not your eyes.
3. Adjust `recipe` keys or `palette`, and preview again.
4. The same arguments plus `output: true` write exactly the previewed pixels: one undo step,
   `undo {id, name}` in the result.
5. Add `apply: <Sprite>` on the write to set its texture; for a sheet also its frames and one
   Sprite Animation per clip. That is a second, scene undo step.
6. `lint_pixel_art {source, palette, rules}` checks the written file or any art.

## Verdict, then next step

| Verdict | Means | Next |
|---|---|---|
| `clean` | pixel art at its own size, no FAIL | nothing to fix; lint against your palette and rules |
| `fixable` | a confident fix exists | look at the image, then the same arguments plus `output: true` |
| `needs_person` | a guard tripped or the grid is unsure | read `guards`; set `recipe.grid` or `palette`, or ask the user; `force: true` writes it anyway and records it |

Guards: `low_confidence`, `low_fit` (the grid: set `recipe.grid` by hand), `lost_share` (colours the
palette loses), `empty_frame`, `shared_cell` (a sheet's cell size), `no_grid`, or a FAIL lint the fix
cannot remove. A `needs_person` write without `force` is refused and writes nothing.

## Recipe keys

Any subset of these, every default in brackets; an unknown key fails naming the valid ones.

| Key | Values |
|---|---|
| `grid` | [`"auto"`], `{kind: "cells", cell_width 1-128, cell_height?, offset_x, offset_y}`, `{kind: "size", width 1-4096, height?}` (pixelate) |
| `follow` | [`off`], `straight`, `bend`: grid lines follow the art's edges |
| `outlines` | [`auto`], `on`, `off`: keep one-pixel strokes whole |
| `moved` | `[["x:3", 41.5], ...]`: lines dragged, in source pixels |
| `sample` | `{inset 0-0.45 [0.25], alpha_threshold 0-255 [128]}` |
| `palette` | [the project's Default Palette, else auto], `"none"`, `"auto"`, `{auto: {count 2-256 [16], method dominant, balanced or distinct}}`, a preset name, a `.palette` path, or `["#rrggbb", ...]`; `locked: [indices]` keeps colours exact |
| `metric` | [`oklab`], `ciede2000` |
| `dither`, `dither_strength` | [`none`], `ordered`; 0-1 [0.5] |
| `cleanup` | `{size 0-16 [0], strength 0-1 [0.5]}`: merge specks; AI art wants `{size: 2}` |
| `clean_keep`, `edits` | specks kept and hand edits, recorded by the window at one output size |
| `target_width`, `reduce_mode` | shrink: null, 1-4096; [`outlines`], `common` |
| `remove_background`, `background` | [false]; `{tolerance 0-0.3 [0.08], enclosed, specks [true], shadow}` |
| `crop` | [false]: only the box around the opaque pixels |
| `lost_threshold` | 0-1 [0.06] |
| `alpha_cutoff`, `seed` | 0-255 [128]; [1] |
| `sheet` | [`auto`], `off`, or `{frames, animations, anchor ground, feet, center or box, padding [1]}` |

The result's `recipe` is the resolved recipe: it re-runs alone. `rerun: true` on an output runs its
stored recipe again; an unchanged source gives identical bytes, a changed one reports
`stale: {source_changed: true}`. A re-run over an output edited since fails `output_edited`.

## Keeping the source as the asset

`output: {import_step: true}` writes no file: the resolved recipe goes in the source's `.meta`
(`import.pixelCleanup`), as one undo step, and the texture cooks the fixed pixels while its source
stays as it is. It takes a raster image, never an `.aseprite` or an animated source, and refuses
`needs_person` like a write does. A source changed later whose verdict drops to `needs_person` cooks
raw, with a Console warning.

## Rules of thumb

- Add a lost colour to the palette rather than raising `lost_threshold`.
- Never loosen a lint threshold to pass; thresholds are reported with their source. Fix the art.
- A sheet's output defaults to one `.aseprite` with a tag per animation; `format: "png"` writes a
  sheet plus Aseprite JSON.

## Worked example: an AI sprite sheet becomes an `.aseprite`

1. `fix_pixel_art {source: "assets/art/walk_sheet.png"}`: verdict `fixable`, `animation.boxes` shows
   one box per frame, `animations` one clip per row.
2. The compare image shows the labels were dropped and the grid fits; `palette: "sweetie-16"` to use
   the project's colours, `recipe: {remove_background: true}` to clear the painted background.
3. The same arguments plus `output: true` write `assets/art/walk_sheet.aseprite`, tags `animation-1`,
   ... and `.meta` with the recipe, one undo step.
4. `lint_pixel_art {source: "assets/art/walk_sheet.aseprite", frames: "animation-1", palette: "sweetie-16"}`.
