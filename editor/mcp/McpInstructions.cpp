// The agent guide: sent as MCP initialize.instructions on every session and served as the
// resource jasper://guide. Keep it tight (it costs tokens on every session); deeper reference
// lives in docs/mcp/guide/*.md (served as jasper://docs/...). Every tool and parameter name it
// mentions must exist (tools/mcp/tests/test_autosync.py checks them against tools/list).
//
// MSVC limits one string literal piece to ~16 KB, so the text is split into adjacent raw
// literals (concatenated by the compiler). Keep every chunk well under 16000 bytes.

namespace jasperengine::editor::mcp {

const char* mcpServerInstructions() {
    return
R"JE(# JasperEngine editor (MCP)

You are driving a running JasperEngine editor: a C++ game engine with a Unity-style editor
(Hierarchy, Inspector, Project window, Console, Scene and Game views) and Lua or C++ gameplay
scripts. Each tool edit is an undoable command, live to a human watching. Build scenes
with tools, write code with your own file tools, verify by playing, screenshots and the Console.
tools/list is authoritative for tool names and arguments; read a tool's description before
first use. Deeper reference: jasper://docs/... (Lua API, C++ scripting, physics, shaders,
prefabs, assets, HTML/CSS UI, data safety, undo, Unity glossary).

## Unity names -> engine names
Tools, component names and script APIs use the engine names.
GameObject = entity (Entity id; script class GameObject). Create Empty = type empty.
Prefab = .prefab asset (PrefabComponent on the instance root; PrefabAsset in scripts).
Mesh Filter/Renderer = MeshComponent (Primitive for box/sphere/plane...). Sprite Renderer =
SpriteComponent. Rigidbody / Rigidbody 2D = RigidbodyComponent (Jolt) / Rigidbody2DComponent
(Box2D). Collider = an entry of the Rigidbody's colliders list, not a separate component.
Most others are Unity's name + "Component" (AudioSourceComponent...).
Build Settings = export; Scenes in Build = the scene list in project.yaml.

## Addressing
- scene: numeric id, scene name, or project-relative .scene path. Omitted = the selected
  scene. While playing, runtime-state tools resolve to the playing scene.
- entity: numeric id, a path of names from the scene root ("Player/Gun"; "Enemy#2": the 2nd
  earlier sibling of that name) or a name unique in the scene. Ambiguous names fail with the
  matching paths. Prefer ids.
  `entities` (array) instead of `entity` applies a tool to several GameObjects. parent
  omitted = scene root; max_width 0 = full size.
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
1. Inspect: `editor_status`, the scene list, the hierarchy, then the GameObjects you will
   touch.
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
Long operations (play with a C++ build, export, build players) take wait/timeout_seconds;
past it you get a job id: poll get_job, cancel with cancel_job.
A tool error says what was wrong and lists valid choices: fix the arguments, don't repeat.

## Undo
Results carry `undo` {id, name}, one step per call. undo/redo scope "timeline" = the human's
Ctrl+Z order; expect_top_id = your undo.id undoes only your call (jasper://docs/undo).

## Code (Lua, C++, shaders, materials)
The project is a normal folder: `editor_status` gives its absolute path (project.path). Work
in it and read, edit, grep and git its scripts, shaders and .material files with
your own tools. Engine API to grep: .jasperengine/engine-api/ (C++ headers) and its lua/
folder (Lua API stubs). Your edits are picked up before your next tool call; that
call's result then carries `external_changes` {changed, problems: [{path, line, message}]}:
fix those problems first. During play they wait until play stops.
Validate without playing: check_script, check_shader, compile_scripts (C++). Through tools: create_script (engine
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
A Lua script returns a module table; attach it to a GameObject through its Script component.
self.scene is the Scene, self.entity the GameObject's numeric id (NOT an object: never call
self.entity:...). Wrap it: GameObject(self.scene, self.entity), Sprite(...), Text(...),
Rigidbody2D(...), AudioSource(...); a constructor given only self.scene CREATES a new
GameObject. Declare the module `---@class Name : ScriptInstance` (Name = the file name): typed self and
properties, typos caught.

```lua
---@class Player : ScriptInstance
local Player = {
    properties = {
        { name = "speed", displayName = "Speed", type = "float", default = 300.0 },
        { name = "label", displayName = "Label", type = "Text" },      -- GameObject reference
        { name = "bullet", displayName = "Bullet", type = "Prefab" },  -- PrefabAsset
    }
}

function Player:init()
    self.go = GameObject(self.scene, self.entity)
    RegisterEngineEvent(self, "onUpdate")
end

function Player:onUpdate()
    local dx = 0
    if Input.isKeyPressed(Input.KEY_RIGHT) then dx = dx + 1 end
    if Input.isKeyPressed(Input.KEY_LEFT) then dx = dx - 1 end
    local p = self.go.position                          -- a copy: assign it back
    self.go.position = Vector3(p.x + dx * self.speed * Engine.deltatime, p.y, p.z)
    if Input.isKeyPressed(Input.KEY_SPACE) and not self.fired then
        GameObject.instantiate(self.bullet, self.scene, self.go.position, Quaternion.IDENTITY)
        Log.print("fired at " .. tostring(p.x))
    end
    self.fired = Input.isKeyPressed(Input.KEY_SPACE)
end

return Player
```
- Property types: bool, int, float, string, vector2/3/4, color3/color4, prefab, entity, any
  wrapper class name (Sprite, Text, Image, Button, AudioSource, Rigidbody2D, ...), or another
  Lua script's class name (you get that script's self). Values are read as self.<name>.
- Events: RegisterEngineEvent(self, "onUpdate" | "onFixedUpdate" | "onPostUpdate" |
  "onKeyDown" | "onMouseDown" | "onTouchStart" | ...); define the method first. Component
  events: RegisterEvent(self, Button(self.scene, self.entity):getButtonComponent().onPress,
  "onPress"). No manual unregister. onDestroy runs on GameObject.destroy.
- Bound getters are often PROPERTIES, not methods: go.position, sprite.width, body.mass,
  Engine.deltatime. Calling a non-existent method fails with "attempt to call a nil value". Check jasper://docs/lua-api (an index: read only the sections you need), the Lua
  stubs or search_engine_api before using a name you have not seen there.
- Input.isKeyPressed is the held state; detect a press by remembering last frame's value or
  with onKeyDown(key, isRepeat, mods). Log.print/warn/error take ONE string (use ..).
- Time: Engine.timeScale and Engine.fixedDeltatime (read/write), Engine.gameTime and
  Engine.frameCount (scaled game time and frames since start).
- C++ default arguments do not exist in Lua: pass every argument (e.g. the wake flag).
- Lua has no Dori.Script, on_start, getComponent, setProperty or timers. Never put editor
  property paths (submeshes[0]...) in scripts.

## C++ scripts
Two kinds (create_script): a ScriptBase subclass, or a subclass of the GameObject's own class
(Mesh, Primitive, Sprite, ...) to call its methods on `this`. No reflection macros
(GENERATED_BODY, GDCLASS): inherit, declare a (jasperengine::Scene*, jasperengine::Entity)
constructor, expose fields with DPROPERTY("Speed") (DPROPERTY("Tint", Color4) for colors).
Includes are flat and quoted ("ScriptBase.h", "Engine.h", "Input.h", "Log.h", "Mesh.h"), never
<core/...>. REGISTER_ENGINE_EVENT(onUpdate) in the constructor, UNREGISTER_ENGINE_EVENT in the
destructor; components: REGISTER_BUTTON_EVENT / REGISTER_UI_EVENT / REGISTER_COMPONENT_EVENT
(+ UNREGISTER_*). Log::print("x %f", x), never printf or std::cout. Keys are D_KEY_*. Angles are
DEGREES: Quaternion(angle, axis); std::sin/cos want radians (Angle::defaultToRad). Script
directories are include roots compiled whole (a build runs only with an enabled C++ script
attached); build settings go in ProjectBuild.cmake, never the generated CMakeLists.txt.
compile_scripts reports errors without playing. More: jasper://docs/cpp-scripting.

## Play and time
- play: start, stop, pause, resume, step (frames), restart; clock options with any action (or
  action "set"): time_scale, fixed_fps (each frame exactly 1/fixed_fps s: reproducible runs),
  auto_pause (frozen between your calls; runs only inside wait, send_input or step).
- wait: run for seconds, game_seconds or frames, or until a Lua condition is truthy; input
  sends send_input events while it waits (hold a key for 2 game seconds, then check).
- run_build runs an exported desktop player with input events and screenshots_at (seconds
  since launch); returns its errors (Lua too) and the log tail.

## 2D and 3D
- 2D: sprites, tilemaps, UI; units are points (pixels of the canvas, project canvasWidth x
  canvasHeight, y up). Box2D converts with pointsToMeterScale (64 points per meter). Light 2D
  adds to the 2D ambient (lower ambient_light_2d_intensity to see it); Shadow Caster 2D needs the
  light's shadows on.
- 3D: meshes (box, sphere, plane, model files), lights, skybox, fog, terrain. Cameras look at
  their target by default (useTarget): the GameObject's rotation is ignored until you call
  disableTarget() (orbiting: jasper://docs/lua-api, Camera).
- Atmosphere (jasper://docs/atmosphere): Fog type "height" (volumetric: true for light shafts
  and Local Volumetric Fog), create_entity volumetric_clouds, apply_atmosphere_preset;
  get_atmosphere_info says what ran.
- UI (text, image, button, panel, scrollbar, progressbar, input field, layout group; HTML/CSS
  UI: below): create_ui lays out in pixels with y DOWN from the parent's top-left, in 2D scenes
  and UI scenes alike, but in a 2D scene layout groups stack bottom-up and pointer input needs a
  UI camera: put HUDs and menus in a UI scene attached as a child scene (set_child_scene).

## Physics
- Add a body and its collider to the SAME GameObject in one step (edit_colliders with
  body_type + add). A Rigidbody without a Collider does not collide. Dynamic: actors;
  static: floors, level geometry; kinematic: scripted movers.
- Collider sizes are mesh-local, scaled by the GameObject's scale: never pre-scale them.
  Omit sizes to fit the mesh bounds (never guess).
- Mass = collider volume/area x density (default 1000 in 3D, 1 in 2D), never set directly.
  Size forces as mass x acceleration; if they get huge, lower the density.
- Continuous forces and torques go in onFixedUpdate, never onUpdate, and are NOT multiplied by
  deltatime. Rigidbody (3D): applyForce(f), applyForce(f, point), applyTorque(t), applyImpulse;
  mass is the property body.mass. Rigidbody2D: applyForceToCenter(f, true), applyForce(f, point,
  true), applyLinearImpulseToCenter(i, true); mass is body:getMass().
- Never move a body by writing its GameObject's transform from onFixedUpdate (silently
  overwritten). Teleport with body.position (and body.rotation / body.angle), velocity with
  body.linearVelocity. One-shot impulses fire once per press, not every held frame.
- Layer Collision Matrix (project settings, per backend) gates contacts; raycasts take an
  optional layer mask.
- Drop or settle objects: place_with_physics.

## Prefabs
- An edit on a prefab instance is an override of that instance only. Push it to the asset
  with Apply, or edit the asset in Prefab Mode. Instances cannot be restructured (reparent,
  reorder, move out): open the prefab instead. Variants are prefabs based on another prefab.
- Runtime: GameObject.instantiate(prefabAsset [, scene] [, position, rotation] [, parent])
  returns the new root named "<name>(Clone)", scripts already initialized (without scene or
  parent it goes to the active scene); GameObject.destroy(go [, delay]).
  Get the PrefabAsset from a script property of type Prefab (preferred) or
  PrefabAsset.load("path/Name"). A prefab that is only loaded by path, never placed or
  referenced, must be marked standalone or the build will not contain it.

## Shaders and materials
Library first: list_shaders, the engine's shaders ("library:<name>"; with name: parameters,
textures, features). A mesh shader goes on a submesh material
(set_properties submeshes[0].material.shader, or a .material file: create_material /
update_material) with .shaderParams.<member> (colours linear, "#RRGGBB" is sRGB),
.shaderTextures.<uniform> and .shaderFeatures; UI, Points, Lines and Sky take customShader plus
shaderUniforms. Outlines and stencil effects: apply_material_preset (list_material_presets), then
tune its extraPass and stencil. Shaders with scene_inputs (cost_note) add a depth
pass or colour copy per camera and must draw transparent; avoid on low-end mobile. Fork only
when nothing fits: fork_shader copies the built-in shader and sets customShader. Read a fork
before editing: keep its declarations, includes, varyings and output, change the math near the
end of main(), parameters in its u_fs_customParams block (set_shader_uniform); rules and depth
forks: jasper://docs/shaders. A failing shader draws as the built-in one (error in the Console).
Post-processing (main camera only): apply_post_processing_preset, then set_scene_settings
post_processing.<field>; get_post_processing_status says what applies (mobile, LUTs:
jasper://docs/post-processing).
Textures from examples (tileable, inpainted, PBR sets): synthesize_texture
(jasper://docs/texture-synthesis). GPU fluid: create_entity type fluid_simulation
(jasper://docs/fluid). Bend/squash: edit_lattice (jasper://docs/lattice).
Shadows: the light's shadow fields (biases in shadow texels: keep 1), the scene's
shadows_quality and shadow_distance, staticShadowCaster on meshes that never move; verify with
get_shadow_stats (jasper://docs/shadows).

## Build and ship
Scenes in Build order and the start scene live in project.yaml (project tools). Export
(Build Settings) produces a standalone CMake project or builds players per platform (a long
job, see The loop). The 2D/3D physics switches strip backends from the export
(jasper://docs/cpp-scripting). Before exporting: every scene saved, Console clean in play, the
start scene correct.
)JE"
R"JE(
## HTML/CSS UI
Menus, HUDs and dialogs in standard HTML + CSS, logic in Lua (no JavaScript): create_ui_document
(templates) writes assets/ui/<name>.html, .css and Lua; edit them yourself. inspect_ui (boxes in
canvas px = send_input coords) and assert_ui check layouts; import_files brings fonts, icons and
videos; effect(<name>, ...) draws GPU effects (list_ui_effects). Edit mode runs no Lua. First read
jasper://docs/html-ui.

## Pitfalls
- Edits made while playing are rolled back when play stops (as in Unity), then offered back
  (get_play_changes): stop before editing the scene for real.
- Save after each step: Auto Save keeps backups only (degraded_document, recovery,
  project_locked: jasper://docs/data-safety).
- Use only engine APIs you have confirmed in jasper://docs or the engine API snapshot
  (.jasperengine/engine-api); never carry over names from Unity C#, Godot or Unreal.
- Lua: static functions use a dot (GameObject.instantiate), methods a colon (go:setScale(2)).
)JE";
}

} // namespace jasperengine::editor::mcp
