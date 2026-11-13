// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// query_physics (docs/specs/S96-physics-gaps.md REQ-17): the running game's physics world asked
// what Lua's Physics / Physics2D queries ask (ray, sphere, box, capsule and point casts,
// overlaps and checks) with the same options, and the shapes and hits drawn for a few seconds
// in the Game view's physics debug lines.
#include "McpTools.h"
#include "PlayViewCommon.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "Configs.h"
#include "Project.h"
#include "Scene.h"
#include "physics/PhysicsDebugLines.h"
#include "physics/PhysicsQueries2D.h"
#include "physics/PhysicsQueries3D.h"
#include "registry/PhysicsLayers.h"
#include "registry/TagManager.h"
#include "subsystem/PhysicsSystem.h"
#include "util/Angle.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

// how long draw: true shows a query (wall time)
constexpr float QUERY_DRAW_SECONDS = 3.0f;
const Vector4 QUERY_SHAPE_COLOR(0.2f, 0.85f, 1.0f, 1.0f);
const Vector4 QUERY_HIT_COLOR(1.0f, 0.35f, 0.1f, 1.0f);

using QueryLines = std::vector<std::pair<Vector3, Vector3>>;

struct QueryArgs {
    bool is2D = false;
    std::string shape;
    std::string mode;
    Vector3 origin;
    Vector3 direction;
    float maxDistance = 0.0f;
    float radius = 0.0f;
    Vector3 halfExtents;
    Vector2 size;
    float angle = 0.0f;
    Quaternion rotation = Quaternion::IDENTITY;
    Vector3 pointA, pointB;
    bool hasPoints = false;
};

// [x, y] or [x, y, z] (2D takes the first two)
bool readQueryVector(const Json& args, const char* key, bool is2D, Vector3& out, std::string& error) {
    if (!args.contains(key)) {
        error = std::string(key) + " is required for this shape and mode.";
        return false;
    }
    const Json& value = args[key];
    const size_t want = is2D ? 2 : 3;
    if (!value.is_array() || value.size() < want || value.size() > 3) {
        error = std::string(key) + (is2D ? " must be [x, y] (points)." : " must be [x, y, z].");
        return false;
    }
    float v[3] = {0.0f, 0.0f, 0.0f};
    for (size_t i = 0; i < want; i++) {
        if (!value[i].is_number()) {
            error = std::string(key) + " must hold numbers.";
            return false;
        }
        v[i] = value[i].get<float>();
    }
    out = Vector3(v[0], v[1], v[2]);
    return true;
}

bool readQueryNumber(const Json& args, const char* key, float& out, std::string& error) {
    if (!args.contains(key) || !args[key].is_number()) {
        error = std::string(key) + " is required for this shape and mode (a number).";
        return false;
    }
    out = args[key].get<float>();
    return true;
}

// a layer mask: a number, "everything" / "nothing", or layer names and numbers
bool readQueryLayerMask(const Json& value, uint32_t& mask, std::string& error) {
    if (value.is_number_integer()) {
        mask = (uint32_t)value.get<long long>();
        return true;
    }
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (text == "everything") { mask = LAYER_MASK_EVERYTHING; return true; }
        if (text == "nothing") { mask = LAYER_MASK_NOTHING; return true; }
    }
    if (value.is_array()) {
        mask = 0;
        for (const Json& item : value) {
            int layer = -1;
            if (item.is_number_integer()) layer = item.get<int>();
            else if (item.is_string()) layer = TagManager::nameToLayer(item.get<std::string>());
            if (layer < 0 || layer >= MAX_LAYERS) {
                error = "layer_mask: unknown layer " + item.dump() + " (Project Settings > Tags and Layers).";
                return false;
            }
            mask |= 1u << layer;
        }
        return true;
    }
    error = "layer_mask: a number, \"everything\", \"nothing\" or a list of layer names or numbers.";
    return false;
}

// ---- the drawn shapes (world space; 2D in points at z 0)

