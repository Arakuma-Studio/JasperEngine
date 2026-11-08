# Project checks: `jasperengine-editor check`

The checks the MCP tools run (`check_script`, `check_shader`, `compile_scripts`, the HTML/CSS
UI diagnostics the automatic pick-up reports, and the synthesized textures' and Vertex Animations'
source state that `get_asset_info` shows) are also a CLI subcommand, so an agent that edits
files with its own tools, a shell, a CI job or an agent hook gets the same findings without an
editor running. Both sides call the same code (`editor/util/ProjectChecks.{h,cpp}`).

```
jasperengine-editor check --project <dir|project.yaml> [--lua] [--shaders] [--cpp] [--ui] [--textures] [--anims] [--vat] [--normals] [--all]
                          [--files <paths...>] [--format text|json] [--quiet] [--hook]
```

| Option | |
|---|---|
| `--project <dir\|project.yaml>` | The project: its directory or its `project.yaml`, absolute or relative to the current directory. |
| `--lua` | Every `.lua` file of the project: syntax (compiled, not run), then the script's top level run in an isolated Lua state to read its `properties`, as the editor does (see below). |
| `--shaders` | Every shader fork: the forks the scenes, prefabs and scene settings use, and every `.vert`/`.frag` pair no scene uses yet (its type guessed from the fragment shader). Compiled with the engine's shader compiler for the editor's backend, exactly as `check_shader` does, includes resolved as the engine resolves them. Also an error for each material of the scenes and prefabs whose shader (or extra pass) reads the scene depth or colour (`@require scenedepth` / `scenecolor`) while it draws in the opaque queue, at the scene or prefab file. Without `--files`, also the Stochastic Tiling warnings (below). A project UI effect (a `.frag` whose manifest, the comment lines after `#version 450`, starts with `// @effect`; declared with `@effect` or not) is checked as one: its manifest (`HTMLUI-FX-0014` at the line, a built-in's name `HTMLUI-FX-0012`), then a compile in each of its forms (paint effects: paint and filter; an effect with state, phase 6, also the step and scatter forms of each; errors `HTMLUI-FX-0015` at the `.frag` or include line), then its parameter block against the manifest (`HTMLUI-FX-0014` at the `@param` line); the two forms of a paint effect report an error once. |
| `--cpp` | The C++ scripts: generated sources, CMake configure and build, as Play and `compile_scripts` do (same build directory, `.jasperengine/build`, so it is incremental). Slow the first time (a configure plus a full build; about 25 s for one script here), then a few seconds to tens of seconds. |
| `--ui` | HTML/CSS UI documents: what the engine's compatibility layer drops or rewrites lossily, missing and remote files, JavaScript, each with `file:line:col`, a stable code and a fix (see below). Pure parsing, no engine start: about 1 ms per document. |
| `--textures` | Synthesized textures: every project PNG that carries a texture synthesis recipe (an iTXt chunk `jasper.synthesis`, written by `synthesize_texture` and Window > Texture Synthesis) whose sources changed or disappeared since it was made: a warning at the PNG (see below). Reads only the PNG chunks before the image data, and hashes sources only for PNGs with a recipe, both cached by mtime and size: milliseconds once cached. |
| `--anims` | Animation clips (`.anim`): every clip read as the engine reads it (an error with `line:col` for text that is not a clip; a warning with the JSON path of each part the lenient reader drops), and the clips the project's scenes and prefabs reference: an Animation Player entry naming a missing `.anim` (an error; a warning when its GUID finds the moved file), a model clip whose model is missing or has no animation of that name (see below). Pure parsing, no engine start. |
| `--vat` | Vertex Animations (`.vat`): every `.vat` read as the engine reads it (an error with `line:col` for text that is not one, a warning with the JSON path of each editable field the lenient reader repairs), a source changed or missing since the bake (warnings), a `.vat.glb` that is missing or no longer matches the `.vat` (errors), and a payload texture over the device limit of the recipe's preset (an error; see below). Loads the project (no scenes). |
| `--normals` | Sprite normal maps: generated ones (a PNG with a `jasper.spritenormals` recipe, written by `generate_sprite_normals`, the `generate-sprite-normals` CLI and Window > Sprite Normal Map) whose source changed or went missing, and normal maps on Sprites, Tilemaps and Polygons (not 3D meshes, whose Lit normal maps are sized independently) that don't match their colour texture (size, frames, filter), look like DirectX or aren't normal maps, or no Light 2D can show (height 0). Pure parsing, no engine start; cached (see below). |
| `--pixel-art` | The pixel-art rules of `check_pixel_art` (docs/specs/S42-pixel-art-mode.md) on the saved project: one line per finding, `path[#GameObject]: error\|warning: [rule] message (fix: ...) [PIXELART-<RULE>]` (FAIL is an error, WARN a warning), then the call that fixes (`jasperengine-editor call --project <p> check_pixel_art '{"fix":["all"],"output":true}'`); `--format json` adds the full result as `pixel_art`. In the default set and in `--hook` while the project's Pixel Art Mode is on (the hook checks it when an image, scene, prefab, meta or project.yaml changed). |
| `--all` | All eight. Without `--lua/--shaders/--cpp/--ui/--textures/--anims/--vat/--normals/--all`: `--lua --shaders --ui --textures --anims --vat --normals` (C++ only when asked, since it builds). |
| `--files <paths...>` | Only these files: absolute, relative to the current directory, or relative to the project. Lua files are checked one by one; a `.vert`/`.frag` checks its fork (a UI effect `.frag`, the effect); a `.glsl` checks every fork and UI effect (any can include it); a C++ file (`.h .hpp .hh .hxx .cpp .cc .cxx .c .inl`) builds the scripts when `--cpp` is given and is ignored otherwise; an `.html`/`.htm` is converted as a document (its stylesheets, imports, images and fonts with it), a `.css` is checked as the project's documents that load it see it (its findings and its imports'), or on its own when no document loads it. An image is checked as a synthesized texture when it carries a recipe, and as a source otherwise: the project's synthesized textures made from it are checked (an edited example reports the textures to regenerate). An `.anim` is checked as a clip (its references from scenes are checked on whole-project runs only). A `.vat` is checked as a Vertex Animation, a `.vat.glb` as its `.vat`, and any other file as a source: the project's `.vat` files whose recipe read it are checked (an edited model reports the bakes to rebake). An image is also checked as a generated normal map when it has a sprite normals recipe, and as a source otherwise (the normal maps whose recipe names it, found through the cache's index, plus `<stem>_normal.png` / `<stem>_sheet_normal.png` beside it); the scene findings of `--normals` run on whole-project runs only. Files outside the project, in hidden or `build` directories, deleted, or of other kinds are skipped; when nothing is left, the command returns at once (no engine start). |
| `--format json` | One JSON object on stdout (below). Default `text`. |
| `-q`, `--quiet` | No summary line on stderr. |
| `--hook` | Agent hook mode (below). |

## Output

Text: one line per problem on stdout, compiler style, paths relative to the project directory,
sorted by tool, path and line:

```
scripts/Enemy.lua:12: error: unexpected symbol near '='
scripts/Boss.lua:3: error: runtime error at load: attempt to call a nil value (global 'spwan')
scripts/Boss.lua: error: property 'speed' (float): default must be a number, got string
shaders/Water.frag:229: error: 'wobble' : undeclared identifier
shaders/inc/noise.glsl:14: error: 'hash3' : no matching overloaded function found
scripts/Spinner.cpp:31:16: error: 'undefinedThing' was not declared in this scope
cpp: error: the C++ build failed without a file:line diagnostic (CMake configuration or linking?); last output: ...
assets/ui/shop.css:13:8: warning: 'mix-blend-mode' is not supported; dropped (fix: use opacity, or pre-blended images) [HTMLUI-CSS-0002]
assets/ui/shop.html:8:3: error: JavaScript is not run (the engine has no JavaScript engine) (fix: use <script type="text/lua"> with the element API (document:querySelector, addEventListener, ...)) [HTMLUI-HTML-0006]
```

`path:line:col: severity: message`, with an unknown column or line left out (Lua and glslang give
no column); a problem without a file starts with the tool name. UI findings add the fix, ` (fix:
...)`, when there is one, then the code, ` [HTMLUI-CSS-0002]`. Then a summary on stderr (unless
`--quiet`): `check: 1 error, 3 warnings (lua 7 files, shaders 1 fork, ui 2 files) in 0.22s`. Engine
and compiler logs are discarded; when a C++ build fails without a file:line, its last 30 output
lines follow on stderr.

JSON (`--format json`):

```json
{
 "ok": false,
 "errors": 1,
 "warnings": 1,
 "checked": {"lua": 7, "shaders": 1, "ui": 2},
 "findings": [
  {"tool": "lua", "path": "scripts/Enemy.lua", "line": 12, "col": 0, "severity": "error",
   "message": "unexpected symbol near '='"},
  {"tool": "ui", "path": "assets/ui/shop.css", "line": 13, "col": 8, "severity": "warning",
   "message": "'mix-blend-mode' is not supported; dropped", "code": "HTMLUI-CSS-0002",
   "suggestion": "use opacity, or pre-blended images"}
 ],
 "seconds": 0.22
}
```

`tool` is `lua`, `shader`, `cpp`, `ui`, `texture`, `anim`, `vat` or `normals`; `path` is project-relative (absolute for a file outside the
project, e.g. a system header warning; `engine/shaderlib/<include>` for an engine shader include;
`""` when there is no file); `line`/`col` are 1-based, 0 when unknown; `severity` is `error` or
`warning`. UI findings also have `code` (the compatibility layer's stable diagnostic code) and,
when there is a fix, `suggestion`; so do the shader findings of a UI effect (`code`
`HTMLUI-FX-0012` to `HTMLUI-FX-0015`), the texture findings (`synthesis-source-changed`,
`synthesis-source-missing`, `synthesis-recipe-invalid`), the vertex animation findings (`code` the kind:
`vat-source-changed`, `vat-source-missing`, `vat-mesh-mismatch`, `vat-layout-mismatch`, `vat-device-limit`, `vat-invalid`,
`vat-format`, below), the normal map findings (`normals-*`, below) and the Stochastic Tiling warnings
(`stochastic-tilted-normal-map`, `stochastic-no-effect`, below) and the water warnings (`water-fork-without-role`,
`planar-reflection-not-flat`, below); the other findings have exactly the six keys above.
`checked` counts what ran: Lua files, shader forks, C++ scripts, UI files (documents plus the
stylesheets checked on their own), synthesized textures (`textures`: the PNGs with a recipe whose
sources were checked), clips (`anims`: the `.anim` files read), Vertex Animations (`vat`: the `.vat` files read), normal maps (`normals`: the PNGs with a recipe plus the normal maps in use checked).

Exit code: **0** no errors (warnings allowed), **1** errors, **2** usage error (bad option, no
`project.yaml`, the project fails to load).

The same findings come back from MCP: `check_script` and `check_shader` return `findings` in this
format, and `compile_scripts` returns the same diagnostics as `errors`/`warnings`
(`{file, line, column, message}`).

## Stochastic Tiling warnings

With `--shaders` (and no `--files`), and from MCP `check_shader` without a `shader`, the materials
and terrain layers of the scenes (and open prefab stages) that use Stochastic Tiling are looked at
too. Warnings (tool `shader`, at the scene or prefab file, with a `code` and a `suggestion`):

| Code | When |
|---|---|
| `stochastic-tilted-normal-map` | Rotation (`stochasticRotation`) is above 0 and the normal map the cells turn has an average normal 3 degrees or more off +Z: each rotated cell then leans its own way. Built-in Lit materials (`normalTexture`), terrain layers with a PBR surface (`surfaceLayers[N].normalTexture`) and library shader materials with the `STOCHASTIC` feature (`u_normalTexture`). The mean is of the decoded texel normals (sampled on a 512 x 512 grid at most), measured once per version of the file (its modification time and size). Fix: Rotation 0, or a normal map whose average normal is +Z. |
| `water-fork-without-role` | A material or post-process pass draws with a fork that has library:water's parameters (`buoyancy`, `waveHeight`, `shallowColor`) or library:underwater's (`waterLevel`, `followWater`) but no `@role` line: the engine does not float bodies in it, answer `Water` queries over it or switch the underwater pass. Add `// @role water` (or `underwater`) to its manifest, or fork with `fork_shader source`. |
| `planar-reflection-not-flat` | A mesh reads a planar reflection (`PLANAR_REFLECTION`) but its world bounds are taller than 1% of their larger horizontal extent plus 0.05: the reflection mirrors about one level (the top of its bounds). Use `REFLECTION` alone on sloped water (a river), or flatten the mesh. |
| `stochastic-no-effect` | A material has `stochasticTiling` on but the built-in Lit's Stochastic Tiling does not apply: it has no texture, it draws through a texture rect (a sprite's atlas frame, a tilemap), it is a terrain's base material (layers have their own), its shader has a manifest (a library shader: use its `STOCHASTIC` feature), or its shader is a fork of Lit that predates Stochastic Tiling (its `.frag` does not define `LIT_STOCHASTIC_PARAMS`: update the fork as the shader guide's Forking section shows, or turn the feature off). |

