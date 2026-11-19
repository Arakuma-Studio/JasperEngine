// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// query_physics (docs/specs/spatial-queries.md R25): the spatial queries of Physics, Physics2D
// and MeshRaycast for agents. Physics worlds answer only while playing (their bodies exist then);
// world "drawn" (SceneBvh bounds) answers in edit mode too. verify compares an overlap with
// PhysicsQueryReference's brute-force sweep; draw shows the query for 2 s in both views.

#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "mcp/tools/PlayViewCommon.h"

#include "Project.h"
#include "Scene.h"
#include "picking/MeshRaycast.h"
#include "physics/PhysicsQueryDebug.h"
#include "physics/PhysicsQueryReference.h"
#include "registry/PhysicsLayers.h"
#include "registry/TagManager.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

using Shape = physics::QueryShape::Type;

Json vectorJson(const Vector3& v) {
    return Json::array({v.x, v.y, v.z});
}

// the shape argument as a QueryShape at origin; false with the error naming the field
bool readQueryShape(const Json& args, const std::string& world, physics::QueryShape& shape, std::string& error) {
    if (args.contains("shape") && !args["shape"].is_object()) {
        error = "shape must be an object {type, radius, half_extents, size, point1, angle, rotation, direction2d}.";
        return false;
    }
    const Json spec = args.contains("shape") ? args["shape"] : Json::object();
    static const std::set<std::string> keys = {"type", "radius", "half_extents", "size", "point1", "angle", "rotation", "direction2d"};
    for (auto it = spec.begin(); it != spec.end(); ++it) {
        if (!keys.count(it.key())) {
            error = "shape." + it.key() + " is not a shape field: type, radius, half_extents, size, point1, angle, rotation, direction2d.";
            return false;
        }
    }
    if (spec.contains("type") && !spec["type"].is_string()) {
        error = "shape.type must be sphere, box, capsule, circle, point or area.";
        return false;
    }
    if (spec.contains("direction2d") && spec["direction2d"] != "vertical" && spec["direction2d"] != "horizontal") {
        error = "shape.direction2d must be vertical or horizontal.";
        return false;
    }
    const bool is2D = world == "2d";
    const std::string type = spec.value("type", is2D ? "circle" : "sphere");
    Vector3 origin;
    if (!args.contains("origin") || !McpValues::parseVector3(args["origin"], origin)) {
        if (args.contains("origin") && is2D) {
            Vector2 flat;
            if (McpValues::parseVector2(args["origin"], flat)) origin = Vector3(flat.x, flat.y, 0.0f);
            else { error = "origin must be [x, y] or [x, y, z]."; return false; }
        } else {
            error = args.contains("origin") ? "origin must be [x, y, z]." : "origin is required: the shape's centre (or a capsule's first end, an area's first corner).";
            return false;
        }
    }
    shape.a = origin;
    auto radius = [&]() {
        if (!spec.contains("radius") || !spec["radius"].is_number()) {
            error = "shape.radius is required for a " + type + ".";
            return false;
        }
        shape.radius = spec["radius"].get<float>();
        return true;
    };
    auto point1 = [&]() {
        if (!spec.contains("point1")) {
            error = std::string("shape.point1 is required for a ") + type + (type == "area" ? " (its second corner)." : " (its second end).");
            return false;
        }
        Vector2 flat;
        if (McpValues::parseVector3(spec["point1"], shape.b)) return true;
        if (McpValues::parseVector2(spec["point1"], flat)) {
            shape.b = Vector3(flat.x, flat.y, 0.0f);
            return true;
        }
        error = "shape.point1 must be [x, y, z] (or [x, y] in 2D).";
        return false;
    };
    auto size = [&]() {
        Vector2 value;
        if (!spec.contains("size") || !McpValues::parseVector2(spec["size"], value)) {
            error = "shape.size [width, height] is required for a 2D " + type + ".";
            return false;
        }
        shape.halfExtents = Vector3(value.x * 0.5f, value.y * 0.5f, 0.0f);
        return true;
    };
    shape.angle = spec.contains("angle") && spec["angle"].is_number() ? spec["angle"].get<float>() : 0.0f;
    if (world == "drawn" || world == "3d") {
        if (type == "sphere") {
            shape.type = Shape::Sphere;
            return radius();
        }
        if (type == "box") {
            shape.type = Shape::Box;
            if (!spec.contains("half_extents") || !McpValues::parseVector3(spec["half_extents"], shape.halfExtents)) {
                error = "shape.half_extents [x, y, z] is required for a box.";
                return false;
            }
            if (spec.contains("rotation") && !McpValues::parseQuaternion(spec["rotation"], shape.rotation, &error)) {
                error = "shape.rotation: " + error;
                return false;
            }
            return true;
        }
        if (type == "capsule" && world == "3d") {
            shape.type = Shape::Capsule;
            return radius() && point1();
        }
        error = "shape.type " + type + (world == "drawn" ? " is not a drawn-world shape: sphere or box." : " is not a 3D shape: sphere, box or capsule.");
        return false;
    }
    if (type == "point") {
        shape.type = Shape::Point2D;
        return true;
    }
    if (type == "circle") {
        shape.type = Shape::Circle2D;
        return radius();
    }
    if (type == "box") {
        shape.type = Shape::Box2D;
        return size();
    }
    if (type == "capsule") {
        shape.type = Shape::Capsule2D;
        shape.direction2D = spec.value("direction2d", "vertical") == "horizontal" ? CapsuleDirection2D::Horizontal : CapsuleDirection2D::Vertical;
        return size();
    }
    if (type == "area") {
        shape.type = Shape::Area2D;
        return point1();
    }
    error = "shape.type " + type + " is not a 2D shape: point, circle, box, capsule or area.";
    return false;
}

