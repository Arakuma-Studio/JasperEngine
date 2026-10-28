# Sprite Normals Phase 5: paint correction layer

Status: binding spec for wave 3 (one implementer, after Phases 1-4 are merged into `sprite-normals`). Conforms to
`2026-10-27-sprite-normals-contract.md`; names below that come from Phase 2 are the P2 names of contract section 8
(Phases 3-4: `SpriteNormalsWindow`, `generate_sprite_normals`, `generate-sprite-normals`), and their exact signatures
are whatever Phases 2-4 shipped. This phase may extend those files.

## 1. Goal and non-goals

Generation from colour gets most pixels right and a few wrong: a painted highlight that became a bump, an eye that
became a dent, a belt that reads as a trench, a sleeve that should face left. Today the only fixes are global settings
or `regions`. After this phase:

- A **paint layer** sits on top of the generated normals: per pixel, a normal and a coverage. It is non-destructive:
  regenerating with any settings re-composites the same paint over the new result.
- Humans paint it in the Sprite Normal Map window with pixel-art brushes: **Angle** (a chosen direction), **Flatten**,
  **Smooth**, **Erase**, round/square, 1 px pencil up to 64 px, a colour **wand** fill (exact or tolerant, this frame or
  all frames), mirror X, Alt-click sampling, per-stroke undo.
- Agents paint it with a `paint` array of operations on `generate_sprite_normals` (and `--paint-json` on the CLI),
  selecting pixels by rectangle, pixel list, colour or frame, and see the effect in the same preview contact sheet.
- The paint lives **inside the normal-map PNG** (a second iTXt chunk), so it survives copies, renames and
  regeneration, needs no sidecar file, and costs nothing at runtime (decoders skip unknown chunks).

Non-goals: painting height (lift/sink) by hand (use `regions.lift`); painting the colour atlas; layers beyond one;
pressure sensitivity; painting during Play; a paint layer on the height map (the height map is never painted; the
guide says so).

## 2. Data model

### 2.1 Overlay

`PaintOverlay` (engine-free, `editor/spritenormals/SpriteNormalsPaint.h`):

```cpp
struct PaintOverlay {
    int width = 0, height = 0;          // == the normal output's size (the atlas for animated sources)
    std::vector<uint8_t> rgba;          // width*height*4. RGB: OpenGL-convention normal encoded
                                        // round((n*0.5+0.5)*255), z > 0 always. A: coverage 0..255.
    bool empty() const;                 // no pixel with A > 0
    size_t paintedCount() const;        // pixels with A > 0
};
```

- RGB is always OpenGL convention (green up), independent of the `directx` setting; the DirectX flip is applied
  after compositing (2.3), so toggling `directx` never invalidates paint.
- A pixel with A = 0 is unpainted; its RGB is ignored and is normalised to (128,128,255) when written, so the encoded
  overlay compresses well and equal overlays encode to equal bytes.

### 2.2 Storage: iTXt `jasper.spritenormals.paint`

Every normal-map PNG the generator writes with a non-empty overlay carries, after IHDR, after the
`jasper.spritenormals` recipe chunk (insert the paint chunk first, then the recipe: Phase 2's `insertRecipeChunk`
inserts right after IHDR and keeps chunks of other keywords), one iTXt chunk:

- keyword `jasper.spritenormals.paint`, compression flag 0, method 0, empty language tag, empty translated keyword
  (the same layout as the recipe chunk; the generalised recipe chunk writer/reader from Phase 2 takes the keyword);
- text = canonical compact JSON (keys in this order):

```json
{"format":"jasperengine.spritenormals.paint","version":1,"size":[W,H],"painted":N,
 "encoding":"png-base64","data":"<base64 (RFC 4648, standard alphabet, padded) of an RGBA8 PNG of the overlay>"}
```

- The embedded PNG is written with the same encoder as outputs (zlib via stb_image_write, no gAMA/sRGB/iCCP) from the
  normalised overlay of 2.1; an all-empty overlay writes **no** chunk (and drops an old one).
- Limits: the embedded PNG at most 8 MiB before base64 (`paint_too_large` otherwise; an overlay the size of the
  4096 x 4096 limit with every pixel painted in distinct values is the worst case and is refused with that message,
  which says to paint fewer pixels or split the sheet).
