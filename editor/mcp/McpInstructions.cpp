// The agent guide: sent as MCP initialize.instructions on every session and served as the
// resource jasper://guide. It costs tokens on every session, so it holds only what an agent must
// know before it picks a tool (names, addressing, the loop, undo, code) and the rules agents broke
// without it; an area's detail lives in docs/mcp/guide/<area>.md (served as jasper://docs/<area>),
// which the guide's last section indexes. A line here needs a case of tools/mcp/eval that fails
// without it. Its limits are the "guide" entry of tools/mcp/budget.json (the whole
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

// The text before the tool groups section: what an agent needs before its first call. Clients may
// cut the instructions (Claude Code keeps 4096 characters), so this part, the generated groups line
// and the pointer to the whole guide come first; tool_budget.py --check gates it ("client_cut").
const char* headInstructions() {
    return
R"JE(# JasperEngine editor (MCP)

You drive a running JasperEngine editor (Unity-style: Hierarchy, Inspector, Console, Scene and Game
views; Lua or C++ scripts). Each tool edit is one undoable command, live to a human watching.
Area references: jasper://docs/<area> (the list: jasper://docs).
Text cut before "## Other areas"? read_file jasper://guide.

## Addressing
- scene: id, name or project-relative .scene path; omitted = the selected scene (while playing,
  runtime tools use the playing one). parent omitted = the scene root.
- entity: id, a path of names from the scene root ("Player/Gun"; "Enemy#2" = the 2nd of that name)
  or a unique name. Results return id and path: prefer ids. `entities` (array) applies to several.
- component: lowercase catalog names (transform, mesh, sprite, rigidbody2d...); property: catalog
  paths ("position", "submeshes[0].material.baseColorFactor", "scripts[0].speed").
- values: vectors [x,y,z]; quaternions [x,y,z,w] or {"euler":[x,y,z]} degrees; colours 0..1; enums by
  name; assets by project-relative path ("assets/sprites/ship.png") or GUID; another scene's
  GameObject {"scene": <scene>, "id": <id>}; Prefab script properties by .prefab path.

## The loop
1. Inspect: `editor_status`, the scenes, the GameObjects you will touch.
2. Edit scenes, prefabs and settings with tools, never by hand-editing .scene/.prefab/project.yaml;
   many edits: one `batch` (undo_group: one undo step). Save the scenes you changed.
3. Play the start scene; look: screenshot, get_runtime_entity / eval_lua, send_input / wait.
4. Read the Console since your last cursor (read_console since); fix and repeat. Never claim a
   script works without playing it and reading the Console.
A long call past timeout_seconds returns a job id: get_job. An error says what was wrong and lists
the valid choices: fix the arguments, don't repeat the call.

## Building scenes
Place what a scene uses in the editor; scripts reach it through script properties and spawn
only what can't be placed ahead, from Prefabs. What repeats is one Prefab (create_prefab);
motion is edit_animation, not per-frame code (jasper://docs/authoring).
)JE";
}