// the cast of the shape (All: nearest first)
void castShape(Scene* scene, const physics::QueryShape& shape, const Vector3& direction, float distance,
               const PhysicsQueryOptions& options, std::vector<RayReturn>& out) {
    const Vector2 origin(shape.a.x, shape.a.y);
    const Vector2 flat(direction.x, direction.y);
    const Vector2 size(shape.halfExtents.x * 2.0f, shape.halfExtents.y * 2.0f);
    switch (shape.type) {
#ifdef JASPERENGINE_PHYSICS_3D
        case Shape::Sphere: Physics::sphereCastAll(scene, shape.a, shape.radius, direction, distance, out, options); break;
        case Shape::Box: Physics::boxCastAll(scene, shape.a, shape.halfExtents, direction, distance, shape.rotation, out, options); break;
        case Shape::Capsule: Physics::capsuleCastAll(scene, shape.a, shape.b, shape.radius, direction, distance, out, options); break;
#endif
#ifdef JASPERENGINE_PHYSICS_2D
        case Shape::Circle2D: Physics2D::circleCastAll(scene, origin, shape.radius, flat, distance, out, options); break;
        case Shape::Box2D: Physics2D::boxCastAll(scene, origin, size, shape.angle, flat, distance, out, options); break;
        case Shape::Capsule2D: Physics2D::capsuleCastAll(scene, origin, size, shape.direction2D, shape.angle, flat, distance, out, options); break;
#endif
        default: (void)origin; (void)flat; (void)size; out.clear(); break;
    }
}

void queryPhysicsTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    const std::string op = args.value("op", "overlap");
    const bool playing = project->getPlaySessionSceneId() != NULL_PROJECT_SCENE;

    // the scene: the running game's in Play, the selected one in edit mode (drawn only)
    std::string world = args.value("world", "");
    SceneProject* sceneProject = nullptr;
    if (playing) {
        sceneProject = w3::resolveRuntimeScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    } else {
        sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    }
    if (!sceneProject) {
        call->fail(error);
        return;
    }
    if (!sceneProject->scene) {
        call->fail(McpRefs::notOpenError(project, *sceneProject));
        return;
    }
    if (world.empty()) world = sceneProject->sceneType == SceneType::SCENE_2D ? "2d" : "3d";
    if (world == "drawn" && op != "overlap") {
        call->fail("world: drawn answers op: overlap only (bounds); for rays against what is drawn use raycast_meshes.");
        return;
    }
    if (world != "drawn" && !playing) {
        call->fail("physics queries need Play: call play first (bodies exist only while the scene runs); world: drawn works in edit mode.");
        return;
    }
    Scene* scene = sceneProject->scene;

    physics::QueryShape shape;
    if (op == "raycast") {
        if (!args.contains("origin") || !McpValues::parseVector3(args["origin"], shape.a)) {
            Vector2 flat;
            if (!(args.contains("origin") && McpValues::parseVector2(args["origin"], flat))) {
                call->fail("origin [x, y, z] is required: where the ray starts.");
                return;
            }
            shape.a = Vector3(flat.x, flat.y, 0.0f);
        }
        shape.type = world == "2d" ? Shape::Circle2D : Shape::Sphere;
    } else if (!readQueryShape(args, world, shape, error)) {
        call->fail(error);
        return;
    }
    Vector3 direction;
    float distance = 0.0f;
    if (op == "cast" || op == "raycast") {
        Vector2 flat;
        if (args.contains("direction") && McpValues::parseVector3(args["direction"], direction)) {
        } else if (args.contains("direction") && McpValues::parseVector2(args["direction"], flat)) {
            direction = Vector3(flat.x, flat.y, 0.0f);
        } else {
            call->fail("direction [x, y, z] is required for op: " + op + ".");
            return;
        }
        if (!args.contains("max_distance") || !args["max_distance"].is_number() || !(args["max_distance"].get<float>() > 0.0f)) {
            call->fail("max_distance (a positive number, world units; 2D scene units) is required for op: " + op + ".");
            return;
        }
        distance = args["max_distance"].get<float>();
    }

    PhysicsQueryOptions options;
    if (args.contains("layer_mask") && args["layer_mask"].is_number_integer()) {
        options.layerMask = (uint32_t)args["layer_mask"].get<long long>();
    }
    const std::string triggers = args.value("triggers", "use_global");
    options.triggers = triggers == "ignore" ? QueryTriggerInteraction::Ignore
                     : triggers == "collide" ? QueryTriggerInteraction::Collide : QueryTriggerInteraction::UseGlobal;
    std::vector<Entity> ignore;
    if (args.contains("ignore") && !McpRefs::resolveEntities(sceneProject, args["ignore"], ignore, error)) {
        call->fail("ignore: " + error);
        return;
    }
    options.ignore = ignore;
    const size_t maxHits = (size_t)std::clamp<long long>(args.value("max_hits", 64LL), 1, 1024);
    const bool verify = args.value("verify", false);
    if (verify && (op != "overlap" || world == "drawn")) {
        call->fail("verify compares a physics overlap (op: overlap, world 3d or 2d) with a brute-force sweep.");
        return;
    }

    // run it
    const bool is2D = world == "2d";
    Json hits = Json::array();
    size_t count = 0;
    std::vector<Vector3> points;
    std::vector<OverlapHit> overlaps;
    if (world == "drawn") {
        std::vector<MeshOverlapHit> drawn;
        MeshRaycastOptions drawnOptions = MeshRaycast::overlapOptions();
        drawnOptions.current = true;
        drawnOptions.ignore = ignore;
        if (shape.type == Shape::Box) MeshRaycast::overlapBox(scene, shape.a, shape.halfExtents, shape.rotation, drawn, options.layerMask, drawnOptions);
        else MeshRaycast::overlapSphere(scene, shape.a, shape.radius, drawn, options.layerMask, drawnOptions);
        count = drawn.size();
        for (size_t i = 0; i < drawn.size() && i < maxHits; i++) {
            Json item = {{"entity", McpRefs::entityJson(sceneProject, drawn[i].entity)}, {"kind", pickKindName(drawn[i].kind)}};
            if (drawn[i].instance >= 0) item["instance"] = drawn[i].instance;
            hits.push_back(item);
        }
    } else if (op == "cast" || op == "raycast") {
        std::vector<RayReturn> casts;
        if (op == "raycast") {
#ifdef JASPERENGINE_PHYSICS_3D
            if (!is2D) casts = Physics::raycastAll(scene, shape.a, direction, distance, options.layerMask, options.triggers);
#endif
#ifdef JASPERENGINE_PHYSICS_2D
            if (is2D) casts = Physics2D::raycastAll(scene, Vector2(shape.a.x, shape.a.y), Vector2(direction.x, direction.y), distance, options.layerMask, options.triggers);
#endif
            // the ray takes no ignore list: drop those bodies here
            casts.erase(std::remove_if(casts.begin(), casts.end(), [&](const RayReturn& hit) {
                return std::find(ignore.begin(), ignore.end(), hit.body) != ignore.end();
            }), casts.end());
        } else {
            castShape(scene, shape, direction, distance, options, casts);
        }
        count = casts.size();
        for (size_t i = 0; i < casts.size() && i < maxHits; i++) {
            hits.push_back({{"entity", McpRefs::entityJson(sceneProject, casts[i].body)}, {"collider", casts[i].colliderIndex},
                            {"point", vectorJson(casts[i].point)}, {"normal", vectorJson(casts[i].normal)}, {"distance", casts[i].distance}});
            points.push_back(casts[i].point);
        }
    } else {
        physics::queryOverlap(scene, shape, options, overlaps);
        if (op == "check" && overlaps.size() > 1) overlaps.resize(1);
        count = overlaps.size();
        for (size_t i = 0; i < overlaps.size() && i < maxHits; i++) {
            hits.push_back({{"entity", McpRefs::entityJson(sceneProject, overlaps[i].body)}, {"collider", overlaps[i].colliderIndex},
                            {"trigger", overlaps[i].trigger}});
        }
    }

    Json result = {{"op", op}, {"world", world}, {"count", count}, {"truncated", count > maxHits}, {"hits", hits}};
    if (op == "check") result["hit"] = count > 0;
    std::string next;

    if (verify) {
        std::vector<OverlapHit> reference;
        physics::referenceOverlap(scene, is2D, shape, options, reference);
        const physics::ReferenceDiff diff = physics::compare(overlaps, reference);
        auto list = [&](const std::vector<OverlapHit>& items) {
            Json out = Json::array();
            for (const OverlapHit& item : items) out.push_back({{"entity", McpRefs::entityJson(sceneProject, item.body)}, {"collider", item.colliderIndex}});
            return out;
        };
        result["verify"] = {{"reference_count", diff.referenceCount}, {"missing", list(diff.missing)}, {"extra", list(diff.extra)}, {"ok", diff.ok()}};
        if (!diff.ok()) next = "report: the query and the sweep differ (verify.missing / verify.extra).";
    }

    // zero hits: say what the filters left out
    if (count == 0 && world != "drawn" && (op == "overlap" || op == "check")) {
        PhysicsQueryOptions open = options;
        open.layerMask = Physics::ALL_LAYERS;
        open.triggers = QueryTriggerInteraction::Collide;
        std::vector<OverlapHit> inside;
        physics::queryOverlap(scene, shape, open, inside);
        std::set<Entity> masked;
        std::set<int> layers;
        bool allTriggers = !inside.empty();
        for (const OverlapHit& hit : inside) {
            const int layer = scene->getEntityLayer(hit.body);
            if (!((options.layerMask >> (layer & 31)) & 1u)) {
                masked.insert(hit.body);
                layers.insert(layer);
            }
            allTriggers = allTriggers && hit.trigger;
        }
        if (!masked.empty()) {
            std::string names;
            for (int layer : layers) {
                const std::string name = TagManager::layerToName(layer);
                names += (names.empty() ? "" : ", ") + (name.empty() ? std::to_string(layer) : name);
            }
            next = "no hits: layer_mask excludes " + std::to_string(masked.size()) + " bodies inside the shape (layers: " + names + ").";
        } else if (allTriggers) {
            next = "no hits: only triggers are inside the shape and triggers are skipped; pass triggers: collide to report them.";
        }
    }
    if (!next.empty()) result["next"] = next;

    if (args.value("draw", false)) {
        Vector3 marks[PhysicsQueryDebug::MAX_HIT_POINTS];
        size_t markCount = 0;
        for (size_t i = 0; i < points.size() && markCount < PhysicsQueryDebug::MAX_HIT_POINTS; i++) marks[markCount++] = points[i];
        const bool cast = op == "cast" || op == "raycast";
        const float reach = cast ? (points.empty() ? distance : (points.back() - shape.a).length()) : 0.0f;
        physics::recordQuery(scene, shape, cast ? direction.normalized() : Vector3::ZERO, reach, cast, marks, markCount, count > 0, true);
    }

    std::string text = op + " (" + world + "): " + std::to_string(count) + " hit" + (count == 1 ? "" : "s");
    for (size_t i = 0; i < hits.size() && i < 8; i++) {
        text += (i == 0 ? ": " : ", ") + hits[i]["entity"].value("path", std::string());
    }
    if (count > 8) text += ", ...";
    text += ".";
    if (!next.empty()) text += " " + next;
    call->ok(text, result);
}

} // namespace

