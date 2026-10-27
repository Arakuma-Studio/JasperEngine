# Sprite Normals, Phase 3: the agent surface (MCP tool, CLI, check, docs)

Status: binding for the Phase 3 implementer. Conforms to `2026-10-27-sprite-normals-contract.md` (the contract wins on
any conflict; tell the controller). Wave 2: one implementer, one worktree, in parallel with Phase 4 (editor UI). Built on
Phase 1 (engine) and Phase 2 (generator + session + apply), both merged into `sprite-normals` before this phase starts.

Phase 2 owns every engine-free and session API this phase calls (contract section 8: `generate`, `presetSettings`,
`resolveSettings`, `validateSettings`, `loadSource`, `packAtlas`, `encodePng`, `renderLit`, `contactSheet`, `metrics`,
`assess`, `Spec`, `checkSpec`, `plannedOutputs`, `buildRequest`, `encodeOutputs`, `writeOutputsUndoable`,
`specFromRecipe`, `checkStaleness`, `framesFromSprite`, `lightsRespondToNormals`, `SpriteNormalsRun`, and the apply
command). Use their exact names and signatures from the Phase 2 headers as merged; this spec never redefines them. Where
this spec needs something Phase 2 does not provide (scene-aware warnings, tilemap frames, the reverse source index),
it is written here, inside this phase's own files.

## 1. Goals

1. An AI agent with only MCP can turn any sprite, sprite sheet, GIF/APNG animation or tileset in the project into a
   correct normal map, **see** the result (a lit contact sheet), understand it (metrics, plain-language assessment, a
   hint, warnings with fixes), write exactly what it saw, wire it onto the GameObject in one call, and know when it is
   out of date, all in the house pattern of `synthesize_texture` and `bake_vertex_animation`.
2. A shell, a CI job or an agent without MCP gets the same through `jasperengine-editor generate-sprite-normals` (batch,
   JSON, dry run, preview image) and `jasperengine-editor check --normals`.
3. Agents find it: one tool description, one line in the agent guide (within its 16 KB budget), a guide document
   `jasper://docs/sprite-normals` with worked examples, `TOOLS.md`, `checks.md`, README entries.
4. Lean: the new tool definition stays small (section 9); no existing tool, argument or result changes shape.

Non-goals: the window and every human UI (Phase 4); the paint/correction layer and the `paint` argument (Phase 5,
which adds `paint` to this tool, the CLI and the guide); an AI/ML backend.

## 2. Files this phase owns

New:
- `editor/mcp/tools/SpriteNormalsTools.cpp`: the tool, its argument parsing, scene-aware warnings, the asset-info and
  view-asset helpers, the auto-sync hook.
- `editor/spritenormals/SpriteNormalsCli.h/.cpp`: the CLI (namespace `jasperengine::editor::spritenormals`).
- `editor/spritenormals/SpriteNormalsCheck.h/.cpp`: the `check --normals` engine (pure parsing, no engine start), used by
  the CLI `check` command.
- `docs/mcp/guide/sprite-normals.md`.
- `tools/mcp/tests/test_generate_sprite_normals.py`, `tools/mcp/tests/test_sprite_normals_cli.py`,
  `tools/mcp/tests/test_sprite_normals_check.py`.
- `tools/fixtures/spritenormals/make_fixtures.py` (generates the test PNGs/GIF/APNG deterministically; the tests may
  also generate inline) only if the tests need more than Phase 2's committed fixtures.

Edited (each edit small; listed so the merge with Phase 4 is predictable):
- `editor/mcp/tools/McpTools.h` (declarations), `editor/mcp/McpServer.cpp` (registration, after `registerVatTools`),
  `editor/mcp/tools/AssetTools.cpp` (get_asset_info / view_asset), `editor/mcp/tools/ScriptTools.cpp` (file-sync hook,
  next to `vatFileChanged`), `editor/mcp/McpInstructions.cpp` (one sentence, section 8.1).
- `editor/main.cpp`, `editor/cli/CommandLine.h/.cpp` (dispatch, usage line, `check --normals`).
- `CMakeLists.txt` (the new sources in the editor's source lists, next to the texsynth/vat entries).
- `tools/mcp/gen_tool_docs.py` (MODULES), `docs/mcp/TOOLS.md` (regenerated), `docs/mcp/guide/README.md`,
  `docs/mcp/README.md`, `docs/mcp/DESIGN.md`, `docs/mcp/checks.md`, `README.md`, `tools/mcp/tests/test_docs.py` (DOCS),
  `tools/mcp/tests/test_checks.py` only if a shared helper must change (prefer the new test file).

Not this phase (Phase 4 owns them, to keep the two worktrees apart): `ViewTools.cpp` `focus_window` value
`sprite_normals`, `EditorHost`, `App`, windows, ProjectWindow, Inspector. The integration step after wave 2 regenerates
`TOOLS.md` once more so the `focus_window` enum Phase 4 adds is in it.

## 3. MCP tool `generate_sprite_normals`

### 3.1 Definition

- `name` `generate_sprite_normals`, `title` "Generate Sprite Normals", module `SpriteNormalsTools`.
- `description` (exactly; about 380 characters):
  "Normal maps for pixel-art sprites, sheets, GIF/APNG animations and tiles (2D lights). Without output nothing is
  written: a lit contact sheet, metrics and a hint. The same arguments + output write exactly that (PNG with a recipe,
  one undo step); apply wires it onto the GameObject (GIFs: atlas, frames, Sprite Animation). Lights need height > 0.
  Guide: jasper://docs/sprite-normals."
- `activity` `Edit`; `allowedWhilePlaying` true (files can be written while playing; `apply` is refused while playing,
  3.6). `writesFiles(args)`: true when `output` or `save_path` is present.
- Nested objects are left loose in the schema (no `additionalProperties`, descriptions empty) and validated by the
  handler, as `synthesize_texture` does; unknown nested keys are refused with the list of valid ones.

### 3.2 Input schema

| Argument | Schema | Default | Meaning |
|---|---|---|---|
| `source` | `{"type": ["string","integer","object"]}`, required | | An image file (project-relative path: `.png` (APNG included), `.gif`, `.jpg/.jpeg`, `.tga`, `.bmp`, `.psd`), or a GameObject (id, path, unique name, or `{id\|name\|path}`) that is a Sprite, a Polygon or a Tilemap |
| `scene` | `sceneRef()` | selected scene | For GameObject sources and `apply` |
| `preset` | enum `soft` `default` `crisp` `detailed` `chunky` | `default` | Contract section 3 |
| `params` | `anyObject("")` | `{}` | Overrides of the preset and the advanced settings: `strength`, `detail`, `smoothing`, `volume`, `radius` (`"auto"` or number), `softness`, `alpha_threshold`, `profile`, `edges`, `ignore_outline`, `regions`. Ranges: contract section 3 |
| `frames` | `{"anyOf": [stringEnum("", ["auto","none"]), loose object {tile_size, columns, rows, count, offset, padding}]}` | `"auto"` | Contract section 3; object keys and meaning exactly `set_sprite_frames.slice` (`tile_size` a number or `[w, h]`) |
| `directx` | boolean | false | Green down (Unreal). The engine reads OpenGL; with `apply` it is a warning |
| `height` | boolean | false | Also the 16-bit height map |
| `output` | `{"type": ["string","boolean","object"]}` | (omitted: preview) | `true`: default names (contract section 5); a string: the normal map's path (the animation's normal atlas); an object `{normal?, atlas?, height?, frames?}` overriding single paths (`frames`: `true` for `<atlas stem>.frames.json`, or a path). `false` = preview |
| `overwrite` | boolean | false | Replace existing outputs |
| `apply` | `{"type": ["boolean","string","integer","object"]}` | false | On write: `true` = the source GameObject; an entity reference = that GameObject (a file source) |
| `save_path` | string | | Also write the preview contact sheet PNG there (project-relative; inside the project, any folder; never needs `overwrite`, like `screenshot`) |
| `wait` | boolean | true | |
| `timeout_seconds` | number 1..3600 | 120 | Then a `job_id` (`get_job`) |

