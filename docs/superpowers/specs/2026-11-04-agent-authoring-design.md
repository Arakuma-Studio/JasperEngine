# Agent authoring: agents build games the way the editor intends, with tools that refuse bad input and never lie about why (design)

Date 2026-11-04. Branch `dc/agent-authoring`. Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md`
(feature 18). Sources: dossier `dc-work/dossiers/agent-authoring.json` (9 Doriax commits), Doriax read-only clone at
`3fb49fbba`. Every Jasper `path:line` below was re-read on `main` (`b4f7816ea`). Labels: **verified** (I ran it, and
say how), **inferred** (from reading code), **assumed**. No build or editor was run for this spec.

## 1. Goals

1. Agents follow the editor's authoring model: a scene opened in the editor shows every GameObject it uses; scripts
   drive what is there; what repeats is a Prefab; HUDs are UI child scenes; authored motion is an Animation clip or
   tween, not per-frame script code; a button click is `onClick`; a project keeps its script language and falls back to
   Lua when C++ cannot build. The rules live in a new `docs/mcp/guide/authoring.md` and a digest in the agent guide.
2. Clients that cut the server instructions (Claude Code keeps the first 4,096 characters) still get the essentials
   and a way to read the rest: the guide's first 4,096 characters hold the intro, Addressing, The loop and Building
   scenes plus a pointer, and `read_file` reads `jasper://` resources so any client (with or without MCP resources)
   can read the whole guide and every doc. A budget gate keeps it so.
