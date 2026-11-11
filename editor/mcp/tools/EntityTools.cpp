// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W1: hierarchy and GameObjects: tree, find, create, delete, duplicate, rename, move,
// active/tag/layer, selection, copy/cut/paste
#include "McpTools.h"
#include "component/Light2DComponent.h"
#include "component/PrefabComponent.h"
#include "component/Transform.h"
#include "LatticeShared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "Out.h"
#include "Project.h"
#include "command/PlayChangeReview.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/DuplicateEntityCmd.h"
#include "command/type/EntityNameCmd.h"
#include "command/type/EntityStateCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/MoveEntityOrderCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/ObjectTransformCmd.h"
#include "command/type/PasteEntitiesCmd.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "lattice/LatticeEditorOps.h"
#include "registry/TagManager.h"
#include "util/EntityClipboard.h"
#include "util/Util.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;

// ------------------------------------------------------------------ helpers

SceneProject* sceneArg(const CallPtr& call, bool requireLoaded = true) {
    std::string error;
    const Json& args = call->args();
    SceneProject* scene = McpRefs::resolveScene(call->ctx().project(), args.contains("scene") ? args["scene"] : Json(), error);
    if (!scene) {
        call->fail(error);
        return nullptr;
    }
    if (requireLoaded && !scene->scene) {
        call->fail(McpRefs::notOpenError(call->ctx().project(), *scene));
        return nullptr;
    }
    return scene;
}

// move_entity's parent: null = the scene root
Json nullableEntityRef(const std::string& description) {
    Json ref = entityRef(description);
    ref["type"] = Json::array({"string", "null"});
    return ref;
}

bool wildcardMatch(const std::string& pattern, const std::string& text) {
    // case-insensitive * and ?
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    auto eq = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || eq(pattern[p], text[t]))) {
            p++;
            t++;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') p++;
    return p == pattern.size();
}

Entity parentOf(Scene* scene, Entity entity) {
    Transform* transform = scene->findComponent<Transform>(entity);
    return transform ? transform->parent : NULL_ENTITY;
}

// Children by parent, in Transform (Hierarchy) order; roots in the Hierarchy window's order
struct Hierarchy {
    std::vector<Entity> roots;
    std::unordered_map<Entity, std::vector<Entity>> children;
};

Hierarchy buildHierarchy(SceneProject* scene) {
    Hierarchy hierarchy;
    Scene* registry = scene->scene;
    const bool playing = scene->playState != ScenePlayState::STOPPED;
    // Membership does not determine order: the vectors below retain Transform and
    // authored order. Runtime trees need no authored-membership index.
    std::unordered_set<Entity> authored;
    if (!playing) {
        authored.reserve(scene->entities.size());
        authored.insert(scene->entities.begin(), scene->entities.end());
    }
    auto transforms = registry->getComponentArray<Transform>();
    const size_t count = transforms ? transforms->size() : 0;
    std::unordered_set<Entity> withTransform;
    withTransform.reserve(count);
    std::vector<Entity> transformRoots;
    for (size_t i = 0; i < count; i++) {
        const Entity entity = transforms->getEntity(i);
        if (!playing && !authored.count(entity)) continue;
        withTransform.insert(entity);
        const Entity parent = transforms->getComponentFromIndex(i).parent;
        if (parent == NULL_ENTITY) transformRoots.push_back(entity);
        else hierarchy.children[parent].push_back(entity);
    }
    // As the Hierarchy window lists them (HierarchyWindow::rebuildEntityTree): the GameObjects
    // without a Transform first, in the scene's order, then the Transform roots in Transform order
    for (Entity entity : scene->entities) {
        if (!withTransform.count(entity) && registry->isEntityCreated(entity)) hierarchy.roots.push_back(entity);
    }
    hierarchy.roots.insert(hierarchy.roots.end(), transformRoots.begin(), transformRoots.end());
    return hierarchy;
}

std::string componentsText(Scene* scene, Entity entity) {
    std::string text;
    for (ComponentType type : Catalog::findComponents(scene, entity)) {
        if (type == ComponentType::Transform) continue;
        if (!text.empty()) text += ", ";
        text += McpRefs::componentName(type);
    }
    return text;
}

Json componentsJson(Scene* scene, Entity entity) {
    Json list = Json::array();
    for (ComponentType type : Catalog::findComponents(scene, entity)) {
        if (type == ComponentType::Transform) continue;
        list.push_back(McpRefs::componentName(type));
    }
    return list;
}

// ------------------------------------------------------------------ entity types

struct EntityKind {
    const char* name;
    EntityCreationType type;
    const char* label;   // Unity's menu name, the default GameObject name
};