void queryCircleLines(QueryLines& lines, const Vector3& center, const Vector3& axisA, const Vector3& axisB, float radius) {
    constexpr int SEGMENTS = 24;
    constexpr float TURN = 6.28318530717958647692f;
    for (int i = 0; i < SEGMENTS; i++) {
        const float a0 = TURN * (float)i / SEGMENTS;
        const float a1 = TURN * (float)(i + 1) / SEGMENTS;
        lines.push_back({center + (axisA * std::cos(a0) + axisB * std::sin(a0)) * radius,
                         center + (axisA * std::cos(a1) + axisB * std::sin(a1)) * radius});
    }
}

void querySphereLines(QueryLines& lines, const Vector3& center, float radius, bool is2D) {
    queryCircleLines(lines, center, Vector3(1, 0, 0), Vector3(0, 1, 0), radius);
    if (!is2D) {
        queryCircleLines(lines, center, Vector3(1, 0, 0), Vector3(0, 0, 1), radius);
        queryCircleLines(lines, center, Vector3(0, 1, 0), Vector3(0, 0, 1), radius);
    }
}

void queryBoxLines(QueryLines& lines, const Vector3& center, const Vector3& halfExtents, const Quaternion& rotation, bool is2D) {
    Vector3 corners[8];
    for (int i = 0; i < 8; i++) {
        const Vector3 local((i & 1) ? halfExtents.x : -halfExtents.x, (i & 2) ? halfExtents.y : -halfExtents.y,
                            (i & 4) ? halfExtents.z : -halfExtents.z);
        corners[i] = center + rotation * local;
    }
    const int edges3D[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    const int edges2D[4][2] = {{0, 1}, {2, 3}, {0, 2}, {1, 3}};
    if (is2D) {
        for (const auto& edge : edges2D) lines.push_back({corners[edge[0]], corners[edge[1]]});
    } else {
        for (const auto& edge : edges3D) lines.push_back({corners[edge[0]], corners[edge[1]]});
    }
}

void queryCapsuleLines(QueryLines& lines, const Vector3& a, const Vector3& b, float radius, bool is2D) {
    querySphereLines(lines, a, radius, is2D);
    querySphereLines(lines, b, radius, is2D);
    Vector3 axis = b - a;
    if (axis.length() <= 0.0f) return;
    axis = axis.normalized();
    Vector3 side = is2D ? Vector3(-axis.y, axis.x, 0.0f) : axis.crossProduct(std::fabs(axis.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
    lines.push_back({a + side * radius, b + side * radius});
    lines.push_back({a - side * radius, b - side * radius});
    if (!is2D) {
        const Vector3 other = axis.crossProduct(side).normalized();
        lines.push_back({a + other * radius, b + other * radius});
        lines.push_back({a - other * radius, b - other * radius});
    }
}

// the query's shape placed at `offset` from its start
void shapeLines(QueryLines& lines, const QueryArgs& q, const Vector3& offset, float pointsToMeter) {
    if (q.shape == "sphere") {
        querySphereLines(lines, q.origin + offset, q.radius, q.is2D);
    } else if (q.shape == "box") {
        if (q.is2D) {
            const Vector3 half = q.hasPoints ? Vector3(std::fabs(q.pointB.x - q.pointA.x) * 0.5f, std::fabs(q.pointB.y - q.pointA.y) * 0.5f, 0.0f)
                                             : Vector3(q.size.x * 0.5f, q.size.y * 0.5f, 0.0f);
            const Vector3 center = q.hasPoints ? (q.pointA + q.pointB) * 0.5f : q.origin;
            queryBoxLines(lines, center + offset, half, Quaternion(0.0f, 0.0f, q.angle), true);
        } else {
            queryBoxLines(lines, q.origin + offset, q.halfExtents, q.rotation, false);
        }
    } else if (q.shape == "capsule") {
        if (q.is2D) {
            const bool vertical = q.size.y >= q.size.x;
            const float radius = 0.5f * (vertical ? q.size.x : q.size.y);
            const float half = std::max(0.0f, 0.5f * (vertical ? q.size.y : q.size.x) - radius);
            const Quaternion turn(0.0f, 0.0f, q.angle);
            const Vector3 axis = turn * (vertical ? Vector3(0, half, 0) : Vector3(half, 0, 0));
            queryCapsuleLines(lines, q.origin + offset - axis, q.origin + offset + axis, radius, true);
        } else {
            queryCapsuleLines(lines, q.pointA + offset, q.pointB + offset, q.radius, false);
        }
    } else {
        PhysicsDebugLines::crossMark(q.origin + offset, 0.1f * pointsToMeter, [&](const Vector3& a, const Vector3& b) { lines.push_back({a, b}); });
    }
}

void queryPhysicsTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    if (!project || project->getPlaySessionSceneId() == NULL_PROJECT_SCENE) {
        call->fail("query_physics reads the running physics world: start Play first (play).");
        return;
    }
    std::string error;
    SceneProject* sp = w3::resolveRuntimeScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sp || !sp->scene) {
        call->fail(error.empty() ? "query_physics reads the running physics world: start Play first (play)." : error);
        return;
    }
    Scene* scene = sp->scene;
    std::shared_ptr<PhysicsSystem> physics = scene->getSystem<PhysicsSystem>();

    QueryArgs q;
    q.is2D = args.contains("dimension") ? args["dimension"].get<std::string>() == "2d" : sp->sceneType != SceneType::SCENE_3D;
    q.shape = args.value("shape", std::string("ray"));
    q.mode = args.value("mode", std::string("cast"));
    const bool cast = q.mode == "cast" || q.mode == "cast_all";
    if (q.shape == "ray" && !cast) {
        call->fail("A ray only casts: mode cast or cast_all (an overlap needs sphere, box, capsule or point).");
        return;
    }
    if (q.shape == "point" && cast) {
        call->fail("A point does not move: mode overlap or check (cast a ray or a sphere instead).");
        return;
    }
#ifndef JASPERENGINE_PHYSICS_2D
    if (q.is2D) { call->fail("This editor is built without 2D physics."); return; }
#endif
#ifndef JASPERENGINE_PHYSICS_3D
    if (!q.is2D) { call->fail("This editor is built without 3D physics."); return; }
#endif

    QueryOptions options;
    std::vector<Entity> ignore;
    if (args.contains("layer_mask") && !readQueryLayerMask(args["layer_mask"], options.layerMask, error)) {
        call->fail(error);
        return;
    }
    if (args.contains("ignore")) {
        for (const Json& ref : args["ignore"]) {
            const Entity entity = w3::resolveLiveEntity(project, sp, ref, error);
            if (entity == NULL_ENTITY) {
                call->fail("ignore: " + error);
                return;
            }
            ignore.push_back(entity);
        }
        options.ignore = ignore.empty() ? nullptr : &ignore;
    }
    const std::string triggers = args.value("triggers", std::string("use_global"));
    options.triggers = triggers == "ignore" ? QueryTriggerInteraction::IGNORE
                     : triggers == "collide" ? QueryTriggerInteraction::COLLIDE : QueryTriggerInteraction::USE_GLOBAL;
    if (args.value("hit_from_inside", false)) {
        options.hitFromInside = true;
        options.startInColliders = true;
    }

    // the shape's arguments
    const bool needsOrigin = !(q.shape == "capsule" && !q.is2D) && !(q.shape == "box" && q.is2D && args.contains("point_a"));
    if (needsOrigin && !readQueryVector(args, "origin", q.is2D, q.origin, error)) {
        call->fail(error);
        return;
    }
    if (cast) {
        if (!readQueryVector(args, "direction", q.is2D, q.direction, error) || !readQueryNumber(args, "max_distance", q.maxDistance, error)) {
            call->fail(error);
            return;
        }
    }
    if (q.shape == "sphere" || (q.shape == "capsule" && !q.is2D)) {
        if (!readQueryNumber(args, "radius", q.radius, error)) {
            call->fail(error);
            return;
        }
    }
    if (q.shape == "box" && !q.is2D) {
        if (!readQueryVector(args, "half_extents", false, q.halfExtents, error)) {
            call->fail(error);
            return;
        }
        if (args.contains("rotation") && !McpValues::parseQuaternion(args["rotation"], q.rotation, &error)) {
            call->fail("rotation: " + error);
            return;
        }
    }
    if ((q.shape == "box" || q.shape == "capsule") && q.is2D) {
        if (q.shape == "box" && args.contains("point_a")) {
            if (cast) {
                call->fail("point_a and point_b make an overlap area: mode overlap or check (a cast takes origin and size).");
                return;
            }
            if (!readQueryVector(args, "point_a", true, q.pointA, error) || !readQueryVector(args, "point_b", true, q.pointB, error)) {
                call->fail(error);
                return;
            }
            q.hasPoints = true;
        } else {
            Vector3 size;
            if (!readQueryVector(args, "size", true, size, error)) {
                call->fail(error);
                return;
            }
            q.size = Vector2(size.x, size.y);
        }
        q.angle = args.value("angle", 0.0f);
    }
    if (q.shape == "capsule" && !q.is2D) {
        if (!readQueryVector(args, "point_a", false, q.pointA, error) || !readQueryVector(args, "point_b", false, q.pointB, error)) {
            call->fail(error);
            return;
        }
        q.origin = (q.pointA + q.pointB) * 0.5f;
    }

    // the arguments the queries refuse, named as the tool's
    const char* why = nullptr;
    if (q.is2D) {
#ifdef JASPERENGINE_PHYSICS_2D
        if (cast) why = PhysicsQueries2D::invalidDirection(Vector2(q.direction.x, q.direction.y), q.maxDistance);
        if (!why && q.shape == "sphere") why = PhysicsQueries2D::invalidRadius(q.radius);
        if (!why && (q.shape == "box" || q.shape == "capsule") && !q.hasPoints) why = PhysicsQueries2D::invalidSize(q.size);
        if (!why && q.hasPoints) why = PhysicsQueries2D::invalidSize(Vector2(std::fabs(q.pointB.x - q.pointA.x), std::fabs(q.pointB.y - q.pointA.y)));
#endif
    } else {
#ifdef JASPERENGINE_PHYSICS_3D
        if (cast) why = PhysicsQueries3D::invalidDirection(q.direction, q.maxDistance);
        if (!why && (q.shape == "sphere" || q.shape == "capsule")) why = PhysicsQueries3D::invalidRadius(q.radius);
        if (!why && q.shape == "box") why = PhysicsQueries3D::invalidHalfExtents(q.halfExtents);
#endif
    }
    if (why) {
        // "direction must be ...", "maxDistance must be ..." in the tool's words
        std::string text = why;
        for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
                 {"maxDistance", "max_distance"}, {"distance must", "max_distance must"}, {"halfExtents", "half_extents"}}) {
            const size_t at = text.find(from);
            if (at != std::string::npos) text.replace(at, from.size(), to);
        }
        call->fail(text + ".");
        return;
    }

    // run it
    std::vector<RayReturn> hits;
    std::vector<std::pair<Entity, int>> colliders;
    bool any = false;
    [[maybe_unused]] const Vector2 origin2(q.origin.x, q.origin.y), direction2(q.direction.x, q.direction.y);
    [[maybe_unused]] const Vector2 size2 = q.size;
    [[maybe_unused]] const CapsuleDirection2D capsuleDirection = q.size.y >= q.size.x ? CapsuleDirection2D::VERTICAL : CapsuleDirection2D::HORIZONTAL;
    if (cast) {
        const bool all = q.mode == "cast_all";
        if (q.is2D) {
#ifdef JASPERENGINE_PHYSICS_2D
            if (q.shape == "ray") {
                if (all) hits = Physics2D::raycastAll(scene, origin2, direction2, q.maxDistance, options);
                else hits.push_back(Physics2D::raycast(scene, origin2, direction2, q.maxDistance, options));
            } else if (q.shape == "sphere") {
                if (all) PhysicsQueries2D::circleCastAll(scene, origin2, q.radius, direction2, q.maxDistance, hits, options);
                else hits.push_back(PhysicsQueries2D::circleCast(scene, origin2, q.radius, direction2, q.maxDistance, options));
            } else if (q.shape == "box") {
                if (all) PhysicsQueries2D::boxCastAll(scene, origin2, size2, q.angle, direction2, q.maxDistance, hits, options);
                else hits.push_back(PhysicsQueries2D::boxCast(scene, origin2, size2, q.angle, direction2, q.maxDistance, options));
            } else {
                if (all) PhysicsQueries2D::capsuleCastAll(scene, origin2, size2, capsuleDirection, q.angle, direction2, q.maxDistance, hits, options);
                else hits.push_back(PhysicsQueries2D::capsuleCast(scene, origin2, size2, capsuleDirection, q.angle, direction2, q.maxDistance, options));
            }
#endif
        } else {
#ifdef JASPERENGINE_PHYSICS_3D
            if (q.shape == "ray") {
                if (all) hits = Physics::raycastAll(scene, q.origin, q.direction, q.maxDistance, options);
                else hits.push_back(Physics::raycast(scene, q.origin, q.direction, q.maxDistance, options));
            } else if (q.shape == "sphere") {
                if (all) PhysicsQueries3D::sphereCastAll(scene, q.origin, q.radius, q.direction, q.maxDistance, hits, options);
                else hits.push_back(PhysicsQueries3D::sphereCast(scene, q.origin, q.radius, q.direction, q.maxDistance, options));
            } else if (q.shape == "box") {
                if (all) PhysicsQueries3D::boxCastAll(scene, q.origin, q.halfExtents, q.direction, q.rotation, q.maxDistance, hits, options);
                else hits.push_back(PhysicsQueries3D::boxCast(scene, q.origin, q.halfExtents, q.direction, q.rotation, q.maxDistance, options));
            } else {
                if (all) PhysicsQueries3D::capsuleCastAll(scene, q.pointA, q.pointB, q.radius, q.direction, q.maxDistance, hits, options);
                else hits.push_back(PhysicsQueries3D::capsuleCast(scene, q.pointA, q.pointB, q.radius, q.direction, q.maxDistance, options));
            }
#endif
        }
        hits.erase(std::remove_if(hits.begin(), hits.end(), [](const RayReturn& hit) { return !hit.hit; }), hits.end());
        any = !hits.empty();
    } else {
        // overlap and check share the overlap: a check is whether it found any
        if (q.is2D) {
#ifdef JASPERENGINE_PHYSICS_2D
            std::vector<ColliderRef2D> found;
            if (q.shape == "sphere") PhysicsQueries2D::overlapCircleAll(scene, origin2, q.radius, found, options);
            else if (q.shape == "box" && q.hasPoints) PhysicsQueries2D::overlapAreaAll(scene, Vector2(q.pointA.x, q.pointA.y), Vector2(q.pointB.x, q.pointB.y), found, options);
            else if (q.shape == "box") PhysicsQueries2D::overlapBoxAll(scene, origin2, size2, q.angle, found, options);
            else if (q.shape == "capsule") PhysicsQueries2D::overlapCapsuleAll(scene, origin2, size2, capsuleDirection, q.angle, found, options);
            else PhysicsQueries2D::overlapPointAll(scene, origin2, found, options);
            for (const ColliderRef2D& collider : found) colliders.push_back({collider.entity, collider.index});
#endif
        } else {
#ifdef JASPERENGINE_PHYSICS_3D
            std::vector<ColliderRef> found;
            if (q.shape == "sphere") PhysicsQueries3D::overlapSphere(scene, q.origin, q.radius, found, options);
            else if (q.shape == "box") PhysicsQueries3D::overlapBox(scene, q.origin, q.halfExtents, q.rotation, found, options);
            else if (q.shape == "capsule") PhysicsQueries3D::overlapCapsule(scene, q.pointA, q.pointB, q.radius, found, options);
            else PhysicsQueries3D::overlapPoint(scene, q.origin, found, options);
            for (const ColliderRef& collider : found) colliders.push_back({collider.entity, collider.index});
#endif
        }
        any = !colliders.empty();
    }

    // draw: the shape at its start (and, cast, at its end or first hit), the hits
    size_t drawn = 0;
    if (args.value("draw", false) && physics) {
        const float pointsToMeter = q.is2D ? physics->getPointsToMeterScale2D() : 1.0f;
        QueryLines shapeLinesOut, hitLines;
        if (q.shape != "ray") shapeLines(shapeLinesOut, q, Vector3::ZERO, pointsToMeter);
        if (cast) {
            const Vector3 direction = q.direction.normalized();
            const float reach = any ? hits.front().distance : q.maxDistance;
            shapeLinesOut.push_back({q.origin, q.origin + direction * reach});
            if (q.shape != "ray") shapeLines(shapeLinesOut, q, direction * reach, pointsToMeter);
            for (const RayReturn& hit : hits) {
                PhysicsDebugLines::crossMark(hit.point, 0.1f * pointsToMeter, [&](const Vector3& a, const Vector3& b) { hitLines.push_back({a, b}); });
                hitLines.push_back({hit.point, hit.point + hit.normal * (0.5f * pointsToMeter)});
            }
        }
        physics->addTimedDebugLines(shapeLinesOut, QUERY_SHAPE_COLOR, QUERY_DRAW_SECONDS);
        physics->addTimedDebugLines(hitLines, QUERY_HIT_COLOR, QUERY_DRAW_SECONDS);
        drawn = shapeLinesOut.size() + hitLines.size();
    }

    Json data = {{"dimension", q.is2D ? "2d" : "3d"}, {"shape", q.shape}, {"mode", q.mode}};
    std::string text;
    if (q.mode == "check") {
        data["hit"] = any;
        text = any ? "Something is there." : "Nothing is there.";
    } else if (cast) {
        Json list = Json::array();
        for (const RayReturn& hit : hits) {
            list.push_back({{"entity", hit.body}, {"name", McpRefs::entityPath(sp, hit.body)}, {"collider", hit.colliderIndex},
                            {"point", McpValues::toJson(hit.point)}, {"normal", McpValues::toJson(hit.normal)},
                            {"distance", McpValues::number(hit.distance)}});
        }
        data["hits"] = list;
        text = list.empty() ? std::string("No hit.")
                            : std::to_string(list.size()) + " hit(s), nearest " + list[0]["name"].get<std::string>() + " at " +
                                  std::to_string(hits.front().distance) + ".";
    } else {
        Json list = Json::array();
        for (const auto& [entity, index] : colliders) {
            list.push_back({{"entity", entity}, {"name", McpRefs::entityPath(sp, entity)}, {"collider", index}});
        }
        data["hits"] = list;
        text = list.empty() ? std::string("Nothing overlaps.") : std::to_string(list.size()) + " collider(s) overlap.";
    }
    if (drawn > 0) data["drawn_lines"] = drawn;
    call->ok(text, data);
}

} // namespace