Reserved for Phase 5: `paint` (do not declare it in this phase).

### 3.3 Resolving the source

1. An integer or object: a GameObject in `scene`.
2. A string: when it has an image extension (`Util::isImageFile`, `.svg` excluded) and `McpRefs::resolveProjectPath`
   finds an existing file: that file. Otherwise it is resolved as a GameObject reference in `scene`; if that fails too,
   `invalid_source` names both attempts ("no image file 'x' in the project and no GameObject 'x' in scene 'Main'").
3. A GameObject must have a MeshComponent with a SpriteComponent, a PolygonComponent or a TilemapComponent; its colour
   texture is `submeshes[0].material.baseColorTexture` (Sprite, Polygon) and must be a file path (an embedded/data or
   framebuffer texture is `invalid_source`: "its texture is not a file"). The image file is the source; the recipe
   records `kind: "sprite"` and `entity` (the GameObject path).
4. A Tilemap: every submesh with a distinct base-texture file is one job; outputs are per texture (only `output: true`
   or omitted are allowed: a string or object with several textures is `invalid_output`, "a Tilemap with N tileset
   textures needs output: true (one normal map per texture)"); `frames: "auto"` is the whole texture (no tile cells are
   inferred; the guide tells agents to pass `frames: {tile_size}` and `params: {edges: "extend"}` for tilesets);
   `apply` assigns each submesh its texture's map. Results are a list (3.7).
5. Frames `"auto"`: a Sprite source with frames -> `framesFromSprite(entity)` (`from: "sprite"`); GIF/APNG -> the
   animation's frames (`from: "gif"|"apng"`); else the whole image (`from: "single"`). `"none"`: whole image (an
   animation still packs its frames into an atlas; `"none"` only drops cell isolation and is refused for GIF/APNG with
   `invalid_frames`, "an animation's frames come from the file"). An object: `from: "given"`, sliced exactly like
   `set_sprite_frames.slice` over the source image; the cells must lie inside the image (`invalid_frames` otherwise,
   with the image size and the grid). For GIF/APNG an object is refused (`invalid_frames`).

### 3.4 Preview and write

- Every call builds a Phase 2 `Spec` from the arguments, runs `checkSpec` (no decode), then a `SpriteNormalsRun`.
- **Preview** (`output` omitted or `false`): nothing is written except `save_path`. The result (3.7) carries the
  contact sheet, `default_output` (what `output: true` would write) and the text "Preview only, nothing written: call
  again with the same arguments plus output (true for <default normal path>) to write exactly this."
- **Write** (`output` present): the same generation (deterministic: the same arguments give the same bytes), then
  `encodeOutputs` + `writeOutputsUndoable`: one Project undo step named "Generate Normal Map <file name>" (several
  files of one call are that one step). Then, with `apply`, the Phase 2 apply command: one Scene undo step.
- **Existing outputs**: refused before the run without `overwrite` (`output_exists`, listing them), and checked again
  when the run finishes (another call or a human may have written them meanwhile), as `synthesize_texture` does.
  Outputs may never equal the source (or any input of the call), even with `overwrite` (`invalid_output`).
- **Output paths** must be inside the assets directory, outside hidden directories, end in `.png` (`.json` for
  `frames`), and be distinct (`invalid_output`, with the reason and the offending path).
- **Recipe**: every PNG carries `jasper.spritenormals` (contract section 6). `regenerate` (3.9) reproduces it.
- **Determinism** is part of the contract: same source bytes + same arguments = identical pixels, whatever the thread
  count. Tests rely on it.

### 3.5 Job, wait, timeout, project switch

- The run is an `McpJob` ("Generate normal map <source> (preview)" / "... -> <output>"), progress forwarded from the
  run's progress callback, cancel through `job->setCancelHandler` -> `run->cancel()`; results posted to the main thread
  with `getEditorHost().enqueueMainThreadTask`, finished with `replyWhenJobDone(call, job, wait, timeout, ...)`, with
  the same exception guard as `SynthesisTools.cpp` (a job never stays running).
- Past `timeout_seconds` the call returns `{job_id, status: "running"}`; `get_job` later returns the same result data
  without images; `cancel_job` writes nothing.
