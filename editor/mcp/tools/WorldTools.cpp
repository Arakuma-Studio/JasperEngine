// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// W6: the specialised editors generic property setting can't express well: terrain brushes,
// tilemaps, sprite sheets and sprite animations, keyframe animations and tweens, physics
// colliders and joints, UI widget trees and rect layouts, particle systems, models, and the
// point lists of polygons / shadow casters / line renderers.
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "mcp/TerrainMaps.h"

#include "Catalog.h"
#include "Project.h"
#include "Stream.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/ModelClearCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/BoneComponent.h"
#include "component/ButtonComponent.h"
#include "component/ImageComponent.h"
#include "component/InputFieldComponent.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/LayoutGroupComponent.h"
#include "component/Light2DComponent.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/ModelComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/PanelComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/PolygonComponent.h"
#include "component/ProgressbarComponent.h"
#include "component/RectTransformComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/ShadowCaster2DComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/SpriteComponent.h"
#include "component/TerrainComponent.h"
#include "component/TextComponent.h"
#include "component/TilemapComponent.h"
#include "component/Transform.h"
#include "component/TranslateTracksComponent.h"
#include "component/TweenComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/AlphaTweenComponent.h"
#include "component/UIComponent.h"
#include "component/CameraComponent.h"
#include "subsystem/UISystem.h"
#include "Engine.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/MeshSystem.h"
#include "lattice/LatticeSolver.h"
#include "util/LatticeTrackKeys.h"
#include "window/AnimationWindow.h"
#include "window/ProjectWindow.h"
#include "window/TerrainEditWindow.h"

#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ common helpers

std::string lowerText(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

// A named choice: "raise" -> value. Unknown names fail listing the valid ones.
template<typename T>
bool pickName(const std::vector<std::pair<const char*, T>>& table, const std::string& name, T& out,
              const std::string& what, std::string& error) {
    const std::string key = lowerText(name);
    for (const auto& item : table) {
        if (key == item.first) {
            out = item.second;
            return true;
        }
    }
    std::vector<std::string> names;
    for (const auto& item : table) names.push_back(item.first);
    error = what + " \"" + name + "\" is not valid. Use one of: " + joinNames(names) + ".";
    return false;
}

template<typename T>
std::string nameOf(const std::vector<std::pair<const char*, T>>& table, T value) {
    for (const auto& item : table) {
        if (item.second == value) return item.first;
    }
    return "unknown";
}

float numberOr(const Json& args, const char* key, float fallback) {
    if (args.is_object() && args.contains(key) && args[key].is_number()) return args[key].get<float>();
    return fallback;
}

int intOr(const Json& args, const char* key, int fallback) {
    if (args.is_object() && args.contains(key) && args[key].is_number()) return static_cast<int>(args[key].get<double>());
    return fallback;
}

bool boolOr(const Json& args, const char* key, bool fallback) {
    if (args.is_object() && args.contains(key) && args[key].is_boolean()) return args[key].get<bool>();
    return fallback;
}

std::string stringOr(const Json& args, const char* key, const std::string& fallback) {
    if (args.is_object() && args.contains(key) && args[key].is_string()) return args[key].get<std::string>();
    return fallback;
}

bool has(const Json& args, const char* key) {
    return args.is_object() && args.contains(key) && !args[key].is_null();
}

bool readVec2(const Json& args, const char* key, Vector2& out, std::string& error) {
    if (!has(args, key)) return true;
    if (!McpValues::parseVector2(args[key], out)) {
        error = std::string(key) + " must be [x, y].";
        return false;
    }
    return true;
}

bool readVec3(const Json& args, const char* key, Vector3& out, std::string& error) {
    if (!has(args, key)) return true;
    if (!McpValues::parseVector3(args[key], out)) {
        error = std::string(key) + " must be [x, y, z].";
        return false;
    }
    return true;
}

// Colors: [r,g,b] or [r,g,b,a], 0-1
bool parseColor4(const Json& value, Vector4& out) {
    if (value.is_array() && value.size() == 3) {
        Vector3 rgb;
        if (!McpValues::parseVector3(value, rgb)) return false;
        out = Vector4(rgb.x, rgb.y, rgb.z, 1.0f);
        return true;
    }
    return McpValues::parseVector4(value, out);
}

bool parseColor3(const Json& value, Vector3& out) {
    if (value.is_array() && value.size() == 4) {
        Vector4 rgba;
        if (!McpValues::parseVector4(value, rgba)) return false;
        out = Vector3(rgba.x, rgba.y, rgba.z);
        return true;
    }
    return McpValues::parseVector3(value, out);
}

// Ease names as Stream writes them (QUAD_IN_OUT); any case, '-' or '_'
bool parseEase(const Json& value, EaseType& out, std::string& error) {
    if (!value.is_string()) {
        error = "ease must be a name like linear, quad_in, cubic_out, sine_in_out, back_out, bounce_out, elastic_out, step.";
        return false;
    }
    std::string name = value.get<std::string>();
    for (char& c : name) c = (c == '-' || c == ' ') ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const EaseType parsed = Stream::stringToEaseType(name);
    if (Stream::easeTypeToString(parsed) != name || parsed == EaseType::CUSTOM) {
        error = "Unknown ease \"" + value.get<std::string>() + "\". Use linear, step, or <quad|cubic|quart|quint|sine|expo|circ|elastic|back|bounce>_<in|out|in_out>.";
        return false;
    }
    out = parsed;
    return true;
}

std::string easeName(EaseType type) {
    return lowerText(Stream::easeTypeToString(type));
}

// Resolves "scene" (default selected) and an entity argument
struct Target {
    Project* project = nullptr;
    SceneProject* scene = nullptr;
    Entity entity = NULL_ENTITY;
    uint32_t sceneId() const { return scene ? scene->id : 0; }
    Scene* engineScene() const { return scene ? scene->scene : nullptr; }
};

bool resolveSceneArg(const CallPtr& call, Target& target) {
    target.project = call->ctx().project();
    std::string error;
    const Json& args = call->args();
    target.scene = McpRefs::resolveScene(target.project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!target.scene || !target.scene->scene) {
        call->fail(error.empty() ? "The scene is not loaded." : error);
        return false;
    }
    return true;
}

bool resolveEntityArg(const CallPtr& call, Target& target, const char* key = "entity") {
    if (!resolveSceneArg(call, target)) return false;
    const Json& args = call->args();
    if (!has(args, key)) {
        call->fail(std::string(key) + " is required: a GameObject id or a name path like \"Player/Gun\".");
        return false;
    }
    std::string error;
    target.entity = McpRefs::resolveEntity(target.scene, args[key], error);
    if (target.entity == NULL_ENTITY) {
        call->fail(error);
        return false;
    }
    return true;
}

Entity resolveRef(SceneProject* scene, const Json& ref, std::string& error) {
    return McpRefs::resolveEntity(scene, ref, error);
}

bool sceneStopped(const CallPtr& call, const Target& target) {
    if (target.scene->playState != ScenePlayState::STOPPED) {
        call->fail("Scene " + std::to_string(target.scene->id) + " is playing; stop play mode before editing it.");
        return false;
    }
    return true;
}

Json entityJson(const Target& target, Entity entity) {
    return McpRefs::entityJson(target.scene, entity);
}

// One undo step for everything a call adds to a scene history: each command goes in with
// setNoMerge, then commit() groups them (CommandHistory::mark / groupSince, as batch does).
class UndoStep {
public:
    explicit UndoStep(uint32_t sceneId): history(CommandHandle::get(sceneId)), start(history->mark()) {}

    // Executes the command; false when it failed (the history deleted it)
    bool add(Command* command) {
        command->setNoMerge();
        return history->addCommand(command);
    }
    bool changed() const { return history->position() > start; }
    void commit() { history->groupSince(start); }
    // Undoes what this step added (a failed call leaves the scene as it was)
    void rollback() {
        while (history->position() > start && history->canUndo()) history->undo();
    }
    // Rolls back and fails the call; when something had been applied, the error says it is
    // all gone (it may name a GameObject the call made)
    void fail(const CallPtr& call, const std::string& error) {
        const bool undone = changed();
        rollback();
        call->fail(undone ? "Nothing was created or changed (the call was rolled back): " + error : error);
    }

private:
    CommandHistory* history;
    size_t start;
};

// Sets Catalog properties of one component from {"propertyName": value, ...} through McpValues
// (every property type W1 supports), appending the commands to `multi`.
bool addCatalogProperties(Project* project, SceneProject* scene, Entity entity, ComponentType component,
                          const Json& props, MultiPropertyCmd& multi, std::string& error) {
    if (!props.is_object()) {
        error = "properties must be an object {\"propertyName\": value}.";
        return false;
    }
    for (auto it = props.begin(); it != props.end(); ++it) {
        PropertyData property = Catalog::findProperty(scene->scene, entity, component, it.key());
        if (!property.ref) {
            std::vector<std::string> names;
            for (const auto& item : Catalog::findEntityProperties(scene->scene, entity, component)) {
                if (item.first.find('[') == std::string::npos) names.push_back(item.first);
            }
            error = Catalog::getComponentName(component, true) + " has no property \"" + it.key() + "\"" +
                    (names.empty() ? std::string(" (or the GameObject lacks the component).") : ". Properties: " + joinNames(names) + ".");
            return false;
        }
        std::string valueError;
        Command* command = McpValues::buildPropertyCommand(project, scene->id, entity, component, it.key(), property,
                                                           it.value(), valueError);
        if (!command) {
            error = Catalog::getComponentName(component, true) + "." + it.key() + ": " + valueError;
            return false;
        }
        multi.addCommand(std::unique_ptr<Command>(command));
    }
    return true;
}

// {"component name": {"property": value}} for several components
bool addComponentProperties(Project* project, SceneProject* scene, Entity entity, const Json& components,
                            MultiPropertyCmd& multi, std::string& error) {
    if (!components.is_object()) {
        error = "properties must be {\"component\": {\"property\": value}}, e.g. {\"text\": {\"fontSize\": 32}}.";
        return false;
    }
    for (auto it = components.begin(); it != components.end(); ++it) {
        ComponentType type;
        if (!McpRefs::resolveComponent(it.key(), type, error)) return false;
        if (!addCatalogProperties(project, scene, entity, type, it.value(), multi, error)) return false;
    }
    return true;
}

template<typename T>
void addProp(MultiPropertyCmd& multi, const Target& target, Entity entity, ComponentType component,
             const std::string& name, const T& value) {
    multi.addPropertyCmd<T>(target.project, target.sceneId(), entity, component, name, value);
}

// Image size from the file header (PNG/JPG/TGA/BMP...); false for SVG and unreadable files
bool imageSize(Project* project, const std::string& storedPath, int& width, int& height) {
    if (storedPath.empty()) return false;
    fs::path full = project->resolveAssetPath(storedPath);
    int channels = 0;
    return stbi_info(full.string().c_str(), &width, &height, &channels) != 0 && width > 0 && height > 0;
}

std::string meshTexturePath(Scene* scene, Entity entity, int submesh) {
    MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    if (!mesh || submesh < 0 || submesh >= static_cast<int>(mesh->numSubmeshes)) return {};
    return mesh->submeshes[submesh].material.baseColorTexture.getPath();
}

// Sets MeshComponent submeshes[N].material.baseColorTexture from a project-relative path
bool addMeshTexture(const Target& target, Entity entity, int submesh, const Json& path, MultiPropertyCmd& multi, std::string& error) {
    const std::string name = "submeshes[" + std::to_string(submesh) + "].material.baseColorTexture";
    PropertyData property = Catalog::findProperty(target.engineScene(), entity, ComponentType::MeshComponent, name);
    if (!property.ref) {
        error = "The GameObject has no mesh submesh " + std::to_string(submesh) + " to hold a texture.";
        return false;
    }
    if (path.is_string()) {
        std::filesystem::path absolute;
        if (!path.get<std::string>().empty() &&
            !McpRefs::resolveProjectPath(target.project, path.get<std::string>(), absolute, error, true)) {
            return false;
        }
    }
    Command* command = McpValues::buildPropertyCommand(target.project, target.sceneId(), entity, ComponentType::MeshComponent,
                                                       name, property, path, error);
    if (!command) return false;
    multi.addCommand(std::unique_ptr<Command>(command));
    return true;
}

// ================================================================== terrain

const std::vector<std::pair<const char*, TerrainBrushMode>>& brushModes() {
    static const std::vector<std::pair<const char*, TerrainBrushMode>> table = {
        {"raise", TerrainBrushMode::Raise}, {"lower", TerrainBrushMode::Lower},
        {"smooth", TerrainBrushMode::Smooth}, {"flatten", TerrainBrushMode::Flatten},
        {"sharpen", TerrainBrushMode::Sharpen}, {"noise", TerrainBrushMode::Noise},
        {"terrace", TerrainBrushMode::Terrace}, {"stamp", TerrainBrushMode::Stamp},
        {"erode", TerrainBrushMode::Erode}, {"ramp", TerrainBrushMode::Ramp},
        {"paint_base", TerrainBrushMode::PaintBase}, {"paint_layer", TerrainBrushMode::PaintLayer},
        {"paint_density", TerrainBrushMode::PaintDensity}, {"erase_density", TerrainBrushMode::EraseDensity},
        {"place_object", TerrainBrushMode::PlaceObject}, {"erase_object", TerrainBrushMode::EraseObject}
    };
    return table;
}

// The terrain GameObject: `entity`, or the scene's only terrain, or the selected one
bool resolveTerrain(const CallPtr& call, Target& target) {
    if (!resolveSceneArg(call, target)) return false;
    Scene* scene = target.engineScene();
    if (has(call->args(), "entity")) {
        std::string error;
        target.entity = resolveRef(target.scene, call->args()["entity"], error);
        if (target.entity == NULL_ENTITY) {
            call->fail(error);
            return false;
        }
    } else {
        std::vector<Entity> terrains;
        for (Entity entity : target.scene->entities) {
            if (scene->findComponent<TerrainComponent>(entity)) terrains.push_back(entity);
        }
        if (terrains.size() == 1) {
            target.entity = terrains[0];
        } else {
            const std::vector<Entity> selected = target.project->getSelectedEntities(target.scene->id);
            if (selected.size() == 1 && scene->findComponent<TerrainComponent>(selected[0])) {
                target.entity = selected[0];
            } else {
                call->fail(terrains.empty() ? "The scene has no terrain. Create one with create_entity type terrain."
                                            : "The scene has several terrains: pass entity.");
                return false;
            }
        }
    }
    if (!scene->findComponent<TerrainComponent>(target.entity) || !scene->findComponent<Transform>(target.entity)) {
        call->fail("\"" + scene->getEntityName(target.entity) + "\" has no Terrain component.");
        return false;
    }
    return true;
}

// [x, z] or [x, y, z] (y ignored) in world or terrain-local space -> terrain-local XZ
bool terrainPoint(const Json& value, bool world, const Matrix4& inverseModel, float worldY, Vector3& local) {
    if (!value.is_array() || (value.size() != 2 && value.size() != 3)) return false;
    for (const Json& n : value) {
        if (!n.is_number()) return false;
    }
    const float x = value[0].get<float>();
    const float z = value.size() == 2 ? value[1].get<float>() : value[2].get<float>();
    if (!world) {
        local = Vector3(x, 0.0f, z);
        return true;
    }
    const Vector3 p = inverseModel * Vector3(x, worldY, z);
    local = Vector3(p.x, 0.0f, p.z);
    return true;
}

Json terrainSample(Scene* scene, Entity entity, const Vector3& local) {
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    Transform& transform = scene->getComponent<Transform>(entity);
    float height = 0.0f;
    Vector3 normal(0.0f, 1.0f, 0.0f);
    scene->getSystem<MeshSystem>()->sampleTerrainSurface(terrain, local.x, local.z, height, normal);
    const Vector3 world = transform.modelMatrix * Vector3(local.x, height, local.z);
    const float normalized = std::abs(terrain.maxHeight) > 0.0f ? height / terrain.maxHeight : 0.0f;
    return Json{{"world", McpValues::toJson(world)}, {"local", McpValues::toJson(Vector3(local.x, height, local.z))},
                {"height", world.y}, {"normalized", normalized}, {"normal", McpValues::toJson(normal)}};
}

Json terrainInfo(Project* project, Scene* scene, Entity entity) {
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    Transform& transform = scene->getComponent<Transform>(entity);
    Json blendMaps = Json::array();
    for (Texture& map : terrain.blendMaps) blendMaps.push_back(McpValues::assetPathToJson(project, map.getPath()));
    Json foliage = Json::array();
    for (const TerrainFoliageLayer& layer : terrain.foliageLayers) {
        foliage.push_back({{"mesh", McpValues::assetPathToJson(project, layer.meshPath)}, {"density", layer.density}});
    }
    const float half = terrain.terrainSize * 0.5f;
    return Json{
        {"size", terrain.terrainSize}, {"max_height", terrain.maxHeight},
        {"position", McpValues::toJson(transform.worldPosition)},
        {"local_bounds", {{"min", {-half, -half}}, {"max", {half, half}}}},
        {"height_map", McpValues::assetPathToJson(project, terrain.heightMap.getPath())},
        {"height_map_loaded", terrain.heightMap.hasData()},
        {"blend_maps", blendMaps}, {"surface_layers", terrain.surfaceLayers.size()}, {"foliage_layers", foliage}
    };
}

void sculptTerrainTool(const CallPtr& call) {
    Target target;
    if (!resolveTerrain(call, target) || !sceneStopped(call, target)) return;
    TerrainEditWindow* window = call->ctx().terrainEditWindow();
    if (!window) {
        call->fail("The Terrain window is not available.");
        return;
    }
    const Json& args = call->args();
    if (!args.contains("strokes") || !args["strokes"].is_array() || args["strokes"].empty()) {
        call->fail("strokes must be a non-empty array of {mode, points, ...}.");
        return;
    }
    Scene* scene = target.engineScene();
    Transform& transform = scene->getComponent<Transform>(target.entity);
    const Matrix4 inverseModel = transform.modelMatrix.inverse();
    const bool world = stringOr(args, "space", "world") != "local";

    UndoStep step(target.sceneId());
    Json results = Json::array();
    for (size_t s = 0; s < args["strokes"].size(); s++) {
        const Json& item = args["strokes"][s];
        const std::string where = "strokes[" + std::to_string(s) + "]";
        std::string error;
        TerrainScriptedBrush brush;
        if (!item.is_object() || !pickName(brushModes(), stringOr(item, "mode", "raise"), brush.mode, where + ".mode", error)) {
            step.fail(call, error.empty() ? where + " must be an object." : error);
            return;
        }
        static const std::vector<std::pair<const char*, TerrainBrushShape>> shapes = {
            {"circle", TerrainBrushShape::Circle}, {"square", TerrainBrushShape::Square}};
        static const std::vector<std::pair<const char*, TerrainBrushFalloff>> falloffs = {
            {"smooth", TerrainBrushFalloff::Smooth}, {"linear", TerrainBrushFalloff::Linear}, {"constant", TerrainBrushFalloff::Constant}};
        if (!pickName(shapes, stringOr(item, "shape", "circle"), brush.shape, where + ".shape", error) ||
            !pickName(falloffs, stringOr(item, "falloff", "smooth"), brush.falloff, where + ".falloff", error)) {
            step.fail(call, error);
            return;
        }
        brush.size = numberOr(item, "size", 4.0f);
        brush.strength = numberOr(item, "strength", 0.5f);
        brush.rotation = numberOr(item, "rotation", 0.0f);
        brush.flattenHeight = numberOr(item, "flatten_height", -1.0f);
        brush.terraceSteps = intOr(item, "terrace_steps", 8);
        brush.noiseSize = numberOr(item, "noise_size", 4.0f);
        brush.textureLayer = intOr(item, "layer", 0);
        brush.foliageLayer = intOr(item, "layer", 0);
        brush.normalizeBlend = boolOr(item, "normalize", true);
        brush.secondsPerPoint = numberOr(item, "seconds_per_point", 0.05f);
        brush.mapResolution = intOr(args, "map_resolution", 0);
        if (has(item, "mask")) {
            std::string stored;
            if (!McpValues::assetPathFromJson(target.project, stringOr(item, "mask", ""), stored, error)) {
                step.fail(call, where + ".mask: " + error);
                return;
            }
            brush.maskPath = stored;
        }
        if (has(item, "asset")) {
            std::string stored;
            if (!McpValues::assetPathFromJson(target.project, stringOr(item, "asset", ""), stored, error)) {
                step.fail(call, where + ".asset: " + error);
                return;
            }
            brush.placeAssetPath = stored;
        }
        brush.placeInstanced = boolOr(item, "instanced", true);
        brush.placeSpacing = numberOr(item, "spacing", 2.0f);
        brush.placeMinScale = numberOr(item, "min_scale", 1.0f);
        brush.placeMaxScale = numberOr(item, "max_scale", brush.placeMinScale);
        brush.placeRotationJitter = numberOr(item, "rotation_jitter", 1.0f);
        brush.placeAlignToNormal = numberOr(item, "align_to_normal", 0.0f);

        if (!item.contains("points") || !item["points"].is_array() || item["points"].empty()) {
            step.fail(call, where + ".points must be a non-empty array of [x, z] (or [x, y, z]) points.");
            return;
        }
        std::vector<Vector3> points;
        for (const Json& p : item["points"]) {
            Vector3 local;
            if (!terrainPoint(p, world, inverseModel, transform.worldPosition.y, local)) {
                step.fail(call, where + ".points: each point is [x, z] or [x, y, z] numbers.");
                return;
            }
            points.push_back(local);
        }
        int placed = 0;
        if (!window->scriptedStroke(target.sceneId(), target.entity, brush, points, error, &placed)) {
            step.fail(call, where + ": " + error);
            return;
        }
        Json result = {{"mode", nameOf(brushModes(), brush.mode)}, {"points", points.size()}};
        if (brush.mode == TerrainBrushMode::PlaceObject || brush.mode == TerrainBrushMode::EraseObject) {
            result[brush.mode == TerrainBrushMode::PlaceObject ? "placed" : "erased"] = placed;
        } else {
            Json after = Json::array();
            for (size_t i = 0; i < points.size() && i < 8; i++) after.push_back(terrainSample(scene, target.entity, points[i])["height"]);
            result["heights_after"] = after;
        }
        results.push_back(result);
    }
    step.commit();
    call->ok("Applied " + std::to_string(results.size()) + " brush stroke(s) to \"" + scene->getEntityName(target.entity) +
             "\" as one undo step.",
             Json{{"terrain", entityJson(target, target.entity)}, {"strokes", results}, {"changed", step.changed()}});
}

// set_terrain_maps: generated or assigned height/blend maps and surface layer textures, one undo step
void setTerrainMapsTool(const CallPtr& call) {
    Target target;
    if (!resolveTerrain(call, target) || !sceneStopped(call, target)) return;
    const Json& args = call->args();
    Project* project = target.project;
    Scene* scene = target.engineScene();
    const uint32_t sceneId = target.sceneId();
    const Entity entity = target.entity;
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    std::string error;

    // A project-relative texture path -> the stored (assets-relative) Texture
    auto texturePath = [&](const Json& value, const std::string& what, Texture& out) {
        std::filesystem::path absolute;
        std::string stored;
        if (!value.is_string() || !McpRefs::resolveProjectPath(project, value.get<std::string>(), absolute, error, true) ||
            !McpValues::assetPathFromJson(project, value.get<std::string>(), stored, error)) {
            if (error.empty()) error = what + " must be a project-relative image path.";
            else error = what + ": " + error;
            return false;
        }
        out = Texture(stored);
        return true;
    };
    auto changed = [project, sceneId, entity](bool height) {
        return [project, sceneId, entity, height]() {
            SceneProject* sp = project ? project->getScene(sceneId) : nullptr;
            TerrainComponent* t = sp && sp->scene ? sp->scene->findComponent<TerrainComponent>(entity) : nullptr;
            if (!t) return;
            if (height) {
                t->heightMapLoaded = false;
                t->heightMap.invalidateRender();
            }
            t->needUpdateTerrain = height;
            t->needUpdateTexture = true;
        };
    };

    auto multi = std::make_unique<MultiPropertyCmd>();
    Json result = Json::object();
    if (has(args, "heightmap")) {
        const Json& spec = args["heightmap"];
        Texture height;
        if (spec.is_object()) {
            static const std::vector<std::string> modes = {"flat", "middle", "random_noise", "fractal_noise"};
            TerrainHeightmapRequest request;
            request.mode = stringOr(spec, "generate", "middle");
            if (std::find(modes.begin(), modes.end(), request.mode) == modes.end()) {
                call->fail("heightmap.generate must be flat, middle, random_noise or fractal_noise.");
                return;
            }
            request.resolution = std::clamp(intOr(spec, "resolution", project->getTerrainEditorSettings().heightMapResolution), 2, 4096);
            request.baseHeight = request.mode == "middle" ? 0.5f : numberOr(spec, "base_height", request.mode == "flat" ? 0.0f : 0.5f);
            request.amplitude = numberOr(spec, "amplitude", 0.35f);
            request.frequency = numberOr(spec, "frequency", 4.0f);
            request.octaves = intOr(spec, "octaves", 4);
            request.seed = has(spec, "seed") && spec["seed"].is_number_integer()
                ? static_cast<uint32_t>(spec["seed"].get<long long>()) : std::random_device{}();
            TerrainHeightmapResult generated = TerrainMaps::createHeightmap(project, sceneId, entity, request);
            if (!generated.success) {
                call->fail(generated.error.empty() ? "Could not write the heightmap." : generated.error);
                return;
            }
            height = generated.texture;
            result["heightmap"] = {{"path", McpValues::assetPathToJson(project, generated.relativePath)},
                                   {"resolution", request.resolution}, {"seed", request.seed}};
        } else if (!texturePath(spec, "heightmap", height)) {
            call->fail(error);
            return;
        } else {
            result["heightmap"] = {{"path", spec}};
        }
        multi->addPropertyCmd<Texture>(project, sceneId, entity, ComponentType::TerrainComponent, "heightMap", height, changed(true));
    }

    std::vector<Texture> blendMaps = terrain.blendMaps;
    bool blendChanged = false;
    if (has(args, "blendmap")) {
        const Json& spec = args["blendmap"];
        Texture blend;
        if (spec.is_object()) {
            Vector3 color(0, 0, 0);
            if (has(spec, "color") && !McpValues::parseVector3(spec["color"], color)) {
                call->fail("blendmap.color must be [r, g, b] (0-1: the weights of layers 0, 1, 2).");
                return;
            }
            TerrainBlendmapRequest request;
            request.resolution = std::clamp(intOr(spec, "resolution", project->getTerrainEditorSettings().blendMapResolution), 2, 4096);
            request.red = color.x;
            request.green = color.y;
            request.blue = color.z;
            TerrainBlendmapResult generated = TerrainMaps::createBlendmap(project, sceneId, entity, request);
            if (!generated.success) {
                call->fail(generated.error.empty() ? "Could not write the blend map." : generated.error);
                return;
            }
            blend = Texture(generated.relativePath);
            if (ProjectWindow* window = call->ctx().projectWindow()) window->requestThumbnailGeneration(generated.fullPath, true);
            result["blendmap"] = {{"path", McpValues::assetPathToJson(project, generated.relativePath)}, {"resolution", request.resolution}};
        } else if (!texturePath(spec, "blendmap", blend)) {
            call->fail(error);
            return;
        } else {
            result["blendmap"] = {{"path", spec}};
        }
        if (blendMaps.empty()) blendMaps.resize(1);
        blendMaps[0] = blend;
        blendChanged = true;
    }

    std::vector<TerrainSurfaceLayer> layers = terrain.surfaceLayers;
    bool layersChanged = false;
    if (has(args, "layers")) {
        if (!args["layers"].is_array()) {
            call->fail("layers must be an array of {index, color, normal, ...}.");
            return;
        }
        static const std::vector<std::pair<const char*, Texture TerrainSurfaceLayer::*>> maps = {
            {"color", &TerrainSurfaceLayer::colorTexture}, {"normal", &TerrainSurfaceLayer::normalTexture},
            {"roughness", &TerrainSurfaceLayer::roughnessTexture}, {"metallic", &TerrainSurfaceLayer::metallicTexture},
            {"occlusion", &TerrainSurfaceLayer::occlusionTexture}, {"height", &TerrainSurfaceLayer::heightTexture}};
        for (const Json& item : args["layers"]) {
            const int index = intOr(item, "index", -1);
            if (index < 0 || index >= MAX_TERRAIN_LAYERS) {
                call->fail("layers[].index must be 0-" + std::to_string(MAX_TERRAIN_LAYERS - 1) + ".");
                return;
            }
            if (layers.size() <= static_cast<size_t>(index)) layers.resize(index + 1);
            TerrainSurfaceLayer& layer = layers[index];
            const std::string where = "layers[" + std::to_string(index) + "].";
            if (has(item, "pbr")) layer.pbr = boolOr(item, "pbr", layer.pbr);
            if (has(item, "stochastic_tiling")) {
                if (!item["stochastic_tiling"].is_boolean()) {
                    call->fail(where + "stochastic_tiling must be true or false.");
                    return;
                }
                layer.stochasticTiling = item["stochastic_tiling"].get<bool>();
            }
            // Stochastic Tiling's parameters, in the inspector's ranges
            struct StochasticParam { const char* name; float TerrainSurfaceLayer::*field; float lo, hi; const char* meaning; };
            static const StochasticParam stochasticParams[] = {
                {"cell_size", &TerrainSurfaceLayer::stochasticCellSize, 0.25f, 4.0f, "the cell width in texture repeats, default 1"},
                {"blend_sharpness", &TerrainSurfaceLayer::stochasticBlendSharpness, 0.0f, 1.0f, "default 0.4"},
                {"contrast", &TerrainSurfaceLayer::stochasticContrast, 0.0f, 1.0f, "default 0.45, 0 blends linearly"},
                {"rotation", &TerrainSurfaceLayer::stochasticRotation, 0.0f, 1.0f, "share of +-180 degrees per cell, default 0"}};
            for (const StochasticParam& param : stochasticParams) {
                if (!has(item, param.name)) continue;
                const Json& value = item[param.name];
                const double number = value.is_number() ? value.get<double>() : -1.0;
                if (!value.is_number() || !std::isfinite(number) || number < param.lo || number > param.hi) {
                    std::ostringstream range;
                    range << where << param.name << " must be a number from " << param.lo << " to " << param.hi << " (" << param.meaning << ").";
                    call->fail(range.str());
                    return;
                }
                layer.*param.field = static_cast<float>(number);
                if (!layer.stochasticTiling) {
                    result["notes"].push_back(where + std::string(param.name) + " is stored but has no effect until stochastic_tiling is true.");
                }
            }
            for (const auto& map : maps) {
                if (!has(item, map.first)) continue;
                if (!texturePath(item[map.first], "layers[" + std::to_string(index) + "]." + map.first, layer.*map.second)) {
                    call->fail(error);
                    return;
                }
            }
            layersChanged = true;
        }
        result["layers"] = layers.size();
        Json stochastic = Json::array();
        for (size_t i = 0; i < layers.size(); i++) {
            const TerrainSurfaceLayer& layer = layers[i];
            if (!layer.stochasticTiling) continue;
            stochastic.push_back({{"index", i}, {"cell_size", layer.stochasticCellSize}, {"blend_sharpness", layer.stochasticBlendSharpness},
                                  {"contrast", layer.stochasticContrast}, {"rotation", layer.stochasticRotation}});
        }
        if (!stochastic.empty()) result["stochastic_tiling"] = stochastic;
    }
    if (blendChanged) {
        multi->addPropertyCmd<std::vector<Texture>>(project, sceneId, entity, ComponentType::TerrainComponent, "blendMaps",
                                                    blendMaps, changed(false));
    }
    if (layersChanged) {
        multi->addPropertyCmd<std::vector<TerrainSurfaceLayer>>(project, sceneId, entity, ComponentType::TerrainComponent,
                                                                "surfaceLayers", layers, changed(false));
    }
    if (result.empty()) {
        call->fail("Pass heightmap, blendmap and/or layers.");
        return;
    }
    if (!runSceneCommand(sceneId, multi.release(), error, "Could not change the terrain maps.")) {
        call->fail(error);
        return;
    }
    result["terrain"] = entityJson(target, entity);
    result["info"] = terrainInfo(project, scene, entity);
    call->ok("Set the terrain maps of " + McpRefs::entityPath(target.scene, entity) + ".", result);
}

void sampleTerrainTool(const CallPtr& call) {
    Target target;
    if (!resolveTerrain(call, target)) return;
    const Json& args = call->args();
    Scene* scene = target.engineScene();
    Transform& transform = scene->getComponent<Transform>(target.entity);
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(target.entity);
    const Matrix4 inverseModel = transform.modelMatrix.inverse();
    const bool world = stringOr(args, "space", "world") != "local";

    Json data = {{"terrain", entityJson(target, target.entity)}, {"info", terrainInfo(target.project, scene, target.entity)}};
    if (!terrain.heightMap.hasData()) {
        data["warning"] = "The height map has no pixel data loaded (none yet, or still loading): heights read as 0.";
    }
    if (has(args, "points")) {
        if (!args["points"].is_array()) {
            call->fail("points must be an array of [x, z] points.");
            return;
        }
        Json samples = Json::array();
        for (const Json& p : args["points"]) {
            Vector3 local;
            if (!terrainPoint(p, world, inverseModel, transform.worldPosition.y, local)) {
                call->fail("points: each point is [x, z] or [x, y, z] numbers.");
                return;
            }
            samples.push_back(terrainSample(scene, target.entity, local));
        }
        data["samples"] = samples;
    }
    const int grid = intOr(args, "grid", has(args, "points") ? 0 : 9);
    if (grid >= 2) {
        const int n = std::min(grid, 65);
        const float half = terrain.terrainSize * 0.5f;
        Json rows = Json::array();
        float lo = std::numeric_limits<float>::max(), hi = -std::numeric_limits<float>::max(), sum = 0.0f;
        for (int r = 0; r < n; r++) {
            Json row = Json::array();
            const float z = -half + terrain.terrainSize * static_cast<float>(r) / static_cast<float>(n - 1);
            for (int c = 0; c < n; c++) {
                const float x = -half + terrain.terrainSize * static_cast<float>(c) / static_cast<float>(n - 1);
                const float h = terrainSample(scene, target.entity, Vector3(x, 0.0f, z))["height"].get<float>();
                lo = std::min(lo, h);
                hi = std::max(hi, h);
                sum += h;
                row.push_back(std::round(h * 1000.0f) / 1000.0f);
            }
            rows.push_back(row);
        }
        data["grid"] = {{"size", n}, {"rows_from", "local -z to +z"}, {"columns_from", "local -x to +x"},
                        {"world_heights", rows}, {"min", lo}, {"max", hi}, {"mean", sum / static_cast<float>(n * n)}};
    }
    call->ok("Sampled terrain \"" + scene->getEntityName(target.entity) + "\".", data);
}

// ================================================================== physics

const std::vector<std::pair<const char*, BodyType>>& bodyTypes() {
    static const std::vector<std::pair<const char*, BodyType>> table = {
        {"static", BodyType::STATIC}, {"kinematic", BodyType::KINEMATIC}, {"dynamic", BodyType::DYNAMIC}};
    return table;
}

const std::vector<std::pair<const char*, Collider2DType>>& collider2DTypes() {
    static const std::vector<std::pair<const char*, Collider2DType>> table = {
        {"box", Collider2DType::POLYGON}, {"polygon", Collider2DType::POLYGON}, {"circle", Collider2DType::CIRCLE},
        {"capsule", Collider2DType::CAPSULE}, {"segment", Collider2DType::SEGMENT}, {"chain", Collider2DType::CHAIN}};
    return table;
}

const std::vector<std::pair<const char*, Collider3DType>>& collider3DTypes() {
    static const std::vector<std::pair<const char*, Collider3DType>> table = {
        {"box", Collider3DType::BOX}, {"sphere", Collider3DType::SPHERE}, {"capsule", Collider3DType::CAPSULE},
        {"tapered_capsule", Collider3DType::TAPERED_CAPSULE}, {"cylinder", Collider3DType::CYLINDER},
        {"convex_hull", Collider3DType::CONVEX_HULL}, {"mesh", Collider3DType::MESH}, {"heightfield", Collider3DType::HEIGHTFIELD}};
    return table;
}

// Local 2D bounds of what the GameObject draws: sprite (with its pivot), mesh AABB, or rect size
bool bounds2D(Scene* scene, Entity entity, Vector2& minOut, Vector2& maxOut, std::string& source) {
    if (SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity)) {
        if (sprite->width > 0 && sprite->height > 0) {
            const float w = static_cast<float>(sprite->width), h = static_cast<float>(sprite->height);
            float px = 0.0f, py = 0.0f;
            switch (sprite->pivotPreset) {
                case PivotPreset::CENTER: px = w * 0.5f; py = h * 0.5f; break;
                case PivotPreset::TOP_CENTER: px = w * 0.5f; py = h; break;
                case PivotPreset::BOTTOM_CENTER: px = w * 0.5f; break;
                case PivotPreset::LEFT_CENTER: py = h * 0.5f; break;
                case PivotPreset::RIGHT_CENTER: px = w; py = h * 0.5f; break;
                case PivotPreset::TOP_LEFT: py = h; break;
                case PivotPreset::BOTTOM_LEFT: break;
                case PivotPreset::TOP_RIGHT: px = w; py = h; break;
                case PivotPreset::BOTTOM_RIGHT: px = w; break;
            }
            minOut = Vector2(-px, -py);
            maxOut = Vector2(w - px, h - py);
            source = "sprite";
            return true;
        }
    }
    if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
        const Vector3 lo = mesh->aabb.getMinimum(), hi = mesh->aabb.getMaximum();
        if (std::isfinite(lo.x) && std::isfinite(hi.x) && hi.x > lo.x && hi.y > lo.y) {
            minOut = Vector2(lo.x, lo.y);
            maxOut = Vector2(hi.x, hi.y);
            source = "mesh";
            return true;
        }
    }
    if (RectTransformComponent* rect = scene->findComponent<RectTransformComponent>(entity)) {
        if (rect->width > 0 && rect->height > 0) {
            minOut = Vector2(0.0f, 0.0f);
            maxOut = Vector2(static_cast<float>(rect->width), static_cast<float>(rect->height));
            source = "rect";
            return true;
        }
    }
    return false;
}