3. Terrain details (Unity's Paint Details; `foliageLayers`) get a validated, one-undo-step tool,
   `edit_terrain_details`; `set_terrain_maps` reaches every blend map slot and every Terrain Layer factor; a new
   `jasper://docs/terrain` documents terrain authoring for agents.
4. `set_scene_camera` never produces a degenerate view: straight up or down gets the Scene view's axis up, a target
   equal to the position is refused, an active camera preview is left. The engine's camera does the same for game
   cameras aimed straight up or down (same class of bug).
5. Rotations given to MCP tools are unit quaternions: near-unit input is normalized, anything else (Euler degrees
   typed as a quaternion, partial objects) is refused with the Euler form to use.
6. Export shader overrides (Build Settings > Shaders: added and excluded shader keys) are editable without starting a
   build, one key at a time, undoable, from MCP and from the Build Settings window, which no longer overwrites a change
   made while it is open.
7. `get_build_info` answers "can Play build C++ scripts here" with the exact check Play runs (on Windows today it runs
   a different, looser check).
8. A tool call that arrives while a person's scene save runs waits for the save and then runs; nothing tells an agent
   to "stop Play mode" when nothing plays, and no edit is refused as "saving before Play".
9. The Terrain window stops overwriting the project's saved Terrain settings with its constructor defaults, and a
   project switch no longer carries the previous project's settings over.

## 2. Non-goals

- An automated lint of authoring style (flagging per-frame tweens or HUD widgets in 3D scenes): heuristics on user
  scripts would misfire; the rules are guidance, verified by review and by the prompts.
- A separate guidance tool (Doriax `get_editor_guidance`): `read_file jasper://guide` does it with no new core tool.
- Normalizing quaternions written by Lua or C++ at runtime (`go.rotation = Quaternion(...)`): that is user code on the
  hot path; the Lua docs already say `Quaternion(w, x, y, z)`. Scene files are not rewritten on load.
- Lua or C++ runtime editing of terrain detail layers (Unity's `TerrainData.detailPrototypes` at runtime): detail
  layers are authored in the editor; nothing in the runtime changes.
- Doriax's "refuse while the Scene view shows the game camera" (`setEditorCamera`): Jasper's Scene view draws the
  editor camera during Play (`SceneRender.cpp:721`, `previewing = !isPlaying && ...`; inferred), so there is nothing
  to refuse.
- Doriax `getPairedShaderKeys` (Dlo twin variants): Jasper has no such variant (no `Dlo` in `editor/Exporter.cpp` or
  `engine/core`; inferred).
- Doriax `8ea63795b` (warn on stale shader keys from a save before the first draw): independently solved; Jasper
  derives saved keys from content without drawing (`editor/Project.cpp:3958-3970`, tested by
  `editor/tests/project/SceneShaderKeysTest.cpp:68-93`). Rejected.
- Doriax's "prefer C++ for new projects": in Jasper C++ forces a compiled build instead of the prebuilt player
  (`docs/mcp/guide/shipping.md:12-14`), so Lua stays the default; only "keep the project's language" and "fall back
  to Lua" are taken.
- Holding edits during Play's own startup: already refused with an exact message and a job id
  (`McpServer.cpp:1210-1240`); unchanged.

## 3. Users and stories

- Agent (Claude Code, stdio): connects, receives the guide cut at 4,096 characters with "… [truncated]". The kept part
  ends after "Building scenes" and says to read the rest with `read_file jasper://guide`; the agent does, then builds a
  platformer level by placing platforms, coins (a Coin Prefab placed 20 times with `instantiate_prefab`), a HUD UI
  scene attached with `set_child_scene`, and a spinning-coin tween made with `edit_animation`, instead of a 400-line
  `main.lua` that creates everything.
- Agent (no MCP resources support, e.g. a CLI client): `read_file jasper://docs/authoring` and
  `jasperengine-editor call read_file --path jasper://docs/terrain` read the references.
- Agent: "add grass to the terrain": `tool_groups {enable: ["terrain"]}`, `write_file`/`import_files` a grass model,
  `edit_terrain_details {add: [{mesh: "assets/models/grass.glb", density: 4, density_map: {fill: 1}, max_slope: 30,
  cast_shadows: false}]}` → one undo step, `sample_terrain` shows `details[0].drawn > 0`. A density of 500 is refused
  with the range and why.
- Agent: "a second texture set for snow": `set_terrain_maps {blendmap: {color: [1,0,0], index: 1}, layers: [{index: 3,
  color: "...", uv_scale: [4,4], roughness_factor: 0.6}]}`.
- Agent: top-down screenshot of a level: `set_scene_camera {position: [0,40,0], target: [0,0,0]}` gives a correct view
  (up = +Z, as the Top axis view), not a collapsed one; `position == target` is refused saying why.
- Agent: sets a rotation `[90, 0, 0, 1]`: refused, "this looks like Euler degrees: use {"euler": [90, 0, 0]}".
- Agent: excludes one unused shader variant: `set_project_settings {values: {"build.shader_exclusions": [...]}}` (one
  undo step, no build started); a person sees it in Build Settings > Shaders at once.
- Agent on Windows: `get_build_info` → `cpp_scripts.can_build: false` with the editor-compatible toolchain missing; it
  writes Lua and tells the user why (rule in `cpp-scripting.md`).
- Agent: calls `create_entity` while the person pressed Ctrl+S on a large scene: the call waits ~2 s and succeeds;
  its result notes that it waited for the person's save. Before: "Stop Play mode first" with nothing playing.
- Human: opens Window > Terrain from the menu; the brush size, map resolutions and placement settings saved last week
  are there (before: defaults, and the saved values were overwritten on the next workspace save).
- Human: Build Settings open, an agent excludes a shader; the list updates; closing the window keeps it; Ctrl+Z
  (project scope) brings it back.
- Human (C++): the event picker describes Button `onRelease` as "released anywhere" and points to UI `onClick` for clicks.

## 4. Findings

Each Doriax commit of the dossier, with what Jasper has today.

### 4.1 Authoring rules and the 4,096-character cut (Doriax `a48d1ce3a`)

- The guide (`editor/mcp/McpInstructions.cpp:24-240`) has the HUD rule (`:151-153`) and the physics timing rule
  (`:173`), but no "assemble in the editor", no "build once as a Prefab", no "don't tween in scripts".
  `docs/mcp/guide/animation.md:11-13` presents entity Animations and tweens without saying scripts should not do it per
  frame. No such rule in `lua-scripting.md`, `cpp-scripting.md`, `prefabs.md`, `ui.md` (grep; inferred).
- The MCP prompt `new_2d_game` (`editor/mcp/tools/DocsTools.cpp:619-650`) tells agents to create "UI text for the
  score" with the scene tools, in the gameplay scene: against the HUD rule of the guide.
- **Claude Code cuts server instructions at 4,096 characters** (verified by reading the installed client: Claude Code
  2.1.318, `/root/.local/share/claude/versions/2.1.318`, minified bundle: `function cr(e,r,n=pB()){... return
  yo(e,"Server instructions",r,n)}`, `function yo(e,r,n,s=pB()){if(e.length<=s)return e; ... return
  Z(e,s)+"… [truncated]"}`, `function pB(e=!1){return i.CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH??(e?PDi:ESr)}`,
  `ESr=4096`, `PDi=16384`; the per-server limit `_j(r)` is unlimited only for http/sse servers on URLs its `E1` accepts,
  else `pB()`). Not run against a live session. The guide is 14,774 bytes (computed from the raw literals plus the
  generated Tool groups section, with Python, not by `tool_budget.py`), so a Claude Code agent sees the intro, Unity
  names, Addressing and The loop, and loses Code, Lua, C++, Physics, Prefabs, Pitfalls and the rest.
- The full guide is a resource (`jasper://guide`, `CoreTools.cpp:874`, `DocsTools.cpp:412`); Claude Code lists
  resources through its own tools, other clients may not. `read_file` (`AssetTools.cpp:3832`, handler `:1160`) reads
  only project files. No tool reads `jasper://` URIs (`DocsTools.cpp` registers `search_engine_api`,
  `search_engine_source`, `read_engine_source`; inferred).
- Byte room: 14,774 of the 14,884 bytes allowed (16,384 minus the 1,500 headroom, `tools/mcp/budget.json` "guide"):
  110 bytes free. New rules must be paid for by moving detail of the sections they touch into docs.

### 4.2 Terrain tools and the editor camera (Doriax `b2375ea0c`)

- No detail (foliage) layer tool: `WorldTools.cpp:520-523` only reads mesh and density. `RenderReflect.cpp:350` lists
  `foliageLayers` as `ListWhole | ListTemplate`, so `set_properties` can replace the whole list with scene-file keys,
  unvalidated (inferred, no test). `StreamComponents.cpp:554` decodes a layer with no range or asset-root check.
  Ranges the Terrain window uses (`TerrainEditWindow.cpp:3420-3470`): density 0-20 drag, scale 0.01-20, slope 0-90,
  height 0-1, rotation 0-1, alignment 0-1, draw distance 1-500. The engine caps a chunk at 8,192 instances per 8 m cell
  (`MeshSystem.cpp:3764-3793`), i.e. 128 instances per square unit. Meshes are `.obj` or glTF
  (`MeshSystem.cpp:3932-3939`). A layer without a density map draws nothing (`MeshSystem.cpp:3973-3976`).
- `set_terrain_maps` always writes `blendMaps[0]` (`WorldTools.cpp:757-758`); its `layers` items have no `uv_scale`,
  `uv_offset`, tint, `normal_strength`, `roughness_factor`, `metallic_factor`, `occlusion_strength`
  (`WorldTools.cpp:5480-5495`), though `set_properties surfaceLayers[i].<field>` reaches them
  (`tools/mcp/tests/test_stochastic_tiling.py:129`). `MAX_TERRAIN_BLENDMAPS` is 3 (`TerrainComponent.h:23`).
- No `docs/mcp/guide/terrain.md`; terrain is documented for agents only in tool descriptions.
- `set_scene_camera` resets up only when `|dir.y| < 0.99` (`ViewTools.cpp:1111-1114`), so aiming straight down from
  the default up keeps `(0,1,0)`, and `Matrix4::lookAtMatrix` (`engine/core/math/Matrix4.cpp:550-557`) gets a zero side
  vector: a degenerate view (inferred, not run). `test_place_with_physics.py:507` sidesteps it with `z = -9.999`. No
  refusal of `target == position`. An active camera preview (`SceneRender.cpp:2934-2965`, Inspector "Preview") keeps
  showing the previewed camera, so the move is invisible.
- Same class in the runtime: `RenderSystem.cpp:12528-12540` builds every camera's view with `lookAtMatrix(position,
  worldTarget, worldUp)`; a game camera targeting straight below with the default up degenerates the same way
  (inferred).

### 4.3 Rotations (Doriax `720e314e7`)

- `McpValues::parseQuaternion` (`McpValues.cpp:998-1028`) stores four numbers as given (`fromQuat`, `:1005-1010`);
  `Quaternion.h:34` keeps them; `getRotationMatrix` then builds a non-rotation (inferred). Readback hides it, because
  `eulerDegrees` normalizes a copy (`McpValues.cpp:986-987`). `[90,0,0,1]` and `{x:90,y:0,z:0}` (partial object, `w`
  kept from the old value through `readComponents`, `McpValues.cpp:107-127`) are accepted.
- Callers: `McpValues.cpp:3068` (every Quaternion property of `set_properties`), `ComponentTools.cpp:631`,
  `PlacementTools.cpp:215`, `WorldTools.cpp:1279` (collider shape), `:3217` (clip keys), `:3775`, `:3784` (tweens),
  `LatticeTools.cpp:792` (Euler only).

### 4.4 Terrain window settings (Doriax `d3cc5503e`)

- `TerrainEditWindow::show()` writes every member into `Project::TerrainEditorSettings` each frame the window is open
  (`TerrainEditWindow.cpp:3589-3617`); the only read is in `openForEntity` (`:3646-3684`), called only by the
  Inspector (`InspectorWindow.cpp:9810`). Window > Terrain (`App.cpp:778-781`, `:941-942`) and the saved layout
  (`App.cpp:5843`) call `setOpen` (`:3628-3644`), which loads nothing, so constructor defaults overwrite the saved
  values, and `Workspace.cpp:340` persists them. `App.cpp:4885-4906` (project switch) resets other windows but not
  this one. `WorldTools.cpp:703,739` read those settings as default map resolutions, so the agent tools change
  behaviour too (all inferred). `toolCategory` must be restored with the mode (`:3654`).

### 4.5 Export shader overrides (Doriax `b2323b44b`)

- Overrides change only as an argument of `build_project` (`BuildTools.cpp:1099-1141`), which then always starts an
  export (`:1146-1176`); `add`/`exclude` move a key between lists, `reset` empties both: one override cannot simply be
  dropped. `get_build_info include_shaders` reads them (`:1362-1380`). Not undoable.
- The Build Settings window keeps a working copy (`ExportWindow.cpp:228-230`) and writes it back
  (`:344-347`), so a change made over MCP while the window is open is overwritten when the window saves (inferred).

### 4.6 Build tools check (Doriax `3fc70298e`)

- `get_build_info` calls `Generator::checkBuildTools()` with defaults (`BuildTools.cpp:1273`), which on Windows skips
  the editor-compatibility kit check (`Generator.cpp:3198-3235`); Play (`Project.cpp:9617-9624`), `compile_scripts`
  (`ProjectChecks.cpp:446-452`) and editor scripts (`EditorScripts.cpp:747`) pass `useDefaultKit` and a kit. So on
  Windows `desktop.available` can be true while Play refuses C++ (inferred; Windows not built here). No guidance on
  keeping a project's language or falling back to Lua (`McpInstructions.cpp:127-133`, `cpp-scripting.md`).

### 4.7 Saves and play mode (Doriax `544c980e8`, `d776665bf`)

- `McpServer::execute` refuses any tool not `allowedWhilePlaying` with "Stop Play mode first" when
  `isAnyScenePlaying()` (`McpServer.cpp:1272-1276`), which counts `SAVING` (`Project.cpp:9241-9252`).
  `saveSceneToPathAsync` sets `SAVING` (`Project.cpp:7525`) for a person's save: Ctrl+S, Save All (`App.cpp:259-274`),
  Hierarchy > Save (`HierarchyWindow.cpp:819`), Save As (`App.cpp:5724`), the unsaved-changes dialog
  (`Project.cpp:1473`). I found no Play path that saves through it (inferred), so "saving before Play"
  (`Project.cpp:365`, `CommandHistory::beforeMutation`) is wrong for every save it reports.
- Tools that are `allowedWhilePlaying` and edit get the `beforeMutation` refusal; tools that edit outside the command
  history (`edit_ui_view`, `update_script_entry`, file writers) are not stopped at all and run beside the save worker,
  which reads component arrays (`Project.cpp:7526-7529`; race inferred, not audited further: the hold below removes it).
- `save_project` says "Stop Play mode before saving" from `ctx.isPlaying()` (`ProjectTools.cpp:599-602`).
- MCP saves are synchronous (`saveSceneNow`, `Project.cpp:5125`); refusals are reported (`McpValues.cpp:4205-4222`);
  attaching an attached child scene is tolerated (`SceneTools.cpp:691,700`). Present; nothing to take.

### 4.8 Button clicks (Doriax `1448b85a6`)

- `onRelease` fires on every release wherever the pointer is (`UISystemPointer.cpp:340-357`); `onClick` only on a
  release over an interactable widget (`:755-765`). `lua-api.md:274-281` lists both with no warning;
  `cpp-scripting.md:151-152` shows `REGISTER_BUTTON_EVENT(onPress, ...)` only; the event picker describes
  `onRelease` as "The button is released" (`editor/util/ScriptEvents.cpp:102-103`).

## 5. Product decisions

1. **The guide keeps a 4,096-character core, gated.** Order: intro (with the pointer), Addressing, The loop, Building
   scenes, then Unity names and the rest as today. Reason: those four are what an agent needs before its first edit;
   Unity names and the area digests are reference the pointer leads to. The pointer names the guide's last section so
   it works for any client that cuts, with or without a marker. The gate (`tool_budget.py`) measures in UTF-16 code
   units, as JavaScript clients count. This adds a rule to `docs/mcp/conventions.md`, a page S31 owns: the lead
   approves it with this spec (recorded here as the one deviation from the page's change process).
2. **`read_file` reads `jasper://` resources** (the guide, `jasper://docs/...`, `jasper://lua-api...`,
   `jasper://project/...`), with its `start_line`/`max_lines`. Reason: extending a core tool beats a new one
   (conventions), and it serves clients without resources and the CLI the same way.
