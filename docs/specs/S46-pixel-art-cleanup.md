# S46 — Pixel Art Cleanup: grid recovery, palette snapping, sheet slicing, craft lints and verdicts

**Wave / Depends on.** Pixel-art family (spec-first), wave 5C. Depends on **S43** (the `Palette`
asset, the OKLab and CIEDE2000 colour maths, the palette presets) and **S44** (`PixelImage`, the
`.aseprite` writer, the Sprite Preview panel, `PixelProblem.h` and the lint result types of
`lint/LintTypes.h`), both wave 5A; and on wave 5B's **S42** (the `pixel_art.backdrops` setting and
`PixelArtCheck::registerRule`, which this spec's `cleanup_outputs` rule uses) and **S48** (the
`off_palette`, `partial_alpha` and `flicker` lint functions in `editor/pixelart/lint/EffectLints`,
which S48 needs first and so owns). S47 (same wave) depends on this spec's lint library
(`lint()` and the catalogue, M1), which lands before S47's lint step. Owner area:
`editor/pixelart/cleanup/` (the port of ritmo's fixer and the session layer),
`editor/pixelart/lint/` (the craft lints, shared), `editor/window/PixelArtCleanupWindow.*`,
`editor/mcp/tools/PixelArtCleanupTools.cpp`, the `fix-pixel-art` and `lint-pixel-art` CLI
subcommands, the `import.pixelCleanup` texture import step, `docs/pixel-art-cleanup.md` and
`docs/mcp/guide/pixel-art-cleanup.md`.

## 1. Goal

Most "pixel art" that reaches a game is not clean: an AI image of fake pixels four to twelve
screen pixels wide, blurred, off-grid and in 3,000 colours; a sheet of frames on a painted grid
with labels; a resized screenshot; or real pixel art with a few stray colours and soft alpha.
S46 recovers the real pixels from such pictures, exactly when it can and never with false
confidence: a full C++ port of ritmo's pixel fixer (grid detection, DP grid-line placement,
vote-based palette snapping, speck cleanup, outline repair, stroke-preserving shrink, lost colours,
background removal and sheet slicing). It adds a shared library of **craft lints** (jaggies, doubles,
holes, flicker, contrast, orphans, palette, banding, pillow shading), each returning PASS, WARN or
FAIL with the values and the pixels. Every run ends in a verdict, `clean`, `fixable` or
`needs_person`, with its confidence. The recipe is stored in the output, so a run can be repeated
exactly. People get a Pixel Art Cleanup window with a split compare view and draggable grid lines;
agents get `fix_pixel_art` and `lint_pixel_art`; shells and CI get `fix-pixel-art` and
`lint-pixel-art`.

## 2. Scope

**In scope**
- A C++20 port of every module of `ritmo/packages/tools/src/pixel/core/` that the fixer, the
  sheet pipeline and the report use (§4.1), with ritmo's constants and order of steps, plus the
  craft rules of `ritmo/packages/tools/src/fx/core/craft.ts` and `check.ts`.
- The recipe (JSON, versioned), stored in a PNG output's `jasper.pixelcleanup` iTXt chunk or in an
  `.aseprite` output's `.meta`; re-running from an output; a stale badge when the source changed.
- Sheet and animation outputs: `.aseprite` with one tag per animation through S44's writer, or a
  PNG sheet plus Aseprite-style JSON.
- `editor/pixelart/lint/`: the lint API (`lint()`, `LintInput`, `LintOptions`) and the catalogue
  (§5, R24–R27), used by S46, S47 and S42; the result types are S44's `LintTypes.h`, and three of
  the catalogue's lints are S48's functions (R26).
- The Pixel Art Cleanup window (Window > 2D > Pixel Art Cleanup), the Project window entry, the
  texture import step (Inspector > Import Settings > Pixel Art Cleanup).
- MCP `fix_pixel_art` and `lint_pixel_art` (group `pixel_art`), CLI `fix-pixel-art` and
  `lint-pixel-art`, and the `cleanup_outputs` rule of S42's validator (`check_pixel_art`,
  `check --pixel-art`).
- Synthetic tests: the port of `ritmo/packages/tools/test/synthetic.ts` and
  `src/pixel/core/demo.ts`, which turn known sprites into fake AI art and check that they come back
  exactly.

**Out of scope**
- Drawing or painting. Aseprite is the people's tool, and agents draw with S47's `edit_sprite`.
  The window's hand edits (erase, restore, paint one pixel or one area, R16) only correct a
  cleanup result, as ritmo's do; they are not a canvas.
- AI or hosted image generation (owner decision, 2026-11-04). S46 cleans pictures that already exist.
- ritmo's page UI code (`src/pixel/ui/*`, a DOM app), its GIF/ZIP/SVG exporters (`gif.ts`,
  `zip.ts`, `export.ts` `toSvg`), its `.tt`-style JSON maps (`sprite.ts` `parseSprite`), and its
  Effect CLI framework. We take their behaviour, not their code.
- The palette asset, palette editing and the colour maths themselves (S43); the `.aseprite` codec
  (S44); atlas packing (S45); the project's pixel-art rules and `check_pixel_art` (S42, which calls
  these lints); effect lints such as spill, loop seams and one-shots (S48).
- Anything at runtime: cleanup runs in the editor, and games receive only the cooked pixels.

## 3. Current state (specs/pixel-art 1b1c34fbd, read)

- **No pixel-art code exists.** `editor/pixelart/` does not exist. There is no `PixelImage`,
  `Palette` class or CIEDE2000 code anywhere. OKLab exists twice, privately:
  `spritenormals::detail::oklab` (`editor/spritenormals/SpriteNormalsField.h:69-70`) and the
  CSS colour code (`engine/core/htmlui/compat/css/ColorValue.h:26`). S43 makes the shared one.
- **The closest precedent is Sprite Normal Map** (`editor/spritenormals/`):
  - One session layer serves the window, MCP and CLI (`SpriteNormalsSession.h:6-19`): `Spec`
    (:73), `prepare`/`preparePlan`/`prepareLoad` (:166-175), `writeOutputsUndoable` (:204, one
    project-history step through `ProjectFileWrites::writeUndoable`, `SpriteNormalsSession.cpp:924`),
    `writeOutputsDirect` (:210, CLI), `checkStaleness` (:237), and the `SpriteNormalsRun` job (:278).
  - Preview first, then write: with `output` omitted or false the call is a preview
    (`SpriteNormalsTools.cpp:356-407`). The contact sheet goes back as a `ToolResult` image (:985,
    :1356), and the write happens at :856. Typed problems `{kind, field, message, fix}` are
    `spritenormals::Problem` (`SpriteNormals.h:155-160`) and `problemJson`
    (`SpriteNormalsTools.cpp:125-130`). They are local to that tool; S44 lifts them into
    `editor/pixelart/PixelProblem.h` for the family.
  - Recipes: `jasper.spritenormals` iTXt (`SpriteNormalsRecipe.h:25`), spliced by
    `texsynth::insertRecipeChunk` (`editor/texsynth/SynthesisRecipe.h:104`, right after IHDR,
    replacing a chunk of the same keyword) and read by `texsynth::readRecipeTextFromPng` (:135).
    `pngchunks::stripEditorChunks` (`editor/util/PngEditorChunks.h:20`) drops every `jasper.*` text
    chunk at export (`Exporter.cpp:2937`), so a recipe never ships.
  - Window: the Split view with a draggable divider (`SpriteNormalsWindow.cpp:3741-3755`, :3899),
    integer fit zoom (:3686), zoom steps (:62), NEAREST magnification (:2105-2106), and paint
    undo local to the window until Save (`SpriteNormalsWindow.h:430-433`; `App.cpp:1392-1403`
    sends Ctrl+Z to the window first). It is registered at `App.cpp:201`, :711, :973-975 and :2085,
    in `shortcuts/DefaultCommands.cpp:230`, and in `AppSettings.cpp:476` and :785.
  - Its history step is named "Write File X" (`WriteFilesCmd.cpp:237-246`), although the header
    comment (`SpriteNormalsSession.h:202-203`) says "Generate Normal Map". S46 names its steps
    with `WriteFilesCmd::setName` (`WriteFilesCmd.h:52`), R33.
- **Image I/O.** `SpriteMedia::loadSource` (`editor/spritenormals/SpriteMedia.h`) loads stills,
  GIF (own LZW, disposal, `SpriteMedia.cpp:461`) and APNG (:693), up to 64 MiB and 256 frames. The
  pixels live in `texsynth::Image` (`editor/texsynth/SynthImage.h:22`). S44's `PixelImage` replaces
  that for the family.
- **Import settings.** `TextureImportSettings` (`editor/texture/TextureImportSettings.h:120`) is the
  `import.textureImporter` section of a `.meta` (:155), and it keeps unknown keys (`unknown`, :146).
  `SetAssetMetaCmd` (`editor/command/type/SetAssetMetaCmd.h:23`) sets one dotted key as a
  project-history step and tells the cook queue through its write listener (:47). There is no
  pixel-art import step.
- **MCP.** There is no `pixel_art` group (`editor/mcp/McpToolGroups.cpp:16-37`). `McpSchema.h` has
  no `anyOf` builder: tools write it by hand (`SpriteNormalsTools.cpp:1591`). Jobs use
  `McpJobs::create` plus `replyWhenJobDone` (`McpJobs.h:97`, :117). One call becomes one undo step
  through `McpUndo::foldCall` (`McpUndo.h:34`, set up at `McpServer.cpp:1327-1349`).
- **Sprite and art tools** that exist: `generate_placeholder_art` (`AssetTools.cpp:4174`),
  `view_asset` (:3815), `set_sprite_frames` and `set_sprite_animation` (`WorldTools.cpp:5645`,
  :5668), and `sample_view_color` (`EditorToolsTools.cpp:605`). None of them can find a grid,
  snap to a palette or lint pixel art.
- **CLI.** The subcommands are dispatched in `editor/main.cpp` (`generate-sprite-normals` at
  :107-108, then `CommandLine.cpp:3714`, then `SpriteNormalsCli.cpp:1221`). Their exit codes are
  0 ok, 1 failed, 2 usage (`SpriteNormalsCli.h:21-22`). `check` (`CommandLine.cpp:3117`) has the
  categories `--lua --shaders --cpp --ui --textures --anims --vat --normals --videos --all` (usage
  :2116-2164).
- **What an agent can do today with an AI-made sprite:** import it as-is. It then renders blurred
  (sprites cook lossy, digest "Textures"), at a fake pixel size, with hundreds of colours and no
  frames. `TextureSlicerToolDialog` (`editor/window/dialog/TextureSlicerToolDialog.h`) cuts only a
  regular grid by hand.

## 4. Reference

### 4.1 ritmo (`/tmp/pixelart-research/ritmo`, MIT, © 2026 Sandro Maglione): what is ported

