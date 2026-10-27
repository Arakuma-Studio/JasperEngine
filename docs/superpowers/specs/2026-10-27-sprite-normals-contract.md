# Sprite Normals: shared contract (all phases)

Status: binding for every Sprite Normals phase spec and implementer. A phase spec may add detail; it may not
contradict this file. When something here is wrong, the controller amends this file and notes it in the ledger.

Owner direction (2026-10-27): port Hugo-Dz/spritefusion-sprite-normals (MIT) into JasperEngine; fix the 2D renderer so
generated maps are correct; ship every phase together; best UX for humans and AI agents; implementation by parallel
subagents in worktrees, merged to `main` when complete.

Research behind this contract: the analysis thread of 2026-10-27 (8 analysts: upstream algorithm, upstream I/O and
API, empirical evaluation of the real upstream binary, engine 2D normal-map path, generator-tool patterns, MCP/CLI
surface, editor UX, market survey of Laigter, SpriteIlluminator, Sprite Lamp, Sprite-AI, Totally Normal Mapper, Unity,
Godot, Aseprite plugins).

## 1. Phases, specs and owners

| Phase | Spec | Scope |
|---|---|---|
| 1 Engine | `2026-10-27-sprite-normals-phase1-engine-design.md` | 2D normal maps correct in the renderer: OpenGL green, mirroring, Light 2D response and default height, library Sprite shader support, Lua setter, fallback texel |
| 2 Generator | `2026-10-27-sprite-normals-phase2-generator-design.md` | Core algorithm (upstream port + advanced options), media I/O (PNG, GIF, APNG, sheets), recipe, CPU lit preview and contact sheet, metrics and assessment, session layer, worker, apply-to-sprite command |
| 3 Agents | `2026-10-27-sprite-normals-phase3-agents-design.md` | MCP tool `generate_sprite_normals`, CLI `generate-sprite-normals`, `check --normals`, `get_asset_info`/`view_asset`, docs (guide, README, TOOLS.md, instructions) |
| 4 Editor UI | `2026-10-27-sprite-normals-phase4-editor-ui-design.md` | Sprite Normal Map window, Project window entries, Inspector entries, image asset view, generated badge, regenerate on source change |
| 5 Paint | `2026-10-27-sprite-normals-phase5-paint-design.md` | Correction layer: paint/flatten/smooth/erase normals on top of generation, kept across regeneration; window brushes; MCP/CLI `paint` operations |

Implementation waves: wave 1 = Phase 1, Phase 2A (core + media), Phase 2B (recipe, preview, metrics, session, apply);
wave 2 = Phase 3 and Phase 4; wave 3 = Phase 5; then integration, full test run and final review.

## 2. Names (identical on every surface)

| Thing | Name |
|---|---|
| Feature (docs, menus) | Sprite Normal Map / sprite normals |
| C++ namespace | `jasperengine::editor::spritenormals` |
| Source folder | `editor/spritenormals/` (core files have no engine, editor-window or GPU dependency) |
| Core test target / option / build.sh target | `jasperengine-spritenormals-tests` / `JASPERENGINE_SPRITENORMALS_TESTS` / `tools/build.sh spritenormals-tests` |
| Window class / title / `focus_window` value / panel key | `SpriteNormalsWindow` / `Sprite Normal Map` / `sprite_normals` / `spriteNormals` |
| MCP tool / module file / registration | `generate_sprite_normals` / `editor/mcp/tools/SpriteNormalsTools.cpp` / `registerSpriteNormalsTools` |
| CLI subcommand | `generate-sprite-normals` |
| Check flag / tool / code prefix | `check --normals` / `normals` / `normals-` |
| Recipe iTXt keyword | `jasper.spritenormals` (paint layer: `jasper.spritenormals.paint`, Phase 5) |
| Guide | `docs/mcp/guide/sprite-normals.md` served as `jasper://docs/sprite-normals` |
| Upstream licence file | `editor/spritenormals/LICENSE-spritefusion-sprite-normals.txt` + a `NOTICE.md` row |

## 3. Settings (canonical names; JSON/MCP snake_case, CLI kebab-case, UI label in quotes)

Values outside the range are errors that name the setting, the range and the value (never silently clamped).