```
Main Scene.scene: warning: GameObject 'Rock' submesh 0: rock_normal.png's average normal is 9° off +Z, and Stochastic Tiling turns its cells (Rotation 0.5): the rotated cells look tilted (fix: set Rotation (stochasticRotation) to 0, or use a normal map whose average normal is +Z) [stochastic-tilted-normal-map]
```

## What the Lua check runs

The editor reads a Lua script's properties by running the script's top level (it returns the
script table with its `properties`). `check` does the same, so a script that fails at load
(`runtime error at load: ...`) or declares a property default it can't read is an error. The
editor (the Inspector, Play's start, `check_script`, `sync_files`) and the CLI both run each
script in a fresh, isolated `lua_State`, never the game's (which is not thread-safe, and Play
reads the properties on its startup thread), so reading or checking a file can't affect
anything:

- no `io`, `debug`, `dofile`, `loadfile`, native modules or binary chunks; `os` has only `time`,
  `clock`, `date` and `difftime`; `print` prints nothing;
- `require` loads the project's Lua modules from the project's Lua directory, as the engine does;
- the engine's globals (`Vector3`, `Engine`, `RegisterEngineEvent`, ...) are inert stand-ins (callable,
  indexable, returning stand-ins), so top-level engine calls run but do nothing; names that are
  not engine globals are `nil`, so `undefinedThing()` fails as in the editor;