3. **Authoring rules, Unity's model.** Assemble in the editor; reference through script properties; spawn only the
   unknowable, as Prefab instances; build once as a Prefab; HUDs and menus are UI child scenes; authored motion is an
   Animation clip or tween; per-frame script motion only for what follows input, physics or game state; a button click
   is `onClick`. Reason: this is how Unity projects are built, it keeps scenes inspectable by the human and by the
   agent's own screenshots, and it is what the editor's tools, Prefab overrides and Animation window are for.
4. **Script language: keep the project's; default Lua; fall back to Lua.** Use the language of the project's existing
   scripts. A project without scripts gets Lua unless the user asks for C++ (prebuilt players, no compiler). For C++,
   `get_build_info.cpp_scripts.can_build` first; switch to Lua (and tell the user why) when tools are missing, a build
   fails outside the agent's code, or it still fails after two fixes of its own code.
5. **Terrain details are "details" (Unity) in tools and docs, `foliageLayers` in data.** New tool
   `edit_terrain_details` (terrain group): `add`, `update` (by index), `remove` (indices), no edits = list. Order:
   remove (old indices), update, add, like `edit_colliders`. One scene undo step. Reason: the Terrain window already says
   "Detail layer"; Unity users say Details.
6. **Detail ranges come from the engine, refusals name them and why.** density 0-128 per square unit (the chunk cap);
   scales 0.01-20 with min ≤ max; slopes 0-90 with min ≤ max; heights 0-1 with min ≤ max; rotation_jitter and
   align_to_normal 0-1; draw_distance 1-500; seed 0-4294967295; mesh: an `.obj`, `.gltf` or `.glb` under the assets
   folder. The same check guards `set_properties` on `foliageLayers` and clamps decoded scene values (with a warning).
7. **`set_terrain_maps` reaches every slot and factor**: `blendmap.index` (0-2, no gaps: a slot needs the one before),
   `blendmap.path` for an image into a slot, and per layer `uv_scale`, `uv_offset`, `tint`, `normal_strength` (0-2),
   `roughness_factor`, `metallic_factor`, `occlusion_strength` (0-1). Names follow the Terrain Layer rows of the
   Inspector (`InspectorWindow.cpp:9849-9861`).
8. **Camera up rule: the axis-view convention.** Looking straight down (|dir·Y| ≥ 0.999) the up is `(0,0,1)`, straight
   up `(0,0,-1)`, as `sceneview::axisViewUp` gives the Top and Bottom views (`SceneViewMath.cpp:41-46`); otherwise
   `(0,1,0)`. The engine owns the rule (`Camera::upFor(direction, preferred)`); the editor's axis views use it.
   Reason: one convention, and a game camera aimed down frames like the Scene view's Top view.
9. **Rotations: within 10 % of unit length is a quaternion.** `| |q| - 1 | ≤ 0.1` → normalized; otherwise refused
   ("looks like Euler degrees" when any component's magnitude is over 2, "zero" for |q| < 1e-6, else "not a unit
   quaternion (|q| = 1.27)"), with the `{"euler": [...]}` form. An object form must give all of `x`, `y`, `z`, `w`.
   Reason: agents type real quaternions with 2-4 decimals (norm within 1 %), or Euler degrees (norm far above 1); the
   gap is wide. Doriax uses 0.2; 0.1 also refuses `[0, 0.5, 0, 1]` (|q| = 1.118), which is a typo, not a rotation.
10. **Shader overrides are two project settings, undoable**: `build.shader_additions` and `build.shader_exclusions`
    (lists of key strings as `get_build_info include_shaders` prints them, or specs such as `"uieffect/<name>"`), on
    `get_project_settings`/`set_project_settings`, one project-scope undo step each (`ShaderOverridesCmd`), saved at
    once. `build_project shaders` stays and goes through the same command. The Build Settings window edits apply at
    once through the command and the window shows the project's lists every frame (no working copy for shaders).
    Reason: `set_project_settings` is the one place agents change project settings; Unity's Build Settings apply at once.
11. **`get_build_info.cpp_scripts`** reports Play's check (`can_build`, `missing_tools`, the resolved `kit`), from one
    shared function Play, `compile_scripts`, editor scripts and `get_build_info` call. No new tool (Doriax
    `check_cpp_build_tools`).
12. **A person's save holds MCP calls, it does not refuse them.** While a scene save the editor started runs (no Play
    session), every call that may change something, or is not allowed while playing, waits up to 60 s for it, then runs;
    the result carries `waited_for_save_ms`. Read-only calls run at once. The refusal text of the command history says
    "a scene is being saved". Reason: the save is seconds long and the agent's call is valid; a refusal teaches nothing.
13. **The Terrain window loads the project's settings once per project** (lazily, on its first frame with a project)
    and only writes them back after that; a project switch resets it.

## 6. Design

### 6.1 Agent guide (`editor/mcp/McpInstructions.cpp`)

Measured draft (Python over the raw literals plus the generated Tool groups section, the texts below applied): 14,850
bytes, 34 under the 14,884 line, Building scenes ending at character 3,423. Changes:

- Intro: the sentence "Build scenes with tools, write code with your own file tools, verify by playing, screenshots
  and the Console." (said again by The loop) is replaced by:
  `Some clients cut this text (it ends with "## Tool groups"): then read_file jasper://guide first.`
- Sections reordered: intro, `## Addressing`, `## The loop`, `## Building scenes` (new), `## Unity names -> engine
  names`, then the rest unchanged.
- The loop, step 2:
  ```
  2. Edit scenes, prefabs and settings with tools (Building scenes), never by hand-editing
     .scene/.prefab/project.yaml; many edits: one `batch` (Undo). Code: your own tools (Code).
  ```
- New section:
  ```
  ## Building scenes
  Place every GameObject a scene uses in the editor, with real values; scripts drive them via
  GameObject script properties and spawn only what can't be placed ahead (bullets, waves) as
  Prefab instances. Make what repeats a Prefab once. HUDs and menus: a UI scene attached as a
  child scene (set_child_scene). Authored motion is a clip or tween
  (edit_animation) a script starts, not per-frame code. jasper://docs/authoring.
  ```
- `## 2D and 3D`: the HUD sentence moves into Building scenes; the bullet keeps "In a 2D scene layout groups stack
  bottom-up and pointer input needs a UI camera."; the Clips bullet becomes
  `- Clips (.anim, shared), retargeting, property tracks: edit_animation (jasper://docs/animation).` (the fields it
  listed are in `animation.md`: "Humanoid clips and retargeting", "Property tracks", "edit_animation: clip mode").
- `## Prefabs`: the runtime signature sentence moves out (it is in `prefabs.md` "Runtime: Instantiate and Destroy");
  "Prefer a Prefab script property" becomes "Spawn through a Prefab script property".
- `## C++ scripts`: unchanged (the fallback rule is in `cpp-scripting.md`, which the section already sends agents to).

If another feature lands guide text first, this one still fits by the same method: move more of the detail of the
sections it touches (2D and 3D animation bullets, Prefabs) into their docs; never other areas' digests.

### 6.2 Guide gate (`tools/mcp/tool_budget.py`, `tools/mcp/budget.json`, `docs/mcp/conventions.md`)

- `budget.json` `guide` gains:
  ```json
  "client_cut": {"chars": 4096, "sections": ["(intro)", "Addressing", "The loop", "Building scenes"],
                 "pointer": "read_file jasper://guide",
                 "reason": "Claude Code keeps the first 4096 characters of server instructions (CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH default)"}
  ```
- `check_guide` adds: every listed section ends within the first `chars` UTF-16 code units
  (`len(text[:end].encode("utf-16-le")) // 2`); the intro contains `pointer`; missing keys are a budget-file error.
  `print_guide` prints where the cut falls ("cut at 4096: inside 'Undo'").
- `docs/mcp/conventions.md` "The agent guide": one paragraph on the cut and its gate; the guide holds what must
  come before the cut. `.claude/rules/mcp.md` gets the same line if it lists the guide rules (`check_agent_docs.py`).

### 6.3 `read_file` over resources (`editor/mcp/tools/AssetTools.cpp`, `editor/mcp/McpServer.{h,cpp}`)

- Extract the body of `resources/read` (`McpServer.cpp:857-875`: find a resource, else match a template, run it in a
  `McpWriteScope`) into `ResourceResult McpServer::readResource(const std::string& uri)`; `resources/read` and
  `readFileTool` both call it.
- `readFileTool`: a `path` starting with `jasper://` reads `readResource(path)`; text contents are concatenated, line
  windowed by `start_line`/`max_lines` exactly as files are (same `total_lines`, `truncated` fields); a blob content
  is refused ("binary resource: read it through resources/read"). An unknown URI fails with the not-found text, which
  already lists `jasper://docs`.