- The call records `texsynth::projectGeneration()` and the project root at start; if another project was opened when
  the run finishes, the job fails with kind `project_changed` and nothing is written (the run is cancelled by the
  session's project-switch hook).

### 3.6 `apply`

- Needs a write: `apply` with no `output` is `apply_needs_output` ("apply assigns the written file: add output: true").
- `apply: true` needs a GameObject source (`invalid_apply`: "apply: true wires the source GameObject; with an image
  source name the GameObject: apply: \"Hero\""). An entity reference must be a Sprite, Polygon or Tilemap GameObject
  in `scene` whose colour texture is the source image (or, for an animated source, any Sprite: its texture becomes the
  atlas); otherwise `invalid_apply` with what it is.
- Refused while playing (`scene_playing`: "stop play first (play {action: \"stop\"}), then call again"), checked when
  the call starts and again before the scene command runs; the files are not written then either.
- What it does (the Phase 2 apply command, one Scene undo step "Assign Normal Map"):
  - Sprite/Polygon: `submeshes[0].material.normalTexture` = the normal map, with the colour texture's min/mag filter and
    wrap copied onto it.
  - Tilemap: each submesh's `normalTexture` = its texture's map, filters copied per submesh.
  - Animated source (GIF/APNG): base texture = the colour atlas, normal = the normal atlas, the Sprite's frames = the
    atlas cells (named `frame_0`..), and a Sprite Animation child named after the source stem (created, or updated when
    a child of that name exists) with the frames in order, `frame_ms` = each delay rounded to whole ms (at least 1; a
    0-delay GIF frame becomes 100 ms, as browsers do, and a warning `zero_delay` says so), `loop` = true unless the
    file's loop count is finite (then false), `autoplay` true when created. The Sprite's size follows the first frame
    (`resize` as `set_sprite_frames` does).
- The result's `undo` lists both steps (3.7) and `entity` is the GameObject (`McpRefs::entityJson`).
- Without `apply`, when the source is a GameObject, the result carries `apply_args`: the exact `set_properties` call
  (and, for animations, `set_sprite_frames` / `set_sprite_animation` calls) that wire the written files, so an agent
  can do it later.

### 3.7 Result

Text first (one paragraph, then `Warning:` lines, then `Image:` line), then one image, then `structuredContent`.

`structuredContent` for one source:

```
{
  "written": false,
  "source": {"path": "assets/sprites/hero.png", "kind": "image|gif|apng|sprite", "entity": {"id": 12, "path": "Hero"}?,
             "width": 128, "height": 32},
  "frames": {"from": "sprite|gif|apng|given|single", "count": 4, "columns": 4, "rows": 1, "frame_width": 32,
             "frame_height": 32, "delays_ms": [100, 100, 100, 100]?, "loop": 0?},
  "settings": {"preset": "default", "strength": 1.5, "detail": 3, "smoothing": 1, "volume": 1.15, "radius": "auto",
               "softness": 1.15, "alpha_threshold": 0.03, "profile": "rounded", "edges": "transparent",
               "ignore_outline": false, "regions": [], "directx": false, "height": false},
  "radius_resolved": 4.8,                       // or "per-island"
  "outline_color": "#1a1c2c"?,                  // when ignore_outline found one
  "metrics": {"coverage_pct": 41.2, "flat_pct": 12.0, "steep_pct": 3.1, "mean_tilt_deg": 21.4, "noise_pct": 2.2,
              "lean_deg": 1.3},
  "assessment": ["Shape: rounded (12% flat)", "Detail: moderate", ...],
  "hint": "Looks fine.",
  "warnings": [{"kind": "no_light_height", "message": "...", "fix": "..."}],
  "default_output": {"normal": "assets/sprites/hero_normal.png", "atlas": null, "height": null, "frames": null},
  "seconds": 0.04,
  "job_id": "...", "status": "succeeded"
}
```

- `metrics` keys are Phase 2's `Metrics` fields in snake_case (the six above are required; Phase 2 may add more, which
  appear too). Percentages rounded to 0.1, degrees to 0.1.
- Write adds: `"written": true`, `"files": [...]` (project-relative, normal first), `"undo": [{"scope": "project",
  "name": "Generate Normal Map hero_normal.png"}, {"scope": "scene", "scene": 3, "name": "Assign Normal Map"}?]`,
  `"entity"` (with apply), `"applied": {"normal_texture": true, "frames": 4?, "animation": {"id", "path"}?}` (with
  apply), `"apply_args"` (GameObject source without apply).
- A Tilemap source: `{"written", "results": [<one object as above per texture, without job fields>], "files", "undo",
  "entity", "job_id", "status"}`; the image is the first texture's sheet; the text summarizes each.
- Text (preview), e.g.: "Normal map of assets/sprites/hero.png (128x32, 4 frames from Sprite 'Hero', preset default,
  radius 4.8 px) in 0.04 s. Preview only, nothing written: call again with the same arguments plus output (true for
  assets/sprites/hero_normal.png) to write exactly this. Shape: rounded (12% flat). Detail: moderate. Hint: Looks
  fine.\nWarning: no_light_height: ... Fix: ...\nImage: the contact sheet (per frame: colour, normal, lit from the
  top-left, top-right, bottom-right, bottom-left)."
- Text (write): "... Wrote assets/sprites/hero_normal.png as one undo step (undo with scope project)" + with apply "
  and assigned it to GameObject Hero (id 12) as one undo step (undo with scope scene, scene 'Main')." + without apply
  for a GameObject source " Use it: set_properties {entity: 12, values: {\"submeshes[0].material.normalTexture\":
  \"sprites/hero_normal.png\"}} (or call again with apply: true)."
- Image: the Phase 2 `contactSheet` (<= 1024 px wide, <= 8 frames; when there are more frames the sheet shows 8 spread
  evenly and `sheet_note` says which). Returned with preview and with write (an agent may skip the preview).
  `save_path` writes the same PNG.

### 3.8 Errors and warnings

Errors are `isError: true` results with a text that says how to fix it and `errors: [{kind, message, fix, field?}]` in
the data (DESIGN.md "Errors"). Kinds and wording (the implementer may improve wording, not drop parts):

| kind | when | message / fix |
|---|---|---|
| `invalid_source` | missing file, not an image, decode failure, not a Sprite/Polygon/Tilemap, texture not a file | "<what> (got <value>)" / "pass a project image path (assets/sprites/hero.png) or a Sprite GameObject" |
| `invalid_settings` | unknown `preset`/`params` key, wrong type, out of range, too many regions, bad colour | "params.detail must be a number in [0, 8] (got 9)" / "valid keys: strength, detail, ..." |
| `invalid_frames` | grid empty/outside the image, object or `"none"` on an animation, too many cells (> 256) | "the grid 40x40 at offset [0,0] does not fit the 128x32 image" / "tile_size [32, 32] or columns 4, rows 1" |
| `too_large` | a contract section 4 limit | "hero.gif decodes to 300 frames; at most 256" / "trim the animation or split it" |
| `invalid_output` | outside assets, hidden dir, wrong extension, equals an input, duplicate, string with several textures | the reason and path / "an assets path ending in .png, or output: true" |
| `output_exists` | an output exists without `overwrite` (also when it appeared during the run) | "would replace assets/sprites/hero_normal.png" / "pass overwrite: true (one undo step puts it back), or another output" |
| `apply_needs_output` | `apply` without `output` | as 3.6 |
| `invalid_apply` | `apply: true` with a file source, a target that is not a Sprite/Polygon/Tilemap, a texture mismatch | as 3.6 |
| `scene_playing` | `apply` while playing | as 3.6 |
| `project_changed` | another project opened during the run | "the project changed while the normal map was generated; nothing was written" / "call again in the open project" |
| `write_failed` | the write or the scene command failed | the OS / command error / "check the path is writable, then call again" |
| `generation_failed` | the core returned an error (its `Problem` list mapped to argument names, as `argumentField` does for texsynth) | the core's message / per problem |

Warnings (in `warnings`, never failing the call). Phase 2's `assess` provides the image-only ones; this phase adds the
scene-aware ones. Each is `{kind, message, fix}`:

| kind | from | when | fix |
|---|---|---|---|
| `no_alpha` | assess | no transparent pixel and `edges: transparent` | "edges: extend for tiles, wrap for seamless textures, or a sprite with transparency" |
| `mostly_flat` | assess | `flat_pct` > 60 | "raise params.strength or params.volume, or preset chunky" |
| `too_steep` | assess | `steep_pct` > 25 | "lower params.strength, or raise params.smoothing" |
| `noisy` | assess | `noise_pct` > 15 (dithering) | "raise params.smoothing to 1.5-2, lower params.detail, or a region with detail 0 for the dither colours" |
| `highlights_as_bumps` | assess | many near-white specular pixels inside islands | "a region {colors: [<the highlight colours>], detail: 0}, or preset soft" |
| `semi_transparent_edges` | assess | many rim pixels with 0.03 < alpha < 0.5 (glow) | "raise params.alpha_threshold (e.g. 0.5) so the glow does not count as shape" |
| `frames_touch_edges` | assess | islands touch a cell border in most cells with `edges: transparent` | "check the frame size; for tiles use params.edges extend" |
| `zero_delay` | this phase | a GIF/APNG frame delay of 0 | "it plays at 100 ms; set frame_ms with set_sprite_animation if another rate is meant" |
| `no_light_height` | this phase | scene known (GameObject source or `apply`) and `lightsRespondToNormals(scene)` is false while the scene has Light 2Ds | "set_properties {entity: <light>, values: {light2d: {height: 64}}}" naming the first light |
| `no_light` | this phase | scene known and it has no Light 2D | "create_entity {type: \"light_2d\"} (new lights get height 64)" |
| `filter_mismatch` | this phase | GameObject source without `apply` whose existing normal texture's filter differs from the colour texture's | "apply: true copies the colour texture's filter" |
| `shader_ignores_normals` | this phase | the target material's shader does not sample normal maps (a manifest shader without Phase 1's normal-map declaration) | "use the built-in sprite material or library:sprite" |
| `directx_in_engine` | this phase | `directx: true` with `apply` or a GameObject source | "the engine reads OpenGL (green up): drop directx" |

### 3.9 `get_asset_info`, `view_asset`, file sync

- `SpriteNormalsTools.cpp` exports (declared in `McpTools.h`):
  `Json spriteNormalsAssetInfo(Project*, const fs::path& file, bool full)` and
  `std::string spriteNormalsAssetText(const Json& info)`, called by `AssetTools.cpp` next to the synthesis ones for
  `.png` files.