- 256 MB of memory and about 100 million VM instructions (an endless loop at load is reported at
  its line).

So the CLI and `check_script` agree, and a top level that depends on real engine values (rare: it
runs before any scene; an engine value used as a property default, say) reads them as stand-ins.

## What the UI check runs

The HTML/CSS check is the engine's own compatibility layer (`engine/core/htmlui/compat`), the code
that converts a document when it loads in the editor or the game, so its findings are what the
Console shows when the document loads, what MCP reports after an edit (`external_changes.problems`)
and what `inspect_ui {file}` returns. It runs without the engine: no window, no RmlUi, only parsing.

- **Documents**: every `.html`/`.htm` of the project's assets directory (project.yaml `assetsDir`)
  is converted as a document: its markup, its `<style>` and linked stylesheets, their `@import`s,
  and the existence of every image, font, SVG and Lottie file they reference (paths resolve as the
  engine resolves them, relative to the file or, with a leading `/`, to the assets directory; a
  path that climbs out of the assets directory is missing). A finding located in a linked
  stylesheet is reported at that stylesheet's path, and once, however many documents link it.
- **Stylesheets**: every `.css` no document links is checked on its own; then a declaration that
  reads a custom property the sheet and its imports do not declare is no finding (a document's
  style attribute or another sheet may declare it). A `.css` named with `--files` that documents
  load is checked in their context: a variable one of them defines is no finding, a typo is.
