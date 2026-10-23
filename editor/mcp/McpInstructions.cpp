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
scripts. Every edit you make through tools is an undoable editor command, visible live to a human
watching. Build scenes with tools, write code with your own file tools, verify by playing,
screenshots and the Console. tools/list is authoritative for tool names and arguments; read a
tool's description before first use. Deeper reference: resources jasper://docs/... (Lua API,
C++ scripting, physics, shaders, prefabs, HTML/CSS UI, Unity glossary).

## Unity names -> engine names
Use Unity terms in prose; tools, component names and script APIs use the engine names.
GameObject = entity (Entity id; script class GameObject). Create Empty = type empty.
Prefab = .prefab asset (PrefabComponent on the instance root; PrefabAsset in scripts).
Mesh Filter/Renderer = MeshComponent (Primitive for box/sphere/plane...). Sprite Renderer =
SpriteComponent. Rigidbody / Rigidbody 2D = RigidbodyComponent (Jolt) / Rigidbody2DComponent
(Box2D). Collider = an entry of the Rigidbody's colliders list, not a separate component.
Most other components keep Unity's name plus "Component" (AudioSourceComponent,
ParticleSystemComponent, Light2DComponent...).
Build Settings = export; Scenes in Build = the scene list in project.yaml.

## Addressing
- scene: numeric id ("12" ok), scene name, or project-relative .scene path. Omitted = the selected
  scene. While playing, runtime-state tools resolve to the playing scene.
- entity: numeric id ("12" ok), or a path of names from the scene root joined by "/" with "#N" for the
  Nth earlier sibling of the same name ("Player/Gun", "Enemy#2"), or a bare name unique in the
  scene; {"id"|"name"|"path"} objects (as results return them) work too. Ambiguous names fail,
  listing the paths. Results return both id and path; prefer ids within a session.
  `entities` (array) instead of `entity` applies a tool to several GameObjects.
- component: lowercase catalog names (transform, mesh, sprite, rigidbody2d, ...); unknown
  names fail with the valid list. property: catalog paths ("position",
  "submeshes[0].material.baseColorFactor", "scripts[0].speed").
- values: vectors [x,y,z] (or {x,y,z}); quaternions [x,y,z,w] or {"euler":[x,y,z]} degrees;
  colors as vectors (0..1); enums by name; textures/fonts/materials by asset path;
  entity references by entity ref, or {"scene": <scene>, "id": <id>} for a GameObject of
  another scene (cross-scene references); Prefab script properties by .prefab path.
- assets: project-relative paths only ("assets/sprites/ship.png", "scripts/Player.lua").

## The loop
1. Inspect: `editor_status`, the scene list, the hierarchy, then the GameObjects you will
   touch.
2. Edit scenes, prefabs and settings with tools, never by hand-editing .scene/.prefab/
   project.yaml (the editor would overwrite you). Group many edits in one `batch` call
   (undo_group: one undo step). Edit code with your own tools (see Code).
3. Save the scenes you changed.
4. Play the start scene; a C++ project builds first (seconds to minutes).
5. Look: screenshot the Game view (Scene view for layout). Drive the game with
   send_input / wait; query runtime state while playing (get_runtime_entity, eval_lua).
6. Read the Console since your last cursor: compile, script and Lua errors, missing assets.
7. Stop, fix, repeat until the Console is clean and the screenshot shows what you intended.
   Never claim a script works without playing it and reading the Console.
Long operations (play with a C++ build, export, build players) take wait/timeout_seconds;
if they outlive the timeout you get a job id: poll get_job, cancel with cancel_job.
A tool error says what was wrong and lists valid choices: fix the arguments, don't repeat.

## Code (Lua, C++, shaders, materials)
The project is a normal folder: `editor_status` gives its absolute path (project.path). Work
in it (cd there) and read, edit, grep and git its scripts, shaders and .material files with
your own tools. Engine API to grep: .jasperengine/engine-api/ (C++ headers) and its lua/
folder (Lua API stubs). Your edits are picked up before your next tool call (script
properties re-parsed, open buffers reloaded, shader forks recompiled, textures reloaded); that
call's result then carries `external_changes` {changed, problems: [{path, line, message}]}:
fix those problems first. During play they wait until play stops.
Validate without playing: check_script (Lua syntax, properties), check_shader, compile_scripts
(C++), or `jasperengine-editor check` from a shell. Through tools: create_script (engine
template, optional attach), attach_script / detach_script, set_script_properties. write_file /
read_file / search_files are for clients without file access; sync_files forces a full rescan.