const std::vector<EntityKind>& entityKinds() {
    static const std::vector<EntityKind> kinds = {
        {"empty", EntityCreationType::GAMEOBJECT, "GameObject"},
        {"bare", EntityCreationType::EMPTY, "GameObject"},
        {"cube", EntityCreationType::BOX, "Cube"},
        {"plane", EntityCreationType::PLANE, "Plane"},
        {"wall", EntityCreationType::WALL, "Wall"},
        {"mirror", EntityCreationType::MIRROR, "Mirror"},
        {"sphere", EntityCreationType::SPHERE, "Sphere"},
        {"cylinder", EntityCreationType::CYLINDER, "Cylinder"},
        {"capsule", EntityCreationType::CAPSULE, "Capsule"},
        {"torus", EntityCreationType::TORUS, "Torus"},
        {"model", EntityCreationType::MODEL, "Model"},
        {"terrain", EntityCreationType::TERRAIN, "Terrain"},
        {"water", EntityCreationType::WATER, "Water"},
        {"camera", EntityCreationType::CAMERA, "Camera"},
        {"directional_light", EntityCreationType::DIRECTIONAL_LIGHT, "Directional Light"},
        {"point_light", EntityCreationType::POINT_LIGHT, "Point Light"},
        {"spot_light", EntityCreationType::SPOT_LIGHT, "Spot Light"},
        {"skybox", EntityCreationType::SKYBOX, "Skybox"},
        {"fog", EntityCreationType::FOG, "Fog"},
        {"local_volumetric_fog", EntityCreationType::LOCAL_VOLUMETRIC_FOG, "Local Volumetric Fog"},
        {"volumetric_clouds", EntityCreationType::VOLUMETRIC_CLOUDS, "Volumetric Clouds"},
        {"reflection_probe", EntityCreationType::REFLECTION_PROBE, "Reflection Probe"},
        {"particle_system", EntityCreationType::PARTICLE_SYSTEM, "Particle System"},
        {"force_field", EntityCreationType::FORCE_FIELD, "Force Field"},
        {"fluid_simulation", EntityCreationType::FLUID_SIMULATION, "Fluid Simulation"},
        {"video_player", EntityCreationType::VIDEO_PLAYER, "Video Player"},
        {"video_background", EntityCreationType::VIDEO_BACKGROUND, "Video Background"},
        {"points", EntityCreationType::POINTS, "Points"},
        {"line_renderer", EntityCreationType::LINE_RENDERER, "Line Renderer"},
        {"trail_renderer", EntityCreationType::TRAIL_RENDERER, "Trail Renderer"},
        {"mesh_polygon", EntityCreationType::MESH_POLYGON, "Mesh Polygon"},
        {"sprite", EntityCreationType::SPRITE, "Sprite"},
        // S50: a Grid and its Tilemap Layer child, as one undo step (createEntityRun)
        {"tilemap", EntityCreationType::TILEMAP_GRID, "Grid"},
        {"free_tilemap", EntityCreationType::TILEMAP, "Free Tilemap"},
        {"light_2d", EntityCreationType::POINT_LIGHT_2D, "Light 2D"},
        {"shadow_caster_2d", EntityCreationType::SHADOW_CASTER_2D, "Shadow Caster 2D"},
        {"rigidbody2d", EntityCreationType::RIGIDBODY2D, "Rigidbody 2D"},
        {"rigidbody", EntityCreationType::RIGIDBODY, "Rigidbody"},
        {"joint2d", EntityCreationType::JOINT2D, "Joint 2D"},
        {"joint3d", EntityCreationType::JOINT3D, "Joint"},
        {"audio_source", EntityCreationType::AUDIO_SOURCE, "Audio Source"},
        {"audio_source_3d", EntityCreationType::AUDIO_SOURCE_3D, "Audio Source 3D"},
        {"image", EntityCreationType::IMAGE, "Image"},
        {"text", EntityCreationType::TEXT, "Text"},
        {"button", EntityCreationType::BUTTON, "Button"},
        {"scrollbar", EntityCreationType::SCROLLBAR, "Scrollbar"},
        {"progressbar", EntityCreationType::PROGRESSBAR, "Progress Bar"},
        {"input_field", EntityCreationType::INPUT_FIELD, "Input Field"},
        {"onscreen_stick", EntityCreationType::ONSCREEN_STICK_FIXED, "On-Screen Stick"},
        {"onscreen_button", EntityCreationType::ONSCREEN_BUTTON, "On-Screen Button"},
        {"touch_area", EntityCreationType::TOUCH_AREA, "Touch Area"},
        {"panel", EntityCreationType::PANEL, "Panel"},
        {"polygon", EntityCreationType::POLYGON, "Polygon"},
        {"layout_group", EntityCreationType::LAYOUT_GROUP, "Layout Group"},
        {"canvas_group", EntityCreationType::CANVAS_GROUP, "Canvas Group"},
        {"safe_area", EntityCreationType::SAFE_AREA, "Safe Area"},
        {"ui_document", EntityCreationType::UI_DOCUMENT, "UI Document"},
        {"animation", EntityCreationType::ANIMATION, "Animation"},
        {"sprite_animation", EntityCreationType::SPRITE_ANIMATION, "Sprite Animation"},
        {"position_tween", EntityCreationType::POSITION_TWEEN, "Position Tween"},
        {"rotation_tween", EntityCreationType::ROTATION_TWEEN, "Rotation Tween"},
        {"scale_tween", EntityCreationType::SCALE_TWEEN, "Scale Tween"},
        {"color_tween", EntityCreationType::COLOR_TWEEN, "Color Tween"},
        {"alpha_tween", EntityCreationType::ALPHA_TWEEN, "Alpha Tween"},
        {"translate_tracks", EntityCreationType::TRANSLATE_TRACKS, "Translate Tracks"},
        {"rotate_tracks", EntityCreationType::ROTATE_TRACKS, "Rotate Tracks"},
        {"scale_tracks", EntityCreationType::SCALE_TRACKS, "Scale Tracks"},
        {"morph_tracks", EntityCreationType::MORPH_TRACKS, "Morph Tracks"},
        {"lattice_tracks", EntityCreationType::LATTICE_TRACKS, "Lattice Tracks"},
        {"lattice", EntityCreationType::LATTICE, "Lattice"}
    };
    return kinds;
}

const EntityKind* findKind(const std::string& given) {
    std::string key;
    for (char c : given) key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    static const std::map<std::string, std::string> aliases = {
        {"box", "cube"}, {"gameobject", "empty"}, {"object", "empty"}, {"create_empty", "empty"},
        {"sky", "skybox"}, {"particles", "particle_system"}, {"lines", "line_renderer"}, {"sound", "audio_source"},
        {"sound_3d", "audio_source_3d"}, {"inputfield", "input_field"}, {"container", "layout_group"},
        {"light2d", "light_2d"}, {"point_light_2d", "light_2d"}, {"rigidbody_2d", "rigidbody2d"},
        {"rigidbody3d", "rigidbody"}, {"joint", "joint3d"}, {"joint_2d", "joint2d"}, {"progress_bar", "progressbar"},
        {"sun", "directional_light"}, {"audio", "audio_source"}, {"ui_image", "image"}, {"ui_text", "text"},
        {"label", "text"}, {"quad", "plane"}, {"uidocument", "ui_document"}, {"html", "ui_document"},
        {"fluid", "fluid_simulation"}, {"line", "line_renderer"}, {"trail", "trail_renderer"}, {"video", "video_player"},
        {"videoplayer", "video_player"}, {"video_camera_background", "video_background"}
    };
    auto alias = aliases.find(key);
    if (alias != aliases.end()) key = alias->second;
    for (const EntityKind& kind : entityKinds()) {
        if (key == kind.name) return &kind;
    }
    return nullptr;
}

std::string kindNamesText() {
    std::string text;
    for (const EntityKind& kind : entityKinds()) {
        if (!text.empty()) text += ", ";
        text += kind.name;
    }
    return text;
}

// ------------------------------------------------------------------ get_scene_tree

struct TreeState {
    SceneProject* scene;
    const Hierarchy* hierarchy;
    bool components;
    int maxDepth;
    size_t limit;
    size_t emitted = 0;
    bool truncated = false;
    std::string text;
    Entity mainCamera;
    std::set<Entity> selected;
};

std::string flagsText(TreeState& state, Entity entity) {
    Scene* scene = state.scene->scene;
    std::string flags;
    if (!scene->isEntityActive(entity)) flags += " (inactive)";
    if (entity == state.mainCamera) flags += " (main camera)";
    if (state.selected.count(entity)) flags += " (selected)";
    return flags;
}

void emitText(TreeState& state, Entity entity, int depth) {
    if (state.emitted >= state.limit) {
        state.truncated = true;
        return;
    }
    state.emitted++;
    Scene* scene = state.scene->scene;
    std::string line(static_cast<size_t>(depth) * 2, ' ');
    line += scene->getEntityName(entity) + " [" + std::to_string(entity) + "]";
    if (state.components) {
        const std::string comps = componentsText(scene, entity);
        if (!comps.empty()) line += " " + comps;
        if (!scene->findComponent<Transform>(entity)) line += " (no transform)";
    }
    line += flagsText(state, entity);
    auto it = state.hierarchy->children.find(entity);
    const size_t childCount = it != state.hierarchy->children.end() ? it->second.size() : 0;
    if (childCount > 0 && depth + 1 > state.maxDepth) line += " {+" + std::to_string(childCount) + " children}";
    state.text += line + "\n";
    if (childCount > 0 && depth + 1 <= state.maxDepth) {
        for (Entity child : it->second) {
            emitText(state, child, depth + 1);
            if (state.truncated) break;
        }
    }
}

