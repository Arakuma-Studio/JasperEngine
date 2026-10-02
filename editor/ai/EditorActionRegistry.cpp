// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EditorActionRegistry.h"

#include "AiPathUtils.h"
#include "util/CxxStandards.h"

#include <cctype>
#include <filesystem>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace jasperengine::editor::ai {

namespace {

Json objectSchema(std::initializer_list<std::pair<const char*, Json>> properties,
                  std::initializer_list<const char*> required = {}) {
    Json props = Json::object();
    for (const auto& item : properties) {
        props[item.first] = item.second;
    }
    Json req = Json::array();
    for (const char* key : required) {
        req.push_back(key);
    }
    return {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties", props},
        {"required", req}
    };
}

Json objectSchemaFromProperties(Json properties, std::initializer_list<const char*> required = {}) {
    Json req = Json::array();
    for (const char* key : required) {
        req.push_back(key);
    }
    return {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties", std::move(properties)},
        {"required", req}
    };
}

// Unity's name for a create_entity type, for the approval card.
std::string unityTypeLabel(const std::string& type) {
    static const std::unordered_map<std::string, std::string> labels = {
        {"empty", "Empty"}, {"object", "Empty"}, {"box", "Cube"}, {"plane", "Plane"}, {"wall", "Wall"},
        {"mirror", "Mirror"}, {"sphere", "Sphere"}, {"cylinder", "Cylinder"}, {"capsule", "Capsule"},
        {"torus", "Torus"}, {"image", "Image"}, {"sprite", "Sprite"}, {"tilemap", "Tilemap"}, {"text", "Text"},
        {"button", "Button"}, {"scrollbar", "Scrollbar"}, {"progressbar", "Progress Bar"},
        {"inputfield", "Input Field"}, {"panel", "Panel"}, {"polygon", "Polygon"}, {"container", "Layout Group"},
        {"point_light", "Point Light"}, {"directional_light", "Directional Light"}, {"spot_light", "Spot Light"},
        {"light_2d", "Light 2D"}, {"shadow_caster_2d", "Shadow Caster 2D"}, {"joint2d", "Joint 2D"},
        {"joint3d", "Joint"}, {"rigidbody2d", "Rigidbody 2D"}, {"rigidbody", "Rigidbody"}, {"sky", "Skybox"},
        {"fog", "Fog"}, {"camera", "Camera"}, {"sound", "Audio Source"}, {"sound_3d", "3D Audio Source"},
        {"animation", "Animation"}, {"sprite_animation", "Sprite Animation"},
        {"position_tween", "Position Tween"}, {"rotation_tween", "Rotation Tween"},
        {"scale_tween", "Scale Tween"}, {"color_tween", "Color Tween"}, {"alpha_tween", "Alpha Tween"},
        {"model", "Model"}, {"particles", "Particle System"}, {"points", "Point Sprites"},
        {"lines", "Line Renderer"}, {"mesh_polygon", "Mesh Polygon"}, {"terrain", "Terrain"},
        {"reflection_probe", "Reflection Probe"}
    };
    const auto it = labels.find(type);
    return it != labels.end() ? it->second : type;
}

Json stringSchema(const std::string& description) {
    return {{"type", "string"}, {"description", description}};
}

Json numberSchema(const std::string& description) {
    return {{"type", "number"}, {"description", description}};
}

Json integerSchema(const std::string& description) {
    return {{"type", "integer"}, {"description", description}};
}

Json boolSchema(const std::string& description) {
    return {{"type", "boolean"}, {"description", description}};
}

Json vector2Schema(const std::string& description) {
    return {
        {"type", "object"},
        {"description", description},
        {"additionalProperties", false},
        {"properties", {
            {"x", numberSchema("X value")},
            {"y", numberSchema("Y value")}
        }}
    };
}

Json vector3Schema(const std::string& description) {
    return {
        {"type", "object"},
        {"description", description},
        {"additionalProperties", false},
        {"properties", {
            {"x", numberSchema("X value")},
            {"y", numberSchema("Y value")},
            {"z", numberSchema("Z value")}
        }}
    };
}

Json vector4Schema(const std::string& description) {
    return {
        {"type", "object"},
        {"description", description},
        {"additionalProperties", false},
        {"properties", {
            {"x", numberSchema("X value")},
            {"y", numberSchema("Y value")},
            {"z", numberSchema("Z value")},
            {"w", numberSchema("W value")}
        }}
    };
}

Json quaternionSchema(const std::string& description) {
    return {
        {"type", "object"},
        {"description", description},
        {"additionalProperties", false},
        {"properties", {
            {"w", numberSchema("W value")},
            {"x", numberSchema("X value")},
            {"y", numberSchema("Y value")},
            {"z", numberSchema("Z value")}
        }}
    };
}

Json propertyValueFields(std::initializer_list<std::pair<const char*, Json>> extra = {}) {
    Json fields = Json::object({
        {"bool_value", boolSchema("Boolean property value")},
        {"int_value", integerSchema("Integer/enum property value")},
        {"number_value", numberSchema("Float/double property value")},
        {"string_value", stringSchema("String property value. The asset-path property filename takes a project-relative path inside the assets directory (get_project_summary reports it as assets_dir), or an empty string to clear it. On a Font property it sets the main font and keeps the fallbacks")},
        {"font_paths", {
            {"type", "array"},
            {"description", "Font property value: project-relative font paths inside the assets directory, main font first and fallback fonts after it. Slots not listed are cleared"},
            {"items", {{"type", "string"}}}
        }},
        {"vector2_value", vector2Schema("Vector2 property value")},
        {"vector3_value", vector3Schema("Vector3 or Color3 property value")},
        {"vector4_value", vector4Schema("Vector4 or Color4 property value")},
        {"quat_value", quaternionSchema("Quaternion property value")},
        {"texture_path", stringSchema("Project-relative texture/resource path for Texture properties, inside the assets directory reported by get_project_summary as assets_dir. For .svg sources an optional '?svgScale=N' suffix sets the rasterization scale (e.g. 'ui/icon.svg?svgScale=2')")},
        {"entity_value", integerSchema("GameObject (entity) id for Entity or EntityReference properties")},
        {"entity_scene_id", integerSchema("Scene id for EntityReference properties. Omit to use scene_id")}
    });
    for (const auto& item : extra) {
        fields[item.first] = item.second;
    }
    return fields;
}

const std::vector<ToolDefinition>& cachedTools() {
    static const std::vector<ToolDefinition> definitions = {
        {
            "get_project_summary",
            "Read a compact summary of the current project, selected scene, selected GameObjects, asset (Project window) roots, and cxx_standard (" + cxxStandardList() + ").",
            objectSchema({}),
            true
        },
        {
            "search_resources",
            "Search project files (the assets shown in the Project window) by name and optional file type. Omit query (or pass an empty one) to browse: it then lists project files, narrowed by type when given. This only reads project paths.",
            objectSchema({
                {"query", stringSchema("Case-insensitive filename search text. Omit to list files instead of filtering")},
                {"type", stringSchema("Optional type: model, image, material, scene, script, audio, font, any")},
                {"max_results", integerSchema("Maximum number of paths to return, 1-50")}
            }),
            true
        },
        {
            "list_scene_entities",
            "List the GameObjects in a scene (what the Hierarchy window shows) by id/name/component summary.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")}
            }),
            true
        },
        {
            "inspect_entity",
            "Read one GameObject's name, selection state, Hierarchy data, components, and optionally all supported property values. For a mesh GameObject it also returns mesh_bounds: the measured local AABB (local_size/local_center) plus the accumulated world_scale and the resulting world_size. Use local_size to size anything expressed in mesh-local units, such as Colliders.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"include_properties", boolSchema("When true, include supported component property values")}
            }),
            true
        },
        {
            "inspect_component",
            "Read supported property names, types, and current values for one GameObject component (what the Inspector shows for it). For the Mesh component it also returns mesh_bounds with the measured local AABB and world size.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"component", stringSchema("Component name, e.g. Transform, Mesh, Camera, TerrainComponent")}
            }, {"component"}),
            true
        },
        {
            "list_component_types",
            "List component names accepted by add_component, remove_component, inspect_component, and set_component_property.",
            objectSchema({}),
            true
        },
        {
            "search_engine_api",
            "Search authoritative JasperEngine API symbols generated from Lua bindings and C++ headers. Use this before writing scripts or engine API code.",
            objectSchema({
                {"query", stringSchema("API symbol, class, method, property, or behavior to search for, e.g. Mesh setColor")},
                {"parent", stringSchema("Optional class/enum parent filter, e.g. Mesh")},
                {"max_results", integerSchema("Maximum number of symbols to return, 1-50")}
            }, {"query"}),
            true
        },
        {
            "create_entity",
            "Create an undoable GameObject in the selected scene using an allowed editor GameObject type.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"name", stringSchema("GameObject display name")},
                {"type", stringSchema("Allowed type: empty, gameobject, box, plane, wall, mirror, sphere, cylinder, capsule, torus, image, sprite, tilemap, text, button, scrollbar, progressbar, input_field, panel, polygon, layout_group, point_light, directional_light, spot_light, light_2d, shadow_caster_2d, joint2d, joint3d, rigidbody2d, rigidbody, skybox, fog, camera, audio_source, audio_source_3d, animation, sprite_animation, position_tween, rotation_tween, scale_tween, color_tween, alpha_tween, model, particle_system, points, line_renderer, mesh_polygon, terrain, reflection_probe. Unity names for these types: object = Create Empty (a GameObject with only a Transform), empty = a bare GameObject without a Transform (scene root only), box = Cube, inputfield = Input Field, container = Layout Group, rigidbody2d = Rigidbody 2D, rigidbody = Rigidbody, sky = Skybox, sound/sound_3d = Audio Source (2D/3D), particles = Particle System, lines = Line Renderer, position_tween/rotation_tween/scale_tween/color_tween/alpha_tween = Position/Rotation/Scale/Color/Alpha Tween. light_2d = 2D radius-falloff light (Light2DComponent); shadow_caster_2d = Shadow Caster 2D (ShadowCaster2DComponent).")},
                {"parent_id", integerSchema("Optional parent GameObject id for GameObjects that have a Transform")},
                {"position", vector3Schema("Optional local position")}
            }, {"name", "type"}),
            false
        },
        {
            "set_entity_transform",
            "Set a GameObject's Transform through the undoable command system.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"position", vector3Schema("Optional local position")},
                {"rotation", quaternionSchema("Optional local rotation quaternion")},
                {"scale", vector3Schema("Optional local scale")}
            }),
            false
        },
        {
            "rename_entity",
            "Rename a GameObject through the undoable command system.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"new_name", stringSchema("New GameObject name")}
            }, {"new_name"}),
            false
        },
        {
            "delete_entity",
            "Delete a GameObject through the undoable command system. A GameObject that comes from a Prefab "
            "is removed from that Prefab instance only (a removed GameObject override); the Prefab keeps it.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")}
            }),
            false
        },
        {
            "duplicate_entity",
            "Duplicate one GameObject through the undoable command system.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")}
            }),
            false
        },
        {
            "reparent_entity",
            "Move a GameObject that has a Transform under a new parent or the scene root in the Hierarchy, through MoveEntityOrderCmd.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id to move")},
                {"entity_name", stringSchema("GameObject name to move, used only when entity_id is omitted")},
                {"parent_id", integerSchema("New parent GameObject id. Use 0 or omit for scene root")},
                {"parent_name", stringSchema("New parent GameObject name, used only when parent_id is omitted")}
            }),
            false
        },
        {
            "move_entity_order",
            "Move a GameObject relative to another GameObject in Hierarchy order.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id to move")},
                {"entity_name", stringSchema("GameObject name to move, used only when entity_id is omitted")},
                {"target_id", integerSchema("Target GameObject id")},
                {"target_name", stringSchema("Target GameObject name, used only when target_id is omitted")},
                {"insertion", stringSchema("before, after, child_first, or child_last")}
            }, {"insertion"}),
            false
        },
        {
            "select_entities",
            "Replace the editor selection for a scene. Supports one GameObject, multiple entity_ids/entity_names, or clear.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Single GameObject id to select")},
                {"entity_name", stringSchema("Single GameObject name to select, used only when entity_id is omitted")},
                {"entity_ids", {
                    {"type", "array"},
                    {"description", "Multiple GameObject ids to select"},
                    {"items", {{"type", "integer"}}}
                }},
                {"entity_names", {
                    {"type", "array"},
                    {"description", "Multiple GameObject names to select"},
                    {"items", {{"type", "string"}}}
                }},
                {"clear", boolSchema("When true, clear selection")}
            }),
            false
        },
        {
            "add_component",
            "Add a component to a GameObject through AddComponentCmd (like Add Component in the Inspector).",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"component", stringSchema("Engine component name from list_component_types, e.g. Rigidbody2DComponent for a Rigidbody 2D or AudioSourceComponent for an Audio Source")}
            }, {"component"}),
            false
        },
        {
            "remove_component",
            "Remove a component from a GameObject through RemoveComponentCmd.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"component", stringSchema("Component name")}
            }, {"component"}),
            false
        },
        {
            "add_rigidbody_collider",
            "Add a Rigidbody (RigidbodyComponent) to an existing 3D GameObject when needed, then append one Collider (collider) to that SAME GameObject's Rigidbody. Use this for a model/mesh that needs physics or a Collider; do not create a separate Rigidbody GameObject. Existing Colliders are preserved. Sizes default to the mesh's measured bounds, which is the reliable way to match the visible geometry; only pass explicit sizes for axes that must deliberately differ from the mesh. The result reports fitted_to_mesh so you can tell a measured Collider from one left at its defaults. convex_hull is exact and cheap for boxy level geometry.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Model or mesh GameObject id that receives the RigidbodyComponent")},
                {"entity_name", stringSchema("Model or mesh GameObject name, used only when entity_id is omitted")},
                {"body_type", stringSchema("Optional Rigidbody type: static, kinematic, or dynamic. A new Rigidbody defaults to static when omitted")},
                {"motion_quality", stringSchema("Optional motion quality: discrete or linear_cast")},
                {"shape_type", stringSchema("Collider type: box, sphere, capsule, tapered_capsule, cylinder, convex_hull, or mesh (Mesh Collider). convex_hull and mesh take their geometry from the GameObject's mesh and ignore all size arguments; convex_hull is exact and cheap for boxy level geometry, mesh suits static concave geometry")},
                {"fit_to_mesh", boolSchema("Size the Collider from the GameObject mesh's local bounds and centre it on them. Defaults to true; any explicit size argument overrides the fitted value for that axis. Set false to keep the Collider defaults")},
                {"position", vector3Schema("Optional Collider local position (center) in GameObject-local units. Overrides the centre chosen by fit_to_mesh")},
                {"rotation", quaternionSchema("Optional Collider local rotation")},
                {"width", numberSchema("Box width in mesh-local units, i.e. before the GameObject's transform scale, which the engine applies itself; defaults to 1")},
                {"height", numberSchema("Box height in mesh-local units; defaults to 1")},
                {"depth", numberSchema("Box depth in mesh-local units; defaults to 1")},
                {"radius", numberSchema("Sphere, capsule, or cylinder radius in mesh-local units; defaults to 1")},
                {"half_height", numberSchema("Capsule or cylinder half-height in mesh-local units; defaults to 0.5. A capsule is 2*(half_height+radius) tall")},
                {"top_radius", numberSchema("Tapered capsule top radius in mesh-local units; defaults to 0.5")},
                {"bottom_radius", numberSchema("Tapered capsule bottom radius in mesh-local units; defaults to 0.5")},
                {"density", numberSchema("Collider density in kg/m3 for dynamic Rigidbodies; defaults to 1000 (water). Mass is volume * density, so the default makes a radius-1 sphere weigh about 4189 kg and a 1x1x1 box 1000 kg. Pick a density that yields the mass the behaviour needs, so scripted forces stay plausible")}
            }, {"shape_type"}),
            false
        },
        {
            "add_rigidbody2d_collider",
            "Add a Rigidbody 2D (Rigidbody2DComponent) to an existing 2D GameObject when needed, then append one Collider 2D (collider) to that SAME GameObject's Rigidbody 2D. Use this for a sprite, tilemap, or 2D mesh that needs physics or a Collider 2D; do not create a separate Rigidbody GameObject. Existing Colliders are preserved.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Sprite, tilemap, or 2D mesh GameObject id that receives the Rigidbody2DComponent")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"body_type", stringSchema("Optional Rigidbody 2D type: static, kinematic, or dynamic. A new Rigidbody 2D defaults to static when omitted")},
                {"shape_type", stringSchema("Collider 2D type: box, circle, capsule, segment (Edge Collider 2D), polygon, or chain (open or looped Edge Collider 2D)")},
                {"center", vector2Schema("Optional local center for a box or circle; defaults to 0,0")},
                {"width", numberSchema("Box width; required for box")},
                {"height", numberSchema("Box height; required for box")},
                {"radius", numberSchema("Circle or capsule radius; required for circle and capsule")},
                {"point_a", vector2Schema("First local point for capsule or segment")},
                {"point_b", vector2Schema("Second local point for capsule or segment")},
                {"points", {
                    {"type", "array"},
                    {"description", "Local points for polygon (at least 3) or chain (at least 4). A chain collides on one side only: counter-clockwise points collide outside, clockwise inside (hollow containers)"},
                    {"items", vector2Schema("Point")}
                }},
                {"loop", boolSchema("Whether a chain closes back to its first point; defaults false. An open chain doesn't collide on its first and last edges")},
                {"density", numberSchema("Collider density; defaults to 1, NOT the 1000 that 3D Rigidbodies default to. 2D mass is area * density, with sizes given in points and scaled to metres by pointsToMeterScale2D (64 by default), so a 100x100 box at the default density weighs about 2.4 kg")},
                {"friction", numberSchema("Collider friction; defaults to 0.6")},
                {"restitution", numberSchema("Collider restitution/bounciness, 0-1; defaults to 0")}
            }, {"shape_type"}),
            false
        },
        {
            "set_component_property",
            "Set one supported component property (an Inspector field) through PropertyCmd. Use the typed value field matching the property type.",
            objectSchemaFromProperties(propertyValueFields({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"component", stringSchema("Component name")},
                {"property", stringSchema("Property path from inspect_component, e.g. position or submeshes[0].material.baseColorTexture")}
            }), {"component", "property"}),
            false
        },
        {
            "set_texture_settings",
            "Set sampler settings and/or SVG scale of a Texture component property; only the provided fields change, the texture source is kept. Filters: nearest, linear, nearest_mipmap_nearest, nearest_mipmap_linear, linear_mipmap_nearest, linear_mipmap_linear (mag_filter accepts only nearest or linear). Wraps: repeat, mirrored_repeat, clamp_to_edge, clamp_to_border.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"component", stringSchema("Component name")},
                {"property", stringSchema("Texture property path from inspect_component, e.g. texture or submeshes[0].material.baseColorTexture")},
                {"min_filter", stringSchema("Minification filter")},
                {"mag_filter", stringSchema("Magnification filter: nearest or linear")},
                {"wrap_u", stringSchema("Wrap mode along U")},
                {"wrap_v", stringSchema("Wrap mode along V")},
                {"svg_scale", numberSchema("Rasterization scale for .svg sources (e.g. 2.0 = twice the intrinsic size)")}
            }, {"component", "property"}),
            false
        },
        {
            "add_project_scene",
            "Add an existing .scene file found in the project folder to the project scene list (Scenes in Build), closed. A .scene file that is not listed is not built, included in a build, opened, or usable as a child scene. Scenes made with create_scene are already listed; use delete_scene to take one off the list.",
            objectSchema({
                {"scene_path", stringSchema("Safe project-relative .scene path")}
            }, {"scene_path"}),
            false
        },
        {
            "create_scene",
            "Create a new 2D, 3D, or UI scene in the project.",
            objectSchema({
                {"name", stringSchema("Scene name")},
                {"type", stringSchema("Scene type: 3d, 2d, or ui")}
            }, {"name", "type"}),
            false
        },
        {
            "rename_scene",
            "Rename a scene through SceneNameCmd.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"new_name", stringSchema("New scene name")}
            }, {"new_name"}),
            false
        },
        {
            "save_scene",
            "Save one scene. Omit scene_id to save the selected scene.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")}
            }),
            false
        },
        {
            "save_all_scenes",
            "Save all project scenes.",
            objectSchema({}),
            false
        },
        {
            "set_start_scene",
            "Set the project startup scene (the first scene in Scenes in Build).",
            objectSchema({
                {"scene_id", integerSchema("Scene id to use as startup scene")}
            }, {"scene_id"}),
            false
        },
        {
            "set_scene_property",
            "Set a supported scene-level property such as background_color, global_illumination_color, ssao_enabled, ssr_enabled, ambient_light_2d_color/ambient_light_2d_intensity (2D light ambient), shadows_quality (3D) and shadows_2d_quality (2D) as int_value 0=none/1=low/2=medium/3=high. Physics gravity in m/s^2: physics_gravity_2d (vector2_value, Box2D world) and physics_gravity_3d (vector3_value, Jolt world), default (0, -9.81). Fixed game resolution (main scene renders at that size and is upscaled to the view): fixed_resolution_enabled (bool_value), fixed_resolution_width/fixed_resolution_height (int_value, pixels), fixed_resolution_filter (int_value 0=nearest, 1=linear). Also sets a scene's default custom shader per type via default_mesh_shader/default_ui_shader/default_sky_shader/default_points_shader/default_lines_shader as string_value (project-relative fork base path, empty to reset to built-in) - used by every component of that type whose own customShader is empty (priority: component customShader > scene default > built-in). Use fork_shader with shader_type (no GameObject) to create the fork first.",
            objectSchemaFromProperties(propertyValueFields({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"property", stringSchema("Scene property name")}
            }), {"property"}),
            false
        },
        {
            "control_play_mode",
            "Start (Play), pause, resume, or stop Play mode for a scene.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"action", stringSchema("start, pause, resume, or stop")}
            }, {"action"}),
            false
        },
        {
            "add_child_scene",
            "Add a child scene reference through AddChildSceneCmd.",
            objectSchema({
                {"scene_id", integerSchema("Parent scene id. Omit to use selected scene")},
                {"child_scene_id", integerSchema("Child scene id")},
                {"start_active", boolSchema("Whether the child scene starts active. Defaults true")}
            }, {"child_scene_id"}),
            false
        },
        {
            "remove_child_scene",
            "Remove a child scene reference through RemoveChildSceneCmd.",
            objectSchema({
                {"scene_id", integerSchema("Parent scene id. Omit to use selected scene")},
                {"child_scene_id", integerSchema("Child scene id")}
            }, {"child_scene_id"}),
            false
        },
        {
            "set_child_scene_start_active",
            "Set a child scene's Start active flag through SetChildSceneStartActiveCmd.",
            objectSchema({
                {"scene_id", integerSchema("Parent scene id. Omit to use selected scene")},
                {"child_scene_id", integerSchema("Child scene id")},
                {"start_active", boolSchema("New Start active value")}
            }, {"child_scene_id", "start_active"}),
            false
        },
        {
            "load_child_scene_inline",
            "Load or unload a child scene inline for editing context.",
            objectSchema({
                {"child_scene_id", integerSchema("Child scene id")},
                {"load", boolSchema("true to load inline, false to unload")}
            }, {"child_scene_id", "load"}),
            false
        },
        {
            "create_terrain_heightmap",
            "Create or replace an undoable 16-bit heightmap for a terrain GameObject. Use mode 'middle' for the editor default flat midpoint map, 'flat' for a custom constant height, 'random_noise' for random heights, or 'fractal_noise' for smoother terrain.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Terrain GameObject id")},
                {"entity_name", stringSchema("Terrain GameObject name, used only when entity_id is omitted")},
                {"resolution", integerSchema("Square heightmap resolution, 2-2048. Defaults to 512")},
                {"mode", stringSchema("flat, middle, random_noise, or fractal_noise. Defaults to middle")},
                {"seed", integerSchema("Optional deterministic seed for noise modes")},
                {"base_height", numberSchema("Normalized base height 0-1. Defaults to 0.5 for middle/flat")},
                {"amplitude", numberSchema("Noise amplitude 0-1. Defaults to 0.35")},
                {"frequency", numberSchema("Noise frequency. Defaults to 4.0")},
                {"octaves", integerSchema("Fractal octave count, 1-8. Defaults to 4")}
            }),
            false
        },
        {
            "import_project_model",
            "Import an existing project-relative glTF/GLB/OBJ model into a 3D scene using ModelLoadCmd.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"model_path", stringSchema("Project-relative model path inside the assets directory (get_project_summary reports it as assets_dir), e.g. assets/models/dog.glb")},
                {"entity_name", stringSchema("Name for the created model GameObject")},
                {"position", vector3Schema("Optional drop position")}
            }, {"model_path"}),
            false
        },
        {
            "create_folder",
            "Create a folder inside the project (Project window) through CreateDirCmd.",
            objectSchema({
                {"parent_path", stringSchema("Safe project-relative parent directory. Empty means project root")},
                {"name", stringSchema("New folder name")}
            }, {"name"}),
            false
        },
        {
            "rename_resource",
            "Rename or move one project asset (a file or folder in the Project window) through RenameFileCmd, updating project references where supported.",
            objectSchema({
                {"path", stringSchema("Safe project-relative file or directory path")},
                {"new_name", stringSchema("New file or directory name, not a full path")}
            }, {"path", "new_name"}),
            false
        },
        {
            "delete_resource",
            "Delete one project asset (a file or folder in the Project window) through DeleteFileCmd, moving it to the project trash where supported.",
            objectSchema({
                {"path", stringSchema("Safe project-relative file or directory path")}
            }, {"path"}),
            false
        },
        {
            "import_resource_file",
            "Copy an existing local file into the project through CopyFileCmd. No shell is used.",
            objectSchema({
                {"source_path", stringSchema("Absolute local file path to import")},
                {"target_dir", stringSchema("Safe project-relative destination directory")}
            }, {"source_path", "target_dir"}),
            false
        },
        {
            "link_material",
            "Link a .material file to a mesh submesh through LinkMaterialCmd.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Mesh GameObject id")},
                {"entity_name", stringSchema("Mesh GameObject name, used only when entity_id is omitted")},
                {"material_path", stringSchema("Safe project-relative .material path")},
                {"submesh_index", integerSchema("Submesh index. Defaults to 0")}
            }, {"material_path"}),
            false
        },
        {
            "unlink_material",
            "Unlink a mesh submesh from a shared material file through UnlinkMaterialCmd.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Mesh GameObject id")},
                {"entity_name", stringSchema("Mesh GameObject name, used only when entity_id is omitted")},
                {"submesh_index", integerSchema("Submesh index. Defaults to 0")}
            }),
            false
        },
        {
            "create_material_file",
            "Create a .material file from a mesh submesh and link it back to that submesh.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Mesh GameObject id")},
                {"entity_name", stringSchema("Mesh GameObject name, used only when entity_id is omitted")},
                {"submesh_index", integerSchema("Submesh index. Defaults to 0")},
                {"target_dir", stringSchema("Safe project-relative directory for the material file")}
            }, {"target_dir"}),
            false
        },
        {
            "set_terrain_textures",
            "Assign terrain height/blend maps and painted layer maps through undoable property commands. Every path must be inside the assets directory (get_project_summary reports it as assets_dir). A layer blends only its color until layer_pbr turns it into a PBR layer, which brings its own normal, roughness, metallic and occlusion.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Terrain GameObject id")},
                {"entity_name", stringSchema("Terrain GameObject name, used only when entity_id is omitted")},
                {"heightmap_path", stringSchema("Optional project-relative heightmap texture path")},
                {"blendmap_path", stringSchema("Optional project-relative blendmap texture path")},
                {"detail_red_path", stringSchema("Optional project-relative color texture path for layer 1")},
                {"detail_green_path", stringSchema("Optional project-relative color texture path for layer 2")},
                {"detail_blue_path", stringSchema("Optional project-relative color texture path for layer 3")},
                {"layer_index", integerSchema("Painted layer to edit, 0-8. Required by the layer_* fields")},
                {"layer_pbr", boolSchema("True gives the layer its own surface maps")},
                {"layer_color_path", stringSchema("Optional project-relative layer base color texture path")},
                {"layer_normal_path", stringSchema("Optional project-relative layer normal map path")},
                {"layer_roughness_path", stringSchema("Optional project-relative layer roughness map path, read from green")},
                {"layer_metallic_path", stringSchema("Optional project-relative layer metallic map path, read from blue")},
                {"layer_occlusion_path", stringSchema("Optional project-relative layer occlusion map path, read from red")},
                {"layer_height_path", stringSchema("Optional project-relative layer height map path, used only for blending")}
            }),
            false
        },
        {
            "create_terrain_blendmap",
            "Create an undoable RGB terrain blendmap texture with constant channel values.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Terrain GameObject id")},
                {"entity_name", stringSchema("Terrain GameObject name, used only when entity_id is omitted")},
                {"resolution", integerSchema("Square blendmap resolution, 2-2048. Defaults to terrain heightmap setting")},
                {"red", numberSchema("Red channel 0-1")},
                {"green", numberSchema("Green channel 0-1")},
                {"blue", numberSchema("Blue channel 0-1")}
            }),
            false
        },
        {
            "add_terrain_collision",
            "Add a Rigidbody (RigidbodyComponent) to a terrain GameObject and configure a height-field Collider (Terrain Collider) when supported.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Terrain GameObject id")},
                {"entity_name", stringSchema("Terrain GameObject name, used only when entity_id is omitted")}
            }),
            false
        },
        {
            "create_script",
            "Create a Lua or C++ script file from a minimal template, attach it to a GameObject, and update ScriptComponent. Lua scripts are returned tables with init(), self.scene, and self.entity. C++ scripts use flat headers (\"Mesh.h\", \"ScriptBase.h\"); for mesh/cube behavior prefer cpp_subclass extending Mesh and unregister engine events in destructors.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"type", stringSchema("lua, cpp_subclass, or cpp_script_class")},
                {"class_name", stringSchema("Class/module name")},
                {"target_dir", stringSchema("Safe project-relative target directory. Defaults to scripts")}
            }, {"type", "class_name"}),
            false
        },
        {
            "attach_script",
            "Attach an existing Lua or C++ script to a GameObject's ScriptComponent as a new entry. Use it only for a script the GameObject does not have yet; to rename or repoint a script it already has, use update_script_entry.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"type", stringSchema("lua or cpp")},
                {"class_name", stringSchema("Class/module name")},
                {"path", stringSchema("Safe project-relative .lua or .cpp path")},
                {"header_path", stringSchema("Safe project-relative header path for C++ scripts")}
            }, {"type", "class_name", "path"}),
            false
        },
        {
            "update_script_entry",
            "Rename the class of a script a GameObject already has, or repoint it at other files. Use this after renaming a script file or its class; it edits the attachment in place instead of adding a second one. Entries come from inspect_component on ScriptComponent.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"index", integerSchema("Script entry index")},
                {"class_name", stringSchema("Current class name of the entry, used when index is omitted")},
                {"path", stringSchema("Current source or header path of the entry, used when index and class_name are omitted")},
                {"new_class_name", stringSchema("New class/module name")},
                {"new_path", stringSchema("New safe project-relative .lua or .cpp path")},
                {"new_header_path", stringSchema("New safe project-relative header path, C++ entries only")}
            }),
            false
        },
        {
            "remove_script_entry",
            "Detach one script from a GameObject's ScriptComponent. The script files are kept; delete them with delete_resource when they are no longer used.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"index", integerSchema("Script entry index")},
                {"class_name", stringSchema("Class name of the entry, used when index is omitted")},
                {"path", stringSchema("Source or header path of the entry, used when index and class_name are omitted")}
            }),
            false
        },
        {
            "update_script_file",
            "Replace an existing project script file with complete content and refresh attached ScriptComponent properties. Lua content must use JasperEngine table modules, not Dori.Script or editor property paths. For Mesh/Primitive color use Primitive(self.scene, self.entity) and setColor(1,0,0,1) or setColor(Vector4(1,0,0,1)). Choose update timing correctly in both Lua and C++: onUpdate is for variable-frame non-physics logic, while continuous forces and torques must use onFixedUpdate and must not be multiplied by delta time. Size forces from the Rigidbody's real mass, not from a guessed magnitude: 3D mass is Collider volume * density (default density 1000 kg/m3), while 2D mass is Collider area * density (default density 1), so never carry the 3D default over to a Rigidbody 2D. C++ content must use flat quoted headers (\"Mesh.h\", not <core/Mesh.h>), cpp_subclass + setColor for mesh GameObjects, REGISTER_ENGINE_EVENT with the appropriate update event, and matching destructor cleanup with UNREGISTER_ENGINE_EVENT.",
            objectSchema({
                {"path", stringSchema("Existing safe project-relative script path. Allowed extensions: .lua, .cpp, .h, .hpp")},
                {"content", stringSchema("Complete replacement file contents")},
                {"reason", stringSchema("Short reason shown in the approval preview")}
            }, {"path", "content"}),
            false
        },
        {
            "create_source_file",
            "Create a NEW C++ source or header that belongs to no GameObject: a helper class, a shared utility, or any library code scripts include. It never touches a ScriptComponent, so use create_script instead when the class is a script a GameObject runs. A source only reaches the build when it lives under a project script directory (get_project_summary reports script_dirs) or when an attached script includes it; the result says which case applies. Change the file afterwards with update_script_file.",
            objectSchema({
                {"path", stringSchema("New safe project-relative file path. Allowed extensions: .h, .hpp, .cpp")},
                {"content", stringSchema("Complete file contents")},
                {"reason", stringSchema("Short reason shown in the approval preview")}
            }, {"path", "content"}),
            false
        },
        {
            "set_project_script_dirs",
            "Replace the project C++ script directories. Each one becomes an include directory, and every source under it is compiled without a script component referencing it. Read the current list from get_project_summary (script_dirs) and pass the complete new list: entries it reported can be passed back verbatim, omitting one removes that root, and an empty list clears them all. A root you ADD must be a project-relative directory that already exists (create it with create_folder first); the project root itself and directories outside the project are added by the user in Project Settings.",
            objectSchema({
                {"directories", {
                    {"type", "array"},
                    {"description", "Complete list of safe project-relative directories to use as C++ script roots"},
                    {"items", {{"type", "string"}}}
                }}
            }, {"directories"}),
            false
        },
        {
            "set_project_cxx_standard",
            "Set the project's C++ language standard (" + cxxStandardList() + "). The next Play or build builds with it, and a build (Build Settings) compiles the engine with it too. Read the current value from get_project_summary (cxx_standard). Do not edit CMAKE_CXX_STANDARD in the generated CMakeLists.txt.",
            objectSchema({
                {"cxx_standard", {{"type", "integer"}, {"enum", cxxStandards},
                    {"description", "C++ standard year: " + cxxStandardList()}}}
            }, {"cxx_standard"}),
            false
        },
        {
            "update_project_build_file",
            "Write ProjectBuild.cmake at the project root: the optional CMake hook carrying extra include directories, libraries, and compile definitions. The generated CMakeLists.txt is rewritten on every build, so it is never the place for custom build settings and the user must never be asked to edit it. Attach settings to ${JASPERENGINE_TARGET} instead of a literal target name (it differs between the editor and player builds) and use ${JASPERENGINE_SCRIPTS_DIR} as the root of script paths. This replaces the whole file, so read the current one with read_resource_file first and keep the settings it already has.",
            objectSchema({
                {"content", stringSchema("Complete replacement contents of ProjectBuild.cmake")},
                {"reason", stringSchema("Short reason shown in the approval preview")}
            }, {"content"}),
            false
        },
        {
            "create_prefab_from_entity",
            "Create a Prefab (.prefab file) from a GameObject hierarchy through CreatePrefabCmd.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Root GameObject id")},
                {"entity_name", stringSchema("Root GameObject name, used only when entity_id is omitted")},
                {"prefab_path", stringSchema("Safe project-relative .prefab output path for the Prefab")}
            }, {"prefab_path"}),
            false
        },
        {
            "import_prefab_instance",
            "Instantiate a Prefab (.prefab file) into the scene through ImportPrefabCmd.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"prefab_path", stringSchema("Safe project-relative .prefab (Prefab) path")},
                {"parent_id", integerSchema("Optional parent GameObject id")}
            }, {"prefab_path"}),
            false
        },
        {
            "add_entity_to_prefab",
            "Insert a GameObject into an existing Prefab instance through AddEntityToPrefabCmd.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"prefab_root_id", integerSchema("Prefab instance root/parent GameObject id")}
            }, {"prefab_root_id"}),
            false
        },
        {
            "remove_entity_from_prefab",
            "Remove a GameObject from the Prefab it belongs to through RemoveEntityFromPrefabCmd.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")}
            }),
            false
        },
        {
            "apply_prefab_component",
            "Apply a Prefab instance's overrides of a component (its overridden properties, or the added or removed component) to the Prefab asset that owns the GameObject, through PrefabOverrideCmd. Edits on Prefab instances are per-instance overrides until applied.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"component", stringSchema("Component name")}
            }, {"component"}),
            false
        },
        {
            "revert_prefab_component",
            "Revert a Prefab instance's overrides of a component (its overridden properties, or the added or removed component) back to the Prefab's values through the command history.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"component", stringSchema("Component name")}
            }, {"component"}),
            false
        },
        {
            "set_standalone_prefab",
            "Add or remove a Prefab (.prefab) in the project standalone list. A Prefab no scene instantiates is only built and registered while it is listed, so scripts can load it by path with PrefabAsset.load and instantiate it with GameObject.instantiate (a Prefab a script property references is built without being listed). Prefabs a scene instantiates are always built and are not listed.",
            objectSchema({
                {"prefab_path", stringSchema("Safe project-relative .prefab (Prefab) path")},
                {"standalone", boolSchema("true to list the Prefab, false to remove it. Default true")}
            }, {"prefab_path"}),
            false
        },
        {
            "export_project",
            "Build the project (the Build Settings build pipeline) for the selected graphic backends.",
            objectSchema({
                {"target_dir", stringSchema("Build output directory. Must be outside or separate from project source")},
                {"backends", stringSchema("Comma-separated graphic backends the shaders are compiled for: opengl, opengles, d3d11, metal-macos, metal-ios, vulkan, all")},
                {"start_scene_id", integerSchema("Optional startup scene id override")}
            }, {"target_dir"}),
            false
        },
        {
            "generate_shaders",
            "Generate selected shaders using the editor shader builder.",
            objectSchema({
                {"target_dir", stringSchema("Output directory")},
                {"backends", stringSchema("Comma-separated graphic backends to compile for: opengl, opengles, d3d11, metal-macos, metal-ios, vulkan, all")},
                {"format", stringSchema("binary, header, or json")}
            }, {"target_dir"}),
            false
        },
        {
            "fork_shader",
            "Fork a built-in shader into a project-relative directory as one undoable step. Give entity_id/entity_name to fork a single component's shader (points its customShader at the fork); with shader_type depth on a MeshComponent it forks the depth shader instead (points customDepthShader at the fork): shadow maps, plus SSAO/post-process depth while SSR is off (with SSR on they read the built-in G-buffer) - needed when the mesh fork displaces vertices or discards, so shadows follow. Omit entity_id/entity_name and give shader_type instead to fork a scene-wide default shader for that type (points the scene's default_<type>_shader at the fork, used by every component of that type without its own customShader - see set_scene_property). Creates <name>.vert and <name>.frag directly in that directory. With fork_includes the fork is placed in its own <directory>/<name>/ folder together with an includes/ copy of the engine .glsl files it uses, so those edits override the engine library only for this fork. This is the correct way to start a custom shader; do not set customShader or a scene default_*_shader to files that do not exist.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id. Omit both this and entity_name, and give shader_type, to fork a scene default instead")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"component", stringSchema("Renderable component name (MeshComponent, UIComponent, PointsComponent, LineRendererComponent for a Line Renderer, SkyboxComponent for a Skybox). Omit to auto-detect the GameObject's renderable component. Ignored when forking a scene default")},
                {"shader_type", stringSchema("mesh, ui, sky, points, or lines. Required when entity_id/entity_name are omitted, to fork a scene-wide default shader for that type; with a GameObject only depth is meaningful (fork a mesh's depth shader)")},
                {"directory", stringSchema("Optional safe project-relative destination directory; defaults to shaders")},
                {"fork_includes", boolSchema("Also copy the built-in transitive .glsl includes so they can be edited; this moves the fork into its own <name>/ directory")}
            }),
            false
        },
        {
            "write_shader_file",
            "Write a project-relative custom shader source file (.vert, .frag, or .glsl). Use after fork_shader to edit entry points, private copied includes, or project-root-relative shared includes. Read the file first and keep ALL original texture and sampler declarations (uniform sampler2D/samplerCube/etc. and their #ifdef guards) plus the uniform blocks, #version, #include lines and output variable - the engine binds textures by fixed slot, so removing or renaming a declaration breaks the bindings. The only block you may add is the fork's own u_vs_customParams / u_fs_customParams (u_fs_postParams in a post-process pass), whose members are set with set_shader_uniform. Unmodified #include \"includes/...\" resolves private fork copies first, then project-root overrides, then the engine library.",
            objectSchema({
                {"path", stringSchema("Existing or new safe project-relative path. Allowed extensions: .vert, .frag, .glsl")},
                {"content", stringSchema("Complete replacement file contents")},
                {"reason", stringSchema("Short reason shown in the approval preview")}
            }, {"path", "content"}),
            false
        },
        {
            "inspect_shader_uniforms",
            "List the custom uniforms of a forked shader: the members its u_vs_customParams / u_fs_customParams blocks declare (or u_fs_postParams for a post-process pass), each with its GLSL type, whether the engine writes it (time, resolution), and the stored value. Give entity_id/entity_name (component optional, auto-detected) for a Mesh/UI/Points/Lines (Line Renderer)/Sky (Skybox) component, or pass_index for a scene post-process pass. The engine reflects the compiled shader, so members appear once the fork has built; a fork that declares no block lists none. Use before set_shader_uniform to learn the member names.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id of a component with a custom shader")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"component", stringSchema("Renderable component name (MeshComponent, UIComponent, PointsComponent, LineRendererComponent for a Line Renderer, SkyboxComponent for a Skybox). Omit to auto-detect")},
                {"pass_index", integerSchema("Post-process pass index (0 = first pass of the scene chain), instead of a GameObject")}
            }),
            true
        },
        {
            "set_shader_uniform",
            "Set or remove one custom uniform value of a forked shader, by member name, as one undoable step. Values live on the component (shaderUniforms) or on the post-process pass and are uploaded to the u_vs_customParams / u_fs_customParams block (u_fs_postParams for a pass) member of the same name; a value is kept as a Vector4, so give number_value for float/int members, vector2_value/vector3_value/vector4_value for vec2/vec3/vec4 (int members truncate). Give entity_id/entity_name (component optional) for a Mesh/UI/Points/Lines (Line Renderer)/Sky (Skybox) component, or pass_index for a scene post-process pass. time and resolution are written by the engine and cannot be set. Set remove=true to drop the value (the member reads zero again). Names the shader does not declare are stored but unused; use inspect_shader_uniforms to see the declared members.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id of a component with a custom shader")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"component", stringSchema("Renderable component name (MeshComponent, UIComponent, PointsComponent, LineRendererComponent for a Line Renderer, SkyboxComponent for a Skybox). Omit to auto-detect")},
                {"pass_index", integerSchema("Post-process pass index (0 = first pass of the scene chain), instead of a GameObject")},
                {"name", stringSchema("Uniform member name as declared in the shader block, e.g. amplitude or tint")},
                {"number_value", numberSchema("Value for a float or int member")},
                {"vector2_value", vector2Schema("Value for a vec2 member")},
                {"vector3_value", vector3Schema("Value for a vec3 member")},
                {"vector4_value", vector4Schema("Value for a vec4 member")},
                {"remove", boolSchema("Remove the stored value instead of setting one")}
            }, {"name"}),
            false
        },
        {
            "search_curated_assets",
            "Search curated model sources. Sketchfab search is metadata-only without user OAuth; Poly Haven API requires a unique User-Agent and may need licensing for commercial API use.",
            objectSchema({
                {"provider", stringSchema("polyhaven or sketchfab")},
                {"query", stringSchema("Search text")},
                {"max_results", integerSchema("Maximum number of assets to return, 1-12")}
            }, {"provider", "query"}),
            true
        },
        {
            "download_curated_asset",
            "Download a reviewed direct model URL into the project's assets (Project window), write attribution metadata, and optionally import it into the scene. Archives are rejected in v1 unless an extractor is added.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"provider", stringSchema("Asset provider name")},
                {"asset_id", stringSchema("Provider asset id")},
                {"title", stringSchema("Asset title")},
                {"author", stringSchema("Author name")},
                {"license", stringSchema("License name or URL")},
                {"source_url", stringSchema("Provider asset page URL")},
                {"download_url", stringSchema("Direct HTTPS URL for a .glb, .gltf, or .obj file")},
                {"slug", stringSchema("Filesystem-safe asset slug")},
                {"entity_name", stringSchema("Optional imported GameObject name")},
                {"position", vector3Schema("Optional import position")}
            }, {"provider", "asset_id", "title", "source_url", "download_url"}),
            false
        },
        {
            "read_resource_file",
            "Read a text project asset such as scripts, materials, scenes, shaders, or config files.",
            objectSchema({
                {"path", stringSchema("Safe project-relative file path")}
            }, {"path"}),
            true
        },
        {
            "inspect_scene",
            "Read scene metadata, child scenes, main camera, and supported scene settings.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")}
            }),
            true
        },
        {
            "set_main_camera",
            "Set or clear the scene main camera through SetMainCameraCmd.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Camera GameObject id")},
                {"entity_name", stringSchema("Camera GameObject name, used only when entity_id is omitted")},
                {"clear", boolSchema("When true, clear the main camera")}
            }),
            false
        },
        {
            "open_scene",
            "Open a .scene file from the project and select it in the editor.",
            objectSchema({
                {"scene_path", stringSchema("Safe project-relative .scene path")},
                {"close_previous", boolSchema("When true, close the previously selected scene tab")}
            }, {"scene_path"}),
            false
        },
        {
            "select_scene",
            "Select an already-open scene tab in the editor.",
            objectSchema({
                {"scene_id", integerSchema("Scene id to select")}
            }, {"scene_id"}),
            false
        },
        {
            "regenerate_mesh_geometry",
            "Replace a GameObject's mesh with a generated primitive through MeshChangeCmd. Rejected for geometry rebuilt on load: a model file, sprite, tilemap, mesh polygon, or terrain. Every parameter not given falls back to its default rather than keeping the mesh's current value.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Mesh GameObject id")},
                {"entity_name", stringSchema("Mesh GameObject name, used only when entity_id is omitted")},
                {"geometry_type", stringSchema("plane, box, sphere, cylinder, capsule, torus, or wall")},
                {"plane_width", numberSchema("Plane width")},
                {"plane_depth", numberSchema("Plane depth")},
                {"plane_tiles", integerSchema("Plane tile count")},
                {"wall_width", numberSchema("Wall width")},
                {"wall_height", numberSchema("Wall height")},
                {"wall_tiles", integerSchema("Wall tile count")},
                {"box_width", numberSchema("Box width")},
                {"box_height", numberSchema("Box height")},
                {"box_depth", numberSchema("Box depth")},
                {"box_tiles", integerSchema("Box tile count")},
                {"sphere_radius", numberSchema("Sphere radius")},
                {"sphere_slices", integerSchema("Sphere slices")},
                {"sphere_stacks", integerSchema("Sphere stacks")},
                {"cylinder_base_radius", numberSchema("Cylinder base radius")},
                {"cylinder_top_radius", numberSchema("Cylinder top radius")},
                {"cylinder_height", numberSchema("Cylinder height")},
                {"cylinder_slices", integerSchema("Cylinder slices")},
                {"cylinder_stacks", integerSchema("Cylinder stacks")},
                {"capsule_base_radius", numberSchema("Capsule base radius")},
                {"capsule_top_radius", numberSchema("Capsule top radius")},
                {"capsule_height", numberSchema("Capsule height")},
                {"capsule_slices", integerSchema("Capsule slices")},
                {"capsule_stacks", integerSchema("Capsule stacks")},
                {"torus_radius", numberSchema("Torus radius")},
                {"torus_ring_radius", numberSchema("Torus ring radius")},
                {"torus_sides", integerSchema("Torus sides")},
                {"torus_rings", integerSchema("Torus rings")}
            }),
            false
        },
        {
            "delete_scene",
            "Remove a saved scene from the project through project command history. Cannot remove the last scene.",
            objectSchema({
                {"scene_id", integerSchema("Scene id to remove")}
            }, {"scene_id"}),
            false
        },
        {
            "save_project",
            "Save the project file and project metadata.",
            objectSchema({}),
            false
        },
        {
            "copy_resource",
            "Copy a project file or directory to another project directory through CopyFileCmd.",
            objectSchema({
                {"source_path", stringSchema("Safe project-relative source file or directory path")},
                {"target_dir", stringSchema("Safe project-relative destination directory")}
            }, {"source_path", "target_dir"}),
            false
        },
        {
            "update_material_file",
            "Validate and replace an existing .material file with complete YAML content through project command history. Texture paths inside the file are stored relative to the project assets directory, so keep the form read_resource_file returned.",
            objectSchema({
                {"path", stringSchema("Existing safe project-relative .material path")},
                {"content", stringSchema("Complete replacement file contents")},
                {"reason", stringSchema("Short reason shown in the approval preview")}
            }, {"path", "content"}),
            false
        },
        {
            "set_component_properties",
            "Set multiple supported component properties (Inspector fields) atomically through MultiPropertyCmd.",
            objectSchemaFromProperties(propertyValueFields({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"properties", {
                    {"type", "array"},
                    {"description", "Property changes. Each item needs component, property, and one typed value field."},
                    {"items", objectSchemaFromProperties(propertyValueFields({
                        {"component", stringSchema("Component name")},
                        {"property", stringSchema("Property path")}
                    }), {"component", "property"})}
                }}
            }), {"properties"}),
            false
        },
        {
            "add_tilemap_tile",
            "Add a tile instance to a TilemapComponent using a tile rect index.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Tilemap GameObject id")},
                {"entity_name", stringSchema("Tilemap GameObject name, used only when entity_id is omitted")},
                {"rect_index", integerSchema("Tile rect index in tilesRect")},
                {"position", vector2Schema("Optional tile position")},
                {"width", numberSchema("Optional tile width override")},
                {"height", numberSchema("Optional tile height override")}
            }, {"rect_index"}),
            false
        },
        {
            "remove_tilemap_tile",
            "Remove a tile from a TilemapComponent by tile index.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Tilemap GameObject id")},
                {"entity_name", stringSchema("Tilemap GameObject name, used only when entity_id is omitted")},
                {"tile_index", integerSchema("Tile index to remove")}
            }, {"tile_index"}),
            false
        },
        {
            "duplicate_tilemap_tile",
            "Duplicate a tile in a TilemapComponent by tile index.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Tilemap GameObject id")},
                {"entity_name", stringSchema("Tilemap GameObject name, used only when entity_id is omitted")},
                {"tile_index", integerSchema("Tile index to duplicate")}
            }, {"tile_index"}),
            false
        },
        {
            "add_animation_action",
            "Add a Tween (action) clip to an AnimationComponent timeline. The Tween is a GameObject with a Position/Rotation/Scale/Color/Alpha Tween component (PositionTweenComponent etc.) or another action.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Animation GameObject id")},
                {"entity_name", stringSchema("Animation GameObject name, used only when entity_id is omitted")},
                {"start_time", numberSchema("Tween start time")},
                {"duration", numberSchema("Tween duration. 0 or omitted = auto (the Tween's own duration)")},
                {"track", integerSchema("Timeline track index")},
                {"action_entity_id", integerSchema("Tween (action) GameObject id")},
                {"action_entity_name", stringSchema("Tween (action) GameObject name, used only when action_entity_id is omitted")}
            }),
            false
        },
        {
            "remove_animation_action",
            "Remove a Tween (action) clip from an AnimationComponent timeline by index.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Animation GameObject id")},
                {"entity_name", stringSchema("Animation GameObject name, used only when entity_id is omitted")},
                {"action_index", integerSchema("Tween (action) index to remove")}
            }, {"action_index"}),
            false
        },
        {
            "set_keyframe_times",
            "Set or append keyframe times on a KeyframeTracksComponent.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id with KeyframeTracksComponent")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"times", {
                    {"type", "array"},
                    {"description", "Complete replacement keyframe time list"},
                    {"items", {{"type", "number"}}}
                }},
                {"append", boolSchema("When true, append time to existing keyframes")},
                {"time", numberSchema("Time value used with append=true")}
            }),
            false
        },
        {
            "set_keyframe_easing",
            "Set per-segment easing on a KeyframeTracksComponent. Segment i eases the interpolation from key i to key i+1; unset segments are LINEAR.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("GameObject id with KeyframeTracksComponent")},
                {"entity_name", stringSchema("GameObject name, used only when entity_id is omitted")},
                {"segment", integerSchema("Segment index (key i to key i+1), used with ease")},
                {"ease", stringSchema("Ease name (e.g. LINEAR, QUAD_IN_OUT, BOUNCE_OUT, ELASTIC_OUT)")},
                {"easings", {
                    {"type", "array"},
                    {"description", "Complete replacement easing list, one ease name per segment"},
                    {"items", {{"type", "string"}}}
                }}
            }),
            false
        },
        {
            "undo_editor",
            "Undo the last editor command in scene or project scope.",
            objectSchema({
                {"scope", stringSchema("scene or project. Defaults to scene")},
                {"scene_id", integerSchema("Scene id for scene scope. Omit to use the selected scene")}
            }),
            false
        },
        {
            "redo_editor",
            "Redo the last undone editor command in scene or project scope.",
            objectSchema({
                {"scope", stringSchema("scene or project. Defaults to scene")},
                {"scene_id", integerSchema("Scene id for scene scope. Omit to use the selected scene")}
            }),
            false
        },
        {
            "search_engine_source",
            "Grep the authoritative JasperEngine source (the real C++ headers and Lua binding files under the editor's engine/ tree) for exact ground-truth signatures, overloads, enum values, macros, and key codes. Prefer this over guessing: search_engine_api is a curated index that uses Lua-style names and omits C++ overloads, so verify real signatures here before writing engine API code. Note key codes are Input.KEY_* in Lua but D_KEY_* macros in C++.",
            objectSchema({
                {"query", stringSchema("Case-insensitive text to find in the engine source, e.g. 'Quaternion(', 'D_KEY_', 'isKeyPressed', 'setPosition'")},
                {"path_filter", stringSchema("Optional substring the file path must contain, e.g. 'Input.h', 'math/', 'binding'")},
                {"max_results", integerSchema("Maximum matching lines to return, 1-80. Defaults to 40")}
            }, {"query"}),
            true
        },
        {
            "read_engine_source",
            "Read a file from the authoritative JasperEngine source tree. Paths are relative to the engine/ root, e.g. core/math/Quaternion.h, core/Input.h, core/object/GameObject.h, or core/script/binding/CoreClassesLua.cpp. Use after search_engine_source to see full signatures and enums in context.",
            objectSchema({
                {"path", stringSchema("Engine-source-relative file path, e.g. core/math/Quaternion.h")},
                {"start_line", integerSchema("Optional 1-based first line to return")},
                {"end_line", integerSchema("Optional 1-based last line to return")}
            }, {"path"}),
            true
        },
        {
            "read_output_log",
            "Read the editor's recent Console log: build/compiler messages, warnings, errors, and runtime script crashes. Use this to VERIFY a script after entering Play mode with control_play_mode -- a C++ compile error appears here as a build failure ('C++ script build failed' plus compiler output), and a Lua or C++ runtime error appears as a 'Script crash in scene ...' entry. If you see errors, stop Play mode, fix the script, and verify again.",
            objectSchema({
                {"max_lines", integerSchema("Maximum number of recent Console entries to return, 1-600. Defaults to 150")},
                {"errors_only", boolSchema("When true, return only error/warning/build entries")}
            }),
            true
        }
    };
    return definitions;
}