bool parsePoints2D(const Json& value, std::vector<Vector2>& out, std::string& error) {
    if (!value.is_array()) {
        error = "points must be an array of [x, y].";
        return false;
    }
    for (const Json& p : value) {
        Vector2 v;
        if (!McpValues::parseVector2(p, v)) {
            error = "points must be an array of [x, y].";
            return false;
        }
        out.push_back(v);
    }
    return true;
}

// Applies a 2D collider spec onto `shape` (a copy of an existing collider or a default one)
bool applyCollider2D(Scene* scene, Entity entity, const Json& spec, Collider2D& shape, bool isNew, std::string& error) {
    std::string shapeName = isNew ? stringOr(spec, "shape", "box") : stringOr(spec, "shape", "");
    if (!shapeName.empty() && !pickName(collider2DTypes(), shapeName, shape.type, "shape", error)) return false;
    shapeName = lowerText(shapeName);

    // fit: size the shape from what the GameObject draws (sprite, mesh or rect)
    Vector2 lo(0.0f, 0.0f), hi(0.0f, 0.0f);
    std::string source;
    const bool wantFit = has(spec, "fit") ? boolOr(spec, "fit", false) : isNew;
    const bool fitted = wantFit && bounds2D(scene, entity, lo, hi, source);
    if (has(spec, "fit") && boolOr(spec, "fit", false) && !fitted) {
        error = "fit needs a sprite with a size, a loaded mesh, or a RectTransform to measure.";
        return false;
    }

    Vector2 center = fitted ? (lo + hi) * 0.5f : Vector2(0.0f, 0.0f);
    Vector2 size = fitted ? (hi - lo) : Vector2(100.0f, 100.0f);
    if (!readVec2(spec, "center", center, error) || !readVec2(spec, "size", size, error)) return false;
    if (has(spec, "offset")) {
        Vector2 offset;
        if (!readVec2(spec, "offset", offset, error)) return false;
        center = center + offset;
    }

    if (shapeName == "box" || (isNew && shapeName.empty())) {
        if (size.x <= 0.0f || size.y <= 0.0f) {
            error = "size must be positive [width, height].";
            return false;
        }
        const Vector2 half = size * 0.5f;
        shape.numVertices = 4;
        shape.vertices[0] = center + Vector2(-half.x, -half.y);
        shape.vertices[1] = center + Vector2(half.x, -half.y);
        shape.vertices[2] = center + Vector2(half.x, half.y);
        shape.vertices[3] = center + Vector2(-half.x, half.y);
        shape.pointA = center;
    } else if (shape.type == Collider2DType::CIRCLE) {
        if (!shapeName.empty() || has(spec, "center")) shape.pointA = center;
        if (!shapeName.empty()) shape.radius = std::min(size.x, size.y) * 0.5f;
        shape.radius = numberOr(spec, "radius", shape.radius);
        if (shape.radius <= 0.0f) {
            error = "circle needs a positive radius (or fit to a sprite).";
            return false;
        }
    } else if (shape.type == Collider2DType::CAPSULE) {
        if (!shapeName.empty()) {
            // Along the longer side of the bounds
            if (size.y >= size.x) {
                shape.radius = size.x * 0.5f;
                shape.pointA = Vector2(center.x, center.y - size.y * 0.5f + shape.radius);
                shape.pointB = Vector2(center.x, center.y + size.y * 0.5f - shape.radius);
            } else {
                shape.radius = size.y * 0.5f;
                shape.pointA = Vector2(center.x - size.x * 0.5f + shape.radius, center.y);
                shape.pointB = Vector2(center.x + size.x * 0.5f - shape.radius, center.y);
            }
        }
        shape.radius = numberOr(spec, "radius", shape.radius);
        if (!readVec2(spec, "point_a", shape.pointA, error) || !readVec2(spec, "point_b", shape.pointB, error)) return false;
        if (shape.radius <= 0.0f) {
            error = "capsule needs a positive radius.";
            return false;
        }
        if (shape.pointA == shape.pointB) shape.pointB = shape.pointA + Vector2(0.0f, 0.01f);
    } else if (shape.type == Collider2DType::SEGMENT) {
        if (!readVec2(spec, "point_a", shape.pointA, error) || !readVec2(spec, "point_b", shape.pointB, error)) return false;
        if (isNew && (!has(spec, "point_a") || !has(spec, "point_b"))) {
            error = "segment needs point_a and point_b.";
            return false;
        }
    } else {
        // polygon / chain: explicit points, or the bounds rectangle
        if (has(spec, "points")) {
            std::vector<Vector2> points;
            if (!parsePoints2D(spec["points"], points, error)) return false;
            if (points.size() > MAX_COLLIDER_POINTS_2D) {
                error = "A 2D collider holds at most " + std::to_string(MAX_COLLIDER_POINTS_2D) + " points.";
                return false;
            }
            shape.numVertices = static_cast<uint8_t>(points.size());
            for (size_t i = 0; i < points.size(); i++) shape.vertices[i] = points[i];
        } else if (!shapeName.empty()) {
            const Vector2 half = size * 0.5f;
            shape.numVertices = 4;
            shape.vertices[0] = center + Vector2(-half.x, -half.y);
            shape.vertices[1] = center + Vector2(half.x, -half.y);
            shape.vertices[2] = center + Vector2(half.x, half.y);
            shape.vertices[3] = center + Vector2(-half.x, half.y);
        }
        const unsigned minPoints = shape.type == Collider2DType::CHAIN ? 4u : 3u;
        if (shape.numVertices < minPoints) {
            error = std::string(shape.type == Collider2DType::CHAIN ? "chain" : "polygon") + " needs at least " +
                    std::to_string(minPoints) + " points.";
            return false;
        }
        shape.loop = boolOr(spec, "loop", shape.loop);
    }

    shape.density = numberOr(spec, "density", shape.density);
    shape.friction = numberOr(spec, "friction", shape.friction);
    shape.restitution = numberOr(spec, "restitution", shape.restitution);
    shape.sensor = boolOr(spec, "sensor", shape.sensor);
    shape.enableHitEvents = boolOr(spec, "hit_events", shape.enableHitEvents);
    shape.contactEvents = boolOr(spec, "contact_events", shape.contactEvents);
    shape.preSolveEvents = boolOr(spec, "pre_solve_events", shape.preSolveEvents);
    shape.sensorEvents = boolOr(spec, "sensor_events", shape.sensorEvents);
    shape.categoryBits = static_cast<uint16_t>(intOr(spec, "category_bits", shape.categoryBits));
    shape.maskBits = static_cast<uint16_t>(intOr(spec, "mask_bits", shape.maskBits));
    shape.groupIndex = static_cast<int16_t>(intOr(spec, "group_index", shape.groupIndex));
    if (shape.density < 0.0f || shape.friction < 0.0f || shape.restitution < 0.0f || shape.restitution > 1.0f) {
        error = "density and friction must be >= 0, restitution 0-1.";
        return false;
    }
    return true;
}

Json collider2DJson(const Collider2D& shape) {
    Json out = {{"shape", nameOf(collider2DTypes(), shape.type)}, {"density", shape.density}, {"friction", shape.friction},
                {"restitution", shape.restitution}, {"sensor", shape.sensor},
                {"category_bits", shape.categoryBits}, {"mask_bits", shape.maskBits}, {"group_index", shape.groupIndex}};
    if (shape.type == Collider2DType::POLYGON || shape.type == Collider2DType::CHAIN) {
        if (shape.type == Collider2DType::POLYGON) out["shape"] = "polygon";
        Json points = Json::array();
        for (unsigned i = 0; i < shape.numVertices; i++) points.push_back(McpValues::toJson(shape.vertices[i]));
        out["points"] = points;
        if (shape.type == Collider2DType::CHAIN) out["loop"] = shape.loop;
    } else if (shape.type == Collider2DType::CIRCLE) {
        out["center"] = McpValues::toJson(shape.pointA);
        out["radius"] = shape.radius;
    } else {
        out["point_a"] = McpValues::toJson(shape.pointA);
        out["point_b"] = McpValues::toJson(shape.pointB);
        if (shape.type == Collider2DType::CAPSULE) out["radius"] = shape.radius;
    }
    return out;
}

bool applyCollider3D(const Target& target, Entity entity, const Json& spec, Collider3D& shape, bool isNew, std::string& error) {
    Scene* scene = target.engineScene();
    const std::string shapeName = isNew ? stringOr(spec, "shape", "box") : stringOr(spec, "shape", "");
    if (!shapeName.empty() && !pickName(collider3DTypes(), shapeName, shape.type, "shape", error)) return false;

    MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
    if (shape.type == Collider3DType::CONVEX_HULL || shape.type == Collider3DType::MESH) {
        Entity source = entity;
        if (has(spec, "source")) {
            source = resolveRef(target.scene, spec["source"], error);
            if (source == NULL_ENTITY) return false;
        }
        if (!scene->findComponent<MeshComponent>(source)) {
            error = "convex_hull and mesh colliders take their geometry from a mesh: the GameObject (or source) has none.";
            return false;
        }
        shape.source = Collider3DSource::ENTITY_MESH;
        shape.sourceEntity = source;
    } else if (shape.type == Collider3DType::HEIGHTFIELD) {
        if (!scene->findComponent<TerrainComponent>(entity)) {
            error = "heightfield colliders belong on a terrain GameObject.";
            return false;
        }
        shape.source = Collider3DSource::ENTITY_HEIGHTFIELD;
        shape.sourceEntity = entity;
        shape.samplesSize = static_cast<unsigned int>(std::clamp(intOr(spec, "samples_size", shape.samplesSize ? shape.samplesSize : 256), 2, 4096));
    } else if (!shapeName.empty()) {
        shape.source = Collider3DSource::NONE;
        shape.sourceEntity = NULL_ENTITY;
    }

    // Fit primitives to the mesh's local bounds (explicit sizes win per field)
    const bool primitive = shape.type == Collider3DType::BOX || shape.type == Collider3DType::SPHERE ||
                           shape.type == Collider3DType::CAPSULE || shape.type == Collider3DType::TAPERED_CAPSULE ||
                           shape.type == Collider3DType::CYLINDER;
    const bool wantFit = has(spec, "fit") ? boolOr(spec, "fit", false) : (isNew && primitive);
    if (wantFit && primitive) {
        const Vector3 lo = mesh ? mesh->aabb.getMinimum() : Vector3::ZERO;
        const Vector3 hi = mesh ? mesh->aabb.getMaximum() : Vector3::ZERO;
        const bool valid = mesh && std::isfinite(lo.x) && std::isfinite(hi.x) && hi.x >= lo.x && hi.y >= lo.y && hi.z >= lo.z &&
                           (hi - lo).length() > 0.0f;
        if (valid) {
            const Vector3 size(std::max(hi.x - lo.x, 0.01f), std::max(hi.y - lo.y, 0.01f), std::max(hi.z - lo.z, 0.01f));
            const Vector3 half = size * 0.5f;
            shape.position = (lo + hi) * 0.5f;
            shape.width = size.x;
            shape.height = size.y;
            shape.depth = size.z;
            const float radial = std::max(half.x, half.z);
            if (shape.type == Collider3DType::SPHERE) {
                shape.radius = std::max(half.x, std::max(half.y, half.z));
            } else if (shape.type == Collider3DType::CYLINDER) {
                shape.radius = radial;
                shape.halfHeight = half.y;
            } else {
                shape.radius = std::min(radial, half.y);
                shape.halfHeight = std::max(0.01f, half.y - shape.radius);
                shape.topRadius = shape.bottomRadius = shape.radius;
            }
        } else if (has(spec, "fit") && boolOr(spec, "fit", false)) {
            error = "fit needs a GameObject mesh with measurable bounds.";
            return false;
        }
    }

    if (!readVec3(spec, "center", shape.position, error)) return false;
    if (has(spec, "rotation") && !McpValues::parseQuaternion(spec["rotation"], shape.rotation)) {
        error = "rotation must be [x, y, z, w] or {\"euler\": [x, y, z]} degrees.";
        return false;
    }
    if (has(spec, "size")) {
        Vector3 size;
        if (!readVec3(spec, "size", size, error)) return false;
        shape.width = size.x;
        shape.height = size.y;
        shape.depth = size.z;
    }
    shape.radius = numberOr(spec, "radius", shape.radius);
    shape.halfHeight = numberOr(spec, "half_height", shape.halfHeight);
    shape.topRadius = numberOr(spec, "top_radius", shape.topRadius);
    shape.bottomRadius = numberOr(spec, "bottom_radius", shape.bottomRadius);
    shape.density = numberOr(spec, "density", shape.density);
    if (shape.width <= 0.0f || shape.height <= 0.0f || shape.depth <= 0.0f || shape.radius <= 0.0f ||
        shape.halfHeight <= 0.0f || shape.topRadius <= 0.0f || shape.bottomRadius <= 0.0f || shape.density <= 0.0f) {
        error = "Collider sizes, radii and density must be positive.";
        return false;
    }
    return true;
}

Json collider3DJson(const Target& target, const Collider3D& shape) {
    Json out = {{"shape", nameOf(collider3DTypes(), shape.type)}, {"center", McpValues::toJson(shape.position)},
                {"rotation", McpValues::toJson(shape.rotation)}, {"density", shape.density}};
    switch (shape.type) {
        case Collider3DType::BOX: out["size"] = McpValues::toJson(Vector3(shape.width, shape.height, shape.depth)); break;
        case Collider3DType::SPHERE: out["radius"] = shape.radius; break;
        case Collider3DType::CAPSULE:
        case Collider3DType::CYLINDER: out["radius"] = shape.radius; out["half_height"] = shape.halfHeight; break;
        case Collider3DType::TAPERED_CAPSULE:
            out["top_radius"] = shape.topRadius; out["bottom_radius"] = shape.bottomRadius; out["half_height"] = shape.halfHeight; break;
        case Collider3DType::HEIGHTFIELD: out["samples_size"] = shape.samplesSize; break;
        default:
            if (shape.sourceEntity != NULL_ENTITY) out["source"] = entityJson(target, shape.sourceEntity);
            break;
    }
    return out;
}

// Removes, updates and adds collider shapes on an array copy; then writes every slot that changed
template<typename Shape, typename Apply>
bool editShapes(const Json& args, std::vector<Shape>& shapes, size_t maxShapes, Apply apply, std::string& error) {
    if (boolOr(args, "clear", false)) shapes.clear();
    if (has(args, "remove")) {
        if (!args["remove"].is_array()) {
            error = "remove must be an array of collider indices.";
            return false;
        }
        std::vector<int> indices;
        for (const Json& v : args["remove"]) {
            if (!v.is_number_integer() || v.get<int>() < 0 || v.get<int>() >= static_cast<int>(shapes.size())) {
                error = "remove: index " + v.dump() + " is out of range (the body has " + std::to_string(shapes.size()) + " collider(s)).";
                return false;
            }
            indices.push_back(v.get<int>());
        }
        std::sort(indices.rbegin(), indices.rend());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        for (int index : indices) shapes.erase(shapes.begin() + index);
    }
    if (has(args, "update")) {
        if (!args["update"].is_array()) {
            error = "update must be an array of {index, ...fields}.";
            return false;
        }
        for (const Json& spec : args["update"]) {
            const int index = intOr(spec, "index", -1);
            if (index < 0 || index >= static_cast<int>(shapes.size())) {
                error = "update: index " + std::to_string(index) + " is out of range (the body has " + std::to_string(shapes.size()) + " collider(s)).";
                return false;
            }
            if (!apply(spec, shapes[index], false, error)) {
                error = "update[" + std::to_string(index) + "]: " + error;
                return false;
            }
        }
    }
    if (has(args, "add")) {
        if (!args["add"].is_array()) {
            error = "add must be an array of collider specs.";
            return false;
        }
        for (size_t i = 0; i < args["add"].size(); i++) {
            Shape shape;
            if (!apply(args["add"][i], shape, true, error)) {
                error = "add[" + std::to_string(i) + "]: " + error;
                return false;
            }
            shapes.push_back(shape);
        }
    }
    if (shapes.size() > maxShapes) {
        error = "A body holds at most " + std::to_string(maxShapes) + " colliders.";
        return false;
    }
    return true;
}

void editCollidersTool(const CallPtr& call) {
    Target target;
    if (!resolveEntityArg(call, target)) return;
    const Json& args = call->args();
    Scene* scene = target.engineScene();

    // 2D or 3D: an existing body decides, then dimension, then the scene type
    std::string dimension = stringOr(args, "dimension", "");
    if (dimension.empty()) {
        if (scene->findComponent<Rigidbody2DComponent>(target.entity)) dimension = "2d";
        else if (scene->findComponent<RigidbodyComponent>(target.entity)) dimension = "3d";
        else dimension = target.scene->sceneType == SceneType::SCENE_3D ? "3d" : "2d";
    }
    if (dimension != "2d" && dimension != "3d") {
        call->fail("dimension must be 2d or 3d.");
        return;
    }
    const bool is2D = dimension == "2d";
    const bool editing = has(args, "add") || has(args, "update") || has(args, "remove") || boolOr(args, "clear", false) ||
                         has(args, "body_type") || has(args, "body");
    const ComponentType bodyType = is2D ? ComponentType::Rigidbody2DComponent : ComponentType::RigidbodyComponent;
    const bool hasBody = is2D ? scene->findComponent<Rigidbody2DComponent>(target.entity) != nullptr
                              : scene->findComponent<RigidbodyComponent>(target.entity) != nullptr;
    if (editing && !sceneStopped(call, target)) return;
    if (!hasBody && !editing) {
        call->fail("\"" + scene->getEntityName(target.entity) + "\" has no " + (is2D ? "Rigidbody 2D" : "Rigidbody") +
                   ". Pass add (and body_type) to create one with its colliders.");
        return;
    }
    if (is2D && !scene->findComponent<Transform>(target.entity)) {
        call->fail("A Rigidbody 2D needs a GameObject with a Transform.");
        return;
    }

    std::string error;
    auto multi = std::make_unique<MultiPropertyCmd>();
    if (!hasBody) multi->addCommand(std::make_unique<AddComponentCmd>(target.project, target.sceneId(), target.entity, bodyType));

    BodyType type = BodyType::STATIC;
    if (has(args, "body_type")) {
        if (!pickName(bodyTypes(), stringOr(args, "body_type", ""), type, "body_type", error)) {
            call->fail(error);
            return;
        }
        addProp<BodyType>(*multi, target, target.entity, bodyType, "type", type);
    }

    size_t oldCount = 0;
    size_t newCount = 0;
    Json colliders = Json::array();
    Json bodyMaterial = Json::object();  // 3D friction / restitution given on shapes
    if (is2D) {
        std::vector<Collider2D> shapes;
        if (Rigidbody2DComponent* body = scene->findComponent<Rigidbody2DComponent>(target.entity)) {
            oldCount = body->numColliders;
            for (size_t i = 0; i < body->numColliders; i++) shapes.push_back(body->colliders[i]);
        }
        auto apply = [&](const Json& spec, Collider2D& shape, bool isNew, std::string& err) {
            return applyCollider2D(scene, target.entity, spec, shape, isNew, err);
        };
        if (!editShapes(args, shapes, MAX_COLLIDERS_2D, apply, error)) {
            call->fail(error);
            return;
        }
        newCount = shapes.size();
        for (size_t i = 0; i < std::max(oldCount, newCount); i++) {
            addProp<Collider2D>(*multi, target, target.entity, bodyType, "colliders[" + std::to_string(i) + "]",
                                i < newCount ? shapes[i] : Collider2D());
        }
        for (const Collider2D& shape : shapes) colliders.push_back(collider2DJson(shape));
    } else {
        std::vector<Collider3D> shapes;
        if (RigidbodyComponent* body = scene->findComponent<RigidbodyComponent>(target.entity)) {
            oldCount = body->numColliders;
            for (size_t i = 0; i < body->numColliders; i++) shapes.push_back(body->colliders[i]);
        }
        auto apply = [&](const Json& spec, Collider3D& shape, bool isNew, std::string& err) {
            return applyCollider3D(target, target.entity, spec, shape, isNew, err);
        };
        if (!editShapes(args, shapes, MAX_COLLIDERS_3D, apply, error)) {
            call->fail(error);
            return;
        }
        newCount = shapes.size();
        // 3D friction / restitution are per body (Jolt): a shape's value goes to the Rigidbody
        for (const char* list : {"update", "add"}) {
            if (!has(args, list) || !args[list].is_array()) continue;
            for (const Json& spec : args[list]) {
                for (const char* field : {"friction", "restitution"}) {
                    if (!has(spec, field)) continue;
                    if (!spec[field].is_number()) {
                        call->fail(std::string(list) + ": " + field + " must be a number.");
                        return;
                    }
                    const float v = spec[field].get<float>();
                    if (v < 0.0f || (std::string(field) == "restitution" && v > 1.0f)) {
                        call->fail(std::string(list) + ": friction must be >= 0, restitution 0-1.");
                        return;
                    }
                    bodyMaterial[field] = v;
                }
            }
        }
        for (size_t i = 0; i < std::max(oldCount, newCount); i++) {
            addProp<Collider3D>(*multi, target, target.entity, bodyType, "colliders[" + std::to_string(i) + "]",
                                i < newCount ? shapes[i] : Collider3D());
        }
        for (const Collider3D& shape : shapes) colliders.push_back(collider3DJson(target, shape));
    }
    if (editing) {
        if (newCount != oldCount || !hasBody) addProp<size_t>(*multi, target, target.entity, bodyType, "numColliders", newCount);
    }

    Json data = {{"entity", entityJson(target, target.entity)}, {"dimension", dimension}, {"colliders", colliders}};
    auto bodyJson = [scene, &target]() {
        const RigidbodyComponent& body = scene->getComponent<RigidbodyComponent>(target.entity);
        return Json{{"friction", body.friction}, {"restitution", body.restitution}};
    };
    if (!editing) {
        if (is2D) data["body_type"] = nameOf(bodyTypes(), scene->getComponent<Rigidbody2DComponent>(target.entity).type);
        else {
            data["body_type"] = nameOf(bodyTypes(), scene->getComponent<RigidbodyComponent>(target.entity).type);
            data["body"] = bodyJson();
        }
        call->ok("\"" + scene->getEntityName(target.entity) + "\" has " + std::to_string(colliders.size()) + " collider(s).", data);
        return;
    }

    UndoStep step(target.sceneId());
    if (!step.add(multi.release())) {
        call->fail("The editor rejected the collider edit.");
        return;
    }
    if (!bodyMaterial.empty()) {
        MultiPropertyCmd* material = new MultiPropertyCmd();
        for (auto it = bodyMaterial.begin(); it != bodyMaterial.end(); ++it) {
            addProp<float>(*material, target, target.entity, bodyType, it.key(), it.value().get<float>());
        }
        if (!step.add(material)) {
            step.fail(call, "The editor rejected the body friction/restitution.");
            return;
        }
    }
    // Other body properties ({"gravityFactor": 0.5, ...}) after the body exists (they win)
    if (has(args, "body")) {
        MultiPropertyCmd* bodyProps = new MultiPropertyCmd();
        if (!addCatalogProperties(target.project, target.scene, target.entity, bodyType, args["body"], *bodyProps, error) ||
            !step.add(bodyProps)) {
            if (error.empty()) error = "The editor rejected the body properties.";
            step.fail(call, "body: " + error);
            return;
        }
    }
    step.commit();
    // What the body holds now
    colliders = Json::array();
    if (is2D) {
        Rigidbody2DComponent& body = scene->getComponent<Rigidbody2DComponent>(target.entity);
        for (size_t i = 0; i < body.numColliders; i++) colliders.push_back(collider2DJson(body.colliders[i]));
    } else {
        RigidbodyComponent& body = scene->getComponent<RigidbodyComponent>(target.entity);
        for (size_t i = 0; i < body.numColliders; i++) colliders.push_back(collider3DJson(target, body.colliders[i]));
    }
    data["colliders"] = colliders;
    data["body_type"] = is2D ? nameOf(bodyTypes(), scene->getComponent<Rigidbody2DComponent>(target.entity).type)
                             : nameOf(bodyTypes(), scene->getComponent<RigidbodyComponent>(target.entity).type);
    data["body_added"] = !hasBody;
    if (!is2D) data["body"] = bodyJson();
    call->ok("\"" + scene->getEntityName(target.entity) + "\" now has " + std::to_string(newCount) + " collider(s)" +
             (hasBody ? "" : std::string(" on a new ") + (is2D ? "Rigidbody 2D" : "Rigidbody")) + " (one undo step)." +
             (bodyMaterial.empty() ? "" : " Note: 3D friction/restitution are per body: set on the Rigidbody (all its colliders)."), data);
}