Json emitJson(TreeState& state, Entity entity, int depth) {
    if (state.emitted >= state.limit) {
        state.truncated = true;
        return nullptr;
    }
    state.emitted++;
    Scene* scene = state.scene->scene;
    Json node = {{"id", entity}, {"name", scene->getEntityName(entity)}};
    if (state.components) node["components"] = componentsJson(scene, entity);
    if (!scene->isEntityActive(entity)) node["active"] = false;
    if (entity == state.mainCamera) node["main_camera"] = true;
    if (state.selected.count(entity)) node["selected"] = true;
    auto it = state.hierarchy->children.find(entity);
    if (it != state.hierarchy->children.end() && !it->second.empty()) {
        if (depth + 1 > state.maxDepth) {
            node["child_count"] = it->second.size();
        } else {
            Json children = Json::array();
            for (Entity child : it->second) {
                Json item = emitJson(state, child, depth + 1);
                if (item.is_null()) break;
                children.push_back(std::move(item));
            }
            node["children"] = std::move(children);
        }
    }
    return node;
}

void getSceneTreeTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    if (args.value("runtime", false)) scene = McpRefs::runtimeScene(project, scene);

    const Hierarchy hierarchy = buildHierarchy(scene);
    TreeState state{scene, &hierarchy, args.value("components", true), args.value("depth", 64),
                    static_cast<size_t>(std::max(1, args.value("limit", 400)))};
    state.mainCamera = scene->mainCamera;
    for (Entity entity : project->getSelectedEntities(scene->id)) state.selected.insert(entity);

    std::vector<Entity> starts = hierarchy.roots;
    Json rootJson = nullptr;
    if (args.contains("root") && !args["root"].is_null()) {
        std::string error;
        const Entity root = McpRefs::resolveEntity(scene, args["root"], error);
        if (root == NULL_ENTITY) {
            call->fail(error);
            return;
        }
        starts = {root};
        rootJson = McpRefs::entityJson(scene, root);
    }
    const bool json = args.value("format", std::string("text")) == "json";
    std::string header = "Scene " + std::to_string(scene->id) + " \"" + scene->name + "\" (" +
                         McpRefs::sceneTypeName(scene->sceneType) + ", " + std::to_string(scene->entities.size()) + " GameObjects" +
                         (scene->playState != ScenePlayState::STOPPED ? ", " + McpRefs::playStateName(scene->playState) : "") + ")";
    if (!rootJson.is_null()) header += ", under " + rootJson["path"].get<std::string>();
    Json data = {{"scene_id", scene->id}, {"scene_name", scene->name}, {"entity_count", scene->entities.size()}};
    if (!rootJson.is_null()) data["root"] = rootJson;

    if (json) {
        Json nodes = Json::array();
        for (Entity entity : starts) {
            Json node = emitJson(state, entity, 0);
            if (node.is_null()) break;
            nodes.push_back(std::move(node));
        }
        data["nodes"] = std::move(nodes);
        data["truncated"] = state.truncated;
        call->ok(header + (state.truncated ? " (truncated at " + std::to_string(state.limit) + " nodes)" : "") +
                 ": tree in structured content.", data);
        return;
    }
    for (Entity entity : starts) {
        emitText(state, entity, 0);
        if (state.truncated) break;
    }
    std::string text = header + ". Each line: Name [id] components (flags); children indented.\n" + state.text;
    if (state.truncated) {
        text += "... truncated at " + std::to_string(state.limit) + " GameObjects (raise limit, or pass root/depth).\n";
    }
    data["tree"] = state.text;
    data["truncated"] = state.truncated;
    call->ok(text, data);
}

// ------------------------------------------------------------------ find_entities

void findEntitiesTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    if (args.value("runtime", false)) scene = McpRefs::runtimeScene(project, scene);
    Scene* registry = scene->scene;
    std::string error;

    const std::string namePattern = args.value("name", std::string());
    std::vector<ComponentType> components;
    if (args.contains("component")) {
        Json list = args["component"].is_array() ? args["component"] : Json::array({args["component"]});
        for (const Json& item : list) {
            ComponentType type;
            if (!McpRefs::resolveComponent(item, type, error)) {
                call->fail(error);
                return;
            }
            components.push_back(type);
        }
    }
    const bool hasTag = args.contains("tag") && args["tag"].is_string();
    const std::string tag = hasTag ? args["tag"].get<std::string>() : std::string();
    int layer = -1;
    if (args.contains("layer") && !args["layer"].is_null()) {
        if (args["layer"].is_number_integer()) layer = args["layer"].get<int>();
        else if (args["layer"].is_string()) layer = TagManager::nameToLayer(args["layer"].get<std::string>());
        if (layer < 0 || layer >= MAX_LAYERS) {
            call->fail("layer must be 0-31 or a layer name.");
            return;
        }
    }
    Entity under = NULL_ENTITY;
    if (args.contains("under") && !args["under"].is_null()) {
        under = McpRefs::resolveEntity(scene, args["under"], error);
        if (under == NULL_ENTITY) {
            call->fail(error);
            return;
        }
    }
    const std::string pathPattern = args.value("path", std::string());
    const bool filterActive = args.contains("active") && args["active"].is_boolean();
    const bool active = filterActive && args["active"].get<bool>();
    const size_t limit = static_cast<size_t>(std::max(1, args.value("limit", 100)));

    const auto paths = McpRefs::entityPaths(scene);
    std::vector<Entity> candidates = scene->entities;
    if (scene->playState != ScenePlayState::STOPPED) {
        std::set<Entity> known(candidates.begin(), candidates.end());
        for (const auto& [entity, path] : paths) {
            if (!known.count(entity)) candidates.push_back(entity);
        }
    }
    Json found = Json::array();
    size_t total = 0;
    for (Entity entity : candidates) {
        if (!registry->isEntityCreated(entity)) continue;
        if (!namePattern.empty() && !wildcardMatch(namePattern, registry->getEntityName(entity))) continue;
        auto pathIt = paths.find(entity);
        const std::string path = pathIt != paths.end() ? pathIt->second : McpRefs::entityPath(scene, entity);
        if (!pathPattern.empty() && !wildcardMatch(pathPattern, path)) continue;
        if (hasTag && registry->getEntityTag(entity) != tag) continue;
        if (layer >= 0 && registry->getEntityLayer(entity) != layer) continue;
        if (filterActive && registry->isEntityActiveInHierarchy(entity) != active) continue;
        if (under != NULL_ENTITY && (entity == under || !registry->isParentOf(under, entity))) continue;
        bool hasAll = true;
        for (ComponentType type : components) {
            if (!McpRefs::hasComponent(scene, entity, type)) {
                hasAll = false;
                break;
            }
        }
        if (!hasAll) continue;
        total++;
        if (found.size() < limit) {
            Json item = {{"id", entity}, {"name", registry->getEntityName(entity)}, {"path", path}};
            if (args.value("with_components", false)) item["components"] = componentsJson(registry, entity);
            found.push_back(item);
        }
    }
    std::string text = std::to_string(total) + " GameObject(s) match" + (total > found.size() ? " (showing " + std::to_string(found.size()) + ")" : "") + ":";
    for (const Json& item : found) text += "\n" + std::to_string(item["id"].get<Entity>()) + " " + item["path"].get<std::string>();
    call->ok(text, Json{{"scene_id", scene->id}, {"count", total}, {"entities", found}, {"truncated", total > found.size()}});
}