bool hasString(const Json& args, const char* key) {
    return args.contains(key) && args[key].is_string() && !args[key].get<std::string>().empty();
}

// Json::value() throws type_error.302 on a present wrong-typed key; read it as absent.
std::string stringArg(const Json& args, const char* key) {
    if (!args.is_object()) return {};
    const auto it = args.find(key);
    if (it == args.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

// Wrong type, excluding null: models spell "no value" as null, but a number is a mistake.
bool isWrongTypedString(const Json& args, const char* key) {
    if (!args.is_object()) return false;
    const auto it = args.find(key);
    return it != args.end() && !it->is_string() && !it->is_null();
}

bool hasObject(const Json& args, const char* key) {
    return args.contains(key) && args[key].is_object();
}

bool hasAnyValueField(const Json& args) {
    static const char* keys[] = {
        "bool_value", "int_value", "number_value", "string_value",
        "vector2_value", "vector3_value", "vector4_value", "quat_value",
        "texture_path", "font_paths", "entity_value"
    };
    for (const char* key : keys) {
        if (args.contains(key)) return true;
    }
    return false;
}

bool hasEntitySelector(const Json& args) {
    return args.contains("entity_id") || hasString(args, "entity_name");
}

bool hasSceneId(const Json& args, const char* key) {
    return args.contains(key) && (args[key].is_number_integer() || args[key].is_number_unsigned());
}

ValidationResult ok() {
    return {true, {}};
}

ValidationResult fail(const std::string& error) {
    return {false, error};
}

} // namespace

std::vector<ToolDefinition> EditorActionRegistry::tools() {
    return cachedTools();
}

bool EditorActionRegistry::hasTool(const std::string& name) {
    for (const auto& tool : cachedTools()) {
        if (tool.name == name) return true;
    }
    return false;
}

bool EditorActionRegistry::isReadOnly(const std::string& name) {
    return getTool(name).readOnly;
}

ToolDefinition EditorActionRegistry::getTool(const std::string& name) {
    for (const auto& tool : cachedTools()) {
        if (tool.name == name) return tool;
    }
    return {};
}

ValidationResult EditorActionRegistry::validate(const std::string& name, const Json& arguments) {
    if (!hasTool(name)) {
        return fail("Unknown editor action: " + name);
    }
    if (!arguments.is_object()) {
        return fail("Action arguments must be a JSON object.");
    }

    if (name == "search_resources") {
        // Missing/empty/null query is a browse request; rejecting it only burned a step.
        if (isWrongTypedString(arguments, "query")) {
            return fail("search_resources query must be a string (omit it to list files).");
        }
        if (isWrongTypedString(arguments, "type")) {
            return fail("search_resources type must be a string.");
        }
        return ok();
    }
    if (name == "search_engine_api") {
        return hasString(arguments, "query") ? ok() : fail("search_engine_api requires query.");
    }
    if (name == "search_engine_source") {
        return hasString(arguments, "query") ? ok() : fail("search_engine_source requires query.");
    }
    if (name == "read_engine_source") {
        return hasString(arguments, "path") ? ok() : fail("read_engine_source requires path.");
    }
    if (name == "read_output_log") {
        return ok();
    }
    if (name == "inspect_entity" || name == "delete_entity" || name == "duplicate_entity") {
        return hasEntitySelector(arguments) ? ok() : fail(name + " requires entity_id or entity_name.");
    }
    if (name == "inspect_component" || name == "add_component" || name == "remove_component" ||
        name == "apply_prefab_component" || name == "revert_prefab_component") {
        if (!hasEntitySelector(arguments)) return fail(name + " requires entity_id or entity_name.");
        return hasString(arguments, "component") ? ok() : fail(name + " requires component.");
    }
    if (name == "add_rigidbody_collider") {
        if (!hasEntitySelector(arguments)) return fail("add_rigidbody_collider requires entity_id or entity_name.");
        return hasString(arguments, "shape_type") ? ok() : fail("add_rigidbody_collider requires shape_type.");
    }
    if (name == "add_rigidbody2d_collider") {
        if (!hasEntitySelector(arguments)) return fail("add_rigidbody2d_collider requires entity_id or entity_name.");
        return hasString(arguments, "shape_type") ? ok() : fail("add_rigidbody2d_collider requires shape_type.");
    }
    if (name == "rename_entity") {
        if (!hasEntitySelector(arguments)) return fail("rename_entity requires entity_id or entity_name.");
        return hasString(arguments, "new_name") ? ok() : fail("rename_entity requires new_name.");
    }
    if (name == "create_entity") {
        if (!hasString(arguments, "name") || !hasString(arguments, "type")) {
            return fail("create_entity requires name and type.");
        }
        return ok();
    }
    if (name == "set_entity_transform") {
        if (!arguments.contains("entity_id") && !hasString(arguments, "entity_name")) {
            return fail("set_entity_transform requires entity_id or entity_name.");
        }
        if (!hasObject(arguments, "position") && !hasObject(arguments, "rotation") && !hasObject(arguments, "scale")) {
            return fail("set_entity_transform requires position, rotation, or scale.");
        }
        return ok();
    }
    if (name == "reparent_entity") {
        return hasEntitySelector(arguments) ? ok() : fail("reparent_entity requires entity_id or entity_name.");
    }
    if (name == "move_entity_order") {
        if (!hasEntitySelector(arguments)) return fail("move_entity_order requires entity_id or entity_name.");
        if (!arguments.contains("target_id") && !hasString(arguments, "target_name")) return fail("move_entity_order requires target_id or target_name.");
        return hasString(arguments, "insertion") ? ok() : fail("move_entity_order requires insertion.");
    }
    if (name == "set_component_property") {
        if (!hasEntitySelector(arguments)) return fail("set_component_property requires entity_id or entity_name.");
        if (!hasString(arguments, "component") || !hasString(arguments, "property")) {
            return fail("set_component_property requires component and property.");
        }
        return hasAnyValueField(arguments) ? ok() : fail("set_component_property requires one typed value field.");
    }
    if (name == "set_texture_settings") {
        if (!hasEntitySelector(arguments)) return fail("set_texture_settings requires entity_id or entity_name.");
        if (!hasString(arguments, "component") || !hasString(arguments, "property")) {
            return fail("set_texture_settings requires component and property.");
        }
        if (!arguments.contains("min_filter") && !arguments.contains("mag_filter") &&
            !arguments.contains("wrap_u") && !arguments.contains("wrap_v") &&
            !arguments.contains("svg_scale")) {
            return fail("set_texture_settings requires at least one of min_filter, mag_filter, wrap_u, wrap_v, svg_scale.");
        }
        return ok();
    }
    if (name == "add_project_scene") {
        return hasString(arguments, "scene_path") ? ok() : fail("add_project_scene requires scene_path.");
    }
    if (name == "create_scene") {
        return hasString(arguments, "name") && hasString(arguments, "type")
            ? ok() : fail("create_scene requires name and type.");
    }
    if (name == "rename_scene") {
        return hasString(arguments, "new_name") ? ok() : fail("rename_scene requires new_name.");
    }
    if (name == "set_start_scene") {
        return hasSceneId(arguments, "scene_id") ? ok() : fail("set_start_scene requires scene_id.");
    }
    if (name == "set_scene_property") {
        if (!hasString(arguments, "property")) return fail("set_scene_property requires property.");
        return hasAnyValueField(arguments) ? ok() : fail("set_scene_property requires one typed value field.");
    }
    if (name == "control_play_mode") {
        return hasString(arguments, "action") ? ok() : fail("control_play_mode requires action.");
    }
    if (name == "add_child_scene" || name == "remove_child_scene") {
        return hasSceneId(arguments, "child_scene_id") ? ok() : fail(name + " requires child_scene_id.");
    }
    if (name == "set_child_scene_start_active") {
        if (!hasSceneId(arguments, "child_scene_id")) return fail("set_child_scene_start_active requires child_scene_id.");
        return arguments.contains("start_active") && arguments["start_active"].is_boolean()
            ? ok() : fail("set_child_scene_start_active requires start_active.");
    }
    if (name == "load_child_scene_inline") {
        if (!hasSceneId(arguments, "child_scene_id")) return fail("load_child_scene_inline requires child_scene_id.");
        return arguments.contains("load") && arguments["load"].is_boolean()
            ? ok() : fail("load_child_scene_inline requires load.");
    }
    if (name == "create_folder") {
        return hasString(arguments, "name") ? ok() : fail("create_folder requires name.");
    }
    if (name == "rename_resource") {
        return hasString(arguments, "path") && hasString(arguments, "new_name")
            ? ok() : fail("rename_resource requires path and new_name.");
    }
    if (name == "delete_resource") {
        return hasString(arguments, "path") ? ok() : fail("delete_resource requires path.");
    }
    if (name == "import_resource_file") {
        return hasString(arguments, "source_path") && hasString(arguments, "target_dir")
            ? ok() : fail("import_resource_file requires source_path and target_dir.");
    }
    if (name == "link_material") {
        if (!hasEntitySelector(arguments)) return fail("link_material requires entity_id or entity_name.");
        return hasString(arguments, "material_path") ? ok() : fail("link_material requires material_path.");
    }
    if (name == "unlink_material" || name == "remove_entity_from_prefab") {
        return hasEntitySelector(arguments) ? ok() : fail(name + " requires entity_id or entity_name.");
    }
    if (name == "create_material_file") {
        if (!hasEntitySelector(arguments)) return fail("create_material_file requires entity_id or entity_name.");
        return hasString(arguments, "target_dir") ? ok() : fail("create_material_file requires target_dir.");
    }
    if (name == "create_script") {
        if (!hasEntitySelector(arguments)) return fail("create_script requires entity_id or entity_name.");
        return hasString(arguments, "type") && hasString(arguments, "class_name")
            ? ok() : fail("create_script requires type and class_name.");
    }
    if (name == "attach_script") {
        if (!hasEntitySelector(arguments)) return fail("attach_script requires entity_id or entity_name.");
        return hasString(arguments, "type") && hasString(arguments, "class_name") && hasString(arguments, "path")
            ? ok() : fail("attach_script requires type, class_name, and path.");
    }
    if (name == "update_script_entry" || name == "remove_script_entry") {
        if (!hasEntitySelector(arguments)) return fail(name + " requires entity_id or entity_name.");
        const bool hasIndex = arguments.contains("index") && arguments["index"].is_number_integer();
        if (!hasIndex && !hasString(arguments, "class_name") && !hasString(arguments, "path")) {
            return fail(name + " requires index, class_name, or path.");
        }
        if (name == "update_script_entry" && !hasString(arguments, "new_class_name") &&
            !hasString(arguments, "new_path") && !hasString(arguments, "new_header_path")) {
            return fail("update_script_entry requires new_class_name, new_path, or new_header_path.");
        }
        return ok();
    }
    if (name == "update_script_file") {
        if (!hasString(arguments, "path")) return fail("update_script_file requires path.");
        if (!arguments.contains("content") || !arguments["content"].is_string()) {
            return fail("update_script_file requires string content.");
        }
        fs::path path = fs::path(arguments["path"].get<std::string>()).lexically_normal();
        if (!PathUtils::isSafeRelativePath(path)) {
            return fail("path must be a safe project-relative path.");
        }
        const std::string ext = path.extension().string();
        if (ext != ".lua" && ext != ".cpp" && ext != ".h" && ext != ".hpp") {
            return fail("update_script_file only supports .lua, .cpp, .h, and .hpp files.");
        }
        return ok();
    }
    if (name == "create_source_file") {
        if (!hasString(arguments, "path")) return fail("create_source_file requires path.");
        if (!arguments.contains("content") || !arguments["content"].is_string()) {
            return fail("create_source_file requires string content.");
        }
        fs::path path = fs::path(arguments["path"].get<std::string>()).lexically_normal();
        if (!PathUtils::isSafeRelativePath(path)) {
            return fail("path must be a safe project-relative path.");
        }
        // The same set update_script_file accepts, so a created file stays editable.
        const std::string ext = path.extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp") {
            return fail("create_source_file only supports .cpp, .h, and .hpp files.");
        }
        return ok();
    }
    if (name == "set_project_script_dirs") {
        const auto it = arguments.find("directories");
        if (it == arguments.end() || !it->is_array()) {
            return fail("set_project_script_dirs requires a directories array.");
        }
        // Only the executor sees the configured roots that decide which forms are allowed.
        for (const Json& entry : *it) {
            if (!entry.is_string() || entry.get<std::string>().empty()) {
                return fail("directories must contain only non-empty strings.");
            }
        }
        return ok();
    }
    if (name == "set_project_cxx_standard") {
        if (!arguments.contains("cxx_standard") || !arguments["cxx_standard"].is_number_integer()) {
            return fail("set_project_cxx_standard requires cxx_standard (" + cxxStandardList() + ").");
        }
        const Json& standard = arguments["cxx_standard"];
        if (standard < cxxStandards[0] || standard > cxxStandards[std::size(cxxStandards) - 1]
                || !isSupportedCxxStandard(standard.get<int>())) {
            return fail("cxx_standard must be " + cxxStandardList() + ".");
        }
        return ok();
    }
    if (name == "update_project_build_file") {
        if (!arguments.contains("content") || !arguments["content"].is_string()) {
            return fail("update_project_build_file requires string content.");
        }
        return ok();
    }
    if (name == "create_prefab_from_entity") {
        if (!hasString(arguments, "prefab_path")) return fail("create_prefab_from_entity requires prefab_path.");
        return hasEntitySelector(arguments) ? ok() : fail("create_prefab_from_entity requires entity_id or entity_name.");
    }
    if (name == "import_prefab_instance") {
        return hasString(arguments, "prefab_path") ? ok() : fail("import_prefab_instance requires prefab_path.");
    }
    if (name == "add_entity_to_prefab") {
        if (!hasEntitySelector(arguments)) return fail("add_entity_to_prefab requires entity_id or entity_name.");
        return arguments.contains("prefab_root_id") ? ok() : fail("add_entity_to_prefab requires prefab_root_id.");
    }
    if (name == "set_standalone_prefab") {
        return hasString(arguments, "prefab_path") ? ok() : fail("set_standalone_prefab requires prefab_path.");
    }
    if (name == "export_project" || name == "generate_shaders") {
        return hasString(arguments, "target_dir") ? ok() : fail(name + " requires target_dir.");
    }
    if (name == "fork_shader") {
        if (arguments.contains("directory")) {
            if (!arguments["directory"].is_string()) {
                return fail("fork_shader directory must be a string.");
            }
            fs::path directory = fs::path(arguments["directory"].get<std::string>()).lexically_normal();
            if (directory != "." && !PathUtils::isSafeRelativePath(directory)) {
                return fail("fork_shader directory must be a safe project-relative path.");
            }
        }
        if (arguments.contains("fork_includes") && !arguments["fork_includes"].is_boolean()) {
            return fail("fork_shader fork_includes must be a boolean.");
        }
        if (hasEntitySelector(arguments) || hasString(arguments, "shader_type")) return ok();
        return fail("fork_shader requires entity_id/entity_name, or shader_type to fork a scene default.");
    }
    if (name == "inspect_shader_uniforms") {
        if (hasEntitySelector(arguments) || arguments.contains("pass_index")) return ok();
        return fail("inspect_shader_uniforms requires entity_id/entity_name, or pass_index for a post-process pass.");
    }
    if (name == "set_shader_uniform") {
        if (!hasEntitySelector(arguments) && !arguments.contains("pass_index")) {
            return fail("set_shader_uniform requires entity_id/entity_name, or pass_index for a post-process pass.");
        }
        if (!hasString(arguments, "name")) return fail("set_shader_uniform requires name.");
        if (arguments.contains("remove") && !arguments["remove"].is_boolean()) {
            return fail("set_shader_uniform remove must be a boolean.");
        }
        const bool remove = arguments.value("remove", false);
        const bool hasValue = arguments.contains("number_value") || hasObject(arguments, "vector2_value") ||
                              hasObject(arguments, "vector3_value") || hasObject(arguments, "vector4_value");
        if (!remove && !hasValue) {
            return fail("set_shader_uniform requires number_value, vector2_value, vector3_value or vector4_value (or remove=true).");
        }
        return ok();
    }
    if (name == "write_shader_file") {
        if (!hasString(arguments, "path")) return fail("write_shader_file requires path.");
        if (!arguments.contains("content") || !arguments["content"].is_string()) {
            return fail("write_shader_file requires string content.");
        }
        fs::path path = fs::path(arguments["path"].get<std::string>()).lexically_normal();
        if (!PathUtils::isSafeRelativePath(path)) {
            return fail("path must be a safe project-relative path.");
        }
        const std::string ext = path.extension().string();
        if (ext != ".vert" && ext != ".frag" && ext != ".glsl") {
            return fail("write_shader_file only supports .vert, .frag, and .glsl files.");
        }
        return ok();
    }
    if (name == "create_terrain_heightmap") {
        std::string mode = arguments.value("mode", "middle");
        if (mode != "flat" && mode != "middle" && mode != "random_noise" && mode != "fractal_noise") {
            return fail("create_terrain_heightmap mode must be flat, middle, random_noise, or fractal_noise.");
        }
        return ok();
    }
    if (name == "import_project_model") {
        if (!hasString(arguments, "model_path")) {
            return fail("import_project_model requires model_path.");
        }
        if (!PathUtils::isSafeRelativePath(arguments["model_path"].get<std::string>())) {
            return fail("model_path must be a safe project-relative path.");
        }
        return ok();
    }
    if (name == "search_curated_assets") {
        return hasString(arguments, "provider") && hasString(arguments, "query")
            ? ok()
            : fail("search_curated_assets requires provider and query.");
    }
    if (name == "download_curated_asset") {
        if (!hasString(arguments, "provider") || !hasString(arguments, "asset_id") ||
            !hasString(arguments, "title") || !hasString(arguments, "source_url") ||
            !hasString(arguments, "download_url")) {
            return fail("download_curated_asset requires provider, asset_id, title, source_url, and download_url.");
        }
        const std::string url = arguments["download_url"].get<std::string>();
        if (url.rfind("https://", 0) != 0) {
            return fail("download_url must use HTTPS.");
        }
        return ok();
    }
    if (name == "read_resource_file") {
        return hasString(arguments, "path") ? ok() : fail("read_resource_file requires path.");
    }
    if (name == "inspect_scene") {
        return ok();
    }
    if (name == "set_main_camera") {
        if (arguments.value("clear", false)) return ok();
        return hasEntitySelector(arguments) ? ok() : fail("set_main_camera requires entity_id, entity_name, or clear.");
    }
    if (name == "open_scene") {
        return hasString(arguments, "scene_path") ? ok() : fail("open_scene requires scene_path.");
    }
    if (name == "select_scene" || name == "delete_scene") {
        return hasSceneId(arguments, "scene_id") ? ok() : fail(name + " requires scene_id.");
    }
    if (name == "regenerate_mesh_geometry") {
        return hasEntitySelector(arguments) ? ok() : fail("regenerate_mesh_geometry requires entity_id or entity_name.");
    }
    if (name == "save_project") {
        return ok();
    }
    if (name == "copy_resource") {
        return hasString(arguments, "source_path") && hasString(arguments, "target_dir")
            ? ok() : fail("copy_resource requires source_path and target_dir.");
    }
    if (name == "update_material_file") {
        if (!hasString(arguments, "path")) return fail("update_material_file requires path.");
        if (!arguments.contains("content") || !arguments["content"].is_string()) {
            return fail("update_material_file requires string content.");
        }
        return ok();
    }
    if (name == "set_component_properties") {
        if (!hasEntitySelector(arguments)) return fail("set_component_properties requires entity_id or entity_name.");
        if (!arguments.contains("properties") || !arguments["properties"].is_array() || arguments["properties"].empty()) {
            return fail("set_component_properties requires a non-empty properties array.");
        }
        for (const Json& item : arguments["properties"]) {
            if (!item.is_object() || !hasString(item, "component") || !hasString(item, "property")) {
                return fail("Each properties entry requires component and property.");
            }
            if (!hasAnyValueField(item)) {
                return fail("Each properties entry requires one typed value field.");
            }
        }
        return ok();
    }
    if (name == "add_tilemap_tile") {
        if (!hasEntitySelector(arguments)) return fail("add_tilemap_tile requires entity_id or entity_name.");
        return arguments.contains("rect_index") ? ok() : fail("add_tilemap_tile requires rect_index.");
    }
    if (name == "remove_tilemap_tile" || name == "duplicate_tilemap_tile") {
        if (!hasEntitySelector(arguments)) return fail(name + " requires entity_id or entity_name.");
        return arguments.contains("tile_index") ? ok() : fail(name + " requires tile_index.");
    }
    if (name == "add_animation_action") {
        return hasEntitySelector(arguments) ? ok() : fail("add_animation_action requires entity_id or entity_name.");
    }
    if (name == "remove_animation_action") {
        if (!hasEntitySelector(arguments)) return fail("remove_animation_action requires entity_id or entity_name.");
        return arguments.contains("action_index") ? ok() : fail("remove_animation_action requires action_index.");
    }
    if (name == "set_keyframe_times") {
        if (!hasEntitySelector(arguments)) return fail("set_keyframe_times requires entity_id or entity_name.");
        if (arguments.contains("times") && arguments["times"].is_array()) return ok();
        if (arguments.value("append", false) && arguments.contains("time") && arguments["time"].is_number()) return ok();
        return fail("set_keyframe_times requires times array or append=true with time.");
    }
    if (name == "set_keyframe_easing") {
        if (!hasEntitySelector(arguments)) return fail("set_keyframe_easing requires entity_id or entity_name.");
        if (arguments.contains("easings") && arguments["easings"].is_array()) return ok();
        if (arguments.contains("segment") && arguments.contains("ease")) return ok();
        return fail("set_keyframe_easing requires easings array or segment + ease.");
    }
    if (name == "undo_editor" || name == "redo_editor") {
        return ok();
    }

    return ok();
}