void registerPhysicsQueryTools(McpRegistry& registry) {
    using namespace schema;
    const Json vector = Json{{"type", "array"}, {"items", Json{{"type", "number"}}}, {"minItems", 2}, {"maxItems", 3}};
    ToolDef tool;
    tool.name = "query_physics";
    tool.title = "Query Physics";
    tool.description =
        "Ask the running game's colliders (Play only): ray/sphere/box/capsule/point cast, cast_all, overlap or check, 3D or 2D "
        "(points; sphere = circle). Lua's Physics queries with their options. Hits: entity, collider, point, normal, distance. "
        "draw: shown 3 s in the Game view. Not raycast_meshes (drawn triangles).";
    tool.inputSchema = object({
        {"scene", sceneRef()},
        {"dimension", stringEnum("Default: the scene's.", {"2d", "3d"})},
        {"shape", stringEnum("", {"ray", "sphere", "box", "capsule", "point"})},
        {"mode", withDefault(stringEnum("", {"cast", "cast_all", "overlap", "check"}), "cast")},
        {"origin", vector},
        {"direction", vector},
        {"max_distance", number("")},
        {"radius", number("")},
        {"half_extents", vec3("3D box.")},
        {"size", vec2("2D box, capsule.")},
        {"angle", number("2D.")},
        {"rotation", quat("3D box.")},
        {"point_a", vector},
        {"point_b", vector},
        {"layer_mask", any("Number, layer names or numbers.")},
        {"ignore", entities()},
        {"triggers", withDefault(stringEnum("", {"use_global", "ignore", "collide"}), "use_global")},
        {"hit_from_inside", boolean("")},
        {"draw", boolean("")}
    }, {"shape"});
    tool.group = "physics";
    tool.annotations.readOnly = true;
    tool.annotations.idempotent = true;
    tool.handler = queryPhysicsTool;
    registry.addTool(std::move(tool));
}

}