- **Not checked**: files in hidden or `build` directories, files outside the assets directory, and
  the web export's page template (project.yaml `web.customHtmlShell`), which is a browser page.
- **Severities**: errors are what breaks the UI (a missing file; a remote URL that is not imported,
  since games load no URLs; JavaScript, `<script>` or `on*` attributes, which never run); warnings
  are constructs dropped or drawn differently (an unsupported property, value, selector or at-rule,
  an element the engine can't draw); notes (lossless rewrites) are not listed. Every finding has a
  stable code (`HTMLUI-<AREA>-NNNN`) and most a fix. The documents are converted at the project's
  canvas size (project.yaml `canvasWidth`/`canvasHeight`), as they load.

What every element, attribute, property, value, selector and at-rule does in the engine is in the
generated support matrix, `docs/mcp/guide/html-ui-support.md` (served as
`jasper://docs/html-ui-support`). `jasperengine-editor ui-support` prints the same tables as JSON
(`{version, editor_version, features: [{kind, name, support, note, suggestion, code?, severity?, ...}],
diagnostics: [{code, name, severity, by_case, summary, help}]}`; `kind` is `element`, `attribute`,
`input_type`, `property`, `value`, `function`, `unit`, `selector`, `pseudo_class`, `pseudo_element`,
`at_rule` or `media_feature`; `support` is `native`, `rewritten`, `approximated`, `emulated`,
`fallback`, `unsupported` or `ignored`, with the fork features of this build), and
`tools/mcp/gen_html_ui_support.py` renders it (`--check` fails when the committed matrix is stale).

## What the texture check runs

Every PNG the texture synthesis writes (the primary output, its companion maps, an uncertainty
map) records how it was made, including the path and SHA-1 of every source image: examples,
sample masks, guides, companion sources, the inpaint mask, the target guide
(`editor/texsynth/SynthesisRecipe.h`). The check hashes those sources again:

```
assets/textures/rock_tile.png: warning: synthesized from assets/textures/rock_albedo.png (examples[0]), which changed since: the texture no longer matches its source (fix: regenerate it: right-click it in the Project window > Edit Synthesis... > Regenerate > Save, or synthesize_texture with the `regenerate` arguments get_asset_info gives (mode tileable, seed 7, overwrite: true)) [synthesis-source-changed]
assets/textures/rock_tile.png: warning: synthesized from assets/textures/rock_height.png (companions.height[0]), which no longer exists (fix: restore assets/textures/rock_height.png (or Edit Synthesis... and pick another source) before regenerating; the texture itself still works) [synthesis-source-missing]
```

- Severity **warning**, tool `texture`, one finding per changed or missing source, at the PNG.
- A garbled recipe (CRC mismatch, not JSON, or a value of the wrong type, e.g. a number where a
  path belongs) is one **warning** `synthesis-recipe-invalid` at the PNG saying what is wrong:
  its sources can't be checked; synthesizing it again writes a fresh recipe.
- File names that are not valid UTF-8 (raw Latin-1 bytes) are scanned like the others (shown with
  U+FFFD in `--format json`); they are just not cached (JSON keys are UTF-8), so read again on
  every run.
- A companion or uncertainty output whose primary output exists is covered by the primary's
  findings (the set was made together); named with `--files`, it is reported itself.
- Fast on large projects: for each PNG only the chunks before `IDAT` are read (where the recipe
  sits), and a PNG without a recipe is remembered by its mtime and size, as are the sources'
  hashes, in `.jasperengine/synthesis-check-cache.json` (written when `.jasperengine/` exists).
  A source is hashed only when a recipe names it.
- Regenerating with the recipe's settings and seed reproduces the texture exactly when the
  sources are unchanged; after a change it gives the up-to-date texture. MCP: `get_asset_info`
  on the PNG returns `synthesis.sources` (each `unchanged`, `changed` or `missing`) and
  `synthesis.regenerate`, the `synthesize_texture` arguments to write it again.

## What the clip check runs

Each `.anim` goes through the engine's clip reader (`engine/core/animation/AnimationClipIO`, the
one the editor, the game and `write_file` use):