// ------------------------------------------------------------------ create_entity

void createEntityRun(const CallPtr& call, bool waited);

void createEntityTool(const CallPtr& call) {
    createEntityRun(call, false);
}

void createEntityRun(const CallPtr& call, bool waited) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    const uint32_t sceneId = scene->id;
    std::string error;

    const std::string typeName = args.value("type", std::string("empty"));
    const EntityKind* kind = findKind(typeName);
    if (!kind) {
        call->fail("Unknown type \"" + typeName + "\". Types: " + kindNamesText() + ".");
        return;
    }
    Entity parent = NULL_ENTITY;
    if (args.contains("parent") && !args["parent"].is_null()) {
        parent = McpRefs::resolveEntity(scene, args["parent"], error);
        if (parent == NULL_ENTITY) {
            call->fail("parent: " + error);
            return;
        }
        if (!scene->scene->findComponent<Transform>(parent)) {
            call->fail("parent \"" + scene->scene->getEntityName(parent) + "\" has no Transform: it cannot hold children.");
            return;
        }
        if (kind->type == EntityCreationType::EMPTY) {
            call->fail("A bare GameObject (no Transform) can only be a scene root; use type empty.");
            return;
        }
    }
    std::string modelStored;
    if (args.contains("model") && args["model"].is_string() && !args["model"].get<std::string>().empty()) {
        if (kind->type != EntityCreationType::MODEL) {
            call->fail("model is only for type \"model\".");
            return;
        }
        const std::string model = args["model"].get<std::string>();
        fs::path absolute;
        if (!McpRefs::resolveProjectPath(project, model, absolute, error, true)) {
            call->fail("model: " + error);
            return;
        }
        if (!Util::isModelFile(absolute.string())) {
            call->fail("model must be a .gltf, .glb, .obj or .fbx file.");
            return;
        }
        if (!McpValues::assetPathFromJson(project, model, modelStored, error)) {
            call->fail("model: " + error);
            return;
        }
    }
    // type lattice: the editor's creation around targets (fitted to their bounds, bound to them)
    std::vector<Entity> targets;
    if (args.contains("targets") && !args["targets"].is_null()) {
        if (kind->type != EntityCreationType::LATTICE) {
            call->fail("targets is only for type \"lattice\" (the GameObjects it is fitted around and deforms).");
            return;
        }
        if (!McpRefs::resolveEntities(scene, args["targets"], targets, error)) {
            call->fail("targets: " + error);
            return;
        }
        for (Entity target : targets) {
            if (!scene->scene->findComponent<Transform>(target)) {
                call->fail("targets: \"" + scene->scene->getEntityName(target) + "\" has no Transform, so it cannot be deformed.");
                return;
            }
        }
        // geometry made or changed by the previous calls is built in the next frame: fit to it then
        if (!waited && lattice_mcp::targetsPending(project, sceneId, targets)) {
            call->waitUntil([project, sceneId, targets]() { return !lattice_mcp::targetsPending(project, sceneId, targets); },
                            lattice_mcp::TARGETS_WAIT_MS, [call](bool) { createEntityRun(call, true); });
            return;
        }
    }
    std::vector<ComponentType> extraComponents;
    if (args.contains("components")) {
        Json list = args["components"].is_array() ? args["components"] : Json::array({args["components"]});
        for (const Json& item : list) {
            ComponentType type;
            if (!McpRefs::resolveComponent(item, type, error)) {
                call->fail("components: " + error);
                return;
            }
            extraComponents.push_back(type);
        }
    }
    std::vector<McpValues::Change> changes;
    for (const char* key : {"values", "changes"}) {
        if (args.contains(key) && !args[key].is_null()) {
            std::vector<McpValues::Change> parsed;
            if (!McpValues::parseChanges(args[key], parsed, error)) {
                call->fail(std::string(key) + ": " + error);
                return;
            }
            changes.insert(changes.end(), parsed.begin(), parsed.end());
        }
    }
    // transform shortcuts
    for (const char* key : {"position", "rotation", "scale"}) {
        if (args.contains(key) && !args[key].is_null()) {
            McpValues::Change change;
            change.component = ComponentType::Transform;
            change.property = key;
            change.value = args[key];
            changes.push_back(change);
        }
    }

    const std::string name = args.value("name", std::string(kind->label));
    auto created = std::make_shared<Entity>(NULL_ENTITY);
    auto* steps = new McpStepsCmd();
    const bool select = args.value("select", true);
    size_t stepCount = 0;
    // a lattice is made by LatticeEditorOps::createLattice (its own grouped commands, as the
    // Hierarchy's create menu does); the steps below then run on it and everything is one undo step
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t start = history->mark();
    const std::vector<Entity> selectedBefore = project->getSelectedEntities(sceneId);
    Json bound = Json::array(), notes = Json::array();
    if (kind->type == EntityCreationType::LATTICE) {
        McpValues::refreshTransforms(scene);  // targets made or moved this frame measure where they are now
        *created = LatticeEditorOps::createLattice(project, sceneId, targets, Vector3::ZERO, name, &error);
        if (*created == NULL_ENTITY) {
            delete steps;
            call->fail("Nothing was created: " + error);
            return;
        }
        for (Entity target : targets) {
            bool isBound = false;
            if (LatticeModifierComponent* modifier = scene->scene->findComponent<LatticeModifierComponent>(target)) {
                for (const LatticeBinding& binding : modifier->bindings) isBound = isBound || binding.lattice == *created;
            }
            if (isBound) {
                bound.push_back(McpRefs::entityJson(scene, target));
            } else {
                notes.push_back("\"" + scene->scene->getEntityName(target) + "\" has no mesh (nor do its children): not bound.");
            }
        }
        if (parent != NULL_ENTITY) {
            const Entity lattice = *created;
            steps->refreshTransforms(project, sceneId);
            steps->addStep([project, sceneId, lattice, parent](std::string& why) -> Command* {
                why = "The lattice could not be put under the parent (see the console).";
                return new MoveEntityOrderCmd(project, sceneId, lattice, parent, InsertionType::INTO);
            });
            stepCount++;
        }
    } else {
        stepCount++;
        steps->addStep([project, sceneId, name, kind, parent, created, select](std::string& why) -> Command* {
            auto* cmd = parent != NULL_ENTITY ? new CreateEntityCmd(project, sceneId, name, kind->type, parent)
                                              : new CreateEntityCmd(project, sceneId, name, kind->type);
            cmd->setQuiet(!select);
            // the id is known once it ran: the next steps read it through `created`
            struct Capture : public Command {
                CreateEntityCmd* inner;
                std::shared_ptr<Entity> out;
                Capture(CreateEntityCmd* inner, std::shared_ptr<Entity> out): inner(inner), out(std::move(out)) {}
                ~Capture() override { delete inner; }
                bool execute() override {
                    if (!inner->execute()) return false;
                    *out = inner->getEntity();
                    return true;
                }
                void undo() override { inner->undo(); }
                std::string describe() const override { return inner->describe(); }
                const char* typeName() const override { return inner->typeName(); }
                size_t memoryBytes() const override { return sizeof(*this) + inner->memoryBytes(); }
                void collectKeepCommands(KeepContext& context) const override { inner->collectKeepCommands(context); }
            };
            why = "The GameObject could not be created.";
            return new Capture(cmd, created);
        });
    }
    if (kind->type == EntityCreationType::TILEMAP_GRID) {
        stepCount++;
        steps->addStep([project, sceneId, created](std::string& why) -> Command* {
            auto* layer = new CreateEntityCmd(project, sceneId, "Tilemap", EntityCreationType::TILEMAP_LAYER, *created);
            layer->setQuiet(true);
            why = "The Tilemap Layer could not be created.";
            return layer;
        });
    }
    for (ComponentType type : extraComponents) {
        stepCount++;
        steps->addStep([project, sceneId, created, type](std::string& why) -> Command* {
            SceneProject* current = project->getScene(sceneId);
            if (McpRefs::hasComponent(current, *created, type)) return new MultiPropertyCmd();  // the type made it
            why = "Could not add " + McpRefs::componentName(type) + ".";
            return new AddComponentCmd(project, sceneId, *created, type);
        });
    }
    if (!modelStored.empty()) {
        stepCount++;
        steps->addStep([project, sceneId, created, modelStored](std::string&) -> Command* {
            return new ModelLoadCmd(project, sceneId, *created, modelStored);
        });
    }
    if (!changes.empty()) {
        stepCount++;
        steps->addStep([project, sceneId, created, changes](std::string& why) -> Command* {
            auto* multi = new MultiPropertyCmd();
            if (!McpValues::appendChangeCommands(project, project->getScene(sceneId), {*created}, changes, *multi, why)) {
                delete multi;
                return nullptr;
            }
            return multi;
        });
    }
    const bool stepsOk = stepCount == 0 ? (delete steps, true) : runSceneCommand(sceneId, steps, error);
    if (kind->type == EntityCreationType::LATTICE) {
        if (!stepsOk) {
            // a refused undo leaves the position (CommandHistory::undo false): it ends the loop too
            while (history->position() > start && history->canUndo() && history->undo()) {}
        } else {
            history->groupSince(start);
            if (!select) project->replaceSelectedEntities(sceneId, selectedBefore);
        }
    }
    if (!stepsOk) {
        // every step was undone: the GameObject the first step made (and its id) is gone
        if (*created != NULL_ENTITY) {
            const std::string label = "GameObject " + std::to_string(*created) + " \"";
            for (size_t at = error.find(label); at != std::string::npos; at = error.find(label, at)) {
                error.replace(at, label.size(), "the new GameObject \"");
            }
        }
        call->fail("Nothing was created (create_entity is one step and was rolled back): " + error);
        return;
    }
    const Entity entity = *created;
    auto reply = [call, project, sceneId, entity, kind, bound, notes]() {
        SceneProject* current = project->getScene(sceneId);
        Json data = {
            {"entity_id", entity},
            {"scene_id", sceneId},
            {"entity", McpRefs::entityJson(current, entity)},
            {"type", kind->name},
            {"components", componentsJson(current->scene, entity)}
        };
        std::vector<Entity> children;
        for (Entity other : current->entities) {
            if (other != entity && current->scene->isEntityCreated(other) && current->scene->isParentOf(entity, other)) {
                children.push_back(other);
            }
        }
        if (!children.empty()) {
            Json list = Json::array();
            for (Entity child : children) list.push_back(McpRefs::entityJson(current, child));
            data["children"] = list;
        }
        std::string extra;
        if (kind->type == EntityCreationType::LATTICE) {
            data["bound"] = bound;
            if (!notes.empty()) data["notes"] = notes;
            extra = " Bound to " + std::to_string(bound.size()) + " target(s); edit it with edit_lattice.";
            for (const Json& note : notes) extra += " " + note.get<std::string>();
        }
        // a Light 2D made under a bright 2D ambient barely shows (S78 R23)
        if (current->scene->findComponent<Light2DComponent>(entity)) {
            const std::string next = ambientLight2DNext(*current->scene);
            if (!next.empty()) {
                data["next"] = next;
                extra += " " + next;
            }
        }
        call->ok("Created " + std::string(kind->label) + " \"" + current->scene->getEntityName(entity) + "\" [" +
                 std::to_string(entity) + "] at " + data["entity"]["path"].get<std::string>() + " (one undo step)." + extra, data);
    };
    if (!modelStored.empty() && ModelLoadCmd::isLoadPending(sceneId, entity)) {
        call->waitUntil([sceneId, entity]() { return !ModelLoadCmd::isLoadPending(sceneId, entity); }, 120000,
                        [reply](bool) { reply(); });
        return;
    }
    reply();
}