- Description: `"Read a project text file (as on disk), or a jasper:// resource (jasper://guide, jasper://docs/<area>);
  the result text is the content."` Schema unchanged (`path` stays `assetPath("")`, a string).
- `docs/mcp/guide/README.md` and `editor-integration.md`: one line each.

### 6.4 Authoring reference (`docs/mcp/guide/authoring.md`, served as `jasper://docs/authoring`)

Sections (each rule with its reason and the tools; Lua and C++ examples checked by the existing doc tests):

1. **Assemble scenes in the editor**: what "a scene shows everything" means; references through script properties
   (Lua `type = "GameObject"` or a wrapper name such as `"Text"`; C++ `DPROPERTY("Label") Text* label`) set with
   `set_script_properties`; objects that appear later placed inactive (`set_entity_state active: false`, then
   `go:setActive(true)`), not hidden (`visible` leaves bodies and scripts running, `lua-api.md:1674`).
2. **Spawn only the unknowable**: bullets, waves, drops, procedural content, as `GameObject.instantiate(prefab ...)` of a
   Prefab script property; never assembling components in code; standalone Prefabs for `PrefabAsset.load`.
3. **Build once as a Prefab**: `create_prefab` from the finished GameObject, `instantiate_prefab` for the others,
   overrides vs Apply, variants (`create_prefab_variant`), Prefab Mode; when two scenes need the same object.
4. **HUDs and menus**: a UI scene attached with `set_child_scene` (`start_active: false` for a pause menu, shown with
   `SceneManager.addChildScene/removeChildScene`); why (camera space, one copy for every level); shared state through a
   Lua module both scripts `require` or a C++ static; HTML UI documents for menus (`jasper://docs/html-ui`).
5. **Animate natively**: Animation clips and the Animation Player, entity Animations and tweens (`edit_animation`),
   the Animator for state machines, sprite animations, Camera Follow and camera shake (`jasper://docs/camera`), Canvas
   Group fades; scripts call `play`/`crossFade`/`stop`; per-frame script motion only for what follows input, physics
   or game state (a player controller, a follow camera without the component).
6. **Input to UI**: a button click is UI `onClick` (released over the button, interactable); `onPress`/`onRelease`
   are the press state and `onRelease` fires wherever the pointer is released. (Naming follows `ui-components`: if it
   adds a Button `onClick` that also fires on keyboard/gamepad Submit, the line names that event.)
7. **Physics timing**: pointer to `jasper://docs/physics` (the rule is in the guide already).
8. **Choosing the script language**: decision 4, with the `get_build_info` field.
9. **Check your build**: `get_scene_tree` lists every object before Play; a Scene view screenshot in edit mode
   shows the level; the Console is clean.
10. **Anti-patterns**: a table "in a script → in the editor" (spawning the level in `init`; `self.t = self.t + dt;
    go.position = ...` bobbing; HUD Text in a 3D scene; copy-pasted enemies; `onRelease` as click).

Also: `lua-api.md` (event table: a note under the Button and UI rows), `cpp-scripting.md` ("Events": a
`REGISTER_UI_EVENT(onClick, onClicked)` row for clicks; new section "Choosing Lua or C++" with decision 4),
`lua-scripting.md` (one line in its events part), `animation.md` intro (one sentence: "scripts start clips and
tweens; they don't animate per frame"), `prefabs.md` (a "When to make a Prefab" paragraph). `docs/mcp/guide/README.md`
gets rows for `authoring.md` and `terrain.md`.

**Prompt `new_2d_game`** (`DocsTools.cpp:619-650`), step 3 becomes: "Scene: place every GameObject in the editor
(sprites, a camera, bodies with colliders); make what repeats a Prefab (create_prefab, instantiate_prefab); put the
score in a UI scene attached with set_child_scene; author motion with edit_animation. Read jasper://docs/authoring."

**Generated project `AGENTS.md`** (`Generator.cpp:1682-1802`, new projects only: the file is written once): a short
"## Building scenes" section with the same five rules and `jasper://docs/authoring`. Claude Code loads it in full
through the generated `CLAUDE.md` (`@AGENTS.md`), so it reaches agents whatever the instructions cut.

**Event picker** (`editor/util/ScriptEvents.cpp:89,102-103`): `onClick` "Released over the element: a button's
click"; `onRelease` "The button is released, wherever the pointer is (a click is UI onClick)".

### 6.5 Terrain (`editor/mcp/tools/WorldTools.cpp`, new `editor/util/TerrainDetailRules.{h,cpp}`)

**Rules module** (editor, pure, unit-tested):

```cpp
namespace jasperengine::editor::terrain {
struct DetailRange { const char* key; float TerrainFoliageLayer::* field; float min, max; const char* meaning; };
const std::vector<DetailRange>& detailRanges();          // density 0-128, min/max_scale 0.01-20, slopes 0-90, ...
// first violation, "" when valid; checks pairs (min <= max) and the mesh extension
std::string checkDetailLayer(const TerrainFoliageLayer& layer);
// clamps every range, swaps inverted pairs; returns the fields it changed (for a load warning)
std::vector<std::string> clampDetailLayer(TerrainFoliageLayer& layer);
}
```

When `render-correctness` adds `minDrawDistance` (and a distance scale) to `TerrainFoliageLayer`, both go into the
table (`min_draw_distance` 0..`draw_distance`).

**`edit_terrain_details`** (group `terrain`, `allowedWhilePlaying` false, not idempotent):

```cpp
add("edit_terrain_details", "Edit Terrain Details",
    "Terrain detail layers (Unity's Details; foliageLayers): list (no edits), remove [indices], update "
    "[{index, ...}], add [{mesh, ...}] in that order, one undo step. A layer draws nothing without a density "
    "map: density_map {fill: 0-1} makes a uniform one, or paint with sculpt_terrain paint_density. Ranges and "
    "fields: jasper://docs/terrain.",
    object({
        {"scene", sceneRef()},
        {"entity", entityRef("Default: the only terrain.")},
        {"remove", array(integerItem, "")},
        {"update", array(anyObject("{index, ...fields}"), "")},
        {"add", array(anyObject("{mesh, ...fields}"), "")}
    }),
    editTerrainDetailsTool, false, false);
```

Fields (snake_case of the members): `mesh`, `density_map` (path or `{fill, resolution}`, resolution default the
Terrain setting `densityMapResolution`), `density`, `min_scale`, `max_scale`, `rotation_jitter`, `align_to_normal`,
`min_slope`, `max_slope`, `min_height`, `max_height`, `draw_distance`, `seed`, `cast_shadows`. Unknown keys fail
listing the valid ones. Handler: resolve the terrain (`resolveTerrain`, `sceneStopped`); copy `foliageLayers`; apply
remove (descending, all indices checked first), update, add (defaults of `TerrainFoliageLayer`, then the fields);
`checkDetailLayer` on every touched layer; paths through the same asset-root resolution `texturePath` uses
(`WorldTools.cpp` helper) plus `.obj/.gltf/.glb`; a `{fill}` map is written by `TerrainMaps::createDensityMap`
(new, beside `createBlendmap` in `editor/mcp/TerrainMaps.{h,cpp}`: 8-bit grey PNG in the terrain maps folder); then
one `MultiPropertyCmd` with `PropertyCmd<std::vector<TerrainFoliageLayer>>` "foliageLayers" and the foliage update
flag, through `runSceneCommand`. Result: `layers` (every field of every layer), `added` indices, `notes` (no density
map; above density 20: "N instances per 8 m cell, budget for mobile"), `terrain`, and `undo`.

**Readback**: `terrainInfo` (`WorldTools.cpp:515-534`) `foliage_layers` becomes `details`: all fields per layer plus
`drawn`, the instance count the editor currently draws for that layer, from a new
`MeshSystem::getFoliageInstanceCount(Entity terrain, size_t layer) const` (sums the layer's chunk instance counts in
`terrainFoliage`). Results are not a file format, so the key is renamed outright and `sample_terrain`'s description
says "details".

**`set_properties` guard**: where `McpValues` builds the command for a `TerrainComponent` `foliageLayers` value
(whole list or an element path), run `checkDetailLayer` on the resulting layers and fail with its text.

**Load clamp**: `StreamComponents.cpp` foliage decode (`:554`) runs `clampDetailLayer` and logs one warning per
layer naming the fields ("Terrain 'Ground' detail layer 2: density -5 clamped to 0").

**`set_terrain_maps`** (`WorldTools.cpp:732-760`, schema `:5480-5500`):
- `blendmap` object gains `path` (string) and `index` (integer 0..`MAX_TERRAIN_BLENDMAPS - 1`, default 0); a string
  value is slot 0 as today. `index > blendMaps.size()` fails ("blend map slot 2 needs slot 1 first"); the slot is set
  in place (`blendMaps[index]`), resizing by one when `index == size()`. Result `blendmap.index`.
- `layers[]` gains `uv_scale` (vec2, each 0.01-100), `uv_offset` (vec2, finite), `tint` (color, 0-1 linear RGBA),
  `normal_strength` (0-2), `roughness_factor`, `metallic_factor`, `occlusion_strength` (0-1); invalid values fail
  naming the range (the existing `StochasticParam` pattern). The description gains "blendmap index (slots 0-2 weight
  layers 0-2, 3-5, 6-8); layer factors".