Every file is copied into C++ under `editor/pixelart/cleanup/`. Each ported file keeps ritmo's MIT
header, and `NOTICE.md` gets one ritmo entry. `outline.ts` and `reduce.ts` follow ToPixel
(daisuqe/ToPixel), which their headers credit, so the NOTICE entry names ToPixel too.

| ritmo file | C++ (`editor/pixelart/cleanup/`) | What it is | Change |
|---|---|---|---|
| `pixel/core/grid.ts` (621) | `CleanupGrid.{h,cpp}` | `edgeProfiles`, `detectAxis` (spectrum, multiples, offset refinement, mid-cell residual, square preference), `detectGrid`, `uniformCuts`, `refineGrid`/`placeCuts` (DP) /`bendCuts`, `crossingProfile`, `fitOffsets` | Doubles everywhere. Loops run in a fixed order. The spectrum and the candidate sizes run as an S06 `ParallelFor` over candidates, reduced in candidate order (R40) |
| `pixel/core/fix.ts` (167) | `CleanupSample.{h,cpp}` | `sampleCells` (inset), `quantize` (votes, median, Bayer 4x4 dither), `usedColors` | Distances come from S43 (`pixelart::ColorMetric`, `PaletteMatcher`) |
| `pixel/core/color.ts` (203) | none: S43 | OKLab, CIELAB D65, CIEDE2000, matcher cache | S46 calls S43's `PixelColor.h` (`toOklab`, `ciede2000`, `PaletteMatcher::nearest`, R8). ritmo's own k-means (`extractPalette`) is kept for its tested behaviour; S43's `medianCut`/`kMeansRefine` serve S43's Reduce Colours |
| `pixel/core/palette.ts` (289) | `CleanupPalette.{h,cpp}` | `proposePalette` (dominant, balanced, distinct), `extractPalette` (seeded k-means++) | The presets and the `.gpl`/`.hex` I/O are S43's |
| `pixel/core/cleanup.ts` (114) | `CleanupSpecks.{h,cpp}` | Speck merging with doubt, locked and kept specks | none |
| `pixel/core/outline.ts` (242), `reduce.ts` (248) | `CleanupOutline.{h,cpp}`, `CleanupReduce.{h,cpp}` | Dark threshold from a histogram valley, `hasOutlines`, `thinOutlines`; shrink modes, stroke marking, thinning, `smoothStairs` | none |
| `pixel/core/lost.ts` (66) | `CleanupLost.{h,cpp}` | Lost colours | none |
| `pixel/core/background.ts` (213) | `CleanupBackground.{h,cpp}` | Border colours, flood, enclosed, specks, drop shadow | none |
| `pixel/core/edits.ts` (81) | `CleanupEdits.{h,cpp}` | Erase, restore and paint one pixel or one area | none |
| `pixel/core/pipeline.ts` (228) | `CleanupPipeline.{h,cpp}` | `runWithSpec` in ritmo's order, the cache | The cache lives in the session |
| `pixel/core/sheet.ts` (874), `animation.ts` (300) | `CleanupSheet.{h,cpp}`, `CleanupAnimation.{h,cpp}` | Sheet background, painted grid lines, frame boxes, label rows, groups, anchors, `layOut`, `runSheet`, `sharedCell` | none |
| `pixel/core/recipe.ts` (363) | `CleanupRecipe.{h,cpp}` | The recipe and its decoder with per-key errors | snake_case keys, a format id, the source hash, palette by asset (R28) |
| `pixel/core/report.ts` (387) | `CleanupReport.{h,cpp}` | Verdicts, `gridFit`, `isNative`, `reportOfFix`, `reportOfSheet`, `compareReports` | `needs-person` becomes `needs_person`, plus the extra guards of R29 |
| `pixel/core/lint.ts` (176), `fx/core/craft.ts` (269), `fx/core/check.ts` (craft part) | `editor/pixelart/lint/*` | The rules and craft lints | One result shape for every lint, plus banding and pillow shading (new) |
| `pixel/core/spritesheet.ts` (142) | `CleanupSheetOutput.{h,cpp}` | Aseprite json-hash atlas | `.aseprite` output through S44 |
| `pixel/core/project.ts` (130) | none: `texsynth::insertRecipeChunk` | The recipe in an iTXt chunk | Our keyword, `jasper.pixelcleanup`, which the exporter strips |
| `pixel/core/demo.ts` (150), `test/synthetic.ts` | `CleanupSynthetic.{h,cpp}` (editor library, also the window's "Try a Demo") | Seeded LCG, `upscale` (scale, offset, drift, blur, noise), `makeSprite`, `pad`, `place`, `fakeSheet`, `warped` | The same generator, so the ported tests keep their numbers |
| `test/core.test.ts`, `outline.test.ts`, `cleanup.test.ts`, `animation.test.ts` | `editor/tests/pixelart/Cleanup*Test.cpp` | The recovery cases (AC1–AC8) | Ported one for one |

Two things we do better than ritmo:
- ritmo stores the recipe on the **source** picture (`project.ts:1-9`). We store it on the
  **output** together with the source's path and SHA-256 (R28). The art in the game then says how
  it was made, and the output can be rebuilt from its source.
- ritmo's report has only two thresholds (`CONFIDENT`, `report.ts`). We add independent guards: a
  share of lost colours, an empty or unreadable frame, the shrink distortion, and a refusal to
  write `needs_person` without `force` (R29, R30).

### 4.2 Other sources

- SpriteCook skills (`/tmp/pixelart-research/x-post/spritecook-skills`, MIT): the alpha cutoff
  `alpha = a >= cutoff ? 255 : 0`, previewed at integer scales (R14).
- pixelfx (`/tmp/pixelart-research/pixelfx/tools/check.mjs`): its lint report shape (code, level,
  message, at) informs the shared result (R25). Its effect lints themselves are S48's.

## 5. Requirements

### Functional — the pipeline (port of ritmo)

- **R1. Input.** A source is:
  - an image file that `SpriteMedia::loadSource` reads (PNG, JPEG, BMP, TGA, PSD, GIF, APNG;
    animated ones as frames, R22);
  - an `.aseprite`/`.ase` file (S44 reader; the composite of the chosen frames, or of one `layer`);
  - or a Sprite GameObject (its texture, and its frames when it has more than one).

  The source becomes S44's `PixelImage` (RGBA8). The size limit is 4,096 × 4,096 and 256 frames.
  A bigger source fails with `image_too_large`, and its `fix` names the limit. The source file is
  never modified (R31).
- **R2. Grid choice** (`recipe.grid`):
  - `auto` (default): R3.
  - `cells {cell_width, cell_height?, offset_x, offset_y}`: a given grid.
  - `size {width, height?}`: pixelate a picture that is not pixel art into that many pixels.
    `height` keeps the aspect ratio when it is omitted.

  Cuts follow `uniformCuts` (`grid.ts`): a partial cell at either end stays only when it is at
  least half a cell wide, and a picture smaller than one cell is one cell.
- **R3. Grid detection** (`detectAxis`, `detectGrid`, ported exactly):
  1. Edge profiles are the summed absolute RGBA differences across each column and row boundary,
     averaged along the boundary.
  2. The active span is where the profile is ≥ 2 % of its peak. Values are capped at the 90th
     percentile, centred, and faded by sin².
  3. The spectrum is sampled at 4 samples per bin, from the largest cell (≤ `max_cell`, default
     128, ≤ length/2) down to 2. Peaks ≥ 25 % of the highest are kept, at most 12, plus their whole
     multiples ×1–×4.
  4. Each candidate is refined: the offset coarsely in 0.25 px steps on the [1,2,1]-smoothed
     profile, then the size within ±max(0.05, 3 %) in reach/40 steps with the offset within ±0.75
     in 0.05 px steps.
  5. Each candidate is judged by its mid-cell edge residual `edgesInside` (the middle half of
     every cell; an edge is a step > max(24, 3 × the median step)).
  6. Among the candidates with residual ≤ the flattest + 0.04, the one with the strongest lines
     wins. When a preferred size is given, any candidate within 10 % of it with residual ≤ the
     flattest + 0.12 is preferred.
  7. The cell is rounded to 0.001 px and the offset to 0.01 px. Confidence =
     clamp(min(1 − residual, 1 − inside/lines), 0, 1).
  8. Cells are square. When the axes differ by more than 2 %, the less sure axis is searched again
     with the surer one's size as the preferred size, and the new result is kept when it is within
     10 %.
  9. A picture with no edges at all gives no grid.
- **R4. Grid-line placement** (`recipe.follow`: `off` | `straight` | `bend`):
  - `straight` moves every inner cut with `placeCuts`, a dynamic program over candidates within
    half a cell, in steps of max(0.25, spacing/40):
    - gain at a cut = min(smoothed edge / mean, 6) − 4 × `crossingProfile`;
    - spacing cost = 20 × ((gap − spacing)/spacing)²; a gap below half a cell is forbidden;
    - border cuts never move; a sliver left at either end joins its neighbour; positions are
      rounded to 0.01.
  - `bend` places the cuts per band of cells, using the band's profile plus `BEND_SPAN` = 1
    neighbouring band on each side. `xs`/`ys` are each line's mean, and `bend.xs[i][j]` gives cut
    i in row j.
  - Lines dragged by hand (`recipe.moved: [["x:3", 41.5], ...]`, source pixels) are applied last.
    A bent line moves as a whole.
- **R5. Sampling** (`sampleCells`): each cell is read from its middle, inset by `sample.inset`
  (0–0.45, default 0.25) on each side, always at least the centre pixel. A pixel with alpha
  < `sample.alpha_threshold` (default 128) counts as transparent. A cell with more transparent
  pixels than opaque ones is transparent.
- **R6. Native pixel art** (`isNative`). With `grid: auto`, a picture counts as pixel art at its own
  size when one of these holds:
  - no grid is found;
  - the cell is below 1.5 px;
  - the detection confidence is below 0.3;
  - it is unsure (confidence < 0.5 or fit < 0.9) and has ≤ 256 colours.

  It is then read one pixel per cell (inset 0), and R8–R17 still run. This is the cleanup of
  existing pixel art: palette snapping, alpha, background, specks.
- **R7. Colour per cell without a palette:** the per-channel median of the cell's opaque pixels.
  The palette is the unique cell colours in the order first met (`quantize`).
- **R8. Palette snap by vote:**
  - Each opaque pixel a cell read votes for its nearest palette colour under `recipe.metric`
    (`oklab`, the default: Euclidean in OKLab; or `ciede2000`). Both come from S43's
    `PaletteMatcher::nearest` (`engine/core/pixelart/PixelColor.h`, `ColorMetric::OkLab` and
    `ColorMetric::Ciede2000`).
  - The colour with the most votes wins. A tie goes to the lowest palette index, so no colour is
    ever made up and runs are deterministic.
  - `dither: ordered` instead snaps the cell's median, pushed by the Bayer 4×4 threshold ×
    `dither_strength` × 64.
- **R9. Palettes** (`recipe.palette`; default: the project's Default Palette, `palettes.default`
  (S43 R4b), when one is set, else `auto`; the resolved recipe stores the palette's colours, R28, so
  a re-run never depends on the setting):
  - `auto {count 2–256 (default 16), method dominant|balanced|distinct}`: `proposePalette`.
    - Dominant merges colours closer than 0.04.
    - Balanced is k-means++ in OKLab, mulberry32 with `recipe.seed` (default 1), at most 24
      iterations, extracted colours darkest first.
    - Distinct is farthest-point, ignoring colours on fewer than 0.2 % of the cells.
  - An S43 `.palette` asset (path or GUID), an S43 preset name, or a list of `#rrggbb`.
  - `locked: [indices]`: those colours stay exact in k-means and are never merged away (R10). The
    window and the agent lock the colours a fix must keep, such as the eyes.
- **R10. Specks** (`cleanup {size 0–16, strength 0–1}`, `cleanUp`):
  - A speck is a group of ≤ `size` same-coloured pixels, 8-connected, so a one-pixel diagonal
    outline is never a speck.
  - It merges into the colour it touches most when doubt ≥ 1 − `strength`. Doubt is the max of
    the vote ratio of its own cells and `1 − ΔOKLab/0.3`.
  - Locked colours and specks kept by hand (`clean_keep`, recorded with the grid size they were
    made at) are never merged.
  - The defaults follow ritmo: size 0 (off), strength 0.5. The guide recommends `{size: 2}` for AI art.
- **R11. Outline repair** (`outlines: auto|on|off`):
  - The dark threshold is a histogram valley. Start past the darkest 5 % of opaque pixels; search
    from that +8 to min(that +60, 120), default min(that +30, 100); the cut goes in the middle of
    an equally empty run. Outline pixels are opaque pixels with luminance at or below it.
  - `auto` turns repair on when the median dark run is 0.6–1.5 cells over at least 40 runs
    (`hasOutlines`), and never for cells under 2 px.
  - With repair on:
    - grid placement avoids splitting dark strokes (R4 `crossingProfile`);
    - `thinOutlines` gives a one-cell stroke split into two pixels back to the pixel holding more
      of it. Strokes wider than 1.4 cells, stroke ends, pixels whose removal would break the
      outline (the 8-ring stays one group) and T or corner joins (`sameSides`) stay.
- **R12. Shrink** (`target_width`, `reduce_mode: outlines|common`, `reduce`): make the fixed
  picture smaller by any ratio.
  - `common`: each new pixel takes the colour covering most of its block.
  - `outlines`:
    - Strokes are marked: the block holding each linked outline pixel's centre, and the
      straight path between those of two touching outline pixels.
    - Blocks with ≥ 60 % outline cover are always marked.
    - The marks are thinned, least-covered first, for up to 4 passes, without breaking a stroke
      or opening a hole.
    - Colours come from the block, or from a neighbour when the block has none.
    - `smoothStairs` turns one-pixel steps into diagonals, for up to 4 passes.
  - A lone dark pixel never grows into a block.
- **R13. Lost colours** (`lostColors`):
  - A cell is lost when its own median colour is more than `lost_threshold` (default 0.06 OKLab)
    from the palette colour it became.
  - Lost cells are grouped within 0.06 of a running centre. Groups under 2 cells are dropped, and
    background cells are skipped.
  - Each lost colour reports `color`, `became`, `pixels` and up to 20 cell locations, most cells
    first. Adding it to the palette (window button, or `palette.add`) brings its details back
    wherever it is nearest.
- **R14. Alpha cutoff:** `alpha_cutoff` (0–255, default the sample threshold 128) makes every output
  pixel either fully opaque or fully transparent. Native art is cut the same way, and the result
  reports how many partial-alpha pixels the source had.
- **R15. Background and shadow removal** (`remove_background`, `background {tolerance 0–0.3
  (0.08), enclosed (false), specks (true), shadow (false)}`, `background.ts`):
  - It runs on the cells' own colours, not the palette's (`pipeline.ts`: a palette with no
    background colour would otherwise take the outline with it).
  - The background colours are the most common border colour plus up to two others ≥ 40 % as
    common, merged within 0.04 (a fake checkerboard has two). The flood goes inward from the
    border through colours within `tolerance`.
  - `enclosed` also clears matching areas the flood can't reach.
  - `specks` clears 8-connected groups of ≤ 3 pixels left alone in the background.
  - `shadow` clears a drop shadow: a group of ≥ 3 pixels that touches the background, lies in
    the bottom half of the subject and is at least as wide as tall, in a colour within 0.035 a/b
    of a background colour and 0.03–0.4 darker.
- **R16. Hand edits** (`edits {size, strokes}`, `applyEdits`): erase, restore and paint one pixel,
  or its 4-connected area, on the fixed picture, applied last. A painted colour snaps to the
  nearest palette colour. Edits recorded at another output size are dropped with
  `warnings: ["edits_stale"]`.
- **R17. Crop** (`crop`): export only the box around the opaque pixels.
- **R18. Order and cache:**
  1. grid;
  2. outline mask;
  3. lines;
  4. sampling;
  5. snap;
  6. thin outlines;
  7. own colours, lost colours;
  8. specks;
  9. shrink;
  10. background;
  11. edits;
  12. alpha cutoff;
  13. crop.

  This is `runWithSpec`'s order with the cutoff added. The session caches the outline mask, the
  base grid (keyed by spec, follow and outlines) and the samples (keyed by grid and sample
  options), so a slider change re-runs only what it affects (R39).

### Functional — sheets and animations

- **R19. Sheet detection** (`recipe.sheet`: `auto` (default) | `off` | an object of frames and
  animations). `auto` slices when `looksAnimated` holds on the found boxes: at least 2 boxes, and
  ≥ 75 % of them alike (mass 0.2–5 × the median, width 0.4–2.5 × the median).
- **R20. Slicing** (`sheet.ts`), in order:
  1. Sheet background colours: covering ≥ 3 % of the border, merged within 0.06. `fillNoise`
     is the 95th percentile spread.
  2. `removeGridLines` paints out painted grid lines over the background only, never over the
     art. A line is a colour covering ≥ 25 % of a row or column in runs ≤ max(4, 1 %) thick, and
     its sides are checked `LINE_SIDE` = 2 px out.
  3. `clearSheet` floods the background from the border.
  4. `findFrames`:
     - bands of rows with art; a band shorter than 40 % of the tallest is a label row and is
       dropped;
     - a band is split at its widest gaps (down to the largest drop in width, ratio > 1.6; gaps
       below max(2, 1 % of the width) never split);
     - boxes are re-split into stacked rows up to 3 levels deep;
     - boxes reach halfway into the gaps around them, so loose sparks stay in their frame.
  5. `groupFrames`: one animation per row, named `animation-1`, `animation-2`, …. A row spanning
     two side-by-side blocks is split where they meet.
- **R21. Per-frame fix** (`runSheet`):
  - The shared cell is the confidence-weighted median of every frame's two axes. Its confidence
    is the mean confidence of the frames within 3 % × their share. A given `cells` grid overrides it.
  - Every frame then gets its own offset (`fitOffsets`) and runs the same recipe, with no shrink,
    crop or hand edits.
  - One palette is shared (`sharePalette`).
  - Frames are placed on one canvas per animation by `anchor`:
    - `ground` (default) keeps each frame's drawn height, so hops stay;
    - `feet` puts the bottom of the body on one row;
    - `center` puts the body's middle on one spot;
    - `box` keeps the boxes as they are.

    The body is the largest 4-connected opaque group. The canvas gets `padding` (default 1) and
    per-frame `nudge`s.
  - Frame boxes, `include` flags, animation names (`[A-Za-z0-9_.-]+`), `fps` (default 8) and
    `loop` (`forward` | `pingpong`) are recipe data. The window and the agent edit them.
- **R22. Animated sources** (GIF, APNG, multi-frame `.aseprite`, a Sprite's frames) skip slicing.
  Their frames go straight to R21 with their own delays, as one animation named after the file
  (or after each `.aseprite` tag).
- **R23. Sheet outputs** (`format`):
  - `aseprite` (the default for sheets):
    - one file through S44's writer;
    - indexed when the shared palette has ≤ 255 colours, with index 0 transparent; RGBA
      otherwise;
    - one layer `Cleanup` and one frame per placed frame; durations are `round(1000/fps)` ms, or
      the source delays;
    - one tag per included animation (direction `forward` or `pingpong`).
  - `png`:
    - one sheet, each animation on its own row and frames left to right with 1 px spacing
      (`atlasOf`);
    - plus `<stem>.json`, Aseprite's json-hash (`frames` keyed `<anim>-<n>` with `frame`,
      `duration`; `meta.frameTags`, `meta.size`, `meta.app: "jasperengine"`), which S45's packer
      and S44's importer may read.

  Single pictures default to `png`. `aseprite` is allowed for one frame too: the file a person
  then opens in Aseprite to finish.

### Functional — lints (`editor/pixelart/lint/`, shared with S47 and S48)

- **R24. The lint API** (§6.2): `lint(const LintInput&, const LintOptions&) → LintReport`, with
  `LintReport`/`LintResult` from S44's `editor/pixelart/lint/LintTypes.h` (the family's one result
  shape, so S48's and S49's results are the same objects).
  - The input is one or more frames of S44 `PixelImage`, plus optionally a palette (S43), the
    backdrop colours, `loop`, and rules (`max_colors`, `sizes`, `transparent_background`).
  - The function is pure and deterministic. It runs on any thread, allocates nothing per pixel
    beyond its scratch, and reads the S43 OKLab cache.