// The rest: the area digests and the index of the other areas, which the pointer leads to
const char* tailInstructions() {
    return
R"JE(
## Code
The project is a normal folder (`editor_status`: project.path): read, edit, grep and git its scripts,
shaders and .material files with your own tools (write_file / read_file are for clients without file
access). Your edits reload before your next call, whose result lists `external_changes` problems:
fix them first. Open scenes and prefabs never reload from disk. Engine API: .jasperengine/engine-api/
(C++ headers, lua/ stubs) and search_engine_api. Check without playing: check_script, check_shader,
compile_scripts (C++). .jasperengine/ and the root CMakeLists.txt are generated: never edit them
(build settings: ProjectBuild.cmake). A .scene is in the project once project.yaml lists it
(edit_build_scenes).

## Undo
Edit results carry `undo` {id, name}. undo/redo scope "timeline" = the human's Ctrl+Z; expect_top_id
undoes only your own last step. Play-time edits roll back at Stop (get_play_changes).

## Lua scripts
Read jasper://docs/lua-scripting before your first script. A script returns a module table
(`---@class Name : ScriptInstance`, Name = the file name); properties = { {name, displayName, type,
default} } are read as self.<name>. self.entity is a numeric id, NOT an object: wrap it
(GameObject(self.scene, self.entity), Sprite(...)). RegisterEngineEvent(self, "onUpdate") in init().
Getters are properties (go.position is a copy: assign it back; Engine.deltatime). Log.print takes one
string. No on_start, getComponent or setProperty. Static functions use a dot (GameObject.instantiate),
methods a colon (go:setScale(2)). Never carry over Unity, Godot or Unreal API names: confirm them.

## Unity names -> engine names
GameObject = entity (script class GameObject); Create Empty = type empty. Prefab = .prefab asset.
Mesh Filter/Renderer = MeshComponent (box, sphere, plane...); Sprite Renderer = SpriteComponent;
Rigidbody / Rigidbody 2D = RigidbodyComponent (Jolt) / Rigidbody2DComponent (Box2D); a Collider is an
entry of the body's colliders list, not a component. Legacy Animation = AnimationPlayerComponent.
Others: Unity's name + "Component". Build Settings = export; Scenes in Build = project.yaml's list.

## C++ scripts
Read jasper://docs/cpp-scripting first: a ScriptBase subclass with a (jasperengine::Scene*,
jasperengine::Entity) constructor, DPROPERTY("Speed") fields, flat quoted includes ("ScriptBase.h"),
REGISTER_ENGINE_EVENT(onUpdate), Log::print. Angles are degrees.

## Physics
Add a body and its collider in one step: edit_colliders with body_type + add (a Rigidbody without a
collider does not collide). Collider sizes are local: never pre-scale or guess them; omit them to fit
the mesh. Forces go in onFixedUpdate, not times deltatime; move a body through the body, not its
transform (jasper://docs/physics).

## Prefabs
An edit on an instance overrides that instance only: apply it to the asset or edit in prefab_mode.
A prefab only loaded by path must be standalone (set_standalone_prefab).

## Shaders and materials
Library first: list_shaders, then a submesh material's shader (create_material / update_material,
.shaderParams.<member>); apply_material_preset for outlines. fork_shader only when nothing fits, and
read the fork first (jasper://docs/shaders).

## HTML/CSS UI
Menus and HUDs are standard HTML + CSS with Lua logic (no JavaScript): create_ui_document (template
main_menu, hud, settings...) writes the files; edit them yourself; inspect_ui / assert_ui check
layouts. Read jasper://docs/html-ui first.

## Other areas
2D (points, y up; Box2D 64 points per meter), cameras (useTarget ignores rotation), animation
(edit_animation), post-processing (apply_post_processing_preset), atmosphere (Fog type height,
apply_atmosphere_preset), shadows (a light's softShadows, the scene's shadows_quality), reflections
(SSR replaces probe reflections; check with ssr_debug_mode 7), profiling (get_frame_times),
navigation (get_navigation_info), sound (analyze_audio), shipping (build_project), data safety (save
after each step; allow_data_loss), meshes (inspect_models, check_render_budgets), device tiers
(set_device_simulation, then play): jasper://docs/<area>.
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
    // A client only offers the model the tools it listed (Claude Code's tool search sees no others),
    // so the line says the agent enables a group itself
    return "\n## Tool groups\nTools outside core are not in your tool list until you call `tool_groups` "
           "{\"enable\": [\"<group>\"]} (no arguments: what each holds)" +
           (names.empty() ? std::string() : ": " + names) + ".\n";
}

} // namespace

const char* mcpServerInstructions() {
    // Built once the server registered its tools (the first initialize or jasper://guide read)
    static const std::string text = std::string(headInstructions()) + toolGroupsSection(McpServer::get().registry()) +
                                    tailInstructions();
    return text.c_str();
}

} // namespace jasperengine::editor::mcp