| Setting | Type, range | Default | Meaning |
|---|---|---|---|
| `preset` | `soft` `default` `crisp` `detailed` `chunky` | `default` | Starting values for the six "main" settings below; explicit values override the preset |
| `strength` "Strength" | 0..8 | preset | Multiplies every slope (upstream `normal_strength`) |
| `detail` "Detail" | 0..8 | preset | Relief from painted shading (upstream `detail_strength`) |
| `smoothing` "Smoothing" | 0..8 px | preset | Sigma of the fine shading blur; the broad blur is 2x (upstream `detail_scale`) |
| `volume` "Volume" | 0..8 | preset | Silhouette rounding (upstream `shape_strength`) |
| `radius` "Radius" | `"auto"` or 0.5..512 px | `auto` | Reach of the silhouette rounding (upstream `shape_radius`). Auto: section 4 |
| `softness` "Softness" | 0..8 px | 1.15 | Blur of the silhouette height (upstream `shape_softness`) |
| `alpha_threshold` "Alpha cutoff" | 0..0.99 | 0.03 | Pixels with alpha/255 <= this are empty |
| `profile` "Profile" | `rounded` `dome` `bevel` | `rounded` | Height across the silhouette: `rounded` = upstream `S*R*log1p(inset/R)`; `dome` = circular cap reaching height `S*R` at inset `R`, flat beyond; `bevel` = linear ramp `S*inset` up to `R`, flat beyond |
| `edges` "Edges" | `transparent` `wrap` `extend` | `transparent` | What lies beyond the image (or each frame cell): `transparent` = upstream (border bevels); `wrap` = toroidal, for seamless tiles; `extend` = border pixels continue outward (tileset tiles, no border bevel) |
| `ignore_outline` "Ignore outline" | bool | false | Auto-detect the outline colour (the darkest colour covering >= 50% of the silhouette rim, Oklab L at most 0.35) and give it zero weight in the detail term |
| `regions` "Regions" | array, at most 16 of `{colors: ["#rrggbb", ...] (1..16), tolerance: 0..0.5 (Oklab distance, default 0.03), detail: 0..8 multiplier (default 1), lift: -16..16 px of height (default 0), flat: bool (default false)}` | `[]` | Per-colour overrides; a pixel takes the first matching region. `detail: 0` excludes painted highlights or outlines from relief; `lift` raises/sinks a material; `flat` forces the pixels' normals to +Z |
| `directx` "Green: DirectX" | bool | false | Green points down the image (Unreal). False = OpenGL, which the engine reads after Phase 1 |
| `height` "Height map" | bool | false | Also write a 16-bit grey height map |
| `frames` "Frames" | `"auto"`, `"none"`, or `{tile_size: N or [w,h], columns, rows, count, offset: [x,y], padding: [x,y]}` | `"auto"` | Cells processed independently. Auto = the Sprite's frames (when the source is a Sprite GameObject), the animation's frames (GIF/APNG), else the whole image. Object keys mirror `set_sprite_frames.slice` |

Presets (the six main settings; `softness` 1.15 and `radius` auto in all):

| preset | strength | detail | smoothing | volume | intent |
|---|---|---|---|---|---|
| `soft` | 1.0 | 2 | 1.0 | 1.15 | gentle, painterly art |
| `default` | 1.5 | 3 | 1.0 | 1.15 | best measured accuracy on 16-64 px sprites, half the upstream flicker |
| `crisp` | 1.5 | 3 | 0.6 | 1.15 | small characters, keeps 1 px features |
| `detailed` | 1.5 | 6 | 0.6 | 1.15 | the upstream defaults (most painted detail, highlights become bumps) |
| `chunky` | 2.5 | 4 | 0.6 | 2.0 | stylised, strong volume |

## 4. Algorithm contract

- Port of upstream `src/{lib,relief,field,distance,components,color}.rs` per the algorithm spec in Phase 2. With
  `profile: rounded`, `edges: transparent`, explicit `radius`, `ignore_outline: false`, no `regions`, and the same five
  numeric settings, output matches the upstream binary within 1 per channel on every golden fixture (fixtures
  generated with the real upstream binary and committed under `editor/spritenormals/tests/fixtures/`).
- 8-connected islands, exact Euclidean distance transform, Oklab lightness, alpha-weighted normalized convolution,
  normalized Sobel, slope -> normal exactly as upstream. Islands and cells are independent; output is deterministic
  regardless of thread count.
- Auto radius: one value per source. For a sequence of frames (GIF/APNG or a frame grid), R = clamp(0.15 x m, 4, 512)
  where m is the median over frames of the shorter bounding-box side of each frame's largest island; for a single
  image, each island gets R = clamp(0.15 x its shorter bbox side, 4, 512). The resolved value(s) are reported.
- Output: RGBA8; alpha = source alpha byte; RGB = (128,128,255) where alpha/255 <= `alpha_threshold`; PNG without
  gAMA/sRGB/iCCP chunks; recipe chunk after IHDR.
- Limits: input file <= 64 MiB; image <= 16,777,216 pixels (4096 x 4096); <= 256 animation frames and <= 16,777,216
  decoded pixels across frames; checked before allocation, errors name the limit and the actual numbers.

## 5. Files