## Project layout
project.yaml (name, canvas size, scenes, start scene, script dirs, C++ standard, standalone
prefabs), *.scene, *.prefab, assets/ (assetsDir), Lua scripts (luaDir), ProjectBuild.cmake.
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
GameObject. Declare the module `---@class Name : ScriptInstance` (Name = the file name): lua-language-server
then types self and its properties and catches typos.

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
  Engine.deltatime. Calling a non-existent method fails at runtime with "attempt to call a nil
  value". Check jasper://docs/lua-api (an index: read only the sections you need), the Lua
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
- play: start, stop, pause, resume, step (frames), restart. Clock options with any action, or
  alone with action "set": time_scale (slow motion / fast forward), fixed_fps (every frame
  advances exactly 1/fixed_fps s: reproducible runs), auto_pause (the game is frozen between
  your calls and runs only inside wait, send_input or step).
- wait: run for seconds, game_seconds or frames, or until a Lua condition is truthy; input
  sends send_input events while it waits (hold a key for 2 game seconds, then check).
- run_build runs an exported desktop player: input events, screenshots_at (seconds since
  launch); returns its errors (Lua ones too) and the log tail.

## 2D and 3D
- 2D: sprites, tilemaps, UI; units are points (pixels of the canvas, project canvasWidth x
  canvasHeight, y up). Box2D converts with pointsToMeterScale (64 points per meter). Light 2D
  adds to the scene's 2D ambient (lower ambient_light_2d_intensity to see lights); Shadow Caster 2D casts
  when the light's shadows are on.
- 3D: meshes (box, sphere, plane, model files), lights, skybox, fog, terrain. Cameras look at
  their target by default (useTarget): the GameObject's rotation is ignored until you call
  disableTarget() (orbiting: jasper://docs/lua-api, Camera).
- UI (text, image, button, panel, scrollbar, progressbar, input field, layout group; HTML/CSS
  UI: below): create_ui lays out in pixels with y DOWN from the parent's top-left, in 2D scenes
  and UI scenes alike, but in a 2D scene layout groups stack bottom-up and pointer input needs a
  UI camera: put HUDs and menus in a UI scene attached as a child scene (set_child_scene).

## Physics
- Add a body and its collider to the SAME GameObject in one step (edit_colliders with
  body_type + add). A Rigidbody without a Collider does not collide. Dynamic for
  actors, static for floors and level geometry, kinematic for scripted movers.
- Collider sizes are local to the mesh and scaled by the GameObject's scale: never pre-divide
  or pre-multiply by scale. Do not guess sizes: omit them to fit the mesh bounds, or read the
  measured mesh bounds.
- Mass comes from collider volume/area x density (3D default 1000, water; 2D 1 per square
  meter), never set directly. Size forces as mass x acceleration; if movement needs huge
  forces, lower the density instead.
- Continuous forces and torques go in onFixedUpdate, never onUpdate, and are NOT multiplied by
  deltatime. Rigidbody (3D): applyForce(f), applyForce(f, point), applyTorque(t), applyImpulse;
  mass is the property body.mass. Rigidbody2D: applyForceToCenter(f, true), applyForce(f, point,
  true), applyLinearImpulseToCenter(i, true); mass is body:getMass().
- Never move a body by writing its GameObject's transform from onFixedUpdate (silently
  overwritten). Teleport with body.position (and body.rotation / body.angle), velocity with
  body.linearVelocity. One-shot impulses fire once per press, not every held frame.
- Layer Collision Matrix (project settings, per backend) gates contacts; raycasts take an
  optional layer mask.

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
post_processing.<field>. Mobile: in-shader colour, low render scale, few passes
(jasper://docs/post-processing).

## Build and ship
Scenes in Build order and the start scene live in project.yaml (project tools). Export
(Build Settings) produces a standalone CMake project or builds players per platform; it is a
long job (wait/timeout_seconds, get_job). The 2D/3D physics switches strip backends from the
export (jasper://docs/cpp-scripting). Before exporting: every scene saved, Console clean in play, the start scene correct.
)JE"
R"JE(
## HTML/CSS UI
Menus, HUDs and dialogs in standard HTML + CSS, logic in Lua (no JavaScript): create_ui_document
(template: blank, main_menu, hud, inventory, settings, shop...) adds the GameObject and writes
assets/ui/<name>.html, its .css and Lua. Edit them yourself; fix what external_changes reports.
inspect_ui (tree, boxes in canvas px = send_input coords, styles, at) and assert_ui (text_fits,
no_overlap, within_viewport...) check layouts; import_files (fonts, icons, videos, from_document)
brings remote resources; effect(<name>, ...) draws GPU effects and simulations (list_ui_effects,
set_ui_effect_time).
Edit mode runs no Lua. First read jasper://docs/html-ui (with the UI tools' rules).

## Pitfalls
- Unsaved scenes are lost when the editor closes; save after each working step.
- Edits made while playing are discarded when play stops (as in Unity): stop before editing
  the scene for real.
- Use only engine APIs you have confirmed in jasper://docs or the engine API snapshot
  (.jasperengine/engine-api); never carry over names from Unity C#, Godot or Unreal.
- Lua: static functions use a dot (GameObject.instantiate), methods a colon (go:setScale(2)).
)JE";
}

} // namespace jasperengine::editor::mcp