```
assets/anim/bob.anim:5:1: error: not valid JSON: syntax error while parsing value - unexpected '}'; expected '[', '{', or a literal
assets/anim/walk.anim: warning: tracks[2].keys[0].value: expected [x, y, z]
assets/anim/old.anim: error: This is a Unity .anim (YAML). JasperEngine clips are JSON {"format": "jasperengine.anim"}: create one with Assets > Create > Animation Clip or edit_animation {clip, create}.
Main Scene.scene: error: an Animation Player plays 'anim/run.anim', which does not exist (paths are relative to the assets folder)
```

- Tool `anim`. **Errors**: text that is not JSON (`line:col`), a root that is not an object, a
  missing or different `format`, a missing or newer `version`, a Unity YAML clip; the clip does not
  load at all.
- **Warnings**: everything the lenient reader repairs or drops (a bad key, track or event, keys out
  of order, an unknown wrap...), with the JSON path in the message; the rest of the clip loads.
- Without `--files` (and in the default set), also every Animation Player entry of the project's
  `.scene` and `.prefab` files: a `.anim` that does not exist is an error, or a warning when the
  entry's GUID finds the clip somewhere else (moved outside the editor: the editor resolves it and
  updates the path when the scene is opened and saved); a model clip (`models/hero.gltf#Walk`)
  whose model file is missing is an error, and so is one whose animation name (or index) the
  model doesn't have: the names are the ones the Inspector shows (`"Wave 2"` for the second
  `"Wave"`, `"Animation 2"` for an unnamed one), read from the `.gltf` JSON or the `.glb` JSON
  chunk, for an `.fbx` from the `.glb` the editor imported it to (an FBX not imported yet is not
  checked). The entries of a scene's prefab-instance overrides (and of GameObjects added to an
  instance) are checked like the others.
- Also warnings: an entry whose `.anim` exists but whose GUID names another clip (the editor
  resolves the GUID first and plays that one), and a `.scene` or `.prefab` whose YAML can't be read
  (its entries are not checked).
- With `--files`, only the named `.anim` files (no reference scan). Binding paths against a
  hierarchy are an editor check: `edit_animation`/`get_animation` with `root`.

## What the vertex animation check runs

A `.vat` records how it was baked or imported (its recipe: the sources with a SHA-1 of every file
the bake read, and the settings, `docs/vertex-animation.md`). The check reads it with the engine's
reader, hashes the sources again, reads the payload in its `.vat.glb` and compares the two
(`editor/vat/VatBakeSession.h` `checkVatStaleness`, what the `.vat` Inspector and `get_asset_info`
show):

```
assets/models/hero.vat: warning: 'assets/models/hero.gltf' changed since 'assets/models/hero.vat' was baked (fix: Rebake (Inspector > Rebake, or bake_vertex_animation {rebake: "assets/models/hero.vat"})) [vat-source-changed]
assets/vat/crowd.vat: error: the payload texture is 4096x37 texels, over the mobile_web preset's 2048 (mobile_web/WebGL2 devices guarantee 2048x2048, desktop 8192): it doesn't upload on those devices and the mesh draws at rest (fix: rebake with a lower --fps, ...) [vat-device-limit]
```