- **R25. One result shape** for every lint:
  - `{id, status: PASS|WARN|FAIL|SKIP, measured, threshold, threshold_source, count, at:
    [[frame, x, y], …], message, fix}`;
  - `at` holds at most 8 spots (`SHOWN`, `craft.ts`) and `count` holds all of them. Frames are
    numbered from 1, as Aseprite shows them and as S47 addresses them, and x, y from the top-left
    pixel, from 0;
  - `threshold_source` is `default`, `rules.<key>`, `options.<key>`, or the project setting
    named;
  - `measured` is a number (count, share or distance);
  - SKIP says why ("no palette given", "fewer than 3 frames").
  - A lint never changes pixels.
- **R26. The catalogue.** The ids are snake_case and the defaults are ritmo's.

  | id | rule | status |
  |---|---|---|
  | `off_palette` (S48's function, S48 D13) | every opaque pixel's RGB is a palette colour exactly (`lint.ts` off-palette) | FAIL when any pixel is off; `measured` = off pixels, `message` lists ≤ 8 colours; SKIP without a palette |
  | `colors` | used colours ≤ `rules.max_colors` | FAIL above; SKIP without the rule |
  | `partial_alpha` (S48's function) | no pixel with 0 < alpha < 255 | FAIL |
  | `transparent_background` | ≥ 3 corners opaque and > half the border opaque (`lint.ts`) | FAIL when `rules.transparent_background` is set (default on for `fix_pixel_art` outputs with `remove_background`) |
  | `size` | the frame size is one of `rules.sizes` | FAIL; SKIP without the rule |
  | `near_duplicates` | pairs of used colours closer than 0.03 OKLab | WARN |
  | `orphans` | a pixel none of whose 4 neighbours shares its colour, while ≥ 5 of its 8 share another (`lint.ts` stray-pixel) | WARN when ≥ 1; SKIP on animation sheets, where sparks are single pixels on purpose (`report.ts`) |
  | `jaggies` | along every edge chain (4 orientations), a run ≥ 2 shorter or longer than both neighbours (3,1,3 / 1,3,1); a chain's end runs are not judged (`craft.ts`) | WARN at ≥ 3 spots (`CRAFT_SPOTS`) |
  | `doubles` | a lit pixel of a shape of ≥ 5 pixels whose only two 8-neighbours sit at a right angle (`craft.ts`) | WARN at ≥ 3 spots |
  | `holes` | an empty pixel with drawn pixels on all four sides | WARN at ≥ 3 spots |
  | `flicker` (S48's function) | a pixel that changes for exactly one frame and changes back; a loop wraps | WARN when the share of lit pixels > 0.06; SKIP under 3 frames |
  | `contrast` | per backdrop, the share of drawn pixels within 0.12 OKLab of it (`craft.ts` `faintOn`) | WARN when the share > 0.5 on any backdrop; SKIP without backdrops |
  | `banding` | R27 | WARN |
  | `pillow_shading` | R27 | WARN; SKIP when the shape is too small or has too few tones |
  | `empty_frame`, `one_frame`, `frame_scale` | sheets: a frame with nothing in it; an animation of one frame; a frame whose own cell differs from the shared one by > 5 % (`report.ts`) | FAIL, WARN, WARN |

  - The craft lints (jaggies to pillow shading) never FAIL. They are a person's taste measured,
    and a WARN with spots is the right strength.
  - The rule lints FAIL, because the art breaks a stated rule.
  - The backdrops are `options.backdrops`, else S42's project setting `pixel_art.backdrops`
    (S42 §6.2), else the main scene's 2D camera clear colour, else SKIP.
  - The three lints marked "S48's function" are defined by this table but implemented in S48's
    `editor/pixelart/lint/EffectLints` (S48 lands first and needs them); `lint()` calls them as
    they are, so every caller gets one implementation.
  - Each `fix` names the tool action that addresses it. For example `off_palette` says "fix_pixel_art
    with palette: <the palette>", and `doubles` says "edit_sprite: remove the corner pixel at x,y
    (S47)".
- **R27. The two new lints, defined:**
  - **`banding`**: parallel tone steps that line up into visible stripes.
    - A *ramp run* is a sequence along one scanline (row, column or one of the two diagonals) of
      ≥ 3 runs of distinct opaque colours. The colours must be strictly monotone in OKLab
      lightness, and every run must be the same length ≤ 2.
    - A *band group* is ramp runs on ≥ 3 consecutive scanlines whose colour sequence is the same
      and whose boundary positions shift by the same constant (0 or ±1) from line to line.
    - `measured` is the pixels in band groups. WARN when it is ≥ 12, with the first pixel of each
      group in `at`.
    - Dithered ramps (alternating colours) and anti-aliasing at single corners are not ramp runs,
      because their runs are not monotone or not ≥ 3 lines long.
  - **`pillow_shading`**: lightness that follows the silhouette inward from every side instead of
    a light direction. It is measured on each opaque body (R21's body rule) of ≥ 64 pixels with
    ≥ 3 distinct lightness levels; otherwise SKIP.
    - d(p) is the 4-connected distance of each pixel to the body's outside.
    - ρ is Spearman's rank correlation between OKLab L and d.
    - R² comes from the least-squares plane L ≈ a + b·x + c·y.
    - `balance` is the spread of the mean L of the edge pixels (d = 1) in the four quadrants
      around the centroid.
    - Pillow when ρ ≥ 0.6, R² ≤ 0.25 and `balance` ≤ 0.03. `measured` = ρ, and `at` holds the
      centroid and the extreme rings.
    - A shape lit from one side gives a high R² or a large balance, so it passes. A flat shape is
      SKIP.

### Functional — verdicts, recipes, outputs

- **R28. Recipe** (`jasperengine.pixel-cleanup-recipe`, version 1).
  - It is canonical JSON with sorted keys and floats rounded to 6 decimals.
  - It holds every key of ritmo's `Recipe` in snake_case: `grid`, `follow`, `outlines`, `moved`,
    `sample`, `palette`, `metric`, `dither`, `dither_strength`, `cleanup`, `clean_keep`,
    `target_width`, `reduce_mode`, `remove_background`, `crop`, `background`, `lost_threshold`,
    `edits`, `sheet`.
  - It adds `alpha_cutoff`, `seed`, `source {path, sha256, frame?, layer?}`, `result_sha256`
    (the SHA-256 of the output's decoded RGBA pixels, so an output edited afterwards, by a person
    or by S47, is recognised), `palette.asset`
    (GUID and path, when an S43 asset is used; its colours are stored too, so a re-run without the
    asset still works), `format`, `engine_version` and `pipeline_version`.
  - The decoder (`decodeRecipe`) accepts partial recipes. Missing keys take their defaults, and
    `palette` may be a preset name or a list of hex colours. Every key error is collected and
    returned at once, as `{field, message, fix}` with the valid values.
  - Unknown keys fail with the list of valid keys, so a typo never silently does nothing. A
    version-2 recipe opened by a version-1 editor fails with `recipe_version`.
- **R29. Verdict and confidence** (`report.ts`, made stricter):
  - `clean`: native art (R6) with no FAIL lint, and with no WARN when `fail_on: warn`.
  - `fixable`: the fix is confident and the output has no FAIL lint.
    - Confident means detection confidence ≥ 0.5 (1 for a given grid) and `gridFit` ≥ 0.9.
      `gridFit` is the share of the read pixels within 0.06 OKLab of their cell's median.
    - For native art, `fixable` means its FAILs are ones the pipeline removes: `off_palette`,
      `partial_alpha`, `transparent_background`, `colors`.
  - `needs_person` in every other case. It also applies, whatever the confidence, when any of
    these guards trips:
    - the lost colours cover > 5 % of the opaque cells;
    - a sheet frame came out empty;
    - the shared cell's confidence is < 0.5;
    - a FAIL lint the pipeline does not remove (`size`, `empty_frame`) remains;
    - no grid was found on a picture that is not native.
  - `confidence` = min(detection confidence, fit). Each guard that trips is listed in
    `guards[]` with its value and threshold.
  - `size` pixelation (R2) has no true grid. It is `fixable` when no FAIL lint remains, and its
    summary says the look is a judgement call (`report.ts`).
- **R30. Never a confident bad fix.** A write of a `needs_person` result is refused
  (`needs_person`, `fix` naming what to set: the grid, the palette, or `force: true`). With
  `force`, the output is written, its recipe records `forced: true`, and `lint_pixel_art` reports
  `forced` on it. People in the window can always write: they are the person. The window still
  shows the verdict badge, and on `needs_person` it asks for confirmation.
- **R31. Outputs:**
  - Default paths sit beside the source: `<stem>_clean.png`, or `<stem>.aseprite` for sheets
    (`<stem>_clean.aseprite` when that exists). `output` may give a path or `{png, aseprite,
    json}`.
  - Outputs must be inside the assets directory, with the same containment rules as
    `generate_sprite_normals` (`outputsContained`), must not be the source, and need `overwrite`
    to replace a file.
  - A PNG carries the recipe in an iTXt chunk `jasper.pixelcleanup` right after IHDR
    (`texsynth::insertRecipeChunk`). The exporter strips it.
  - An `.aseprite` output carries it in its `.meta` (`pixelCleanup.recipe`), written in the same
    step.
  - The JSON atlas carries `meta.jasperengine.recipe`.
  - All files of one write go all-or-nothing (`texsynth::writeFilesAllOrNothing`) as one
    project-history step named "Pixel Art Cleanup (<file>)" (`WriteFilesCmd::setName`).
  - An `.aseprite` output, and its undo and redo, call `AsepriteImporter::reimport(path)` (S44
    R23), because S05's self-write suppression hides the editor's own writes from the watcher.
- **R32. Re-run and staleness:**
  - `fix_pixel_art {source: <an output>, rerun: true}`, the CLI's `--rerun` and the window's
    "Re-run Cleanup" read the stored recipe, load its source and run it again. With an unchanged
    source the result is byte-identical (R41).
  - When the source's SHA-256 differs from the recorded one, the result says
    `stale: {source_changed: true}`. The Project window shows a "Source changed" badge on the output
    (the same badge path as stale normal maps, `checkStaleness`).
  - A missing source fails with `source_missing`, naming the recorded path.
  - When the output's pixels no longer match `result_sha256` (it was edited after the cleanup), a
    re-run that would write over it fails with `output_edited`. Its `fix` offers a new output path
    or `force: true`, so a re-run never silently throws away later hand or agent edits.
- **R33. Import step** (`import.pixelCleanup: {enabled, recipe}` in a raster texture's `.meta`):
  - When enabled, the texture cooker decodes the source, runs the recipe (sheets included,
    producing the packed sheet) and prepares the fixed pixels instead of the source.
  - The cache key adds the recipe text and `pipeline_version`. The texture's logical size becomes
    the output size. The source file stays as it was.
  - If the source changed and the cook-time verdict is `needs_person`:
    - the raw source is cooked instead;
    - the Console logs a warning naming the file and the guard;
    - the Inspector badge shows "Cleanup needs a person".

    So a bad fix never ships silently.
  - `.aseprite` sources never take this step: they are already pixel art.
  - Old editors keep the key without using it, and cook the raw source.

### Platform & backends

- **R34. Editor-only.** All of S46 is editor code, CPU and single-process, on Linux, Windows and
  macOS. Nothing links into players. The cooked texture of R33 is an ordinary texture: it works
  on every backend and target, with no new shader. The recipe chunk is stripped at export
  (`stripEditorChunks`), and `tools/mcp/tests/test_export*.py` checks that it is gone.
- **R35. Portable code:** C++20 within `tools/check-cxx-subset.py`. No SIMD intrinsics (the
  compiler may vectorise).
  - Colour conversions and distances are S43's `PixelColor.h`: float `Lab`, with a fixed Newton
    `cbrt`. They are bit-identical across platforms (S43 R12).
  - The pipeline's own sums (profiles, the spectrum, the DP scores) are `double`, with
    `std::cos`/`std::sin`/`std::hypot` only.

### Performance (Release editor build, build server A, one core unless stated)

- **R36. Detection:** `detectGrid` ≤ 120 ms for 1024 × 1024 and ≤ 450 ms for 2048 × 2048 using
  the S06 job pool (8 workers). ritmo's 1022 px test case (48 × 21.3) has a budget of 5 s in
  JavaScript (`core.test.ts`).
- **R37. Whole fix:**
  - 1024 × 1024 single picture, 32-colour auto palette: ≤ 300 ms.
  - 2048 × 1024 sheet of 8 frames: ≤ 900 ms.
  - Past 1 s an MCP call becomes a job (`McpJobs`, `timeout_seconds` default 60), cancellable,
    and a cancelled run writes nothing.
- **R38. Lints:** all of them on 16 frames of 64 × 64 take ≤ 15 ms. `pillow_shading` and `banding`
  are each ≤ 5 ms per 128 × 128 frame. S47 runs lints after edits, so they must stay
  interactive.
- **R39. Interactive re-run** in the window, after one slider changes (the grid and samples
  cached): ≤ 33 ms for a 128 × 128 output. A run that will take longer goes to a background job,
  with the previous result shown until it finishes.

### Determinism

- **R40.** The same source bytes, recipe and engine build on one platform give byte-identical
  output files, JSON results and preview images, whatever the thread count.
  - Parallel loops reduce in index order.
  - Ties go to the lowest index or the earliest position.
  - The k-means seed is in the recipe.
  - The preview image carries no timestamp.

  Across platforms, results may differ only where the maths library differs by an ulp. AC2's
  recovery cases must match exactly on every platform CI runs (Linux on the servers;
  Windows and macOS in CI).
- **R41. Re-run equality:** an output written and then re-run (R32) is byte-identical, recipe
  chunk included. The recipe records no time.

### Editor UX (human)

- **R42. Window > 2D > Pixel Art Cleanup** (`PixelArtCleanupWindow`, registered as Sprite Normal
  Map is, §3). The empty state has "Open Image…", drag and drop from the Project window, and
  "Try a Demo", which builds a fake AI sprite with `CleanupSynthetic` and shows what the tool
  does in one click.
  - **Toolbar:**
    - the source;
    - Grid: Auto / Cells / Size, with cell width, height and offsets;
    - Follow: Off / Straight / Bend;
    - Outlines: Auto / On / Off;
    - Detect Again;
    - the verdict badge: green Clean, blue Fixable, amber Needs a Person, with the confidence
      and a tooltip listing the guards.
  - **Canvas:**
    - View: Source / Result / Compare. Compare is a split with a draggable divider labelled
      "Source | Result", the result upscaled onto the source's cells.
    - Integer fit zoom, the window's zoom steps, NEAREST, pan with the middle button or Space.
    - The grid is drawn over the source. With **Lines** on, a hovered line highlights and drags,
      writing `recipe.moved`. A bent line drags as a whole.
    - Lost cells and lint spots draw as outlines when their list row is hovered.
  - **Right panel tabs:**
    - **Palette:**
      - Auto (Dominant / Balanced / Distinct, Count), or a Palette asset picker (S43), or a
        preset;
      - one swatch per colour with a lock toggle and the share it covers;
      - Metric (OKLab / CIEDE2000), Dither and Strength.
    - **Lost Colors:** each row shows the swatch, "became" swatch, pixels and Add to Palette.
      Hovering a row highlights its cells, and clicking zooms to the first one.
    - **Cleanup:**
      - Specks (Size, Strength); Remove Background (Tolerance, Enclosed, Specks, Shadow);
      - Alpha Cutoff; Shrink (Target Width, Keep Outlines / Most Common Color); Crop;
      - the hand tools: Erase, Restore, Paint, Pick, with the Area modifier key Shift.
    - **Lints:** status icon, id, measured value and threshold. Clicking a row pans and zooms to
      the spot and flashes it for 1 s, and the arrow keys step through spots.
    - **Frames** (when the source is a sheet or an animation):
      - the boxes over the source, which can be moved, resized, added (drag on empty space) and
        excluded (double-click);
      - the animation list: Name, FPS, Loop Forward / Ping-Pong, Include, Split Here, Join With
        Next;
      - Anchor (Ground / Feet / Center / Box) and Padding;
      - a player with onion skin (previous and next frame at 30 %) and per-frame nudge with
        Alt+Arrows.
  - **Footer:**
    - Output (path, Format PNG / Aseprite);
    - **Write** (Ctrl+Enter): one undo step, "Pixel Art Cleanup (hero_clean.png)";
    - Save as Import Step;
    - Copy Recipe, which copies the JSON for an agent or the CLI;
    - Open in Aseprite (S44's "Edit in Aseprite" command) on the written file.
- **R43. Undo and hand-off:**
  - Recipe edits in the window (slider moves, line drags, box edits, hand edits) are a local undo
    stack until Write. A slider drag is one step. Ctrl+Z reaches the window first, as Sprite
    Normal Map's paint does (`App.cpp:1392-1403`).
  - Write and Save as Import Step are project-history steps (Ctrl+Z in the Project window or
    anywhere outside the window).
  - Opening an output re-opens its recipe and source, so a person can finish an agent's run,
    and an agent can re-run a person's tuning with `rerun`.
  - The Project window's right-click on an image offers **Pixel Art Cleanup…**; on an output it
    offers **Re-run Pixel Art Cleanup**.
  - The Inspector's texture import settings get a **Pixel Art Cleanup** section: Enabled, recipe
    summary, verdict badge, and Open in Cleanup Window.

### Agent UX (MCP, CLI, Lua)

- **R44. `fix_pixel_art`** (group `pixel_art`, activity Edit, `writesFiles` when `output` is set,
  `allowedWhilePlaying` true, as it touches no scene unless `apply` is given). The loop:
  1. Preview without `output`. Nothing is written. The result holds:
     - the verdict, confidence, guards and summary;
     - the grid `{cell_width, cell_height, offset_x, offset_y, confidence, fit, outlines,
       follow}`;
     - the output size and colours;
     - the palette (colours, locked, lost with locations);
     - specks, background, the animation boxes and clips;
     - the lints, the resolved recipe and `default_output`.

     It also returns one image (R45).
  2. Adjust `recipe` keys, or `palette`.
  3. Write with the same arguments plus `output: true` (or a path): exactly the previewed pixels,
     one undo step, `undo {id, name}` in the result.

  `apply` (a Sprite GameObject reference, on write) sets its texture, and for sheets its frames
  and one Sprite animation per clip. That is a second, scene undo step, as sprite normals' `apply`
  is. `rerun`, `force` and `overwrite` behave as in R30–R32.
- **R45. The preview image** (`image`: `compare` (default) | `result` | `grid` | `lost` | `lints`
  | `none`; `max_width` 256–2,048, default 1,024):
  - `compare`: the source with the grid lines (bent lines drawn bent) at left, and the result at
    an integer scale onto the same height at right.
  - `lost` and `lints` outline their cells or spots in magenta, numbered as the result's lists
    are (L1…, S1…).
  - Sheets show each frame box labelled and each clip as a strip.
  - Every image states its scale in a corner label. Images go out as `ToolResult` images, never
    base64 in text.
- **R46. `lint_pixel_art`** (group `pixel_art`, activity Read, readOnly, idempotent, allowed while
  playing).
  - `source`: an image file, `.aseprite` (with `frames`: a tag name or a list of frames, and
    `layer`), or a Sprite GameObject.
  - Other arguments: `lints` (default every applicable one), `palette`, `rules`, `backdrops`,
    `loop` (default from the tag's direction, else true), `image` (the frames with spots marked),
    `fail_on`.
  - It returns `{verdict, results: [R25], counts {pass, warn, fail, skip}, frames, image?}`. The
    text gives one line per non-PASS lint.
  - `.aseprite` files are read through S44's reader with the same pixels the Sprite Preview
    shows, so a lint matches what a person sees.
- **R47. Errors** are typed: `{kind, message, fix, field?}` (`editor/pixelart/PixelProblem.h`,
  the family's shared struct, lifted from `spritenormals::Problem` by S44). The kinds are:
  `source_missing`, `image_too_large`, `bad_recipe`, `recipe_version`, `palette_missing`,
  `no_grid`, `needs_person`, `output_exists`, `output_edited`, `output_outside_assets`,
  `output_is_source`, `project_changed`, `cancelled`.
  - `fix` always gives a valid value or range, for example "cell_width is 1–128; the detected
    cell was 7.62".
- **R48. CLI** (`editor/pixelart/cleanup/CleanupCli.{h,cpp}`, dispatched in `main.cpp` as
  `generate-sprite-normals` is):
  - `jasperengine-editor fix-pixel-art <in> [--recipe r.json | --rerun] [--palette p] [--grid
    auto|<w>[x<h>][+<ox>,<oy>]] [--sheet auto|off] [--format png|aseprite] [--out path]
    [--overwrite] [--force] [--dry-run] [--image out.png] [--project dir]`;
  - `jasperengine-editor lint-pixel-art <in>… [--palette p] [--lints a,b] [--rules r.json]
    [--backdrops #hex,…] [--fail-on fail|warn] [--image out.png]`.
  - Output is JSON on stdout (the MCP structured result), with `--format text` for people.
  - Exit codes:
    - 0 `clean` (lint), or written / dry run with a clean output (fix);
    - 2 usage, including `bad_recipe`;
    - 3 `fixable` (lint: auto-fixable FAILs; fix dry run: a confident fix exists; fix write:
      written, with WARN lints left when `--fail-on warn`);
    - 4 `needs_person` (nothing written without `--force`);
    - 1 could not run (I/O, write failure), as the existing subcommands use 1.
  - Without `--project`, paths are plain files and no project is needed. With it, outputs follow
    R31.
- **R49. The `cleanup_outputs` rule of S42's validator.** S42 owns `check_pixel_art` and
  `check --pixel-art` (S42 R22, R24); S46 registers one rule there (`PixelArtCheck::registerRule`,
  S42 §6.1) instead of a second `--pixel-art` category:
  - it lints every PNG and `.aseprite` under the assets folders that carries a
    `jasper.pixelcleanup` recipe or a `.meta` with `import.pixelCleanup`;
  - it reports `stale` (R32) and forced writes as WARN and FAIL lints as FAIL (S42 R10's row), with
    `fix_pixel_art {source, rerun: true}` as the fix for `stale`;
  - exit codes are those of `check`: 0 no errors, 1 errors, 2 usage.
- **R50. Lua:** none. Cleanup is editor-only, and editor scripts reach it with
  `Editor.call("fix_pixel_art", {...})` (`editor/util/EditorScripts.cpp:283`). Runtime code
  never cleans art, because the cooked texture is already clean.
- **R51. Guidance** goes in `docs/mcp/guide/pixel-art-cleanup.md` (`jasper://docs/pixel-art-cleanup`):
  - the loop, the recipe keys with their defaults and ranges;
  - a table of which verdict leads to which next step;
  - "never loosen a lint threshold to pass" (thresholds are reported with their source);
  - "look at the compare image before writing";
  - "add lost colours rather than raising `lost_threshold`";
  - and a worked example of an AI sprite sheet becoming an `.aseprite` with tags.

  The agent guide gets one line in the `## Pixel art` digest section, next to S43's palette line (§6.1).

### Data & compatibility

- **R52.**
  - No scene, prefab or component format changes. Old projects load unchanged.
  - The new data:
    - the `jasper.pixelcleanup` iTXt chunk (format id and version inside);
    - the `.meta` keys `import.pixelCleanup` and `pixelCleanup.recipe`, written with
      `SetAssetMetaCmd` (a missing key means off);
    - the atlas JSON's `meta.jasperengine` object, which other readers ignore.
  - Recipe version 1. A later version bump adds a migration in `CleanupRecipe::upgrade`, and
    version-1 recipes keep re-running with version-1 behaviour (`pipeline_version` pins the
    algorithm revision, as sprite normals' recipe versions do, `SpriteNormalsRecipe.h:27-29`).

## 6. Design constraints & decisions

- **D1. A port, not a reinvention.** ritmo's tests define the behaviour, and the port keeps its
  constants with their names (`SAME_SIZE`, `NEAR_SQUARE`, `CROSSING_COST`, …), each commented
  with its ritmo file. A deviation needs a failing ported test that shows why, recorded under
  Deviations.
- **D2. One session layer** (`CleanupSession.{h,cpp}`, as `SpriteNormalsSession`):
  `Spec → prepare (main thread: paths, project, palette asset) → run (any thread) → Result →
  encodeOutputs → writeUndoable | writeDirect`. The window, MCP, CLI, the import cook and
  `check` call only this, so a preview is the same pixels as a write everywhere.
- **D3. Two tools, not one with modes.** Linting is read-only and idempotent, and S47 and S48 call
  it after every edit. Fixing writes files and becomes a job. Different annotations, activity and
  "allowed while playing" settings would be misleading on one tool. Two tools cost about 2,500
  characters in total (§6.1).
- **D4. The recipe is open in the schema and closed in the handler.** `recipe` is
  `schema::anyObject` (its keys are in `jasper://docs/pixel-art-cleanup`). This keeps
  `fix_pixel_art` within 1,500 characters while the decoder (R28) refuses unknown keys by name
  with the valid list. A fully typed recipe schema would cost about 3,000 characters in every
  session where the group is listed.
- **D5. Lints live in their own library** with no dependency on the cleanup pipeline, the session
  or MCP. S47's `edit_sprite {lint: true}`, S48's contact sheets and S42's `check_pixel_art` link
  `editor/pixelart/lint/` alone.
- **D6. The verdict is stricter than ritmo, never looser** (R29 guards). When in doubt the answer
  is `needs_person` with the reason, because a confident wrong fix costs a person more than a
  question.
- **D7. Outputs are new files.** The source is never replaced, even with `overwrite`. That
  guards only existing outputs, so a person's original art survives every agent run.
- **D8. The import step cooks; it does not write.** The cooked texture is derived, and nothing
  in `assets/` changes. A person who wants an editable file uses Write.
- **D9. Hand edits stay minimal**, the same four as ritmo: erase, restore, paint, pick. Anything
  more is Aseprite's (owner decision) or S47's.

### 6.1 MCP tools and the budget (S31)

| Tool | Group | Parameters | Estimated listing chars |
|---|---|---|---|
| `fix_pixel_art` | `pixel_art` | `source` (string: path), `entity` (entityRef: a Sprite), `recipe` (anyObject), `palette` (string\|array), `sheet` (enum auto/off/on), `rerun`, `output` (bool\|string\|object `{png, aseprite, json}`), `format` (enum png/aseprite), `overwrite`, `force`, `apply` (entityRef), `image` (enum), `max_width`, `timeout_seconds` | ≤ 1,450 |
| `lint_pixel_art` | `pixel_art` | `source`, `entity`, `frames` (string\|array of integers), `layer`, `lints` (array of enum), `palette`, `rules` (object `{max_colors, sizes, transparent_background}`), `backdrops` (array of strings), `loop`, `image`, `fail_on` | ≤ 1,100 |

- S46 adds 2 tools and about 2,550 characters to `pixel_art`. That group is shared by S41, S42,
  S43, S46 and S48, at most 12 tools and 15,000 characters (`budget.json` `group_default`), so
  no budget entry is needed.
- The group row is S43's (S43 §6.1, wave 5A). The family's `pixel_art` total is 6 tools and
  ≈ 8,050 characters (ledger "MCP budget").
- Core listing +0. The guide's `## Pixel art` digest section (S41 §6.4) gets one line next to S43's
  palette line: "Cleanup of AI or loose pixel art, craft lints: fix_pixel_art (preview, then output:
  true), lint_pixel_art; jasper://docs/pixel-art-cleanup". That is about 130 bytes, within the
  section's family share and the guide's 1,500 bytes of headroom.
- `tool_budget.py --check`, `test_tool_conventions.py` (snake_case, closed `rules` object, no
  `oneOf`), `test_tool_coverage.py`, and the hidden-until-enabled test (AC17).

### 6.2 API sketch

```cpp
// PixelProblem, Verdict (editor/pixelart/PixelProblem.h) and LintStatus, LintSpot, LintResult,
// LintReport, toJson (editor/pixelart/lint/LintTypes.h) are S44's (S44 §6.3)
namespace jasperengine::editor::pixelart::lint {
    struct LintRules { std::optional<int> maxColors; std::vector<std::pair<int,int>> sizes;
                       bool transparentBackground = false; };
    struct LintInput {
        std::span<const PixelImage> frames;                 // S44; one or more, same size
        const Palette* palette = nullptr;                   // S43; nullptr: palette lint SKIPs
        std::vector<Rgba8> backdrops; bool loop = true; bool sheet = false;
        LintRules rules;
    };
    struct LintOptions { std::vector<std::string> only;     // empty: every applicable lint
                         std::map<std::string, double> thresholds; };   // reported as options.<id>
    LintReport lint(const LintInput& input, const LintOptions& options = {});   // calls S48's three
    std::span<const char* const> lintIds();                  // catalogue order (R26)
}
namespace jasperengine::editor::pixelart::cleanup {
    struct Recipe;                                           // R28; CleanupRecipe.h
    bool decodeRecipe(const nlohmann::json& json, Recipe& out, std::vector<PixelProblem>& problems);
    nlohmann::json encodeRecipe(const Recipe& recipe);       // canonical
    // Verdict is the family's (PixelProblem.h, S44)
    struct GridFit { double cellWidth, cellHeight, offsetX, offsetY, confidence, fit; bool outlines; };
    struct Result {                                          // what preview and write share
        Verdict verdict; double confidence; std::vector<nlohmann::json> guards; std::string summary;
        GridFit grid; std::vector<PixelImage> frames; Palette palette;
        std::vector<LostColor> lost; lint::LintReport lints; Recipe resolved;
    };
    struct Spec { std::string source; std::optional<EntityRef> entity; nlohmann::json recipe;
                  bool rerun = false; OutputSpec output; bool overwrite = false, force = false; };
    struct Prepared;                                         // main thread: paths, palette asset, project
    std::optional<Prepared> prepare(Project* project, const Spec& spec, PixelProblem& problem);
    Result run(const Prepared& prepared, const std::atomic<bool>* cancel = nullptr);
    bool writeUndoable(Project* project, const Prepared&, const Result&, std::string& error);
    bool writeDirect(const Prepared&, const Result&, std::string& error);       // CLI
}
```

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R3, R2) | `editor/tests/pixelart/CleanupGridTest.cpp`, ported from `core.test.ts` "grid": `uniformCuts(20,4,1) == [1,5,9,13,17,20]` and the three other cases; a blank picture finds no grid; the drift case (`scale 8, offset 2, drift 0.12, blur 1, noise 4`, refined) has every cut within 1 px of `boundaries()` |
| AC2 (R3, R5, R8, R40) | `CleanupRecoveryTest`: `makeSprite(32, 24, sweetie16, 11)` through `upscale` at {8×}, {5×, offset 2}, {7.6×, offset 3, blur 1, noise 6}, {12.3×, offset 5, blur 2, noise 4}, {3×, offset 1}: cell within 0.05 of the scale, confidence > 0.5, **0 mismatched pixels**; the padded case (16×12 in a 14 px margin at 9.5×) and the large case (48×48 at 21.3×) also exact, the large one ≤ 120 ms (R36). Red control: with the vote replaced by the median the 7.6× case mismatches (a test-only toggle, run once and recorded here) |
| AC3 (R3 squares) | the two square-cell cases of `core.test.ts` (9.4× blurred along one axis; a 31.5× picture stretched 4 % on one axis) keep near-square cells |
| AC4 (R4, R11) | `CleanupOutlineTest` ported from `outline.test.ts`: one-cell strokes detected, a split stroke given back, a real 2 px stroke kept, the staircase whole, a warped grid (`warped(sprite, 10, 0, 0.45, 12)`) recovered with `bend` but not with `straight`, and every bent cut within ~1 px of the truth |
| AC5 (R10) | `CleanupSpecksTest` ported from `cleanup.test.ts`: a doubtful speck merged, a clear one kept, a close colour merged, two-pixel eyes kept until strength 1 and never when locked, a diagonal line never a speck, a kept speck left alone, size 0 does nothing |
| AC6 (R12) | `CleanupReduceTest`: a one-pixel ring shrunk by 2 stays closed and one pixel thick; a one-pixel step becomes a diagonal; a real corner stays; `common` mode gives the majority colour |
| AC7 (R13, R15, R14) | `CleanupPaletteTest`: a palette missing a saturated accent reports it lost with its cells and the colour it became, and adding it brings back every cell; a fake checkerboard background (two tones) is removed and a drop shadow too with `shadow`; an enclosed eye of the background colour stays without `enclosed`; partial alpha is cut to 0/255 and counted |
| AC8 (R19–R21) | `CleanupSheetTest` ported from `animation.test.ts`: `fakeSheet(frames, 6)` gives one box per frame around its art and drops the labels; side-by-side blocks split; a thrown bomb stays in its frame; a faint last frame keeps it an animation; a grid as dark as the outline is painted out and the outline kept; a single picture is not a sheet; every frame on one cell size, one palette and one canvas with the feet on one row; `ground` keeps the bob |
| AC9 (R23) | `CleanupSheetOutputTest`: the sheet case written as `.aseprite` reads back through S44's reader with the frames pixel for pixel, the tags and durations; as PNG + JSON the JSON's frame rects index the PNG exactly and `frameTags` match |
| AC10 (R24–R27) | `editor/tests/pixelart/LintTest.cpp`: per lint one hand-made positive and negative fixture in `editor/tests/pixelart/fixtures/lint/*.png` (e.g. jaggies 3-1-3 WARN vs 2-1-2-1 PASS; doubles L-corner; a pinhole; a 1-frame blink on 10 % of pixels; a dark sprite on a dark backdrop); `banding`: three parallel 1-px ramps WARN, a dithered ramp and a single anti-aliased corner PASS; `pillow_shading`: concentric rings on a 12×12 disc WARN, the same disc lit from the top-left PASS, a flat disc SKIP. Every result has the R25 keys; `at` ≤ 8; determinism over 3 runs and 1/8 threads |
| AC11 (R29, R30) | `CleanupReportTest`: the AC2 cases are `fixable`; a photo (`tools/fixtures/pixel-art/photo.jpg`) is `needs_person` with guard `no_grid` or low fit; a palette losing 8 % of cells is `needs_person` with guard `lost_share`; the native mushroom is `clean`; the mushroom with 3 off-palette pixels is `fixable`. MCP: writing `needs_person` without `force` fails `needs_person` and writes nothing (directory listing unchanged); with `force` the recipe has `forced: true` |
| AC12 (R28, R31, R32, R41) | `tools/mcp/tests/test_fix_pixel_art.py`: preview writes nothing and returns one image + `default_output`; the same arguments + `output: true` write `<stem>_clean.png` whose pixels equal the previewed result (decoded); its chunk `jasper.pixelcleanup` decodes to the resolved recipe with the source SHA-256; `rerun` gives byte-identical bytes; editing the source flips `stale`; one undo removes the file (`get_undo_history` name "…: Pixel Art Cleanup (hero_clean.png)"); an unknown recipe key fails `bad_recipe` naming the valid keys; `overwrite` false on an existing output fails `output_exists` |
| AC13 (R33) | `test_fix_pixel_art.py::test_import_step`: `SetAssetMetaCmd` `import.pixelCleanup` on an AI PNG; `get_asset_info` reports the cooked size = the fixed size; the cooked pixels (CPU-readable cook) equal the written output's; changing the source so the verdict drops cooks the raw source and logs the warning; `export` output contains no `jasper.pixelcleanup` chunk |
| AC14 (R44–R46) | `test_lint_pixel_art.py`: an `.aseprite` fixture (S44) with a tag of 4 frames, `frames: "walk"`: flicker measured equals the Python oracle `tools/mcp/pixel_lint_ref.py` (an independent re-implementation of R26/R27 in numpy, written from this spec) for every lint's `measured` and `count`; `image: true` returns one image whose magenta spots match `at`; read-only (file mtimes unchanged) |
| AC15 (R48, R49) | `tools/mcp/tests/test_pixel_art_cli.py`: `fix-pixel-art --dry-run` exits 3 on an AI sprite, 4 on the photo, 0 on the native mushroom; `lint-pixel-art` exits 0/3/4 likewise, 2 on `--lints nope` with the valid ids in the JSON error; `check --pixel-art` lists a stale output as a `cleanup_outputs` warning (none after `--rerun`) and exits 1 on an output with a FAIL lint; `check_pixel_art {rules: ["cleanup_outputs"]}` returns the same findings (S42's rule registry) |
| AC16 (R36–R39) | `tools/perf/pixel_cleanup_bench` on build server A: detection 1024²/2048², whole fix, sheet, lints, interactive re-run; the table goes into this spec's Deviations if any budget is missed (never a raised budget without the owner) |
| AC17 (§6.1, R51) | `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py`, `gen_tool_docs.py --check`, `test_protocol.py` (guide size); `test_pixel_art_groups.py`: `fix_pixel_art` and `lint_pixel_art` absent from the default listing and listed after `tool_groups {enable: ["pixel_art"]}`; `test_autosync.py` finds every tool name the guide line uses |
| AC18 (R42, R43) | `tools/mcp/tests/test_pixel_art_cleanup_window.py` (xvfb, isolated editor, `find_ui_item`): open the window on an AI sprite, the badge reads Fixable; drag grid line x:3 by 2 px → `recipe.moved` in Copy Recipe; hover a Lost Colors row → the canvas highlight item exists; click a lint row → the canvas zoom centres on its spot; Ctrl+Z inside the window undoes the drag, outside it does not; Write creates one project-history step; screenshots (Compare view, Frames tab with onion skin) in the evidence |

## 8. Dependencies & conflicts

- **Depends on:**
  - **S43** (wave 5A, landed first): `engine/core/pixelart/Palette.h` (the asset, presets, ramps,
    `locked`) and `PixelColor.h` (`toOklab`, `toCielab`, `ciede2000`, `ColorMetric`,
    `PaletteMatcher`, Sharma test pairs).
  - **S44** (wave 5A): `PixelImage` (RGBA8 plus an optional index plane), `PixelProblem.h`,
    `lint/LintTypes.h`, the `.aseprite` reader (composite of frames/layer/tag) and writer (indexed
    and RGBA, layers, frames, durations, tags, `.meta`), "Edit in Aseprite", and the Sprite Preview
    panel.
  - **S42** (wave 5B): `pixel_art.backdrops` and `registerRule` for `cleanup_outputs`.
  - **S48** (wave 5B): `off_palette`, `partial_alpha` and `flicker` in `EffectLints`.
  - Existing: S03 (atomic writes), S05 (the watcher reloads written outputs), S06 (`ParallelFor`,
    background jobs), S14 (the texture cook queue and import settings), S24 (project history,
    names, agent attribution), S25 (commands and shortcuts), S31 (groups and budget).
- **Blocks:** S47 (same wave: `edit_sprite {lint}`, `lint_pixel_art` on drawn sprites) needs M1.
  S42's `check_pixel_art` gains `cleanup_outputs` when S46 lands. S48 does not wait for S46: its
  effect lints use S44's result types and its own three rule lints.
- **Interfaces this spec defines for others:** `editor/pixelart/lint/PixelLint.h` (R24–R27,
  §6.2), `editor/pixelart/PixelProblem.h`, and the recipe chunk keyword `jasper.pixelcleanup`.
  S48's effect lints live beside them in `editor/pixelart/lint/EffectLints.{h,cpp}` with the same
  result type (S48 D13), and include the three rule lints this catalogue calls (`off_palette`,
  `partial_alpha`, `flicker`); thresholds passed through `LintOptions::thresholds` are reported as
  `options.<id>`.
- **Merge hotspots:**
  - `McpToolGroups.cpp` (the `pixel_art` row, shared with S41–S43, S48);
  - `McpInstructions.cpp` (the `## Pixel art` digest section);
  - `editor/main.cpp` and `CommandLine.cpp` (subcommands, `check` categories);
  - `App.cpp`, `DefaultCommands.cpp` and `AppSettings.cpp` (the window);
  - `TextureCooker.cpp` (the import step, shared with S14 and S44's Aseprite cook);
  - `ProjectWindow.cpp` (context menu, badges);
  - `NOTICE.md`.

## 9. Risks & defaults

- **ritmo's behaviour is pinned by its tests, and we cannot run them** (the clones must not be
  run). Default: the ported tests use the same generator and seeds, so the numbers in AC1–AC8
  must hold as they are. A C++ result that differs from a TS assertion is a port bug until proven
  otherwise.
- **Lint thresholds are taste.** The craft lints default to WARN and never FAIL. Thresholds can be
  changed per call but are always reported with their source, and the guide forbids loosening
  them to pass.
- **`pillow_shading` and `banding` are new and may misfire on unusual art.** Default: WARN only.
  Their fixtures in AC10 define them, and a false positive found in use adds a fixture before the
  rule changes.
- **Big AI images (4K) are slow to preview over MCP.** Default: a job past 1 s, `max_width`
  1,024, and the 4,096 px input limit.
- **The import step adds cook time** (one fix per source change, ≤ 300 ms for 1 MP). Default:
  off. It is cached by the recipe and source hash.
- **Order.** S43, S44 (wave 5A) and S42, S48 (wave 5B) land before S46, so every interface this
  spec uses exists when it starts; nothing here needs a temporary copy.

## Milestones

- **M1** Lint library (R24–R27) on S44's types and S48's three rule lints, with fixtures and the
  Python oracle (AC10). S47's lint step can start.
- **M2** Grid, sampling, palette snap, specks, outlines, shrink, lost colours, background, edits,
  pipeline, verdict (R1–R18, R29) with the ported tests (AC1–AC7, AC11 unit part).
- **M3** Recipe, outputs, re-run, `fix_pixel_art`/`lint_pixel_art`, CLI, the `cleanup_outputs` rule
  (R28, R30–R32, R44–R49); AC11 MCP part, AC12, AC14, AC15, AC17.
- **M4** Sheets and animations (R19–R23); AC8, AC9.
- **M5** Window and import step (R33, R42, R43); AC13, AC18.
- **M6** Performance and docs (R36–R39, R51); AC16.

## Deviations

- **S48's lints are copied, not depended on.** S48 had not landed when S46 started, and `lint()` needs its `off_palette`, `partial_alpha` and `flicker` (R26). `editor/pixelart/lint/EffectLints.{h,cpp}` are byte-identical copies of `feature/s48-pixel-fx` at `8531847fc`. S48 owns them: at merge S48's version wins, and S46's CMake block for them is dropped. `off_palette`, `partial_alpha` and `flicker` are defined once, in S48's file; S46's catalogue only calls them.
- **Fixture paths.** The lint fixtures live in `editor/tests/fixtures/pixelart/lint/`, not `editor/tests/pixelart/fixtures/lint/` (`JASPER_EDITOR_TEST_FIXTURES` is the suite's one fixture root, `docs/testing.md`). The AC11/AC15 photo is `editor/tests/fixtures/pixelart/cleanup/photo.jpg`, not `tools/fixtures/pixel-art/photo.jpg`, because that directory is a fixture project other suites read.
- **Guide.** The upstream MCP rules (ledger) replace §6.1's `## Pixel art` digest line: the area is reached through the existing `pixel_art` "Other areas" entry and `jasper://docs/pixel-art-cleanup`.
- **AC2's red control.** Replacing the vote by the median does NOT make the 7.6x case mismatch: vote and median agree on every AC2 case (0 mismatches, run once on a scratch copy). The vote is pinned instead by `VotingGetsAVeryNoisySpriteBackWhereTheMedianDoesNot` (noise 20) and `TheVoteNeverPicksAColourTheCellsPixelsDidNotVoteFor` (a hand-built cell), each failing with 2 wrong pixels under the median.
- **R14.** `alpha_cutoff` decides only 1x1 (native) cells. Larger cells use `sample.alpha_threshold` per pixel read, as R5 says. Every output pixel is 0 or 255 either way.
- **Lint interpretations (R26, R27, where the spec leaves room).** Drawn is alpha > 0; opaque (`off_palette`, `banding`, `transparent_background`, `pillow_shading`) is alpha 255. Used colours are the distinct RGB across all frames. `orphans` counts empty as a colour. `jaggies` counts one spot per chain occurrence. `near_duplicates` measures the closest pair and SKIPs under 2 colours. `banding` ramp runs are maximal windows, a lightness peak belongs to both slopes, the diagonal shift is in x, and `count` is groups. `pillow_shading`'s `count` is 3 per pillow body (its centroid and two extreme rings). `contrast`'s `threshold_source` is the backdrops' origin. `lint()` rewrites S48's fix text for `partial_alpha` and `flicker` so each names a tool action (R26). `empty_frame`, `one_frame` and `frame_scale` are tested with in-code inputs, not PNG fixtures, since they judge animation data rather than pixels.
- **`checkImage` and R6.** A picture with more than 256 colours is never native, so a photo with no grid is `needs_person` (guard `no_grid`) as AC11 requires; R6's first bullet alone would call it native.
- **Recipe key `output_format`.** R28's `format` (png/aseprite) is `output_format` in the recipe, because `format` already holds the recipe's format id, as in the family's other JSON documents. The palette also accepts `"default"` and `"original"`; sheet-level `fps` and `loop` are read only when `animations` is absent (ritmo's older recipes).
- **Report guards.** Beyond R29's list, `low_confidence`, `low_fit` and the id of any FAIL lint the pipeline does not remove are listed in `guards[]`. A sheet's lints run on a padded common canvas, with `flicker` and `size` judged per clip; an auto palette is proposed once per sheet.
- **AC16's bench is a gtest suite.** `tools/perf/pixel_cleanup_bench` is `CleanupBench` in `editor/tests/pixelart/CleanupBenchTest.cpp` (the family's `AsepriteBench` pattern: `[bench]` lines, enforced through `perfSlack()` on the benchmark server); R38's lints are `PixelLintBench`. The sheet case is 2008 x 1000 rather than 2048 x 1024, see the next item.
- **Two limits inherited from ritmo (port compared line by line; the TypeScript was never run).** (1) `removeGridLines` can take a painted grid's dark colour as the sheet's fill and a 3-row background sliver for a grid line, depending on where the frame boxes land against the painted grid; the real fill is then not cleared and the sheet reads as one box (`fakeSheet` of 30x84 sprites at 6x; 30x82 slices). The result is then a single picture, judged as one. (2) `clearSheet` zeroes only alpha and the edge profile sums RGBA, so hidden grid and box lines still make edges, and per-frame detection on very small sprites can pick the wrong cell (10x12 at 6x: shared confidence 0.38). The verdict is then `needs_person`, the safe direction; `animation.test.ts` makes the same observation, and a given `cells` grid overrides it.
- **`flicker` and S48's copy (follow-up for S48).** S48's `lintFlicker` compares whole pixels, so the RGB a clear pixel happens to keep reads as a change or hides one (found by a blind re-implementation of R26). `EffectLints.cpp` stays byte-identical, so `lint()` hands it frames whose clear pixels are zeroed, only when a frame needs it. S48's other caller, `capture_contact_sheet`, still has the bug on non-premultiplied frames; the one-line fix (compare alpha-0 pixels as equal in `samePixel`) belongs on `feature/s48-pixel-fx`, after which this workaround can go.
- **AC14's independence.** `tools/mcp/pixel_lint_ref.py` was written by the lint author after the C++, so a second numpy implementation was written blind from R25-R27 alone and compared on 154 cases (fixtures plus seeded random art). It found the `flicker` issue above and two alpha bugs in the craft lints (`pillow_shading`, `transparent_background`), each fixed with a test that failed first. The remaining differences are readings the spec leaves open, listed above.
- **Session API (§6.2, D2).** `prepare` returns `std::optional<Prepared>` and takes `std::vector<PixelProblem>&` instead of one problem, because R28 returns every recipe key error at once. `Spec::person` is added: the window writes a `needs_person` result with no `forced` recorded (R30's "they are the person"). `checkImage` gained an optional `PipelineCache*`, without which the single-picture path could not reuse R39's cache.
- **Lint verdict (R46, AC15).** On a single non-`.aseprite` picture the verdict also runs `checkImage` and takes the worse of the two, so it judges the grid: AC15 needs exit 3 on an AI sprite and 4 on a photo, where the lints alone say clean. Multi-frame and `.aseprite` sources use the lints alone.
- **CLI flags (R48).** `--format` also takes `json`/`text` on `fix-pixel-art` (R48 uses one flag for both meanings). `fix-pixel-art` also takes `--fail-on`, which R48's exit 3 needs but its usage line omits. `lint-pixel-art` also takes `--project` (palettes, backdrops). `--project` reads `project.yaml` (assets directory, `palettes.default`, `pixelArt`) and loads no Project, so the CLI has no Sprite source.
- **Re-run (R31, R32, R41).** A re-run replaces its own files without `overwrite`, unless a new output path is given, because R41 has it write back byte-identical; `output_edited` still guards it. `forced` carries over so the bytes stay identical. `recipe` together with `rerun` is refused (`bad_recipe`). `stale` appears in results on a re-run only; `checkStaleness` and the rule cover outputs generally.
- **`sheet: on`.** The recipe has no "on". The argument is passed as `auto` and held as a session flag that slices whatever frames are found; with none it warns `sheet_not_found` and fixes one picture.
- **Conventions test.** `fix` and `lint` were added to `VERBS` in `test_tool_conventions.py`, because the spec fixes those tool names.
- **MCP job behaviour (R37).** Every fix is a job; `timeout_seconds` (default 60) is how long the call waits for it, so a run past 1 s answers with `job_id` only after that wait. Lint runs as a job only above 2,000,000 pixels.
- **Cancel (R37).** Cancel is polled between pipeline steps and between frames, so it lands after the current step. The longest step at R1's limit is `detectGrid` on 4096 x 4096: 1.5 s on one core (measured; less with the job pool). R37 sets no latency bound, and a cancelled run writes nothing.
- **`import.pixelCleanup` from agents (R33, AGENTS.md).** Beyond the spec, `fix_pixel_art {output: {import_step: true}}` stores the resolved recipe in the source's `.meta` as one undo step. It sits on the existing `output` argument rather than on `set_texture_import_settings`, because the value is a resolved recipe that only a cleanup run produces, and the same preview-then-write loop and `needs_person` refusal apply. It refuses `.aseprite` and animated sources and, like a write, `needs_person` without `force`.
- **The cook's fix runs once.** The queue plans and cooks a texture in separate stages, each with its own request, so the step would otherwise run twice on a miss. `importCookStep` keeps the plan's outcome for the cook in a small memo keyed by recipe, source hash and size; the cook takes it, which frees it.
- **Resolved sheet recipes (R28).** A sheet's resolved recipe keeps the grid as asked (`auto`) and resolves sheet and palette, because each frame has its own offset; single pictures resolve the grid.
- **Animated sources and R6.** With `grid: auto`, an animated source reads at 1x1 when up to about 8 sampled frames are all native, extending R6 to R22; `animationFix` would otherwise fall back to an 8 px cell.
- **Errors (R47).** Kinds beyond R47: `invalid_value`, `invalid_apply`, `write_failed`, `failed`. Unreadable or corrupt files report `source_missing`; bad frame, layer or lint ids report `bad_recipe`. `no_grid` is raised only on a write that has no output to write; a preview reports guard `no_grid` with verdict `needs_person`.
- **Writes (R31).** `writeDirect` uses `spritenormals::writeOutputsDirect` (a `WriteFilesCmd`, all or nothing) instead of `texsynth::writeFilesAllOrNothing`. A PNG sheet's JSON defaults to `<png stem>.json`.
- **`apply` (R44).** On an `.aseprite` output, `apply` uses the sheet's Animation Player tag clips rather than one Sprite Animation per clip; PNG sheets do get one per clip. `apply` and `entity` resolve in the selected scene only, since §6.1 has no `scene` parameter. Only a single PNG's `apply` is exercised by a test.
- **`cleanup_outputs` (R49).** On an `import.pixelCleanup` texture the rule lints the import step's output, not the raw source, and adds a FAIL when that verdict is `needs_person`. `output_edited` is not a finding.
- **Main guide.** The agent guide is unchanged, with no digest line or new "Other areas" entry (ledger rule): the route is the `pixel_art` group row, the pointer in `pixel-art.md` and `jasper://docs/pixel-art-cleanup`.
- **The demo cannot be written (R42).** Try a Demo's picture exists only in memory, so Write and Save as Import Step are off for it: it has no source path or SHA-256 for the recipe (R28), nothing to re-run (R32) and no default output beside a source (R31). Writing the demo PNG into the assets directory as a project step first, and opening it as an ordinary source, would make it possible.
- **Inspector Enabled (R43).** The checkbox writes at once as its own undo step, not through the import settings' Apply, because `import.pixelCleanup` is outside `import.textureImporter`. Enabled with no recipe runs the default recipe. A recipe that cannot be decoded cooks the raw source with a warning instead of failing the cook.
- **Import step and saved source (R33).** An empty recorded SHA-256 counts as "source changed", because a hand-written recipe cannot vouch for the file.
- **Source changed badge (R32).** It is the same badge, driven by `checkStaleness` on a 0.5 s stat for known outputs, because the normal-map watcher indexes only normal maps.
- **Window limits (R42).** Hand tools work only in the Result view on single pictures, since R21 has no per-frame edits. Compare paints a bent grid on each line's mean. A sheet's Result view is always the PNG atlas, whatever the output format. Added frame boxes join the selected animation. On a sheet, a click on a lint spot centres and flashes the frame's own cell, placement included.
- **Window writes (R30, R39, R42).** Write over an existing or edited file asks, then replaces; Re-run from the Project window only alerts. Save as Import Step also asks on `needs_person`. R39's prediction is the last measured run time, or the last full run for grid edits, and the first run is always a job.
- **New window hooks.** `focus_window` gained `pixel_art_cleanup` and `editor_status.ui` gained `pixel_art_cleanup`, so an agent can open and check the window; focus_window's description pays for the enum value (the core listing is 98,997 of 99,000).
- **`near_duplicates` on photos (R26, R38).** Used colours are now collected in linear time (`contrast` too). `near_duplicates` compares every pair, so above 1,024 used colours (four times pixel art's 256) it SKIPs ("not pixel art yet; fix_pixel_art snaps it to a palette first"). Before this, a 256 x 256 noise picture took 7.4 s, and a picture at R1's 4,096 px limit would not finish; now it is 7 ms. The oracle SKIPs the same way.
- **`moved` ids.** An id with an empty index (`"x:"`) is ignored; ritmo's `Number("")` reads it as line 0.