| Output | Default path | When |
|---|---|---|
| Normal map | `<stem>_normal.png` beside the source (`_normal` is what Texture Synthesis' companion finder already recognises) | always |
| Colour atlas | `<stem>_sheet.png` | GIF/APNG source |
| Normal atlas | `<stem>_sheet_normal.png` | GIF/APNG source (replaces `<stem>_normal.png`) |
| Height map | `<stem>_height.png` (or `<stem>_sheet_height.png`) | `height: true` |
| Frames JSON | `<stem>_sheet.frames.json` (upstream schema + `version`, `rows`, `loop`, `frame_ms` floats, `source`, `settings`) | CLI `--frames-json` / MCP `output.frames: true` only |

Animated sources are packed row-major with `columns = ceil(sqrt(n))`, like upstream. Outputs are written all-or-nothing;
inputs are never overwritten; an existing output needs `overwrite`; in the editor a write is one Project undo step
(`WriteFilesCmd`), and assigning to a Sprite is one Scene undo step.

## 6. Recipe

Every PNG the generator writes carries iTXt `jasper.spritenormals` with canonical JSON:
`{"format":"jasperengine.spritenormals","version":1,"generator":"JasperEngine <version>","role":"normal|atlas|height",
"source":{"path":<project-relative or as given>,"sha1":...,"kind":"image|gif|apng|sprite","entity":<path, optional>},
"frames":{"from":"sprite|gif|apng|given|single","cells":[[x,y,w,h],...],"columns":..,"rows":..,"delays_ms":[...]},
"settings":{<every setting of section 3, resolved, preset included>},"radius_resolved":<number or "per-island">,
"outputs":{"normal":..,"atlas":..,"height":..}}`. Texture Synthesis' recipe chunk code is generalized to take the
keyword (no behaviour change for `jasper.synthesis`). Staleness = the source's SHA-1 differs or the source is gone.

## 7. Engine conventions after Phase 1 (what the generator and tools may assume)

- 2D (Sprite, sprite grid, Polygon, Tilemap, the built-in Mesh path and the library Sprite shader) reads normal maps
  OpenGL style: green = up the image, red = right, same as 3D Lit, Unity and Godot. Mirroring (negative scale,
  mirrored frame rects, flipY) keeps lighting correct.
- A Light 2D's `height` drives N.L only on fragments that sample a normal map; sprites without a normal map are lit
  exactly as before regardless of height. New Light 2Ds made by the editor (menu, Hierarchy, MCP `create_entity`) get
  `height = 64`. The struct default stays 0 (scenes and scripts unchanged).
- The normal map is assigned at `submeshes[0].material.normalTexture` (every submesh for tilemaps); it must have the
  colour texture's pixel size and frame layout; the tools copy the colour texture's min/mag filter and wrap onto it.
- Lua can assign a sprite's normal map (Phase 1 defines the exact API).

## 8. Shared engine-free APIs (Phase 2 defines exact headers; Phases 3-5 call only these)

- Core: `generate(const Request&, const Control&) -> Result` (blocking, never throws, `Problem{field,message}` list,
  cancel flag, progress callback), `presetSettings(Preset)`, `resolveSettings`, `validateSettings`.
- Media: `loadSource(path|bytes) -> Source` (frames, delays, loop, cells), `packAtlas`, `encodePng(image, recipe)`.
- Preview: `renderLit(normal, color, LightSetup) -> Image` using exactly the engine's `getLight2DIntensity` formula;
  `contactSheet(...)` (per frame: colour | normal | lit from 4 directions; nearest integer upscale; <= 1024 px wide;
  <= 8 frames); `metrics(...)` and `assess(...)` (plain-language lines + one hint).
- Session (editor side, project-aware): `Spec` (+ `fingerprint()`), `checkSpec`, `plannedOutputs`, `buildRequest`,
  `encodeOutputs`, `writeOutputsUndoable`, `specFromRecipe`, `checkStaleness`, `framesFromSprite(entity)`,
  `lightsRespondToNormals(scene)`, `SpriteNormalsRun` (worker: start/cancel/finished/latest result).
- Apply: one Scene-undoable command that assigns the normal map (and, for animated sources, the atlas, frames and
  Sprite Animation) to a Sprite GameObject and copies the colour texture's filter and wrap.

## 9. Quality bar (every phase)

- No behaviour change for existing scenes, prefabs, MCP calls or tests except the deliberate Phase 1 fixes, each
  listed in its spec with its migration note.
- Every new public function has tests; every MCP/CLI behaviour has an e2e test under `tools/mcp/tests/`; GPU-visible
  changes have render tests (software GL under Xvfb, plus Vulkan via lavapipe where the harness supports it).
- Docs follow the house rules (`docs/mcp/guide/README.md`), the agent guide stays within its 16 KB budget, tool
  definitions stay lean (`tools/mcp/tool_budget.py`).
- Build: `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release` with ccache (`source /root/.ccache-env`); `-Wall -Wextra` clean
  in new code.