const std::vector<std::pair<const char*, Joint2DType>>& joint2DTypes() {
    static const std::vector<std::pair<const char*, Joint2DType>> table = {
        {"distance", Joint2DType::DISTANCE}, {"revolute", Joint2DType::REVOLUTE}, {"prismatic", Joint2DType::PRISMATIC},
        {"mouse", Joint2DType::MOUSE}, {"wheel", Joint2DType::WHEEL}, {"weld", Joint2DType::WELD}, {"motor", Joint2DType::MOTOR}};
    return table;
}

const std::vector<std::pair<const char*, Joint3DType>>& joint3DTypes() {
    static const std::vector<std::pair<const char*, Joint3DType>> table = {
        {"fixed", Joint3DType::FIXED}, {"distance", Joint3DType::DISTANCE}, {"point", Joint3DType::POINT},
        {"hinge", Joint3DType::HINGE}, {"cone", Joint3DType::CONE}, {"prismatic", Joint3DType::PRISMATIC},
        {"swing_twist", Joint3DType::SWINGTWIST}, {"six_dof", Joint3DType::SIXDOF}, {"path", Joint3DType::PATH},
        {"gear", Joint3DType::GEAR}, {"rack_and_pinion", Joint3DType::RACKANDPINON}, {"pulley", Joint3DType::PULLEY}};
    return table;
}

void setJointTool(const CallPtr& call) {
    Target target;
    if (!resolveSceneArg(call, target) || !sceneStopped(call, target)) return;
    const Json& args = call->args();
    Scene* scene = target.engineScene();
    std::string error;

    Entity joint = NULL_ENTITY;
    if (has(args, "joint")) {
        joint = resolveRef(target.scene, args["joint"], error);
        if (joint == NULL_ENTITY) {
            call->fail(error);
            return;
        }
    }
    Entity bodyA = NULL_ENTITY, bodyB = NULL_ENTITY;
    if (has(args, "body_a") && (bodyA = resolveRef(target.scene, args["body_a"], error)) == NULL_ENTITY) {
        call->fail("body_a: " + error);
        return;
    }
    if (has(args, "body_b") && (bodyB = resolveRef(target.scene, args["body_b"], error)) == NULL_ENTITY) {
        call->fail("body_b: " + error);
        return;
    }

    std::string dimension = stringOr(args, "dimension", "");
    if (dimension.empty()) {
        if (joint != NULL_ENTITY) dimension = scene->findComponent<Joint2DComponent>(joint) ? "2d" : "3d";
        else if (bodyA != NULL_ENTITY && scene->findComponent<Rigidbody2DComponent>(bodyA)) dimension = "2d";
        else if (bodyA != NULL_ENTITY && scene->findComponent<RigidbodyComponent>(bodyA)) dimension = "3d";
        else dimension = target.scene->sceneType == SceneType::SCENE_3D ? "3d" : "2d";
    }
    const bool is2D = dimension == "2d";
    const ComponentType jointType = is2D ? ComponentType::Joint2DComponent : ComponentType::Joint3DComponent;
    if (joint != NULL_ENTITY && !(is2D ? (void*)scene->findComponent<Joint2DComponent>(joint) : (void*)scene->findComponent<Joint3DComponent>(joint))) {
        call->fail("\"" + scene->getEntityName(joint) + "\" has no " + (is2D ? "Joint 2D" : "Joint") + " component.");
        return;
    }
    if (joint == NULL_ENTITY && (bodyA == NULL_ENTITY || bodyB == NULL_ENTITY)) {
        call->fail("A new joint needs body_a and body_b (the two Rigidbody GameObjects it connects). Pass joint to edit an existing one.");
        return;
    }
    Json warnings = Json::array();
    for (Entity body : {bodyA, bodyB}) {
        if (body == NULL_ENTITY) continue;
        const bool ok = is2D ? scene->findComponent<Rigidbody2DComponent>(body) != nullptr : scene->findComponent<RigidbodyComponent>(body) != nullptr;
        if (!ok) warnings.push_back("\"" + scene->getEntityName(body) + "\" has no " + std::string(is2D ? "Rigidbody 2D" : "Rigidbody") +
                                    ": the joint is not created at play time until it has one (edit_colliders adds it).");
    }

    UndoStep step(target.sceneId());
    const bool created = joint == NULL_ENTITY;
    if (created) {
        auto* create = new CreateEntityCmd(target.project, target.sceneId(), stringOr(args, "name", is2D ? "Joint 2D" : "Joint"),
                                           is2D ? EntityCreationType::JOINT2D : EntityCreationType::JOINT3D);
        if (!step.add(create)) {
            call->fail("Could not create the joint GameObject.");
            return;
        }
        joint = create->getEntity();
    }

    auto* multi = new MultiPropertyCmd();
    if (has(args, "type")) {
        if (is2D) {
            Joint2DType type;
            if (!pickName(joint2DTypes(), stringOr(args, "type", ""), type, "type", error)) { delete multi; step.fail(call, error); return; }
            addProp<Joint2DType>(*multi, target, joint, jointType, "type", type);
        } else {
            Joint3DType type;
            if (!pickName(joint3DTypes(), stringOr(args, "type", ""), type, "type", error)) { delete multi; step.fail(call, error); return; }
            addProp<Joint3DType>(*multi, target, joint, jointType, "type", type);
        }
    }
    if (bodyA != NULL_ENTITY) addProp<Entity>(*multi, target, joint, jointType, "bodyA", bodyA);
    if (bodyB != NULL_ENTITY) addProp<Entity>(*multi, target, joint, jointType, "bodyB", bodyB);
    struct VecArg { const char* arg; const char* prop; };
    if (is2D) {
        for (const VecArg& v : {VecArg{"anchor_a", "anchorA"}, VecArg{"anchor_b", "anchorB"}, VecArg{"axis", "axis"}, VecArg{"target", "target"}}) {
            Vector2 value;
            if (!has(args, v.arg)) continue;
            if (!readVec2(args, v.arg, value, error)) { delete multi; step.fail(call, error); return; }
            addProp<Vector2>(*multi, target, joint, jointType, v.prop, value);
        }
        if (has(args, "rope")) addProp<bool>(*multi, target, joint, jointType, "rope", boolOr(args, "rope", false));
    } else {
        for (const VecArg& v : {VecArg{"anchor_a", "anchorA"}, VecArg{"anchor_b", "anchorB"}, VecArg{"anchor", "anchor"}, VecArg{"axis", "axis"}, VecArg{"normal", "normal"}}) {
            Vector3 value;
            if (!has(args, v.arg)) continue;
            if (!readVec3(args, v.arg, value, error)) { delete multi; step.fail(call, error); return; }
            addProp<Vector3>(*multi, target, joint, jointType, v.prop, value);
        }
        if (has(args, "limits")) {
            Vector2 limits;
            if (!readVec2(args, "limits", limits, error)) { delete multi; step.fail(call, error); return; }
            addProp<float>(*multi, target, joint, jointType, "limitsMin", limits.x);
            addProp<float>(*multi, target, joint, jointType, "limitsMax", limits.y);
        }
    }
    const bool anchorsGiven = has(args, "anchor_a") || has(args, "anchor_b") || has(args, "anchor");
    if (has(args, "auto_anchors") || anchorsGiven) {
        addProp<bool>(*multi, target, joint, jointType, "autoAnchors", boolOr(args, "auto_anchors", !anchorsGiven));
    }
    if (has(args, "properties") && !addCatalogProperties(target.project, target.scene, joint, jointType, args["properties"], *multi, error)) {
        delete multi;
        step.fail(call, "properties: " + error);
        return;
    }
    if (!step.add(multi)) {
        step.fail(call, "The editor rejected the joint edit.");
        return;
    }
    step.commit();

    Json data = {{"joint", entityJson(target, joint)}, {"dimension", dimension}, {"created", created}};
    if (is2D) {
        Joint2DComponent& j = scene->getComponent<Joint2DComponent>(joint);
        data["type"] = nameOf(joint2DTypes(), j.type);
        data["body_a"] = j.bodyA != NULL_ENTITY ? entityJson(target, j.bodyA) : Json(nullptr);
        data["body_b"] = j.bodyB != NULL_ENTITY ? entityJson(target, j.bodyB) : Json(nullptr);
        data["auto_anchors"] = j.autoAnchors;
    } else {
        Joint3DComponent& j = scene->getComponent<Joint3DComponent>(joint);
        data["type"] = nameOf(joint3DTypes(), j.type);
        data["body_a"] = j.bodyA != NULL_ENTITY ? entityJson(target, j.bodyA) : Json(nullptr);
        data["body_b"] = j.bodyB != NULL_ENTITY ? entityJson(target, j.bodyB) : Json(nullptr);
        data["auto_anchors"] = j.autoAnchors;
    }
    if (!warnings.empty()) data["warnings"] = warnings;
    call->ok(std::string(created ? "Created" : "Updated") + " joint \"" + scene->getEntityName(joint) + "\" (" + data["type"].get<std::string>() + ").", data);
}

// ================================================================== sprite sheets (shared slicing)

struct SliceRect {
    std::string name;
    Rect rect;
};

// The TextureSlicer's grid: {columns, rows} or {tile_size:[w,h]}, plus offset, padding, count,
// names / prefix. Pixel rects from the sheet's top-left, row-major.
bool sliceSheet(const Json& spec, int sheetW, int sheetH, const std::string& defaultPrefix,
                std::vector<SliceRect>& out, size_t maxRects, std::string& error) {
    Vector2 offset(0.0f, 0.0f), padding(0.0f, 0.0f), tile(0.0f, 0.0f);
    if (!readVec2(spec, "offset", offset, error) || !readVec2(spec, "padding", padding, error) ||
        !readVec2(spec, "tile_size", tile, error)) return false;
    const float availW = static_cast<float>(sheetW) - offset.x;
    const float availH = static_cast<float>(sheetH) - offset.y;
    int cols = intOr(spec, "columns", 0), rows = intOr(spec, "rows", 0);
    float cw = tile.x, ch = tile.y;
    if (cw > 0.0f && ch > 0.0f) {
        if (cols <= 0) cols = static_cast<int>(std::floor((availW + padding.x) / (cw + padding.x)));
        if (rows <= 0) rows = static_cast<int>(std::floor((availH + padding.y) / (ch + padding.y)));
    } else if (cols > 0 && rows > 0) {
        cw = std::floor((availW - padding.x * static_cast<float>(cols - 1)) / static_cast<float>(cols));
        ch = std::floor((availH - padding.y * static_cast<float>(rows - 1)) / static_cast<float>(rows));
    } else {
        error = "Slice with tile_size [w, h] (pixels) or columns and rows.";
        return false;
    }
    if (cols <= 0 || rows <= 0 || cw < 1.0f || ch < 1.0f) {
        error = "The slice grid is empty: the sheet is " + std::to_string(sheetW) + "x" + std::to_string(sheetH) + " pixels.";
        return false;
    }
    const int count = std::min(intOr(spec, "count", cols * rows), cols * rows);
    const std::string prefix = stringOr(spec, "prefix", defaultPrefix);
    const Json names = has(spec, "names") && spec["names"].is_array() ? spec["names"] : Json::array();
    for (int i = 0; i < count; i++) {
        const int r = i / cols, c = i % cols;
        const float x = offset.x + static_cast<float>(c) * (cw + padding.x);
        const float y = offset.y + static_cast<float>(r) * (ch + padding.y);
        SliceRect slice;
        slice.name = i < static_cast<int>(names.size()) && names[i].is_string() ? names[i].get<std::string>() : prefix + std::to_string(i);
        slice.rect = Rect(x, y, std::min(cw, static_cast<float>(sheetW) - x), std::min(ch, static_cast<float>(sheetH) - y));
        out.push_back(slice);
    }
    if (out.size() > maxRects) {
        error = "The slice makes " + std::to_string(out.size()) + " rects; at most " + std::to_string(maxRects) + " fit (pass count).";
        return false;
    }
    return true;
}

bool parseRect(const Json& value, Rect& out) {
    Vector4 v;
    if (!McpValues::parseVector4(value, v)) return false;
    out = Rect(v.x, v.y, v.z, v.w);
    return v.z > 0.0f && v.w > 0.0f;
}

Json rectJson(const Rect& rect) {
    return Json::array({rect.getX(), rect.getY(), rect.getWidth(), rect.getHeight()});
}

// The sheet size for slicing: sheet_size, else the image file's size
bool sheetSize(Project* project, const Json& spec, const std::string& storedTexture, int& w, int& h, std::string& error) {
    if (has(spec, "sheet_size")) {
        Vector2 size;
        if (!readVec2(spec, "sheet_size", size, error) || size.x < 1.0f || size.y < 1.0f) {
            error = "sheet_size must be [width, height] pixels.";
            return false;
        }
        w = static_cast<int>(size.x);
        h = static_cast<int>(size.y);
        return true;
    }
    if (imageSize(project, storedTexture, w, h)) return true;
    error = storedTexture.empty() ? "Set texture first (or pass sheet_size [w, h])."
                                  : "Could not read the size of \"" + McpValues::assetPathToJson(project, storedTexture) + "\"; pass sheet_size [w, h].";
    return false;
}

// Optionally creates a GameObject of `type` from {name, parent, position}; else resolves `entity`
bool entityOrCreate(const CallPtr& call, Target& target, UndoStep& step, EntityCreationType type, const char* defaultName, bool& created) {
    const Json& args = call->args();
    created = false;
    std::string error;
    if (has(args, "entity")) {
        target.entity = resolveRef(target.scene, args["entity"], error);
        if (target.entity == NULL_ENTITY) {
            call->fail(error);
            return false;
        }
        return true;
    }
    if (!has(args, "create")) {
        call->fail("Pass entity (an existing GameObject) or create {name, parent, position} to make a new one.");
        return false;
    }
    const Json& create = args["create"];
    Entity parent = NULL_ENTITY;
    if (has(create, "parent") && (parent = resolveRef(target.scene, create["parent"], error)) == NULL_ENTITY) {
        call->fail("create.parent: " + error);
        return false;
    }
    auto* cmd = parent != NULL_ENTITY
        ? new CreateEntityCmd(target.project, target.sceneId(), stringOr(create, "name", defaultName), type, parent)
        : new CreateEntityCmd(target.project, target.sceneId(), stringOr(create, "name", defaultName), type);
    Vector3 position(0.0f, 0.0f, 0.0f);
    if (!readVec3(create, "position", position, error)) {
        delete cmd;
        call->fail("create.position: " + error);
        return false;
    }
    if (has(create, "position")) cmd->addProperty<Vector3>(ComponentType::Transform, "position", position);
    if (!step.add(cmd)) {
        call->fail("Could not create the GameObject.");
        return false;
    }
    target.entity = cmd->getEntity();
    created = true;
    return true;
}

// ================================================================== tilemap

struct TileRef {
    int rect = -1;   // -1 = erase
};

struct CellKey {
    int col, row;
    bool operator<(const CellKey& other) const { return col < other.col || (col == other.col && row < other.row); }
};

bool sameTile(const TileData& a, const TileData& b) {
    return a.name == b.name && a.rectId == b.rectId && a.position == b.position && a.width == b.width && a.height == b.height;
}

bool sameRect(const TileRectData& a, const TileRectData& b) {
    return a.name == b.name && a.submeshId == b.submeshId && a.rect.getX() == b.rect.getX() && a.rect.getY() == b.rect.getY() &&
           a.rect.getWidth() == b.rect.getWidth() && a.rect.getHeight() == b.rect.getHeight();
}

// rect by name or index
bool resolveTileRect(const std::vector<TileRectData>& rects, const Json& value, TileRef& out, std::string& error) {
    if (value.is_null()) {
        out.rect = -1;
        return true;
    }
    if (value.is_number_integer()) {
        const int index = value.get<int>();
        if (index < 0 || index >= static_cast<int>(rects.size())) {
            error = "tile rect index " + std::to_string(index) + " is out of range (the tilemap has " + std::to_string(rects.size()) + " tile rects).";
            return false;
        }
        out.rect = index;
        return true;
    }
    if (value.is_string()) {
        for (size_t i = 0; i < rects.size(); i++) {
            if (rects[i].name == value.get<std::string>()) {
                out.rect = static_cast<int>(i);
                return true;
            }
        }
        std::vector<std::string> names;
        for (size_t i = 0; i < rects.size() && i < 40; i++) names.push_back(rects[i].name);
        error = "No tile rect named \"" + value.get<std::string>() + "\". Tile rects: " + (names.empty() ? std::string("none (define a tileset first)") : joinNames(names)) + ".";
        return false;
    }
    error = "A tile is a tile rect name, its index, or null (erase).";
    return false;
}

const char* AUTO_TILE_CHARS = "#ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789@$%&*+=?!";

// ASCII view of the grid-aligned tiles: lines top (highest row) to bottom
Json tilemapGridJson(const TilemapComponent& tilemap, const Vector2& cell, const Vector2& origin,
                     const std::map<int, char>& preferred, int maxCells) {
    std::map<CellKey, int> cells;
    int offGrid = 0;
    int minC = std::numeric_limits<int>::max(), minR = std::numeric_limits<int>::max();
    int maxC = std::numeric_limits<int>::min(), maxR = std::numeric_limits<int>::min();
    for (unsigned i = 0; i < tilemap.numTiles; i++) {
        const TileData& tile = tilemap.tiles[i];
        if (tile.width <= 0.0f || tile.height <= 0.0f) continue;
        const float fc = (tile.position.x - origin.x) / cell.x, fr = (tile.position.y - origin.y) / cell.y;
        const int c = static_cast<int>(std::lround(fc)), r = static_cast<int>(std::lround(fr));
        if (std::abs(fc - c) > 0.01f || std::abs(fr - r) > 0.01f) {
            offGrid++;
            continue;
        }
        cells[{c, r}] = tile.rectId;
        minC = std::min(minC, c); maxC = std::max(maxC, c);
        minR = std::min(minR, r); maxR = std::max(maxR, r);
    }
    std::map<int, char> chars = preferred;
    std::set<char> used;
    for (const auto& item : chars) used.insert(item.second);
    Json legend = Json::object();
    for (const auto& item : cells) {
        if (chars.count(item.second)) continue;
        if (item.second >= 0 && item.second < static_cast<int>(tilemap.numTilesRect) && !tilemap.tilesRect[item.second].name.empty()) {
            const char first = tilemap.tilesRect[item.second].name[0];
            if (std::isalnum(static_cast<unsigned char>(first)) && !used.count(first)) {
                chars[item.second] = first;
                used.insert(first);
                continue;
            }
        }
        for (const char* p = AUTO_TILE_CHARS; *p; p++) {
            if (!used.count(*p)) {
                chars[item.second] = *p;
                used.insert(*p);
                break;
            }
        }
        if (!chars.count(item.second)) chars[item.second] = '?';
    }
    Json out = {{"cell_size", McpValues::toJson(cell)}, {"origin", McpValues::toJson(origin)}, {"tiles_on_grid", cells.size()},
                {"tiles_off_grid", offGrid}};
    if (cells.empty()) {
        out["lines"] = Json::array();
        return out;
    }
    const int cols = maxC - minC + 1, rows = maxR - minR + 1;
    out["columns"] = {minC, maxC};
    out["rows"] = {minR, maxR};
    for (const auto& item : chars) {
        if (item.first >= 0 && item.first < static_cast<int>(tilemap.numTilesRect)) {
            legend[std::string(1, item.second)] = tilemap.tilesRect[item.first].name;
        } else {
            legend[std::string(1, item.second)] = item.first;
        }
    }
    out["legend"] = legend;
    if (static_cast<long long>(cols) * rows > maxCells) {
        out["lines_omitted"] = "The grid is " + std::to_string(cols) + "x" + std::to_string(rows) + " cells; raise max_cells to see it.";
        return out;
    }
    Json lines = Json::array();
    for (int r = maxR; r >= minR; r--) {
        std::string line(static_cast<size_t>(cols), '.');
        for (int c = minC; c <= maxC; c++) {
            auto it = cells.find({c, r});
            if (it != cells.end()) line[static_cast<size_t>(c - minC)] = chars[it->second];
        }
        lines.push_back(line);
    }
    out["lines"] = lines;
    out["lines_note"] = "Top line is row " + std::to_string(maxR) + ", bottom line row " + std::to_string(minR) +
                        "; first character is column " + std::to_string(minC) + ". '.' is empty.";
    return out;
}

// The cell size a tilemap's grid most likely uses: the commonest tile size, else the first rect's
Vector2 guessCellSize(const TilemapComponent& tilemap) {
    std::map<std::pair<float, float>, int> counts;
    for (unsigned i = 0; i < tilemap.numTiles; i++) {
        if (tilemap.tiles[i].width > 0.0f && tilemap.tiles[i].height > 0.0f) counts[{tilemap.tiles[i].width, tilemap.tiles[i].height}]++;
    }
    std::pair<float, float> best = {0.0f, 0.0f};
    int bestCount = 0;
    for (const auto& item : counts) {
        if (item.second > bestCount) {
            best = item.first;
            bestCount = item.second;
        }
    }
    if (bestCount > 0) return Vector2(best.first, best.second);
    for (unsigned i = 0; i < tilemap.numTilesRect; i++) {
        if (tilemap.tilesRect[i].rect.getWidth() > 1.0f) return Vector2(tilemap.tilesRect[i].rect.getWidth(), tilemap.tilesRect[i].rect.getHeight());
    }
    return Vector2(32.0f, 32.0f);
}

Json tileRectsJson(const TilemapComponent& tilemap) {
    Json rects = Json::array();
    for (unsigned i = 0; i < tilemap.numTilesRect; i++) {
        rects.push_back({{"index", i}, {"name", tilemap.tilesRect[i].name}, {"rect", rectJson(tilemap.tilesRect[i].rect)},
                         {"submesh", tilemap.tilesRect[i].submeshId}});
    }
    return rects;
}