- The recipe (`jasper.spritenormals`) gains `"paint":{"painted":N}` when a paint chunk is written, and nothing
  otherwise; paint never changes the recipe's `source.sha1`, so painting is not staleness.
- Reading: `readPaint(pngBytes) -> std::optional<PaintOverlay>` plus a problem string. A chunk with another format,
  a version above 1, bad base64, a PNG that fails to decode, or a size that disagrees with `size` or with the PNG's own
  IHDR is reported as `paint_unreadable` (a warning: the file still works as a normal map; the overlay is treated as
  empty and the next write drops it after saying so). Unknown extra JSON keys are ignored (forward compatibility).
- Fast probing: `readPaintSummary(path)` reads only chunks before IDAT (the recipe reader's head scan) and returns
  `{painted, size}` from the JSON without decoding `data`, for `get_asset_info`, `view_asset` and the Project window.

### 2.3 Compositing

`composite(const Image& generatedGL, const PaintOverlay&, const Image& sourceRGBA, float alphaThreshold, bool directx)
-> Image` (RGBA8):

For each pixel:
1. Alpha = source alpha byte (unchanged passthrough).
2. If `alpha/255 <= alphaThreshold`: RGB = (128,128,255) regardless of paint (transparent pixels stay neutral; the
   overlay pixel is kept in storage so a later lower threshold shows it again).
3. Else, decode `g` (generated, OpenGL) and `p` (overlay) as `c/127.5 - 1` per channel, `a = A/255`.
   `a == 0`: result = `g` bytes unchanged (bit-exact; unpainted pixels are exactly the generation).
   `a == 1`: result = `p` bytes unchanged.
   Otherwise `m = (1-a)*g + a*p`; if `|m| < 1e-4` use `p`; else `n = m/|m|`; if `n.z < 0.0175` (about 89 degrees)
   set `n.z = 0.0175` and renormalise. Encode `round((n*0.5+0.5)*255)`, round half away from zero.
4. Then, if `directx`, green = 255 - green (the only place the convention flips).

The session composites in the worker run, after `generate()` and before `measure()`: when `spec.paint` is non-empty
or a kept overlay exists, the run calls `generate()` with `settings.directx = false`, then `applyPaintOps`, then
`composite(..., directx)` (which flips green). Otherwise it calls `generate()` unchanged, so unpainted output, both
conventions, stays byte-identical to Phase 2. `Request` is not changed.

### 2.4 Operations (shared by window, MCP and CLI)

```cpp
enum class PaintOpKind { Angle, Flatten, Smooth, Erase };
enum class PaintMode { Keep, Replace, Clear };                    // 2.5
enum class PaintTool { Navigate, Angle, Flatten, Smooth, Erase };  // the window (5.5)
struct PaintWhere {                     // at least one member; members intersect
    std::optional<std::array<int,4>> rect;             // x, y, w, h (w,h >= 1)
    std::vector<std::array<int,2>> pixels;             // <= 65,536 pairs
    std::vector<std::array<uint8_t,3>> colors;         // <= 16; matched on the source colour
    float tolerance = 0.0f;                            // 0..0.5 Oklab distance; 0 = exact RGB match
    std::optional<int> frame;                          // cell index; rect/pixels become cell-relative
};
struct PaintOp {
    PaintOpKind kind;
    PaintWhere where;
    std::array<float,3> direction{0,0,1};   // Angle only; OpenGL tangent space, normalised, z > 0
    float strength = 1.0f;              // 0..1
    int radius = 1;                     // Smooth only, 1..4 (box kernel (2r+1)^2)
};
struct PaintContext {                   // what selection and Smooth need
    const Image& source;                // RGBA8, output layout (the colour atlas for animated sources)
    const Image& generatedGL;           // before paint
    std::vector<Cell> cells;            // the output's frame cells (one full-image cell when none)
    float alphaThreshold;
};
struct PaintStats {
    struct Op { int selected = 0, changed = 0; };
    std::vector<Op> ops;                // one per op, in order
    int painted = 0, hidden = 0;        // painted pixels after the ops; of those, now transparent
};
struct PaintBrush {                     // the window's brush (5.1, 5.5)
    enum class Shape { Round, Square } shape = Shape::Round;
    int size = 1;                       // 1..64 px
    float strength = 1.0f;              // 0..1
    int smoothRadius = 1;               // 1..4
    enum class Fill { Brush, Wand } fill = Fill::Brush;
    float wandTolerance = 0.0f;         // 0..0.5
    bool wandContiguous = true;
    bool wandAllFrames = false;
    bool mirrorX = false;
    std::array<float,3> direction{0,0,1};
};
std::vector<Problem> applyPaintOps(PaintOverlay&, const std::vector<PaintOp>&, const PaintContext&,
                                   PaintStats* stats);   // stats: per op selected/changed counts
```

Selection: the pixels in `where` (intersection of the given members), clipped to the image (and to the cell when
`frame` is set), minus pixels with `alpha/255 <= alphaThreshold`. Colour matching uses the source pixel's RGB:
`tolerance == 0` means exact 8-bit equality, otherwise Oklab Euclidean distance <= tolerance (the same Oklab code as
`regions`). Selecting zero pixels is not an error: the op is reported with `selected: 0` and a warning
`paint_empty_selection` naming the op index.

Effects on each selected pixel (overlay value `p`, coverage `A`, `s = strength`):

| Op | New overlay |
|---|---|
| Angle | RGB = encode(direction); A = max(A, round(255*s)) |
| Flatten | RGB = (128,128,255); A = max(A, round(255*s)) |
| Smooth | RGB = encode(normalise(mean of the **current composite** (generated + overlay, before the DirectX flip) over the (2r+1)^2 box, counting only pixels that are opaque and in the same cell and same 8-connected island)); A = max(A, round(255*s)) |
| Erase | A = round(A*(1-s)); A = 0 also resets RGB to (128,128,255) |

Ops run in array order; Smooth reads the composite as it stands after the previous ops. Smooth bakes values: after a
regeneration with different settings, smoothed pixels keep their old smoothed normals (documented in the guide and
the window tooltip: "Smooth paints the current result; re-smooth after big setting changes").

Direction inputs: `[x,y,z]` (normalised by the reader; error if `z <= 0` after normalising or the length is 0) or
`{azimuth_deg, tilt_deg}`: azimuth 0 = right (+x), 90 = up the image (+y, OpenGL), counter-clockwise, any real
value (wrapped); tilt 0 = facing the viewer (+z), 0..85 (error above). `x = sin(tilt)cos(az)`, `y = sin(tilt)sin(az)`,
`z = cos(tilt)`.

Paint operates on the output layout. For animated sources that is the atlas; `frame` addresses atlas cells in
row-major order (the same order as the recipe's `frames.cells`).

### 2.5 Paint across regeneration

`paint_mode` (MCP/CLI; the window uses `keep` always):

| mode | Starting overlay | Then |
|---|---|---|
| `keep` (default) | the paint chunk of the existing file at the planned normal-map output path (if any) | apply `paint` ops |
| `replace` | empty | apply `paint` ops |
| `clear` | empty | `paint` must be empty (`invalid_paint` otherwise); the written file has no paint chunk |

When previewing (no `output`), `keep` reads the file the default output would write to, so a preview shows exactly
what writing would produce.

Size rules for a kept overlay: if its size differs from the new output size (the source was resized, frames changed
the atlas size), the call fails with `paint_size_mismatch` ("hero_normal.png has 214 painted pixels for a 64x32 image;
the new output is 96x32. Pass paint_mode: \"clear\" to drop the paint, or restore the source size.") and nothing is
written. If the size matches but the recipe's `frames.cells` differ from the new cells, the overlay is kept and the
result carries the warning `paint_layout_changed` (painted pixels may now land on other frames). Painted pixels that
are now transparent are counted as `hidden` and reported (`paint_hidden_pixels` warning when > 0).

## 3. Session integration (Phase 2 files, extended)

- `Spec` gains `std::vector<PaintOp> paint` and `PaintMode paintMode = PaintMode::Keep`; `fingerprint()` includes both
  (canonical serialisation of the ops).
- `checkSpec` validates ops (ranges, counts, direction) without decoding images, problems keyed `paint[i].field`.
- `plannedOutputs` reports, per normal output, `existingPaint: {painted, size}` from `readPaintSummary`.
- `prepare` loads the starting overlay (per 2.5) into `Prepared`; `SpriteNormalsRun` applies ops and composites (2.3,
  using the generated OpenGL image, the source/colour atlas and the cells); `Result` (or the run) carries the overlay
  and `PaintStats` (the window adopts the overlay).
- `encodeOutputs` writes the paint chunk after the recipe chunk for the normal map (never for atlas or height; order
  of insertion: 2.2).
- Metrics (`measure`, `assess`) are computed on the **composited** result; metrics gain `painted_pct` (painted and
  visible pixels / opaque pixels, one decimal) and the assessment adds "Painted: N px (P%)" when N > 0.
- `checkStaleness` is unchanged (paint is not a source). The regenerate arguments built from a recipe
  (`specFromRecipe`) use `paint_mode: keep` and no ops, so regenerating preserves paint.

## 4. MCP and CLI

### 4.1 `generate_sprite_normals` additions

Two top-level arguments (kept loose in the schema; validated by the handler, as `params` and `regions` are):

| arg | type / default | notes |
|---|---|---|
| `paint` | array of op objects, <= 256 ops | `{op: "angle"|"flatten"|"smooth"|"erase", where: {rect, pixels, colors, tolerance, frame}, direction: [x,y,z] | {azimuth_deg, tilt_deg}, strength, radius}` |
| `paint_mode` | `"keep"` (default) | `"replace"`, `"clear"` |

`paint` is refused (`invalid_paint`) when the call has more than one normal output (Tilemaps with several textures, CLI
batches); use one call per output.

Text as P3 §3.7 text (`Warning:` lines; errors as P3 §3.8). Errors (tool result `isError`): `invalid_paint` (names
`paint[i].field`, the valid range or enum), `paint_size_mismatch`, `paint_too_large`. Warnings:
`paint_empty_selection`, `paint_layout_changed`, `paint_hidden_pixels`, `paint_unreadable` (an unreadable chunk was
dropped).

Result additions (`structuredContent`): `paint: {mode, kept_from: <path or null>, ops: [{op, selected, changed}],
painted_pixels, hidden_pixels, painted_pct}`. The contact sheet gains, when the composite has painted pixels, a
magenta 1 px outline around painted pixels in the encoded-normal column only (the lit columns stay clean so the
agent judges the lighting), and the image caption says "magenta outline: painted pixels".

Description string addition (keep the tool definition growth under 250 characters): "paint: fix spots
(angle/flatten/smooth/erase by rect, pixels, colors or frame); kept on regenerate (paint_mode)."

`get_asset_info` on a generated normal map adds `sprite_normals.paint: {painted, size}` (from `readPaintSummary`)
when a paint chunk exists; `view_asset`'s summary line adds "Painted: N px".

### 4.2 `generate-sprite-normals` additions

- `--paint-json <file|->` (a JSON array of ops, the MCP shape; `-` reads stdin; a value starting with `[` is parsed as
  inline JSON), `--paint-mode keep|replace|clear`.
- JSON output gains the `paint` object of 4.1; text output prints `paint: N ops, P px painted (H hidden)`.
- Exit codes unchanged: invalid ops are usage errors (2), `paint_size_mismatch`/`paint_too_large` are failures (1).

### 4.3 `check --normals`

Adds `normals-paint-unreadable` (warning, at the PNG: the paint chunk cannot be read; fix: "regenerate with
paint_mode: clear, or repaint") and `normals-paint-size` (warning: paint size differs from the PNG size; same fix).

## 5. Window (Sprite Normal Map, Phase 4 file, extended)

### 5.1 Layout

A **Paint** foldout (`Theme::foldoutHeader`) in the settings column, after Advanced and before Output, open by
default once the overlay is non-empty:

```
Paint                                   Painted: 214 px   [Clear]
[Navigate][Angle][Flatten][Smooth][Erase]          (segmented, icons + tooltips with shortcuts)
Fill   (o) Brush  ( ) Wand
Shape  (o) Round  ( ) Square            Size [1 .. 64] px   (1 = pencil)
Strength [0 .. 1]                       Smooth radius [1..4] (Smooth only)
Wand   Tolerance [0 .. 0.5] (0 = exact)  [x] Contiguous   Scope (o) This frame ( ) All frames
[ ] Mirror X                            [x] Show paint    [ ] Highlight painted
Direction  (hemisphere disk)  Azimuth [  135 ]  Tilt [ 40 ]   (Angle only)
```

- Hemisphere disk: a square widget `ImGui::GetFrameHeight()*4` showing the encoded-normal colour wheel (each point of
  the disk drawn with the colour of the normal it stands for, CPU-generated once and cached as a texture), a dot at
  the current direction; click/drag sets it (centre = flat, rim = 85 degrees tilt); Shift snaps azimuth to 45 degree
  steps and tilt to 15 degree steps. Azimuth/Tilt are `sliderFloatInput` fields (degrees). Precedent: the Inspector
  Direction row's drag behaviour, but as a 2D disk because normals are a hemisphere.
- "Clear" clears the whole overlay as one window-undo step (confirm alert "Clear Paint?" when > 0 px painted, via
  `registerConfirmAlert`).
- "Show paint" off previews the generation alone (does not change what Save writes; tooltip says so).
- "Highlight painted" draws a 1 px magenta outline (`IM_COL32(255,0,255,255)`, which reads on every view) around
  painted, visible pixels on the canvas in every view mode.

### 5.2 Canvas input

With a paint tool other than Navigate active, left-drag paints, Alt+click samples the normal and Shift+wheel sizes the
brush. In Navigate, P4's bindings (light drag, Alt+click region pick, Shift+wheel light height) apply.

- With a paint tool active: left-drag paints; right/middle drag still pans; wheel zooms; **Shift+wheel** changes brush
  size by 1 px per notch (Ctrl+Shift+wheel by 4); Alt+left-click samples the composited normal under the cursor into
  the Direction picker and switches to Angle (Alt-click on a transparent pixel does nothing).
- Brush footprint: Round = pixels whose centres lie within `size/2` of the cursor pixel's centre (size 1 = one pixel,
  2 = 2x2 square, odd sizes centred on the cursor pixel, even sizes anchored up-left of the cursor); Square = `size x
  size` pixels with the same anchoring. The cursor draws the exact footprint outline snapped to the pixel grid.
- Stroke rasterisation: mouse samples are joined with Bresenham lines in pixel space, the footprint is stamped at every
  line pixel, and a per-stroke mask makes each pixel change at most once per stroke (strength never accumulates within
  a stroke; Erase with strength 0.5 halves coverage once per stroke).
- Wand: a click selects pixels whose source colour matches the clicked pixel's (exact, or within Tolerance), limited to
  the clicked pixel's frame cell (Scope: This frame) or every cell (All frames), and to the 8-connected region of the
  clicked pixel when Contiguous; then applies the current tool to that selection as one stroke. The wand applies on
  click; while hovering, the canvas outlines the would-be selection in cyan so the user sees it before clicking.
- Mirror X: every stamp is repeated at `x' = cellX + cellW - 1 - (x - cellX)` within the cursor's frame cell (the
  whole image when there are no cells); Angle mirrors the direction (`x -> -x`); the mirrored pixels join the same
  stroke.
- Painting is disabled (tools greyed, tooltip "Generate first") until a result exists, and while a generation runs
  strokes go to the overlay and are composited when the run finishes (the overlay is window state independent of the
  generated image).
- Every view mode (Color, Normal, Lit, Split) accepts strokes; Color view shows the colour with the highlight outline.

### 5.3 Shortcuts (gated like the window's others: no text input or popup, no Ctrl/Alt/Super except as listed)

| Key | Action |
|---|---|
| V | Navigate (no paint tool) |
| B | Angle brush |
| N | Flatten |
| M | Smooth |
| E | Erase |
| W | toggle Fill: Brush / Wand |
| X | toggle Mirror X |
| H | toggle Highlight painted |
| P | toggle Show paint |
| `-` / `=` | brush size -1 / +1 |
| Alt+click | sample direction |
| Ctrl+Z / Ctrl+Shift+Z, Ctrl+Y | undo / redo the last stroke (5.4) |

These do not clash with the Phase 4 window keys (1-4, G, Space, `,`/`.`, O, F, 0, Esc, Ctrl+Enter).
The Help > Keyboard Shortcuts text gains a "Sprite Normal Map: paint" block.

### 5.4 Undo

- Strokes are window-local until Save: a stroke stack of patches `{pixel indices, old RGBA, new RGBA}` (Clear is one
  patch; a wand fill is one patch). Cap: 256 steps or 64 MiB of patch data, oldest dropped first.
- Routing: when the Sprite Normal Map window was focused last and its stroke stack can undo (or redo), Ctrl+Z/Ctrl+Y
  and Edit > Undo/Redo act on the stroke stack (the Edit menu label reads "Undo Paint Stroke"); otherwise Phase 4's
  routing (the last assign's scene, else the Project). Implemented in `App` next to `undoGoesToProjectHistory` (a new
  branch asking `spriteNormalsWindow->canUndoPaint()/canRedoPaint()`).
- Save writes the normal map with its paint chunk through `writeOutputsUndoable`: one Project undo step that restores
  the previous file bytes (and so the previous paint) on undo. Saving does not clear the stroke stack; the window shows
  "Unsaved paint" next to Save while the overlay differs from the last saved/loaded overlay.
- Opening a file (Edit Normal Map..., the Inspector entries) or changing the output path loads the overlay from the
  target file's paint chunk (`keep`), clears the stroke stack, and reports `paint_size_mismatch` as a warning row with
  a "Clear Paint" fix button instead of failing (the window cannot save until it is resolved).
- Project switch and window close with unsaved paint ask "Discard Paint Changes?" (Phase 5 adds the prompt; a project
  switch asks before `resetForProjectSwitch()` runs).

### 5.5 Scripted API (tests, public on `SpriteNormalsWindow`)

```cpp
void setPaintTool(PaintTool tool);            // Navigate, Angle, Flatten, Smooth, Erase
void setPaintBrush(const PaintBrush&);        // shape, size, strength, smoothRadius, fill, wandTolerance,
                                              // wandContiguous, wandAllFrames, mirrorX, direction
bool paintStroke(const std::vector<std::array<int,2>>& path);   // canvas pixel path, as a mouse stroke
bool wandFill(int x, int y);
bool applyPaintOps(const std::vector<PaintOp>& ops, std::vector<Problem>* problems);
bool undoPaint(); bool redoPaint(); bool canUndoPaint() const; bool canRedoPaint() const;
void clearPaint();                            // no confirm alert from the scripted API
const PaintOverlay& paintOverlay() const;
size_t paintedPixelCount() const;
bool hasUnsavedPaint() const;
```

## 6. Docs

`docs/mcp/guide/sprite-normals.md` gains a section "Fixing spots: paint" (after "Reading the result"):
- when to paint vs `regions` (a material everywhere -> regions; a few pixels or one frame -> paint);
- the op table, direction conventions with a small table of azimuths (0 right, 90 up, 180 left, 270 down), and the
  "Smooth bakes" note;
- `paint_mode` and the size rule; the warnings and errors table rows;
- two worked examples: flatten a highlight by colour
  (`{"op":"flatten","where":{"colors":["#ffffff"],"tolerance":0.02}}`) and turn a sleeve left on frame 3
  (`{"op":"angle","where":{"frame":3,"rect":[10,12,4,6]},"direction":{"azimuth_deg":180,"tilt_deg":45}}`).

Keep `sprite-normals.md` <= 18 KB; trim P3 sections if needed.

`docs/mcp/TOOLS.md` regenerated; `docs/mcp/checks.md` gains the two codes; the README feature bullet's sentence gains
"…and paint corrections that survive regeneration". The agent guide line (McpInstructions) is unchanged (budget).

## 7. Tests

### 7.1 Core unit tests (`jasperengine-spritenormals-tests`, `editor/spritenormals/tests/SpriteNormalsPaintTest.cpp`)

1. Composite identity: empty overlay -> output byte-identical to the Phase 2 output for every golden fixture, with and
   without `directx`.
2. Composite full coverage: A = 255 -> painted RGB exactly; A = 0 -> generated exactly; A = 128 -> the documented
   lerp/renormalise value (hand-computed for 3 pixel pairs incl. near-opposite vectors and z clamping).
3. Transparent pixels stay (128,128,255) with painted overlay; alpha byte passthrough for 0..255 ramp.
4. DirectX: composite then flip equals flip(composite) only on green; painting with `directx` on stores OpenGL.
5. Each op: Angle/Flatten/Erase set exactly the documented values; strength max rule; Erase 0.5 twice = 0.25 coverage
   (rounded); Smooth on a hand-built 5x5 field equals the hand-computed box mean, ignores transparent pixels and pixels
   of another island/cell; Smooth reads previous ops in the same list.
6. Selection: rect clipping, pixels list, colours exact vs tolerance (Oklab), frame + cell-relative rect, intersection
   of members, transparent pixels excluded, empty selection -> warning not error.
7. Direction parsing: vector normalisation, z <= 0 error, azimuth/tilt conversions at 0/90/180/270 and wrap, tilt > 85
   error.
8. Storage: `encodePaintChunk` -> `readPaint` round trip bit-exact; normalisation of A = 0 RGB; empty overlay -> no
   chunk; canonical JSON key order; equal overlays -> equal bytes; corrupted base64 / wrong size / version 2 /
   unknown keys behaviour; 8 MiB limit message; `readPaintSummary` reads head only (works on a PNG truncated right
   after the chunk).
9. Determinism: ops applied with 1 and N threads (if the session parallelises compositing) give identical bytes.

### 7.2 Editor/session tests (`editor/tests/SpriteNormalsPaintSessionTest.cpp`)

1. keep: write with ops -> regenerate with different `strength` and no ops -> painted pixels unchanged where A = 255,
   unpainted pixels equal the new generation; chunk preserved; recipe `paint.painted` correct.
2. replace / clear semantics; clear with ops -> `invalid_paint`.
3. Source resized -> `paint_size_mismatch`, nothing written; with `clear` -> written without paint.
4. Frames changed with same size -> `paint_layout_changed` warning; newly transparent painted pixels -> hidden count.
5. Preview (no output) with `keep` composites the existing file's paint.
6. `writeOutputsUndoable` undo restores the previous bytes including the previous paint chunk.
7. Metrics on the composite (`painted_pct`; flattening every pixel -> flat_pct 100).
8. `specFromRecipe` regenerate keeps paint.

### 7.3 Window tests (`editor/tests/SpriteNormalsPaintWindowTest.cpp`, test-access friend as the VAT baker window)

1. `paintStroke` along a diagonal with size 1 round touches exactly the Bresenham pixels; size 3 round/square
   footprints; no double application within a stroke.
2. Mirror X within a cell; Angle direction mirrored.
3. `wandFill` exact vs tolerance, contiguous vs not, this frame vs all frames.
4. Stroke undo/redo, cap eviction, Clear as one step, `hasUnsavedPaint` transitions across save/undo.
5. Opening a file with a paint chunk loads it; size mismatch shows the fix row and blocks Save until cleared.
6. Alt-click sampling sets the direction to the composited normal.

### 7.4 E2E (`tools/mcp/tests/test_sprite_normals_paint.py`, CLI in `test_sprite_normals_cli.py`)

1. MCP preview with `paint` -> structuredContent `paint.ops[i].selected/changed` correct, contact sheet image present;
   nothing written.
2. Write with paint -> PNG has both chunks; `get_asset_info` reports `paint.painted`; `view_asset` summary line.
3. Regenerate via the returned regenerate args keeps paint; `paint_mode: clear` drops it.
4. Each error/warning kind once (invalid op enum, tilt 90, 257 ops, size mismatch, empty selection, layout changed,
   `paint` on a Tilemap with two tileset textures -> `invalid_paint`).
5. Undo after write restores the previous file (project history).
6. CLI `--paint-json` file, inline and stdin; `--paint-mode`; JSON output `paint` object; exit codes 0/1/2.
7. `check --normals` reports `normals-paint-unreadable` for a hand-corrupted chunk.

## 8. Acceptance criteria

- Unpainted output is byte-identical to Phase 2 output (all golden fixtures, both conventions).
- Paint survives regeneration with any settings, copies and renames of the PNG, and editor undo/redo of the write.
- Every MCP/CLI paint behaviour above is covered by a passing e2e test; window behaviours by window tests.
- Tool-definition growth for `generate_sprite_normals` <= 250 characters (`tools/mcp/tool_budget.py` before/after in
  the report); agent guide unchanged in size.
- No new warnings in new code with `-Wall -Wextra`; all existing test suites still pass.

## 9. Task breakdown (one implementer, in order, one or more commits each, prefix "Sprite normals paint:")

1. Core `SpriteNormalsPaint.{h,cpp}`: overlay, ops, selection, direction parsing, compositing, chunk encode/decode and
   head summary (`Request` unchanged: the run composites, 2.3); tests 7.1. Insert the paint chunk first, then the
   recipe (2.2; Phase 2's `insertRecipeChunk` keeps chunks of other keywords).
2. Session: `Spec.paint/paintMode`, check, planned outputs, prepare/run compositing/encode, metrics, recipe `paint`,
   staleness and regenerate; tests 7.2.
3. MCP and CLI: args, errors/warnings, result `paint`, contact-sheet outline, `get_asset_info`/`view_asset`,
   `check --normals` codes; e2e 7.4; regenerate TOOLS.md; tool budget numbers.
4. Window: Paint foldout, hemisphere picker, canvas strokes/wand/mirror/sampling, highlight, shortcuts, stroke undo
   and App routing, unsaved-paint prompts, scripted API; tests 7.3; Help shortcuts text.
5. Docs: guide section, checks.md, README sentence; final full run of core, editor and MCP suites.

## Amendments (2026-10-27, consistency review)

- API names: the Phase 2 names of contract section 8; `prepare` (was `buildRequest`), `measure` (was `metrics`) [1].
- Compositing (2.3, 3, task 1): `Request` is not changed (no `Request::paint`); the run calls `generate()` with
  `settings.directx = false`, then `applyPaintOps`, then `composite(..., directx)` when there is paint; otherwise
  `generate()` unchanged, so unpainted output stays byte-identical in both conventions. `prepare` loads the overlay
  into `Prepared`; `Result` (or the run) carries the overlay and `PaintStats` [12].
- Chunk order (2.2, task 1): the paint chunk is inserted first, then the recipe, so the recipe sits right after IHDR
  and the paint chunk after it [38].
- Types (2.4): `Cell` cells, `std::array<float,3> direction`, and defined `PaintStats`, `PaintMode`, `PaintTool`,
  `PaintBrush` [37].
- MCP (4.1): `paint` refused (`invalid_paint`) with more than one normal output [39]; texts as P3 §3.7/§3.8;
  warning `paint_invalid` renamed `paint_unreadable` and check code `normals-paint-invalid` renamed
  `normals-paint-unreadable` [40]. Test 7.4.4 gains the multi-output case.
- Window: paint tools other than Navigate own left-drag, Alt+click and Shift+wheel; Navigate keeps P4's bindings
  (5.2) [34]; P4 key list, undo routing and the unsaved-paint prompt corrected (5.3, 5.4) [35]; scripted API is for
  tests only (no `show_in_editor`) [36].
- Docs (6): keep `sprite-normals.md` <= 18 KB [32].

## Amendments (2026-10-28, implementation)

- Compositing with `directx` (2.3, 3): the run calls `generate()` with the requested settings; a DirectX generation is
  flipped to OpenGL green (an exact byte inversion of the opaque pixels) for the ops and `composite(..., directx)`.
  The composite rules are unchanged, and unpainted pixels of a painted DirectX map stay bit-identical to Phase 2 (a
  separate OpenGL generation could differ by one in rare rounding cases). Without paint: `generate()` unchanged.
- Window (3, 5): in single mode the window's runs use `paint_mode: replace` with no ops (the plain generation); the
  window loads the target file's paint itself, keeps it as its own overlay, composites it over every result (only the
  pixels a stroke changed) and writes it on Save. This lets a size mismatch be a fix row instead of a failed run. A
  batch uses `keep` (each map's paint is kept by Save All) and has no brushes. Retargeting (another source or map, an
  output with its own paint) asks "Discard Paint Changes?" while strokes are unsaved; an output without a paint layer
  keeps the strokes (Save As). A change of the target file on disk (a Save undone, a regeneration) re-reads its paint
  as the saved state and says so while strokes are unsaved. Undoing a project or scene step while the window is
  focused drops the stroke redos.
- `Spec::fingerprint()` (3) includes the paint only when it is not the default (ops, or a mode other than `keep`), so
  unpainted specs keep their Phase 2 fingerprints.
- Schema (4.1): `paint_mode` is `{"type": "string"}` (its values in the description's "(paint_mode)", the guide and
  the handler's error) to keep the tool budget.
- `check --normals` (4.3): its cache is version 2 (PNG entries carry the paint state).