void registerPhysicsQueryTools(McpRegistry& registry) {
    using namespace schema;
    ToolDef tool;
    tool.name = "query_physics";
    tool.group = "physics";
    tool.title = "Query Physics";
    tool.description =
        "Spatial query of the running game's colliders (Play only) or of drawn bounds (world drawn, overlap, edit mode too): "
        "overlap lists colliders touching a shape, check says if any, cast sweeps a shape (nearest first), raycast. Use it "
        "instead of looping over objects in eval_lua. verify compares an overlap with a brute-force sweep; draw shows it 2 s.";
    tool.inputSchema = object({
        {"op", withDefault(stringEnum("", {"overlap", "check", "cast", "raycast"}), "overlap")},
        {"world", stringEnum("Default: the scene's type.", {"3d", "2d", "drawn"})},
        {"shape", anyObject("{type: sphere|box|capsule|circle|point|area, radius, half_extents, size (2D), point1 (capsule end, "
                            "area corner), angle (2D degrees), rotation, direction2d: vertical|horizontal}.")},
        {"origin", vec3("Shape centre or start; 2D in scene units.")},
        {"direction", vec3("")},
        {"max_distance", number("")},
        {"layer_mask", integer("")},
        {"triggers", stringEnum("", {"use_global", "ignore", "collide"})},
        {"ignore", entities()},
        {"max_hits", withDefault(integer("", 1, 1024), 64)},
        {"verify", boolean("")},
        {"draw", boolean("")},
        {"scene", sceneRef()}
    });
    tool.annotations.readOnly = true;
    tool.annotations.idempotent = true;
    tool.allowedWhilePlaying = true;
    tool.handler = queryPhysicsTool;
    registry.addTool(std::move(tool));
}

} // namespace jasperengine::editor::mcp