void editTilemapTool(const CallPtr& call) {
    Target target;
    if (!resolveSceneArg(call, target) || !sceneStopped(call, target)) return;
    const Json& args = call->args();
    Scene* scene = target.engineScene();
    std::string error;

    UndoStep step(target.sceneId());
    bool created = false;
    if (!entityOrCreate(call, target, step, EntityCreationType::TILEMAP, "Tilemap", created)) {
        step.rollback();
        return;
    }
    auto failWith = [&](const std::string& message) {
        step.fail(call, message);
    };
    if (!scene->findComponent<TilemapComponent>(target.entity) || !scene->findComponent<MeshComponent>(target.entity)) {
        failWith("\"" + scene->getEntityName(target.entity) + "\" is not a Tilemap (create one with create: {name}).");
        return;
    }

    // 1. texture and tile rects
    {
        TilemapComponent& tilemap = scene->getComponent<TilemapComponent>(target.entity);
        auto* setup = new MultiPropertyCmd();
        std::string stored = meshTexturePath(scene, target.entity, 0);
        if (has(args, "texture")) {
            if (!addMeshTexture(target, target.entity, 0, args["texture"], *setup, error) ||
                !McpValues::assetPathFromJson(target.project, stringOr(args, "texture", ""), stored, error)) {
                delete setup;
                failWith("texture: " + error);
                return;
            }
        }
        std::vector<TileRectData> rects;
        for (unsigned i = 0; i < tilemap.numTilesRect; i++) rects.push_back(tilemap.tilesRect[i]);
        const std::vector<TileRectData> before = rects;
        if (has(args, "tileset")) {
            int sheetW = 0, sheetH = 0;
            if (!sheetSize(target.project, args["tileset"], stored, sheetW, sheetH, error)) {
                delete setup;
                failWith("tileset: " + error);
                return;
            }
            std::vector<SliceRect> slices;
            if (!sliceSheet(args["tileset"], sheetW, sheetH, "tile_", slices, MAX_TILEMAP_TILESRECT, error)) {
                delete setup;
                failWith("tileset: " + error);
                return;
            }
            // Like the Texture Slicer: the submesh's rects are replaced, the other submeshes' kept
            std::vector<TileRectData> kept;
            for (const TileRectData& rect : rects) {
                if (rect.submeshId != 0) kept.push_back(rect);
            }
            rects = kept;
            for (const SliceRect& slice : slices) {
                TileRectData rect;
                rect.name = slice.name;
                rect.submeshId = 0;
                rect.rect = slice.rect;
                rects.push_back(rect);
            }
        }
        if (has(args, "rects")) {
            if (!args["rects"].is_array()) {
                delete setup;
                failWith("rects must be an array of {name, rect: [x, y, w, h]}.");
                return;
            }
            if (boolOr(args, "replace_rects", false)) rects.clear();
            for (const Json& item : args["rects"]) {
                TileRectData rect;
                rect.name = stringOr(item, "name", "rect_" + std::to_string(rects.size()));
                rect.submeshId = intOr(item, "submesh", 0);
                if (!has(item, "rect") || !parseRect(item["rect"], rect.rect)) {
                    delete setup;
                    failWith("rects: each item needs rect [x, y, width, height] in pixels from the image's top-left.");
                    return;
                }
                bool replaced = false;
                for (TileRectData& existing : rects) {
                    if (existing.name == rect.name) {
                        existing = rect;
                        replaced = true;
                    }
                }
                if (!replaced) rects.push_back(rect);
            }
        }
        if (rects.size() > MAX_TILEMAP_TILESRECT) {
            delete setup;
            failWith("A tilemap holds at most " + std::to_string(MAX_TILEMAP_TILESRECT) + " tile rects.");
            return;
        }
        for (size_t i = 0; i < std::max(before.size(), rects.size()); i++) {
            TileRectData value;
            if (i < rects.size()) value = rects[i];
            else value.rect = Rect(0, 0, 0, 0);
            if (i < before.size() && sameRect(before[i], value)) continue;
            addProp<TileRectData>(*setup, target, target.entity, ComponentType::TilemapComponent, "tilesRect[" + std::to_string(i) + "]", value);
        }
        if (rects.size() != before.size()) {
            addProp<unsigned int>(*setup, target, target.entity, ComponentType::TilemapComponent, "numTilesRect", static_cast<unsigned int>(rects.size()));
        }
        const bool setupUsed = has(args, "texture") || has(args, "tileset") || has(args, "rects");
        if (!setupUsed) {
            delete setup;
        } else if (!step.add(setup)) {
            failWith("The editor rejected the tileset edit.");
            return;
        }
    }

    // 2. tiles
    TilemapComponent& tilemap = scene->getComponent<TilemapComponent>(target.entity);
    std::vector<TileRectData> rects;
    for (unsigned i = 0; i < tilemap.numTilesRect; i++) rects.push_back(tilemap.tilesRect[i]);
    Vector2 cell = guessCellSize(tilemap);
    Vector2 origin(0.0f, 0.0f);
    if (has(args, "cell_size") && (!readVec2(args, "cell_size", cell, error) || cell.x <= 0.0f || cell.y <= 0.0f)) {
        failWith("cell_size must be [width, height] > 0 in the tilemap's local units (pixels in 2D).");
        return;
    }
    if (!readVec2(args, "origin", origin, error) || origin.x < 0.0f || origin.y < 0.0f) {
        failWith("origin must be [x, y] >= 0 (tile positions below 0 are clamped by the engine).");
        return;
    }

    std::vector<TileData> tiles;
    for (unsigned i = 0; i < tilemap.numTiles; i++) tiles.push_back(tilemap.tiles[i]);
    const std::vector<TileData> beforeTiles = tiles;
    std::vector<bool> removed(tiles.size(), false);
    std::map<CellKey, size_t> index;
    auto cellOf = [&](const TileData& tile, CellKey& key) {
        const float fc = (tile.position.x - origin.x) / cell.x, fr = (tile.position.y - origin.y) / cell.y;
        key = {static_cast<int>(std::lround(fc)), static_cast<int>(std::lround(fr))};
        return std::abs(fc - key.col) < 0.01f && std::abs(fr - key.row) < 0.01f && tile.width > 0.0f;
    };
    for (size_t i = 0; i < tiles.size(); i++) {
        CellKey key;
        if (cellOf(tiles[i], key)) index[key] = i;
    }
    int painted = 0, erased = 0;
    auto setCell = [&](int col, int row, const TileRef& ref) -> bool {
        const float x = origin.x + static_cast<float>(col) * cell.x, y = origin.y + static_cast<float>(row) * cell.y;
        if (x < -0.001f || y < -0.001f) {
            error = "cell (" + std::to_string(col) + ", " + std::to_string(row) + ") is below the tilemap origin; tile positions must be >= 0 (use origin to shift the grid).";
            return false;
        }
        auto it = index.find({col, row});
        if (ref.rect < 0) {
            if (it != index.end()) {
                removed[it->second] = true;
                index.erase(it);
                erased++;
            }
            return true;
        }
        TileData tile{};
        tile.name = rects[ref.rect].name;
        tile.rectId = ref.rect;
        tile.position = Vector2(x, y);
        tile.width = cell.x;
        tile.height = cell.y;
        if (it != index.end()) {
            tiles[it->second] = tile;
        } else {
            index[{col, row}] = tiles.size();
            tiles.push_back(tile);
            removed.push_back(false);
        }
        painted++;
        return true;
    };

    std::map<int, char> preferredChars;
    if (has(args, "ops")) {
        if (!args["ops"].is_array()) {
            failWith("ops must be an array of {op: paint | fill | erase | set | clear, ...}.");
            return;
        }
        for (size_t o = 0; o < args["ops"].size(); o++) {
            const Json& op = args["ops"][o];
            const std::string where = "ops[" + std::to_string(o) + "]";
            const std::string kind = lowerText(stringOr(op, "op", ""));
            Vector2 at(0.0f, 0.0f);
            if (!readVec2(op, "at", at, error)) { failWith(where + ": " + error); return; }
            if (kind == "paint") {
                std::vector<std::string> lines;
                if (has(op, "grid") && op["grid"].is_string()) {
                    std::stringstream stream(op["grid"].get<std::string>());
                    std::string line;
                    while (std::getline(stream, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        lines.push_back(line);
                    }
                    // a leading / trailing empty line from a """ block is not a row
                    while (!lines.empty() && lines.front().find_first_not_of(' ') == std::string::npos) lines.erase(lines.begin());
                    while (!lines.empty() && lines.back().find_first_not_of(' ') == std::string::npos) lines.pop_back();
                } else if (has(op, "grid") && op["grid"].is_array()) {
                    for (const Json& line : op["grid"]) lines.push_back(line.is_string() ? line.get<std::string>() : std::string());
                } else {
                    failWith(where + ": paint needs grid (a multi-line string or an array of lines).");
                    return;
                }
                std::map<char, TileRef> legend;
                if (has(op, "legend")) {
                    if (!op["legend"].is_object()) { failWith(where + ".legend must be {\"char\": tile rect name | index | null}."); return; }
                    for (auto it = op["legend"].begin(); it != op["legend"].end(); ++it) {
                        if (it.key().size() != 1) { failWith(where + ".legend keys are single characters."); return; }
                        TileRef ref;
                        if (!resolveTileRect(rects, it.value(), ref, error)) { failWith(where + ".legend['" + it.key() + "']: " + error); return; }
                        legend[it.key()[0]] = ref;
                        if (ref.rect >= 0) preferredChars[ref.rect] = it.key()[0];
                    }
                }
                const bool eraseBlank = lowerText(stringOr(op, "blank", "skip")) == "erase";
                const int rowsCount = static_cast<int>(lines.size());
                for (int i = 0; i < rowsCount; i++) {
                    const int row = static_cast<int>(at.y) + (rowsCount - 1 - i);
                    for (size_t j = 0; j < lines[i].size(); j++) {
                        const char ch = lines[i][j];
                        const int col = static_cast<int>(at.x) + static_cast<int>(j);
                        auto it = legend.find(ch);
                        if (it == legend.end()) {
                            if (ch == '.' || ch == ' ') {
                                if (eraseBlank && !setCell(col, row, TileRef{})) { failWith(where + ": " + error); return; }
                                continue;
                            }
                            failWith(where + ": character '" + std::string(1, ch) + "' (line " + std::to_string(i + 1) +
                                     ") is not in the legend. '.' and ' ' are empty cells.");
                            return;
                        }
                        if (!setCell(col, row, it->second)) { failWith(where + ": " + error); return; }
                    }
                }
            } else if (kind == "fill" || kind == "erase") {
                Vector2 from(0.0f, 0.0f), to(0.0f, 0.0f);
                if (!has(op, "from") || !readVec2(op, "from", from, error) || !readVec2(op, "to", to, error)) {
                    failWith(where + ": " + kind + " needs from [col, row] and to [col, row] (inclusive).");
                    return;
                }
                if (!has(op, "to")) to = from;
                TileRef ref;
                if (kind == "fill" && !resolveTileRect(rects, has(op, "tile") ? op["tile"] : Json(), ref, error)) { failWith(where + ".tile: " + error); return; }
                if (kind == "fill" && ref.rect < 0) { failWith(where + ": fill needs tile (a tile rect name or index)."); return; }
                const int c0 = static_cast<int>(std::min(from.x, to.x)), c1 = static_cast<int>(std::max(from.x, to.x));
                const int r0 = static_cast<int>(std::min(from.y, to.y)), r1 = static_cast<int>(std::max(from.y, to.y));
                if (static_cast<long long>(c1 - c0 + 1) * (r1 - r0 + 1) > MAX_TILEMAP_TILES * 4) { failWith(where + ": the area is too large."); return; }
                for (int r = r0; r <= r1; r++) {
                    for (int c = c0; c <= c1; c++) {
                        if (!setCell(c, r, ref)) { failWith(where + ": " + error); return; }
                    }
                }
            } else if (kind == "set") {
                if (!has(op, "cells") || !op["cells"].is_array()) { failWith(where + ": set needs cells [[col, row, tile], ...]."); return; }
                for (const Json& item : op["cells"]) {
                    if (!item.is_array() || item.size() != 3 || !item[0].is_number() || !item[1].is_number()) {
                        failWith(where + ": each cell is [col, row, tile] (tile: rect name, index or null).");
                        return;
                    }
                    TileRef ref;
                    if (!resolveTileRect(rects, item[2], ref, error) || !setCell(item[0].get<int>(), item[1].get<int>(), ref)) {
                        failWith(where + ": " + error);
                        return;
                    }
                }
            } else if (kind == "clear") {
                for (size_t i = 0; i < removed.size(); i++) {
                    if (!removed[i]) erased++;
                    removed[i] = true;
                }
                index.clear();
            } else {
                failWith(where + ".op must be paint, fill, erase, set or clear.");
                return;
            }
        }
    }

    std::vector<TileData> finalTiles;
    for (size_t i = 0; i < tiles.size(); i++) {
        if (!removed[i]) finalTiles.push_back(tiles[i]);
    }
    if (finalTiles.size() > MAX_TILEMAP_TILES) {
        failWith("The tilemap would hold " + std::to_string(finalTiles.size()) + " tiles; the maximum is " + std::to_string(MAX_TILEMAP_TILES) + ".");
        return;
    }
    auto* tileCmd = new MultiPropertyCmd();
    size_t writes = 0;
    for (size_t i = 0; i < std::max(beforeTiles.size(), finalTiles.size()); i++) {
        TileData value{};
        if (i < finalTiles.size()) value = finalTiles[i];
        if (i < beforeTiles.size() && sameTile(beforeTiles[i], value)) continue;
        addProp<TileData>(*tileCmd, target, target.entity, ComponentType::TilemapComponent, "tiles[" + std::to_string(i) + "]", value);
        writes++;
    }
    if (finalTiles.size() != beforeTiles.size()) {
        addProp<unsigned int>(*tileCmd, target, target.entity, ComponentType::TilemapComponent, "numTiles", static_cast<unsigned int>(finalTiles.size()));
        writes++;
    }
    if (writes > 0) {
        if (!step.add(tileCmd)) {
            failWith("The editor rejected the tile edit.");
            return;
        }
    } else {
        delete tileCmd;
    }
    step.commit();

    TilemapComponent& result = scene->getComponent<TilemapComponent>(target.entity);
    Json data = {{"tilemap", entityJson(target, target.entity)}, {"created", created}, {"tile_rects", result.numTilesRect},
                 {"tiles", result.numTiles}, {"painted", painted}, {"erased", erased}};
    if (has(args, "tileset") || has(args, "rects")) data["rects"] = tileRectsJson(result);
    if (boolOr(args, "return_grid", true)) data["grid"] = tilemapGridJson(result, cell, origin, preferredChars, intOr(args, "max_cells", 6000));
    call->ok("Tilemap \"" + scene->getEntityName(target.entity) + "\": " + std::to_string(result.numTilesRect) + " tile rects, " +
             std::to_string(result.numTiles) + " tiles (" + std::to_string(painted) + " painted, " + std::to_string(erased) +
             " erased, one undo step).", data);
}

void getTilemapTool(const CallPtr& call) {
    Target target;
    if (!resolveEntityArg(call, target)) return;
    const Json& args = call->args();
    Scene* scene = target.engineScene();
    TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(target.entity);
    if (!tilemap) {
        call->fail("\"" + scene->getEntityName(target.entity) + "\" has no Tilemap component.");
        return;
    }
    std::string error;
    Vector2 cell = guessCellSize(*tilemap), origin(0.0f, 0.0f);
    if (!readVec2(args, "cell_size", cell, error) || !readVec2(args, "origin", origin, error) || cell.x <= 0.0f || cell.y <= 0.0f) {
        call->fail(error.empty() ? "cell_size must be positive." : error);
        return;
    }
    Json data = {{"tilemap", entityJson(target, target.entity)},
                 {"texture", McpValues::assetPathToJson(target.project, meshTexturePath(scene, target.entity, 0))},
                 {"rects", tileRectsJson(*tilemap)}, {"tiles", tilemap->numTiles},
                 {"grid", tilemapGridJson(*tilemap, cell, origin, {}, intOr(args, "max_cells", 6000))}};
    if (boolOr(args, "include_tiles", false)) {
        Json list = Json::array();
        const unsigned limit = static_cast<unsigned>(std::max(1, intOr(args, "limit", 200)));
        for (unsigned i = 0; i < tilemap->numTiles && i < limit; i++) {
            const TileData& t = tilemap->tiles[i];
            list.push_back({{"index", i}, {"rect", t.rectId}, {"name", t.name}, {"position", McpValues::toJson(t.position)},
                            {"size", McpValues::toJson(Vector2(t.width, t.height))}});
        }
        data["tile_list"] = list;
    }
    call->ok("Tilemap \"" + scene->getEntityName(target.entity) + "\": " + std::to_string(tilemap->numTilesRect) + " tile rects, " +
             std::to_string(tilemap->numTiles) + " tiles.", data);
}

// ================================================================== sprites

const std::vector<std::pair<const char*, PivotPreset>>& pivotPresets() {
    static const std::vector<std::pair<const char*, PivotPreset>> table = {
        {"center", PivotPreset::CENTER}, {"top_center", PivotPreset::TOP_CENTER}, {"bottom_center", PivotPreset::BOTTOM_CENTER},
        {"left_center", PivotPreset::LEFT_CENTER}, {"right_center", PivotPreset::RIGHT_CENTER}, {"top_left", PivotPreset::TOP_LEFT},
        {"bottom_left", PivotPreset::BOTTOM_LEFT}, {"top_right", PivotPreset::TOP_RIGHT}, {"bottom_right", PivotPreset::BOTTOM_RIGHT}};
    return table;
}

void setSpriteFramesTool(const CallPtr& call) {
    Target target;
    if (!resolveSceneArg(call, target) || !sceneStopped(call, target)) return;
    const Json& args = call->args();
    Scene* scene = target.engineScene();
    std::string error;
    UndoStep step(target.sceneId());
    bool created = false;
    if (!entityOrCreate(call, target, step, EntityCreationType::SPRITE, "Sprite", created)) {
        step.rollback();
        return;
    }
    auto failWith = [&](const std::string& message) {
        step.fail(call, message);
    };
    SpriteComponent* sprite = scene->findComponent<SpriteComponent>(target.entity);
    if (!sprite || !scene->findComponent<MeshComponent>(target.entity)) {
        failWith("\"" + scene->getEntityName(target.entity) + "\" is not a Sprite.");
        return;
    }

    auto* multi = new MultiPropertyCmd();
    std::string stored = meshTexturePath(scene, target.entity, 0);
    if (has(args, "texture")) {
        if (!addMeshTexture(target, target.entity, 0, args["texture"], *multi, error) ||
            !McpValues::assetPathFromJson(target.project, stringOr(args, "texture", ""), stored, error)) {
            delete multi;
            failWith("texture: " + error);
            return;
        }
    }
    std::vector<SliceRect> frames;
    bool framesGiven = false;
    if (has(args, "slice")) {
        int sheetW = 0, sheetH = 0;
        if (!sheetSize(target.project, args["slice"], stored, sheetW, sheetH, error) ||
            !sliceSheet(args["slice"], sheetW, sheetH, "frame_", frames, MAX_SPRITE_FRAMES, error)) {
            delete multi;
            failWith("slice: " + error);
            return;
        }
        framesGiven = true;
    } else if (has(args, "frames")) {
        if (!args["frames"].is_array()) {
            delete multi;
            failWith("frames must be an array of {name, rect: [x, y, w, h]}.");
            return;
        }
        for (const Json& item : args["frames"]) {
            SliceRect slice;
            slice.name = stringOr(item, "name", "frame_" + std::to_string(frames.size()));
            if (!has(item, "rect") || !parseRect(item["rect"], slice.rect)) {
                delete multi;
                failWith("frames: each item needs rect [x, y, width, height] in pixels from the image's top-left.");
                return;
            }
            frames.push_back(slice);
        }
        if (frames.size() > MAX_SPRITE_FRAMES) {
            delete multi;
            failWith("A sprite holds at most " + std::to_string(MAX_SPRITE_FRAMES) + " frames.");
            return;
        }
        framesGiven = true;
    }
    const ComponentType cp = ComponentType::SpriteComponent;
    if (framesGiven) {
        for (unsigned i = static_cast<unsigned>(frames.size()); i < sprite->numFramesRect; i++) {
            addProp<std::string>(*multi, target, target.entity, cp, "framesRect[" + std::to_string(i) + "].name", std::string());
            addProp<Vector4>(*multi, target, target.entity, cp, "framesRect[" + std::to_string(i) + "].rect", Vector4(0, 0, 0, 0));
        }
        for (size_t i = 0; i < frames.size(); i++) {
            const Rect& r = frames[i].rect;
            addProp<std::string>(*multi, target, target.entity, cp, "framesRect[" + std::to_string(i) + "].name", frames[i].name);
            addProp<Vector4>(*multi, target, target.entity, cp, "framesRect[" + std::to_string(i) + "].rect",
                             Vector4(r.getX(), r.getY(), r.getWidth(), r.getHeight()));
        }
        addProp<unsigned int>(*multi, target, target.entity, cp, "numFramesRect", static_cast<unsigned int>(frames.size()));
    } else {
        for (unsigned i = 0; i < sprite->numFramesRect; i++) frames.push_back({sprite->framesRect[i].name, sprite->framesRect[i].rect});
    }

    // The frame shown in the editor (and the sprite's size)
    const int shown = intOr(args, "frame", 0);
    if (!frames.empty() && (shown < 0 || shown >= static_cast<int>(frames.size()))) {
        delete multi;
        failWith("frame " + std::to_string(shown) + " is out of range (" + std::to_string(frames.size()) + " frames).");
        return;
    }
    int sheetW = 0, sheetH = 0;
    if (!frames.empty() && (framesGiven || has(args, "frame"))) {
        if (imageSize(target.project, stored, sheetW, sheetH) ||
            (has(args, "slice") && sheetSize(target.project, args["slice"], stored, sheetW, sheetH, error))) {
            const Rect& r = frames[shown].rect;
            addProp<Vector4>(*multi, target, target.entity, ComponentType::MeshComponent, "submeshes[0].textureRect",
                             Vector4(r.getX() / sheetW, r.getY() / sheetH, r.getWidth() / sheetW, r.getHeight() / sheetH));
        }
        if (boolOr(args, "resize", true)) {
            addProp<unsigned int>(*multi, target, target.entity, cp, "width", static_cast<unsigned int>(frames[shown].rect.getWidth()));
            addProp<unsigned int>(*multi, target, target.entity, cp, "height", static_cast<unsigned int>(frames[shown].rect.getHeight()));
        }
    }
    if (has(args, "size")) {
        Vector2 size;
        if (!readVec2(args, "size", size, error) || size.x < 1.0f || size.y < 1.0f) {
            delete multi;
            failWith("size must be [width, height] pixels.");
            return;
        }
        addProp<unsigned int>(*multi, target, target.entity, cp, "width", static_cast<unsigned int>(size.x));
        addProp<unsigned int>(*multi, target, target.entity, cp, "height", static_cast<unsigned int>(size.y));
    }
    if (has(args, "pivot")) {
        PivotPreset pivot;
        if (!pickName(pivotPresets(), stringOr(args, "pivot", ""), pivot, "pivot", error)) {
            delete multi;
            failWith(error);
            return;
        }
        addProp<PivotPreset>(*multi, target, target.entity, cp, "pivotPreset", pivot);
    }
    if (!step.add(multi)) {
        failWith("The editor rejected the sprite edit.");
        return;
    }
    step.commit();

    SpriteComponent& result = scene->getComponent<SpriteComponent>(target.entity);
    Json list = Json::array();
    for (unsigned i = 0; i < result.numFramesRect; i++) {
        list.push_back({{"index", i}, {"name", result.framesRect[i].name}, {"rect", rectJson(result.framesRect[i].rect)}});
    }
    call->ok("Sprite \"" + scene->getEntityName(target.entity) + "\" has " + std::to_string(result.numFramesRect) + " frame(s).",
             Json{{"sprite", entityJson(target, target.entity)}, {"created", created}, {"frames", list},
                  {"texture", McpValues::assetPathToJson(target.project, stored)},
                  {"size", {result.width, result.height}}, {"pivot", nameOf(pivotPresets(), result.pivotPreset)}});
}

void setSpriteAnimationTool(const CallPtr& call) {
    Target target;
    if (!resolveEntityArg(call, target, "sprite") || !sceneStopped(call, target)) return;
    const Json& args = call->args();
    Scene* scene = target.engineScene();
    std::string error;
    SpriteComponent* sprite = scene->findComponent<SpriteComponent>(target.entity);
    if (!sprite) {
        call->fail("\"" + scene->getEntityName(target.entity) + "\" is not a Sprite (set_sprite_frames makes one).");
        return;
    }
    if (sprite->numFramesRect == 0) {
        call->fail("The sprite has no frames yet: slice its sheet with set_sprite_frames first.");
        return;
    }
    // frames: indices or frame names
    std::vector<int> frames;
    if (has(args, "frames")) {
        if (!args["frames"].is_array() || args["frames"].empty()) {
            call->fail("frames must be a non-empty array of frame indices or names.");
            return;
        }
        for (const Json& f : args["frames"]) {
            int index = -1;
            if (f.is_number_integer()) index = f.get<int>();
            else if (f.is_string()) {
                for (unsigned i = 0; i < sprite->numFramesRect; i++) {
                    if (sprite->framesRect[i].name == f.get<std::string>()) index = static_cast<int>(i);
                }
            }
            if (index < 0 || index >= static_cast<int>(sprite->numFramesRect)) {
                call->fail("frame " + f.dump() + " is not one of the sprite's " + std::to_string(sprite->numFramesRect) + " frames (index or name).");
                return;
            }
            frames.push_back(index);
        }
    } else {
        for (unsigned i = 0; i < sprite->numFramesRect; i++) frames.push_back(static_cast<int>(i));
    }
    if (frames.size() > MAX_SPRITE_FRAMES) {
        call->fail("At most " + std::to_string(MAX_SPRITE_FRAMES) + " frames.");
        return;
    }
    std::vector<int> times(frames.size(), static_cast<int>(std::lround(1000.0f / std::max(0.1f, numberOr(args, "fps", 10.0f)))));
    if (has(args, "frame_ms")) {
        if (args["frame_ms"].is_number()) {
            std::fill(times.begin(), times.end(), std::max(1, args["frame_ms"].get<int>()));
        } else if (args["frame_ms"].is_array() && args["frame_ms"].size() == frames.size()) {
            for (size_t i = 0; i < frames.size(); i++) times[i] = std::max(1, args["frame_ms"][i].is_number() ? args["frame_ms"][i].get<int>() : 100);
        } else {
            call->fail("frame_ms is one number (every frame) or one number per frame.");
            return;
        }
    }

    UndoStep step(target.sceneId());
    Entity animation = NULL_ENTITY;
    bool created = false;
    if (has(args, "animation")) {
        animation = resolveRef(target.scene, args["animation"], error);
        if (animation == NULL_ENTITY || !scene->findComponent<SpriteAnimationComponent>(animation)) {
            call->fail(animation == NULL_ENTITY ? error : "\"" + scene->getEntityName(animation) + "\" is not a Sprite Animation.");
            return;
        }
    } else {
        auto* create = new CreateEntityCmd(target.project, target.sceneId(), stringOr(args, "name", "Sprite Animation"),
                                           EntityCreationType::SPRITE_ANIMATION, target.entity);
        if (!step.add(create)) {
            call->fail("Could not create the Sprite Animation GameObject.");
            return;
        }
        animation = create->getEntity();
        created = true;
    }
    SpriteAnimationComponent& anim = scene->getComponent<SpriteAnimationComponent>(animation);
    const ComponentType cp = ComponentType::SpriteAnimationComponent;
    auto* multi = new MultiPropertyCmd();
    for (size_t i = 0; i < std::max<size_t>(frames.size(), anim.framesSize); i++) {
        addProp<int>(*multi, target, animation, cp, "frames[" + std::to_string(i) + "]", i < frames.size() ? frames[i] : 0);
        addProp<int>(*multi, target, animation, cp, "framesTime[" + std::to_string(i) + "]", i < frames.size() ? times[i] : 0);
    }
    addProp<unsigned int>(*multi, target, animation, cp, "framesSize", static_cast<unsigned int>(frames.size()));
    addProp<unsigned int>(*multi, target, animation, cp, "framesTimeSize", static_cast<unsigned int>(frames.size()));
    addProp<bool>(*multi, target, animation, cp, "loop", boolOr(args, "loop", true));
    if (scene->getComponent<ActionComponent>(animation).target != target.entity) {
        addProp<Entity>(*multi, target, animation, ComponentType::ActionComponent, "target", target.entity);
    }
    if (has(args, "autoplay") || created) {
        addProp<ActionState>(*multi, target, animation, ComponentType::ActionComponent, "state",
                             boolOr(args, "autoplay", true) ? ActionState::Running : ActionState::Stopped);
    }
    if (!step.add(multi)) {
        step.fail(call, "The editor rejected the sprite animation edit.");
        return;
    }
    step.commit();
    int total = 0;
    for (int t : times) total += t;
    call->ok(std::string(created ? "Created" : "Updated") + " sprite animation \"" + scene->getEntityName(animation) + "\": " +
             std::to_string(frames.size()) + " frames, " + std::to_string(total) + " ms per cycle.",
             Json{{"animation", entityJson(target, animation)}, {"sprite", entityJson(target, target.entity)}, {"created", created},
                  {"frames", frames}, {"frame_ms", times}, {"loop", boolOr(args, "loop", true)},
                  {"autoplay", scene->getComponent<ActionComponent>(animation).state == ActionState::Running}});
}

// ================================================================== keyframe animations and tweens

enum class Channel { Position, Rotation, Scale, Morph, Lattice };

const std::vector<std::pair<const char*, Channel>>& channels() {
    static const std::vector<std::pair<const char*, Channel>> table = {
        {"position", Channel::Position}, {"rotation", Channel::Rotation}, {"scale", Channel::Scale}, {"morph", Channel::Morph},
        {"lattice", Channel::Lattice}};
    return table;
}

bool hasChannel(Scene* scene, Entity action, Channel channel) {
    switch (channel) {
        case Channel::Position: return scene->findComponent<TranslateTracksComponent>(action) != nullptr;
        case Channel::Rotation: return scene->findComponent<RotateTracksComponent>(action) != nullptr;
        case Channel::Scale: return scene->findComponent<ScaleTracksComponent>(action) != nullptr;
        case Channel::Morph: return scene->findComponent<MorphTracksComponent>(action) != nullptr;
        case Channel::Lattice: return scene->findComponent<LatticeTracksComponent>(action) != nullptr;
    }
    return false;
}

std::string actionKind(Scene* scene, Entity action) {
    if (scene->findComponent<KeyframeTracksComponent>(action)) {
        for (const auto& item : channels()) {
            if (hasChannel(scene, action, item.second)) return std::string(item.first) + "_track";
        }
        return "track";
    }
    if (scene->findComponent<PositionTweenComponent>(action)) return "position_tween";
    if (scene->findComponent<RotationTweenComponent>(action)) return "rotation_tween";
    if (scene->findComponent<ScaleTweenComponent>(action)) return "scale_tween";
    if (scene->findComponent<ColorTweenComponent>(action)) return "color_tween";
    if (scene->findComponent<AlphaTweenComponent>(action)) return "alpha_tween";
    if (scene->findComponent<SpriteAnimationComponent>(action)) return "sprite_animation";
    if (scene->findComponent<ParticleSystemComponent>(action)) return "particles";
    if (scene->findComponent<AnimationComponent>(action)) return "animation";
    return "action";
}

struct Key {
    float time = 0.0f;
    Vector3 vec;
    Quaternion quat;
    std::vector<float> weights;
    // lattice: offsets of points (flat indices, ascending, once each), or the lattice's own
    // current offsets chosen as editor keying does (resolved by the caller)
    std::vector<int> points;
    std::vector<Vector3> offsets;
    bool current = false;
    bool rest = false;     // "rest": every keyed point at zero
    std::string pose;      // a pose name: the pose's offsets
    EaseType ease = EaseType::LINEAR;
};

// A lattice key's points: {"<index>": [x,y,z]}, [[[i,j,k] | index, [x,y,z]], ...], "current",
// "rest" or the name of one of the lattice's poses
bool parseLatticePoints(const Json& v, const LatticeComponent& lattice, Key& key, std::string& error) {
    if (v.is_string()) {
        const std::string name = v.get<std::string>();
        if (lowerText(name) == "current") {
            key.current = true;
            return true;
        }
        if (lowerText(name) == "rest") {
            key.rest = true;
            return true;
        }
        std::string poses;
        for (const LatticePose& pose : lattice.poses) {
            if (pose.name == name) {
                key.pose = name;
                return true;
            }
            poses += (poses.empty() ? "" : ", ") + pose.name;
        }
        error = "points: " + v.dump() + " is not \"current\", \"rest\" or a pose of the lattice (" +
                (poses.empty() ? std::string("it has no poses: save one with edit_lattice ops [{op: pose, save: name}]") : "poses: " + poses) + ").";
        return false;
    }
    const int rx = std::clamp(lattice.resolutionX, 2, 16), ry = std::clamp(lattice.resolutionY, 2, 16),
              rz = std::clamp(lattice.resolutionZ, 2, 16);
    const int count = rx * ry * rz;
    const std::string shape = "{\"<index>\": [x,y,z]}, [[[i,j,k], [x,y,z]], ...] or \"current\"";
    std::map<int, Vector3> offsets;
    auto add = [&](int point, const Json& offset, const std::string& where) {
        if (point < 0 || point >= count) {
            error = where + ": point " + std::to_string(point) + " is out of range (" + std::to_string(count) + " points, resolution " +
                    std::to_string(rx) + "x" + std::to_string(ry) + "x" + std::to_string(rz) + ", index i + rx*(j + ry*k)).";
            return false;
        }
        Vector3 value;
        if (!McpValues::parseVector3(offset, value)) {
            error = where + " must be an offset [x, y, z] (lattice local units).";
            return false;
        }
        offsets[point] = value;
        return true;
    };
    if (v.is_object()) {
        for (auto it = v.begin(); it != v.end(); ++it) {
            const std::string& name = it.key();
            const std::string where = "points[\"" + name + "\"]";
            if (name.empty() || name.size() > 9 || !std::all_of(name.begin(), name.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
                error = where + ": keys are flat point indices (\"13\"); use [[[i,j,k], [x,y,z]], ...] for grid coordinates.";
                return false;
            }
            if (!add(std::stoi(name), it.value(), where)) return false;
        }
    } else if (v.is_array()) {
        for (size_t n = 0; n < v.size(); n++) {
            const Json& pair = v[n];
            const std::string where = "points[" + std::to_string(n) + "]";
            if (!pair.is_array() || pair.size() != 2) {
                error = where + " must be a pair [[i,j,k], [x,y,z]] (or [index, [x,y,z]]).";
                return false;
            }
            const Json& at = pair[0];
            int point = -1;
            if (at.is_number_integer()) {
                point = at.get<int>();
            } else if (at.is_array() && at.size() == 3 && at[0].is_number_integer() && at[1].is_number_integer() && at[2].is_number_integer()) {
                const int i = at[0].get<int>(), j = at[1].get<int>(), k = at[2].get<int>();
                if (i < 0 || i >= rx || j < 0 || j >= ry || k < 0 || k >= rz) {
                    error = where + ": (" + std::to_string(i) + ", " + std::to_string(j) + ", " + std::to_string(k) +
                            ") is outside the " + std::to_string(rx) + "x" + std::to_string(ry) + "x" + std::to_string(rz) + " lattice (0-based).";
                    return false;
                }
                point = lattice::pointIndex(rx, ry, i, j, k);
            } else {
                error = where + "[0] must be [i, j, k] (0-based integers) or a flat index.";
                return false;
            }
            if (!add(point, pair[1], where + "[1]")) return false;
        }
    } else {
        error = "points must be " + shape + ".";
        return false;
    }
    if (offsets.empty()) {
        error = "points is empty: give at least one point, or \"current\".";
        return false;
    }
    for (const auto& entry : offsets) {
        key.points.push_back(entry.first);
        key.offsets.push_back(entry.second);
    }
    return true;
}

bool parseKeys(Channel channel, const Json& value, std::vector<Key>& keys, std::string& error, const LatticeComponent* lattice = nullptr) {
    if (!value.is_array() || value.empty()) {
        error = "keys must be a non-empty array of {time, value, ease}.";
        return false;
    }
    for (size_t i = 0; i < value.size(); i++) {
        const Json& item = value[i];
        Key key;
        if (!has(item, "time") || !item["time"].is_number() || item["time"].get<float>() < 0.0f) {
            error = "keys[" + std::to_string(i) + "].time must be a number of seconds >= 0.";
            return false;
        }
        key.time = item["time"].get<float>();
        if (channel == Channel::Lattice) {
            // spec §7 names it points; value is accepted too
            const char* field = has(item, "points") ? "points" : "value";
            if (!has(item, field)) {
                error = "keys[" + std::to_string(i) + "].points is required (lattice offsets).";
                return false;
            }
            if (!lattice || !parseLatticePoints(item[field], *lattice, key, error)) {
                error = "keys[" + std::to_string(i) + "]." + (lattice ? (std::string(field) == "value" ? "value (a lattice key's points): " : "") + error
                                                                       : std::string("points: the target has no Lattice."));
                return false;
            }
            if (has(item, "ease") && !parseEase(item["ease"], key.ease, error)) {
                error = "keys[" + std::to_string(i) + "]: " + error;
                return false;
            }
            keys.push_back(key);
            continue;
        }
        if (!has(item, "value")) {
            error = "keys[" + std::to_string(i) + "].value is required.";
            return false;
        }
        const Json& v = item["value"];
        bool ok = true;
        if (channel == Channel::Rotation) ok = McpValues::parseQuaternion(v, key.quat);
        else if (channel == Channel::Morph) {
            ok = v.is_array();
            for (const Json& w : v) {
                if (!w.is_number()) ok = false;
                else key.weights.push_back(w.get<float>());
            }
        } else ok = McpValues::parseVector3(v, key.vec);
        if (!ok) {
            error = "keys[" + std::to_string(i) + "].value must be " +
                    (channel == Channel::Rotation ? std::string("[x, y, z, w] or {\"euler\": [x, y, z]} degrees")
                     : channel == Channel::Morph ? std::string("an array of morph target weights") : std::string("[x, y, z]")) + ".";
            return false;
        }
        if (has(item, "ease") && !parseEase(item["ease"], key.ease, error)) {
            error = "keys[" + std::to_string(i) + "]: " + error;
            return false;
        }
        keys.push_back(key);
    }
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
    return true;
}

// Existing keys of a track entity, so merge mode can add to them
void readKeys(Scene* scene, Entity action, Channel channel, std::vector<Key>& keys) {
    KeyframeTracksComponent* kf = scene->findComponent<KeyframeTracksComponent>(action);
    if (!kf) return;
    for (size_t i = 0; i < kf->times.size(); i++) {
        Key key;
        key.time = kf->times[i];
        key.ease = i < kf->easings.size() ? kf->easings[i] : EaseType::LINEAR;
        bool ok = false;
        switch (channel) {
            case Channel::Position: if (auto* t = scene->findComponent<TranslateTracksComponent>(action)) { ok = i < t->values.size(); if (ok) key.vec = t->values[i]; } break;
            case Channel::Rotation: if (auto* t = scene->findComponent<RotateTracksComponent>(action)) { ok = i < t->values.size(); if (ok) key.quat = t->values[i]; } break;
            case Channel::Scale: if (auto* t = scene->findComponent<ScaleTracksComponent>(action)) { ok = i < t->values.size(); if (ok) key.vec = t->values[i]; } break;
            case Channel::Morph: if (auto* t = scene->findComponent<MorphTracksComponent>(action)) { ok = i < t->values.size(); if (ok) key.weights = t->values[i]; } break;
            case Channel::Lattice:
                if (auto* t = scene->findComponent<LatticeTracksComponent>(action)) {
                    ok = i < t->values.size();
                    for (size_t n = 0; ok && n < t->points.size() && n < t->values[i].size(); n++) {
                        key.points.push_back(t->points[n]);
                        key.offsets.push_back(t->values[i][n]);
                    }
                }
                break;
        }
        if (ok) keys.push_back(key);
    }
}

std::vector<EaseType> easingsOf(const std::vector<Key>& keys) {
    std::vector<EaseType> easings;
    for (size_t i = 0; i + 1 < keys.size(); i++) easings.push_back(keys[i].ease);
    if (std::all_of(easings.begin(), easings.end(), [](EaseType e) { return e == EaseType::LINEAR; })) easings.clear();
    return easings;
}

std::vector<float> timesOf(const std::vector<Key>& keys) {
    std::vector<float> times;
    for (const Key& key : keys) times.push_back(key.time);
    return times;
}

// Writes the keys onto a track entity: times, values, easings, tangents cleared (linear keys)
void addTrackWrites(MultiPropertyCmd& multi, const Target& target, Entity action, Channel channel, const std::vector<Key>& keys) {
    addProp<std::vector<float>>(multi, target, action, ComponentType::KeyframeTracksComponent, "times", timesOf(keys));
    addProp<std::vector<EaseType>>(multi, target, action, ComponentType::KeyframeTracksComponent, "easings", easingsOf(keys));
    auto writeValues = [&](auto sample, ComponentType type, auto get) {
        using T = decltype(sample);
        std::vector<T> values;
        for (const Key& key : keys) values.push_back(get(key));
        addProp<std::vector<T>>(multi, target, action, type, "values", values);
        addProp<std::vector<T>>(multi, target, action, type, "inTangents", std::vector<T>());
        addProp<std::vector<T>>(multi, target, action, type, "outTangents", std::vector<T>());
    };
    switch (channel) {
        case Channel::Position: writeValues(Vector3(), ComponentType::TranslateTracksComponent, [](const Key& k) { return k.vec; }); break;
        case Channel::Rotation: writeValues(Quaternion(), ComponentType::RotateTracksComponent, [](const Key& k) { return k.quat; }); break;
        case Channel::Scale: writeValues(Vector3(), ComponentType::ScaleTracksComponent, [](const Key& k) { return k.vec; }); break;
        case Channel::Morph: writeValues(std::vector<float>(), ComponentType::MorphTracksComponent, [](const Key& k) { return k.weights; }); break;
        case Channel::Lattice: break; // whole tracks, addLatticeTrackWrites
    }
}

// The "current" lattice keys take the lattice's offsets, the points chosen as the Animation
// window's keying chooses them (selected points, else moved points plus the track's own)
void resolveCurrentLatticeKeys(uint32_t sceneId, Entity latticeEntity, const LatticeComponent& lattice,
                               const LatticeTracksComponent* existing, std::vector<Key>& keys) {
    for (Key& key : keys) {
        if (!key.current) continue;
        key.points = LatticeTrackKeys::pointsToKey(sceneId, latticeEntity, lattice, existing);
        key.offsets.clear();
        for (int point : key.points) key.offsets.push_back(LatticeTrackKeys::currentOffset(lattice, point));
    }
}

// "rest" and pose keys name every point the other keys, the poses used and (merging) the track
// already animate, so each holds the whole shape at its time (every point when none is named)
void resolveShapeLatticeKeys(const LatticeComponent& lattice, const LatticeTracksComponent* existing, std::vector<Key>& keys) {
    const int count = std::clamp(lattice.resolutionX, 2, 16) * std::clamp(lattice.resolutionY, 2, 16) * std::clamp(lattice.resolutionZ, 2, 16);
    auto poseOf = [&lattice](const std::string& name) -> const LatticePose* {
        for (const LatticePose& pose : lattice.poses) {
            if (pose.name == name) return &pose;
        }
        return nullptr;
    };
    std::set<int> points;
    bool shapes = false;
    for (const Key& key : keys) {
        if (key.rest || !key.pose.empty()) {
            shapes = true;
            if (const LatticePose* pose = key.pose.empty() ? nullptr : poseOf(key.pose)) {
                for (size_t p = 0; p < pose->offsets.size() && static_cast<int>(p) < count; p++) {
                    if (!(pose->offsets[p] == Vector3(0, 0, 0))) points.insert(static_cast<int>(p));
                }
            }
        } else {
            points.insert(key.points.begin(), key.points.end());
        }
    }
    if (!shapes) return;
    if (existing) points.insert(existing->points.begin(), existing->points.end());
    if (points.empty()) {
        for (int p = 0; p < count; p++) points.insert(p);
    }
    for (Key& key : keys) {
        if (!key.rest && key.pose.empty()) continue;
        const LatticePose* pose = key.pose.empty() ? nullptr : poseOf(key.pose);
        key.points.assign(points.begin(), points.end());
        key.offsets.clear();
        for (int p : key.points) {
            key.offsets.push_back(pose && static_cast<size_t>(p) < pose->offsets.size() ? pose->offsets[static_cast<size_t>(p)] : Vector3(0, 0, 0));
        }
    }
}

// Lattice keys as a whole track: one column per point any key names; a key that does not name
// a point gets it interpolated in time between the nearest keys naming it (held past the first
// and last). Merge: each key is set into the existing track (LatticeTrackKeys::setKey). Linear
// keys, tangents cleared like the other channels.
LatticeTrackData latticeTrackData(Scene* scene, Entity track, const std::vector<Key>& keys, const LatticeComponent& lattice, bool merge) {
    LatticeTrackData data;
    KeyframeTracksComponent* kf = track != NULL_ENTITY ? scene->findComponent<KeyframeTracksComponent>(track) : nullptr;
    LatticeTracksComponent* tracks = track != NULL_ENTITY ? scene->findComponent<LatticeTracksComponent>(track) : nullptr;
    if (merge && kf && tracks) {
        data = LatticeTrackData(*kf, *tracks);
        for (const Key& key : keys) {
            LatticeTrackKeys::setKey(data, key.time, key.points, key.offsets, lattice, 0.001f);
        }
        // each given key sets the ease to its next key
        if (data.times.size() > 1) data.easings.resize(data.times.size() - 1, EaseType::LINEAR);
        for (const Key& key : keys) {
            for (size_t i = 0; i + 1 < data.times.size(); i++) {
                if (std::abs(data.times[i] - key.time) < 0.001f) data.easings[i] = key.ease;
            }
        }
        if (std::all_of(data.easings.begin(), data.easings.end(), [](EaseType e) { return e == EaseType::LINEAR; })) data.easings.clear();
    } else {
        data.times = timesOf(keys);
        data.easings = easingsOf(keys);
        for (const Key& key : keys) data.points.insert(data.points.end(), key.points.begin(), key.points.end());
        std::sort(data.points.begin(), data.points.end());
        data.points.erase(std::unique(data.points.begin(), data.points.end()), data.points.end());
        data.values.assign(keys.size(), std::vector<Vector3>(data.points.size()));
        for (size_t column = 0; column < data.points.size(); column++) {
            std::vector<int> known(keys.size(), -1); // index into the key's own lists
            for (size_t k = 0; k < keys.size(); k++) {
                auto it = std::find(keys[k].points.begin(), keys[k].points.end(), data.points[column]);
                if (it != keys[k].points.end()) known[k] = static_cast<int>(it - keys[k].points.begin());
            }
            for (size_t k = 0; k < keys.size(); k++) {
                if (known[k] >= 0) { data.values[k][column] = keys[k].offsets[known[k]]; continue; }
                int before = -1, after = -1;
                for (int b = static_cast<int>(k) - 1; b >= 0 && before < 0; b--) if (known[b] >= 0) before = b;
                for (size_t a = k + 1; a < keys.size() && after < 0; a++) if (known[a] >= 0) after = static_cast<int>(a);
                const Vector3 from = before >= 0 ? keys[before].offsets[known[before]] : keys[after].offsets[known[after]];
                const Vector3 to = after >= 0 ? keys[after].offsets[known[after]] : from;
                float t = 0.0f;
                if (before >= 0 && after >= 0 && keys[after].time > keys[before].time) {
                    t = (keys[k].time - keys[before].time) / (keys[after].time - keys[before].time);
                }
                data.values[k][column] = from + t * (to - from);
            }
        }
    }
    data.inTangents.clear();
    data.outTangents.clear();
    return data;
}

void addLatticeTrackWrites(MultiPropertyCmd& multi, const Target& target, Entity action, const LatticeTrackData& data) {
    addProp<std::vector<float>>(multi, target, action, ComponentType::KeyframeTracksComponent, "times", data.times);
    addProp<std::vector<EaseType>>(multi, target, action, ComponentType::KeyframeTracksComponent, "easings", data.easings);
    addProp<std::vector<int>>(multi, target, action, ComponentType::LatticeTracksComponent, "points", data.points);
    addProp<std::vector<std::vector<Vector3>>>(multi, target, action, ComponentType::LatticeTracksComponent, "values", data.values);
    addProp<std::vector<std::vector<Vector3>>>(multi, target, action, ComponentType::LatticeTracksComponent, "inTangents", data.inTangents);
    addProp<std::vector<std::vector<Vector3>>>(multi, target, action, ComponentType::LatticeTracksComponent, "outTangents", data.outTangents);
}

void seedTrack(CreateEntityCmd& create, Channel channel, const std::vector<Key>& keys) {
    create.addProperty<std::vector<float>>(ComponentType::KeyframeTracksComponent, "times", timesOf(keys));
    create.addProperty<std::vector<EaseType>>(ComponentType::KeyframeTracksComponent, "easings", easingsOf(keys));
    switch (channel) {
        case Channel::Position: { std::vector<Vector3> v; for (const Key& k : keys) v.push_back(k.vec); create.addProperty(ComponentType::TranslateTracksComponent, "values", v); break; }
        case Channel::Rotation: { std::vector<Quaternion> v; for (const Key& k : keys) v.push_back(k.quat); create.addProperty(ComponentType::RotateTracksComponent, "values", v); break; }
        case Channel::Scale: { std::vector<Vector3> v; for (const Key& k : keys) v.push_back(k.vec); create.addProperty(ComponentType::ScaleTracksComponent, "values", v); break; }
        case Channel::Morph: { std::vector<std::vector<float>> v; for (const Key& k : keys) v.push_back(k.weights); create.addProperty(ComponentType::MorphTracksComponent, "values", v); break; }
        case Channel::Lattice: break; // seedLatticeTrack
    }
}

void seedLatticeTrack(CreateEntityCmd& create, const LatticeTrackData& data) {
    create.addProperty<std::vector<float>>(ComponentType::KeyframeTracksComponent, "times", data.times);
    create.addProperty<std::vector<EaseType>>(ComponentType::KeyframeTracksComponent, "easings", data.easings);
    create.addProperty<std::vector<int>>(ComponentType::LatticeTracksComponent, "points", data.points);
    create.addProperty<std::vector<std::vector<Vector3>>>(ComponentType::LatticeTracksComponent, "values", data.values);
}

EntityCreationType trackCreationType(Channel channel) {
    switch (channel) {
        case Channel::Position: return EntityCreationType::TRANSLATE_TRACKS;
        case Channel::Rotation: return EntityCreationType::ROTATE_TRACKS;
        case Channel::Scale: return EntityCreationType::SCALE_TRACKS;
        case Channel::Morph: return EntityCreationType::MORPH_TRACKS;
        case Channel::Lattice: return EntityCreationType::LATTICE_TRACKS;
    }
    return EntityCreationType::TRANSLATE_TRACKS;
}

Json keysJson(Scene* scene, Entity action, Channel channel, size_t limit) {
    std::vector<Key> keys;
    readKeys(scene, action, channel, keys);
    Json out = Json::array();
    for (size_t i = 0; i < keys.size() && i < limit; i++) {
        Json item = {{"time", keys[i].time}};
        if (channel == Channel::Rotation) item["value"] = McpValues::toJson(keys[i].quat);
        else if (channel == Channel::Morph) item["value"] = keys[i].weights;
        else if (channel == Channel::Lattice) {
            // a key holding every animated point at zero reads as "rest" (it is what "rest" writes)
            const bool rest = std::all_of(keys[i].offsets.begin(), keys[i].offsets.end(), [](const Vector3& o) { return o == Vector3(0, 0, 0); });
            Json points = Json::object();
            for (size_t n = 0; n < keys[i].points.size(); n++) points[std::to_string(keys[i].points[n])] = McpValues::toJson(keys[i].offsets[n]);
            item["points"] = rest ? Json("rest") : points;
        }
        else item["value"] = McpValues::toJson(keys[i].vec);
        if (i + 1 < keys.size()) item["ease"] = easeName(keys[i].ease);
        out.push_back(item);
    }
    return out;
}

Json animationJson(const Target& target, Entity animation, size_t keyLimit) {
    Scene* scene = target.engineScene();
    AnimationComponent& anim = scene->getComponent<AnimationComponent>(animation);
    ActionComponent* action = scene->findComponent<ActionComponent>(animation);
    ActionSystem* actions = scene->getSystem<ActionSystem>().get();
    Json list = Json::array();
    float length = 0.0f;
    for (size_t i = 0; i < anim.actions.size(); i++) {
        const ActionFrame& frame = anim.actions[i];
        Json item = {{"index", i}, {"start_time", frame.startTime}, {"duration", frame.duration}, {"lane", frame.track}};
        if (frame.action == NULL_ENTITY || !scene->isEntityCreated(frame.action)) {
            item["action"] = nullptr;
            list.push_back(item);
            continue;
        }
        const float effective = actions ? actions->getFrameDuration(frame) : frame.duration;
        length = std::max(length, frame.startTime + effective);
        item["effective_duration"] = effective;
        item["action"] = entityJson(target, frame.action);
        const std::string kind = actionKind(scene, frame.action);
        item["kind"] = kind;
        if (ActionComponent* ac = scene->findComponent<ActionComponent>(frame.action)) {
            item["target"] = ac->target != NULL_ENTITY && scene->isEntityCreated(ac->target) ? entityJson(target, ac->target) : Json(nullptr);
        }
        for (const auto& c : channels()) {
            if (kind == std::string(c.first) + "_track") item["keys"] = keysJson(scene, frame.action, c.second, keyLimit);
        }
        if (TweenComponent* tween = scene->findComponent<TweenComponent>(frame.action)) {
            Json t = {{"duration", tween->duration}, {"loop", tween->loop}, {"ease", easeName(tween->function.getType())}};
            if (auto* p = scene->findComponent<PositionTweenComponent>(frame.action)) { t["from"] = McpValues::toJson(p->startPosition); t["to"] = McpValues::toJson(p->endPosition); }
            if (auto* p = scene->findComponent<RotationTweenComponent>(frame.action)) { t["from"] = McpValues::toJson(p->startRotation); t["to"] = McpValues::toJson(p->endRotation); }
            if (auto* p = scene->findComponent<ScaleTweenComponent>(frame.action)) { t["from"] = McpValues::toJson(p->startScale); t["to"] = McpValues::toJson(p->endScale); }
            if (auto* p = scene->findComponent<ColorTweenComponent>(frame.action)) { t["from"] = McpValues::toJson(p->startColor); t["to"] = McpValues::toJson(p->endColor); }
            if (auto* p = scene->findComponent<AlphaTweenComponent>(frame.action)) { t["from"] = p->startAlpha; t["to"] = p->endAlpha; }
            item["tween"] = t;
        }
        list.push_back(item);
    }
    Json out = {{"animation", entityJson(target, animation)}, {"loop", anim.loop}, {"duration", anim.duration},
                {"length", length}, {"actions", list}};
    if (action) {
        out["target"] = action->target != NULL_ENTITY && scene->isEntityCreated(action->target) ? entityJson(target, action->target) : Json(nullptr);
        out["autoplay"] = action->state == ActionState::Running;
        out["speed"] = action->speed;
    }
    return out;
}

// Track entity animating `target`'s channel in this animation
Entity findTrack(Scene* scene, const std::vector<ActionFrame>& frames, Entity target, Channel channel, size_t& frameIndex) {
    for (size_t i = 0; i < frames.size(); i++) {
        Entity a = frames[i].action;
        if (a == NULL_ENTITY || !scene->isEntityCreated(a) || !scene->findComponent<KeyframeTracksComponent>(a)) continue;
        ActionComponent* ac = scene->findComponent<ActionComponent>(a);
        if (ac && ac->target == target && hasChannel(scene, a, channel)) {
            frameIndex = i;
            return a;
        }
    }
    return NULL_ENTITY;
}

void editAnimationTool(const CallPtr& call) {
    Target target;
    if (!resolveSceneArg(call, target) || !sceneStopped(call, target)) return;
    const Json& args = call->args();
    Scene* scene = target.engineScene();
    std::string error;
    UndoStep step(target.sceneId());
    auto failWith = [&](const std::string& message) {
        step.fail(call, message);
    };

    Entity animTarget = NULL_ENTITY;
    if (has(args, "target") && (animTarget = resolveRef(target.scene, args["target"], error)) == NULL_ENTITY) {
        call->fail("target: " + error);
        return;
    }
    Entity animation = NULL_ENTITY;
    bool created = false;
    if (has(args, "animation")) {
        animation = resolveRef(target.scene, args["animation"], error);
        if (animation == NULL_ENTITY) {
            call->fail("animation: " + error);
            return;
        }
        if (!scene->findComponent<AnimationComponent>(animation)) {
            call->fail("\"" + scene->getEntityName(animation) + "\" has no Animation component. Omit animation (with target) to create one.");
            return;
        }
        if (animTarget == NULL_ENTITY) {
            ActionComponent* ac = scene->findComponent<ActionComponent>(animation);
            if (ac) animTarget = ac->target;
        }
    } else {
        if (animTarget == NULL_ENTITY) {
            call->fail("Pass animation (an existing Animation GameObject) or target (the GameObject a new Animation animates).");
            return;
        }
        auto* create = new CreateEntityCmd(target.project, target.sceneId(), stringOr(args, "name", "Animation"),
                                           EntityCreationType::ANIMATION, animTarget);
        if (!step.add(create)) {
            call->fail("Could not create the Animation GameObject.");
            return;
        }
        animation = create->getEntity();
        created = true;
    }

    AnimationComponent& anim = scene->getComponent<AnimationComponent>(animation);
    std::vector<ActionFrame> frames = anim.actions;
    std::vector<bool> drop(frames.size(), false);
    std::vector<Entity> toDelete;

    // Existing frames: timing edits, then removals (indices are the ones before this call)
    if (has(args, "set_frames")) {
        if (!args["set_frames"].is_array()) { failWith("set_frames must be an array of {index, start_time, duration, lane}."); return; }
        for (const Json& item : args["set_frames"]) {
            const int index = intOr(item, "index", -1);
            if (index < 0 || index >= static_cast<int>(frames.size())) { failWith("set_frames: index " + std::to_string(index) + " is out of range (" + std::to_string(frames.size()) + " actions)."); return; }
            frames[index].startTime = std::max(0.0f, numberOr(item, "start_time", frames[index].startTime));
            frames[index].duration = numberOr(item, "duration", frames[index].duration);
            frames[index].track = static_cast<uint32_t>(std::max(0, intOr(item, "lane", static_cast<int>(frames[index].track))));
        }
    }
    if (has(args, "remove_actions")) {
        if (!args["remove_actions"].is_array()) { failWith("remove_actions must be an array of action indices."); return; }
        for (const Json& v : args["remove_actions"]) {
            const int index = v.is_number_integer() ? v.get<int>() : -1;
            if (index < 0 || index >= static_cast<int>(frames.size())) { failWith("remove_actions: index " + v.dump() + " is out of range (" + std::to_string(frames.size()) + " actions)."); return; }
            drop[index] = true;
        }
        if (boolOr(args, "delete_removed", true)) {
            for (size_t i = 0; i < frames.size(); i++) {
                if (!drop[i] || frames[i].action == NULL_ENTITY || !scene->isEntityCreated(frames[i].action)) continue;
                bool usedElsewhere = false;
                for (size_t j = 0; j < frames.size(); j++) usedElsewhere |= !drop[j] && frames[j].action == frames[i].action;
                if (!usedElsewhere && std::find(toDelete.begin(), toDelete.end(), frames[i].action) == toDelete.end()) toDelete.push_back(frames[i].action);
            }
        }
    }
    std::vector<ActionFrame> kept;
    for (size_t i = 0; i < frames.size(); i++) {
        if (!drop[i]) kept.push_back(frames[i]);
    }
    uint32_t nextLane = 0;
    for (const ActionFrame& f : kept) nextLane = std::max(nextLane, f.track + 1);

    auto* edits = new MultiPropertyCmd();
    std::unique_ptr<MultiPropertyCmd> editsGuard(edits);
    int tracksWritten = 0, tracksCreated = 0, tweensCreated = 0;

    // Keyframe tracks
    if (has(args, "tracks")) {
        if (!args["tracks"].is_array()) { failWith("tracks must be an array of {channel, keys, ...}."); return; }
        for (size_t t = 0; t < args["tracks"].size(); t++) {
            const Json& item = args["tracks"][t];
            const std::string where = "tracks[" + std::to_string(t) + "]";
            Channel channel;
            if (!pickName(channels(), stringOr(item, "channel", "position"), channel, where + ".channel", error)) { failWith(error); return; }
            Entity trackTarget = animTarget;
            if (has(item, "target") && (trackTarget = resolveRef(target.scene, item["target"], error)) == NULL_ENTITY) { failWith(where + ".target: " + error); return; }
            if (trackTarget == NULL_ENTITY) { failWith(where + ": no target GameObject (pass target)."); return; }
            LatticeComponent* lattice = scene->findComponent<LatticeComponent>(trackTarget);
            if (channel == Channel::Lattice && !lattice) {
                // the usual mistake: the mesh instead of the cage that deforms it
                std::string cages;
                if (const LatticeModifierComponent* modifier = scene->findComponent<LatticeModifierComponent>(trackTarget)) {
                    for (const LatticeBinding& binding : modifier->bindings) {
                        if (binding.lattice != NULL_ENTITY && scene->isEntityCreated(binding.lattice))
                            cages += (cages.empty() ? "" : ", ") + ("\"" + scene->getEntityName(binding.lattice) + "\" [" + std::to_string(binding.lattice) + "]");
                    }
                }
                failWith(where + ": \"" + scene->getEntityName(trackTarget) + "\" has no Lattice to animate: a lattice track's target is the cage "
                         "(the lattice GameObject), not the mesh it deforms" + (cages.empty() ? std::string(".") : "; its cage: " + cages + "."));
                return;
            }
            if (channel != Channel::Morph && !scene->findComponent<Transform>(trackTarget)) { failWith(where + ": the target has no Transform to animate."); return; }
            std::vector<Key> keys;
            if (!parseKeys(channel, has(item, "keys") ? item["keys"] : Json(), keys, error, lattice)) { failWith(where + "." + error); return; }

            size_t frameIndex = 0;
            Entity track = findTrack(scene, kept, trackTarget, channel, frameIndex);
            const bool merge = lowerText(stringOr(item, "mode", "replace")) == "merge";
            if (channel == Channel::Lattice) {
                const LatticeTracksComponent* existing = track != NULL_ENTITY ? scene->findComponent<LatticeTracksComponent>(track) : nullptr;
                resolveCurrentLatticeKeys(target.sceneId(), trackTarget, *lattice, existing, keys);
                resolveShapeLatticeKeys(*lattice, merge ? existing : nullptr, keys);
            }
            if (track != NULL_ENTITY && channel == Channel::Lattice) {
                addLatticeTrackWrites(*edits, target, track, latticeTrackData(scene, track, keys, *lattice, merge));
                if (has(item, "start_time")) kept[frameIndex].startTime = std::max(0.0f, numberOr(item, "start_time", 0.0f));
                tracksWritten++;
            } else if (track != NULL_ENTITY) {
                if (merge) {
                    std::vector<Key> merged;
                    readKeys(scene, track, channel, merged);
                    for (const Key& key : keys) {
                        auto same = std::find_if(merged.begin(), merged.end(), [&](const Key& k) { return std::abs(k.time - key.time) < 0.001f; });
                        if (same != merged.end()) *same = key;
                        else merged.push_back(key);
                    }
                    std::stable_sort(merged.begin(), merged.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
                    keys = merged;
                }
                addTrackWrites(*edits, target, track, channel, keys);
                if (has(item, "start_time")) kept[frameIndex].startTime = std::max(0.0f, numberOr(item, "start_time", 0.0f));
                tracksWritten++;
            } else {
                static const char* names[] = {"Position Tracks", "Rotation Tracks", "Scale Tracks", "Morph Tracks", "Lattice Tracks"};
                auto* create = new CreateEntityCmd(target.project, target.sceneId(), stringOr(item, "name", names[static_cast<int>(channel)]),
                                                   trackCreationType(channel), trackTarget);
                if (channel == Channel::Lattice) seedLatticeTrack(*create, latticeTrackData(scene, NULL_ENTITY, keys, *lattice, false));
                else seedTrack(*create, channel, keys);
                if (!step.add(create)) { failWith(where + ": could not create the track."); return; }
                ActionFrame frame;
                frame.startTime = std::max(0.0f, numberOr(item, "start_time", 0.0f));
                frame.duration = 0.0f;
                frame.action = create->getEntity();
                frame.track = nextLane++;
                kept.push_back(frame);
                tracksCreated++;
            }
        }
    }

    // Tweens
    if (has(args, "tweens")) {
        if (!args["tweens"].is_array()) { failWith("tweens must be an array of {type, from, to, duration, ...}."); return; }
        static const std::vector<std::pair<const char*, EntityCreationType>> tweenTypes = {
            {"position", EntityCreationType::POSITION_TWEEN}, {"rotation", EntityCreationType::ROTATION_TWEEN},
            {"scale", EntityCreationType::SCALE_TWEEN}, {"color", EntityCreationType::COLOR_TWEEN}, {"alpha", EntityCreationType::ALPHA_TWEEN}};
        for (size_t t = 0; t < args["tweens"].size(); t++) {
            const Json& item = args["tweens"][t];
            const std::string where = "tweens[" + std::to_string(t) + "]";
            EntityCreationType type;
            const std::string typeName = lowerText(stringOr(item, "type", ""));
            if (!pickName(tweenTypes, typeName, type, where + ".type", error)) { failWith(error); return; }
            Entity tweenTarget = animTarget;
            if (has(item, "target") && (tweenTarget = resolveRef(target.scene, item["target"], error)) == NULL_ENTITY) { failWith(where + ".target: " + error); return; }
            if (tweenTarget == NULL_ENTITY) { failWith(where + ": no target GameObject (pass target)."); return; }
            if (!has(item, "to")) { failWith(where + ": to is required (and from, defaulting to the target's current value)."); return; }
            const float duration = numberOr(item, "duration", 1.0f);
            if (duration <= 0.0f) { failWith(where + ".duration must be > 0 seconds."); return; }
            EaseType ease = EaseType::LINEAR;
            if (has(item, "ease") && !parseEase(item["ease"], ease, error)) { failWith(where + ": " + error); return; }

            auto* create = new CreateEntityCmd(target.project, target.sceneId(), stringOr(item, "name", typeName.empty() ? "Tween" : std::string(1, static_cast<char>(std::toupper(typeName[0]))) + typeName.substr(1) + " Tween"),
                                               type, tweenTarget);
            std::unique_ptr<CreateEntityCmd> guard(create);
            create->addProperty<float>(ComponentType::TweenComponent, "duration", duration);
            create->addProperty<bool>(ComponentType::TweenComponent, "loop", boolOr(item, "loop", false));
            create->addProperty<Ease>(ComponentType::TweenComponent, "function", Ease(ease));
            Transform* tf = scene->findComponent<Transform>(tweenTarget);
            bool ok = true;
            if (typeName == "position" || typeName == "scale") {
                Vector3 from = tf ? (typeName == "position" ? tf->position : tf->scale) : Vector3::ZERO, to;
                ok = (!has(item, "from") || McpValues::parseVector3(item["from"], from)) && McpValues::parseVector3(item["to"], to);
                const ComponentType comp = typeName == "position" ? ComponentType::PositionTweenComponent : ComponentType::ScaleTweenComponent;
                create->addProperty<Vector3>(comp, typeName == "position" ? "startPosition" : "startScale", from);
                create->addProperty<Vector3>(comp, typeName == "position" ? "endPosition" : "endScale", to);
            } else if (typeName == "rotation") {
                Quaternion from = tf ? tf->rotation : Quaternion(), to;
                ok = (!has(item, "from") || McpValues::parseQuaternion(item["from"], from)) && McpValues::parseQuaternion(item["to"], to);
                create->addProperty<Quaternion>(ComponentType::RotationTweenComponent, "startRotation", from);
                create->addProperty<Quaternion>(ComponentType::RotationTweenComponent, "endRotation", to);
                create->addProperty<bool>(ComponentType::RotationTweenComponent, "shortestPath", boolOr(item, "shortest_path", true));
            } else if (typeName == "color") {
                Vector3 from(1.0f, 1.0f, 1.0f), to;
                ok = (!has(item, "from") || parseColor3(item["from"], from)) && parseColor3(item["to"], to);
                create->addProperty<Vector3>(ComponentType::ColorTweenComponent, "startColor", from);
                create->addProperty<Vector3>(ComponentType::ColorTweenComponent, "endColor", to);
            } else {
                ok = item["to"].is_number() && (!has(item, "from") || item["from"].is_number());
                if (ok) {
                    create->addProperty<float>(ComponentType::AlphaTweenComponent, "startAlpha", numberOr(item, "from", 1.0f));
                    create->addProperty<float>(ComponentType::AlphaTweenComponent, "endAlpha", item["to"].get<float>());
                }
            }
            if (!ok) { failWith(where + ": from / to don't fit a " + typeName + " tween (vectors [x,y,z], rotations [x,y,z,w] or {\"euler\": [...]}, colors [r,g,b], alpha numbers)."); return; }
            if (!step.add(guard.release())) { failWith(where + ": could not create the tween."); return; }
            ActionFrame frame;
            frame.startTime = std::max(0.0f, numberOr(item, "start_time", 0.0f));
            frame.duration = 0.0f;
            frame.action = create->getEntity();
            frame.track = nextLane++;
            kept.push_back(frame);
            tweensCreated++;
        }
    }

    // Existing actions (tweens, tracks, sprite animations, particles, nested animations)
    if (has(args, "add_actions")) {
        if (!args["add_actions"].is_array()) { failWith("add_actions must be an array of {action, start_time, duration, lane}."); return; }
        ActionSystem* actionSystem = scene->getSystem<ActionSystem>().get();
        for (const Json& item : args["add_actions"]) {
            Entity action = has(item, "action") ? resolveRef(target.scene, item["action"], error) : NULL_ENTITY;
            if (action == NULL_ENTITY) { failWith("add_actions: " + (error.empty() ? std::string("action is required.") : error)); return; }
            if (!scene->findComponent<ActionComponent>(action)) { failWith("add_actions: \"" + scene->getEntityName(action) + "\" is not an action (tween, track, sprite animation, particles or animation)."); return; }
            if (action == animation || (actionSystem && actionSystem->isAnimationReachable(action, animation))) { failWith("add_actions: that would make the animation contain itself."); return; }
            ActionFrame frame;
            frame.startTime = std::max(0.0f, numberOr(item, "start_time", 0.0f));
            frame.duration = numberOr(item, "duration", 0.0f);
            frame.action = action;
            frame.track = static_cast<uint32_t>(std::max(0, intOr(item, "lane", static_cast<int>(nextLane++))));
            kept.push_back(frame);
        }
    }

    // Animation settings
    const ComponentType animType = ComponentType::AnimationComponent;
    bool framesChanged = kept.size() != anim.actions.size();
    for (size_t i = 0; !framesChanged && i < kept.size(); i++) {
        const ActionFrame& a = kept[i];
        const ActionFrame& b = anim.actions[i];
        framesChanged = a.action != b.action || a.startTime != b.startTime || a.duration != b.duration || a.track != b.track;
    }
    if (framesChanged) addProp<std::vector<ActionFrame>>(*edits, target, animation, animType, "actions", kept);
    if (has(args, "loop")) addProp<bool>(*edits, target, animation, animType, "loop", boolOr(args, "loop", false));
    if (has(args, "duration")) addProp<float>(*edits, target, animation, animType, "duration", numberOr(args, "duration", -1.0f));
    if (has(args, "autoplay")) {
        addProp<ActionState>(*edits, target, animation, ComponentType::ActionComponent, "state",
                             boolOr(args, "autoplay", false) ? ActionState::Running : ActionState::Stopped);
    }
    if (has(args, "speed")) addProp<float>(*edits, target, animation, ComponentType::ActionComponent, "speed", numberOr(args, "speed", 1.0f));
    if (!step.add(editsGuard.release())) { failWith("The editor rejected the animation edit."); return; }
    if (!toDelete.empty()) {
        if (!step.add(new DeleteEntityCmd(target.project, target.sceneId(), toDelete))) { failWith("Could not delete the removed actions."); return; }
    }
    step.commit();

    Json data = animationJson(target, animation, static_cast<size_t>(std::max(1, intOr(args, "max_keys", 20))));
    data["created"] = created;
    call->ok(std::string(created ? "Created" : "Updated") + " animation \"" + scene->getEntityName(animation) + "\": " +
             std::to_string(kept.size()) + " action(s); " + std::to_string(tracksCreated) + " track(s) created, " +
             std::to_string(tracksWritten) + " rewritten, " + std::to_string(tweensCreated) + " tween(s) added (one undo step).", data);
}

void getAnimationTool(const CallPtr& call) {
    Target target;
    if (!resolveEntityArg(call, target)) return;
    Scene* scene = target.engineScene();
    const size_t keyLimit = static_cast<size_t>(std::max(1, intOr(call->args(), "max_keys", 50)));
    if (scene->findComponent<AnimationComponent>(target.entity)) {
        call->ok("Animation \"" + scene->getEntityName(target.entity) + "\".", animationJson(target, target.entity, keyLimit));
        return;
    }
    // The animations that animate this GameObject (or are its children)
    Json list = Json::array();
    for (Entity entity : target.scene->entities) {
        if (!scene->findComponent<AnimationComponent>(entity)) continue;
        ActionComponent* ac = scene->findComponent<ActionComponent>(entity);
        Transform* tf = scene->findComponent<Transform>(entity);
        bool related = (ac && ac->target == target.entity) || (tf && tf->parent == target.entity);
        if (ModelComponent* model = scene->findComponent<ModelComponent>(target.entity)) {
            related |= std::find(model->animations.begin(), model->animations.end(), entity) != model->animations.end();
        }
        if (related) list.push_back(animationJson(target, entity, keyLimit));
    }
    if (list.empty()) {
        call->fail("\"" + scene->getEntityName(target.entity) + "\" is not an Animation and no Animation targets it. Create one with edit_animation {target}.");
        return;
    }
    call->ok(std::to_string(list.size()) + " animation(s) for \"" + scene->getEntityName(target.entity) + "\".",
             Json{{"entity", entityJson(target, target.entity)}, {"animations", list}});
}

void previewAnimationTool(const CallPtr& call) {
    Target target;
    if (!resolveEntityArg(call, target, "animation")) return;
    Scene* scene = target.engineScene();
    AnimationWindow* window = call->ctx().animationWindow();
    if (!window) {
        call->fail("The Animation window is not available.");
        return;
    }
    if (!scene->findComponent<AnimationComponent>(target.entity) || !scene->findComponent<ActionComponent>(target.entity)) {
        call->fail("\"" + scene->getEntityName(target.entity) + "\" is not an Animation (it needs Animation and Action components).");
        return;
    }
    if (target.scene->playState != ScenePlayState::STOPPED) {
        call->fail("The Animation window previews in edit mode only; the scene is playing.");
        return;
    }
    const std::string action = lowerText(stringOr(call->args(), "action", "play"));
    // The preview runs on the selected scene's selected Animation, as when a human uses the window
    if (target.project->getSelectedSceneId() != target.sceneId()) target.project->setSelectedSceneId(target.sceneId());
    target.project->setSelectedEntity(target.sceneId(), target.entity);
    window->setOpen(true);
    window->selectEntity(target.entity, target.sceneId());
    if (action == "play") window->externalPlay(target.entity, target.sceneId());
    else if (action == "pause") window->externalPause();
    else if (action == "stop") window->externalStop();
    else if (action == "seek") {
        if (!has(call->args(), "time")) {
            call->fail("seek needs time (seconds).");
            return;
        }
        window->seekPreviewExternal(scene, target.scene, std::max(0.0f, numberOr(call->args(), "time", 0.0f)));
    } else {
        call->fail("action must be play, pause, stop or seek.");
        return;
    }
    auto report = [call, target, action]() {
        Scene* scene = target.engineScene();
        AnimationWindow* window = call->ctx().animationWindow();
        Json data = {{"animation", entityJson(target, target.entity)}, {"action", action},
                     {"previewing", window->isPreviewingEntity(target.entity, target.sceneId())}, {"playing", window->getIsPlaying()}};
        ActionComponent* ac = scene->findComponent<ActionComponent>(target.entity);
        if (ac && ac->target != NULL_ENTITY && scene->isEntityCreated(ac->target)) {
            data["target"] = entityJson(target, ac->target);
            if (Transform* tf = scene->findComponent<Transform>(ac->target)) {
                data["target_transform"] = {{"position", McpValues::toJson(tf->position)}, {"rotation", McpValues::toJson(tf->rotation)},
                                            {"scale", McpValues::toJson(tf->scale)}};
            }
        }
        call->ok("Animation preview: " + action + ".", data);
    };
    const int waitMs = std::clamp(intOr(call->args(), "wait_ms", 0), 0, 30000);
    if (waitMs > 0) {
        auto start = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        call->waitUntil([start, waitMs]() {
            return std::chrono::steady_clock::now() - *start >= std::chrono::milliseconds(waitMs);
        }, waitMs + 5000, [report](bool) { report(); });
        return;
    }
    call->afterFrames(1, report);
}

// ================================================================== UI

const std::vector<std::pair<const char*, AnchorPreset>>& anchorPresets() {
    static const std::vector<std::pair<const char*, AnchorPreset>> table = {
        {"none", AnchorPreset::NONE}, {"top_left", AnchorPreset::TOP_LEFT}, {"top_right", AnchorPreset::TOP_RIGHT},
        {"bottom_left", AnchorPreset::BOTTOM_LEFT}, {"bottom_right", AnchorPreset::BOTTOM_RIGHT},
        {"center_left", AnchorPreset::CENTER_LEFT}, {"left_center", AnchorPreset::CENTER_LEFT},
        {"center_top", AnchorPreset::CENTER_TOP}, {"top_center", AnchorPreset::CENTER_TOP},
        {"center_right", AnchorPreset::CENTER_RIGHT}, {"right_center", AnchorPreset::CENTER_RIGHT},
        {"center_bottom", AnchorPreset::CENTER_BOTTOM}, {"bottom_center", AnchorPreset::CENTER_BOTTOM},
        {"center", AnchorPreset::CENTER}, {"left_wide", AnchorPreset::LEFT_WIDE}, {"top_wide", AnchorPreset::TOP_WIDE},
        {"right_wide", AnchorPreset::RIGHT_WIDE}, {"bottom_wide", AnchorPreset::BOTTOM_WIDE},
        {"vertical_center_wide", AnchorPreset::VERTICAL_CENTER_WIDE}, {"horizontal_center_wide", AnchorPreset::HORIZONTAL_CENTER_WIDE},
        {"stretch", AnchorPreset::FULL_LAYOUT}, {"full", AnchorPreset::FULL_LAYOUT}};
    return table;
}

// UI layout is computed top-down (y down from the parent's top-left) whatever the camera, so a
// scene whose camera is not a UI camera (2D: bottom-up orthographic) shows it vertically mirrored
// (texts and images are still drawn upright). The tools speak top-down and map the values for such
// scenes, both ways. Layout groups still stack bottom-up there: no mapping fixes that.
bool uiMirrored(const Target& target) {
    Scene* scene = target.engineScene();
    // the camera the scene renders through (as Project::getSceneCamera, which is private)
    Entity camera = target.scene->mainCamera;
    if (camera == NULL_ENTITY || !scene->isEntityCreated(camera)) camera = target.scene->defaultCamera;
    if (camera == NULL_ENTITY) camera = scene->getCamera();
    CameraComponent* cc = camera != NULL_ENTITY ? scene->findComponent<CameraComponent>(camera) : nullptr;
    return cc && cc->type != CameraType::CAMERA_UI;
}

AnchorPreset mirrorPreset(AnchorPreset preset) {
    switch (preset) {
        case AnchorPreset::TOP_LEFT: return AnchorPreset::BOTTOM_LEFT;
        case AnchorPreset::BOTTOM_LEFT: return AnchorPreset::TOP_LEFT;
        case AnchorPreset::TOP_RIGHT: return AnchorPreset::BOTTOM_RIGHT;
        case AnchorPreset::BOTTOM_RIGHT: return AnchorPreset::TOP_RIGHT;
        case AnchorPreset::CENTER_TOP: return AnchorPreset::CENTER_BOTTOM;
        case AnchorPreset::CENTER_BOTTOM: return AnchorPreset::CENTER_TOP;
        case AnchorPreset::TOP_WIDE: return AnchorPreset::BOTTOM_WIDE;
        case AnchorPreset::BOTTOM_WIDE: return AnchorPreset::TOP_WIDE;
        default: return preset;
    }
}

// Height of the space an element's absolute position is in: its RectTransform parent, else the canvas
float layoutParentHeight(Scene* scene, Entity entity) {
    Transform* tf = scene->findComponent<Transform>(entity);
    for (Entity parent = tf ? tf->parent : NULL_ENTITY; parent != NULL_ENTITY;) {
        if (RectTransformComponent* rect = scene->findComponent<RectTransformComponent>(parent)) return static_cast<float>(rect->height);
        Transform* ptf = scene->findComponent<Transform>(parent);
        parent = ptf ? ptf->parent : NULL_ENTITY;
    }
    UISystem* ui = scene->getSystem<UISystem>().get();
    const int height = ui ? ui->getAnchorReferenceHeight() : 0;
    return static_cast<float>(height != 0 ? height : Engine::getCanvasHeight());
}

// RectTransform layout: absolute (position + size), an anchor preset (+ size, offset), or
// stretch with margins. Writes ordered so the UI system keeps the values (usingAnchors last).
bool addRectLayout(MultiPropertyCmd& multi, const Target& target, Entity entity, const Json& layout, std::string& error) {
    Scene* scene = target.engineScene();
    RectTransformComponent* rect = scene->findComponent<RectTransformComponent>(entity);
    if (!rect) {
        error = "\"" + scene->getEntityName(entity) + "\" has no RectTransform (not a UI element).";
        return false;
    }
    if (!layout.is_object()) {
        error = "layout must be an object {anchor, size, position, pivot, margins}.";
        return false;
    }
    const bool mirrored = uiMirrored(target);
    const ComponentType rt = ComponentType::RectTransformComponent;
    Vector2 size, position(0.0f, 0.0f), pivot;
    if (!readVec2(layout, "size", size, error) || !readVec2(layout, "position", position, error) || !readVec2(layout, "pivot", pivot, error)) return false;
    if (has(layout, "size") && (size.x < 0.0f || size.y < 0.0f)) {
        error = "size must be [width, height] >= 0.";
        return false;
    }
    if (mirrored) pivot.y = 1.0f - pivot.y;
    if (has(layout, "pivot")) addProp<Vector2>(multi, target, entity, rt, "pivot", pivot);

    const bool margins = has(layout, "margins");
    std::string anchorName = stringOr(layout, "anchor", margins ? "stretch" : "");
    // Text sizes itself to its content unless its size is fixed
    if (scene->findComponent<TextComponent>(entity) &&
        (has(layout, "size") || margins || lowerText(anchorName).find("wide") != std::string::npos ||
         lowerText(anchorName) == "stretch" || lowerText(anchorName) == "full")) {
        addProp<bool>(multi, target, entity, ComponentType::TextComponent, "fixedWidth", true);
        addProp<bool>(multi, target, entity, ComponentType::TextComponent, "fixedHeight", true);
    }
    if (anchorName.empty()) {
        // Absolute: Transform position is the top-left in the parent's layout space (y down);
        // mirrored, the bottom-left from the parent's bottom (y up)
        if (has(layout, "size")) {
            addProp<unsigned int>(multi, target, entity, rt, "width", static_cast<unsigned int>(std::lround(size.x)));
            addProp<unsigned int>(multi, target, entity, rt, "height", static_cast<unsigned int>(std::lround(size.y)));
        }
        Transform* tf = scene->findComponent<Transform>(entity);
        if (tf && (has(layout, "position") || (mirrored && has(layout, "size")))) {
            Vector3 pos = tf->position;
            if (mirrored) {
                // keep the top edge when only the size changes
                const float parentHeight = layoutParentHeight(scene, entity);
                const float oldHeight = static_cast<float>(rect->height);
                const float newHeight = has(layout, "size") ? static_cast<float>(std::lround(size.y)) : oldHeight;
                const float top = has(layout, "position") ? position.y : parentHeight - pos.y - oldHeight;
                if (has(layout, "position")) pos.x = position.x;
                pos.y = parentHeight - top - newHeight;
            } else {
                pos.x = position.x;
                pos.y = position.y;
            }
            addProp<Vector3>(multi, target, entity, ComponentType::Transform, "position", pos);
        }
        if (rect->usingAnchors && (has(layout, "size") || has(layout, "position"))) {
            addProp<bool>(multi, target, entity, rt, "usingAnchors", false);
        }
        return true;
    }
    AnchorPreset preset;
    if (!pickName(anchorPresets(), anchorName, preset, "anchor", error)) return false;
    if (mirrored) position.y = -position.y;
    if (margins) {
        if (preset != AnchorPreset::FULL_LAYOUT) {
            error = "margins go with anchor stretch (the element fills its parent minus the margins).";
            return false;
        }
        Vector4 m;
        if (layout["margins"].is_number()) {
            const float v = layout["margins"].get<float>();
            m = Vector4(v, v, v, v);
        } else if (!McpValues::parseVector4(layout["margins"], m)) {
            error = "margins is a number or [left, top, right, bottom].";
            return false;
        }
        if (mirrored) std::swap(m.y, m.w);
        addProp<AnchorPreset>(multi, target, entity, rt, "anchorPreset", AnchorPreset::NONE);
        addProp<float>(multi, target, entity, rt, "anchorPointLeft", 0.0f);
        addProp<float>(multi, target, entity, rt, "anchorPointTop", 0.0f);
        addProp<float>(multi, target, entity, rt, "anchorPointRight", 1.0f);
        addProp<float>(multi, target, entity, rt, "anchorPointBottom", 1.0f);
        addProp<int>(multi, target, entity, rt, "anchorOffsetLeft", static_cast<int>(std::lround(m.x)));
        addProp<int>(multi, target, entity, rt, "anchorOffsetTop", static_cast<int>(std::lround(m.y)));
        addProp<int>(multi, target, entity, rt, "anchorOffsetRight", -static_cast<int>(std::lround(m.z)));
        addProp<int>(multi, target, entity, rt, "anchorOffsetBottom", -static_cast<int>(std::lround(m.w)));
        addProp<Vector2>(multi, target, entity, rt, "positionOffset", position);
        addProp<bool>(multi, target, entity, rt, "usingAnchors", true);
        return true;
    }
    if (mirrored) preset = mirrorPreset(preset);
    if (has(layout, "size")) {
        addProp<unsigned int>(multi, target, entity, rt, "width", static_cast<unsigned int>(std::lround(size.x)));
        addProp<unsigned int>(multi, target, entity, rt, "height", static_cast<unsigned int>(std::lround(size.y)));
    }
    addProp<AnchorPreset>(multi, target, entity, rt, "anchorPreset", preset);
    addProp<Vector2>(multi, target, entity, rt, "positionOffset", position);
    addProp<bool>(multi, target, entity, rt, "usingAnchors", preset != AnchorPreset::NONE);
    return true;
}

// The layout as top-down values (create_ui's semantics), whatever the scene's camera
Json rectLayoutJson(const Target& target, Entity entity) {
    Scene* scene = target.engineScene();
    RectTransformComponent* rect = scene->findComponent<RectTransformComponent>(entity);
    if (!rect) return nullptr;
    const bool mirrored = uiMirrored(target);
    Vector2 pivot = rect->pivot;
    if (mirrored) pivot.y = 1.0f - pivot.y;
    Json out = {{"size", {rect->width, rect->height}}, {"pivot", McpValues::toJson(pivot)}, {"using_anchors", rect->usingAnchors}};
    if (rect->usingAnchors) {
        Vector2 offset = rect->positionOffset;
        if (mirrored) {
            offset.y = -offset.y;
            out["anchor"] = nameOf(anchorPresets(), mirrorPreset(rect->anchorPreset));
            out["anchor_points"] = {rect->anchorPointLeft, 1.0f - rect->anchorPointBottom, rect->anchorPointRight, 1.0f - rect->anchorPointTop};
            out["anchor_offsets"] = {rect->anchorOffsetLeft, -rect->anchorOffsetBottom, rect->anchorOffsetRight, -rect->anchorOffsetTop};
        } else {
            out["anchor"] = nameOf(anchorPresets(), rect->anchorPreset);
            out["anchor_points"] = {rect->anchorPointLeft, rect->anchorPointTop, rect->anchorPointRight, rect->anchorPointBottom};
            out["anchor_offsets"] = {rect->anchorOffsetLeft, rect->anchorOffsetTop, rect->anchorOffsetRight, rect->anchorOffsetBottom};
        }
        out["position_offset"] = McpValues::toJson(offset);
    } else if (Transform* tf = scene->findComponent<Transform>(entity)) {
        float y = tf->position.y;
        if (mirrored) y = layoutParentHeight(scene, entity) - y - static_cast<float>(rect->height);
        out["position"] = McpValues::toJson(Vector2(tf->position.x, y));
    }
    return out;
}

struct UiBuild {
    Target target;
    UndoStep* step = nullptr;
    int count = 0;
    bool layoutGroup = false;
};

const std::vector<std::pair<const char*, EntityCreationType>>& uiTypes() {
    static const std::vector<std::pair<const char*, EntityCreationType>> table = {
        {"panel", EntityCreationType::PANEL}, {"image", EntityCreationType::IMAGE}, {"text", EntityCreationType::TEXT},
        {"button", EntityCreationType::BUTTON}, {"input_field", EntityCreationType::INPUT_FIELD},
        {"progress_bar", EntityCreationType::PROGRESSBAR}, {"scrollbar", EntityCreationType::SCROLLBAR},
        {"layout_group", EntityCreationType::LAYOUT_GROUP}, {"container", EntityCreationType::GAMEOBJECT},
        {"polygon", EntityCreationType::POLYGON}};
    return table;
}

bool buildUiNode(UiBuild& build, const Json& node, Entity parent, const std::string& where, Json& out, std::string& error) {
    if (!node.is_object()) {
        error = where + " must be an object {type, name, layout, ...}.";
        return false;
    }
    Scene* scene = build.target.engineScene();
    const Target& target = build.target;
    EntityCreationType type;
    const std::string typeName = lowerText(stringOr(node, "type", ""));
    if (!pickName(uiTypes(), typeName, type, where + ".type", error)) return false;
    if (++build.count > 300) {
        error = "create_ui builds at most 300 elements per call.";
        return false;
    }
    std::string name = stringOr(node, "name", "");
    if (name.empty()) {
        name = typeName;
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    }
    auto* create = parent != NULL_ENTITY ? new CreateEntityCmd(target.project, target.sceneId(), name, type, parent)
                                         : new CreateEntityCmd(target.project, target.sceneId(), name, type);
    if (!build.step->add(create)) {
        error = where + ": could not create the GameObject.";
        return false;
    }
    const Entity entity = create->getEntity();
    if (type == EntityCreationType::LAYOUT_GROUP) build.layoutGroup = true;
    if (type == EntityCreationType::GAMEOBJECT &&
        !build.step->add(new AddComponentCmd(target.project, target.sceneId(), entity, ComponentType::RectTransformComponent))) {
        error = where + ": could not add the RectTransform.";
        return false;
    }

    // The entity holding the node's text: itself, a button label, a panel title, an input field's text
    Entity textEntity = NULL_ENTITY;
    if (type == EntityCreationType::TEXT) textEntity = entity;
    else if (ButtonComponent* button = scene->findComponent<ButtonComponent>(entity)) textEntity = button->label;
    else if (PanelComponent* panel = scene->findComponent<PanelComponent>(entity)) textEntity = panel->headertext;
    else if (InputFieldComponent* input = scene->findComponent<InputFieldComponent>(entity)) textEntity = input->text;

    auto* multi = new MultiPropertyCmd();
    std::unique_ptr<MultiPropertyCmd> guard(multi);
    const ComponentType tc = ComponentType::TextComponent;
    if (has(node, "text") || has(node, "title")) {
        if (textEntity == NULL_ENTITY) {
            error = where + ": a " + typeName + " has no text.";
            return false;
        }
        addProp<std::string>(*multi, target, textEntity, tc, "text", stringOr(node, has(node, "text") ? "text" : "title", ""));
    }
    if (has(node, "font_size") && textEntity != NULL_ENTITY) {
        addProp<unsigned int>(*multi, target, textEntity, tc, "fontSize", static_cast<unsigned int>(std::max(1, intOr(node, "font_size", 20))));
    }
    if (has(node, "font") && textEntity != NULL_ENTITY &&
        !addCatalogProperties(target.project, target.scene, textEntity, tc, Json{{"font", node["font"]}}, *multi, error)) {
        error = where + ".font: " + error;
        return false;
    }
    if (has(node, "multiline") && textEntity != NULL_ENTITY) addProp<bool>(*multi, target, textEntity, tc, "multiline", boolOr(node, "multiline", true));
    Vector4 color;
    if (has(node, "text_color")) {
        if (textEntity == NULL_ENTITY || !parseColor4(node["text_color"], color)) {
            error = where + ".text_color must be [r, g, b(, a)] on an element with text.";
            return false;
        }
        addProp<Vector4>(*multi, target, textEntity, ComponentType::UIComponent, "color", color);
    }
    if (has(node, "color")) {
        if (!parseColor4(node["color"], color)) {
            error = where + ".color must be [r, g, b] or [r, g, b, a] (linear 0-1).";
            return false;
        }
        if (scene->findComponent<UIComponent>(entity)) addProp<Vector4>(*multi, target, entity, ComponentType::UIComponent, "color", color);
        if (scene->findComponent<ButtonComponent>(entity)) addProp<Vector4>(*multi, target, entity, ComponentType::ButtonComponent, "colorNormal", color);
    }
    if (has(node, "colors")) {
        if (!scene->findComponent<ButtonComponent>(entity) || !node["colors"].is_object()) {
            error = where + ".colors {normal, hovered, pressed, disabled} is for buttons.";
            return false;
        }
        static const std::vector<std::pair<const char*, const char*>> states = {
            {"normal", "colorNormal"}, {"hovered", "colorHovered"}, {"pressed", "colorPressed"}, {"disabled", "colorDisabled"}};
        for (const auto& state : states) {
            if (!has(node["colors"], state.first)) continue;
            if (!parseColor4(node["colors"][state.first], color)) {
                error = where + ".colors." + state.first + " must be [r, g, b(, a)].";
                return false;
            }
            addProp<Vector4>(*multi, target, entity, ComponentType::ButtonComponent, state.second, color);
        }
    }
    if (has(node, "texture")) {
        if (!scene->findComponent<UIComponent>(entity) ||
            !addCatalogProperties(target.project, target.scene, entity, ComponentType::UIComponent, Json{{"texture", node["texture"]}}, *multi, error)) {
            error = where + ".texture: " + (error.empty() ? std::string("this element has no image.") : error);
            return false;
        }
    }
    if (has(node, "value")) {
        if (!scene->findComponent<ProgressbarComponent>(entity)) {
            error = where + ".value is for progress_bar (0-1).";
            return false;
        }
        addProp<float>(*multi, target, entity, ComponentType::ProgressbarComponent, "value", std::clamp(numberOr(node, "value", 0.5f), 0.0f, 1.0f));
    }
    if (has(node, "placeholder")) {
        if (!scene->findComponent<InputFieldComponent>(entity)) {
            error = where + ".placeholder is for input_field.";
            return false;
        }
        addProp<std::string>(*multi, target, entity, ComponentType::InputFieldComponent, "placeholder", stringOr(node, "placeholder", ""));
    }
    if (has(node, "direction")) {
        const std::string direction = lowerText(stringOr(node, "direction", ""));
        if (LayoutGroupComponent* group = scene->findComponent<LayoutGroupComponent>(entity)) {
            (void)group;
            static const std::vector<std::pair<const char*, LayoutGroupType>> table = {
                {"vertical", LayoutGroupType::VERTICAL}, {"horizontal", LayoutGroupType::HORIZONTAL},
                {"vertical_wrap", LayoutGroupType::VERTICAL_WRAP}, {"horizontal_wrap", LayoutGroupType::HORIZONTAL_WRAP}};
            LayoutGroupType value;
            if (!pickName(table, direction, value, where + ".direction", error)) return false;
            addProp<LayoutGroupType>(*multi, target, entity, ComponentType::LayoutGroupComponent, "type", value);
        } else if (scene->findComponent<ProgressbarComponent>(entity)) {
            static const std::vector<std::pair<const char*, ProgressbarType>> table = {
                {"vertical", ProgressbarType::VERTICAL}, {"horizontal", ProgressbarType::HORIZONTAL}};
            ProgressbarType value;
            if (!pickName(table, direction, value, where + ".direction", error)) return false;
            addProp<ProgressbarType>(*multi, target, entity, ComponentType::ProgressbarComponent, "type", value);
        } else if (scene->findComponent<ScrollbarComponent>(entity)) {
            static const std::vector<std::pair<const char*, ScrollbarType>> table = {
                {"vertical", ScrollbarType::VERTICAL}, {"horizontal", ScrollbarType::HORIZONTAL}};
            ScrollbarType value;
            if (!pickName(table, direction, value, where + ".direction", error)) return false;
            addProp<ScrollbarType>(*multi, target, entity, ComponentType::ScrollbarComponent, "type", value);
        } else {
            error = where + ".direction is for layout_group, progress_bar and scrollbar.";
            return false;
        }
    }
    if (has(node, "layout") && !addRectLayout(*multi, target, entity, node["layout"], error)) {
        error = where + ".layout: " + error;
        return false;
    }
    if (has(node, "properties") && !addComponentProperties(target.project, target.scene, entity, node["properties"], *multi, error)) {
        error = where + ".properties: " + error;
        return false;
    }
    if (!build.step->add(guard.release())) {
        error = where + ": the editor rejected the element's properties.";
        return false;
    }

    out = entityJson(target, entity);
    out["type"] = typeName;
    if (textEntity != NULL_ENTITY && textEntity != entity) out["text_entity"] = entityJson(target, textEntity);
    if (ProgressbarComponent* bar = scene->findComponent<ProgressbarComponent>(entity)) out["fill"] = entityJson(target, bar->fill);
    if (ScrollbarComponent* bar = scene->findComponent<ScrollbarComponent>(entity)) out["bar"] = entityJson(target, bar->bar);
    if (has(node, "children")) {
        if (!node["children"].is_array()) {
            error = where + ".children must be an array of nodes.";
            return false;
        }
        Json children = Json::array();
        for (size_t i = 0; i < node["children"].size(); i++) {
            Json child;
            if (!buildUiNode(build, node["children"][i], entity, where + ".children[" + std::to_string(i) + "]", child, error)) return false;
            children.push_back(child);
        }
        out["children"] = children;
    }
    return true;
}

void createUiTool(const CallPtr& call) {
    Target target;
    if (!resolveSceneArg(call, target) || !sceneStopped(call, target)) return;
    const Json& args = call->args();
    std::string error;
    Entity parent = NULL_ENTITY;
    if (has(args, "parent") && (parent = resolveRef(target.scene, args["parent"], error)) == NULL_ENTITY) {
        call->fail("parent: " + error);
        return;
    }
    if (!has(args, "root")) {
        call->fail("root is required: a node {type, name, layout, text, children: [...]}.");
        return;
    }
    UndoStep step(target.sceneId());
    UiBuild build;
    build.target = target;
    build.step = &step;
    Json tree;
    if (!buildUiNode(build, args["root"], parent, "root", tree, error)) {
        step.fail(call, error);
        return;
    }
    step.commit();
    std::string text = "Created " + std::to_string(build.count) + " UI element(s) under \"" + tree["name"].get<std::string>() + "\" (one undo step).";
    Json data = {{"root", tree}, {"count", build.count}};
    if (target.scene->sceneType != SceneType::SCENE_UI) {
        text += " Note: UI pointer events (button presses) reach a scene whose camera is a UI camera; a HUD usually lives in a UI scene (child scene).";
    }
    Scene* scene = target.engineScene();
    if (uiMirrored(target) && (build.layoutGroup || (parent != NULL_ENTITY && scene->findComponent<LayoutGroupComponent>(parent)))) {
        const std::string warning = "This scene's camera is not a UI camera: layout groups stack their children bottom-up here "
                                    "(anchors and positions are mapped, stacking is not). Build menus/HUDs in a UI scene (child scene).";
        text += " Warning: " + warning;
        data["warnings"] = Json::array({warning});
    }
    call->ok(text, data);
}

void setRectLayoutTool(const CallPtr& call) {
    Target target;
    if (!resolveSceneArg(call, target) || !sceneStopped(call, target)) return;
    const Json& args = call->args();
    if (!has(args, "items") || !args["items"].is_array() || args["items"].empty()) {
        call->fail("items must be a non-empty array of {entity, anchor, size, position, pivot, margins}.");
        return;
    }
    std::string error;
    UndoStep step(target.sceneId());
    auto* multi = new MultiPropertyCmd();
    std::unique_ptr<MultiPropertyCmd> guard(multi);
    std::vector<Entity> entities;
    for (size_t i = 0; i < args["items"].size(); i++) {
        const Json& item = args["items"][i];
        Entity entity = has(item, "entity") ? resolveRef(target.scene, item["entity"], error) : NULL_ENTITY;
        if (entity == NULL_ENTITY) {
            call->fail("items[" + std::to_string(i) + "].entity: " + (error.empty() ? std::string("required.") : error));
            return;
        }
        if (!addRectLayout(*multi, target, entity, item, error)) {
            call->fail("items[" + std::to_string(i) + "]: " + error);
            return;
        }
        entities.push_back(entity);
    }
    if (!step.add(guard.release())) {
        call->fail("The editor rejected the layout edit.");
        return;
    }
    step.commit();
    // The UI system resolves anchors on the next frame
    call->afterFrames(2, [call, target, entities]() {
        Json list = Json::array();
        for (Entity entity : entities) {
            Json item = entityJson(target, entity);
            item["layout"] = rectLayoutJson(target, entity);
            list.push_back(item);
        }
        call->ok("Laid out " + std::to_string(entities.size()) + " UI element(s) (one undo step).", Json{{"items", list}});
    });
}

// ================================================================== particles

struct ParticleSettings {
    unsigned int maxParticles = 100;
    int rate = 10;
    bool loop = true;
    Vector2 life = Vector2(1.0f, 2.0f);
    ParticleEmitterShape shape = ParticleEmitterShape::Sphere;
    Vector3 minPosition, maxPosition;
    float radius = 0.1f, innerRadius = 0.0f, coneAngle = 25.0f, coneHeight = 1.0f;
    Vector3 minVelocity, maxVelocity, minAcceleration, maxAcceleration;
    Vector2 size = Vector2(0.2f, 0.3f);
    Vector2 sizeOverLife = Vector2(-1.0f, -1.0f);   // from, to (x < 0: off)
    Vector2 alphaOverLife = Vector2(-1.0f, -1.0f);
    Vector3 minColor = Vector3(1, 1, 1), maxColor = Vector3(1, 1, 1);
    std::vector<ParticleColorGradientStop> gradient;
    std::vector<ParticleBurst> bursts;
};

bool particlePreset(const std::string& name, ParticleSettings& s) {
    if (name == "fire") {
        s.maxParticles = 200; s.rate = 60; s.life = Vector2(0.5f, 1.0f);
        s.shape = ParticleEmitterShape::Circle; s.radius = 0.25f;
        s.minVelocity = Vector3(-0.15f, 1.0f, -0.15f); s.maxVelocity = Vector3(0.15f, 2.0f, 0.15f);
        s.size = Vector2(0.35f, 0.5f); s.sizeOverLife = Vector2(0.45f, 0.05f); s.alphaOverLife = Vector2(1.0f, 0.0f);
        s.gradient = {{0.0f, Vector3(1.0f, 0.9f, 0.4f)}, {0.4f, Vector3(1.0f, 0.45f, 0.1f)}, {1.0f, Vector3(0.35f, 0.08f, 0.03f)}};
    } else if (name == "smoke") {
        s.maxParticles = 120; s.rate = 15; s.life = Vector2(2.5f, 4.0f);
        s.shape = ParticleEmitterShape::Circle; s.radius = 0.3f;
        s.minVelocity = Vector3(-0.2f, 0.5f, -0.2f); s.maxVelocity = Vector3(0.2f, 1.0f, 0.2f);
        s.minAcceleration = s.maxAcceleration = Vector3(0.05f, 0.1f, 0.0f);
        s.size = Vector2(0.5f, 0.8f); s.sizeOverLife = Vector2(0.6f, 2.0f); s.alphaOverLife = Vector2(0.6f, 0.0f);
        s.gradient = {{0.0f, Vector3(0.55f, 0.55f, 0.55f)}, {1.0f, Vector3(0.25f, 0.25f, 0.25f)}};
    } else if (name == "sparks") {
        s.maxParticles = 300; s.rate = 80; s.life = Vector2(0.3f, 0.8f);
        s.shape = ParticleEmitterShape::Sphere; s.radius = 0.05f;
        s.minVelocity = Vector3(-3.0f, 1.5f, -3.0f); s.maxVelocity = Vector3(3.0f, 5.0f, 3.0f);
        s.minAcceleration = s.maxAcceleration = Vector3(0.0f, -9.8f, 0.0f);
        s.size = Vector2(0.05f, 0.1f); s.alphaOverLife = Vector2(1.0f, 0.0f);
        s.gradient = {{0.0f, Vector3(1.0f, 1.0f, 0.8f)}, {0.5f, Vector3(1.0f, 0.75f, 0.2f)}, {1.0f, Vector3(1.0f, 0.3f, 0.05f)}};
    } else if (name == "explosion") {
        s.maxParticles = 250; s.rate = 0; s.loop = false; s.life = Vector2(0.5f, 1.2f);
        s.shape = ParticleEmitterShape::Sphere; s.radius = 0.2f;
        s.minVelocity = Vector3(-6.0f, -6.0f, -6.0f); s.maxVelocity = Vector3(6.0f, 6.0f, 6.0f);
        s.size = Vector2(0.4f, 0.8f); s.sizeOverLife = Vector2(0.6f, 1.6f); s.alphaOverLife = Vector2(1.0f, 0.0f);
        s.gradient = {{0.0f, Vector3(1.0f, 1.0f, 0.6f)}, {0.3f, Vector3(1.0f, 0.45f, 0.1f)}, {1.0f, Vector3(0.2f, 0.2f, 0.2f)}};
        s.bursts = {{0.0f, 160, 200}};
    } else if (name == "snow") {
        s.maxParticles = 400; s.rate = 40; s.life = Vector2(6.0f, 8.0f);
        s.shape = ParticleEmitterShape::Box; s.minPosition = Vector3(-10.0f, 8.0f, -10.0f); s.maxPosition = Vector3(10.0f, 8.0f, 10.0f);
        s.minVelocity = Vector3(-0.3f, -1.2f, -0.3f); s.maxVelocity = Vector3(0.3f, -0.8f, 0.3f);
        s.size = Vector2(0.05f, 0.12f);
    } else if (name == "rain") {
        s.maxParticles = 800; s.rate = 250; s.life = Vector2(1.0f, 1.2f);
        s.shape = ParticleEmitterShape::Box; s.minPosition = Vector3(-10.0f, 10.0f, -10.0f); s.maxPosition = Vector3(10.0f, 10.0f, 10.0f);
        s.minVelocity = Vector3(0.0f, -15.0f, 0.0f); s.maxVelocity = Vector3(0.0f, -12.0f, 0.0f);
        s.size = Vector2(0.03f, 0.04f); s.minColor = s.maxColor = Vector3(0.7f, 0.8f, 1.0f); s.alphaOverLife = Vector2(0.6f, 0.6f);
    } else if (name == "magic") {
        s.maxParticles = 150; s.rate = 30; s.life = Vector2(1.0f, 2.0f);
        s.shape = ParticleEmitterShape::Sphere; s.radius = 0.5f;
        s.minVelocity = Vector3(-0.3f, 0.2f, -0.3f); s.maxVelocity = Vector3(0.3f, 0.8f, 0.3f);
        s.size = Vector2(0.08f, 0.15f); s.sizeOverLife = Vector2(0.12f, 0.0f); s.alphaOverLife = Vector2(1.0f, 0.0f);
        s.gradient = {{0.0f, Vector3(0.4f, 0.9f, 1.0f)}, {1.0f, Vector3(0.7f, 0.3f, 1.0f)}};
    } else if (name != "none") {
        return false;
    }
    return true;
}

// Presets are in 3D world units; 2D scenes measure in pixels, so they scale up and lie flat
void scaleParticleSettings(ParticleSettings& s, float k, bool flat) {
    auto v = [&](Vector3& value) {
        value = value * k;
        if (flat) value.z = 0.0f;
    };
    v(s.minPosition); v(s.maxPosition); v(s.minVelocity); v(s.maxVelocity); v(s.minAcceleration); v(s.maxAcceleration);
    s.radius *= k; s.innerRadius *= k; s.coneHeight *= k;
    s.size = s.size * k;
    if (s.sizeOverLife.x >= 0.0f) s.sizeOverLife = s.sizeOverLife * k;
}

bool readMinMax3(const Json& value, Vector3& lo, Vector3& hi, std::string& error, const std::string& what) {
    if (value.is_object() && has(value, "min") && has(value, "max")) {
        if (McpValues::parseVector3(value["min"], lo) && McpValues::parseVector3(value["max"], hi)) return true;
    } else if (McpValues::parseVector3(value, lo)) {
        hi = lo;
        return true;
    }
    error = what + " is [x, y, z] or {min: [x, y, z], max: [x, y, z]}.";
    return false;
}

bool readRange(const Json& value, Vector2& out) {
    if (value.is_number()) {
        out = Vector2(value.get<float>(), value.get<float>());
        return true;
    }
    return McpValues::parseVector2(value, out);
}

bool applyParticleOverrides(const Json& args, ParticleSettings& s, std::string& error) {
    if (has(args, "max_particles")) s.maxParticles = static_cast<unsigned int>(std::clamp(intOr(args, "max_particles", 100), 1, 100000));
    if (has(args, "rate")) s.rate = std::max(0, intOr(args, "rate", 10));
    s.loop = boolOr(args, "loop", s.loop);
    if (has(args, "life") && !readRange(args["life"], s.life)) { error = "life is seconds: a number or [min, max]."; return false; }
    if (has(args, "size") && !readRange(args["size"], s.size)) { error = "size is a number or [min, max]."; return false; }
    if (has(args, "size_over_life") && !McpValues::parseVector2(args["size_over_life"], s.sizeOverLife)) { error = "size_over_life is [from, to]."; return false; }
    if (has(args, "alpha_over_life") && !McpValues::parseVector2(args["alpha_over_life"], s.alphaOverLife)) { error = "alpha_over_life is [from, to] (0-1)."; return false; }
    if (has(args, "velocity") && !readMinMax3(args["velocity"], s.minVelocity, s.maxVelocity, error, "velocity")) return false;
    if (has(args, "acceleration") && !readMinMax3(args["acceleration"], s.minAcceleration, s.maxAcceleration, error, "acceleration")) return false;
    if (has(args, "color")) {
        if (args["color"].is_object() && has(args["color"], "min")) {
            if (!parseColor3(args["color"]["min"], s.minColor) || !parseColor3(args["color"]["max"], s.maxColor)) { error = "color is [r, g, b] or {min, max}."; return false; }
        } else if (parseColor3(args["color"], s.minColor)) {
            s.maxColor = s.minColor;
        } else {
            error = "color is [r, g, b] or {min, max}.";
            return false;
        }
        if (!has(args, "gradient")) s.gradient.clear();
    }
    if (has(args, "gradient")) {
        s.gradient.clear();
        if (!args["gradient"].is_array()) { error = "gradient is [[time 0-1, [r, g, b]], ...]."; return false; }
        for (const Json& stop : args["gradient"]) {
            ParticleColorGradientStop item;
            if (!stop.is_array() || stop.size() != 2 || !stop[0].is_number() || !parseColor3(stop[1], item.color)) {
                error = "gradient is [[time 0-1, [r, g, b]], ...].";
                return false;
            }
            item.time = std::clamp(stop[0].get<float>(), 0.0f, 1.0f);
            s.gradient.push_back(item);
        }
    }
    if (has(args, "bursts")) {
        s.bursts.clear();
        if (!args["bursts"].is_array()) { error = "bursts is [{time, count}] (count a number or [min, max])."; return false; }
        for (const Json& b : args["bursts"]) {
            ParticleBurst burst;
            burst.time = numberOr(b, "time", 0.0f);
            Vector2 count;
            if (!has(b, "count") || !readRange(b["count"], count)) { error = "bursts is [{time, count}] (count a number or [min, max])."; return false; }
            burst.minCount = static_cast<int>(count.x);
            burst.maxCount = static_cast<int>(count.y);
            s.bursts.push_back(burst);
        }
    }
    if (has(args, "emitter")) {
        const Json& e = args["emitter"];
        static const std::vector<std::pair<const char*, ParticleEmitterShape>> shapes = {
            {"box", ParticleEmitterShape::Box}, {"sphere", ParticleEmitterShape::Sphere}, {"hemisphere", ParticleEmitterShape::Hemisphere},
            {"circle", ParticleEmitterShape::Circle}, {"cone", ParticleEmitterShape::Cone}};
        if (has(e, "shape") && !pickName(shapes, stringOr(e, "shape", ""), s.shape, "emitter.shape", error)) return false;
        if ((has(e, "min") || has(e, "max")) && (!readVec3(e, "min", s.minPosition, error) || !readVec3(e, "max", s.maxPosition, error))) return false;
        s.radius = numberOr(e, "radius", s.radius);
        s.innerRadius = numberOr(e, "inner_radius", s.innerRadius);
        s.coneAngle = numberOr(e, "cone_angle", s.coneAngle);
        s.coneHeight = numberOr(e, "cone_height", s.coneHeight);
    }
    return true;
}

void addParticleWrites(MultiPropertyCmd& m, const Target& target, Entity e, const ParticleSettings& s) {
    const ComponentType p = ComponentType::ParticleSystemComponent;
    addProp<unsigned int>(m, target, e, p, "maxParticles", s.maxParticles);
    addProp<int>(m, target, e, p, "rate", s.rate);
    addProp<bool>(m, target, e, p, "loop", s.loop);
    addProp<std::vector<ParticleBurst>>(m, target, e, p, "bursts", s.bursts);
    addProp<float>(m, target, e, p, "lifeInitializer.minLife", s.life.x);
    addProp<float>(m, target, e, p, "lifeInitializer.maxLife", s.life.y);
    addProp<ParticleEmitterShape>(m, target, e, p, "positionInitializer.shape", s.shape);
    addProp<Vector3>(m, target, e, p, "positionInitializer.minPosition", s.minPosition);
    addProp<Vector3>(m, target, e, p, "positionInitializer.maxPosition", s.maxPosition);
    addProp<float>(m, target, e, p, "positionInitializer.radius", s.radius);
    addProp<float>(m, target, e, p, "positionInitializer.innerRadius", s.innerRadius);
    addProp<float>(m, target, e, p, "positionInitializer.coneAngle", s.coneAngle);
    addProp<float>(m, target, e, p, "positionInitializer.coneHeight", s.coneHeight);
    addProp<Vector3>(m, target, e, p, "velocityInitializer.minVelocity", s.minVelocity);
    addProp<Vector3>(m, target, e, p, "velocityInitializer.maxVelocity", s.maxVelocity);
    addProp<Vector3>(m, target, e, p, "accelerationInitializer.minAcceleration", s.minAcceleration);
    addProp<Vector3>(m, target, e, p, "accelerationInitializer.maxAcceleration", s.maxAcceleration);
    addProp<float>(m, target, e, p, "sizeInitializer.minSize", s.size.x);
    addProp<float>(m, target, e, p, "sizeInitializer.maxSize", s.size.y);
    addProp<Vector3>(m, target, e, p, "colorInitializer.minColor", s.minColor);
    addProp<Vector3>(m, target, e, p, "colorInitializer.maxColor", s.maxColor);
    ParticleColorGradient gradient;
    gradient.stops = s.gradient;
    gradient.normalize();
    addProp<ParticleColorGradient>(m, target, e, p, "colorGradient", gradient);
    // Over-life modifiers run from spawn to the longest life
    const bool sizeOn = s.sizeOverLife.x >= 0.0f, alphaOn = s.alphaOverLife.x >= 0.0f;
    addProp<float>(m, target, e, p, "sizeModifier.fromTime", 0.0f);
    addProp<float>(m, target, e, p, "sizeModifier.toTime", sizeOn ? s.life.y : 0.0f);
    addProp<float>(m, target, e, p, "sizeModifier.fromSize", sizeOn ? s.sizeOverLife.x : 0.0f);
    addProp<float>(m, target, e, p, "sizeModifier.toSize", sizeOn ? s.sizeOverLife.y : 0.0f);
    addProp<float>(m, target, e, p, "alphaModifier.fromTime", 0.0f);
    addProp<float>(m, target, e, p, "alphaModifier.toTime", alphaOn ? s.life.y : 0.0f);
    addProp<float>(m, target, e, p, "alphaModifier.fromAlpha", alphaOn ? s.alphaOverLife.x : 0.0f);
    addProp<float>(m, target, e, p, "alphaModifier.toAlpha", alphaOn ? s.alphaOverLife.y : 0.0f);
}

void createParticlesTool(const CallPtr& call) {
    Target target;
    if (!resolveSceneArg(call, target) || !sceneStopped(call, target)) return;
    const Json& args = call->args();
    Scene* scene = target.engineScene();
    std::string error;
    UndoStep step(target.sceneId());
    auto failWith = [&](const std::string& message) {
        step.fail(call, message);
    };

    Entity system = NULL_ENTITY;
    Entity emitter = NULL_ENTITY;
    bool created = false;
    if (has(args, "entity")) {
        system = resolveRef(target.scene, args["entity"], error);
        if (system == NULL_ENTITY || !scene->findComponent<ParticleSystemComponent>(system)) {
            call->fail(system == NULL_ENTITY ? error : "\"" + scene->getEntityName(system) + "\" has no Particle System component.");
            return;
        }
        emitter = scene->getComponent<ActionComponent>(system).target;
    } else {
        // The emitter: an existing GameObject with Points or a Mesh, else a new Points GameObject
        if (has(args, "emitter_entity")) {
            emitter = resolveRef(target.scene, args["emitter_entity"], error);
            if (emitter == NULL_ENTITY) { failWith("emitter_entity: " + error); return; }
            if (!scene->findComponent<PointsComponent>(emitter) && !scene->findComponent<MeshComponent>(emitter)) {
                failWith("emitter_entity needs a Points or Mesh component (particles are drawn as its points or mesh instances).");
                return;
            }
        } else {
            Entity parent = NULL_ENTITY;
            if (has(args, "parent") && (parent = resolveRef(target.scene, args["parent"], error)) == NULL_ENTITY) { failWith("parent: " + error); return; }
            auto* points = parent != NULL_ENTITY
                ? new CreateEntityCmd(target.project, target.sceneId(), stringOr(args, "name", "Particles"), EntityCreationType::POINTS, parent)
                : new CreateEntityCmd(target.project, target.sceneId(), stringOr(args, "name", "Particles"), EntityCreationType::POINTS);
            Vector3 position;
            if (!readVec3(args, "position", position, error)) { delete points; failWith(error); return; }
            if (has(args, "position")) points->addProperty<Vector3>(ComponentType::Transform, "position", position);
            if (!step.add(points)) { failWith("Could not create the Points GameObject."); return; }
            emitter = points->getEntity();
        }
        auto* create = new CreateEntityCmd(target.project, target.sceneId(), "Particle System", EntityCreationType::PARTICLE_SYSTEM, emitter);
        if (!step.add(create)) { failWith("Could not create the Particle System."); return; }
        system = create->getEntity();
        created = true;
    }

    ParticleSettings settings;
    const std::string preset = lowerText(stringOr(args, "preset", created ? "fire" : ""));
    auto* multi = new MultiPropertyCmd();
    std::unique_ptr<MultiPropertyCmd> guard(multi);
    if (!preset.empty() || has(args, "max_particles") || has(args, "rate") || has(args, "life") || has(args, "size") ||
        has(args, "velocity") || has(args, "acceleration") || has(args, "color") || has(args, "gradient") || has(args, "bursts") ||
        has(args, "emitter") || has(args, "loop") || has(args, "size_over_life") || has(args, "alpha_over_life")) {
        if (preset.empty()) {
            // Start from the system's current values
            ParticleSystemComponent& c = scene->getComponent<ParticleSystemComponent>(system);
            settings.maxParticles = c.maxParticles; settings.rate = c.rate; settings.loop = c.loop;
            settings.life = Vector2(c.lifeInitializer.minLife, c.lifeInitializer.maxLife);
            settings.shape = c.positionInitializer.shape; settings.minPosition = c.positionInitializer.minPosition;
            settings.maxPosition = c.positionInitializer.maxPosition; settings.radius = c.positionInitializer.radius;
            settings.innerRadius = c.positionInitializer.innerRadius; settings.coneAngle = c.positionInitializer.coneAngle;
            settings.coneHeight = c.positionInitializer.coneHeight;
            settings.minVelocity = c.velocityInitializer.minVelocity; settings.maxVelocity = c.velocityInitializer.maxVelocity;
            settings.minAcceleration = c.accelerationInitializer.minAcceleration; settings.maxAcceleration = c.accelerationInitializer.maxAcceleration;
            settings.size = Vector2(c.sizeInitializer.minSize, c.sizeInitializer.maxSize);
            settings.minColor = c.colorInitializer.minColor; settings.maxColor = c.colorInitializer.maxColor;
            settings.gradient = c.colorGradient.stops; settings.bursts = c.bursts;
            if (c.sizeModifier.toTime > c.sizeModifier.fromTime) settings.sizeOverLife = Vector2(c.sizeModifier.fromSize, c.sizeModifier.toSize);
            if (c.alphaModifier.toTime > c.alphaModifier.fromTime) settings.alphaOverLife = Vector2(c.alphaModifier.fromAlpha, c.alphaModifier.toAlpha);
        } else if (!particlePreset(preset, settings)) {
            failWith("preset must be fire, smoke, sparks, explosion, snow, rain, magic or none.");
            return;
        } else {
            const bool is2D = target.scene->sceneType != SceneType::SCENE_3D;
            scaleParticleSettings(settings, numberOr(args, "scale", is2D ? 40.0f : 1.0f), is2D);
            if (is2D) {
                // y is up in 2D too; flat emitters keep z at 0
                settings.minPosition.z = settings.maxPosition.z = 0.0f;
            }
        }
        if (!applyParticleOverrides(args, settings, error)) { failWith(error); return; }
        addParticleWrites(*multi, target, system, settings);
    }
    if (has(args, "texture") && emitter != NULL_ENTITY && scene->findComponent<PointsComponent>(emitter) &&
        !addCatalogProperties(target.project, target.scene, emitter, ComponentType::PointsComponent, Json{{"texture", args["texture"]}}, *multi, error)) {
        failWith("texture: " + error);
        return;
    }
    if (has(args, "properties") && !addCatalogProperties(target.project, target.scene, system, ComponentType::ParticleSystemComponent, args["properties"], *multi, error)) {
        failWith("properties: " + error);
        return;
    }
    if (has(args, "autoplay") || created) {
        addProp<ActionState>(*multi, target, system, ComponentType::ActionComponent, "state",
                             boolOr(args, "autoplay", true) ? ActionState::Running : ActionState::Stopped);
    }
    if (!step.add(guard.release())) { failWith("The editor rejected the particle settings."); return; }
    step.commit();

    ParticleSystemComponent& c = scene->getComponent<ParticleSystemComponent>(system);
    call->ok(std::string(created ? "Created" : "Updated") + " particle system \"" + scene->getEntityName(system) + "\"" +
             (preset.empty() ? std::string() : " (" + preset + ")") + ". It emits when the scene plays (autoplay) or when a script starts it.",
             Json{{"particle_system", entityJson(target, system)},
                  {"emitter", emitter != NULL_ENTITY ? entityJson(target, emitter) : Json(nullptr)}, {"created", created},
                  {"max_particles", c.maxParticles}, {"rate", c.rate}, {"loop", c.loop},
                  {"life", {c.lifeInitializer.minLife, c.lifeInitializer.maxLife}}, {"bursts", c.bursts.size()},
                  {"gradient_stops", c.colorGradient.stops.size()},
                  {"autoplay", scene->getComponent<ActionComponent>(system).state == ActionState::Running}});
}

// ================================================================== models

Json modelInfoJson(const Target& target, Entity entity) {
    Scene* scene = target.engineScene();
    ModelComponent& model = scene->getComponent<ModelComponent>(entity);
    Json meshes = Json::array();
    for (const auto& item : model.meshNodesMapping) {
        if (item.second != NULL_ENTITY && scene->isEntityCreated(item.second)) {
            Json m = entityJson(target, item.second);
            m["node"] = item.first;
            if (MeshComponent* mesh = scene->findComponent<MeshComponent>(item.second)) m["submeshes"] = mesh->numSubmeshes;
            meshes.push_back(m);
        }
    }
    if (meshes.empty()) {
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
            if (mesh->numSubmeshes > 0) {
                Json m = entityJson(target, entity);
                m["submeshes"] = mesh->numSubmeshes;
                meshes.push_back(m);
            }
        }
    }
    Json bones = Json::array();
    for (const auto& item : model.bonesNameMapping) {
        if (item.second != NULL_ENTITY && scene->isEntityCreated(item.second)) bones.push_back({{"name", item.first}, {"id", item.second}});
    }
    Json animations = Json::array();
    ActionSystem* actions = scene->getSystem<ActionSystem>().get();
    for (Entity anim : model.animations) {
        if (anim == NULL_ENTITY || !scene->isEntityCreated(anim)) continue;
        Json a = entityJson(target, anim);
        if (AnimationComponent* ac = scene->findComponent<AnimationComponent>(anim)) {
            float length = 0.0f;
            for (const ActionFrame& frame : ac->actions) length = std::max(length, frame.startTime + (actions ? actions->getFrameDuration(frame) : frame.duration));
            a["tracks"] = ac->actions.size();
            a["length"] = length;
            a["loop"] = ac->loop;
        }
        animations.push_back(a);
    }
    Json morphs = Json::array();
    for (const auto& item : model.morphNameMapping) morphs.push_back(item.first);
    return Json{{"model", entityJson(target, entity)}, {"file", McpValues::assetPathToJson(target.project, model.filename)},
                {"loading", ModelLoadCmd::isLoadPending(target.sceneId(), entity)}, {"meshes", meshes},
                {"bones", bones}, {"bone_count", model.bonesNameMapping.size()}, {"animations", animations}, {"morph_targets", morphs},
                {"skeleton", model.skeleton != NULL_ENTITY && scene->isEntityCreated(model.skeleton) ? entityJson(target, model.skeleton) : Json(nullptr)}};
}

void getModelInfoTool(const CallPtr& call) {
    Target target;
    if (!resolveEntityArg(call, target)) return;
    Scene* scene = target.engineScene();
    if (!scene->findComponent<ModelComponent>(target.entity)) {
        call->fail("\"" + scene->getEntityName(target.entity) + "\" has no Model component.");
        return;
    }
    Json info = modelInfoJson(target, target.entity);
    if (!boolOr(call->args(), "include_bones", true)) info.erase("bones");
    call->ok("Model \"" + scene->getEntityName(target.entity) + "\": " + std::to_string(info["meshes"].size()) + " mesh(es), " +
             std::to_string(info["bone_count"].get<size_t>()) + " bone(s), " + std::to_string(info["animations"].size()) + " animation(s).", info);
}

void setModelTool(const CallPtr& call) {
    Target target;
    if (!resolveSceneArg(call, target) || !sceneStopped(call, target)) return;
    const Json& args = call->args();
    Scene* scene = target.engineScene();
    std::string error;
    const bool clear = boolOr(args, "clear", false);
    if (!clear && !has(args, "path")) {
        call->fail("Pass path (a .gltf / .glb / .obj inside the assets directory) to load, or clear: true to clear the model.");
        return;
    }
    std::string stored;
    if (!clear) {
        std::filesystem::path absolute;
        if (!McpRefs::resolveProjectPath(target.project, stringOr(args, "path", ""), absolute, error, true) ||
            !McpValues::assetPathFromJson(target.project, stringOr(args, "path", ""), stored, error)) {
            call->fail("path: " + error);
            return;
        }
    }
    UndoStep step(target.sceneId());
    bool created = false;
    if (!entityOrCreate(call, target, step, EntityCreationType::MODEL, "Model", created)) {
        step.rollback();
        return;
    }
    if (!scene->findComponent<ModelComponent>(target.entity)) {
        step.fail(call, "\"" + scene->getEntityName(target.entity) + "\" has no Model component.");
        return;
    }
    if (clear) {
        std::string reason;
        if (!ModelClearCmd::canClear(target.project, target.sceneId(), target.entity, &reason)) {
            step.fail(call, "Can't clear the model: " + reason);
            return;
        }
        if (!step.add(new ModelClearCmd(target.project, target.sceneId(), target.entity))) {
            step.fail(call, "The editor could not clear the model.");
            return;
        }
        step.commit();
        call->ok("Cleared the model of \"" + scene->getEntityName(target.entity) + "\" (one undo step).", Json{{"model", entityJson(target, target.entity)}});
        return;
    }
    auto* load = has(args, "merge_static_meshes")
        ? new ModelLoadCmd(target.project, target.sceneId(), target.entity, stored, boolOr(args, "merge_static_meshes", false))
        : new ModelLoadCmd(target.project, target.sceneId(), target.entity, stored);
    if (!step.add(load)) {
        step.fail(call, "The model failed to load: check that \"" + stringOr(args, "path", "") + "\" is a valid glTF/GLB/OBJ file (the console has details).");
        return;
    }
    step.commit();
    const uint32_t sceneId = target.sceneId();
    const Entity entity = target.entity;
    const int timeoutMs = std::clamp(intOr(args, "timeout_seconds", 60), 1, 600) * 1000;
    call->waitUntil([sceneId, entity]() { return !ModelLoadCmd::isLoadPending(sceneId, entity); }, timeoutMs,
                    [call, target, created](bool timedOut) {
        Scene* scene = target.engineScene();
        if (!scene || !scene->isEntityCreated(target.entity) || !scene->findComponent<ModelComponent>(target.entity)) {
            call->fail("The model GameObject is gone (undone while loading?).");
            return;
        }
        Json info = modelInfoJson(target, target.entity);
        info["created"] = created;
        info.erase("bones");
        if (timedOut) {
            call->ok("The model is still loading; call get_model_info later.", info);
            return;
        }
        call->ok("Loaded \"" + info["file"].get<std::string>() + "\" into \"" + scene->getEntityName(target.entity) + "\": " +
                 std::to_string(info["meshes"].size()) + " mesh(es), " + std::to_string(info["bone_count"].get<size_t>()) + " bone(s), " +
                 std::to_string(info["animations"].size()) + " animation(s).", info);
    });
}

// ================================================================== shape points

// A convex outline (in order around the shape) as the triangle strip polygons draw
template<typename T>
std::vector<T> outlineToStrip(const std::vector<T>& outline) {
    std::vector<T> strip;
    if (outline.size() < 3) return outline;
    size_t lo = 0, hi = outline.size() - 1;
    strip.push_back(outline[lo++]);
    bool takeLow = true;
    while (lo <= hi) {
        strip.push_back(takeLow ? outline[lo++] : outline[hi--]);
        takeLow = !takeLow;
    }
    return strip;
}

void setShapePointsTool(const CallPtr& call) {
    Target target;
    if (!resolveEntityArg(call, target) || !sceneStopped(call, target)) return;
    const Json& args = call->args();
    Scene* scene = target.engineScene();
    std::string error;

    std::string component = lowerText(stringOr(args, "component", ""));
    if (component.empty()) {
        if (scene->findComponent<PolygonComponent>(target.entity)) component = "polygon";
        else if (scene->findComponent<MeshPolygonComponent>(target.entity)) component = "mesh_polygon";
        else if (scene->findComponent<ShadowCaster2DComponent>(target.entity)) component = "shadow_caster_2d";
        else if (scene->findComponent<LineRendererComponent>(target.entity)) component = "line_renderer";
        else {
            call->fail("\"" + scene->getEntityName(target.entity) + "\" has no Polygon, Polygon Mesh, Shadow Caster 2D or Line Renderer.");
            return;
        }
    }

    // Points: explicit, or generated (circle / rect)
    std::vector<Vector3> points;
    if (has(args, "points")) {
        if (!args["points"].is_array()) {
            call->fail("points must be an array of [x, y] or [x, y, z].");
            return;
        }
        for (const Json& p : args["points"]) {
            Vector3 v;
            Vector2 v2;
            if (p.is_array() && p.size() == 2 && McpValues::parseVector2(p, v2)) v = Vector3(v2.x, v2.y, 0.0f);
            else if (!McpValues::parseVector3(p, v)) {
                call->fail("points must be an array of [x, y] or [x, y, z].");
                return;
            }
            points.push_back(v);
        }
    } else if (has(args, "circle")) {
        const Json& c = args["circle"];
        Vector2 center(0.0f, 0.0f);
        if (!readVec2(c, "center", center, error)) { call->fail("circle." + error); return; }
        const float radius = numberOr(c, "radius", 50.0f);
        const int segments = std::clamp(intOr(c, "segments", 24), 3, 256);
        for (int i = 0; i < segments; i++) {
            const float a = 6.28318530718f * static_cast<float>(i) / static_cast<float>(segments);
            points.push_back(Vector3(center.x + std::cos(a) * radius, center.y + std::sin(a) * radius, 0.0f));
        }
    } else if (has(args, "rect")) {
        Vector4 r;
        if (!McpValues::parseVector4(args["rect"], r)) { call->fail("rect is [x, y, width, height]."); return; }
        points = {Vector3(r.x, r.y, 0), Vector3(r.x + r.z, r.y, 0), Vector3(r.x + r.z, r.y + r.w, 0), Vector3(r.x, r.y + r.w, 0)};
    } else if (component != "line_renderer" || !has(args, "lines")) {
        call->fail("Pass points [[x, y], ...], circle {center, radius, segments} or rect [x, y, w, h] (line_renderer also takes lines).");
        return;
    }

    std::vector<Vector4> colors;
    Vector4 color(1, 1, 1, 1);
    if (has(args, "color") && !parseColor4(args["color"], color)) { call->fail("color is [r, g, b(, a)]."); return; }
    if (has(args, "colors")) {
        if (!args["colors"].is_array() || args["colors"].size() != points.size()) { call->fail("colors needs one [r, g, b(, a)] per point."); return; }
        for (const Json& c : args["colors"]) {
            Vector4 v;
            if (!parseColor4(c, v)) { call->fail("colors needs one [r, g, b(, a)] per point."); return; }
            colors.push_back(v);
        }
    }
    auto colorAt = [&](size_t i) { return i < colors.size() ? colors[i] : color; };

    auto* multi = new MultiPropertyCmd();
    std::unique_ptr<MultiPropertyCmd> guard(multi);
    size_t written = 0;
    if (component == "polygon" || component == "mesh_polygon") {
        const ComponentType type = component == "polygon" ? ComponentType::PolygonComponent : ComponentType::MeshPolygonComponent;
        if (!(component == "polygon" ? (void*)scene->findComponent<PolygonComponent>(target.entity) : (void*)scene->findComponent<MeshPolygonComponent>(target.entity))) {
            call->fail("The GameObject has no " + component + " component.");
            return;
        }
        std::vector<PolygonPoint> list;
        for (size_t i = 0; i < points.size(); i++) list.push_back({points[i], colorAt(i)});
        if (lowerText(stringOr(args, "order", "outline")) == "outline") list = outlineToStrip(list);
        if (list.size() < 3) { call->fail("A polygon needs at least 3 points."); return; }
        addProp<std::vector<PolygonPoint>>(*multi, target, target.entity, type, "points", list);
        written = list.size();
    } else if (component == "shadow_caster_2d") {
        if (!scene->findComponent<ShadowCaster2DComponent>(target.entity)) { call->fail("The GameObject has no Shadow Caster 2D."); return; }
        std::vector<Vector2> list;
        for (const Vector3& p : points) list.push_back(Vector2(p.x, p.y));
        if (list.size() < 2) { call->fail("A shadow caster outline needs at least 2 points."); return; }
        addProp<ShadowCaster2DShape>(*multi, target, target.entity, ComponentType::ShadowCaster2DComponent, "shape", ShadowCaster2DShape::POLYGON);
        addProp<std::vector<Vector2>>(*multi, target, target.entity, ComponentType::ShadowCaster2DComponent, "points", list);
        if (has(args, "closed")) addProp<bool>(*multi, target, target.entity, ComponentType::ShadowCaster2DComponent, "closed", boolOr(args, "closed", true));
        written = list.size();
    } else if (component == "line_renderer") {
        if (!scene->findComponent<LineRendererComponent>(target.entity)) { call->fail("The GameObject has no Line Renderer."); return; }
        std::vector<LineData> lines;
        if (has(args, "lines")) {
            if (!args["lines"].is_array()) { call->fail("lines is [{a: [x,y,z], b: [x,y,z], color_a, color_b}]."); return; }
            for (const Json& l : args["lines"]) {
                LineData line;
                line.colorA = line.colorB = color;
                if (!has(l, "a") || !has(l, "b") || !McpValues::parseVector3(l["a"], line.pointA) || !McpValues::parseVector3(l["b"], line.pointB) ||
                    (has(l, "color_a") && !parseColor4(l["color_a"], line.colorA)) || (has(l, "color_b") && !parseColor4(l["color_b"], line.colorB))) {
                    call->fail("lines is [{a: [x,y,z], b: [x,y,z], color_a, color_b}].");
                    return;
                }
                lines.push_back(line);
            }
        } else {
            // A polyline through the points; closed joins the last to the first
            for (size_t i = 0; i + 1 < points.size(); i++) lines.push_back({points[i], colorAt(i), points[i + 1], colorAt(i + 1)});
            if (boolOr(args, "closed", false) && points.size() > 2) lines.push_back({points.back(), colorAt(points.size() - 1), points.front(), colorAt(0)});
        }
        LineRendererComponent& renderer = scene->getComponent<LineRendererComponent>(target.entity);
        if (lines.size() > renderer.maxLines) {
            addProp<unsigned int>(*multi, target, target.entity, ComponentType::LineRendererComponent, "maxLines", static_cast<unsigned int>(lines.size()));
        }
        addProp<std::vector<LineData>>(*multi, target, target.entity, ComponentType::LineRendererComponent, "lines", lines);
        written = lines.size();
    } else {
        call->fail("component must be polygon, mesh_polygon, shadow_caster_2d or line_renderer.");
        return;
    }
    UndoStep step(target.sceneId());
    if (!step.add(guard.release())) {
        call->fail("The editor rejected the points.");
        return;
    }
    step.commit();
    call->ok("Set " + std::to_string(written) + (component == "line_renderer" ? " line(s)" : " point(s)") + " on the " + component +
             " of \"" + scene->getEntityName(target.entity) + "\".",
             Json{{"entity", entityJson(target, target.entity)}, {"component", component}, {"count", written}});
}

} // namespace

void registerWorldTools(McpRegistry& registry) {
    using namespace schema;

    auto add = [&registry](const char* name, const char* title, std::string description, Json input,
                           std::function<void(CallPtr)> handler, bool readOnly, bool whilePlaying, bool idempotent = false) {
        ToolDef tool;
        tool.name = name;
        tool.title = title;
        tool.description = std::move(description);
        tool.inputSchema = std::move(input);
        tool.annotations.readOnly = readOnly;
        tool.annotations.idempotent = readOnly || idempotent;
        tool.allowedWhilePlaying = whilePlaying;
        tool.handler = std::move(handler);
        registry.addTool(std::move(tool));
    };
    const Json createSpec = object({
        {"name", string("")},
        {"parent", entityRef("Default: scene root.")},
        {"position", vec3("Local.")}
    });
    const Json numberItem = Json{{"type", "number"}};
    const Json arrayItem = Json{{"type", "array"}};
    const Json integerItem = Json{{"type", "integer"}};

    // ---------------------------------------------------------------- terrain
    add("sculpt_terrain", "Sculpt / Paint Terrain",
        "Terrain-window brush strokes (a mouse drag): the brush moves along each stroke's points, "
        "seconds_per_point at each. Heights are 0-1 of max_height; raise/lower move "
        "~strength*1.5*seconds_per_point per point (repeat points for big hills). ramp: first to last point; "
        "stamp needs mask; place/erase_object: asset, one per point within size. Missing maps are created. "
        "Returns heights after each stroke. Whole maps: set_terrain_maps.",
        object({
            {"scene", sceneRef()},
            {"entity", entityRef("Default: the only terrain.")},
            {"space", withDefault(stringEnum("local: terrain-centred.", {"world", "local"}), "world")},
            {"map_resolution", integer("New maps.", 2, 4096)},
            {"strokes", Json{{"type", "array"}, {"minItems", 1},
                {"items", object({
                    {"mode", stringEnum("", {"raise", "lower", "smooth", "flatten", "sharpen", "noise", "terrace", "stamp", "erode", "ramp",
                                             "paint_base", "paint_layer", "paint_density", "erase_density", "place_object", "erase_object"})},
                    {"points", array(array(numberItem, ""), "[x, z] or [x, y, z] points.")},
                    {"size", withDefault(number("Radius, world units.", 0.1, 50), 4)},
                    {"strength", withDefault(number("", 0.01, 1), 0.5)},
                    {"shape", withDefault(stringEnum("", {"circle", "square"}), "circle")},
                    {"falloff", withDefault(stringEnum("", {"smooth", "linear", "constant"}), "smooth")},
                    {"rotation", number("Degrees.")},
                    {"seconds_per_point", withDefault(number("", 0, 10), 0.05)},
                    {"flatten_height", number("Default: height at first point.", 0, 1)},
                    {"terrace_steps", integer("", 2, 64)},
                    {"noise_size", number("World units per cell.")},
                    {"layer", integer("Surface or foliage layer.", 0, 64)},
                    {"normalize", withDefault(boolean(""), true)},
                    {"mask", assetPath("Mask texture.")},
                    {"asset", assetPath("Model or prefab.")},
                    {"instanced", withDefault(boolean(""), true)},
                    {"spacing", number("")},
                    {"min_scale", number("")},
                    {"max_scale", number("")},
                    {"rotation_jitter", number("Turns.", 0, 1)},
                    {"align_to_normal", number("0 upright, 1 surface.", 0, 1)}
                }, {"points"})}}}
        }, {"strokes"}),
        sculptTerrainTool, false, false);

    add("set_terrain_maps", "Set Terrain Maps",
        "Assign or generate terrain maps in one undo step. heightmap: an image path, or {generate} a 16-bit map "
        "(middle = flat 0.5, flat at base_height, random_noise, fractal_noise). blendmap: an image path, or "
        "{color} a constant map whose r,g,b weight layers 0,1,2. layers: textures per surface layer (pbr: its "
        "normal/roughness/metallic/occlusion), stochastic_tiling (hides tiling repetition, up to 3x the reads) "
        "and its cell_size/blend_sharpness/contrast/rotation. Images under the assets dir.",
        object({
            {"scene", sceneRef()},
            {"entity", entityRef("Default: the only terrain.")},
            {"heightmap", {{"type", {"string", "object"}}, {"properties", {
                {"generate", withDefault(stringEnum("", {"middle", "flat", "random_noise", "fractal_noise"}), "middle")},
                {"base_height", number("0-1.", 0, 1)},
                {"amplitude", withDefault(number("0-1.", 0, 1), 0.35)},
                {"frequency", withDefault(number(""), 4)},
                {"octaves", withDefault(integer("", 1, 8), 4)},
                {"seed", integer("")},
                {"resolution", integer("Default: Terrain setting.", 2, 4096)}}}}},
            {"blendmap", {{"type", {"string", "object"}}, {"properties", {
                {"color", vec3("Layer 0,1,2 weights 0-1.")},
                {"resolution", integer("", 2, 4096)}}}}},
            {"layers", array(object({
                {"index", integer("", 0, MAX_TERRAIN_LAYERS - 1)},
                {"pbr", boolean("")},
                {"color", string("")}, {"normal", string("")}, {"roughness", string("")},
                {"metallic", string("")}, {"occlusion", string("")}, {"height", string("Blend height only.")},
                {"stochastic_tiling", boolean("")},
                {"cell_size", withDefault(number("Texture repeats.", 0.25, 4), 1)},
                {"blend_sharpness", withDefault(number("", 0, 1), 0.4)},
                {"contrast", withDefault(number("", 0, 1), 0.45)},
                {"rotation", withDefault(number("Of +-180 deg.", 0, 1), 0)}
            }, {"index"}), "")}
        }),
        setTerrainMapsTool, false, false);

    add("sample_terrain", "Sample Terrain",
        "Terrain size, max_height, maps, layers and heights. points: each one's position, world height, 0-1 "
        "height, normal; else an N x N grid of world heights, min/max/mean (rows -z to +z, columns -x to +x).",
        object({
            {"scene", sceneRef()},
            {"entity", entityRef("Default: the only terrain.")},
            {"points", array(array(numberItem, ""), "[x, z] points.")},
            {"space", withDefault(stringEnum("", {"world", "local"}), "world")},
            {"grid", integer("Default 9 without points.", 0, 65)}
        }),
        sampleTerrainTool, true, true);

    // ---------------------------------------------------------------- physics
    add("edit_colliders", "Edit Colliders",
        "Add/update/remove a Rigidbody 2D / Rigidbody's colliders, creating the body if missing (no edits = "
        "list). Order: clear, remove (old indices), update [{index, ...}], add. 2D: box (size, center), circle "
        "(radius), capsule (radius, point_a, point_b), segment, polygon (3-16 points), chain (4-16, loop), px "
        "before scale. 3D: box, sphere, capsule/cylinder (radius, half_height), tapered_capsule, "
        "convex_hull/mesh, heightfield. New shapes fit what the GameObject draws unless fit: false. Also "
        "density, friction, restitution (3D: per body), sensor, *_events, category_bits, mask_bits, center, "
        "rotation. Returns the colliders.",
        object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"dimension", stringEnum("Default: body, else scene.", {"2d", "3d"})},
            {"body_type", stringEnum("", {"static", "kinematic", "dynamic"})},
            {"body", anyObject("Other body properties.")},
            {"clear", boolean("")},
            {"remove", array(integerItem, "")},
            {"update", array(anyObject("{index, ...fields}"), "")},
            {"add", array(anyObject("{shape, ...fields}"), "")}
        }, {"entity"}),
        editCollidersTool, false, false);

    add("set_joint", "Create / Edit Joint",
        "Create a joint GameObject between two bodies, or edit one (joint). 2D: distance, revolute, prismatic, "
        "mouse, wheel, weld, motor. 3D: fixed, distance, point, hinge, cone, prismatic, swing_twist, six_dof, "
        "path, gear, rack_and_pinion, pulley. Anchors turn auto_anchors off. Both bodies need Rigidbodies of "
        "that dimension; built on play.",
        object({
            {"scene", sceneRef()},
            {"joint", entityRef("Joint to edit.")},
            {"name", string("")},
            {"dimension", stringEnum("", {"2d", "3d"})},
            {"type", string("")},
            {"body_a", entityRef()},
            {"body_b", entityRef()},
            {"anchor_a", any("")},
            {"anchor_b", any("")},
            {"anchor", vec3("3D.")},
            {"axis", any("")},
            {"normal", vec3("3D.")},
            {"target", vec2("2D mouse.")},
            {"limits", vec2("3D [min, max].")},
            {"rope", boolean("2D distance.")},
            {"auto_anchors", boolean("")},
            {"properties", anyObject("Other joint properties.")}
        }),
        setJointTool, false, false);

    // ---------------------------------------------------------------- tilemap
    add("edit_tilemap", "Edit Tilemap",
        "Build or edit a Tilemap: texture, tile rects (tileset slice or rects), then ops in order. Cells (col, "
        "row) of cell_size (default: first rect's) from origin; row 0 = bottom. paint {grid, legend, at, blank}: "
        "grid = multi-line string or lines, TOP line = highest row, bottom line at row at[1]; legend: char -> "
        "rect name/index (null erases); '.' and ' ' are empty (blank: skip|erase). fill {tile, from, to} "
        "(inclusive), erase {from, to}, set {cells: [[col, row, tile]]}, clear.",
        object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"create", createSpec},
            {"texture", assetPath("Tileset image.")},
            {"tileset", object({
                {"tile_size", vec2("Pixels.")},
                {"columns", integer("")},
                {"rows", integer("")},
                {"offset", vec2("Pixels.")},
                {"padding", vec2("Pixels.")},
                {"count", integer("")},
                {"names", array(string(""), "Row-major.")},
                {"prefix", withDefault(string(""), "tile_")},
                {"sheet_size", vec2("If unreadable.")}
            })},
            {"rects", array(object({{"name", string("Replaces same name.")},
                                    {"rect", vec4("[x, y, w, h] px, top-left origin.")},
                                    {"submesh", integer("")}}, {"rect"}), "Extra rects.")},
            {"replace_rects", boolean("")},
            {"cell_size", vec2("")},
            {"origin", vec2(">= 0.")},
            {"ops", array(anyObject("{op, ...}"), "")},
            {"return_grid", withDefault(boolean(""), true)},
            {"max_cells", withDefault(integer("Largest grid as lines."), 6000)}
        }),
        editTilemapTool, false, false);

    add("get_tilemap", "Get Tilemap",
        "A Tilemap: texture, rects (index, name, pixel rect), tile count, cell size, col/row ranges, an ASCII "
        "view (grid.lines, top = highest row; grid.legend). Off-grid tiles are only counted.",
        object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"cell_size", vec2("")},
            {"origin", vec2("")},
            {"include_tiles", boolean("")},
            {"limit", withDefault(integer(""), 200)},
            {"max_cells", withDefault(integer("Largest grid as lines."), 6000)}
        }, {"entity"}),
        getTilemapTool, true, true);

    // ---------------------------------------------------------------- sprites
    add("set_sprite_frames", "Set Sprite Frames",
        "Give a Sprite its sheet and frames (Texture Slicer): slice cuts texture into row-major frames, or "
        "frames lists pixel rects (top-left origin). It shows frame (sized to it with resize); pivot center "
        "suits characters. create instead of entity makes one. Animate: set_sprite_animation; scripts: "
        "Sprite:setFrame(index or name).",
        object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"create", createSpec},
            {"texture", assetPath("Sheet image.")},
            {"slice", object({
                {"tile_size", vec2("Pixels.")}, {"columns", integer("")}, {"rows", integer("")},
                {"offset", vec2("Pixels.")}, {"padding", vec2("Pixels.")},
                {"count", integer("")}, {"names", array(string(""), "")},
                {"prefix", withDefault(string(""), "frame_")}, {"sheet_size", vec2("If unreadable.")}})},
            {"frames", array(object({{"name", string("")}, {"rect", vec4("[x, y, w, h] px.")}}, {"rect"}), "")},
            {"frame", withDefault(integer(""), 0)},
            {"resize", withDefault(boolean(""), true)},
            {"size", vec2("")},
            {"pivot", stringEnum("", {"center", "top_center", "bottom_center", "left_center", "right_center",
                                      "top_left", "bottom_left", "top_right", "bottom_right"})}
        }),
        setSpriteFramesTool, false, false);

    add("set_sprite_animation", "Set Sprite Animation",
        "Create (or update: animation) a Sprite Animation, a child of the sprite cycling its frames (indices or "
        "names, default all) by fps or frame_ms (number or one per frame). autoplay (default true when new) "
        "starts it on play; make several (Idle, Run), switch them in scripts.",
        object({
            {"scene", sceneRef()},
            {"sprite", entityRef("Sprite with frames.")},
            {"animation", entityRef("To update.")},
            {"name", string("")},
            {"frames", array(Json{{"type", {"integer", "string"}}}, "")},
            {"fps", withDefault(number(""), 10)},
            {"frame_ms", any("")},
            {"loop", withDefault(boolean(""), true)},
            {"autoplay", boolean("")}
        }, {"sprite"}),
        setSpriteAnimationTool, false, false);

    // ---------------------------------------------------------------- animation
    add("edit_animation", "Edit Animation",
        "Author an Animation timeline (seconds); no animation: creates one under target. tracks: keys {time, "
        "value, ease} of local position/scale, rotation (quaternion or euler; > 180 degrees needs middle keys), "
        "morph weights or lattice offsets (points {\"i\": [x,y,z]}, [[[i,j,k],[x,y,z]]], \"current\" = "
        "editor keying, \"rest\" or a pose name); ease to the next key: linear, step, "
        "<quad|cubic|sine|back|bounce|elastic...>_<in|out|in_out>; the same target+channel replaces or merges. "
        "tweens: tween actions (from: current). add_actions: existing actions; set_frames/remove_actions: "
        "pre-call indices.",
        object({
            {"scene", sceneRef()},
            {"animation", entityRef("Animation to edit.")},
            {"target", entityRef("Required to create.")},
            {"name", string("")},
            {"tracks", array(object({
                {"channel", stringEnum("", {"position", "rotation", "scale", "morph", "lattice"})},
                {"keys", array(object({{"time", number("")}, {"value", any("")}, {"points", any("")}, {"ease", string("")}}, {"time"}), "")},
                {"target", entityRef("Default: target.")},
                {"start_time", number("")},
                {"mode", withDefault(stringEnum("", {"replace", "merge"}), "replace")},
                {"name", string("")}
            }, {"channel", "keys"}), "")},
            {"tweens", array(object({
                {"type", stringEnum("", {"position", "rotation", "scale", "color", "alpha"})},
                {"from", any("")}, {"to", any("")},
                {"duration", number("> 0.")}, {"ease", string("")}, {"loop", boolean("")},
                {"start_time", number("")}, {"target", entityRef("Default: target.")},
                {"name", string("")}
            }, {"type", "to"}), "")},
            {"add_actions", array(object({{"action", entityRef()}, {"start_time", number("")},
                                          {"duration", number("0 = own.")}, {"lane", integer("")}}, {"action"}), "")},
            {"set_frames", array(object({{"index", integer("")}, {"start_time", number("")},
                                         {"duration", number("0 = auto.")}, {"lane", integer("")}}, {"index"}), "")},
            {"remove_actions", array(integerItem, "")},
            {"delete_removed", withDefault(boolean("Delete removed GameObjects."), true)},
            {"loop", boolean("")},
            {"duration", number("-1: until actions end.")},
            {"speed", number("")},
            {"autoplay", boolean("")},
            {"max_keys", withDefault(integer("Per track."), 20)}
        }),
        editAnimationTool, false, false);

    add("get_animation", "Get Animation",
        "An Animation's timeline: loop, duration, autoplay, target and actions (index, kind, GameObject, target, "
        "start_time, duration, lane, keys or tween values). For another GameObject: animations targeting it or "
        "its children (model clips too).",
        object({
            {"scene", sceneRef()},
            {"entity", entityRef("Animation, or an animated GameObject.")},
            {"max_keys", withDefault(integer("Per track."), 50)}
        }, {"entity"}),
        getAnimationTool, true, true);

    add("preview_animation", "Preview Animation",
        "Edit-mode preview (Animation window): play, pause, seek (time), stop (restores the authored "
        "state; always stop when done). Selects the animation; wait_ms lets it play first.",
        object({
            {"scene", sceneRef()},
            {"animation", entityRef()},
            {"action", withDefault(stringEnum("", {"play", "pause", "stop", "seek"}), "play")},
            {"time", number("seek: seconds.")},
            {"wait_ms", integer("", 0, 30000)}
        }, {"animation"}),
        previewAnimationTool, false, false);

    // ---------------------------------------------------------------- UI
    add("create_ui", "Create UI",
        "Build a UI tree from one root node {type, name, layout, text, font_size, font, text_color, color, "
        "colors, texture, value, placeholder, direction, multiline, properties, children}. Types: panel (text = "
        "title), image, text, button (text = label), input_field, progress_bar (value 0-1), scrollbar, "
        "layout_group (direction), container, polygon. layout {anchor, size, position, pivot, margins}: px, y "
        "DOWN from the parent's top-left; an anchor (top_left, center, top_wide, ..., stretch) pins to the "
        "parent (screen for roots), position then offsets; stretch + margins [l,t,r,b] fills it. Colors linear. "
        "HUDs/menus go in a UI scene (child scene): pointer input needs its UI camera; layout groups stack "
        "bottom-up elsewhere.",
        object({
            {"scene", sceneRef()},
            {"parent", entityRef("Default: scene root.")},
            {"root", anyObject("")}
        }, {"root"}),
        createUiTool, false, false);

    add("set_rect_layout", "Set UI Layout",
        "Lay out existing UI elements (RectTransform) with create_ui's layout rules.",
        object({
            {"scene", sceneRef()},
            {"items", array(object({
                {"entity", entityRef()}, {"anchor", string("")}, {"size", vec2("")},
                {"position", vec2("")}, {"pivot", vec2("0-1, (0,0) top-left.")},
                {"margins", any("")}
            }, {"entity"}), "")}
        }, {"items"}),
        setRectLayoutTool, false, false, true);

    // ---------------------------------------------------------------- particles
    add("create_particles", "Create / Configure Particles",
        "Create a particle effect (Points GameObject + Particle System child) from a preset, or reconfigure one "
        "(entity). 3D presets in meters; 2D ones scaled by scale (px per unit). emitter {shape: "
        "box|sphere|hemisphere|circle|cone, min, max, radius, inner_radius, cone_angle, cone_height}; "
        "velocity/acceleration/color: value or {min, max}; gradient = color over life; bursts [{time, count}]. "
        "Runs in play (autoplay). emitter_entity: draw with an existing Points/Mesh.",
        object({
            {"scene", sceneRef()},
            {"entity", entityRef("Effect to reconfigure.")},
            {"name", string("")},
            {"parent", entityRef("Default: scene root.")},
            {"position", vec3("Local.")},
            {"emitter_entity", entityRef()},
            {"preset", stringEnum("", {"fire", "smoke", "sparks", "explosion", "snow", "rain", "magic", "none"})},
            {"scale", number("Default 1 (3D), 40 (2D).")},
            {"max_particles", integer("", 1, 100000)},
            {"rate", integer("", 0, 100000)},
            {"loop", boolean("")},
            {"life", any("Seconds or [min, max].")},
            {"emitter", anyObject("")},
            {"velocity", any("")},
            {"acceleration", any("")},
            {"size", any("Or [min, max].")},
            {"size_over_life", vec2("")},
            {"alpha_over_life", vec2("")},
            {"color", any("")},
            {"gradient", array(arrayItem, "[[t, [r, g, b]], ...]")},
            {"bursts", array(anyObject(""), "")},
            {"texture", assetPath("")},
            {"properties", anyObject("")},
            {"autoplay", boolean("")}
        }),
        createParticlesTool, false, false);

    // ---------------------------------------------------------------- models
    add("get_model_info", "Get Model Info",
        "What a Model loaded: file, loading, mesh GameObjects, bones, animation clips (tracks, length, loop), "
        "morph targets, skeleton.",
        object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"include_bones", withDefault(boolean(""), true)}
        }, {"entity"}),
        getModelInfoTool, true, true);

    add("set_model", "Load / Clear Model",
        "Load a .gltf/.glb/.obj into a Model GameObject (replacing it), or clear it; create instead of entity "
        "makes one. Waits for the load; returns get_model_info's summary.",
        object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"create", createSpec},
            {"path", assetPath("")},
            {"merge_static_meshes", boolean("")},
            {"clear", boolean("")},
            {"timeout_seconds", withDefault(integer("", 1, 600), 60)}
        }),
        setModelTool, false, false);

    // ---------------------------------------------------------------- shape points
    add("set_shape_points", "Set Shape Points",
        "Set the points of a Polygon (UI), Polygon Mesh, Shadow Caster 2D or Line Renderer: points (local), "
        "circle or rect. Polygon order outline converts a convex outline to the triangle strip; strip: points "
        "as is. Line renderers: polyline (closed joins ends) or lines.",
        object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"component", stringEnum("", {"polygon", "mesh_polygon", "shadow_caster_2d", "line_renderer"})},
            {"points", array(arrayItem, "[[x, y(, z)], ...]")},
            {"circle", object({{"center", vec2("")}, {"radius", number("")}, {"segments", integer("3-256.")}})},
            {"rect", vec4("[x, y, w, h].")},
            {"order", withDefault(stringEnum("", {"outline", "strip"}), "outline")},
            {"closed", boolean("")},
            {"lines", array(anyObject("{a, b, color_a, color_b}"), "")},
            {"color", color("")},
            {"colors", array(arrayItem, "One per point.")}
        }, {"entity"}),
        setShapePointsTool, false, false, true);
}

}