// ------------------------------------------------------------------ delete / duplicate / rename

void deleteEntitiesTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::vector<Entity> entities;
    std::string error;
    if (!McpRefs::resolveEntityArgs(scene, call->args(), entities, error)) {
        call->fail(error);
        return;
    }
    Json deleted = Json::array();
    for (Entity entity : entities) deleted.push_back(McpRefs::entityJson(scene, entity));
    const uint32_t sceneId = scene->id;
    if (!runSceneCommand(sceneId, new DeleteEntityCmd(project, sceneId, entities), error,
                         "The GameObjects could not be deleted (a locked or generated GameObject?).")) {
        call->fail(error);
        return;
    }
    call->ok("Deleted " + std::to_string(entities.size()) + " GameObject(s) with their children (one undo step).",
             Json{{"scene_id", sceneId}, {"deleted", deleted}});
}

void duplicateEntitiesTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::vector<Entity> entities;
    std::string error;
    if (!McpRefs::resolveEntityArgs(scene, call->args(), entities, error)) {
        call->fail(error);
        return;
    }
    const uint32_t sceneId = scene->id;
    auto* cmd = new DuplicateEntityCmd(project, sceneId, entities);
    // the command is deleted by the history on failure: read it only after success
    if (!runSceneCommand(sceneId, cmd, error, "The GameObjects could not be duplicated.")) {
        call->fail(error);
        return;
    }
    SceneProject* current = project->getScene(sceneId);
    Json created = Json::array();
    for (Entity entity : cmd->getCreatedEntities()) created.push_back(McpRefs::entityJson(current, entity));
    std::string text = "Duplicated " + std::to_string(entities.size()) + " GameObject(s) (one undo step); the copies are selected.";
    Json data = {{"scene_id", sceneId}, {"created", created}};
    // copies of a plain GameObject are independent: an object placed several times is a Prefab
    const bool plain = std::any_of(entities.begin(), entities.end(), [current](Entity entity) {
        return current->scene->isEntityCreated(entity) && !current->scene->findComponent<PrefabComponent>(entity);
    });
    if (plain) {
        data["next"] = "The copies are independent GameObjects: for an object placed more than once, make it a Prefab "
                       "(create_prefab) and place instances (instantiate_prefab), so one edit changes every copy.";
        text += " " + data["next"].get<std::string>();
    }
    call->ok(text, data);
}

void renameEntityTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::string error;
    const Entity entity = McpRefs::resolveEntity(scene, call->args().contains("entity") ? call->args()["entity"] : Json(), error);
    if (entity == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    const std::string name = call->args().value("name", std::string());
    if (name.empty()) {
        call->fail("name must not be empty.");
        return;
    }
    const uint32_t sceneId = scene->id;
    if (!runSceneCommand(sceneId, new EntityNameCmd(project, sceneId, entity, name), error)) {
        call->fail(error);
        return;
    }
    call->ok("Renamed to \"" + name + "\".", Json{{"entity", McpRefs::entityJson(project->getScene(sceneId), entity)}});
}

// ------------------------------------------------------------------ move_entity

void moveEntityTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    Scene* registry = scene->scene;
    const uint32_t sceneId = scene->id;
    std::vector<Entity> entities;
    std::string error;
    if (!McpRefs::resolveEntityArgs(scene, args, entities, error)) {
        call->fail(error);
        return;
    }
    for (Entity entity : entities) {
        std::string blocked = MoveEntityOrderCmd::getPrefabRestructureBlock(project, sceneId, entity);
        if (!blocked.empty()) {
            call->fail("\"" + registry->getEntityName(entity) + "\": " + blocked +
                       " (prefab tools: open the prefab, or unpack the instance).");
            return;
        }
    }
    const int modes = (args.contains("before") ? 1 : 0) + (args.contains("after") ? 1 : 0) + (args.contains("parent") ? 1 : 0);
    if (modes != 1) {
        call->fail("Pass exactly one of parent (a GameObject, or null for the scene root; with optional index), "
                   "before or after (a sibling to place next to).");
        return;
    }
    // (source, target, insertion) for each entity, in order
    struct Move { Entity source; Entity target; InsertionType type; };
    std::vector<Move> moves;
    const Hierarchy hierarchy = buildHierarchy(scene);
    if (scene->playState == ScenePlayState::STOPPED) McpValues::refreshTransforms(scene);
    if (args.contains("before") || args.contains("after")) {
        const bool before = args.contains("before");
        Entity anchor = McpRefs::resolveEntity(scene, args[before ? "before" : "after"], error);
        if (anchor == NULL_ENTITY) {
            call->fail(error);
            return;
        }
        // keeps the given order: each after the previous one
        for (Entity entity : entities) {
            if (entity == anchor) {
                call->fail("A GameObject cannot be placed next to itself.");
                return;
            }
            moves.push_back({entity, anchor, before ? InsertionType::BEFORE : InsertionType::AFTER});
            if (!before) anchor = entity;
        }
    } else {
        Entity parent = NULL_ENTITY;
        if (!args["parent"].is_null()) {
            parent = McpRefs::resolveEntity(scene, args["parent"], error);
            if (parent == NULL_ENTITY) {
                call->fail("parent: " + error);
                return;
            }
            if (!registry->findComponent<Transform>(parent)) {
                call->fail("parent has no Transform: it cannot hold children.");
                return;
            }
        }
        std::vector<Entity> siblings;
        if (parent == NULL_ENTITY) {
            siblings = hierarchy.roots;
        } else {
            auto it = hierarchy.children.find(parent);
            if (it != hierarchy.children.end()) siblings = it->second;
        }
        for (Entity entity : entities) {
            if (entity == parent || (parent != NULL_ENTITY && registry->isParentOf(entity, parent))) {
                call->fail("\"" + registry->getEntityName(entity) + "\" cannot become a child of itself or of its own child.");
                return;
            }
            siblings.erase(std::remove(siblings.begin(), siblings.end(), entity), siblings.end());
        }
        // a GameObject with a Transform is placed among the ones with a Transform (one without
        // keeps its own place in the list)
        if (!entities.empty() && registry->findComponent<Transform>(entities.front())) {
            siblings.erase(std::remove_if(siblings.begin(), siblings.end(),
                [registry](Entity sibling) { return !registry->findComponent<Transform>(sibling); }), siblings.end());
        }
        long index = args.contains("index") && args["index"].is_number_integer() ? args["index"].get<long>() : -1;
        if (index < 0 || index > static_cast<long>(siblings.size())) index = static_cast<long>(siblings.size());
        // place the first, then each next after the previous
        Entity previous = NULL_ENTITY;
        for (Entity entity : entities) {
            if (previous != NULL_ENTITY) {
                moves.push_back({entity, previous, InsertionType::AFTER});
            } else if (index < static_cast<long>(siblings.size())) {
                moves.push_back({entity, siblings[static_cast<size_t>(index)], InsertionType::BEFORE});
            } else if (!siblings.empty()) {
                moves.push_back({entity, siblings.back(), InsertionType::AFTER});
            } else if (parent != NULL_ENTITY) {
                moves.push_back({entity, parent, InsertionType::INTO});
            } else {
                moves.push_back({entity, NULL_ENTITY, InsertionType::AFTER});  // alone at the root: nothing to do
            }
            previous = entity;
        }
    }
    // nothing moves when one of them would only reorder the list (refused by the command too)
    for (const Move& move : moves) {
        if (move.target == NULL_ENTITY) continue;
        const std::string problem = MoveEntityOrderCmd::transformAnchorProblem(registry, move.source, move.target, move.type);
        if (!problem.empty()) {
            call->fail(problem + ": place it with parent (and index), or next to a GameObject with a Transform.");
            return;
        }
    }
    // the Hierarchy drag keeps the world pose (the local transform adapts); false keeps the local values
    const bool keepWorld = args.value("keep_world_transform", true);
    auto* steps = new McpStepsCmd();
    steps->refreshTransforms(project, sceneId);
    size_t applied = 0;
    for (const Move& move : moves) {
        if (move.target == NULL_ENTITY) continue;
        const Entity source = move.source;
        Transform* transform = registry->findComponent<Transform>(source);
        const bool hasTransform = transform != nullptr;
        const Vector3 position = hasTransform ? transform->position : Vector3::ZERO;
        const Quaternion rotation = hasTransform ? transform->rotation : Quaternion::IDENTITY;
        const Vector3 scale = hasTransform ? transform->scale : Vector3(1.0f, 1.0f, 1.0f);
        steps->addStep([project, sceneId, move](std::string& why) -> Command* {
            why = "The move did not apply (see the console).";
            return new MoveEntityOrderCmd(project, sceneId, move.source, move.target, move.type);
        });
        if (!keepWorld && hasTransform) {
            steps->addStep([project, sceneId, source, position, rotation, scale](std::string&) -> Command* {
                return new ObjectTransformCmd(project, sceneId, source, position, rotation, scale);
            });
        }
        applied++;
    }
    if (applied == 0) {
        delete steps;
        call->ok("Nothing to move.", Json{{"scene_id", sceneId}});
        return;
    }
    if (!runSceneCommand(sceneId, steps, error)) {
        call->fail(error);
        return;
    }
    SceneProject* current = project->getScene(sceneId);
    Json moved = Json::array();
    for (Entity entity : entities) {
        Json item = McpRefs::entityJson(current, entity);
        item["parent"] = McpValues::entityToJson(current, parentOf(current->scene, entity));
        moved.push_back(item);
    }
    call->ok("Moved " + std::to_string(entities.size()) + " GameObject(s) (one undo step)" +
             (keepWorld ? "; world poses kept (local transforms adapted)." : "; local transform values kept."),
             Json{{"scene_id", sceneId}, {"moved", moved}});
}

