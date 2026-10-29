// The agent guide: sent as MCP initialize.instructions on every session and served as the
// resource jasper://guide. It costs tokens on every session, so it holds the workflow every
// agent needs (names, addressing, the loop, undo, code, layout) and a short digest per area;
// an area's detail lives in docs/mcp/guide/<area>.md (served as jasper://docs/<area>), which
// its digest points to. Its limits are the "guide" entry of tools/mcp/budget.json (the whole
// text, with the generated tool groups section, the headroom it must leave for later specs,
// and each "## " section), gated by
// tools/mcp/tool_budget.py --check; docs/mcp/conventions.md ("The agent guide") has the rule.
// Every tool and parameter name it mentions must exist (tools/mcp/tests/test_autosync.py
// checks them against tools/list).
//
// MSVC limits one string literal piece to ~16 KB, so the text is split into adjacent raw
// literals (concatenated by the compiler). Keep every chunk well under 16000 bytes.

#include "McpServer.h"
#include "McpToolGroups.h"

#include <string>

namespace jasperengine::editor::mcp {

namespace {

const char* baseInstructions() {
    return
R"JE(# JasperEngine editor (MCP)

You are driving a running JasperEngine editor: a C++ game engine with a Unity-style editor
(Hierarchy, Inspector, Project window, Console, Scene and Game views) and Lua or C++ gameplay
scripts. Each tool edit is an undoable command, live to a human watching. Build scenes
with tools, write code with your own file tools, verify by playing, screenshots and the Console.
tools/list is authoritative for tool names and arguments; read a tool's description before
first use. Each area below points to its reference (jasper://docs/<area>).

## Unity names -> engine names
Tools, component names and script APIs use the engine names.
GameObject = entity (Entity id; script class GameObject). Create Empty = type empty.
Prefab = .prefab asset (PrefabComponent on the instance root; PrefabAsset in scripts).
Mesh Filter/Renderer = MeshComponent (Primitive for box/sphere/plane...). Sprite Renderer =
SpriteComponent. Rigidbody / Rigidbody 2D = RigidbodyComponent (Jolt) / Rigidbody2DComponent
(Box2D). Collider = an entry of the Rigidbody's colliders list, not a separate component.
Most others are Unity's name + "Component" (AudioSourceComponent...).
Legacy Animation = AnimationPlayerComponent (AnimationComponent: entity sequences).
Build Settings = export; Scenes in Build = the scene list in project.yaml.

## Addressing
- scene: numeric id ("12" ok), scene name, or project-relative .scene path. Omitted = the
  selected scene. While playing, runtime-state tools resolve to the playing scene. parent:
  omitted = the scene root. max_width 0 = full size.
- entity: numeric id ("12" ok), or a path of names from the scene root joined by "/" with "#N" for the
  Nth earlier sibling of the same name ("Player/Gun", "Enemy#2"), or a bare name unique in the
  scene; {"id"|"name"|"path"} objects (as results return them) work too. Ambiguous names fail
  and list the matching paths. Results return both id and path; prefer ids within a session.
  `entities` (array) instead of `entity` applies a tool to several GameObjects.
- component: lowercase catalog names (transform, mesh, sprite, rigidbody2d, ...); unknown
  names fail with the valid list. property: catalog paths ("position",
  "submeshes[0].material.baseColorFactor", "scripts[0].speed").
- values: vectors [x,y,z] (or {x,y,z}); quaternions [x,y,z,w] or {"euler":[x,y,z]} degrees;
  colors as vectors (0..1); enums by name; textures/fonts/materials by asset path;
  entity references by entity ref, or {"scene": <scene>, "id": <id>} for a GameObject of
  another scene; Prefab script properties by .prefab path.
- assets: project-relative paths ("assets/sprites/ship.png") or GUIDs. A move rewrites no
  referrer; see find_references, list_missing_assets, retarget_asset_references.

## The loop
1. Inspect: `editor_status`, the scenes, the hierarchy, the GameObjects you will touch.
2. Edit scenes, prefabs and settings with tools, never by hand-editing .scene/.prefab/
   project.yaml. Group many edits in one `batch` call
   (undo_group: one undo step, see Undo). Edit code with your own tools (see Code).
3. Save the scenes you changed.
4. Play the start scene; a C++ project builds first (seconds to minutes).
5. Look: screenshot the Game view (Scene view for layout). Drive the game with
   send_input / wait; query runtime state while playing (get_runtime_entity, eval_lua).
6. Read the Console since your last cursor: compile, script and Lua errors, missing assets.
7. Stop, fix, repeat until the Console is clean and the screenshot shows what you intended.
   Never claim a script works without playing it and reading the Console.
Long operations (C++ builds, export, players) take timeout_seconds; past it you get a job
id: poll get_job, cancel_job.
A tool error says what was wrong and lists valid choices: fix the arguments, don't repeat.

## Undo
Edit results carry `undo` {id, name}: one named step per call (a `batch` with undo_group: one,
named by undo_name). undo/redo scope "timeline" = the human's Ctrl+Z/Ctrl+Y; expect_top_id
undoes only your own last step. Play-time edits roll back at play stop and are offered back
(get_play_changes). More: jasper://docs/undo.

## Editor search
editor_ux group: search_editor = the Ctrl+K palette (> / : @ + asset:); run_command runs a
list_shortcuts id or a result key; set_shortcut rebinds; get_inspector_layout reads the Inspector's
rows. list_assets search: t: l: ext: path: re: ref: missing:. Layouts, labels, saved searches,
bookmarks: edit_* (jasper://docs/editor-integration).

## Code (Lua, C++, shaders, materials)
The project is a normal folder (`editor_status`: project.path). Work in it and read, edit,
grep and git its scripts, shaders and .material files with your own tools. Engine API to grep: .jasperengine/engine-api/ (C++ headers) and its lua/
(Lua API stubs). Your file edits are reloaded before your next call, whose result
carries `external_changes` {changed, reloaded, problems: [{path, line, message}]}: fix those
problems first. During play a saved Lua script reloads in place, keeping its instances
(external_changes.script_reload; a file that fails keeps its old code running); C++ waits for
Stop unless set_script_settings reload_cpp_during_play. Open scenes and prefabs are not
reloaded from disk: edit them with the tools.
Validate without playing: check_script (Lua), check_shader, compile_scripts
(C++), or `jasperengine-editor check` from a shell. Through tools: create_script (engine
template, optional attach), attach_script / detach_script, set_script_properties. write_file /
read_file / search_files are for clients without file access; sync_files forces a full rescan.

## Project layout
project.yaml (settings, scenes, start scene), *.scene, *.prefab, assets/ (assetsDir), Lua scripts (luaDir), ProjectBuild.cmake.
A .scene belongs to the project only while project.yaml lists it (Scenes in Build): add a
scene created outside the editor before opening, playing or building it.
.jasperengine/ and the root CMakeLists.txt are generated on every play/build: never edit them.
)JE"
R"JE(
## Lua scripts
Read jasper://docs/lua-scripting (a complete script, property types, events) before your first
script. A script returns a module table (`---@class Name : ScriptInstance`, Name = the file
name) attached through the Script component; properties = { {name, displayName, type,
default} } are read as self.<name>. self.entity is a numeric id, NOT an object: wrap it
(GameObject(self.scene, self.entity), Sprite(...), Rigidbody2D(...)); a constructor given only
self.scene CREATES a GameObject. RegisterEngineEvent(self, "onUpdate") in init(), the method
defined first. Bound getters are PROPERTIES: go.position (a copy: assign it back),
Engine.deltatime; a made-up method fails at runtime, so check jasper://docs/lua-api or
search_engine_api. Time: Engine.timeScale and Engine.fixedDeltatime (read/write),
Engine.gameTime, Engine.frameCount. Log.print/warn/error take ONE string (use ..). Lua has no
on_start, getComponent or setProperty (timers: Timer.after/every).

## C++ scripts
Read jasper://docs/cpp-scripting before your first one. A ScriptBase subclass, or a subclass of
the GameObject's own class (Mesh, Sprite, ...) to call its methods on `this` (create_script). No
reflection macros: declare a (jasperengine::Scene*, jasperengine::Entity) constructor, expose
fields with DPROPERTY("Speed"). Includes are flat and quoted ("ScriptBase.h"), never <core/...>.
REGISTER_ENGINE_EVENT(onUpdate) in the constructor, UNREGISTER_ENGINE_EVENT in the destructor.
Log::print, never printf or std::cout. Angles are DEGREES. Build settings go in ProjectBuild.cmake.

## Play and time
- play: start, stop, pause, resume, step (frames), restart; clock options with any action (or
  action "set"): time_scale, fixed_fps (each frame exactly 1/fixed_fps s: reproducible runs),
  auto_pause (frozen between your calls; runs only inside wait, send_input or step).
- wait: run for seconds, game_seconds or frames, or until a Lua condition is truthy; input
  sends send_input events while it waits (hold a key for 2 game seconds, then check).
- run_build runs an exported desktop player (input events, screenshots_at seconds since
  launch); returns its errors and the log tail.

## 2D and 3D
- 2D: sprites, tilemaps, UI; units are points (pixels of the canvas, project canvasWidth x
  canvasHeight, y up). Box2D converts with pointsToMeterScale (64 points per meter). Light 2D
  adds to the 2D ambient (lower ambient_light_2d_intensity to see it); Shadow Caster 2D needs the
  light's shadows on.
- 3D: meshes (box, sphere, plane, model files), lights, skybox, fog, terrain. Cameras look at
  their target (useTarget): the rotation is ignored until disableTarget() (jasper://docs/lua-api).
- UI widgets (HTML/CSS UI: below): create_ui lays out in pixels, y DOWN from the parent's
  top-left. Put HUDs and menus in a UI scene attached as a child scene (set_child_scene): in a
  2D scene layout groups stack bottom-up and pointer input needs a UI camera.
- Poses (.poseanim): get/edit/render_pose_animation (jasper://docs/pose-animation).
- Clips (.anim, shared): edit_animation {clip, create, tracks [{path}]}, for an Animation
  Player (jasper://docs/animation).

## Atmosphere
A Fog of type "height" is physically based height fog (volumetric: true adds light shafts and
Local Volumetric Fog volumes near the camera); create_entity volumetric_clouds adds clouds.
apply_atmosphere_preset sets a look (list_atmosphere_presets); get_atmosphere_info says what
ran (tier, passes, resolution). Atmosphere Quality is per platform (project settings
quality.atmosphere_quality_*). More: jasper://docs/atmosphere.

## Physics
- Add a body and its collider to the SAME GameObject in one step (edit_colliders with
  body_type + add); a Rigidbody without a Collider does not collide.
- Collider sizes are local (scaled by the GameObject's scale): never pre-scale or guess them;
  omit them to fit the mesh bounds. Mass is collider size x density, never set directly.
- Forces go in onFixedUpdate, not multiplied by deltatime. Move a body through body.position /
  body.linearVelocity, never its transform. 3D and 2D APIs differ: jasper://docs/physics.
- Drop or settle objects: place_with_physics (group editor_ux). Navigation: jasper://docs/navigation.

## Prefabs
An edit on a prefab instance overrides that instance only: Apply it to the asset, or edit the
asset in Prefab Mode (instances cannot be restructured). Runtime: GameObject.instantiate(prefab
[, scene] [, position, rotation] [, parent]) returns "<name>(Clone)"; GameObject.destroy(go).
Prefer a Prefab script property; a prefab only loaded by path (PrefabAsset.load) must be marked
standalone or the build lacks it. More: jasper://docs/prefabs.

## Shaders and materials
Library first: list_shaders ("library:<name>"; with name: its parameters). A mesh shader goes on
a submesh material (set_properties submeshes[0].material.shader, or create_material /
update_material) with .shaderParams.<member> (colours linear, "#RRGGBB" sRGB); UI, Points, Lines
and Sky take customShader. Outlines and stencil: apply_material_preset. fork_shader only when
nothing fits; read the fork first. A failing shader draws as the built-in one. Rules, costs,
uniforms: jasper://docs/shaders; Lit lobes, Lit Quality, light units: jasper://docs/materials.

Textures from examples: synthesize_texture (group assets_pipeline,
jasper://docs/texture-synthesis). GPU fluid: create_entity type fluid_simulation
(jasper://docs/fluid). Bend/squash: edit_lattice (group animation, jasper://docs/lattice).

## Post-processing
Main camera only: apply_post_processing_preset (film, vhs, mobile_adaptive...), then
set_scene_settings post_processing.<field>; get_post_processing_status reads what applies,
inspect_color_lut a .cube. Mobile: in-shader colour, low render scale or dynamic resolution, few
passes (jasper://docs/post-processing).

## Shadows
A light's shadows, softShadows, shadowBias/shadowNormalBias (shadow texels: leave at 1); the
scene's shadows_quality and shadow_distance; meshes that never move: staticShadowCaster true.
Verify with get_shadow_stats and shadows_debug_view (jasper://docs/shadows).

## Profiling
The profiling group (get_frame_times, capture_profile, get_gpu_passes, get_vram_report...):
compare medians, not single frames (jasper://docs/profiling).
Calls time out: get_editor_health (answers mid-hang) tells hung from slow; get_crash_reports
(jasper://docs/crash-diagnostics).

## Build and ship
Scenes in Build order and the start scene live in project.yaml (project tools). Export
(Build Settings, build_project) makes a CMake project or players per platform.
Before exporting: scenes saved, Console clean in play, the start scene right.
)JE"
R"JE(
## HTML/CSS UI
Menus, HUDs and dialogs in standard HTML + CSS, logic in Lua (no JavaScript): create_ui_document
(template: blank, main_menu, hud, inventory, settings, shop...) adds the GameObject and writes
assets/ui/<name>.html, its .css and Lua. Edit them yourself; fix what external_changes reports.
inspect_ui (boxes in canvas px = send_input coords) and assert_ui (text_fits, no_overlap...)
check layouts; import_files brings fonts, icons and videos; effect(<name>, ...) draws GPU effects
(list_ui_effects, set_ui_effect_time).
Edit mode runs no Lua. Read jasper://docs/html-ui (the UI tools' rules) first.

## Data safety
Save after each step (Auto Save keeps backups only). A save that would lose data the editor
could not read is refused; allow_data_loss writes it anyway. degraded_document, recovery,
project_locked: jasper://docs/data-safety.

## Pitfalls
- Never carry over API names from Unity C#, Godot or Unreal: confirm them in jasper://docs or
  .jasperengine/engine-api first.
- Lua: static functions use a dot (GameObject.instantiate), methods a colon (go:setScale(2)).
)JE";
}

// Generated from the group table and the registry, since every spec adds groups and tools
std::string toolGroupsSection(const McpRegistry& registry) {
    std::string names;
    for (const ToolGroupInfo& group : toolGroups()) {
        if (group.name == CORE_TOOL_GROUP) continue;
        for (const ToolDef& def : registry.tools()) {
            if (McpRegistry::groupOf(def) != group.name) continue;
            names += (names.empty() ? "" : ", ") + group.name;
            break;
        }
    }
    return "\n## Tool groups\n`tool_groups` lists and enables the groups beyond core" +
           (names.empty() ? std::string() : " (" + names + ")") +
           " in tools/list; any tool is callable by name without it (also in `batch`).\n";
}

} // namespace

const char* mcpServerInstructions() {
    // Built once the server registered its tools (the first initialize or jasper://guide read)
    static const std::string text = std::string(baseInstructions()) + toolGroupsSection(McpServer::get().registry());
    return text.c_str();
}

} // namespace jasperengine::editor::mcp