- `budget.json` `exceptions.set_terrain_maps.limit`: the measured size (expected about 2,680), reason "agent-authoring:
  blend map slots and Terrain Layer factors on the same tool".

**`docs/mcp/guide/terrain.md`** (`jasper://docs/terrain`): the Terrain component (size, height, maps, layers), the
three map kinds and slots, Terrain Layers and their factors, details (fields, ranges and why, density maps, `drawn`),
`sculpt_terrain` brushes, `sample_terrain`, the Terrain window and its saved settings, budgets (instances per cell,
`cast_shadows: false` for grass), recipes (a meadow, snow on peaks with a height mask), common mistakes. The terrain
group line in `McpToolGroups.cpp:25` gains "details". `docs/unity-glossary.md`: Terrain Details / Detail mesh =
`foliageLayers` (Terrain window "Detail layer", `edit_terrain_details`); Terrain Layer = `surfaceLayers`.

### 6.6 Camera (`engine/core/object/Camera.{h,cpp}`, `engine/core/subsystem/RenderSystem.cpp`, `editor/...`)

- Engine: `static Vector3 Camera::upFor(const Vector3& viewDirection, const Vector3& preferredUp)`: returns
  `preferredUp` unless `|normalize(dir) × normalize(up)| < 0.0175` (about one degree), then `(0,0,1)` for a direction
  with negative Y and `(0,0,-1)` for positive Y (axis-view convention, decision 8).