// ------------------------------------------------------------------ active / tag / layer

void setEntityStateTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::vector<Entity> entities;
    std::string error;
    if (!McpRefs::resolveEntityArgs(scene, args, entities, error)) {
        call->fail(error);
        return;
    }
    const uint32_t sceneId = scene->id;
    auto* multi = new MultiPropertyCmd();
    bool any = false;
    if (args.contains("active")) {
        if (!args["active"].is_boolean()) {
            delete multi;
            call->fail("active must be true or false.");
            return;
        }
        multi->addCommand(std::unique_ptr<Command>(EntityStateCmd::active(project, sceneId, entities, args["active"].get<bool>())));
        any = true;
    }
    if (args.contains("tag")) {
        const std::string tag = args["tag"].is_string() ? args["tag"].get<std::string>() : std::string();
        if (!tag.empty() && !TagManager::isTagDefined(tag)) {
            std::string tags;
            for (const std::string& known : TagManager::getAllTags()) tags += (tags.empty() ? "" : ", ") + known;
            delete multi;
            call->fail("Tag \"" + tag + "\" is not defined. Tags: " + tags + " (add tags in the project settings).");
            return;
        }
        multi->addCommand(std::unique_ptr<Command>(EntityStateCmd::tag(project, sceneId, entities, tag)));
        any = true;
    }
    if (args.contains("layer")) {
        int layer = -1;
        if (args["layer"].is_number_integer()) layer = args["layer"].get<int>();
        else if (args["layer"].is_string()) layer = TagManager::nameToLayer(args["layer"].get<std::string>());
        if (layer < 0 || layer >= MAX_LAYERS) {
            std::string layers;
            for (int i = 0; i < MAX_LAYERS; i++) {
                const std::string layerName = TagManager::layerToName(i);
                if (!layerName.empty()) layers += (layers.empty() ? "" : ", ") + std::to_string(i) + "=" + layerName;
            }
            delete multi;
            call->fail("layer must be 0-31 or a layer name. Layers: " + layers + ".");
            return;
        }
        multi->addCommand(std::unique_ptr<Command>(EntityStateCmd::layer(project, sceneId, entities, layer)));
        any = true;
    }
    if (!any) {
        delete multi;
        call->fail("Pass active, tag and/or layer.");
        return;
    }
    if (!runSceneCommand(sceneId, multi, error)) {
        call->fail(error);
        return;
    }
    SceneProject* current = project->getScene(sceneId);
    Json list = Json::array();
    for (Entity entity : entities) {
        Json item = McpRefs::entityJson(current, entity);
        item["active"] = current->scene->isEntityActive(entity);
        item["tag"] = current->scene->getEntityTag(entity);
        item["layer"] = current->scene->getEntityLayer(entity);
        list.push_back(item);
    }
    call->ok("Updated " + std::to_string(entities.size()) + " GameObject(s) (one undo step).", Json{{"scene_id", sceneId}, {"entities", list}});
}

// ------------------------------------------------------------------ selection

void getSelectionTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    Json list = Json::array();
    for (Entity entity : project->getSelectedEntities(scene->id)) list.push_back(McpRefs::entityJson(scene, entity));
    std::string text = std::to_string(list.size()) + " selected in scene " + std::to_string(scene->id) + ":";
    for (const Json& item : list) text += "\n" + std::to_string(item["id"].get<Entity>()) + " " + item["path"].get<std::string>();
    call->ok(text, Json{{"scene_id", scene->id}, {"entities", list}});
}

void selectEntitiesTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::vector<Entity> entities;
    std::string error;
    const bool clearing = args.contains("entities") && args["entities"].is_array() && args["entities"].empty();
    if (!clearing && !McpRefs::resolveEntityArgs(scene, args, entities, error)) {
        call->fail(error + " (entities: [] clears the selection)");
        return;
    }
    if (args.value("add", false)) {
        std::vector<Entity> current = project->getSelectedEntities(scene->id);
        for (Entity entity : entities) {
            if (std::find(current.begin(), current.end(), entity) == current.end()) current.push_back(entity);
        }
        entities = current;
    }
    project->replaceSelectedEntities(scene->id, entities);
    if (!entities.empty()) project->setSelectedSceneForProperties(scene->id);
    Json list = Json::array();
    for (Entity entity : project->getSelectedEntities(scene->id)) list.push_back(McpRefs::entityJson(scene, entity));
    call->ok(list.empty() ? "Cleared the selection." : "Selected " + std::to_string(list.size()) + " GameObject(s).",
             Json{{"scene_id", scene->id}, {"entities", list}});
}

// ------------------------------------------------------------------ copy / paste

void copyEntitiesTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::vector<Entity> entities;
    std::string error;
    if (!McpRefs::resolveEntityArgs(scene, call->args(), entities, error)) {
        call->fail(error);
        return;
    }
    const bool cut = call->args().value("cut", false);
    const uint64_t logId = Out::lastLogId();
    const bool done = cut ? EntityClipboard::cutGameObjects(project, scene->id, entities)
                          : EntityClipboard::copyGameObjects(project, scene->id, entities);
    if (!done) {
        std::string logged;
        for (const LogEntry& entry : Out::getLogsSince(logId, 3, true)) logged += (logged.empty() ? "" : " ") + entry.message;
        call->fail(logged.empty() ? "Nothing could be copied." : logged);
        return;
    }
    EntityClipboard::finishPendingWrite();
    call->ok(std::string(cut ? "Cut " : "Copied ") + std::to_string(entities.size()) +
             " GameObject(s) with their children to the clipboard" + (cut ? " (deleted; one undo step)" : "") +
             ". paste_entities pastes them (any scene of this project).",
             Json{{"scene_id", scene->id}, {"count", entities.size()}, {"cut", cut}});
}

void pasteEntitiesTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* scene = sceneArg(call);
    if (!scene) return;
    std::shared_ptr<const GameObjectsClip> clip = EntityClipboard::readGameObjects(project);
    if (!clip) {
        call->fail("The clipboard holds no GameObjects of this project: copy_entities first.");
        return;
    }
    std::string error;
    EntityClipboard::Placement placement = EntityClipboard::Placement::Root;
    Entity anchor = NULL_ENTITY;
    if (args.contains("parent") && !args["parent"].is_null()) {
        anchor = McpRefs::resolveEntity(scene, args["parent"], error);
        placement = EntityClipboard::Placement::Into;
    } else if (args.contains("after") && !args["after"].is_null()) {
        anchor = McpRefs::resolveEntity(scene, args["after"], error);
        placement = EntityClipboard::Placement::After;
    }
    if (placement != EntityClipboard::Placement::Root && anchor == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    const uint32_t sceneId = scene->id;
    auto* cmd = new PasteEntitiesCmd(project, sceneId, clip, placement, anchor);
    if (!runSceneCommand(sceneId, cmd, error, "The paste did not apply.")) {
        call->fail(error);
        return;
    }
    SceneProject* current = project->getScene(sceneId);
    Json created = Json::array();
    for (Entity entity : cmd->getCreatedRoots()) created.push_back(McpRefs::entityJson(current, entity));
    call->ok("Pasted " + std::to_string(created.size()) + " GameObject(s) (one undo step); they are selected.",
             Json{{"scene_id", sceneId}, {"created", created}});
}

} // namespace

