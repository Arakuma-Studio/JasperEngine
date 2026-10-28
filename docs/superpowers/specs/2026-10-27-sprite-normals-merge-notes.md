# Sprite Normals: merge and release notes

Branch `sprite-normals` (phases 1-5, specs `2026-10-27-sprite-normals-{contract,phase1..phase5}-*.md`) plus the
integration pass `sn/final`. Editor-only tooling (nothing of the generator ships in a game); the renderer fixes of
Phase 1 are engine changes and ship in every game.

## What shipped, per surface

| Surface | What |
|---|---|
| Engine (Phase 1) | 2D normal maps read in the OpenGL convention (green up) on sprites, tilemaps and polygons; mirrored objects (negative scale, flipped frames, mirrored glTF nodes) shade correctly and keep their front faces (a per-draw reversed-winding pipeline variant, shared and reference-counted through `SokolPipelineCache`); a Light 2D's Height shapes only normal-mapped objects and Lit materials; `@require normalmap` (manifest input) and `library:sprite` sampling the material's Normal Texture; flat `(128, 128, 255)` fallback for a normal map that fails to load |
| Lua / C++ | `Mesh:setNormalTexture([submesh,] path)`, `setNormalTexture(id, TextureData)`, `getNormalTexture([submesh])`, rw `normalTexture` (submesh 0); Light2D `height` documented |
| Generator (Phase 2) | C++ port of Sprite Fusion's sprite-normals 0.1.0 (bit-exact on every upstream fixture with glibc; within 1 per channel across platforms, whose maths libraries differ; no FMA contraction on any compiler) plus presets (`soft`, `default`, `crisp`, `detailed`, `chunky`), profiles, edges, outline detection, colour regions; PNG/APNG/GIF/JPEG/TGA/BMP/PSD input with hostile-input bounds; sheets, Sprite frames, GIF/APNG to atlas; recipe chunk `jasper.spritenormals` (staleness by source SHA-1); CPU lit preview and contact sheet; metrics and assessment; worker with cancel; apply command (one Scene undo step) |
| MCP (Phase 3) | `generate_sprite_normals` (preview, write as one project undo step, `apply`, `save_path`, jobs), `get_asset_info` / `view_asset` recipe summary and `regenerate` arguments, file-sync `external_changes.problems` for maps whose source changed (asks the editor's watcher first, then the default names and the check cache), guide `jasper://docs/sprite-normals`, `focus_window sprite_normals` |
| CLI | `jasperengine-editor generate-sprite-normals` (`--frame-size`, `--cells-json`, `--project --sprite`, `--dry-run`, `--format json`, `--preview-out`, `--paint-json`, `--paint-mode`); `jasperengine-editor check --normals` (12 `normals-*` codes on generated PNGs and on the normal maps of Sprites, Tilemaps and Polygons; 3D meshes are not checked; cached in `.jasperengine/normals-check-cache.json`, version 2) |
| Export | PNGs ship without the editor's `jasper.*` text chunks (sprite normals recipe and paint layer, texture synthesis recipe), dropped losslessly (every other chunk kept byte for byte) |
| Editor UI (Phase 4) | Window > Sprite Normal Map (single and batch mode, live preview with a movable light, frames, presets, advanced settings, Save / Save & Assign / Save All); Project window Generate / Edit / Regenerate Normal Map and a stale badge; Inspector: a wand on both Normal Texture rows, Normal Map... under Sprite Frames, an asset view for generated PNGs (Regenerate, Open in Generator, Assign to Selection, opt-in auto-regenerate); `SpriteNormalsWatcher` staleness index |
| Paint (Phase 5) | A correction layer stored in the normal map (`jasper.spritenormals.paint`) and composited again on every regeneration: Angle, Flatten, Smooth, Erase brushes and a wand in the window (stroke undo, unsaved-paint prompts); `paint` / `paint_mode` in MCP and the CLI; `normals-paint-unreadable`, `normals-paint-size` checks |
| Docs | README (features, migration notes, CLI block), NOTICE (upstream MIT row), `docs/mcp/guide/sprite-normals.md`, guide index, MCP README, `checks.md`, `lua-api.md`, `shaders.md`, `docs/shaders/library.md`, regenerated `TOOLS.md` |

## Behaviour changes and migration notes

- **2D reads OpenGL green.** 2D normal maps authored for the old behaviour light upside down: flip the green channel
  (the generator's DirectX toggle, an image editor), or generate again. `check --normals` flags such maps
  (`normals-green-convention`, `normals-directx`).
- **Light 2D Height shapes only normal-mapped unlit objects** (and Lit materials, which keep their N.L). Plain sprites
  under a Light 2D with Height > 0 were dimmed by `height / |L|`; they now get the full radial light, as at height 0.
  Lower the light's Intensity if the dimming was wanted.
- **New editor Light 2Ds start at Height 64** (Hierarchy, menus, MCP `create_entity light_2d`, Add Component). Lua,
  C++ and loaded lights keep their value; 0 makes normal maps flat.
- **Mirroring is correct.** Mirrored normal-mapped objects (2D and 3D), including imported glTF models with mirrored
  nodes, now shade correctly (they were wrong; no migration).
- **Mirrored meshes keep their front faces.** Meshes mirrored at runtime: single-sided ones no longer cull their
  fronts; double-sided Lit ones shade the visible side as the front; shaders branching on `gl_FrontFacing` see the
  front. Remove `windingOrder = CW` workarounds on such meshes (they are now reversed twice). Not covered (shade and
  cull as before): a mirror made only by an instance matrix, by skinning joints, by a lattice turning the mesh inside
  out, or baked into a Vertex Animation.
- **`library:sprite` reads the Normal Texture** under 2D lights (it was ignored).
- **A normal map that fails to load shades flat** (it was tilted about 55 degrees).
- **The editor's shader cache moves to `shaders/v40`** (the shader sources of this branch changed): the first run
  rebuilds the cached built-in and library programs once; exported projects' shader path follows.
- **Exported PNGs lose their `jasper.*` chunks** (a few hundred bytes to a few KB each); the project files keep them.
- **Custom shaders** that include `lighting2d.glsl` and pass their own normals to `getLight2DIntensity` must
  `#define LIGHT2D_SURFACE_NORMALS` before the include to keep the Light 2D Height response (`docs/mcp/guide/shaders.md`).
- **Engine internals, no behaviour change at exit:** `System::hasInstance()` is new (true once `Engine::systemInit`
  set an instance), and `Log` writes to stderr instead of aborting when no instance is set (editor CLI tools that run
  without the engine). `Engine::systemShutdown` keeps the instance set, as before: Phase 3 had made it clear the
  instance, which made exit paths that touch the System after shutdown abort (the HTML UI test binary did); the
  integration pass reverted that. The HTML UI test binary also shuts the engine down after `RUN_ALL_TESTS` now
  (after every global environment, before the statics, as a game quits).

## Known limitations

- `check --normals` and the Inspector wand look at Sprites, Tilemaps and Polygons only; a prefab-instance override
  that changes only the mesh of such an instance (no 2D component in the override) is not checked.
- The staleness watcher rests on a project with no generated map: it walks it again on this tool's writes, an MCP
  file sync, a generated PNG shown in the Project window, or every 10 minutes (files copied in by hand show up then).

- Brightness is read as height: dark albedo becomes dents, bright paint bumps (use `regions` with `detail: 0` or
  paint). 1 px lines get no sideways slope; overlapping parts merge. Frames are processed independently, so painted
  detail that moves can flicker.
- 2D lights do not reach UI `Image`/`Button` (by design of `ui.frag`).
- The undo step names ("Generate Normal Map", "Assign Normal Map") exist only in the results: `CommandHistory` has no
  step names.
- The Normal Texture wand sits in the Material section, collapsed by default; Sprite Frames' Normal Map... is the
  discoverable entry for sprites.
- An opt-in auto-regenerate of a map the window is painting rewrites the file under the window; the window keeps its
  strokes as unsaved and says so, but does not merge.
- A 4096x4096 single opaque island takes about 3 s to generate (one work item cannot be split).
- The asset view re-checks staleness every 3 s itself; the Project badge follows the watcher (rescans at most every
  5 s, only while the editor draws frames).
- A window status line ("Saved ...") is not cleared when the save is undone elsewhere.

## Test evidence (integration head)

All on `sn/final` (final head after the final-review fixes, 69109afc6 code + 7c320e40c TOOLS.md), Release, this host
(Linux, llvmpipe/lavapipe under Xvfb):

| Suite | Result |
|---|---|
| Build (editor, all targets incl. editor/spritenormals/texsynth tests; install), htmlui glcore and Vulkan, engine-only `jasperengine-project` | 0 compiler warnings (the engine-only build's earlier `Matrix3.cpp` `-Wstringop-overflow` note is pre-existing, in an untouched file) |
| `jasperengine-spritenormals-tests` | 95/95 passed |
| `jasperengine-texsynth-tests` | 24/24 passed |
| `jasperengine-editor-tests` (full) | 845 ran: 839 passed, 6 skipped (environment: SideFX exports, convex decomposition cache), 0 failed |
| MCP e2e (`run_tests.py -j 3`) | 767 passed, 14 skipped (tools not installed / slow builds), 5 failed: all `test_animation_assets.py`, the missing external fixture `build/fixture-unity` (pre-existing) |
| htmlui GPU suite, OpenGL (glcore) | 3077/3077 passed, exit 0, no golden changed |
| htmlui GPU suite, Vulkan (lavapipe) | 3077/3077 passed, exit 0 |
| `shaders --library sprite --backend all` | 111 variants x 6 backends (normal-mapped sprite cases included), 666 compiles, 0 failed |
| `shaders --library all --backend all` | 42 library shaders, 27,558 compiles, 0 failed |
| CLI smoke | PNG, GIF (+frames JSON, preview), sheet with `--frame-size` and `--cells-json`, `--dry-run --format json`, paint, exit codes 1/2, `check --normals` (clean, source changed, source missing, JSON), `export` (the exported map has no `jasper.*` chunk, same pixels) |