- On a PNG with a `jasper.spritenormals` recipe: `data.sprite_normals = {role, source: {path, kind, entity?, state:
  unchanged|changed|missing}, preset, frames: {from, count}, outputs, stale}`; with `full` (get_asset_info) also
  `recipe` and `regenerate` (the tool arguments that write it again: `source`, `scene`?, `preset`, `params` (only the
  values differing from the preset), `frames` (as the recipe's cells: an object, or "auto"/"none"), `directx`,
  `height`, `output` (the recipe's outputs object), `overwrite: true`). A garbled recipe gives `{error}`.
  Text: " Sprite normal map (normal, preset default) of sprites/hero.png; source unchanged." or "; the source changed
  since: regenerate with generate_sprite_normals and the `regenerate` arguments (get_asset_info)."
- On a PNG without a recipe: when `<stem>_normal.png` (or `<stem>_sheet_normal.png` for a GIF/APNG) exists beside it
  and its recipe names this file as the source: `data.normal_map = {path, stale}` and text " Its normal map:
  sprites/hero_normal.png (up to date)." (a cheap probe of the default name only).
- `view_asset` appends the same summary text (no extra image).
- File sync (`ScriptTools.cpp`, beside `vatFileChanged`): `spriteNormalsFileChanged(Project*, path, problems)`: for a
  changed image that is the source of a normal map found by the default-name probe above (and of any recipe found in
  the check cache, section 5.4, without scanning), push `{path: <normal map>, message: "made from <source>, which
  changed: regenerate it (generate_sprite_normals with get_asset_info's regenerate arguments)"}` into
  `external_changes.problems`.

## 4. CLI `generate-sprite-normals`

### 4.1 Usage (printed by `--help`, exit 0; and after a usage error on stderr, exit 2)

```
Usage:
  jasperengine-editor generate-sprite-normals --source <image> [--source <image>...] [options]
  jasperengine-editor generate-sprite-normals --project <dir> --sprite <GameObject> [--scene <scene>] [options]

Generates normal maps for pixel-art sprites, sprite sheets, GIF/APNG animations and tiles (a port of
Sprite Fusion's sprite-normals, MIT). OpenGL convention (green up) unless --directx. Every PNG written
embeds its recipe (settings, source and SHA-1) as an iTXt chunk named jasper.spritenormals.

Input:
  -s, --source <image>        PNG (APNG too), GIF, JPEG, TGA, BMP or PSD. Can repeat (a batch).
      --sprite <GameObject>   A Sprite, Polygon or Tilemap GameObject of the project (path or name): its
                              texture is the source and its frames the cells. Needs --project.
      --scene <scene>         The scene of --sprite (name or .scene path; default: the project's start scene).
  -p, --project <path>        Project directory or project.yaml: recipe paths are relative to it, and
                              outputs must be inside its assets directory. Optional with --source.
Settings (defaults: the preset's):
      --preset <name>         soft, default (default), crisp, detailed, chunky.
      --strength <x>          0..8.          --detail <x>      0..8.
      --smoothing <px>        0..8.          --volume <x>      0..8.
      --radius <auto|px>      auto (default) or 0.5..512.
      --softness <px>         0..8 (1.15).   --alpha-threshold <x>  0..0.99 (0.03).
      --profile <p>           rounded (default), dome, bevel.
      --edges <e>             transparent (default), wrap (seamless), extend (tiles).
      --ignore-outline        Leave the detected outline colour out of the relief.
      --region <spec>         colors=#rrggbb[,#rrggbb...][;tolerance=x][;detail=x][;lift=px][;flat]. Can repeat (16).
      --directx               Green down (Unreal). The engine reads OpenGL.
Frames (default: the animation's, the --sprite's, else the whole image):
      --frame-size <WxH|N>    Cell size.   --columns <n>  --rows <n>  --count <n>
      --offset <x,y>          --padding <x,y>      (as the Texture Slicer)
      --no-frames             Whole image (not for GIF/APNG).
Output:
  -o, --output <png>          The normal map (one source only). Default: <stem>_normal.png beside the source
                              (<stem>_sheet_normal.png for an animation).
      --output-dir <dir>      Write every output there instead of beside its source.
      --atlas <png>           An animation's colour atlas (default <stem>_sheet.png).
      --height-map            Also write <stem>_height.png (16-bit grey).
      --frames-json           Also write <atlas stem>.frames.json (animations and frame grids).
      --preview-out <png>     Also write the lit contact sheet (one source only).
      --overwrite             Replace existing outputs (inputs are never replaced).
      --dry-run               Generate and report, write nothing (not even --preview-out).
      --threads <n>           0 = all cores (default). Never changes the output.
      --format <format>       text (default) or json (one object on stdout).
  -q, --quiet                 No summary on stderr.
  -h, --help                  This help.

Paths are absolute, relative to the current directory, or (with --project) relative to the project.
Exit code: 0 written (or dry run done; warnings allowed), 1 generation or write failed, or an output
exists without --overwrite (nothing is written then), 2 usage error.
```

### 4.2 Behaviour

- Dispatched from `editor/main.cpp` (`generate-sprite-normals`) through `CommandLine::runGenerateSpriteNormalsCommand`
  to `spritenormals::runGenerateSpriteNormals(argc, argv, commandName, out, err)`; listed in `printMainUsage`.
- Without `--sprite`: no engine, no window, no GPU (like `synthesize-texture`). With `--sprite`: the project is loaded
  with the headless engine pattern of `place-physics`/`bake-vat`, the scene opened, `framesFromSprite` read, the engine
  shut down before generating.
- Without `--project`: recipe `source.path` is the source path relative to the output's directory (so a copied folder
  keeps working); outputs may go anywhere. With `--project`: paths project-relative, outputs must be inside the assets
  directory (else exit 2 `invalid_output`).
- **Preflight before any write** (the upstream lesson): parse; resolve every source; read every header; check limits,
  frames and settings; plan every output (`plannedOutputs`); refuse: `-o`/`--preview-out` with several sources (2),
  duplicate output paths across the batch (2, naming both sources), an output equal to any input (2, even with
  `--overwrite`; aliases resolved with `weakly_canonical`), existing outputs without `--overwrite` (1, `output_exists`,
  listing them all). Nothing is written when a preflight check fails.
- **All or nothing across the batch**: every output of every source is encoded first, then written by one
  `writeFilesAllOrNothing` (the texsynth writer); a failure writes nothing and exits 1 naming the file.
- `--dry-run`: runs the generation (so metrics and warnings are real), reports what would be written (`written: false`,
  `planned` paths), writes nothing.
- `--frames-json`: written for animations and frame grids only (else a usage error 2): the contract schema
  `{version: 1, source, width, height, animation: {frame_width, frame_height, columns, rows, frame_count, delays_ms:
  [[num, den]...], frame_ms: [floats], loop}, settings}` (upstream's keys kept for compatibility, extended).
- Text output: per source one line on stderr `generate-sprite-normals: <source> -> <outputs> (<w>x<h>, <n> frames,
  preset <p>) in <s>s` plus `warning: <kind>: <message> (fix: <fix>)` lines; errors on stderr as
  `generate-sprite-normals: <kind>: <message> (fix: <fix>)`; `-q` silences the summaries, not errors.
- JSON output (`--format json`): exactly one object on stdout:

```
{"ok": true, "dry_run": false, "seconds": 0.12,
 "results": [{"source": "art/hero.png", "kind": "image|gif|apng|sprite", "entity": "Hero"?, "width": 128, "height": 32,
              "frames": {...as MCP 3.7...}, "settings": {...}, "radius_resolved": 4.8, "metrics": {...},
              "assessment": [...], "hint": "...", "warnings": [{kind, message, fix}],
              "outputs": {"normal": "art/hero_normal.png", "atlas": null, "height": null, "frames_json": null,
                          "preview": null},
              "written": true}],
 "errors": [{"kind": "...", "message": "...", "fix": "...", "source": "art/x.png"?}]}
```

  Paths in the JSON are as the user would type them back: project-relative with `--project`, else relative to the
  current directory when below it, else absolute. Engine logs never reach stdout.

## 5. `check --normals`

### 5.1 Options and wiring

- `check` gains `--normals` (help line: "Sprite normal maps: generated ones whose source changed or went missing, and
  normal maps on sprites that don't match their colour texture (size, frames, filter), look like DirectX or aren't
  normal maps, or no Light 2D can show (height 0)."). It joins the default set (pure parsing, no engine start) and
  `--all`. `checked` gets `"normals": <PNGs with a recipe + normal maps in use checked>`.
- `--files`: an image is checked as a generated normal map when it has a recipe, and as a source otherwise (the
  generated maps whose recipe names it, found through the cache index, plus the default-name probe); scene findings run
  on whole-project runs only (as `--anims`).
- `editor/spritenormals/SpriteNormalsCheck.h`: `struct NormalsCheck { std::vector<ProjectChecks::Finding> findings;
  size_t recipes = 0; size_t maps = 0; }; NormalsCheck checkSpriteNormals(const fs::path& projectRoot, const
  std::vector<fs::path>& files);` Findings: tool `normals`, `code` and `suggestion` set; `ProjectChecks::toJson` and
  `sortFindings` already handle them (add `normals` to the tool order after `vat`).

### 5.2 Codes

| code | severity | at | when | suggestion |
|---|---|---|---|---|
| `normals-source-changed` | warning | the generated PNG | the recipe's source SHA-1 differs | "regenerate it: right-click it in the Project window > Regenerate Normal Map, generate_sprite_normals with the `regenerate` arguments get_asset_info gives, or `generate-sprite-normals` with the same settings" |
| `normals-source-missing` | warning | the generated PNG | the recipe's source no longer exists | "restore <source>, or generate from another source; the normal map itself still works" |
| `normals-recipe-invalid` | warning | the generated PNG | CRC/JSON/type error in the recipe | "generate it again (a fresh recipe is written)" |
| `normals-size-mismatch` | error | scene/prefab file | a submesh's `normalTexture` pixel size differs from its `baseColorTexture` (they share UVs: lighting is misplaced) | "generate the normal map from this colour texture (generate_sprite_normals {source: <GameObject>, output: true, apply: true})" |
| `normals-frames-mismatch` | warning | scene/prefab file | the normal map's recipe cells differ from the Sprite's frames (frames resliced after generating) | "regenerate with frames auto from the Sprite" |
| `normals-filter-mismatch` | warning | scene/prefab file | colour and normal textures of one submesh have different min/mag filters (pixel art: Nearest colour + Linear normal smears lighting) | "set the normal texture's filter to the colour texture's (apply: true does it)" |
| `normals-directx` | warning | scene/prefab file | a normal map in use whose recipe says `directx: true` | "regenerate without directx: the engine reads OpenGL (green up)" |
| `normals-green-convention` | warning | scene/prefab file | a normal map in use **without** a recipe that looks DirectX: over all islands' top-rim pixels (opaque, transparent above) at least 70% have green < 128 and over bottom-rim pixels at least 70% have green > 128, with at least 32 rim pixels of each | "its green looks flipped (DirectX): the engine reads OpenGL since this version; invert its green channel, or generate it again" |
| `normals-not-a-normal-map` | warning | scene/prefab file | a normal map in use whose opaque texels don't decode to unit vectors (mean \|len - 1\| > 0.1) or have blue < 128 on more than 5% of them (a colour image in the normal slot) | "assign a normal map (generate_sprite_normals) or clear the slot" |
| `normals-no-light-height` | warning | scene file | the scene has a submesh with a `normalTexture` and Light 2Ds, but none with `height` > 0 | "set a Light 2D's height above 0 (64 is a good start): normal maps respond only then" |

- Scene findings come from the `.scene` and `.prefab` YAML (pure parsing, the `--anims` way: MeshComponent submesh
  materials `baseColorTexture`/`normalTexture` paths and `minFilter`/`magFilter`, SpriteComponent frames, Light2D
  `height`; keys as `editor/Stream.cpp` writes them). Messages name the GameObject and submesh: "GameObject 'Hero'
  submesh 0: hero_normal.png is 64x32 but its colour texture hero.png is 128x32 ...". Prefab-instance overrides are
  read like the clip check reads them.
- Image analysis (sizes, unit vectors, green convention) reads each normal map once per version.

### 5.3 Output example

```
assets/sprites/hero_normal.png: warning: made from assets/sprites/hero.png, which changed since: the normal map no longer matches its sprite (fix: regenerate it: ...) [normals-source-changed]
Main Scene.scene: error: GameObject 'Hero' submesh 0: hero_normal.png is 64x32 but its colour texture hero.png is 128x32: they share UVs, so the lighting lands on the wrong pixels (fix: ...) [normals-size-mismatch]
```

### 5.4 Cache

`.jasperengine/normals-check-cache.json` (written when `.jasperengine/` exists): per PNG path `{mtime, size, recipe?:
<parsed or null>, width, height, analysis?: {unit_ok, blue_low_pct, green_verdict}}` and per source `{mtime, size,
sha1}`; plus a reverse index `source -> [generated PNGs]` rebuilt from the cached recipes. Only the chunks before IDAT
are read for recipes (the texsynth scan); the pixel analysis decodes a PNG only when it is used as a normal texture and
not cached. A second run on an unchanged project decodes nothing and hashes nothing.

## 6. Shared behaviour with the window

The tool, the CLI and the window (Phase 4) all go through the Phase 2 session, so a preview in one is the same pixels
as a write in another. This phase adds nothing the window would need to duplicate; the scene-aware warnings
(`no_light_height`, `no_light`, `filter_mismatch`, `shader_ignores_normals`, `directx_in_engine`) are computed by a
function in `SpriteNormalsTools.cpp` that takes `(Project*, SceneProject*, Entity or NULL_ENTITY, const Spec&)` and
returns `{kind, message, fix}` items; declare it in `McpTools.h` so Phase 4 may call it for its banner (Phase 4 has its
own wording and is not required to).

## 7. Guide `docs/mcp/guide/sprite-normals.md`

Served as `jasper://docs/sprite-normals` (CMake glob, nothing to register). Outline and the content each section must
carry:

1. **Title and summary**: "Sprite normal maps (`generate_sprite_normals`)"; what it makes (tangent-space normal maps
   from a sprite's silhouette and painted shading, OpenGL green up, same size and frame layout as the sprite, alpha kept,
   flat (128,128,255) where transparent), that it runs in the editor only, credit (port of Sprite Fusion's MIT
   sprite-normals), humans: Window > Sprite Normal Map and the Project window's right-click > Generate Normal Map...
2. **When it shows**: 2D lights only; a Light 2D lights a normal map only when its `height` > 0 (new lights get 64;
   older ones are 0); lower `ambient_light_2d_intensity` to see the light; sprites without a normal map are unaffected
   by height. One example `set_properties {entity: "Light 2D", values: {light2d: {height: 64}}}`.
3. **The loop**: preview -> look at the sheet and `hint`/`warnings` -> adjust `preset`/`params` -> write with the same
   arguments + `output: true` (+ `apply: true`) -> `screenshot` of the Game view with a light near the sprite. Undo
   scopes (project for files, scene for apply). Job/wait/timeout in two sentences.
4. **Arguments**: the table of 3.2 with ranges.
5. **Presets and settings**: the contract preset table (values and intent), what each setting does in one line,
   profiles and edges with when to use each, `ignore_outline`, `regions` with two examples (exclude a highlight colour;
   lift armour plates).
6. **Sprites, sheets, animations, tiles**: frames auto/none/object; GIF/APNG -> atlas (columns = ceil(sqrt n), row
   major), what `apply` sets (frames, Sprite Animation, frame_ms from delays, 0-delay -> 100 ms), `.frames.json` only
   on request; tilesets: `frames: {tile_size: 16}`, `params: {edges: "extend"}`; seamless textures: `edges: "wrap"`.
7. **Reading the result**: the contact sheet layout, each metric with thresholds (flat > 60%, steep > 25%, noise >
   15%), assessment lines, the hint.
8. **Problems**: two tables: errors (kind, means, fix) and warnings (kind, means, fix), from 3.8.
9. **Using it on a GameObject**: the property paths (`submeshes[0].material.normalTexture`, every submesh for
   tilemaps), filters (apply copies them), the green convention (OpenGL; `directx` only for exporting to Unreal), the
   library Sprite shader samples normal maps (after Phase 1), mirroring works, Lua (the Phase 1 setter, one line).
10. **Recipes and regenerating**: the iTXt recipe, `get_asset_info` (`sprite_normals`, `regenerate`,
    `normal_map` on the source), `external_changes.problems` after a source edit, `check --normals` codes table.
11. **Shell**: the CLI in three examples (single, batch with `--output-dir`, GIF with `--frames-json`), `--dry-run`,
    `--preview-out`, JSON output keys, exit codes.
12. **Worked recipes** (complete JSON calls):
    - a single sprite: preview, then `output: true, apply: true` on the GameObject `"Hero"`;
    - a sliced character sheet: `set_sprite_frames` first, then `source: "Hero"` (frames from the Sprite);
    - a GIF to an animated, lit sprite: `source: "assets/fx/torch.gif", output: true, apply: "Torch"`;
    - a 16 px tileset on a Tilemap: `source: "Ground", frames: {tile_size: 16}, params: {edges: "extend"}, output:
      true, apply: true`;
    - fixing painted highlights: `params: {regions: [{colors: ["#ffffff", "#fff3c4"], detail: 0}]}`;
    - a seamless tile: `params: {edges: "wrap"}`.
13. **Common mistakes and limitations**: height 0; linear filter on pixel art; a normal map from another sheet size;
    DirectX maps; brightness read as height (dark albedo becomes dents: use regions); 1 px lines get no sideways slope;
    overlapping parts merge (no depth between an arm and the body); frames processed independently (painted detail that
    moves flickers: higher smoothing); not for UI Images.

Length target: 12-16 KB. Every tool, argument, property path and Lua name in it must exist (guide/README.md rule; the
docs test greps a few).

## 8. Other docs and registrations

### 8.1 Agent guide line (`editor/mcp/McpInstructions.cpp`)

The instructions are 16,232 bytes of their 16,384 budget. Change only the "## 2D and 3D" first bullet's last sentence,
from "...; Shadow Caster 2D needs the\n  light's shadows on." to:

```
  adds to the 2D ambient (lower ambient_light_2d_intensity to see it); Shadow Caster 2D needs the
  light's shadows on; normal maps (generate_sprite_normals, jasper://docs/sprite-normals) need
  its height > 0.
```

(+91 bytes -> 16,323.) No other phase edits this file; if Phase 1 needs a line it is folded in here at integration.

### 8.2 The rest

- `docs/mcp/guide/README.md`: a table row after texture-synthesis: `[sprite-normals.md](sprite-normals.md) |
  jasper://docs/sprite-normals | generate_sprite_normals: the preview-then-write loop with the lit contact sheet, presets
  and every setting, frames for sheets, GIFs and tilesets, apply, reading metrics, errors and warnings, recipes and
  check --normals, the CLI, worked recipes`.
- `docs/mcp/README.md`: a bullet after texture synthesis: "- generate normal maps for sprites, sheets, GIF animations and
  tilesets so 2D lights give them volume, with a lit preview before writing
  ([guide/sprite-normals.md](guide/sprite-normals.md), `generate_sprite_normals`)".
- `docs/mcp/DESIGN.md` module list: `SpriteNormalsTools.cpp  sprite normal maps (generate_sprite_normals; the recipe
  summary get_asset_info shows), sharing spritenormals/SpriteNormalsSession with Window > Sprite Normal Map and the
  generate-sprite-normals CLI`.
- `docs/mcp/checks.md`: `--normals` in the synopsis, the options table (and the `--all`/default sentence: "All eight";
  default adds `--normals`), `tool` list gains `normals`, the codes paragraph, and a section "What the normal map check
  runs" with 5.2's table, 5.3's example, the cache, `--files` behaviour.
- `tools/mcp/gen_tool_docs.py`: `("SpriteNormalsTools", "Sprite normal maps")` after `("SynthesisTools", ...)`;
  regenerate `docs/mcp/TOOLS.md` with it.
- `README.md`: an Editor feature bullet after Texture Synthesis's: "- Generate normal maps for pixel-art sprites, sheets,
  GIF/APNG animations and tilesets so 2D lights give them volume: presets, a lit preview with a movable light, frames
  from the Sprite, recipes that know when the sprite changed (Window > Sprite Normal Map, the `generate-sprite-normals`
  CLI, or the `generate_sprite_normals` MCP tool; a port of Sprite Fusion's sprite-normals)"; and in "Command-line
  tools": `jasperengine-editor generate-sprite-normals --help  # normal maps for sprites, sheets and GIFs, no window`
  (also add the missing `synthesize-texture --help` line).
- `tools/mcp/tests/test_docs.py`: `"sprite-normals"` in `DOCS`.

## 9. Tool-definition budget

- Before the first commit, measure: `uv run --no-project --with mcp --with pyyaml python tools/mcp/tool_budget.py
  --top 15` (record the total and the top rows in the report). After: the same.
- The new tool's `name + description + compact inputSchema` must be at most 1,500 characters (descriptions inside the
  schema empty, nested objects loose; `params` and `frames` objects not spelled out).
- The tool must pay for itself: bring the total back to no more than the measured "before" + 300 characters by
  trimming text that the guides already say (move, don't lose: every removed sentence must exist in a guide the tool
  description points to). Report the before/after totals and what was trimmed. If that is impossible without losing
  information, stop at + 1,500 and say so in the report.

## 10. Tests

All new tests are `tools/mcp/tests/test_*.py` (auto-discovered by `run_tests.py`), each with a module docstring listing
what it covers, generating their inputs in code (pure-Python PNG/GIF/APNG writers; `htmlui_pixels.decode_png` to read
back) or copying Phase 2's fixtures from `editor/spritenormals/tests/fixtures/`.

### 10.1 `test_generate_sprite_normals.py` (editor, new project)

Inputs: a shaded 32x32 ball with transparency, a 128x32 sheet of 4 frames, a 3-frame GIF (delays 50/90/0 ms, one with
disposal), a 2-frame APNG, an opaque 32x32 tile, a dithered sprite, one upstream golden fixture.

1. Invalid arguments each fail with their kind and write nothing: missing `source`; unknown file and GameObject; a
   `.txt`; `preset: "x"`; `params: {detail: 9}`; `params: {bogus: 1}`; `frames: {tile_size: 40}` on 32x32;
   `frames: "none"` on the GIF; `apply: true` without `output`; `apply: true` with a file source; `output:
   "../x.png"`; `output: "assets/.hidden/x.png"`; `output: "assets/x.jpg"`; output equal to the source with
   `overwrite: true`.
2. Preview: nothing new on disk (directory listing before/after); one image, PNG, width <= 1024; `structuredContent`
   has every key of 3.7 with the right types; `default_output.normal` = `<stem>_normal.png`; settings equal the preset
   (`detailed` values when `preset: "detailed"`); the text says "Preview only".
3. Write with the same arguments + `output: true`: the file exists; it decodes; alpha byte-equal to the source's;
   RGB (128,128,255) wherever alpha <= 7; every opaque texel decodes to a unit vector within 0.02; its iTXt
   `jasper.spritenormals` recipe parses with `format`, `version` 1, `source.sha1` = SHA-1 of the source, settings equal
   the result's; a second write to another path gives identical pixels (determinism); `undo` scope project removes the
   file, `redo` restores it.
4. Parity anchor: the upstream fixture with `preset: "detailed", params: {radius: 4}` written and compared with the
   committed upstream output: every channel within 1.
5. Overwrite: a second write without `overwrite` fails `output_exists` and leaves the file's bytes unchanged; with
   `overwrite: true` it succeeds; one undo puts the old bytes back.
6. GameObject source: create a Sprite from the sheet with `set_sprite_frames {slice: {tile_size: [32, 32]}}`; preview
   `source: "<name>"` reports `frames.from: "sprite"`, `count: 4`; write with `apply: true`: the GameObject's
   `submeshes[0].material.normalTexture` is the file (get_properties), its filter equals the colour texture's (set the
   colour texture to Nearest first with `set_texture_settings`); `undo` lists two steps; undoing the scene step clears
   the slot, the file stays.
7. GIF with `output: true, apply: "<a new Sprite>"`: files `<stem>_sheet.png` + `<stem>_sheet_normal.png`; the atlas is
   2x2 cells; the Sprite's frames are the 3 cells; a Sprite Animation child exists with `frame_ms` [50, 90, 100];
   `warnings` contains `zero_delay`. APNG likewise (2 cells, its delays).
8. Warnings: the opaque tile -> `no_alpha`; with `params: {edges: "extend"}` it is gone. A scene whose only Light 2D
   has height 0 (set with set_properties) and a GameObject source -> `no_light_height` naming the light; after setting
   64 it is gone; a scene with no light -> `no_light`. `directx: true` with apply -> `directx_in_engine`.
9. `height: true` writes a 16-bit grey PNG of the same size (IHDR bit depth 16, colour type 0).
10. `output: {normal: "assets/n/custom.png", frames: true}` on the sheet with `frames: {tile_size: 32}` writes both,
    the JSON has `frame_count` 4 and `version` 1.
11. `save_path` on a preview writes the sheet PNG (same bytes as the returned image) and nothing else.
12. Job path: `timeout_seconds` tiny on the largest input returns `job_id` + `status: "running"` or a finished result;
    `get_job` reaches `succeeded` with the same data (no images); `cancel_job` on a new run writes nothing.
13. `get_asset_info` on the written map: `sprite_normals.source.state` `unchanged`, `regenerate` present; calling the
    tool with `regenerate` reproduces identical pixels; after editing the source (write_file of new PNG bytes) the state
    is `changed`, `stale` true, and the sync result's `external_changes.problems` names the normal map. `get_asset_info`
    on the source reports `normal_map`. `view_asset` text contains "Sprite normal map".
14. Playing: start play, `apply` fails `scene_playing` and nothing is written; a preview during play succeeds; stop.
15. (last: switches projects) a run started with a long timeout-less job, then `open_project` another: the job fails
    `project_changed`, nothing written in either project.

### 10.2 `test_sprite_normals_cli.py` (binary only, scratch directories, run from a working directory below the inputs
so relative paths must resolve against it)

1. `--help` exits 0 and prints "generate-sprite-normals"; usage errors exit 2 with the reason: unknown option,
   `--detail 9`, `--preset x`, `--region colors=red`, `-o` with two sources, `--preview-out` with two sources,
   `--frames-json` on a plain image, `--no-frames` on a GIF, `--sprite` without `--project`.
2. Single source, relative paths, `--format json`: one JSON object on stdout with every key of 4.2; the file exists
   with its recipe; stdout has nothing else (engine logs off stdout).
3. Batch `--source a.png --source b.png --output-dir out`: both written; a duplicate stem from two folders into one
   `--output-dir` exits 2 naming both, and `out/` stays empty.
4. Existing output without `--overwrite`: exit 1 `output_exists`, bytes unchanged; with it: replaced. Output equal to
   the source (also through `dir/../a.png` and a symlink) exits 2 even with `--overwrite`; the source is unchanged.
5. `--dry-run` writes nothing (not even `--preview-out`), reports `written: false` and the planned paths.
6. `--preview-out p.png` writes a PNG <= 1024 wide.
7. GIF: atlas + normal atlas; `--frames-json` schema (`version`, `frame_count`, `delays_ms`, `frame_ms`, `loop`).
8. All or nothing: a batch where the second output's directory is a file: exit 1, the first output is not written.
9. Determinism: two runs, `--threads 1` and `--threads 0`, byte-identical PNG pixel data.
10. Without `--project` the recipe's `source.path` is relative to the output directory; with `--project` it is
    project-relative and an output outside assets exits 2.
11. `--sprite` with a project (created through the editor binary's MCP harness once, saved): frames from the Sprite in
    the JSON (`from: "sprite"`).

### 10.3 `test_sprite_normals_check.py`

Project made through MCP and saved, then `check --normals --format json` (and the default set) run on it:
`normals-source-changed` after rewriting the source; `-source-missing` after deleting it; `-recipe-invalid` after
corrupting the chunk CRC; `-size-mismatch` (a 64x32 normal on a 128x32 sprite, error, exit 1);
`-filter-mismatch`; `-directx`; `-green-convention` (a hand-made map with flipped green on a ball);
`-not-a-normal-map` (the colour image in the slot); `-no-light-height`; `-frames-mismatch` (reslice after generating).
Clean project: no `normals-*` finding, exit 0. `--files <source.png>` reports the map made from it. A second run
creates no new cache entries and returns the same findings; deleting the cache gives the same findings.

### 10.4 Docs and budget

- `test_docs.py`: `sprite-normals` in `DOCS` (listed and readable).
- In `test_generate_sprite_normals.py`: the tool description contains `jasper://docs/sprite-normals`; the instructions
  contain `generate_sprite_normals` and stay <= 16,384 bytes.
- `python3 tools/mcp/gen_tool_docs.py --check` (or the repo's equivalent) passes: TOOLS.md is current and no tool lands
  in "Other".
- The budget measurement of section 9 in the report.
- The existing suites that touch shared code still pass: `test_synthesize_texture*.py`, `test_assets.py`,
  `test_checks.py`, `test_autosync.py`, `test_docs.py`, `test_protocol.py`, `test_bake_vat_cli.py`.

## 11. Acceptance criteria

1. Every behaviour of sections 3-5 exists and is covered by a test of section 10; all of section 10 passes headless on
   Linux (Xvfb, software GL) with the Release editor build.
2. A preview and a write with the same arguments produce identical pixels; the parity anchor holds within 1.
3. Nothing is written by a failing call, a preview (except `save_path`), a dry run, a failed preflight, or a cancelled
   job; inputs are never overwritten.
4. `apply` leaves the scene correct (normal texture, filters; frames and Sprite Animation for animations) and is one
   Scene undo step; files are one Project undo step.
5. `check --normals` findings match 5.2; a cached run decodes and hashes nothing.
6. The agent guide <= 16,384 bytes with the 8.1 line; the guide document exists and is served; TOOLS.md regenerated;
   README, checks.md, DESIGN.md, guide README updated; budget reported.
7. No existing test changes expectations; no existing tool's schema changes.
8. `-Wall -Wextra` clean in new files; no new third-party dependency.

## 12. Task breakdown (one implementer, in order, a commit per task)

1. **Measure** the tool budget (report). Read the merged Phase 2 headers and the apply command.
2. **Arguments**: `SpriteNormalsTools.cpp` skeleton, schema (3.1-3.2), source resolution (3.3), argument -> `Spec`
   mapping and every error kind except run-time ones; register (`McpTools.h`, `McpServer.cpp`, CMake). Test items
   10.1.1.
3. **Run and preview**: job, run, result (3.5, 3.7), contact sheet image, `save_path`, assess warnings + scene-aware
   warnings (3.8, section 6 helper). Tests 10.1.2, 8, 11, 12.
4. **Write and apply**: outputs, overwrite (both checks), undo steps, recipe, apply (3.6) incl. GIF/APNG and Tilemap,
   `apply_args`, playing refusal, project switch. Tests 10.1.3-7, 9, 10, 14, 15.
5. **Asset info**: get_asset_info / view_asset / file-sync hook (3.9). Test 10.1.13.
6. **CLI** (section 4) + main/CommandLine wiring. Tests 10.2.
7. **Check** (section 5) + cache + CommandLine `--normals`. Tests 10.3.
8. **Docs** (sections 7-8), TOOLS.md regeneration, budget trimming (section 9), docs tests 10.4.
9. Full run of the new and the listed existing suites; report with commands and outputs.