Tool `vat`, at the `.vat`, `code` the kind and `suggestion` the fix:

| Code | Severity | When | Fix |
|---|---|---|---|
| `vat-source-changed` | warning | a file the bake read (model, `.anim`, `.poseanim`, prefab, scene, export texture...) has changed | Rebake: Inspector > Rebake, `bake_vertex_animation {rebake}` or `bake-vat --rebake` |
| `vat-source-missing` | warning | a file the bake read is gone (the `.vat` still plays) | restore it, or bake from another source |
| `vat-mesh-mismatch` | error | the `.vat.glb` is missing, holds no payload, or its payload doesn't fit the `.vat` | Rebake |
| `vat-layout-mismatch` | error | the `.vat` and its `.vat.glb` describe different layouts: a generated field (`layout`, clip `start`/`frames`/`fps`, `mode`) was edited, or the two files come from two bakes | Rebake |
| `vat-device-limit` | error | the payload texture (width or height) is over the device limit of the recipe's `preset`: 2048 for `mobile_web` (what WebGL2 and GLES3 devices guarantee), 8192 for `desktop`; such a texture doesn't upload there and the mesh draws at rest | rebake with a lower fps, fewer clips, normals off or Bones mode (or, off the web and mobile, the desktop preset) |
| `vat-invalid` | error | the text is not a `.vat` (not JSON, wrong `format`, a newer `version`, an invalid generated field), at `line:col` when known | bake it again from its source (`bake-vat --source ... --output <the .vat> --overwrite`), or restore it |
| `vat-format` | warning | an editable field (a clip's `wrap`, `speed`, an event...) the reader repaired, with its JSON path | fix the value |

The CLI that bakes them is `jasperengine-editor bake-vat` (`--help`; `docs/vertex-animation.md`).

## What the normal map check runs

A generated normal map records how it was made (its recipe: the source with its SHA-1, the settings and the frames;
`docs/mcp/guide/sprite-normals.md`). The check reads the recipes of the project's PNGs (the chunks before the image
data only), hashes their sources, and reads the scenes' and prefabs' YAML (MeshComponent submesh materials, Sprite
frames, Light 2D heights; prefab-instance overrides included) for the normal maps in use:

```
assets/sprites/hero_normal.png: warning: made from assets/sprites/hero.png, which changed since: the normal map no longer matches its sprite (fix: regenerate it: ...) [normals-source-changed]
scenes/Main.scene: error: GameObject 'Hero' submesh 0: hero_normal.png is 64x32 but its colour texture hero.png is 128x32: they share UVs, so the lighting lands on the wrong pixels (fix: ...) [normals-size-mismatch]
```

Tool `normals`, `code` and `suggestion` set:

| Code | Severity | At | When | Fix |
|---|---|---|---|---|
| `normals-source-changed` | warning | the generated PNG | the recipe's source SHA-1 differs | regenerate it: right-click it in the Project window > Regenerate Normal Map, `generate_sprite_normals` with the `regenerate` arguments `get_asset_info` gives, or `generate-sprite-normals` with the same settings |
| `normals-source-missing` | warning | the generated PNG | the recipe's source no longer exists | restore it, or generate from another source; the normal map itself still works |
| `normals-recipe-invalid` | warning | the generated PNG | the recipe chunk is garbled (CRC, JSON, types) | generate it again (a fresh recipe is written) |
| `normals-size-mismatch` | error | scene/prefab file | a submesh's normal texture's pixel size differs from its colour texture's (they share UVs: lighting is misplaced) | generate the normal map from this colour texture (`generate_sprite_normals {source: <GameObject>, output: true, apply: true}`) |
| `normals-frames-mismatch` | warning | scene/prefab file | the normal map's recipe cells differ from the Sprite's frames (resliced after generating) | regenerate with frames auto from the Sprite |
| `normals-filter-mismatch` | warning | scene/prefab file | colour and normal texture of a submesh have different min/mag filters (Nearest colour + Linear normal smears the light) | set the normal texture's filter to the colour texture's (`apply: true` does it) |
| `normals-directx` | warning | scene/prefab file | a normal map in use whose recipe says `directx: true` | regenerate without directx: the engine reads OpenGL (green up) |
| `normals-green-convention` | warning | scene/prefab file | a normal map in use without a recipe that looks DirectX: at least 70% of the top-rim pixels (opaque, transparent above) have green < 128 and at least 70% of the bottom-rim ones green > 128, at least 32 of each | invert its green channel, or generate it again |
| `normals-not-a-normal-map` | warning | scene/prefab file | a normal map in use (without a recipe) whose opaque texels are not unit vectors (mean \|len - 1\| > 0.1) or have blue < 128 on more than 5% of them (a colour image in the slot) | assign a normal map (`generate_sprite_normals`) or clear the slot |
| `normals-no-light-height` | warning | scene file | the scene has a normal-mapped submesh and Light 2Ds, none with `height` > 0 | set a Light 2D's height above 0 (64 is a good start) |
| `normals-paint-unreadable` | warning | the generated PNG | its paint layer (`jasper.spritenormals.paint` chunk) can't be read (another format, a newer version, bad base64, an image that doesn't decode, sizes that disagree); the map still works, a regeneration drops the paint | regenerate with `paint_mode: clear`, or repaint |
| `normals-paint-size` | warning | the generated PNG | its paint layer's size differs from the PNG's (a regeneration refuses it: `paint_size_mismatch`) | regenerate with `paint_mode: clear`, or repaint |
| `normals-before-hires` | warning | the generated PNG | a normal map whose sprite is over 128 px (the median over its recipe's frames of the largest island's shorter side, from its alpha: a sheet of 32 px frames is pixel art) and whose recipe is version 1 (made before high-resolution support): regenerating keeps its old look (creases down rounded shapes, fans, little relief) | regenerate with settings version 2 (`params.version: 2` on the `regenerate` arguments, `--settings-version 2`, or the window's Use the current generator) |

The cache is `.jasperengine/normals-check-cache.json` (written when `.jasperengine/` exists): per PNG its mtime,
size, recipe, pixel size and (for a map in use without a recipe) the pixel analysis; per source its SHA-1; and a
reverse index source -> generated PNGs, rebuilt from the cached recipes on whole-project runs (the editor's file sync
reads it to name the maps of an edited source). A second run on an unchanged project decodes and hashes nothing.
With `--files`, a PNG with a recipe is checked as a generated map, any other image as a source (the maps made from
it); the scene findings need a whole-project run. The CLI that makes the maps is `jasperengine-editor
generate-sprite-normals` (`--help`).

## Examples

```sh
# everything but C++ (fast: about 0.2-0.5 s for the TappyPlane fixture)
jasperengine-editor check --project .

# after editing files
jasperengine-editor check --project . --files scripts/Enemy.lua shaders/Water.frag assets/ui/menu.html

# only the HTML/CSS UI
jasperengine-editor check --project . --ui

# animation clips (after editing an .anim with your own tools: --files it)
jasperengine-editor check --project . --anims
jasperengine-editor check --project . --files assets/anim/bob.anim

# Vertex Animations: stale bakes, broken files, device limits (after re-exporting a model: --files it)
jasperengine-editor check --project . --vat
jasperengine-editor check --project . --files assets/models/hero.gltf

# synthesized textures whose sources changed (after repainting an example: --files it)
jasperengine-editor check --project . --textures
jasperengine-editor check --project . --files assets/textures/rock_albedo.png

# sprite normal maps: stale maps, wrong sizes, filters, DirectX maps, lights without height
jasperengine-editor check --project . --normals
jasperengine-editor check --project . --files assets/sprites/hero.png

# CI: all checks, machine-readable
jasperengine-editor check --project . --all --format json > check.json
```

## Agent hooks

`--hook` is meant to run after **every** file-changing tool call of an agent, shell commands
included: with bypassed permissions an agent edits as often through its shell (`sed -i`,
`python3 - <<EOF`, `cat > file`) as through its Edit/Write tools, and a shell call names no file.
So each `--hook` run checks:

- the files the hook input names: Claude Code's `tool_input.file_path` / `notebook_path`, the
  `*** Add File:` / `*** Update File:` / `*** Move to:` lines of a Codex `apply_patch`;
- and every Lua, shader, C++, `.html`, `.htm`, `.css`, `.anim`, `.vat` and `.vat.glb` file of the project that changed since the
  previous `--hook` run, whatever the tool was. The stamps (mtime, size) of those files are kept in
  `.jasperengine/check-hook-state.json`; a change is checked once, by the first run after it. The
  first run (no state yet) checks the files changed in the last 10 minutes, not the project's
  older problems.

**UI warnings are shown once.** A UI error (a missing file, JavaScript, a remote URL) is printed by
every run that checks its file, but a UI warning (a dropped property, say) only by the run after
the edit that introduced it: the state file remembers the warnings already shown per file
(`"version": 2`, `"ui_reported": {"assets/ui/menu.css": ["HTMLUI-CSS-0002|'mix-blend-mode' is not
supported; dropped", ...]}`, keyed by code and message, not line, so moving code does not show it
again). A fixed warning drops out, so bringing it back shows it again. After shown UI warnings
comes one last line, `(UI warnings are shown once, after the edit that introduced them;
jasperengine-editor check --ui lists them all)`. A version-1 state file (before UI files were
tracked) is read as before.

When nothing changed, the run only lists the project's files (about 10 ms for the fixture, plus
the process start, about 0.3 s on the test host) and exits 0 without starting the engine. Each run appends one line to `.jasperengine/check-hook.log`
(rotated to `check-hook.log.1` past 512 KB), so you can see that the hook ran and what it
checked:

```
2026-10-06T09:34:12Z tool=Bash files=scripts/GameController.lua 1 error, 0 warnings exit 2 0.04s
2026-10-06T09:46:46Z tool=Read files=- nothing changed exit 0 0.01s
```

Exit codes: **2** when there are errors or new UI warnings, with the problems on stderr (the agent
sees them), **0** otherwise (also when nothing it checks changed, and when the only UI warnings were
shown before), **1** when it can't run (bad stdin, no project), which the agent's harness shows the
user only. The log line counts the shown UI warnings as warnings. Add `--cpp` to also build the C++ scripts when a
C++ file changed (seconds to tens of seconds). The edit itself is never undone.

### Claude Code

`.claude/settings.json` of the project:

```json
{
  "hooks": {
    "PostToolUse": [
      {
        "matcher": "Edit|Write|MultiEdit|NotebookEdit|Bash",
        "hooks": [
          { "type": "command", "command": "jasperengine-editor check --project \"$CLAUDE_PROJECT_DIR\" --hook", "timeout": 120 }
        ]
      }
    ]
  }
}
```

Claude Code shows the model the hook's stderr only when it exits with **2**. Read-only tools
(Read, Grep, Glob) and MCP calls need no hook: the editor's own pick-up checks files before every
MCP call. `"matcher": ""` (every tool) works too, at the cost of one quick run per call.

Tested with Claude Code 2.1.285 (`claude -p --permission-mode bypassPermissions`): told to run
`sed -i 's/local BEST_SCORE_KEY = /local BEST_SCORE_KEY = = /' scripts/GameController.lua` with
its Bash tool, Claude received:

```
PostToolUse:Bash hook blocking error from command: "jasperengine-editor check --project "$CLAUDE_PROJECT_DIR" --hook": [...]: scripts/GameController.lua:35: error: unexpected symbol near '='
```

### Codex

Codex CLI (0.159) has the same hooks (`codex features list`: `hooks stable`), in
`<repo>/.codex/hooks.json` or `~/.codex/hooks.json` (or `[[hooks.PostToolUse]]` in `config.toml`):

```json
{
  "hooks": {
    "PostToolUse": [
      {
        "matcher": "Bash|apply_patch",
        "hooks": [
          { "type": "command", "command": "jasperengine-editor check --project . --hook", "timeout": 120 }
        ]
      }
    ]
  }
}
```

Codex names its shell tool `Bash` in the hook input and its
patch tool `apply_patch` (the paths are in the patch text, which `--hook` reads). Tested with
Codex CLI 0.159 (`codex exec`): after `sed -i ...` or `python3 -c "..."` through the shell, and
after an `apply_patch`, that broke `scripts/GameController.lua`, the model received
`scripts/GameController.lua:35: error: unexpected symbol near '='`.

Exit 2 shows stderr to the model, like Claude Code. Codex runs a hook only once it is trusted
(review it with `/hooks` in the interactive CLI; project hooks also need a trusted project;
`codex exec --dangerously-bypass-hook-trust` skips the review for one run) and skips untrusted
hooks silently. `CLAUDE_PROJECT_DIR` is not set: the hook runs in the session's directory, hence
`--project .`.

### Without `--hook`

On a system with `jq`, checking only the file an Edit/Write names:

```sh
f=$(jq -r '.tool_input.file_path // empty'); [ -z "$f" ] || jasperengine-editor check --project "$CLAUDE_PROJECT_DIR" --files "$f" --quiet 1>&2 || exit 2
```

This misses shell edits; `--hook` doesn't.

## CI

```yaml
- name: Check scripts, shaders, C++ and UI
  run: jasperengine-editor check --project . --all
```

The CLI needs no display (it doesn't open a window) and `--cpp` needs CMake and a C++ compiler
like Play. It reads the project; `--shaders`/`--cpp` load it as the editor does (which may refresh
`.jasperengine/assetdb.yaml`), and `--cpp` writes the generated sources and the build under
`.jasperengine/` and regenerates `CMakeLists.txt`, as Play does. Don't run `--cpp` while the editor
builds the same project.
