# S47 — Agent Sprite Drawing: MCP tools that read, draw, render and diff `.aseprite` sprites

**Wave / Depends on.** Pixel-art family (spec-first), wave 5C. Depends on:
- **S44** (wave 5A): the `.aseprite` codec, `PixelImage`, `PixelProblem`, the Sprite Preview panel
  and the `AsepriteSprite` hot reload;
- **S43** (wave 5A): `Palette`, OKLab, nearest colour, ramps;
- **S42** (wave 5B): the project backdrops `pixel_art.backdrops` for `render_sprite`;
- **S46 M1** (same wave 5C): the lint library `editor/pixelart/lint/` (`lint()`); S47's lint step
  (M5) starts after it.

S48 (wave 5B) composes its contact sheets itself (S48 D12) and does not depend on this spec;
`render_sprite` may share S48's CPU sheet-layout and label code. Owner area:
- `editor/pixelart/draw/`: the document session, the grid codec, the ops and the renderer;
- `editor/command/type/SpriteEditCmd.*`;
- `editor/mcp/tools/SpriteDrawTools.cpp`;
- the `schema::anyOf` builder in `editor/mcp/McpSchema.h`;
- the drawing sections of `docs/mcp/guide/sprites.md` (`jasper://docs/sprites`, shared with S44
  and S45).

## 1. Goal

An agent can draw pixel art the way a pixel artist does: it reads a small region, draws with a
batch of exact operations, looks at a zoomed picture, and fixes the worst spot. All of it happens
in the same `.aseprite` file a person has open in Aseprite. Each batch is one step in the
editor's undo history, named after the agent, so a person's Ctrl+Z works on agent edits. The
engine hot-reloads the file, so the Sprite Preview and the game show every stroke.

The tools learn from tenten (atomic op batches, an echo after each edit, PNG renders, diffs,
errors with hints) and fix its weak spots:
- reads take a region and echoes are capped, instead of returning the whole grid every time;
- ops are checked against closed per-op field tables before anything changes;
- the grid alphabet has no off-by-one;
- palettes larger than 62 colours and RGBA sprites are supported;
- edits are stored as dirty-rect patches, not whole-document snapshots;
- a person's save in Aseprite is never silently overwritten.

The drawing algorithms are Pixelorama's (pixel-perfect lines, scanline fill, hue-shift shading,
symmetry, cleanEdge/rotxel/OmniScale rotation), ported to CPU code that is the reference.

## 2. Scope

**In scope**
- Six MCP tools in group `sprites`: `create_sprite`, `get_sprite`, `read_sprite_pixels`,
  `edit_sprite`, `render_sprite`, `diff_sprite`.
- The document session and its revisions, the grid codec (chars, hex2, legend, hex8), and region
  reads with size caps.
- The op set:
  - pixel, line (pixel-perfect), rect, ellipse (Zingl), polygon, fill (scanline, tolerance,
    contiguous or global), replace, dither (Bayer), clear and paste (grid);
  - outline, inline and shade (simple, hue shift, palette ramp);
  - shift (wrap), flip, rotate (90° steps exact; cleanEdge, rotxel, OmniScale, nearest),
    scale (nearest, Scale2x, Scale3x), mirror, copy (between frames and layers) and select
    (masks);
  - batch-level symmetry;
  - structure ops: layer, frame, tag, slice, palette, canvas, cel.
- `SpriteEditCmd`: dirty-rect undo on the project history, with conflict-safe undo.
- The conflict policy for files saved from Aseprite while an agent edits them.
- Lints after a draw (S46), the hooks that let the Sprite Preview show live changes, and the
  agent guide page.
- Editing plain PNG sprites (one frame and one layer; RGBA, or indexed for palette PNGs) with
  the same pixel ops.

**Out of scope**
- An in-editor painting canvas or sprite editor for people. Aseprite is the people's tool (owner
  decision, 2026-11-04). The Sprite Preview panel is S44's and read-only.
- AI or hosted image generation. Cleanup of existing art is S46's (`fix_pixel_art`).
- The `.aseprite` codec itself (S44), palette files and colour maths (S43), atlas packing (S45),
  effects (S48), tilemap layers (S50 reads them; S47 refuses to edit a tilemap layer's cels with
  `unsupported_layer`).
- Runtime pixel access from Lua or C++ (D8).
- Copying tenten's TypeScript: its design is taken and every algorithm is re-specified here.

## 3. Current state (specs/pixel-art 1b1c34fbd, read)

- **No sprite drawing exists.** `editor/pixelart/` does not exist, and nothing reads or writes
  `.aseprite`. That is S44's codec, landing first. The art tools today are:
  - `generate_placeholder_art` (`editor/mcp/tools/AssetTools.cpp:4174`);
  - `set_sprite_frames` and `set_sprite_animation` (`WorldTools.cpp:5645`, :5668), which edit
    the Sprite component, not image files;
  - `edit_tilemap` (`WorldTools.cpp:5596`), whose ASCII grid with a legend is a precedent for
    this spec's grids;
  - `sample_view_color` (`EditorToolsTools.cpp:605`).
- **Undo** (`editor/command/`):
  - `Command` (`Command.h`) has `execute`/`undo`, `describe` (:95), `touchedPaths` (:50),
    `undoRefusal`/`redoRefusal` (:81-82; a non-empty string refuses the step) and `memoryBytes`
    (:103).
  - `TerrainMapPatchCmd` (`type/TerrainMapPatchCmd.h:19`) keeps a `TerrainMapPatch {region,
    beforePixels, afterPixels}` (`util/TerrainMapUtils.h:73-82`). It rewrites the whole PNG on
    every apply (`TerrainMapPatchCmd.cpp:18-66`) and never checks whether the file changed
    outside the editor.
  - `WriteFilesCmd` (`type/WriteFilesCmd.h:31`) keeps the whole old and new contents, with
    `setName` (:52) and `afterChange`.
  - The project history is `Project::projectHistory` (`Project.h:928`). The single Ctrl+Z
    timeline over the project and scene histories is S24's `UndoTimeline`
    (`editor/command/UndoTimeline.h`).
- **Agent attribution:** S24 landed. `McpServer` sets an `UndoOrigin{Agent, client, callId,
  tool}` for Edit tools (`McpServer.cpp:1327-1335`). `McpUndo::foldCall` (`McpUndo.h:34`) folds a
  call into one step named "<client>: <title> (<subject>)", which goes into the result's `undo`.
  `UndoHistoryWindow` shows agent steps with a robot icon (`UndoHistoryWindow.cpp:25`, :158-159).
  `undo {expect_top_id}` lets an agent undo only its own call (`CoreTools.cpp:323-324`).
- **File watching** (S05): `FileChange.origin` is `External` or `Editor`
  (`engine/core/io/watch/FileWatcher.h:26`). `ScopedExpectedWrite`
  (`FileWatchService.h:199`) marks the editor's own writes, so their echo is dropped
  (`ExpectedWriteLedger::isEchoOfOurWrite`, `ExpectedWriteLedger.h:76`). `HotReloadService`
  still receives editor writes (`deliverEditorWrites`, `reload/HotReloadService.cpp:143`), so
  textures reload after them. Atomic writes are `io::writeFileAtomic`, `io::publishFile` and
  `io::makeTemporarySiblingPath` (`engine/core/io/AtomicFile.h:88-107`).
- **MCP:**
  - `McpSchema.h` has no `anyOf` builder; tools assemble it by hand
    (`SpriteNormalsTools.cpp:1591`, `SynthesisTools.cpp:840-854`).
  - `ToolDef` (`McpRegistry.h:41-68`) carries `annotations`, `activity`, `group`,
    `writesFiles` and `allowedWhilePlaying`.
  - Images go in `ToolResult::images` (`McpCall.h:24-30`).
  - Typed problems `{kind, field, message, fix}` exist only inside the sprite normals tool
    (`SpriteNormalsTools.cpp:125-130`). S44 lifts them into `editor/pixelart/PixelProblem.h`.
  - There is no `sprites` group (`McpToolGroups.cpp:16-37`).
  - The tool-name verbs are listed in `tools/mcp/tests/test_tool_conventions.py:37-55`. `draw`
    and `describe` are not on it; `edit`, `get`, `read`, `render`, `diff` and `create` are.
- **Lua:** editor scripts call any tool with `Editor.call(tool, args)`
  (`editor/util/EditorScripts.cpp:283`). Runtime Lua has no pixel access to textures.

## 4. Reference

### 4.1 tenten (`/tmp/pixelart-research/tenten`, MIT, © 2026 debuff): what we take and what we fix

