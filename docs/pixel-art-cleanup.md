# Pixel Art Cleanup

Pixel Art Cleanup turns a picture that only looks like pixel art (an AI image of fake pixels, a
resized screenshot, a sheet of frames on a painted grid) into real pixel art, exactly when it can.
It finds the grid, reads each cell, snaps to a palette, and slices sheets into animations. It
tells you when it is sure, and refuses to guess when it is not.

Open it from **Window > 2D > Pixel Art Cleanup**, or right-click an image in the Project window and
choose **Pixel Art Cleanup...**. Agents use the `fix_pixel_art` and `lint_pixel_art` tools; shells
and CI use the `fix-pixel-art` and `lint-pixel-art` commands.

## What you get

Every run ends in a verdict:

- **Clean**: already pixel art at its own size, no rule broken.
- **Fixable**: a confident fix exists.
- **Needs a Person**: the grid is unsure, too much of the picture is lost, or a frame came out
  empty. Nothing is written without your say-so: in the window you confirm, an agent passes
  `force`, and `lint_pixel_art` reports that output as forced.

The fix keeps its recipe with the output, so it can be repeated exactly: in a PNG's `jasper.pixelcleanup`
chunk (the exporter strips it, so it never ships) or in an `.aseprite`'s `.meta`. If the source
changes later, the output shows a **Source changed** badge and **Re-run Pixel Art Cleanup** brings it
back in line. A re-run refuses to overwrite an output someone edited since.

## Outputs

- A single picture writes `<name>_clean.png` beside the source.
- A sheet or animation writes `<name>.aseprite`, one tag per animation, or a PNG sheet plus
  Aseprite-style JSON when the format is PNG.
- The source is never replaced. Every write is one undo step.

## The recipe

The recipe holds every setting: the grid (auto, a cell size, or a pixel count for pixelating), how
grid lines follow the art, the palette (the project's Default Palette, automatic, a palette asset
or a preset, with locked colours), speck cleanup, outline repair, shrinking, background and shadow
removal, the alpha cutoff, hand edits and sheet frames. Copy it from the window (**Copy Recipe**), or
read it from a tool result. Unknown keys are refused, so a typo never does nothing silently.

## Craft lints

The lints measure what makes pixel art read badly: jaggies, doubles, holes, orphans, near-duplicate
colours, banding, pillow shading, low contrast against the backdrops, flicker between frames, off-palette
pixels and partial alpha. Each returns PASS, WARN, FAIL or SKIP with the measured value, its threshold and
the pixels. Craft lints only warn: they measure taste. Rule lints fail: the art broke a stated rule. Don't
raise a threshold to pass; fix the art.

## The command line

`jasperengine-editor fix-pixel-art <in> [--recipe r.json | --rerun] [--palette p] [--grid auto|<w>[x<h>][+<ox>,<oy>]] [--sheet auto|off] [--format png|aseprite] [--out path] [--overwrite] [--force] [--dry-run] [--image out.png] [--project dir]`

`jasperengine-editor lint-pixel-art <in>... [--palette p] [--lints a,b] [--rules r.json] [--backdrops #hex,...] [--fail-on fail|warn] [--image out.png]`

Output is JSON on stdout (`--format text` for people). Exit codes: 0 clean or written, 2 usage, 3 fixable,
4 needs a person, 1 could not run.

`check --pixel-art` reports stale cleanup outputs and written-with-force outputs as warnings, and outputs
with failing lints as errors.