std::string EditorActionRegistry::describe(const std::string& name, const Json& arguments) {
    std::ostringstream out;
    if (name == "get_project_summary") {
        return "Read project summary";
    }
    if (name == "search_resources") {
        const std::string query = stringArg(arguments, "query");
        const std::string type = stringArg(arguments, "type");
        if (!query.empty()) {
            return "Search assets for \"" + query + "\"";
        }
        return (type.empty() || type == "any")
            ? "List project assets"
            : "List project " + type + " assets";
    }
    if (name == "list_scene_entities") {
        return arguments.contains("scene_id")
            ? "List GameObjects in scene " + std::to_string(arguments.value("scene_id", 0))
            : "List GameObjects in the selected scene";
    }
    if (name == "inspect_entity") {
        return arguments.contains("entity_id")
            ? "Inspect GameObject " + std::to_string(arguments.value("entity_id", 0))
            : "Inspect GameObject \"" + arguments.value("entity_name", "") + "\"";
    }
    if (name == "inspect_component") {
        return "Inspect " + arguments.value("component", "component") + " properties";
    }
    if (name == "list_component_types") {
        return "List editor component types";
    }
    if (name == "search_engine_api") {
        return "Search engine API for \"" + arguments.value("query", "") + "\"";
    }
    if (name == "search_engine_source") {
        return "Search engine source for \"" + arguments.value("query", "") + "\"";
    }
    if (name == "read_engine_source") {
        return "Read engine source " + arguments.value("path", "");
    }
    if (name == "read_output_log") {
        return "Read Console log";
    }
    if (name == "create_entity") {
        out << "Create " << unityTypeLabel(arguments.value("type", "empty"))
            << " GameObject \"" << arguments.value("name", "GameObject") << "\"";
        return out.str();
    }
    if (name == "set_entity_transform") {
        if (arguments.contains("entity_id")) {
            out << "Set transform for GameObject " << arguments["entity_id"].get<uint64_t>();
        } else {
            out << "Set transform for GameObject \"" << arguments.value("entity_name", "") << "\"";
        }
        return out.str();
    }
    if (name == "rename_entity") {
        return "Rename GameObject to \"" + arguments.value("new_name", "") + "\"";
    }
    if (name == "delete_entity") {
        return "Delete GameObject";
    }
    if (name == "duplicate_entity") {
        return "Duplicate GameObject";
    }
    if (name == "reparent_entity") {
        return "Reparent GameObject";
    }
    if (name == "move_entity_order") {
        return "Move GameObject " + arguments.value("insertion", "relative to target");
    }
    if (name == "select_entities") {
        return arguments.value("clear", false) ? "Clear GameObject selection" : "Select GameObject";
    }
    if (name == "add_component") {
        return "Add " + arguments.value("component", "component");
    }
    if (name == "remove_component") {
        return "Remove " + arguments.value("component", "component");
    }
    if (name == "add_rigidbody_collider") {
        return "Add " + arguments.value("shape_type", "3D") + " Collider to Rigidbody";
    }
    if (name == "add_rigidbody2d_collider") {
        return "Add " + arguments.value("shape_type", "2D") + " Collider 2D to Rigidbody 2D";
    }
    if (name == "set_component_property") {
        return "Set " + arguments.value("component", "component") + "." + arguments.value("property", "property");
    }
    if (name == "set_texture_settings") {
        return "Set texture settings of " + arguments.value("component", "component") + "." + arguments.value("property", "property");
    }
    if (name == "add_project_scene") {
        return "Add scene " + arguments.value("scene_path", "") + " to the project";
    }
    if (name == "create_scene") {
        return "Create " + arguments.value("type", "scene") + " scene \"" + arguments.value("name", "Scene") + "\"";
    }
    if (name == "rename_scene") {
        return "Rename scene to \"" + arguments.value("new_name", "") + "\"";
    }
    if (name == "save_scene") {
        return "Save scene";
    }
    if (name == "save_all_scenes") {
        return "Save all scenes";
    }
    if (name == "set_start_scene") {
        return "Set startup scene " + std::to_string(arguments.value("scene_id", 0));
    }
    if (name == "set_scene_property") {
        return "Set scene property " + arguments.value("property", "");
    }
    if (name == "control_play_mode") {
        std::string action = arguments.value("action", "control");
        if (!action.empty()) {
            action[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(action[0])));
        }
        return action + " Play mode";
    }
    if (name == "add_child_scene") {
        return "Add child scene " + std::to_string(arguments.value("child_scene_id", 0));
    }
    if (name == "remove_child_scene") {
        return "Remove child scene " + std::to_string(arguments.value("child_scene_id", 0));
    }
    if (name == "set_child_scene_start_active") {
        return std::string(arguments.value("start_active", true) ? "Enable" : "Disable") + " child scene Start active";
    }
    if (name == "load_child_scene_inline") {
        return std::string(arguments.value("load", true) ? "Load" : "Unload") + " child scene inline";
    }
    if (name == "create_folder") {
        return "Create folder \"" + arguments.value("name", "") + "\"";
    }
    if (name == "rename_resource") {
        return "Rename asset " + arguments.value("path", "");
    }
    if (name == "delete_resource") {
        return "Delete asset " + arguments.value("path", "");
    }
    if (name == "import_resource_file") {
        return "Import file " + arguments.value("source_path", "");
    }
    if (name == "link_material") {
        return "Link material " + arguments.value("material_path", "");
    }
    if (name == "unlink_material") {
        return "Unlink material";
    }
    if (name == "create_material_file") {
        return "Create linked material file";
    }
    if (name == "set_terrain_textures") {
        return "Set terrain textures";
    }
    if (name == "create_terrain_blendmap") {
        return "Create terrain blendmap";
    }
    if (name == "add_terrain_collision") {
        return "Add terrain collision";
    }
    if (name == "create_script") {
        return "Create " + arguments.value("type", "script") + " script " + arguments.value("class_name", "");
    }
    if (name == "attach_script") {
        return "Attach script " + arguments.value("class_name", "");
    }
    if (name == "update_script_entry" || name == "remove_script_entry") {
        const std::string action = (name == "update_script_entry") ? "Update" : "Remove";
        std::string label = arguments.value("class_name", "");
        if (label.empty()) label = arguments.value("path", "");
        return label.empty() ? action + " script entry" : action + " script entry " + label;
    }
    if (name == "update_script_file") {
        return "Update script file " + arguments.value("path", "");
    }
    if (name == "create_source_file") {
        return "Create source file " + arguments.value("path", "");
    }
    if (name == "set_project_script_dirs") {
        const auto it = arguments.find("directories");
        if (it == arguments.end() || !it->is_array() || it->empty()) {
            return "Clear project script directories";
        }
        std::string list;
        for (const Json& entry : *it) {
            if (!entry.is_string()) continue;
            if (!list.empty()) list += ", ";
            list += entry.get<std::string>();
        }
        return list.empty() ? "Set project script directories"
                            : "Set project script directories: " + list;
    }
    if (name == "set_project_cxx_standard") {
        if (arguments.contains("cxx_standard") && arguments["cxx_standard"].is_number_integer()) {
            return "Set project C++ standard to C++" + std::to_string(arguments["cxx_standard"].get<int>());
        }
        return "Set project C++ standard";
    }
    if (name == "update_project_build_file") {
        return "Update ProjectBuild.cmake";
    }
    if (name == "create_prefab_from_entity") {
        return "Create Prefab " + arguments.value("prefab_path", "");
    }
    if (name == "import_prefab_instance") {
        return "Instantiate Prefab " + arguments.value("prefab_path", "");
    }
    if (name == "add_entity_to_prefab") {
        return "Add GameObject to Prefab";
    }
    if (name == "remove_entity_from_prefab") {
        return "Remove GameObject from Prefab";
    }
    if (name == "apply_prefab_component") {
        return "Apply Prefab component overrides";
    }
    if (name == "revert_prefab_component") {
        return "Revert Prefab component overrides";
    }
    if (name == "set_standalone_prefab") {
        return "Set standalone Prefab " + arguments.value("prefab_path", "");
    }
    if (name == "export_project") {
        return "Build project to " + arguments.value("target_dir", "");
    }
    if (name == "generate_shaders") {
        return "Generate shaders to " + arguments.value("target_dir", "");
    }
    if (name == "fork_shader") {
        if (arguments.contains("entity_id")) {
            return "Fork shader for GameObject " + std::to_string(arguments.value("entity_id", 0));
        }
        if (arguments.contains("entity_name")) {
            return "Fork shader for \"" + arguments.value("entity_name", "") + "\"";
        }
        if (arguments.contains("shader_type")) {
            return "Fork scene default " + arguments.value("shader_type", "") + " shader";
        }
        return "Fork custom shader";
    }
    if (name == "write_shader_file") {
        return "Write shader " + arguments.value("path", "");
    }
    if (name == "inspect_shader_uniforms") {
        return "Inspect shader uniforms";
    }
    if (name == "set_shader_uniform") {
        const std::string verb = arguments.value("remove", false) ? "Remove" : "Set";
        if (arguments.contains("pass_index")) {
            return verb + " post-process uniform " + arguments.value("name", "") + " of pass " + std::to_string(arguments.value("pass_index", 0));
        }
        return verb + " shader uniform " + arguments.value("name", "");
    }
    if (name == "create_terrain_heightmap") {
        out << "Create " << arguments.value("mode", "middle")
            << " terrain heightmap";
        if (arguments.contains("entity_id")) {
            out << " for GameObject " << arguments["entity_id"].get<uint64_t>();
        } else if (arguments.contains("entity_name")) {
            out << " for \"" << arguments.value("entity_name", "") << "\"";
        }
        return out.str();
    }
    if (name == "import_project_model") {
        return "Import model " + arguments.value("model_path", "");
    }
    if (name == "search_curated_assets") {
        return "Search " + arguments.value("provider", "curated assets") +
               " for \"" + arguments.value("query", "") + "\"";
    }
    if (name == "download_curated_asset") {
        return "Download curated asset \"" + arguments.value("title", "Asset") + "\"";
    }
    if (name == "read_resource_file") {
        return "Read asset " + arguments.value("path", "");
    }
    if (name == "inspect_scene") {
        return arguments.contains("scene_id")
            ? "Inspect scene " + std::to_string(arguments.value("scene_id", 0))
            : "Inspect selected scene";
    }
    if (name == "set_main_camera") {
        return arguments.value("clear", false) ? "Clear main camera" : "Set main camera";
    }
    if (name == "open_scene") {
        return "Open scene " + arguments.value("scene_path", "");
    }
    if (name == "select_scene") {
        return "Select scene " + std::to_string(arguments.value("scene_id", 0));
    }
    if (name == "regenerate_mesh_geometry") {
        return "Regenerate mesh geometry";
    }
    if (name == "delete_scene") {
        return "Delete scene " + std::to_string(arguments.value("scene_id", 0));
    }
    if (name == "save_project") {
        return "Save project";
    }
    if (name == "copy_resource") {
        return "Copy " + arguments.value("source_path", "") + " to " + arguments.value("target_dir", "");
    }
    if (name == "update_material_file") {
        return "Update material file " + arguments.value("path", "");
    }
    if (name == "set_component_properties") {
        return "Set multiple component properties";
    }
    if (name == "add_tilemap_tile") {
        return "Add tilemap tile";
    }
    if (name == "remove_tilemap_tile") {
        return "Remove tilemap tile " + std::to_string(arguments.value("tile_index", 0));
    }
    if (name == "duplicate_tilemap_tile") {
        return "Duplicate tilemap tile " + std::to_string(arguments.value("tile_index", 0));
    }
    if (name == "add_animation_action") {
        return "Add Tween to animation";
    }
    if (name == "remove_animation_action") {
        return "Remove animation Tween " + std::to_string(arguments.value("action_index", 0));
    }
    if (name == "set_keyframe_times") {
        return "Set keyframe times";
    }
    if (name == "set_keyframe_easing") {
        return "Set keyframe easing";
    }
    if (name == "undo_editor") {
        return "Undo " + arguments.value("scope", "scene") + " command";
    }
    if (name == "redo_editor") {
        return "Redo " + arguments.value("scope", "scene") + " command";
    }
    return name;
}

} // namespace jasperengine::editor::ai