- `RenderSystem.cpp:12528-12540` (camera view update): `worldUp = Camera::upFor(worldTarget - worldPosition,
  worldUp)` before `lookAtMatrix`; `camera.right` uses the same up. Cost: one cross product and length per camera view
  update. The stored `camera.up` is not changed (the user's value stays).
- `sceneview::axisViewUp` (`SceneViewMath.cpp:41-46`) keeps its values; a unit test pins that it equals
  `Camera::upFor` for the Top and Bottom directions.
- `setSceneCameraTool` (`ViewTools.cpp:960-1205`): after resolving position and target (from the args and the current
  camera), `|target - position| < 1e-4` fails "target must differ from position (it is the point the camera looks at)"
  before anything changes; the up line (`:1111-1114`) becomes `camera->setUp(Camera::upFor(target - position, (0,1,0)))`;
  if `sceneRender->isPreviewCameraActive()` and the call moves the camera (position/target/orbit/distance/view/
  frame_all), `clearPreviewCamera()` and `changed` gets `"left_preview"`, the text says "Left the camera preview".
  `sceneCameraJson` (`ViewTools.cpp:881-900`) adds `"up"`. Description unchanged (budget).
- `test_place_with_physics.py:507`: `z = -10` (the workaround goes).

### 6.7 Rotations (`editor/mcp/McpValues.{h,cpp}`)

```cpp
// Unit quaternion from [x,y,z,w], {x,y,z,w} (all four), {"quat": [...]}, {"euler": [x,y,z]} (degrees) or both
// (the one that moved wins). Near-unit input is normalized; error says why it was refused.
static bool parseQuaternion(const Json& value, Quaternion& out, std::string* error = nullptr);
```

- `fromQuat`: an object needs all four keys; after reading, `n = norm`; `n < 1e-6` → "a zero quaternion is not a
  rotation"; `|n - 1| > 0.1` → if any |component| > 2: "[90,0,0,1] looks like Euler degrees: use {"euler": [90, 0,
  0]}" (the first three numbers), else "not a unit quaternion (|q| = 1.27): give [x,y,z,w] of length 1, or
  {"euler": [x,y,z]}"; else `normalize()`. Euler input is already unit.
- Every caller passes `&error` and shows it (`McpValues.cpp:3068` `expect(...)` gets the reason; `ComponentTools.cpp:631`,
  `PlacementTools.cpp:215`, `WorldTools.cpp:1279,3217,3775,3784`). `WorldTools.cpp:3217` clip keys: the reason plus the
  key index.
- `docs/mcp/guide` addressing text (the guide's values line) unchanged; `editor-integration.md` or the area docs that
  describe rotations get "near-unit quaternions are normalized; others are refused".

### 6.8 Shader overrides (`editor/command/type/ShaderOverridesCmd.{h,cpp}`, `editor/settings/SettingsRows.cpp`, `BuildTools.cpp`, `ExportWindow.cpp`)

- `ShaderOverridesCmd(Project*, ShaderOverrides before, ShaderOverrides after)`: `execute`/`undo` set
  `project->getShaderOverrides()`, `saveProjectFile()` (as `DebugSettingsCmd.cpp:12-19`), `touchedPaths` =
  project.yaml; name "Change Shader Overrides"; `setNoMerge`.
- Settings rows `build.shader_additions`, `build.shader_exclusions` (type `list`, description "Shader keys the build
  adds / leaves out besides the scenes' own (get_build_info include_shaders). Undoable (undo scope project)."): `get`
  lists `shaderKeyText(key)`; `set` parses every item with the `parseShaderKeys` of `BuildTools.cpp` (moved to a shared
  `editor/export/ShaderKeyText.{h,cpp}` with `shaderKeyText`), normalizes (`ShaderPool::normalizeKey`), refuses a
  malformed key (the existing message) and a key present in both lists after the change ("0x... is in both
  shader_additions and shader_exclusions: remove it from one"), then adds one `ShaderOverridesCmd` on apply.
- `build_project shaders` (`BuildTools.cpp:1099-1141`): computes the new sets as today and applies them through
  `ShaderOverridesCmd` (undoable now) instead of assigning.
- Build Settings window: the shader list reads `project->getShaderOverrides()` each frame; checkbox edits add a
  `ShaderOverridesCmd`; `m_shaderAdditions/m_shaderExclusions` and the write-back at `ExportWindow.cpp:344-347` go.
- `docs/mcp/guide/shipping.md`: a "Shader overrides" paragraph; `get_build_info` description: "include_shaders: keys
  (edit: set_project_settings build.shader_*)".

### 6.9 Build tools (`editor/util/ProjectChecks.{h,cpp}`, `BuildTools.cpp`, `Project.cpp`, `EditorScripts.cpp`)

- `std::string ProjectChecks::checkCppToolchain(const LocalBuildSettings& settings, CMakeKit& kit)`: the
  `useDefaultKit` + kit setup + `Generator::checkBuildTools(useDefaultKit, &kit)` of `ProjectChecks.cpp:446-452`.
  Callers: `prepareCppBuild`, `Project::start` (`Project.cpp:9617-9624`), `EditorScripts.cpp:747`, `get_build_info`.
- `get_build_info` adds `cpp_scripts: {can_build, missing_tools, kit: {c_compiler, cxx_compiler, generator}}`.
  Windows: `detectAvailableKits` runs vswhere; if that takes more than ~1 s on a slow machine the implementer caches
  the result for the session keyed by the build settings (stated, not measured here). Description gains
  "cpp_scripts: can Play build C++ here".
- Platform note: the Windows branch of `checkBuildTools` cannot be built here; the change is only the call site, which
  compiles on every platform.

### 6.10 Saves and holds (`editor/mcp/McpServer.cpp`, `editor/Project.cpp`)

- `McpServer::execute`, before the "not available while a scene is playing" check (`:1272`):

  ```cpp
  // a person's save (Ctrl+S, Save All, Save As): the call is valid, it waits as at the window
  if (project->isAnySceneSaving() && !project->isPlaySessionActive() &&
      (mayChange(def, call->args()) || !def.allowedWhilePlaying)) {
      const auto started = std::chrono::steady_clock::now();
      call->waitUntil([this]() { return !appPtr->getProject()->isAnySceneSaving(); }, 60000,
          [this, call, waiting = &def, started](bool timedOut) {
              if (timedOut) { call->fail("A scene save started in the editor is still running after 60 s; "
                                         "nothing was changed. Retry when editor_status shows no scene saving."); return; }
              call->noteWaitedForSave(msSince(started));   // result field waited_for_save_ms
              execute(call, *waiting);
          });
      return;
  }
  ```
  `noteWaitedForSave` adds `waited_for_save_ms` to the result's structured content and "(Waited N ms for a scene
  save started in the editor.)" to its text (the mechanism `notes` uses at `:1316-1320`, implementer's choice).
- The play check's text then only fires for real play states; it names the state: "while a scene is playing" /
  "while Play mode is starting" / "while Play mode is stopping" (from the states, `McpRefs.cpp:92`).
- `CommandHistory::beforeMutation` (`Project.cpp:362-371`): `SAVING` → "a scene is being saved".
- `save_project` (`ProjectTools.cpp:599-602`): unchanged code; the hold runs before it.
- Test hook: `saveSceneToPathAsync`'s worker sleeps `JASPER_TEST_SCENE_SAVE_DELAY_MS` before writing (pattern of
  `JASPER_TEST_EXPORT_HOLD`, `Exporter.cpp:2217`). Debug and release alike, read once.
- `docs/mcp/guide/data-safety.md`: "a call during a person's save waits for it".

### 6.11 Terrain window (`editor/window/TerrainEditWindow.{h,cpp}`, `editor/App.cpp`)

- Move the body of `openForEntity` that reads settings (`:3652-3683`, `toolCategory` included) into
  `void loadSettings()`; add `bool settingsLoaded = false`.
- `show()`: at the top, when `project` exists and `!settingsLoaded`: `loadSettings(); settingsLoaded = true;`. The
  write-back block (`:3589-3617`) runs only when `settingsLoaded`.
- `openForEntity`: no settings read (the window has them).
- `void resetProjectState()`: `endStroke()`, clear the target, `settingsLoaded = false`. Called from the project switch
  block (`App.cpp:4885-4906`) beside `sceneWindow->resetProjectState()`, and after `Stream` applies the workspace's
  terrain settings at project load (`Stream.cpp:3700-3702`) if the first frame can draw before it (the implementer
  checks the startup order and records it).
- `scriptedStroke` (MCP strokes) calls the same lazy load first, so a stroke before any frame does not later write
  defaults.

### 6.12 Export and platforms

No runtime format or exported-build change except the camera up fallback (engine, every backend and target: plain CPU
math in the view update). Windows-only code paths touched: none new (`checkBuildTools` call sites only).

## 7. Behaviour rules and edge cases

- Guide: the pointer sentence is inside the cut; the cut never splits Building scenes (gate). A client without a cut
  sees the pointer and ignores it (the text ends with Tool groups).
- `read_file jasper://guide` returns exactly `mcpServerInstructions()`; `jasper://project/<path>` keeps its
  trusted-root rules (`McpRefs::resolveProjectPath`); resources that write (project AGENTS.md) still run in a
  `McpWriteScope`. `read_file` keeps `readOnly: true`: the AGENTS.md resource writes only a missing file and already
  does so through `resources/read`, which is read-only too (no change in policy). `jasper://docs/<name>/<slug>`
  sections read like documents.
- Details: `remove` with a duplicate or out-of-range index fails before any change; `update` of a removed index fails
  (indices are those before the call for remove, after removes for update, as `edit_colliders`). An empty `add` item
  fails ("mesh is required"). A mesh path that does not exist fails; one outside the assets folder fails with the
  assets folder named. `density_map: ""` clears the map (allowed; note "draws nothing").
- Details while the Terrain window has a detail layer selected that is removed: the window clamps its selection
  (existing `layerCount` handling).
- `set_terrain_maps` with `blendmap.index` and `layers` in one call: one undo step as today.
- Camera: `orbit` or `distance` alone never refuses (they keep a non-zero offset, `ViewTools.cpp:1101-1105`); `view`
  axis views keep their own up. A 2D scene is unaffected. A game camera whose target equals its position keeps today's
  behaviour (the direction is zero; `upFor` returns the preferred up; nothing new is computed).
- Rotations: `{"euler": ..., "quat": ...}` written back from a read: the moved one wins (unchanged), the quaternion
  normalized. A `[0,0,0,1]` identity passes. Lattice and Euler-only callers unchanged.
- Shader overrides: setting a list to its current value adds no undo step; undo restores both lists; a change while a
  build runs is refused by the busy policy (`ToolActivity::Edit` vs export) as every edit is.
- Save hold: a save that finishes with failure still releases the hold (the state returns to STOPPED, `Project.cpp:7552`);
  the call then runs. A second save started while a call waits extends the wait (same predicate). Play pressed by the
  person during the wait: the call re-enters `execute` and meets the play checks.
- Terrain window: a project with no saved terrain settings loads the defaults (as today); a person's changes are
  saved from the first frame after the load.

## 8. Performance and cost

- Guide: +76 bytes, 34 under the line after the change; `read_file` on resources costs only when called.
- Camera fallback: one cross product per camera view update (cameras whose transform or target changed); no GPU cost.
- Details: editor-only validation; `getFoliageInstanceCount` runs only inside `sample_terrain`/`edit_terrain_details`.
- Rotation normalization: per MCP value parse, negligible.
- Save hold: zero when no save runs (one loop over scenes per call: `isAnySceneSaving`).
- `get_build_info`: Windows kit detection now always runs there (it ran in Play already); cache if slow (6.9).
- Nothing reaches exported games except the camera fallback.

## 9. Compatibility

- No file format change: `foliageLayers`, `blendMaps`, `surfaceLayers`, `shaderAdditions/shaderExclusions` and the
  workspace's `terrainEditor` keep their keys. Old scenes with out-of-range detail values load clamped, with a warning.
- MCP results: `sample_terrain`/`set_terrain_maps` `info.foliage_layers` is renamed `details` (richer); tests that read
  it are updated (`grep foliage_layers tools/mcp/tests`). Rotations that were wrongly accepted are now refused: an
  agent gets the reason and the form to use.
- Projects keep their Terrain window settings; a workspace whose values were already overwritten by the bug stays
  as saved (nothing to recover).

## 10. Tests

Red controls: the test fails on `main` (`b4f7816ea`) for the reason given, and passes with the change.

Unit (`jasperengine-editor-tests`):
- `editor/tests/mcp/McpQuaternionTest.cpp` **(red)**: `[90,0,0,1]` refused with "euler"; `{x:90,y:0,z:0}` refused (all
  four keys); `[0,0,0.71,0.71]` accepted and stored with |q| = 1 ± 1e-6; `[0,0,0,0]` refused; `[0,0.5,0,1]` refused;
  `{"euler":[0,90,0]}` equals `Quaternion(0,90,0)`; identity passes.
- `editor/tests/terrain/TerrainDetailRulesTest.cpp`: every range and pair, the mesh extensions; `clampDetailLayer`
  swaps inverted pairs and reports the fields. Red: the decode test below.
- `editor/tests/stream/TerrainDetailDecodeTest.cpp` **(red)**: decoding a YAML layer with `density: -5`,
  `minScale: 3, maxScale: 1` yields 0 and swapped scales (today: kept as is).
- `editor/tests/command/ShaderOverridesCmdTest.cpp`: execute/undo restore both sets and rewrite project.yaml (on a
  temporary project, `DebugSettingsTest.cpp:150-195` pattern).
- `editor/tests/sceneview/SceneViewMathTest.cpp`: `axisViewUp(TOP/BOTTOM)` equals `Camera::upFor` for straight
  down/up.

Unit (`jasperengine-core-tests`):
- `engine/tests/core/camera/CameraLookTest.cpp` **(red)**: a scene camera at `(0,10,0)` targeting the origin with the
  default up: after an update its view matrix is finite with an orthonormal 3x3 (|det - 1| < 1e-4) and maps the
  origin to the view's -Z axis (today: a zero row, det 0).

MCP (`tools/mcp/tests/`):
- `test_agent_guide.py` (new): **(red)** `initialize` instructions' first 4,096 characters contain "read_file
  jasper://guide" and "## Building scenes" (today neither); `read_file path=jasper://guide` returns the instructions;
  `read_file path=jasper://docs/authoring` and `jasper://docs/terrain` return their docs (red: today read_file refuses
  `jasper://`); `start_line`/`max_lines` window a doc; `jasper://docs/nope` fails listing documents; the `new_2d_game`
  prompt mentions `set_child_scene` and `create_prefab`.
- `test_tool_budget.py`: `test_guide_rules` gains the client cut: a made-up guide whose "Building scenes" ends after
  character 4,096 fails naming it; one whose intro lacks the pointer fails; a budget without `client_cut` keys fails
  the shape check. The live check passes.
- `test_terrain_details.py` (new): group hidden until `tool_groups` enables `terrain`, listed after; add a layer with a
  written `.obj` and `density_map {fill: 1, resolution: 64}` → one undo step, the PNG exists, after `wait frames=3`
  `sample_terrain` `details[0].drawn > 0`; `undo` removes it; update `density` and `max_slope`; remove; refusals:
  density 500 (names 0-128 and the cell cap), `min_scale > max_scale`, mesh `.png`, mesh outside assets, bad index;
  **(red)** `set_properties foliageLayers` with density -5 is refused (today accepted).
- `test_world.py` (or `test_stochastic_tiling.py`) **(red)**: `set_terrain_maps blendmap {color:[1,0,0], index: 1}`
  sets `blendMaps[1]` and keeps `[0]` (today: unknown key `index` refused by the closed schema); `index: 2` with one
  slot refused; `layers [{index: 3, uv_scale: [4,4], roughness_factor: 0.6, tint: [1,0.9,0.8,1]}]` read back through
  `get_component`; `normal_strength: 3` refused.
- `test_scene_view.py`: **(red)** `set_scene_camera position [0,25,0] target [0,0,0]` returns `camera.up` `[0,0,1]`
  and `pick_at u=0.5 v=0.5` hits the object under the view's centre (today up stays `[0,1,0]`); `position == target`
  refused; with a camera preview active (`set_properties` route or the Inspector's preview through `run_command`, the
  implementer picks what exists) a move returns `changed` containing `left_preview`.
- `test_scene.py` **(red)**: `set_properties transform rotation [90,0,0,1]` refused with "euler"; `[0,0,0.71,0.71]`
  accepted, read back with |quat| = 1.
- `test_build.py` (or `test_export_shaders.py`): **(red)** `set_project_settings {"build.shader_exclusions":
  [<key from get_build_info>]}` → no job started (`list_jobs` empty), `get_build_info include_shaders` shows it
  excluded, project.yaml has it, `undo {scope: "project"}` restores (today: unknown key); a key in both lists refused;
  a malformed key refused; `build_project shaders.exclude` is undoable. `get_build_info.cpp_scripts.can_build` agrees
  with `compile_scripts` on a project with a C++ script (no red possible on Linux: the divergence is Windows-only,
  inferred).
- `test_busy.py` **(red)**: editor started with `JASPER_TEST_SCENE_SAVE_DELAY_MS=1500`; dirty the scene, `run_command`
  the File > Save command (its id from `list_shortcuts`), then at once `create_entity` and `set_properties` succeed
  with `waited_for_save_ms > 0` (today: "Stop Play mode first" and "saving before Play"); `editor_status` during the
  save answers at once and shows the scene `saving`; with a delay of 70 s (`JASPER_TEST_SCENE_SAVE_DELAY_MS=70000`) the
  call fails after 60 s with the save message (marked slow; or a test-only shorter hold through
  `JASPER_TEST_SAVE_HOLD_MS`, the implementer's choice).
- `test_terrain_window.py` (new) **(red)**: write `terrainEditor: {heightMapResolution: 64, brushSize: 9}` into a
  copy's `workspace.yaml` (pattern `test_scene_view.py:571-594`), start an editor, open Window > Terrain through
  `run_command`, `wait frames=3`, `save_project`: the workspace still has 64 and 9 (today: 512 and 4);
  `set_terrain_maps heightmap {generate: "middle"}` without resolution writes a 64-pixel map.
- Doc tests: `tools/lua_api/doctest.py` and `test_lua_api_json.py` cover the Lua blocks in `authoring.md`; `test_autosync.py`
  checks the tool and parameter names in the guide; `test_docs.py` (`gen_tool_docs.py --check`).

Gates: `tools/check-changed` per commit; `tool_budget.py --check tools/mcp/budget.json`; `gen_tool_docs.py --check`.

## 11. Acceptance criteria

1. The guide's first 4,096 characters contain the intro with the `read_file jasper://guide` pointer, Addressing, The
   loop and Building scenes; `tool_budget.py --check` passes with the 1,500-byte headroom and the new `client_cut`
   rule, with a red rule test.
2. `read_file` reads `jasper://guide`, `jasper://docs/<area>` and its sections, in windows, from MCP and from
   `jasperengine-editor call read_file --path jasper://guide`.
3. `docs/mcp/guide/authoring.md` and `docs/mcp/guide/terrain.md` exist, are served, are listed in the guide README,
   and their Lua blocks pass the doc tests; `lua-api.md`, `cpp-scripting.md`, `animation.md`, `prefabs.md`,
   `shipping.md`, `data-safety.md`, the glossary and the `new_2d_game` prompt carry the rules of 6.4; new projects'
   `AGENTS.md` has "Building scenes".
4. `edit_terrain_details` adds, updates, removes and lists detail layers in one undo step each call, refuses every
   out-of-range value and bad mesh naming the range, and `sample_terrain` reports `drawn` per layer.
5. `set_properties` on `foliageLayers` refuses invalid layers; scenes with invalid values load clamped with a warning.
6. `set_terrain_maps` writes blend map slots 0-2 (no gaps) and the seven layer factors, validated.
7. `set_scene_camera` aimed straight down or up gives a valid view with the axis-view up, refuses `position == target`,
   leaves an active preview and reports it; a game camera aimed straight down renders a valid view.
8. MCP rotations are stored as unit quaternions; Euler-looking and partial input is refused with the Euler form.
9. `build.shader_additions`/`build.shader_exclusions` read and write over MCP without a build, one undo step each;
   `build_project shaders` is undoable; the Build Settings window shows a change made while it is open and keeps it.
10. `get_build_info.cpp_scripts` is computed by the same function Play uses.
11. During a person's scene save, editing calls wait and succeed (or fail after 60 s with the save named); no message
    says "Stop Play mode" or "saving before Play" when nothing plays.
12. Opening the Terrain window from the menu or the layout keeps the saved settings; a project switch loads the new
    project's.
13. Every red test of section 10 fails on `main` and passes on the branch; the per-commit gate passes; `TOOLS.md` is
    regenerated.

## 12. Files likely touched

- Guide and docs: `editor/mcp/McpInstructions.cpp`, `docs/mcp/guide/{authoring,terrain}.md` (new),
  `docs/mcp/guide/{README,lua-api,lua-scripting,cpp-scripting,animation,prefabs,shipping,data-safety,editor-integration}.md`,
  `docs/unity-glossary.md`, `docs/mcp/conventions.md`, `.claude/rules/mcp.md` (if it lists guide rules),
  `docs/mcp/TOOLS.md` (generated), `tools/mcp/budget.json`, `tools/mcp/tool_budget.py`.
- MCP: `editor/mcp/McpServer.{h,cpp}` (readResource, save hold, play texts), `editor/mcp/McpValues.{h,cpp}`
  (parseQuaternion, foliage guard), `editor/mcp/McpToolGroups.cpp` (terrain line), `editor/mcp/TerrainMaps.{h,cpp}`,
  `editor/mcp/tools/{AssetTools,WorldTools,ViewTools,BuildTools,DocsTools,ComponentTools,PlacementTools}.cpp`.
- Editor: `editor/util/TerrainDetailRules.{h,cpp}` (new), `editor/StreamComponents.cpp`, `editor/window/TerrainEditWindow.{h,cpp}`,
  `editor/App.cpp` (project switch call), `editor/Project.cpp` (beforeMutation text, start's toolchain call, save
  delay hook), `editor/util/ProjectChecks.{h,cpp}`, `editor/util/EditorScripts.cpp`, `editor/util/ScriptEvents.cpp`,
  `editor/Generator.cpp` (AGENTS.md text), `editor/command/type/ShaderOverridesCmd.{h,cpp}` (new),
  `editor/settings/SettingsRows.cpp`, `editor/window/dialog/ExportWindow.{h,cpp}`, `editor/export/ShaderKeyText.{h,cpp}`
  (new, moved helpers), `editor/sceneview/SceneViewMath.cpp` (test only).
- Engine: `engine/core/object/Camera.{h,cpp}`, `engine/core/subsystem/RenderSystem.cpp` (camera view update),
  `engine/core/subsystem/MeshSystem.{h,cpp}` (instance count getter).
- Tests: as in section 10, plus `tools/mcp/tests/test_place_with_physics.py:507`, CMake lists of the new unit tests.

## 13. Cross-feature dependencies

- **All features adding guide text**: after this change the guide is 34 bytes under its line; they must pay by
  moving detail of their own sections into docs (conventions). Land order: this feature's guide restructure first is
  simplest (others then rebase onto the new order); the lead decides.
- **render-correctness**: adds `TerrainFoliageLayer` fields (`minDrawDistance`, distance scale; Doriax `e1bfe0341`) →
  the detail rules table and `edit_terrain_details` fields; both edit `RenderSystem.cpp` (camera view update here) and
  `MeshSystem.cpp` (foliage ring there, an instance-count getter here).
- **ui-components**: Button click semantics (a Button `onClick` firing on Submit, navigation) → the click line in
  `authoring.md`, `lua-api.md`, `cpp-scripting.md`; both may edit `ScriptEvents.cpp`.
- **scripting**: edits `lua-api.md` and `cpp-scripting.md` (bindings, script property refresh) → text merge points only.
- **export-pipeline**: shader keys (stale keys, deleted forks) may flag overrides naming a missing shader; both touch
  `BuildTools.cpp`, `get_build_info`, and possibly `ExportWindow.cpp`.
- **mobile-shipping**: adds Project Settings rows (`SettingsRows.cpp`), edits `shipping.md` and `Generator.cpp`, removes
  AdMob from `shipping.md:12-14` text this spec cites.
- **build-toolchain**: edits `Generator.cpp` (generated files) and possibly `ProjectChecks.cpp`; the AGENTS.md text
  block is in `Generator.cpp:1682-1802`.
- **scene-integrity**: Save All for never-saved scenes and save callbacks (`App.cpp:259-290`,
  `Project.cpp:7500-7590`): the save hold relies on `SAVING` being set for every person's save and cleared on every
  outcome; the test hook sits in `saveSceneToPathAsync`.
- **code-editor-git**: none in code; guide budget only.

## Deviations (implementation, 2026-11-04)

1. **The guide plan follows the lean MCP rules (`fe3e90b97`), not section 6.1.** `main` halved the guide (6,241
   bytes, limit 8,192 with 1,500 headroom: at most 6,692) and made every new guide or tool text need an eval case.
   The client cut is **not** handled by that change: measured on `main`'s guide (Python, the gate's own functions,
   `dc-work/aa-evidence/red-client-cut.txt`), Claude Code's 4,096 characters end inside "Lua scripts", so a Claude Code
   agent never saw C++, Physics, Prefabs, Shaders, HTML/CSS UI, Other areas or the Tool groups line (the line that
   tells it to call `tool_groups`; the eval itself runs Claude Code, so it measured the cut guide). What changed:
   - `McpInstructions.cpp` is two literals around the generated groups line: head = intro (with the pointer
     `Text cut before "## Other areas"? read_file jasper://guide.`), Addressing,
     The loop, Building scenes, then **Tool groups**; tail = Code, Undo, Lua, Unity names, C++, Physics, Prefabs,
     Shaders, HTML/CSS UI, Other areas. Unity names moved after the cut (the pointer leads there).
   - "Building scenes" is one short section (assemble in the editor, script properties, spawn from a Prefab, make what
     repeats a Prefab, animation through `edit_animation`); the HUD rule is not repeated there (the HTML/CSS UI digest
     and `create_ui`'s result carry it). The Prefabs digest loses its runtime signature (in `prefabs.md` and the Lua
     digest). Measured after merging `main` on 2026-11-06: about 6,680 bytes (the first gate run measured 6,682 before the last trim), just under the line; the cut keeps intro, Addressing, The loop, Building scenes,
     Tool groups, Code and Undo.
   - Rules agents need at the moment they act went into results (`next`, the lean rules' preferred place):
     `create_ui` in a 3D scene says where a HUD goes; `duplicate_entities` of a plain GameObject suggests a Prefab.
   - `budget.json` `guide.client_cut` lists `(intro)`, Addressing, The loop, Building scenes and Tool groups; the gate
     also checks the pointer. Eval cases added (`tools/mcp/eval/cases.py`): `bad_repeat_without_prefab`,
     `bad_bob_in_script`, `bad_hud_widgets_in_3d`, `guard_rotation_not_euler`, `guard_guide_past_the_cut`,
     `bad_terrain_grass`, `bad_shader_override_by_building`, `bad_cpp_without_toolchain`.
2. **Shader overrides: a tool in the `shipping` group (`edit_build_shaders`), not settings rows.** The settings
   registry's rows are Project Settings / Preferences rows: the palette and the settings search would send a person to
   a Project Settings page that does not draw them (the overrides live in Build Settings). `edit_build_shaders
   {add, exclude, remove, clear}` (no arguments: lists) is one project undo step (`ShaderOverridesCmd`), and
   `build_project shaders` goes through the same code. The Build Settings window applies each edit through the command
   and shows the project's lists every frame (it no longer writes a working copy back).
3. **`left_preview` has no MCP test**: no tool turns the Inspector's camera preview on, so the branch is covered by
   reading only (inferred).
4. **`editor_status` no longer reports `playing: true` during a person's save** (same class as 4.7): it reports
   `saving: true`.
5. **The Terrain window reloads by generation, not only on a project switch.** `Project::setTerrainEditorSettings`
   bumps a generation (workspace load, project close); the window reads the settings when the generation changed and
   writes back only over the generation it read, so a frame drawn before the workspace loads cannot overwrite it.
6. **The generated `AGENTS.md` gets no authoring text.** Its cap is 5,000 bytes (`test_generators.py`), and after the
   2026-11-06 merge `main`'s own text measures about 5,150 bytes (estimated from the literals; the first gate run
   measured 5,160 with one authoring paragraph). The guide, `jasper://docs/authoring` and the results' `next` hints
   carry the rules instead.
7. **`get_build_info`'s description does not name `cpp_scripts`.** The core listing is capped at 99,000 characters
   and measured 99,053 with it after the 2026-11-06 merge; the field is in every result (AGENTS.md "MCP text":
   descriptions hold no result field lists), and `read_file`'s description names only `jasper://guide`.
8. **The Terrain window's regression test drives the bug through MCP.** In a headless test editor the window opened
   from Window > Terrain need not draw (a docked tab), so the red build passed that test. The test now makes a
   `sculpt_terrain` stroke on a terrain with no height map before the window ever opens: the missing map is created
   at the window's resolution, the saved 64 pixels, where the old code used the constructor's 512.
9. **`Camera::upFor` is the free function `cameraUpFor`** in `engine/core/component/CameraComponent.h`: the
   render system and the editor call it with no Camera object. After review it catches only truly parallel
   vectors (sine below 1e-6), so an orbit clamped 1 degree from vertical keeps its up, and its fallback is
   perpendicular to the view (looking along Y: the Top/Bottom views' Z; otherwise world Y).
10. **The near-unit quaternion rule is documented in `docs/mcp/DESIGN.md` (Values)** and the `read_file`
    route in `editor-integration.md` ("Reading the API"); reads return the stored rotation normalized, so a
    read-modify-write of a rotation stored off unit length succeeds.
11. **Detail meshes are every model the import pipeline loads** (`Util::isModelFile`: .obj, .gltf, .glb, .fbx,
    an FBX through its import as in the Terrain window); `edit_terrain_details` writes its density maps
    inside its undoable step (undo removes them) only after every item was checked, and `set_properties`
    checks only the detail layers it changes. A layer clamped on load is a load-report issue naming the
    layer, at the terrain's line of the scene file.
12. **After the 2026-11-07 merge** (`main` 8e21b1dd2: ssr-probes' and the sound layer's index entries) the guide
    measured 6,744 bytes, 52 into the headroom. The intro's "Build scenes with tools, write code with your own
    file tools, verify by playing..." sentence went (The loop and Building scenes say it), and Building scenes
    and the pointer were shortened: 6,613 bytes, 79 under the line.

## Follow-ups (2026-11-07)

1. **An FBX detail layer drew 0 instances: blend shapes plus instancing, not FBX and not headless.** The
   test's `rigged_cube.fbx` is textured, skinned and has a blend shape. Every foliage chunk asked for the Lit
   variant with instancing and morph targets: position, normal, UV, the instance's six vectors and eight morph
   slots make 17 vertex inputs (skinning adds two), past the 16 of GL 3.3, GLES3/WebGL2 and the other backends.
   The variant never compiled, so the chunks never finished loading. A copy a system places never animates:
   it builds no skeleton (its bones stay at the identity) and nothing sets its blend shape weights.
   - `MeshSystem::loadPlacedModel` (foliage chunks, spline chunks and their probe) loads it at rest as static
     geometry. The skin goes, and the blend shapes' default weights (the file's mesh weights) are baked into
     positions and normals. Its node transform now bakes too, as for a mesh without blend shapes
     (`GltfMeshOffset.FoliageBakesTheWholeNodeTransform` changed with it). The decision lives in a private
     loader parameter that only `loadPlacedModel` passes; `loadGLTF` (and its Lua binding) keeps skins and
     blend shapes whatever its flags.
   - The export's shader variant collector probes foliage through `loadPlacedModel`, so it ships the variant
     the chunk asks for (it shipped the skinned one).
   - An Instanced Mesh a person adds to a textured model with a blend shape still cannot draw (its weights
     can change at runtime, so nothing is baked). The Console says so once, naming the GameObject and the fix,
     in place of only the shader compiler's lines.
   - Tests: `test_terrain_details.py::test_fbx_layer` (the FBX, textured), `PlacedModel.*` (the bake, the Lua
     flags, the collected variant), `test_render_correctness.py::test_instanced_blend_shapes_say_why_they_do_not_draw`.
     The draw checks wait for the state with a hang guard and report their time (`over_budget`): on a box's
     first run (8 editors, cold shader variants) the OBJ layer of the older test drew 0 within the old
     100-frame budget too.
2. **The generated `AGENTS.md` has headroom under its 5,000-byte cap** (`test_generators.py`, not raised). The text
   is the same for every template (2D, 3D, Empty, UI): `writeAgentsFile` takes only the project paths, which are
   always `.jasperengine/...`. Only the editor version string changes its size. Before: 4,929 bytes (version
   `unknown`) to 4,955 (`doriax-base-7478-g17c19b941-dirty`). After: 4,373 to 4,399 bytes (4,401 with a
   30-character version), so about 600 bytes of headroom. Wording was cut, not rules. The second CMake example line
   went, the Source references paragraph became a link plus the formats line, and the `set_shortcut` sentence
   moved into the list. The lines the eval cases pin stay: the sound tools and their group, `edit_sound_event`
   for clips that vary, `set_shortcut` (tool group `editor_ux`), `inspect_models`/`check_render_budgets`, and
   `jasperengine-editor call`. No template under `editor/resources/templates` embeds the file, so nothing was
   regenerated.