| tenten | Where | S47 |
|---|---|---|
| 12 tools over open documents (`tt_new`, `tt_open`, `tt_describe`, `tt_draw`, `tt_undo`, `tt_render`, `tt_diff`, …) | `src/server.ts` | Six tools over project files, with no "open" step. The session opens a file on first use and follows it on disk (R2). Undo is the editor's (R32), not a private stack (`src/editor.ts`, which clones the whole document per batch, `cloneDoc`, and caps the stack at 200) |
| An atomic batch of ops, rolled back on any failure | `src/editor.ts` `batch` | Kept (R12), plus the index of the failing op and a `fix` (R35) |
| Every edit replies with the **whole frame grid** | `server.ts` `tt_draw` | An echo of the dirty rect only, capped at `echo_max` pixels (R13) |
| `tt_describe` always includes a full grid | `server.ts` `describe` | `get_sprite` returns structure only. Pixels come from `read_sprite_pixels {rect}` with a size cap (R9) |
| Untyped ops: `z.array(z.record(z.string(), z.unknown()))` | `server.ts` `tt_draw` | Closed per-op field tables checked before any change, with the op names in the schema (§6.2, D4) |
| Alphabet `.0123…9a…zA…Z`: **index 1 is written `0`** (off by one), indices ≤ 61 only | `src/grid.ts:3-16`, `docs/format.md:40` | `.` = 0, `1`–`9` = 1–9, `a`–`z` = 10–35, `A`–`Z` = 36–61; `hex2` for up to 256 entries; `legend` and `hex8` for RGBA (R6–R8) |
| Indexed only, up to 255 entries; RGB `.aseprite` is quantized on open | `src/doc.ts`, `server.ts` `tt_open` | Indexed and RGBA kept as they are, with no silent conversion (R1) |
| 4-connected exact flood fill | `src/ops.ts` `flood` | Pixelorama's scanline fill with tolerance, 4/8-connectivity, contiguous or global (R18) |
| Bresenham and Zingl's ellipse | `ops.ts` `line`, `ellipse` | Kept, with Pixelorama's pixel-perfect mode on polylines (R16), plus rect thickness/radius and polygons |
| `outline` (4-neighbour), `shift` (wrap), `flip`, `replace`, `copy`, `grid` (transparent chars don't overwrite) | `ops.ts` | Outline/inline with widths and brush shapes (R19). `paste` with an explicit keep character `-` (R7). Copy between any frame and layer, with a rect and a destination (R23) |
| `diamond` (2:1 isometric footprint) | `ops.ts` | Dropped as an op: `polygon` with four points draws it, and the guide gives the recipe |
| `tt_diff`: changed count, ≤ 64 cells, a grid with `-` for unchanged | `src/grid.ts` `diffPixels` | Kept, limited to the changed bounding box. Its grid is a valid `paste` patch (R30) |
| `tt_render` PNG of all frames with labels, scale 8 | `src/render.ts`, `server.ts` | Integer scale fitted to `max_width`, light/dark/checker backdrops, onion skin, last-edit highlight, scene context (R27) |
| Errors `CODE: message\nHint: …` | `server.ts` `fail`, `doc.ts` `TentenError` | `{kind, message, fix, field, op_index}`; every `fix` gives the valid range (R44) |
| `tt_watch` re-export, `tt_preview` browser window | `server.ts` | Not needed: the editor hot-reloads the file and the Sprite Preview panel shows it (R41) |

### 4.2 Pixelorama (`/tmp/pixelart-research/pixelorama`, MIT, © 2019-present Orama Interactive and contributors): algorithms ported

| Algorithm | Pixelorama source | Port notes |
|---|---|---|
| Pixel-perfect line | `src/Classes/Drawers.gd:37-59` (`PixelPerfectDrawer.set_pixel`): a two-entry window; when `P − corner` is diagonal and `P − neighbour` is orthogonal, the middle pixel is restored | One window per mirrored copy. Pixelorama shares one window across symmetry copies (`BaseDraw.gd:796-816`), a quirk we fix. Only for thickness 1 (`BaseDraw.gd:423`) |
| Lines | `Geometry2D.bresenham_line` (Godot built-in), stroke segments without repeated first points (`BaseDraw.gd:465-490`), thick lines as square stamps (:561-570) | Our own integer Bresenham with both ends |
| Scanline flood fill | `src/Classes/FloodFillObject.gd:29-174` (from Allegro 4.2.1 `flood.c`), `Bucket.gd:408-523`; tolerance `DrawingAlgos.similar_colors` (`DrawingAlgos.gd:754-764`), per-channel Chebyshev on RGBA | 4-connected as upstream, plus an 8-connected option. Global mode = `COLORS` fill (`ColorReplace.gdshader`) as a CPU pass |
| Shading | `src/Tools/DesignTools/Shading.gd:19-119`: SIMPLE (`lightened`/`darkened`), HUE_SHIFTING (hue limits 60°/240°, saturation floor 0.1 on lighten, value floor 0.1 on darken), COLOR_REPLACE (one step along palette entries) | The clamps are ported literally (:87-119) |
| Symmetry | `src/Autoload/Tools.gd:642-687`; axis `x_symmetry_point = size.x − 1` (doubled coordinates); diagonals reflect about pixel centres | Same axes. A custom axis is given in pixels with halves allowed |
| Zingl ellipse, rounded rect | `DrawingAlgos.gd:233-295` (`get_ellipse_points`), :157-230 | Filled ellipses use spans between border points, not the O(n²) membership test (:298-326) |
| Outline / inline | `src/Shaders/Effects/OutlineInline.gdshaderinc`: width 0–10, brush diamond/circle/square, `inside` | A CPU port with the same neighbourhoods |
| Scale3x | `DrawingAlgos.gd:329-381` | **Not copied as is.** It writes `E8 = f` instead of `i`, and its block is offset one pixel up-left (upstream bugs). We implement the published AdvMAME3x and AdvMAME2x rules (Pixelorama has no Scale2x) |
| rotxel, fake RotSprite, nearest rotate | `DrawingAlgos.gd:528-712`, :715-729, :732-749; pivot `size/2`, −0.5 on even sizes for the CPU paths (`RotateImage.gd:40-73`) | Ported |
| cleanEdge | `src/Shaders/Effects/Rotation/cleanEdge.gdshader` (MIT © 2022 torcado; SLOPE and CLEANUP defines, `similarThreshold`, `lineWidth`) | A CPU port of the per-pixel shader with nearest clamp-to-edge sampling. The CPU code is the reference, and there is no GPU path |
| OmniScale | `src/Shaders/Effects/Rotation/OmniScale.gdshader` (MIT © 2015-2016 Lior Halphon; no-AA version by deakcor) | A CPU port, used for rotation (`omniscale`) |
| Bayer dithering | `assets/dither-matrices/bayer{2,4,8,16}.png`, `Gradient.gdshaderinc:77-151` | Matrices generated by the standard recursion (no assets). Tiling uses absolute canvas coordinates, so Pixelorama's tiling error on sizes that are not multiples of N is avoided |
| Indexed model | `src/Classes/ImageExtended.gd:156-189` (index + 1 in an R8 image, nearest by RGBA distance) | Not taken. We keep Aseprite's model through S44: index 0 is the transparent index of indexed sprites, and nearest-colour snapping uses S43 OKLab, not RGBA distance |

Copied or ported files keep their headers. `NOTICE.md` gets entries for tenten, Pixelorama,
cleanEdge (torcado) and OmniScale (Lior Halphon, deakcor).

## 5. Requirements

### Functional — documents, revisions, addressing

- **R1. Sprite files.**
  - `.aseprite`/`.ase` through S44's codec: indexed, RGBA or grayscale colour mode; layers
    including groups; frames; cels including linked ones; tags; slices; the palette; user data.
    Grayscale is edited as RGBA and written back as grayscale.
  - `.png` as a one-frame, one-layer document: RGBA, or indexed for a PNG with a PLTE chunk.
    Every ancillary chunk (`jasper.*` recipes included) is kept byte for byte, and only IHDR,
    PLTE, tRNS and IDAT are rewritten.
  - The colour mode is never changed silently. `canvas {action: "convert"}` does it on request
    (R25).
  - A file using a feature S44 cannot write back losslessly is refused with
    `unsupported_feature`, naming the feature, before anything changes.
- **R2. Session** (`editor/pixelart/draw/SpriteDocCache`).
  - Each file is decoded once and kept, keyed by project-relative path. Up to 16 documents or
    256 MB are kept, least recently used first out.
  - Each document holds:
    - its **revision**: the first 12 hex digits of the SHA-256 of the file bytes;
    - the file's size and mtime;
    - the last edit (rects, origin, revision);
    - per MCP session, the revision that session last saw.
  - Every call stats the file first. A changed size or mtime re-hashes it, and a changed hash
    reloads it (R34).
  - `sprite` (every tool) is a project-relative path or an S04 GUID. Paths outside the assets
    directory are refused with `outside_assets`.
- **R3. Addressing.**
  - Coordinates `x, y` are pixels from the top-left, from 0. A rect is `[x, y, w, h]`.
  - **Frames are numbered from 1, as Aseprite shows them.** A person and an agent then say the
    same number. `frame` is an integer, a list, a tag name or `"all"`.
  - **Layers** are a name, a group path (`"Body/Arm"`) or an index from 0 at the bottom, as
    Aseprite stores them. The default is the topmost visible pixel layer.
  - **Colours:**
    - an integer palette index;
    - `"#rrggbb"`, `"#rrggbbaa"`;
    - or `"transparent"`.

    In indexed mode, a hex colour must be in the palette exactly. Otherwise the op fails with
    `color_not_in_palette`, and the `fix` names the nearest index (S43 OKLab) and the `palette`
    op that would add it.
  - Ops outside the canvas are clipped. An op that changes nothing adds the warning
    `nothing_drawn` with its index.
- **R4. Determinism.** The same file bytes and ops give byte-identical files, echoes and images
  on one platform:
  - integer algorithms;
  - `double` for rotation and shading, with fixed rounding (half away from zero);
  - no time in any output;
  - S44's writer is deterministic (fixed zlib level, no timestamps).
- **R5. Limits:**
  - canvas ≤ 4,096 × 4,096 (`create_sprite`, `canvas` resize);
  - ≤ 256 frames;
  - ≤ 256 layers;
  - ≤ 256 ops per batch;
  - ≤ 16,384 points per op;
  - ≤ 4,096 grid rows.

  Each limit is in the error's `fix`. A file larger than 4,096 × 4,096 (S44 reads up to 16,384²)
  can be read and rendered; editing it fails with `canvas_too_large`.

### Functional — the grid codec

- **R6. Indexed alphabet:** `.` = 0, `1`–`9` = 1–9, `a`–`z` = 10–35, `A`–`Z` = 36–61. Each index
  is its own digit, so `5` is index 5.
  - On input `0` is accepted for index 0 too.
  - Palettes with more than 62 entries use `hex2`: two lowercase hex digits per pixel, `00`–`ff`,
    with `--` for keep.
  - `encoding: "auto"` picks `chars` when every index in the region is ≤ 61, else `hex2`.
- **R7. Paste rules** (`paste` op):
  - Rows are top first. A short row or a missing row keeps the rest.
  - `-` and space mean **keep** (`--` in `hex2`), and every other character writes, `.`
    included.
  - `blend: "over"` also keeps where the pasted pixel is index 0 or alpha 0 (tenten's rule).
  - An unknown character fails with `bad_grid_char`, giving the row, the column and the valid
    characters.
  - A grid that `diff_sprite` returns (R30) pastes back as a patch.
- **R8. RGBA encodings:**
  - `legend`: reads return `legend: {char: "#rrggbbaa"}`, with characters from the R6 alphabet
    (without `.`) given in order of first appearance in the region, row by row. `.` is alpha 0.
    `paste` takes a `legend` the same way.
  - Past 61 distinct colours in a region, `auto` switches to `hex8`: eight hex digits per pixel,
    `--------` for keep.
  - For indexed reads, `used` lists the palette entries the region contains (`{"5": "#3b5dc9"}`),
    so an agent needs no second call to know the colours.

### Functional — reading

- **R9. `read_sprite_pixels`** `{sprite, frame (default 1; up to 16 frames), layer (default
  "composite" = visible layers blended as Aseprite does), rect (default the canvas), encoding,
  max_pixels (default 4,096, at most 16,384)}`:
  - It returns `{revision, mode, size, encoding, frames: [{frame, layer, rect, rows}], used |
    legend}`.
  - The text is a header line and the rows, so an agent reads the grid once.
  - A region larger than `max_pixels` × frames fails with `region_too_large`. The `fix` suggests
    tiles (for example "rect [0,0,64,64] then [64,0,64,64]") or `render_sprite` to look.
  - A frame that has no cel on the layer reads as all `.`.
- **R10. `get_sprite`** `{sprite}`: structure, no pixels. It is S44's `aseprite::describe(doc,
  cels)` JSON (S44 R9, the shape `get_asset_info` also embeds), plus `revision`, `used_by` and
  `last_edit`:
  - `{path, revision, size, mode, palette: ["#rrggbb[aa]", …] (index = position), transparent_index,
    layers: [{index, name, group, visible, opacity, blend_mode, kind}], frames: [{frame,
    duration_ms}], tags: [{name, from, to, direction, repeat}], slices: [{name, rect, pivot,
    center}], cels: [{frame, layer, bbox, linked_to}], used_by: [GameObjects whose Sprite uses
    it, via S04's dependency graph], last_edit: {by, revision, rects}}`.
  - The text is one compact paragraph. Grid characters are not listed (R6 is fixed, and the guide
    states it).

### Functional — editing

- **R11. `edit_sprite`** `{sprite, ops[1–256], frame, layer (defaults for ops that name none),
  symmetry, echo ("dirty" | "composite" | "none"), echo_max (default 1,024 pixels, at most
  4,096), image (bool: also render the touched frames, R27 defaults, with the change outlined),
  lint (true | [lint ids]), expect_revision}`.
- **R12. Atomic.** The ops run in order on a copy-on-write working document. Any failure leaves
  the file and the session as they were and returns the failing `op_index`. Success writes the
  file once, atomically (`io::writeFileAtomic` under `ScopedExpectedWrite`), and pushes one
  `SpriteEditCmd` (R32).
- **R13. The echo.** The union dirty rect per (frame, layer) of the edited layer's pixels, in R6/R8
  encoding; with `echo: "composite"`, the composite instead. The result holds:
  - `revision`, `previous_revision`, `applied`;
  - `changed {pixels, rects: [{frame, layer, rect}]}`;
  - `echo`;
  - `structure` (as `get_sprite`, only when a structure op ran);
  - `warnings`, `lints`, `undo {id, name}`.

  When the echo would exceed `echo_max` pixels, the rects beyond it are listed in
  `echo_truncated` with a hint (`read_sprite_pixels {rect}` or `render_sprite`).
- **R14. The selection** (the `select` op, R23) holds for the rest of the batch. Every pixel op
  after it changes only selected pixels, unless the op sets `ignore_selection`. Selections do not
  persist across calls, so a later call is never clipped by a forgotten mask.
- **R15. Symmetry** (batch-level `symmetry {x, y, diagonal}`):
  - It applies to `pixel`, `line`, `rect`, `ellipse`, `polygon`, `fill` seeds, `paste` and
    `shade` points.
  - `x: true` mirrors about `(w − 1)/2`, the canvas centre (Pixelorama `Tools.gd:642-687`).
    `x: 15.5` sets the axis in pixels.
  - `diagonal: "xy" | "x_minus_y" | "both"` reflects about pixel centres, combined as Pixelorama
    combines them (`Tools.gd:646-669`).
  - Each copy is drawn once (duplicates removed) and has its own pixel-perfect window.

### Functional — the ops (three typed families, §6.2)

- **R16. Paint:**
  - `pixel {points}`.
  - `line {points ≥ 2, thickness 1–16, pixel_perfect (default true)}`:
    - a polyline of integer Bresenham segments with both ends drawn and joints drawn once;
    - `pixel_perfect` removes every L-corner along the stroke (`Drawers.gd:37-59`; thickness 1
      only);
    - thicker lines stamp a square brush.
  - `rect {rect, fill, thickness, radius}`: rounded corners as `DrawingAlgos.gd:157-230`.
  - `ellipse {rect, fill, thickness}`: Zingl's rectangle-bounded ellipse, so odd and even sizes
    come out as artists expect. A filled ellipse fills spans.
  - `polygon {points ≥ 3, fill}`: edges by the line rule; the fill is even-odd at pixel centres.

  All take `color`.
- **R17. Paste:** `paste {rows, at [x, y], encoding, legend, blend}`, R7.
- **R18. Fill:**
  - `fill {at, color, tolerance 0–255 (default 0), contiguous (default true), connectivity 4|8
    (default 4), sample "layer"|"composite"}`:
    - the scanline flood of `FloodFillObject.gd:29-174`;
    - "similar" is `max(|Δr|, |Δg|, |Δb|, |Δa|) ≤ tolerance` (`DrawingAlgos.gd:754-764`, scaled
      to 0–255); in indexed mode with tolerance 0 the indices are compared;
    - `contiguous: false` replaces every similar pixel of the region (Pixelorama's COLORS mode);
    - `sample: composite` decides the area on the composite and paints the target layer
      (`_fill_merged_area`).
  - `replace {from, to, tolerance}`: every pixel similar to `from`, in the target rect or the
    selection.
  - `dither {colors [a, b], pattern bayer2|bayer4|bayer8|bayer16|checker, level 0–1 | gradient
    {from [x,y], to [x,y], kind linear|radial}, rect}`:
    - a pixel gets `b` when the level exceeds `(M[x mod N][y mod N] + 0.5)/N²`, else `a`;
    - x, y are canvas coordinates, so the pattern lines up across ops and frames;
    - the matrices follow the standard recursion `M2n = [[4M, 4M+2], [4M+3, 4M+1]]`.
  - `clear {rect}`: transparent, within the rect, the selection or the cel.
- **R19. Outline and inline:** `outline` / `inline {color, width 1–10 (default 1), shape
  diamond|circle|square (default diamond)}`, as `OutlineInline.gdshaderinc`:
  - an outline paints transparent pixels next to the shape;
  - an inline paints the shape's own border pixels;
  - the neighbourhood is diamond `w − |i|`, circle `⌊√((w+0.5)² − i²)⌋` or square `w`;
  - diamond with width 1 is tenten's 4-neighbour outline.
- **R20. Shade:** `shade {mode simple|hue_shift|ramp, direction lighten|darken, amount 0–100
  (simple, default 10), hue −180–180, saturation −100–100, value −100–100 (hue_shift, defaults
  10/10/10), ramp [indices] | ramp_name (S43 palette ramp), points | rect | match color,
  palette_mode snap|add}`.
  - The formulas are `Shading.gd:19-119`, with its clamps.
  - Transparent pixels never change.
  - `ramp` moves a pixel one step along the ramp and leaves colours outside it unchanged.
  - In indexed mode the results of `simple` and `hue_shift` snap to the nearest palette colour
    (S43 OKLab). With `palette_mode: "add"` they are appended instead, and a full palette fails
    with `palette_full`.
- **R21. Transform** (target: `rect`, else the selection's bounding box, else the cel; the canvas
  size never changes):
  - `shift {by [dx, dy], wrap}`.
  - `flip {axis x|y}`.
  - `rotate {angle, method auto|cleanedge|rotxel|omniscale|nearest, pivot}`:
    - multiples of 90° are exact pixel permutations whatever the method;
    - `auto` uses `cleanedge` for any other angle;
    - the pivot defaults to the target's centre (Pixelorama: `size/2`, −0.5 on even sizes for
      rotxel and nearest);
    - every method only selects source colours, so an indexed sprite stays in its palette.
      AC5 checks that no new colour appears.
  - `scale {factor 2|3|4, method nearest|scale2x|scale3x}`: AdvMAME2x/3x with exact equality
    (index equality in indexed mode, or `tolerance`); 4 is Scale2x twice; the result is centred
    on the target and clipped.
  - `mirror {axis x|y, from left|right|top|bottom, at}`: copies one half onto the other.
- **R22. Rotation is a CPU reference.** cleanEdge, rotxel and OmniScale run per output pixel on
  the CPU with nearest clamp-to-edge sampling of the source. There is no GPU path in the editor,
  and nothing runs in games (D9).
- **R23. Copy and select:**
  - `copy {from {frame, layer, rect}, at, to_frame, to_layer, blend replace|over, cut}`: copies
    within or between frames and layers, using the selection when `from.rect` is absent;
    `cut: true` clears the source.
  - `select {shape rect|ellipse|polygon|color|all|none|invert, mode replace|add|subtract|intersect,
    rect, points, at, tolerance, contiguous}`: `color` is a magic wand with R18's similarity
    rule.
- **R24. Structure** (`{op, action, …}`):
  - `layer`: add, remove, rename, move, set (visible, opacity, blend mode, group).
    - The last layer cannot be removed (`last_layer`).
  - `frame`: add, remove, move, duplicate, set (duration).
    - `add` copies the previous frame's cels unless `copy: false`, and links them with
      `link: true`.
    - The last frame cannot be removed (`last_frame`).
  - `tag`: set (name, from, to, direction forward|reverse|pingpong|pingpong_reverse, repeat,
    color), remove.
  - `slice`: set (name, rect, pivot, center for 9-slice), remove.
  - `cel`: link, unlink, clear, set (opacity, z_index).
  - Removing frames shifts tags as Aseprite does: a tag left empty is removed with a warning.
- **R25. Palette and canvas** (structure ops):
  - `palette`:
    - set (a list), add, replace an index, `load` an S43 `.palette`
      (`remap: keep_indices|nearest`);
    - in RGBA mode the palette is reference only;
    - index 0 of an indexed sprite stays its transparent index (`index_0`).
  - `canvas`:
    - resize (`size`, `anchor` centre|top_left|…);
    - crop (`rect`);
    - convert (`mode indexed|rgba`): indexed to RGBA is exact; RGBA to indexed fails with
      `too_many_colors` unless every colour is in the palette or `snap: true`, in which case it
      snaps with S43 OKLab nearest and reports how many pixels changed.
  - Plain PNGs accept only `palette` and `canvas`. Any other structure op fails with
    `png_structure`, whose `fix` is "create_sprite {from: <the png>} makes an .aseprite".

### Functional — creating, rendering, diffing

- **R26. `create_sprite`** `{path (.aseprite|.ase), size [w, h], mode indexed|rgba (default
  indexed), palette (an S43 .palette path, an S43 built-in name, or a hex list; default the
  project's Default Palette (`palettes.default`, S43 R4b), else the project's only `.palette` asset
  when it has exactly one, else S43's built-in Sweetie 16), frames 1–256 (1), duration_ms (100),
  layers [names] (["Layer 1"]), from (a .png or GIF/APNG path: its frames and colours; indexed
  when ≤ 255 colours), overwrite}`.
  - Writing is one project-history step "Create Sprite (hero.aseprite)" (`WriteFilesCmd` with
    `setName`).
  - The result is `get_sprite`'s.
- **R27. `render_sprite`** `{sprite, frames (default all, ≤ 64, or a tag), layers
  visible|all|[names], scale (0 = auto: the largest integer scale that fits `max_width` in at
  most `columns`), columns (default auto ≤ 8), backdrops (default ["checker"]; also "dark",
  "light" or hex values: one row of frames per backdrop), onion {before 0–3, after 0–3, opacity
  0.1–0.9}, grid (pixel grid lines at scale ≥ 6), labels (default true: frame number, duration,
  tag), highlight (last_edit | none | rect), max_width (128–2,048, default 1,024), context
  {entity, view game|scene}}`.
  - It returns one PNG image plus `{scale, layout: [{frame, x, y, w, h}], last_edit}`.
  - Pixels are drawn at integer scale with NEAREST only. The "dark" and "light" backdrops are
    S42's project setting `pixel_art.backdrops` (S42 §6.2; default `#1a1c2c` and `#f4f4f4`).
  - `context` adds a second image: the Game or Scene view around that GameObject (S44's Sprite
    rendering through the existing screenshot path, cropped to its bounds plus 50 %, shown at
    integer scale). It is a job when the view needs a frame rendered.
- **R28. The last edit highlight:** `highlight: last_edit` outlines the previous `edit_sprite`
  rects of this file in a 1-pixel magenta frame (at the render's scale) in every frame panel
  they touch.
- **R29. `diff_sprite`** `{sprite, against ("previous" | a revision | {sprite: path} | {frame:
  n}), frames, layer (default "composite"), rect, max_cells (default 64, at most 1,024), grid
  (default true), image}`.
  - It returns `{same, changed, palette_same, structure_changes: [...], frames: [{frame, changed,
    bbox, cells: [{x, y, from, to}], rows}], image?}`.
  - `previous` is the state before this file's newest `SpriteEditCmd`.
  - A revision is any state this editor session can rebuild from the history's patches. An
    unknown one fails with `revision_unknown`, listing the known ones.
  - `{frame: n}` compares frames of one sprite (animation deltas).
  - Sizes must match, or the error is `size_mismatch` with both sizes.
- **R30. Diff grids are patches.** `rows` covers only `bbox`, with `-` for unchanged pixels and
  the new value elsewhere. `paste {rows, at: bbox.xy}` re-applies the change to another frame or
  file.

### Functional — undo, conflicts, lints, errors

- **R31. Lints after a draw:** `lint: true` (every applicable lint) or a list of ids runs S46's
  `lint::lint` on the composite of each touched frame, with the sprite's palette. `flicker` runs
  on the touched tag's frames.
  - The result lists only non-PASS lints plus `counts`.
  - Spots use R3 coordinates and frames, so a `doubles` spot can be fixed with
    `pixel {points:[[x, y]], color: "transparent"}`.
  - Lints never block a write.
- **R32. Undo** is one `SpriteEditCmd` per call on the project history
  (`editor/command/type/SpriteEditCmd.{h,cpp}`), in the pattern of `TerrainMapPatchCmd`. It
  holds:
  - the path;
  - `revision_before`, `revision_after`;
  - `CelPatch {layer id, frame, rect, before, after, existed_before, existed_after}` per touched
    cel;
  - `StructurePatch {before, after}` (layers, frames, tags, slices, palette, canvas as compact
    JSON, plus every removed cel's pixels).

  Execute and redo apply the after side, undo the before side. Each re-encodes through S44's
  writer and writes atomically.
  - `describe()` is "Edit Sprite (hero.aseprite: line, fill ×2)", from the op kinds.
    `McpUndo::foldCall` makes it "Claude Code: Edit Sprite (hero.aseprite: line, fill ×2)" in
    the Undo History, with the robot icon.
  - `memoryBytes()` is the patch bytes: 1,000 one-pixel edits cost ≤ 200 KB, against tenten's
    whole-document snapshot per batch.
- **R33. Conflict-safe undo.** `undoRefusal()` returns a refusal when the file's revision is not
  `revision_after`, because a person saved the file from Aseprite since. The refusal reads:
  "hero.aseprite changed outside the editor after this step; undoing would overwrite that
  change". `redoRefusal()` does the same against `revision_before`. A refused step stays in the
  history and is drawn greyed with that tooltip. Undo never overwrites a person's save.
- **R34. Conflict policy for edits:**
  1. The file changed on disk since this MCP session last saw it (an `External` change, or
     another session's edit). The first `edit_sprite` from this session fails with
     `sprite_changed {current_revision, last_seen_revision, changed_by: external|editor,
     changed_rects}`:
     - `changed_rects` comes from a diff against the last-seen state when it can be rebuilt;
     - the `fix` is "read the region again, then resend with expect_revision: <current>";
     - the session has already reloaded, so the next call works on the person's version.
  2. `expect_revision` given and not current fails the same way, every time. Agents that must
     never race send it on every call.
  3. When a person saves from Aseprite after agent edits, Aseprite's in-memory document wins. It
     does not reload the file, so the agent's edits made after Aseprite opened the file are gone
     from disk.
     - The editor detects this. The external revision is not a descendant of the agent's last
       revision.
     - The Console warns: "hero.aseprite was saved from outside the editor over 2 agent edits
       (Claude Code: …); those steps can no longer be undone".
     - The Sprite Preview shows the same notice.
     - The guide tells people to reopen a file in Aseprite after an agent has edited it. The
       Sprite Preview shows "Edited by <agent> — reopen it in Aseprite before editing there"
       after every agent edit (R41).
- **R35. Errors** are the family's `PixelProblem {kind, message, fix, field}` (S44's
  `editor/pixelart/PixelProblem.h`) plus `op_index` for ops.
  - The kinds:
    - files: `sprite_missing`, `not_a_sprite`, `outside_assets`, `unsupported_feature`,
      `unsupported_layer`;
    - addressing: `no_layer`, `no_frame`, `no_tag`, `last_layer`, `last_frame`;
    - colour: `bad_color`, `color_not_in_palette`, `palette_full`, `index_0`, `too_many_colors`;
    - grids and sizes: `bad_grid_char`, `grid_too_wide`, `region_too_large`, `too_many_ops`,
      `size_mismatch`;
    - state: `canvas_too_large`, `sprite_changed`, `revision_unknown`, `png_structure`, `output_exists`,
      `file_locked` (an S29 lock held by someone else).
  - Every `fix` states the valid range or values, for example "frame is 1–8 or a tag (walk,
    idle)", "x is 0–31", or "palette has indices 0–15; add colours with {op: "palette", action:
    "add"}".
  - The CLI's schema check (`jasperengine-editor call edit_sprite`) catches the top-level type and
    key errors and an unknown `op` name before the editor sees them; an op's fields are checked by
    the handler before anything changes (`bad_op_field`, listing that op's fields, §6.2).

### Platform & backends

- **R36. Editor-only CPU code** on Linux, Windows and macOS. Edited files are ordinary
  `.aseprite`/`.png` assets: S44 imports them and S14 cooks them for every backend and target as
  before. No shader, no runtime code, no player size change.
- **R37. Portable C++20** within `tools/check-cxx-subset.py`. The rotation ports use only
  `std::sin`/`std::cos`/`std::atan2`/`std::hypot` on `double`, rounded half away from zero.

### Performance (Release editor, build server A)

- **R38. Budgets:**
  - `edit_sprite`, 50 paint ops on a 128 × 128 × 8-frame indexed file: ≤ 10 ms of ops, ≤ 40 ms
    in total including encode and write. S44's writer re-compresses only the touched cels and
    copies the others' compressed bytes (S44 R7).
  - 512 × 512 × 32-frame RGBA file, one cel touched: ≤ 120 ms.
  - `read_sprite_pixels` of 4,096 pixels: ≤ 2 ms.
  - `render_sprite`, 64 frames of 64 × 64 at scale 4: ≤ 60 ms.
  - `rotate` with cleanEdge on 128 × 128: ≤ 40 ms.
  - Lints after a draw: S46 R38.
  - Stat on every call: ≤ 0.1 ms; a re-hash only on a stat change.
- **R39. No idle cost.** Nothing runs between calls: no polling, no watcher thread of its own.
  The session uses S05's watcher events.

### Editor UX (human)

- **R40. Undo History and Ctrl+Z.** Agent edits are ordinary steps of the project history in
  S24's single timeline. A person's Ctrl+Z (or Edit > Undo) undoes the newest one, and the
  history panel shows "Claude Code: Edit Sprite (hero.aseprite: …)" with the robot icon. A step
  refused by R33 shows its reason as the tooltip and in the status bar when Ctrl+Z hits it.
- **R41. Live view** (hooks for S44's Sprite Preview panel):
  - `SpriteEditEvents::edited(path, revision, rects, origin)` fires after every write, undo and
    redo.
  - The panel redraws within one frame and outlines the changed rects in the agent colour, fading
    over 3 s (setting **Preferences > Pixel Art > Highlight Agent Edits**, key
    `preferences.pixel_art.agent_edit_highlight`, default on; editor preferences use the
    `preferences.` prefix, so it never collides with S42's project keys `pixel_art.*`).
  - The panel shows a line "Edited by Claude Code: line, fill — 2 s ago" with **Undo**, which
    undoes that step only when it is still the newest (`expect_top_id` semantics, otherwise
    disabled with the reason).
  - `SpriteEditEvents::externalSave(path)` fires for R34.3.
  - Every write, undo and redo of an `.aseprite` file calls `AsepriteImporter::reimport(path)`
    (S44 R23), because S05's self-write suppression hides the editor's own writes from the
    watcher. The Sprite Preview, the Game view and the Scene view then update exactly as after a
    person's save.
- **R42. Project window and Inspector:** the file's thumbnail refreshes after each edit (the
  `filesWritten` refresh path, `ProjectFileWrites.h:42`). S44's `.aseprite` Inspector shows "Last
  edited by Claude Code, 2 min ago" from `last_edit`.
- **R43. No in-editor painting.** People edit in Aseprite. S44's "Edit in Aseprite" stays the one
  entry point, and S47 adds no canvas tools to the panel (owner decision).

### Agent UX (MCP, CLI, Lua)

- **R44. Tools** (group `sprites`):

  | Tool | Activity, annotations | Allowed while playing |
  |---|---|---|
  | `create_sprite` | Edit, writesFiles | yes (art can change during Play; the game hot-reloads it) |
  | `get_sprite` | Read, readOnly, idempotent | yes |
  | `read_sprite_pixels` | Read, readOnly, idempotent | yes |
  | `edit_sprite` | Edit, writesFiles, destructive (undoable) | yes |
  | `render_sprite` | Read, readOnly, idempotent (`context` may run as a job) | yes |
  | `diff_sprite` | Read, readOnly, idempotent | yes |

  Each tool's text is a compact summary, with the grid for reads and echoes.
  `structuredContent` holds the data. Images are `ToolResult` images.
- **R45. The loop the guide teaches** (`jasper://docs/sprites`, its Drawing sections):
  1. `get_sprite`.
  2. `render_sprite` to look. An image of a 32 × 32 sprite at 8× costs about 90 tokens; a full
     grid costs about 450.
  3. `read_sprite_pixels {rect}` on the part to change.
  4. `edit_sprite` with a batch and `lint: true`.
  5. `render_sprite {highlight: "last_edit"}`.
  6. Fix the worst lint spot, and repeat.

  The guide also covers:
  - `expect_revision` when a person may be editing;
  - `undo {scope: "timeline", expect_top_id}` to take back only your own step;
  - the alphabet, with an example;
  - building animations (`frame {action: add}` + `copy` + small edits; `tag`);
  - shading with ramps instead of new colours;
  - "never fight a person's save: re-read".
- **R46. Lua.** No runtime API. Editor scripts reach every tool with `Editor.call("edit_sprite",
  {...})`, which is enough for batch jobs ("recolour every enemy with this ramp"). See D8.
- **R47. CLI.** No new subcommand. `jasperengine-editor call edit_sprite '{...}'` (S31) runs any
  tool with schema validation, and `--project` runs a one-shot headless editor. The images go to
  files, as `call` does for every tool.

### Data & compatibility

- **R48. No new formats.** Files are standard Aseprite (S44 writes what Aseprite 1.3 reads) and
  PNG. S47 adds no asset type, no `.meta` key and no scene field. The session cache is memory
  only. The one new setting is `preferences.pixel_art.agent_edit_highlight` (editor preferences,
  default true). Old projects are unaffected.
- **R49. Lossless round trip.** Opening and writing a file with no op changes nothing: same bytes.
  S44's writer re-emits every unchanged chunk's original bytes (S44 R7), so an edit changes only
  the chunks it touched. A batch that changes no pixel and no structure writes nothing, pushes no undo step, and
  returns `applied` with `changed.pixels: 0`.

## 6. Design constraints & decisions

- **D1. Six tools, not twelve.** Opening, saving and closing are implicit, because the project
  file is the document. Undo is the editor's. Export is S45's and S44's. Watching and preview are
  the editor itself. That leaves one slot of S47's seven for later.
- **D2. `edit_sprite`, not `draw_sprite`.** The conventions name a tool that runs several
  operations `edit_<thing>`, and `draw` is not a listed verb. `get_sprite` replaces
  "describe_sprite" for the same reason. The descriptions start with "A sprite file (.aseprite,
  .png)…", so no agent confuses them with the Sprite component tools (`set_sprite_frames`).
- **D3. One atomic batch carries pixel and structure ops**, so "add a frame, copy the last one,
  move the arm, tag it" is one call and one undo step. Splitting structure into its own tool
  would save listing characters but break that atomicity.
- **D4. Ops are open in the schema and closed in the handler.** A typed `anyOf` of three closed op
  families would cost ≈ 4,800 characters, over the 1,500 a new tool may take: `budget.json`'s
  `exceptions` are for tools that predate the limit (and lower caps), and the conventions send area
  detail to the guide, never into a raised limit. So each `ops` item is `schema::anyObject` with one
  declared property, `op` (the enum of the 27 op names, required); the fields per op are in
  `jasper://docs/sprites`, and the handler checks them before anything changes (`bad_op_field`,
  listing the op's own fields; S46 D4 does the same for its recipe). `edit_sprite` stays ≤ 1,450
  characters and needs no `exceptions` entry. The CLI still validates the top level and the `op`
  names.
- **D5. A builder for `anyOf`.** `schema::anyOf(desc, {schemas…})` is added to `McpSchema.h`,
  emitting only the allowed keyword `anyOf`, for `diff_sprite`'s `against` (a string or an object).
  The hand-written `anyOf`s of `SpriteNormalsTools.cpp:1591` and `SynthesisTools.cpp:840-854`, and
  S46's `fix_pixel_art.output` (same wave), may move to it, which is not required here.
- **D6. Echo the change, look with images.** Grids cost about 1 token per 2–3 characters
  (assumed, measured by AC15). A 512 × 512 PNG costs about 350 tokens (assumed:
  width × height / 750, Anthropic's published estimate for Claude vision). So the default loop
  reads small regions as text and looks at whole sprites as images.
- **D7. Patches, not snapshots, and refusal over overwrite.** Dirty-rect patches make undo cheap.
  Comparing revisions makes it safe: a step that would overwrite a person's work is refused, not
  forced.
- **D8. No runtime pixel access.** Games run cooked textures, often GPU-compressed (S14 BC7, ASTC
  or ETC) and not resident in CPU memory. A CPU pixel API at runtime would need a separate
  dynamic-texture feature with its own memory and upload budgets on mobile and web. The use case
  here is authoring: `Editor.call` covers editor automation, and runtime effects are S48's
  shaders.
- **D9. CPU rotation only.** It is an authoring operation on at most 4,096² pixels, deterministic
  and testable, with no GPU readback. A shader version would need all five backends to agree bit
  for bit, and it would gain nothing.
- **D10. Frames are numbered from 1.** Aseprite's UI numbers frames from 1. When the person says
  "frame 3", the agent's `frame: 3` is the same frame. Layers keep Aseprite's stored order
  (0 = bottom), and the guide says so next to the frame rule.

### 6.1 MCP tools and the budget (S31)

| Tool | Parameters | Estimated listing chars |
|---|---|---|
| `create_sprite` | `path`, `size`, `mode`, `palette` (string\|array), `frames`, `duration_ms`, `layers`, `from`, `overwrite` | ≤ 850 |
| `get_sprite` | `sprite` | ≤ 350 |
| `read_sprite_pixels` | `sprite`, `frame` (integer\|array\|string), `layer`, `rect`, `encoding` (enum), `max_pixels` | ≤ 750 |
| `edit_sprite` | `sprite`, `ops` (1–256 `anyObject` items with a required `op` enum of the 27 ops; fields per op in `jasper://docs/sprites`, checked by the handler, §6.2), `frame`, `layer`, `symmetry` (object `{x, y, diagonal}`), `echo` (enum), `echo_max`, `image`, `lint` (boolean\|array), `expect_revision` | ≤ 1,450 (no `exceptions` entry) |
| `render_sprite` | `sprite`, `frames`, `layers`, `scale`, `columns`, `backdrops`, `onion` (object), `grid`, `labels`, `highlight`, `max_width`, `context` (object) | ≤ 1,200 |
| `diff_sprite` | `sprite`, `against` (string\|object), `frames`, `layer`, `rect`, `max_cells`, `grid`, `image` | ≤ 750 |

- S47 adds 6 tools and about 5,350 characters to `sprites`, which S44, S45 and S47 share.
  - S44 has 1 tool (`set_sprite_import_settings`, ≤ 1,100) and S45 2 (≤ 2,200), so the group
    comes to 9 tools and about 8,650 characters, inside `group_default` (12 tools, 15,000; ledger
    "MCP budget"). No `groups` or `exceptions` entry is needed.
  - If S45's tools grow past the remaining room, the first fix is to move descriptions into
    `jasper://docs/sprites`. A `groups` entry is the last resort, decided by the owner, and limits
    are never raised inside a test.
- The group row is S44's (S44 §6.1, added in wave 5A): "Aseprite import settings, sprite atlases,
  and agent sprite drawing on .aseprite files (jasper://docs/sprites)."
- Core listing +0. The shared Sprites line of the guide's `## Pixel art` section (S44 §6.1) gets one
  clause from S47: "draw: get_sprite, read_sprite_pixels, edit_sprite, render_sprite; frames from 1; grid '.'=0,
  1-9, a-z=10-35, A-Z=36-61". That is about 110 bytes, within the guide's limits.
- **Naming.** The structure tool is `get_sprite` (S44 uses this name too), because `describe` is
  not on the conventions' verb list (`test_tool_conventions.py:37-55`) and `get_` is the listed
  verb for a read.
- Gates: `tool_budget.py --check`, `test_tool_conventions.py` (closed op objects, `anyOf` only,
  snake_case), `test_tool_coverage.py`, and the hidden-until-enabled test (AC16).

### 6.2 The op fields (the handler's closed table per family; the schema lists only `op`, D4)

```text
paint:      op ∈ {pixel, line, rect, ellipse, polygon, paste, fill, replace, dither, clear,
                  outline, inline, shade}
            points [[x,y]…], rect [x,y,w,h], at [x,y], color, from, to (integer|string),
            fill, pixel_perfect, ignore_selection (bool), thickness 1–16, radius 0–64,
            width 1–10, tolerance 0–255, connectivity 4|8, contiguous (bool),
            sample layer|composite, colors [a,b], pattern bayer2|bayer4|bayer8|bayer16|checker,
            level 0–1, gradient {from, to, kind linear|radial}, rows [string], encoding
            chars|hex2|legend|hex8, legend {char: hex} (anyObject), blend replace|over,
            shape diamond|circle|square, mode simple|hue_shift|ramp, direction lighten|darken,
            amount 0–100, hue −180–180, saturation, value −100–100, ramp [int], ramp_name,
            match, palette_mode snap|add, frame, layer
transform:  op ∈ {shift, flip, rotate, scale, mirror, copy, select}
            by [dx,dy], wrap, axis x|y, angle −360–360, method auto|cleanedge|rotxel|omniscale|
            nearest|scale2x|scale3x, pivot [x,y], factor 2–4, from_side left|right|top|bottom,
            source {frame, layer, rect}, at [x,y], to_frame, to_layer, cut, blend,
            shape rect|ellipse|polygon|color|all|none|invert, select_mode replace|add|subtract|
            intersect, rect, points, tolerance, contiguous, frame, layer
structure:  op ∈ {layer, frame, tag, slice, cel, palette, canvas}
            action add|remove|rename|move|duplicate|set|link|unlink|clear|load|resize|crop|convert,
            name, new_name, index, at, count, copy, link, duration_ms, visible, opacity 0–255,
            blend_mode (Aseprite's 19 modes), group, from, to (frames), direction
            forward|reverse|pingpong|pingpong_reverse, repeat 0–65535, color, rect, pivot,
            center, z_index, colors [hex], asset (.palette path), remap keep_indices|nearest,
            size [w,h], anchor (9 values), mode indexed|rgba, snap
```

The handler checks which fields each op allows, before any op runs. An unknown field, or one
another op uses, fails with `bad_op_field` and `op_index`, listing the op's own fields, so an open
schema item never hides a misplaced key. These tables are also the Drawing section of
`jasper://docs/sprites`.

### 6.3 Token budget (bytes per typical call; estimates, measured by AC15)

| Call | 32 × 32, 16 colours, 4 frames | 128 × 128, 32 colours, 8 frames | tenten for the same |
|---|---|---|---|
| `get_sprite` | ~0.6 KB | ~0.9 KB | `tt_describe` 1.5 KB / 17.6 KB (includes a full grid) |
| read one full frame | 32 × 35 = 1.1 KB + `used` 0.2 KB | 16.8 KB, refused by default (> 4,096 px); a 32 × 32 tile is 1.1 KB | full grid in every describe |
| `edit_sprite`, 8 ops, 16 × 12 changed | request ~0.5 KB, reply ~0.6 KB (echo 0.23 KB) | same ~1.1 KB; a large fill reports `echo_truncated` (~0.2 KB) | reply = full frame: 1.1 KB / 16.8 KB |
| `render_sprite` all frames | 4 × 256 px at 8× ≈ 1,056 × 280 px ≈ 390 image tokens + 0.2 KB | 8 × 256 px at 2× in 4 columns ≈ 1,024 × 560 ≈ 760 tokens; 4× is ≈ 2,900 | `tt_render` png at 8×: 8,192 × 1,040 px, which the client shrinks, blurring the pixels |
| `diff_sprite previous` | ~0.4 KB | ~0.5 KB (bbox only) | `tt_diff` whole-frame grid 1.1 KB / 16.8 KB |
| **20-edit session** (1 get, 4 reads, 20 edits, 5 renders) | ≈ 20 KB text + ≈ 2,000 image tokens | ≈ 30 KB + ≈ 3,800 image tokens | ≈ 30 KB / ≈ 370 KB of grids |

At an assumed 2.5 characters per token, the 128 × 128 session costs about 12,000 text tokens
with S47 against about 150,000 with tenten's full-grid replies.

### 6.4 API sketch

```cpp
namespace jasperengine::editor::pixelart::draw {
    struct Revision { std::string id; uint64_t size = 0; int64_t mtimeNs = 0; };     // id: 12 hex of SHA-256
    struct DirtyRect { int frame = 1; int layer = 0; IntRect rect; };
    class SpriteDoc;                                         // wraps S44's aseprite::Document or a PNG
    class SpriteDocCache {                                   // main thread
    public:
        std::shared_ptr<SpriteDoc> open(Project*, const std::string& sprite, PixelProblem& problem);
        Revision lastSeen(const std::string& path, uint64_t mcpSession) const;
    };
    enum class GridEncoding : uint8_t { Auto, Chars, Hex2, Legend, Hex8 };
    std::vector<std::string> encodeRows(const PixelImage&, IntRect, GridEncoding, nlohmann::json& legendOrUsed);
    bool decodeRows(std::span<const std::string> rows, GridEncoding, const nlohmann::json& legend,
                    PixelPatch& out, PixelProblem& problem);         // '-'/' ' keep
    struct EditRequest { nlohmann::json ops; int frame = 1; std::string layer; nlohmann::json symmetry;
                         std::optional<std::string> expectRevision; };
    struct EditResult { Revision before, after; std::vector<DirtyRect> changed; int pixels = 0;
                        std::vector<std::string> opKinds; std::vector<PixelProblem> warnings; };
    // applies to a copy-on-write working document; nothing changes on failure (op_index in problem)
    std::optional<EditResult> applyOps(SpriteDoc& working, const EditRequest&, PixelProblem& problem, int& opIndex);
    PixelImage renderSheet(const SpriteDoc&, const RenderOptions&, nlohmann::json& layout);
}
namespace jasperengine::editor {
    class SpriteEditCmd : public Command {                   // project history
        // execute/undo apply CelPatch/StructurePatch, re-encode through S44, write atomically
        std::string undoRefusal() const override;            // file revision != revisionAfter
        std::string redoRefusal() const override;            // file revision != revisionBefore
        size_t memoryBytes() const override;                 // patch bytes
        std::string describe() const override;               // "Edit Sprite (hero.aseprite: line, fill ×2)"
    };
}
namespace jasperengine::editor::mcp::schema { Json anyOf(const std::string& description, std::vector<Json> schemas); }
```

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R6–R8) | `editor/tests/pixelart/GridCodecTest.cpp`: index 1 encodes as `1`, 10 as `a`, 36 as `A`, 61 as `Z`, 62 forces `hex2`; `0` and `.` both decode to 0; `-` and space keep; `over` keeps index 0; `legend` assigns characters in first-appearance order and switches to `hex8` at 62 colours; a bad character reports row, column and the valid set. Red control: tenten's alphabet (index 1 = `0`) fails the first assertion |
| AC2 (R16) | `DrawOpsTest`: Bresenham lines with both ends; a polyline with `pixel_perfect` has no L-corner (S46 `doubles` lint = PASS) and without it the joints have them; Zingl ellipses match golden masks for 1×1 to 9×7 (odd and even); rounded rects match `DrawingAlgos.gd` spans; polygon fill even-odd on a star |
| AC3 (R18) | `FillOpsTest`: scanline fill equals a reference BFS on 200 random masks (4 and 8 connectivity); tolerance per channel; `contiguous: false` replaces disconnected areas; `sample: composite` fills the area seen on the composite into the target layer; indexed tolerance 0 compares indices |
| AC4 (R19, R20) | `ColorOpsTest`: outline/inline widths 1–3 per shape match hand-made masks; hue shift on `#3b5dc9` lighten/darken gives the values of the `Shading.gd` formulas (computed by hand in the test, hue limits 60°/240° hit); ramp moves one step and leaves other colours; indexed `snap` stays in the palette, `add` appends, a full palette fails `palette_full` |
| AC5 (R21, R22) | `TransformOpsTest`: rotate 90/180/270 are exact permutations and four 90° rotations are the identity; cleanEdge/rotxel/omniscale at 30° and 45° produce **no colour that was not in the source** (indexed sprite stays indexed) and match committed golden PNGs (`editor/tests/pixelart/golden/rotate_*.png`, reviewed by eye once); Scale2x/3x match the published AdvMAME rules on their test patterns (and the Pixelorama E8 quirk is absent); shift with wrap, flip, mirror |
| AC6 (R15, R23) | `SymmetryTest`: x/y/diagonal mirrors with even and odd sizes and a custom axis 15.5 match Pixelorama's formulas; a mirrored pixel-perfect stroke has no corners on either copy; `select` modes add/subtract/intersect; pixel ops clip to the selection; copy between frame 1 layer A and frame 3 layer B with `cut` |
| AC7 (R24, R25) | `StructureOpsTest`: add/remove/move layers and frames keep cels; removing frames shifts tags as Aseprite; linked cels on `frame add {link: true}`; `canvas convert` rgba→indexed fails `too_many_colors` then works with `snap` and reports changed pixels; PNG structure ops fail `png_structure` |
| AC8 (R12, R13, R49) | `tools/mcp/tests/test_sprite_draw.py`: a batch whose 3rd op fails leaves the file bytes and revision unchanged and names `op_index: 2` and a `fix`; a successful batch's echo covers exactly the dirty rect; `echo_max: 16` truncates with rects listed; an empty batch effect writes nothing and pushes no undo step; open + no-op leaves bytes identical |
| AC9 (R32, R40) | `test_sprite_draw.py::test_undo`: one `edit_sprite` = one project-history step named "<client>: Edit Sprite (hero.aseprite: line, fill)" in `get_undo_history` with origin agent; `undo {scope: timeline, expect_top_id}` restores the previous bytes exactly and redo the new ones; `memoryBytes` for 1,000 single-pixel calls ≤ 200 KB (`get_undo_history.memory`) |
| AC10 (R33, R34) | `test_sprite_conflict.py`: after an agent edit, the test rewrites the file as an external program would (new bytes, then waits for the watcher): `undo` is refused with the R33 message and the bytes stay the person's; the agent's next `edit_sprite` fails `sprite_changed` with `changed_rects`, and the call after it succeeds on the new content; `expect_revision` stale fails every time; a Console warning names the overwritten agent steps |
| AC11 (R9, R10, R29, R30) | `test_sprite_read.py`: `read_sprite_pixels` of a 64×64 region on a 128×128 sprite works, the full frame fails `region_too_large` with the tile hint; `used` lists only the region's colours; `get_sprite` lists layers (with groups), frames from 1, tags, slices, `used_by`; `diff_sprite previous` after an edit lists the changed cells and its `rows` pasted onto the old state reproduces the new state byte for byte |
| AC12 (R27, R28) | `test_sprite_render.py`: the PNG decodes; scale is the largest integer fitting `max_width`; every sprite pixel maps to a `scale × scale` block of one colour (no filtering); 3 backdrops give 3 rows; onion skin pixels have the given opacity; the last-edit outline encloses exactly the previous rects; `context` returns a second image containing the sprite's pixels at integer scale |
| AC13 (R31) | `test_sprite_draw.py::test_lint_after_draw`: drawing an L-corner on a line with `lint: ["doubles"]` returns WARN after 3 such corners with the spots; fixing them by `pixel` with `transparent` at the spots returns no non-PASS lint |
| AC14 (R26) | `test_sprite_create.py`: `create_sprite` indexed 32×32 with an S43 palette, 4 frames, 2 layers → S44's reader gets the same structure; `from` a GIF of 6 frames keeps delays and colours; `overwrite: false` on an existing file fails `output_exists`; one undo deletes the file |
| AC15 (§6.3, R38) | `test_sprite_budget.py` measures the text bytes of every row of §6.3 on generated 32×32 and 128×128 fixtures and fails if any exceeds its estimate by > 25 %; `tools/perf/sprite_draw_bench` on server A checks R38 (results table appended to Deviations when a budget is missed) |
| AC16 (§6.1) | `test_tool_budget.py` (`edit_sprite` ≤ 1,500 with no `exceptions` entry), `test_tool_conventions.py` (`edit_sprite` op items are `anyObject` with a declared `op` enum; no `oneOf`), `test_tool_coverage.py`, `gen_tool_docs.py --check`, `test_protocol.py`; the six tools absent by default and listed after `tool_groups {enable: ["sprites"]}`; `jasperengine-editor call edit_sprite` with an unknown `op` fails client-side, and with a misspelled op field fails with `bad_op_field` naming the op's fields while the file bytes stay unchanged |
| AC17 (R41, R42) | `test_sprite_preview_live.py` (xvfb, isolated editor, S44's panel open on the file): after `edit_sprite` the panel's texture revision equals the new revision within 2 frames and the highlight item for the rect exists (`find_ui_item`); the "Edited by" line's Undo works when on top and is disabled after a later user step; screenshot in the evidence |
| AC18 (R4) | `DeterminismTest`: a 100-op script (every op kind) run twice and at 1 and 8 job workers gives identical file bytes and echo text |

## 8. Dependencies & conflicts

- **Depends on:**
  - **S44:**
    - `aseprite::Document` read/write with lossless round trip of unmodified files (R49), and
      cel-level re-encode that copies untouched cels' compressed bytes (R38);
    - `PixelImage`;
    - the composite rule;
    - the `AsepriteSprite` hot reload;
    - the Sprite Preview panel, which subscribes to `SpriteEditEvents` (R41);
    - "Edit in Aseprite".
  - **S43:** `engine/core/pixelart/Palette.h` (the asset, built-ins, ramps; `ramp_name` resolves
    through `findRamp`) and `PixelColor.h` (`PaletteMatcher::nearest` with `ColorMetric::OkLab`
    for every snap, R3, R20, R25).
  - **S46 M1** (same wave): `lint::lint`. `PixelProblem` is S44's.
  - **S42** (wave 5B): `pixel_art.backdrops`.
  - Existing: S03 (atomic writes), S04 (GUIDs, `used_by`), S05 (watcher, `ScopedExpectedWrite`),
    S24 (project history, timeline, attribution, `expect_top_id`), S29 (locks: `file_locked`),
    S31 (`call`, groups, budget).
- **Blocks:** nothing hard. S48 lands earlier and composes its own contact sheets; S50 (same wave)
  may reuse the paint ops for tileset editing by agents (not required).
- **Name coordination:**
  - S47 owns the tool names `create_sprite`, `get_sprite`, `read_sprite_pixels`, `edit_sprite`,
    `render_sprite` and `diff_sprite`.
  - S44 should not add another tool that describes an `.aseprite` file's structure. Its
    import-only fields (import settings, generated clips) can be added to `get_sprite`'s result.
- **Merge hotspots:**
  - `McpSchema.h` (the `anyOf` builder);
  - `McpToolGroups.cpp` (the `sprites` row);
  - `McpInstructions.cpp` (the digest line);
  - `NOTICE.md`;
  - S44's Sprite Preview panel files (the event subscription).

## 9. Risks & defaults

- **Aseprite overwrites agent edits when a person saves without reloading.** It cannot be
  prevented from outside Aseprite. Default: detect it, warn (R34.3), refuse unsafe undo (R33),
  and tell people to reopen the file. An Aseprite extension that reloads on change is a possible
  follow-up, not in scope.
- **Ops are not typed in the schema** (D4), so a client-side validator sees only `op` names.
  Default: the handler's closed per-op tables with `bad_op_field` before any change, and the
  tables in the guide. If typed ops are needed later, structure ops can move to a second tool in
  the group (10 tools, within 12 and 15,000 characters); that breaks D3's one-call atomicity, so it
  is not done now.
- **1-based frames may surprise agents** used to 0-based APIs. Default: 1-based (D10). Every
  frame error repeats the rule ("frames are 1–8, as Aseprite shows them"), and so does the guide
  digest line.
- **Lossless round trip depends on S44.** Default: if S44's writer cannot round-trip a feature
  (for example tilemap layers or an extension's user data), S47 refuses the file with
  `unsupported_feature` rather than dropping data.
- **cleanEdge and OmniScale ports are long** (≈ 340 and ≈ 300 shader lines). Default: golden PNGs
  reviewed by eye once, and the no-new-colour property tested on every run.
- **Token estimates are assumptions** (characters per token, image cost). Default: AC15 measures
  bytes; tokens are reported as estimates only.

## Milestones

- **M1** Session, revisions, grid codec, `get_sprite`, `read_sprite_pixels` (R1–R10); AC1, AC11
  (read part).
- **M2** `edit_sprite` with paint and paste ops, the per-op field tables, `SpriteEditCmd`, echoes,
  the `anyOf` builder (R11–R17, R32, R35, R49); AC2, AC8, AC9.
- **M3** Fill, dither, outline, shade, transforms, rotation ports, scale, select, copy, symmetry
  (R18–R23); AC3–AC6.
- **M4** Structure, palette and canvas ops, `create_sprite` (R24–R26); AC7, AC14.
- **M5** Conflicts, `render_sprite`, `diff_sprite`, lints after a draw (R27–R31, R33, R34); AC10,
  AC12, AC13, AC11 (diff part).
- **M6** Live view hooks with S44's panel, guide, budgets, determinism, performance (R4, R36–R48);
  AC15–AC18.

## Deviations

Recorded while implementing (branch `feature/s47-agent-sprite-drawing`):

- **R10, get_sprite's shape.** `aseprite::describe` numbers frames from 0 and is verbose; `get_sprite`
  builds the shape R10 lists directly from the document, with frames from 1 (D10), compact enough for
  §6.3. Fields it adds beyond the list are `used_by`, `last_edit`, `revision`.
- **§6.2 field names over R21/R23/R24 words.** The closed tables are what the handler checks: `copy`
  takes `source {frame, layer, rect}` (R23 says `from`), `select` takes `select_mode` (R23 `mode`), and
  `mirror` takes `from_side` (R21 `from`). `cel` also takes `frame` and `layer`, which the table leaves
  to the paint family.
- **R5/R24 structure limits.** A `frame add` or `layer add` past 256 fails with `too_many_ops` (the
  closest listed kind) naming the limit.
- **S46's lint library** (`editor/pixelart/lint/PixelLint`, `CraftLints`, `EffectLints`) is copied
  verbatim from `feature/s46-pixel-art-cleanup` (commit `66819890`) so `edit_sprite {lint}` runs against
  the real API; the batch landing keeps S46's copy. `SpriteDrawLint` is the one call site (R31).
- **`.png` deviations from R1:** 16-bit PNGs are refused (`unsupported_feature`, their precision would be
  lost). A PNG re-saves as 8-bit: a palette PNG at 1/2/4 bits becomes 8-bit indexed.
- **`create_sprite` palettes:** an indexed sprite's index 0 is its transparent index, so a named palette
  (S43, whose index 0 is a real colour) is written one index up with `#00000000` at 0, unless its entry
  0 is already clear or its `transparent` is 0. A palette of 256 real colours needs that slot, so it
  fails with `palette_full`.
- **`unsupported_feature` also covers** an indexed sprite with palette changes on later frames (S44 reads
  them, the ops keep one palette for all frames).
- **Rotation and scale conventions.** Positive angles turn clockwise on screen for every method.
  Pivots are in pixel-index coordinates (`pixel i`'s centre is `i`, halves allowed), default the
  target's centre `(w-1)/2`; Pixelorama's rotxel and odd-size pivots are off by half a pixel and were
  corrected. `AdvMAME3x` is the published rule set: the spec's E8 note was wrong (`E8 = F` is correct);
  the real Pixelorama bug is its block written one pixel up-left, and `SpriteRotateTest` pins that.
  OmniScale's "no-AA" corners still average in the shader; here each resolves to one source pixel.
- **The cleanEdge/OmniScale source map:** rotations return a per-output-pixel source index, so a rotation
  is exact on palette indices by construction (AC5).
- **R27 `context`** is a deferred reply (`captureView` renders the frame; the call waits on the main
  thread's frames), not an `McpJobs` job: it is at most a few frames long.
- **AC15 measures the JSON of `structuredContent`**, what a client like Claude Code shows the model (its
  text mirrors the grids): it is the bigger part of a reply.
- **R38's bench** is `editor/tests/pixelart/SpriteDrawBench.cpp` (`SpriteDrawBench.*`, printed `[bench]`
  lines, enforced by `JASPER_TEST_PERF_SLACK`), not `tools/perf/sprite_draw_bench`, matching the
  repository's bench suites.