void registerEntityTools(McpRegistry& registry) {

    {
        ToolDef tool;
        tool.name = "get_scene_tree";
        tool.title = "Get Scene Tree";
        tool.description =
            "The Hierarchy: each GameObject's id, components (Transform omitted) and flags (inactive, main camera, "
            "selected), children indented. format json: nested {id, name, components, children}.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"root", entityRef("Only this subtree.")},
            {"depth", integer("Max depth (0: start nodes only).", 0, 1000)},
            {"limit", withDefault(integer("", 1, 100000), 400)},
            {"format", withDefault(stringEnum("", {"text", "json"}), "text")},
            {"components", withDefault(boolean(""), true)},
            {"runtime", withDefault(boolean("While playing: the running copy."), false)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getSceneTreeTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "find_entities";
        tool.title = "Find Entities";
        tool.description =
            "Find GameObjects matching all given filters (globs: * ?, case-insensitive).";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"name", string("")},
            {"path", string("")},
            {"component", {{"type", {"string", "array"}}, {"items", {{"type", "string"}}}, {"description", "Required component(s)."}}},
            {"tag", string("")},
            {"layer", {{"type", {"integer", "string"}}, {"description", "0-31 or name."}}},
            {"under", entityRef("Only descendants of this.")},
            {"active", boolean("")},
            {"with_components", withDefault(boolean("Add component names."), false)},
            {"limit", withDefault(integer("", 1, 100000), 100)},
            {"runtime", withDefault(boolean("While playing: the running copy."), false)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = findEntitiesTool;
        registry.addTool(std::move(tool));
    }
    {
        std::vector<std::string> types;
        for (const EntityKind& kind : entityKinds()) types.push_back(kind.name);
        ToolDef tool;
        tool.name = "create_entity";
        tool.title = "Create Entity";
        tool.description =
            "GameObject > Create: parent, name (default: the type's Unity name), local transform, "
            "components, values ({component: "
            "{property: value}} or set_properties changes). empty: Transform only; bare: "
            "no components (root only); model: a project .gltf/.glb/.obj/.fbx; lattice: fitted and bound to "
            "targets; *_tween/*_tracks: animation actions.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"type", withDefault(stringEnum("", types), "empty")},
            {"name", string("")},
            {"parent", entityRef()},
            {"position", vec3("")},
            {"rotation", quat()},
            {"scale", vec3("")},
            {"model", assetPath("")},
            {"targets", entities()},
            {"components", {{"type", {"string", "array"}}, {"items", {{"type", "string"}}}}},
            {"values", anyObject("")},
            {"changes", array(anyObject(""), "")},
            {"select", withDefault(boolean(""), true)}
        });
        tool.handler = createEntityTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "delete_entities";
        tool.title = "Delete Entities";
        tool.description =
            "Deletes GameObjects with their children; in a prefab instance only from that instance (an override).";
        tool.inputSchema = object({{"scene", sceneRef()}, {"entity", entityRef()}, {"entities", entities()}});
        tool.annotations.destructive = true;
        tool.handler = deleteEntitiesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "duplicate_entities";
        tool.title = "Duplicate Entities";
        tool.description =
            "Duplicate GameObjects with their children next to the originals (Ctrl+D) and select the copies "
            "(returned in order).";
        tool.inputSchema = object({{"scene", sceneRef()}, {"entity", entityRef()}, {"entities", entities()}});
        tool.handler = duplicateEntitiesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "rename_entity";
        tool.title = "Rename Entity";
        tool.description =
            "Renames a GameObject.";
        tool.inputSchema = object({{"scene", sceneRef()}, {"entity", entityRef()}, {"name", string("")}}, {"entity", "name"});
        tool.annotations.idempotent = true;
        tool.handler = renameEntityTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "move_entity";
        tool.title = "Move Entity";
        tool.description =
            "Reparent/reorder GameObjects: exactly one of parent (null = root; index among its children, "
            "default last), before or after (a sibling). Several keep their order and world pose unless "
            "keep_world_transform=false. Prefab instance children cannot be rearranged.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"entities", entities()},
            {"parent", nullableEntityRef("")},
            {"index", integer("", 0, 1000000)},
            {"before", entityRef("")},
            {"after", entityRef("")},
            {"keep_world_transform", withDefault(boolean(""), true)}
        });
        tool.handler = moveEntityTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_entity_state";
        tool.title = "Set Entity State";
        tool.description =
            "Sets GameObjects' active (SetActive), tag (a defined tag; \"\" = untagged) and/or layer (0-31 or name).";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"entities", entities()},
            {"active", boolean("")},
            {"tag", string("")},
            {"layer", Json::object({{"type", {"integer", "string"}}})}
        });
        tool.annotations.idempotent = true;
        tool.handler = setEntityStateTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_selection";
        tool.title = "Get Selection";
        tool.description = "The selected GameObjects of a scene: {entities: [{id, name, path}]}.";
        tool.inputSchema = object({{"scene", sceneRef()}});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getSelectionTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "select_entities";
        tool.activity = ToolActivity::Control;  // busy policy (McpServer::busyRefusal)
        tool.title = "Select Entities";
        tool.description =
            "Replace a scene's selection (what the Inspector shows); add=true adds to it; entities: [] clears it.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"entities", entities()},
            {"add", withDefault(boolean(""), false)}
        });
        tool.annotations.idempotent = true;
        tool.attributesUndo = true;  // a selection step of the agent (S24 R40)
        tool.handler = selectEntitiesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "copy_entities";
        tool.activity = ToolActivity::Control;  // busy policy (McpServer::busyRefusal)
        tool.title = "Copy Entities";
        tool.description =
            "Copy GameObjects with their children to the clipboard (Ctrl+C) for paste_entities (any scene of the "
            "project); cut=true also deletes them.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"entities", entities()},
            {"cut", withDefault(boolean(""), false)}
        });
        tool.annotations.destructive = true;  // cut deletes
        tool.handler = copyEntitiesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "paste_entities";
        tool.title = "Paste Entities";
        tool.description =
            "Paste the copy_entities clipboard (Ctrl+V): at the scene root, as last children of parent, or after "
            "`after`; selects them.";
        tool.inputSchema = object({{"scene", sceneRef()}, {"parent", entityRef("")}, {"after", entityRef("")}});
        tool.handler = pasteEntitiesTool;
        registry.addTool(std::move(tool));
    }
}

}
