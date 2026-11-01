// The agent guide: sent as MCP initialize.instructions on every session and served as the
// resource jasper://guide. Keep it tight (it costs tokens on every session); deeper reference
// lives in docs/mcp/guide/*.md (served as jasper://docs/<file name>). Every tool and parameter
// name it mentions must exist.
//
// BUDGET: 16384 bytes, hard (tools/mcp/tests/test_protocol.py::test_initialize and others).
// Restructured to ~15 KB to leave headroom: a feature adds at most ONE short line here (usually
// under "Feature guides": "<what>: <entry tools> (jasper://docs/<name>)"); its details go to
// docs/mcp/guide. Only rules that prevent mistakes belong here; tighten before adding.
//
// MSVC limits one string literal piece to ~16 KB, so the text is split into adjacent raw
// literals (concatenated by the compiler). Keep every chunk well under 16000 bytes.

namespace jasperengine::editor::mcp {

const char* mcpServerInstructions() {
    return
R"JE(# JasperEngine editor (MCP)

You are driving a running JasperEngine editor: a C++ game engine with a Unity-style editor
and Lua or C++ gameplay scripts. Every tool edit is an undoable command, shown live to a human.
Build scenes with tools, write code with your own file tools, verify by playing, screenshots
and the Console. tools/list is authoritative; read a tool's description before first use.
Reference: jasper://docs (the index) and the jasper://docs/<name> pages named below.

## Unity names -> engine names
Use Unity terms in prose; tools, components and script APIs use the engine names
(jasper://docs/unity-glossary). GameObject = entity (Entity id; script class GameObject);
Create Empty = type empty. Prefab = .prefab asset (PrefabComponent on the instance root;
PrefabAsset in scripts). Mesh Filter/Renderer = MeshComponent (Primitive for box/sphere/
plane...); Sprite Renderer = SpriteComponent. Rigidbody / Rigidbody 2D = RigidbodyComponent
(Jolt) / Rigidbody2DComponent (Box2D); a Collider is an entry of the Rigidbody's colliders
list, not a component. Most others: Unity's name + "Component" (AudioSourceComponent...).
Legacy Animation = AnimationPlayerComponent (AnimationComponent: entity sequences).

## Addressing
- scene: numeric id ("12" ok), scene name, or project-relative .scene path. Omitted = the
  selected scene; while playing, runtime-state tools resolve to the playing scene.
- entity: numeric id ("12" ok), a path of names from the scene root joined by "/" with "#N"
  for the Nth earlier sibling of the same name ("Player/Gun", "Enemy#2"), or a name unique in
  the scene; {"id"|"name"|"path"} objects (as results return them) work too. Ambiguous names
  fail listing the matches; prefer the returned ids within a session. `entities` (array)
  applies a tool to several GameObjects. parent omitted = scene root; max_width 0 = full size.
- component: lowercase catalog names (transform, mesh, sprite, rigidbody2d...); unknown names
  fail with the valid list. property: catalog paths ("position",
  "submeshes[0].material.baseColorFactor", "scripts[0].speed").
- values: vectors [x,y,z] (or {x,y,z}); quaternions [x,y,z,w] or {"euler":[x,y,z]} degrees;
  colors as vectors (0..1); enums by name; textures/fonts/materials by asset path; entity
  references by entity ref, or {"scene": <scene>, "id": <id>} for another scene's GameObject;
  Prefab script properties by .prefab path.
- assets: project-relative paths only ("assets/sprites/ship.png", "scripts/Player.lua");
  writes refuse links out of the project.

## The loop
1. Inspect: `editor_status`, the scene list, the hierarchy, the GameObjects you will touch.
2. Edit scenes, prefabs and settings with tools, never by hand-editing .scene/.prefab/
   project.yaml (the editor would overwrite you). Group many edits in one `batch` call
   (undo_group: one undo step). Edit code with your own tools (see Code).
3. Save the scenes you changed.
4. Play the start scene; a C++ project builds first.
5. Look: screenshot the Game view (Scene view for layout). Drive the game with
   send_input / wait; query runtime state while playing (get_runtime_entity, eval_lua).
6. Read the Console since your last cursor: compile, script and Lua errors, missing assets.
7. Stop, fix, repeat until the Console is clean and the screenshot shows what you intended.
   Never claim a script works without playing it and reading the Console.
Long operations (C++ play builds, exports, players) take wait/timeout_seconds; past it a job
id (poll get_job, cancel_job). A tool error says what was wrong and lists valid choices: fix
the arguments, don't repeat.

## Code (Lua, C++, shaders, materials)
The project is a normal folder (`editor_status` project.path): edit, grep and git its
scripts, shaders and .material files with your own tools; engine API to grep:
.jasperengine/engine-api/ (C++ headers; lua/: Lua stubs). Edits are picked up before your
next tool call, whose result carries `external_changes` {changed, problems: [{path, line,
message}]}: fix those first (during play they wait). Validate without playing: check_script
(Lua), check_shader, compile_scripts (C++), `jasperengine-editor check`. Tools: create_script
(template, optional attach), attach_script / detach_script, set_script_properties;
write_file / read_file / search_files for clients without file access; sync_files rescans
(jasper://docs/editor-integration).

## Project layout
project.yaml (name, canvas size, scenes, start scene, script dirs, C++ standard, standalone
prefabs), *.scene, *.prefab, assets/ (assetsDir), Lua scripts (luaDir), ProjectBuild.cmake.
A .scene belongs to the project only while project.yaml lists it (Scenes in Build): add one
created outside the editor before opening, playing or building it. .jasperengine/ and the
root CMakeLists.txt are regenerated on every play/build: never edit them.
)JE"
R"JE(
## Lua scripts
A Lua script returns a module table, attached through the GameObject's Script component.
self.gameObject is its GameObject, self.scene the Scene, self.entity a numeric id (NOT an
object: never call self.entity:...). Wrap it: Sprite(self.scene, self.entity), Text(...),
AudioSource(...); a constructor given only self.scene CREATES a new GameObject. Declare
`---@class Name : ScriptInstance` (Name = the file name): LuaLS then types self.

```lua
---@class Player : ScriptInstance
local Player = {
    properties = {
        { name = "speed", displayName = "Speed", type = "float", default = 300.0 },
        { name = "label", displayName = "Label", type = "Text" },  -- GameObject reference
        { name = "bullet", displayName = "Bullet", type = "Prefab" },  -- PrefabAsset
    }
}

function Player:init()
    self.go = self.gameObject
    RegisterEngineEvent(self, "onUpdate")
end

function Player:onUpdate()
    local dx = Input.getAxis("Horizontal")  -- keys, gamepad, on-screen stick
    local p = self.go.position  -- a copy: assign it back
    self.go.position = Vector3(p.x + dx * self.speed * Engine.deltatime, p.y, p.z)
    if Input.getButtonDown("Jump") then  -- pressed this frame (space, pad Y)
        GameObject.instantiate(self.bullet, self.scene, self.go.position, Quaternion.IDENTITY)
    end
end

return Player
```
- Property types: bool, int, float, string, vector2/3/4, color3/color4, prefab, entity, a
  wrapper class (Sprite, Text, Button, AudioSource...) or another Lua script's class name
  (that script's self); read as self.<name>.
- Events: RegisterEngineEvent(self, "onUpdate" | "onFixedUpdate" | "onPostUpdate" |
  "onKeyDown" | "onTouchStart"...); define the method first. Component events:
  RegisterEvent(self, Button(self.scene, self.entity):getButtonComponent().onPress,
  "onPress"). Unregister* is optional (automatic on destroy).
- Lifecycle by name: init (Awake: first activation, even disabled), onEnable, start,
  onDisable, onDestroy; self.enabled; obj:getScript("Enemy") (lua-api 1.1). Survive loads:
  DontDestroyOnLoad(self); singleton: if GameObject.persistSingleton(self)~=self then return end
- Bound getters are often PROPERTIES: go.position, sprite.width, body.mass, Engine.deltatime
  ("attempt to call a nil value": no such method). Pass every argument (C++ default args do
  not apply). Unseen names: jasper://docs/lua-api (an index), the stubs, search_engine_api.
- Input.getAxis/getButtonDown("Jump") read Project Settings > Input actions; getKeyDown(key)
  is an edge, isKeyPressed held. Test with send_input action/axis events (jasper://docs/input).
- Log.print/warn/error take ONE string (use ..). Time.deltaTime/unscaledDeltaTime; waits:
  self:startCoroutine(fn) + Coroutine.wait; timers: self:invoke.
- Saves: PlayerPrefs, Json, File.writeText("data://x") (lua-api 2.4; tool user_settings).
  HTTP: Http.get/post, main-thread callbacks (jasper://docs/networking).
- No Dori.Script, getComponent or setProperty in Lua; no editor property paths
  (submeshes[0]...) in scripts.

## C++ scripts
Two kinds (create_script): a ScriptBase subclass, or a subclass of the GameObject's own class
(Mesh, Sprite...) to call its methods on `this`. No reflection macros (GENERATED_BODY,
GDCLASS): inherit and declare a (jasperengine::Scene*, jasperengine::Entity) constructor;
fields: DPROPERTY("Speed") (DPROPERTY("Tint", Color4) for colors). Includes are flat and
quoted ("ScriptBase.h", "Mesh.h"), never <core/...>. Lifecycle as in Lua.
REGISTER_ENGINE_EVENT(onUpdate) in the constructor or init, UNREGISTER_* in the destructor
(also REGISTER_BUTTON/UI/COMPONENT_EVENT). Log::print("x %f", x), never printf/std::cout;
keys D_KEY_*. Angles are DEGREES: Quaternion(angle, axis); std::sin/cos want radians
(Angle::defaultToRad). Script directories are include roots compiled whole (a build runs only
with a C++ script attached); build settings go in ProjectBuild.cmake. More:
jasper://docs/cpp-scripting.

## Play and time
- play: start, stop, pause, resume, step (frames), restart; clock options with any action
  (or "set"): time_scale, fixed_fps (exactly 1/fixed_fps s a frame: reproducible),
  auto_pause (frozen between your calls; runs only inside wait, send_input or step).
- wait: seconds, game_seconds, frames or until a Lua condition is truthy; its input sends
  send_input events meanwhile.
- run_build runs an exported desktop player (input events, screenshots_at) and returns its
  errors (Lua too) and log tail.
- Simulate focus, pause, rotation, language: jasper://docs/platform-services.

## 2D, 3D and UI
- 2D units are points (canvas pixels, y up), 64 per Box2D meter (pointsToMeterScale). Light
  2D adds to the 2D ambient (lower ambient_light_2d_intensity); Shadow Caster 2D needs the
  light's shadows on; normal maps (generate_sprite_normals, jasper://docs/sprite-normals)
  need its height > 0.
- Cameras look at their target by default (useTarget): the GameObject's rotation is ignored
  until disableTarget() (orbiting: jasper://docs/lua-api, Camera).
- Native UI (create_ui) lays out in pixels with y DOWN from the parent's top-left, but in a
  2D scene layout groups stack bottom-up and pointer input needs a UI camera: put HUDs and
  menus in a child UI scene (set_child_scene). Fade/disable/click-through a subtree: Canvas
  Group (jasper://docs/ui); onscreen_stick/onscreen_button/touch_area feed Input actions;
  pointer callbacks: lua-api 1.7.

## Physics
- Add a body and its collider to the SAME GameObject in one step (edit_colliders with
  body_type + add); a Rigidbody without a Collider does not collide. Dynamic: actors;
  static: floors, levels; kinematic: scripted movers.
- Collider sizes are mesh-local, scaled by the GameObject's scale: never pre-scale them;
  omit sizes to fit the mesh bounds (never guess).
- Mass = collider volume/area x density (default 1000 in 3D, 1 in 2D) unless set explicitly.
  Size forces as mass x acceleration; if huge, lower density.
- Continuous forces / torques go in onFixedUpdate (not onUpdate), NOT multiplied by
  deltatime. Never write a body's transform in onFixedUpdate (overwritten): teleport with
  body.position (rotation / angle), set body.linearVelocity. One-shot impulses: once per press.
- Collisions: onCollisionEnter / onTriggerEnter(2D) on the script; filters: Layer Collision
  Matrix, Physics.ignoreCollision; raycasts take a layer mask. 3D and 2D APIs differ (2D
  calls take a wake flag); joints (set_joint), create_ragdoll, place_with_physics, physics
  debug: jasper://docs/physics.

## Prefabs
- An edit on a prefab instance is an override of that instance only: Apply pushes it to the
  asset, or edit the asset in Prefab Mode. Instances cannot be restructured (reparent,
  reorder, move out): open the prefab instead. Variants are prefabs based on another prefab.
- Runtime: GameObject.instantiate(prefabAsset [, scene] [, position, rotation] [, parent])
  returns "<name>(Clone)", scripts initialized; GameObject.destroy(go [, delay]). Get the
  PrefabAsset from a Prefab script property; one loaded by path (PrefabAsset.load) must be
  marked standalone to be in the build (jasper://docs/prefabs).

## Shaders and materials
Library first: list_shaders ("library:<name>"; with name: its parameters).
A mesh shader goes on a submesh material (set_properties submeshes[0].material.shader, or a
.material file: create_material / update_material) with .shaderParams.<member> (colours
linear, "#RRGGBB" is sRGB), .shaderTextures.<uniform> and .shaderFeatures; UI, Points, Lines
and Sky take customShader plus shaderUniforms. Outlines and stencil effects:
apply_material_preset (list_material_presets). Shaders with scene_inputs (cost_note) add a
depth pass or colour copy per camera and must draw transparent; avoid on low-end mobile.
Fork only when nothing fits (fork_shader), read the fork first and keep its declarations,
includes, varyings and output; parameters go in its u_fs_customParams block
(set_shader_uniform). A failing shader draws as the built-in one (error in the Console).
Rules, globals (Lua Shader.setGlobal*): jasper://docs/shaders. Post-processing is on the main
camera only: apply_post_processing_preset, then set_scene_settings post_processing.<field>
(mobile advice: jasper://docs/post-processing).

## Feature guides
Read the page before using these tools:
- Clips (.anim, shared): edit_animation {clip, create, tracks [{path}]}; play with an
  Animation Player (jasper://docs/animation).
- Humanoid poses (.poseanim): get/edit/render_pose_animation, mirror_pose; degrees from
  standing, arms down (jasper://docs/pose-animation).
- Crowds: bake_vertex_animation (preview, then output) -> .vat on a Vertex Animator
  (jasper://docs/vertex-animation).
- Particles: create_particles, import_unity_particles (jasper://docs/particles).
- Lines, lasers, trails: line_renderer / trail_renderer (jasper://docs/lines-trails).
- Video: type video_player / video_background, texture video|<id>; MP4 and WebM
  (jasper://docs/video).
- Bend/squash: edit_lattice (jasper://docs/lattice). GPU fluid: type fluid_simulation
  (jasper://docs/fluid). Textures from examples: synthesize_texture
  (jasper://docs/texture-synthesis).

## HTML/CSS UI
Menus, HUDs and dialogs in standard HTML + CSS, logic in Lua (no JavaScript):
create_ui_document (templates) adds the GameObject and writes assets/ui/<name>.html, its .css
and Lua; edit them yourself and fix what external_changes reports. Check layouts with
inspect_ui (boxes in canvas px = send_input coords) and assert_ui; import_files brings remote
fonts, icons and videos; effect(<name>, ...) draws GPU effects (list_ui_effects). Edit mode
runs no Lua. First read jasper://docs/html-ui.

## Build and ship
Scenes in Build and the start scene live in project.yaml (project tools). Export (Build
Settings) writes a CMake project or builds players per platform (a long job). Before
exporting: every scene saved, Console clean in play, the start scene correct.

## Pitfalls
- Save after each working step: unsaved scenes are lost on close.
- Play-mode edits are lost when play stops: stop first.
- Lua: static functions use a dot (GameObject.instantiate), methods a colon (go:setScale(2)).
)JE";
}

} // namespace jasperengine::editor::mcp
