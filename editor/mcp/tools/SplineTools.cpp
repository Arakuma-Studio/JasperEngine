// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The spline tools (docs/specs/S18-splines.md R27, D12): edit_spline (knots, a stroke, type,
// closed), sample_spline (frames, uniform samples, closest points, signed distances; the Lua
// Spline API's numbers) and build_spline (presets, Extrude/Instantiate/terrain settings, Apply
// to Terrain). Edits go through the commands the editor uses (SplineEditCmd, the bake's
// TerrainMapPatchCmds), each call one undo step.
#include "McpTools.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "Catalog.h"
#include "Project.h"
#include "Stream.h"
#include "StreamSchema.h"
#include "reflect/Enums.h"
#include "reflect/Reflect.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/ComponentValuesCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/SplineEditCmd.h"
#include "object/spline/Spline.h"
#include "spline/SplineEditor.h"
#include "spline/SplinePresets.h"
#include "subsystem/SplineSystem.h"
#include "util/EntityClipboard.h"
#include "util/TerrainSplineBake.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

    // a component's YAML as its reflection table writes it (defaults left out)
    YAML::Node componentYaml(ComponentType type, const void* component) {
        return reflect::encode(*reflect::find(type), component, reflect::EncodeContext{});
    }

}

namespace {

constexpr size_t MAX_QUERIES = 4096;
constexpr size_t MAX_KNOTS = 10000;

bool has(const Json& args, const char* key) {
    return args.is_object() && args.contains(key) && !args[key].is_null();
}

// every push of the call grouped into one step; rolled back when a later part fails
class UndoStep {
public:
    explicit UndoStep(uint32_t sceneId): history(CommandHandle::get(sceneId)), start(history->mark()) {}
    bool add(Command* command) {
        command->setNoMerge();
        return history->addCommand(command);
    }
    void commit() { history->groupSince(start); }
    void rollback() {
        while (history->position() > start && history->canUndo()) history->undo();
    }

private:
    CommandHistory* history;
    size_t start;
};

Json vec(const Vector3& v) {
    return Json::array({v.x, v.y, v.z});
}

bool readVec3(const Json& value, Vector3& out) {
    return McpValues::parseVector3(value, out) && std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

Vector3 dirTo(const Matrix4& m, const Vector3& v) {
    return m * v - m * Vector3(0.0f, 0.0f, 0.0f);
}

float axisScale(const Matrix4& m) {
    return std::max(dirTo(m, Vector3(1, 0, 0)).length(), std::max(dirTo(m, Vector3(0, 1, 0)).length(), dirTo(m, Vector3(0, 0, 1)).length()));
}

const char* tangentModeName(spline::TangentMode mode) {
    return mode == spline::TangentMode::Aligned ? "aligned" : mode == spline::TangentMode::Broken ? "broken" : "mirrored";
}

bool parseTangentMode(const Json& v, spline::TangentMode& out) {
    if (!v.is_string()) return false;
    const std::string s = v.get<std::string>();
    if (s == "mirrored") out = spline::TangentMode::Mirrored;
    else if (s == "aligned") out = spline::TangentMode::Aligned;
    else if (s == "broken") out = spline::TangentMode::Broken;
    else return false;
    return true;
}

bool parseType(const Json& v, spline::SplineType& out) {
    if (!v.is_string()) return false;
    const std::string s = v.get<std::string>();
    if (s == "linear") out = spline::SplineType::Linear;
    else if (s == "catmull_rom") out = spline::SplineType::CatmullRom;
    else if (s == "bezier") out = spline::SplineType::CubicBezier;
    else return false;
    return true;
}

// a knot from [x,y,z] or {position, tangent_in, tangent_out, radius, roll, mode}, in `space`
// (world converts through the GameObject's transform); fields absent keep `base`'s
bool readKnot(const Json& v, bool world, const Matrix4& toLocal, spline::SplineKnot base, spline::SplineKnot& out, std::string& error) {
    out = base;
    Vector3 p;
    if (v.is_array()) {
        if (!readVec3(v, p)) { error = "a knot is [x, y, z] or {position, ...}"; return false; }
        out.position = world ? toLocal * p : p;
        return true;
    }
    if (!v.is_object()) { error = "a knot is [x, y, z] or {position, tangent_in, tangent_out, radius, roll, mode}"; return false; }
    const float scale = world ? axisScale(toLocal) : 1.0f;
    if (has(v, "position")) {
        if (!readVec3(v["position"], p)) { error = "position must be [x, y, z]"; return false; }
        out.position = world ? toLocal * p : p;
    }
    if (has(v, "tangent_in")) {
        if (!readVec3(v["tangent_in"], p)) { error = "tangent_in must be [x, y, z]"; return false; }
        out.tangentIn = world ? dirTo(toLocal, p) : p;
    }
    if (has(v, "tangent_out")) {
        if (!readVec3(v["tangent_out"], p)) { error = "tangent_out must be [x, y, z]"; return false; }
        out.tangentOut = world ? dirTo(toLocal, p) : p;
    }
    if (has(v, "radius")) {
        if (!v["radius"].is_number() || !(v["radius"].get<double>() >= 0.0)) { error = "radius must be a number >= 0"; return false; }
        out.radius = v["radius"].get<float>() * scale;
    }
    if (has(v, "roll")) {
        if (!v["roll"].is_number()) { error = "roll must be degrees"; return false; }
        out.rollDeg = v["roll"].get<float>();
    }
    if (has(v, "mode") && !parseTangentMode(v["mode"], out.mode)) { error = "mode is mirrored, aligned or broken"; return false; }
    return true;
}

Json knotJson(const spline::SplineKnot& k, bool world, const Matrix4& toWorld, bool bezier) {
    const float scale = world ? axisScale(toWorld) : 1.0f;
    Json j = {{"position", vec(world ? toWorld * k.position : k.position)}, {"radius", k.radius * scale}, {"roll", k.rollDeg}};
    if (bezier) {
        j["tangent_in"] = vec(world ? dirTo(toWorld, k.tangentIn) : k.tangentIn);
        j["tangent_out"] = vec(world ? dirTo(toWorld, k.tangentOut) : k.tangentOut);
        j["mode"] = tangentModeName(k.mode);
    }
    return j;
}

Json splineSummary(SceneProject* sp, Entity entity, bool world) {
    const SplineComponent& spline = sp->scene->getComponent<SplineComponent>(entity);
    const Transform* transform = sp->scene->findComponent<Transform>(entity);
    const Matrix4 toWorld = transform ? transform->modelMatrix : Matrix4();
    const bool bezier = spline.spline.type() == spline::SplineType::CubicBezier;
    Json knots = Json::array();
    for (const spline::SplineKnot& k : spline.spline.knots()) {
        knots.push_back(knotJson(k, world, toWorld, bezier));
    }
    static const char* types[] = {"linear", "catmull_rom", "bezier"};
    return {{"entity", McpRefs::entityJson(sp, entity)}, {"type", types[static_cast<int>(spline.spline.type())]},
            {"closed", spline.spline.closed()}, {"is_3d", spline.spline.upAxis().z == 0.0f}, {"space", world ? "world" : "local"},
            {"knots", knots}, {"length", Spline(sp->scene, entity).getLength()}};
}

Json frameJson(const spline::SplineFrame& f) {
    return {{"position", vec(f.position)}, {"forward", vec(f.forward)}, {"right", vec(f.right)}, {"up", vec(f.up)},
            {"radius", f.radius}, {"roll", f.rollDeg}};
}

SceneProject* sceneOf(const CallPtr& call, std::string& error) {
    const Json& args = call->args();
    return McpRefs::resolveScene(call->ctx().project(), has(args, "scene") ? args["scene"] : Json(), error);
}

// the spline GameObject: entity, else the selected or only one with a Spline
Entity splineEntityOf(SceneProject* sp, const Json& args, std::string& error) {
    if (has(args, "entity")) {
        const Entity entity = McpRefs::resolveEntity(sp, args["entity"], error);
        if (entity != NULL_ENTITY && !sp->scene->findComponent<SplineComponent>(entity)) {
            error = McpRefs::entityPath(sp, entity) + " has no Spline (add one with edit_spline create, or add_component spline).";
            return NULL_ENTITY;
        }
        return entity;
    }
    Entity found = NULL_ENTITY;
    for (Entity e : sp->entities) {
        if (sp->scene->findComponent<SplineComponent>(e)) {
            if (found != NULL_ENTITY) { error = "Several GameObjects have a Spline: pass entity."; return NULL_ENTITY; }
            found = e;
        }
    }
    if (found == NULL_ENTITY) error = "No GameObject has a Spline: pass entity, or create one.";
    return found;
}

// ------------------------------------------------------------------ edit_spline

void editSplineTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sp = sceneOf(call, error);
    if (!sp || !sp->scene) { call->fail(error.empty() ? "The scene is not loaded." : error); return; }
    if (sp->playState != ScenePlayState::STOPPED) {
        call->fail("The scene is playing: stop it to edit splines (Lua Spline:setKnotPosition edits the running game).");
        return;
    }
    const bool world = !has(args, "space") || args["space"] == "world";
    UndoStep step(sp->id);
    auto failCall = [&](const std::string& message) {
        step.rollback();
        call->fail(message);
    };

    Entity entity = NULL_ENTITY;
    if (has(args, "create")) {
        const Json& create = args["create"];
        if (!create.is_object()) { call->fail("create is {name, parent, dimension}"); return; }
        Entity parent = NULL_ENTITY;
        if (has(create, "parent")) {
            parent = McpRefs::resolveEntity(sp, create["parent"], error);
            if (parent == NULL_ENTITY) { call->fail(error); return; }
        }
        const std::string name = has(create, "name") && create["name"].is_string() ? create["name"].get<std::string>() : "Spline";
        CreateEntityCmd* cmd = new CreateEntityCmd(project, sp->id, name, EntityCreationType::SPLINE_EMPTY, parent);
        if (!step.add(cmd)) { failCall("Could not create the GameObject."); return; }
        entity = cmd->getEntity();
        if (has(create, "dimension")) {
            const std::string dimension = create["dimension"].is_string() ? create["dimension"].get<std::string>() : "";
            if (dimension != "2d" && dimension != "3d") { failCall("create.dimension is 2d or 3d"); return; }
            SplineEditState state = SplineEditCmd::capture(sp->scene->getComponent<SplineComponent>(entity));
            state.upAxis = dimension == "2d" ? Vector3(0, 0, 1) : Vector3(0, 1, 0);
            step.add(new SplineEditCmd(project, sp->id, entity, state, SplineEditKind::Edit));
        }
    } else {
        entity = splineEntityOf(sp, args, error);
        if (entity == NULL_ENTITY) { call->fail(error); return; }
    }

    const SplineComponent& spline = sp->scene->getComponent<SplineComponent>(entity);
    const Transform* transform = sp->scene->findComponent<Transform>(entity);
    const Matrix4 toWorld = transform ? transform->modelMatrix : Matrix4();
    const Matrix4 toLocal = toWorld.inverse();
    SplineEditState state = SplineEditCmd::capture(spline);
    SplineEditKind kind = SplineEditKind::Edit;
    bool edited = false;

    if (has(args, "type")) {
        if (!parseType(args["type"], state.type)) { failCall("type is linear, catmull_rom or bezier"); return; }
        edited = true;
    }
    if (has(args, "closed")) {
        if (!args["closed"].is_boolean()) { failCall("closed is true or false"); return; }
        state.closed = args["closed"].get<bool>();
        edited = true;
    }
    if (has(args, "set_knots")) {
        const Json& list = args["set_knots"];
        if (!list.is_array() || list.size() > MAX_KNOTS) { failCall("set_knots is a list of knots"); return; }
        state.knots.clear();
        for (size_t i = 0; i < list.size(); i++) {
            spline::SplineKnot knot;
            if (!readKnot(list[i], world, toLocal, i > 0 ? state.knots.back() : spline::SplineKnot(), knot, error)) {
                failCall("set_knots[" + std::to_string(i) + "]: " + error);
                return;
            }
            if (i > 0 && list[i].is_array()) { knot.tangentIn = Vector3(0, 0, 0); knot.tangentOut = Vector3(0, 0, 0); }
            state.knots.push_back(knot);
        }
        edited = true;
    }
    if (has(args, "remove")) {
        const Json& list = args["remove"];
        if (!list.is_array()) { failCall("remove is a list of knot indices"); return; }
        std::vector<size_t> indices;
        for (const Json& v : list) {
            if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<size_t>() >= state.knots.size()) {
                failCall("remove: " + v.dump() + " is not a knot index (0-" + std::to_string(state.knots.size()) + ")");
                return;
            }
            indices.push_back(v.get<size_t>());
        }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        if (state.knots.size() - indices.size() < spline::SplineData::MIN_KNOTS) {
            failCall("A spline keeps two knots at least.");
            return;
        }
        for (auto it = indices.rbegin(); it != indices.rend(); ++it) state.knots.erase(state.knots.begin() + static_cast<std::ptrdiff_t>(*it));
        kind = SplineEditKind::DeleteKnots;
        edited = true;
    }
    if (has(args, "insert")) {
        const Json& list = args["insert"];
        if (!list.is_array()) { failCall("insert is [{index, knot}]"); return; }
        for (const Json& item : list) {
            if (!item.is_object() || !has(item, "index") || !item["index"].is_number_integer() || !has(item, "knot")) {
                failCall("insert items are {index, knot}");
                return;
            }
            const size_t index = std::min<size_t>(item["index"].get<size_t>(), state.knots.size());
            spline::SplineKnot base = state.knots.empty() ? spline::SplineKnot() : state.knots[std::min(index, state.knots.size() - 1)];
            base.tangentIn = Vector3(0, 0, 0);
            base.tangentOut = Vector3(0, 0, 0);
            spline::SplineKnot knot;
            if (!readKnot(item["knot"], world, toLocal, base, knot, error)) { failCall("insert: " + error); return; }
            state.knots.insert(state.knots.begin() + static_cast<std::ptrdiff_t>(index), knot);
        }
        kind = SplineEditKind::InsertKnot;
        edited = true;
    }
    if (has(args, "update")) {
        const Json& list = args["update"];
        if (!list.is_array()) { failCall("update is [{index, position, radius, ...}]"); return; }
        for (const Json& item : list) {
            if (!item.is_object() || !has(item, "index") || !item["index"].is_number_integer() || item["index"].get<long long>() < 0 ||
                item["index"].get<size_t>() >= state.knots.size()) {
                failCall("update items are {index, ...} with an index of a knot (0-" + std::to_string(state.knots.size()) + ")");
                return;
            }
            const size_t index = item["index"].get<size_t>();
            spline::SplineKnot knot;
            if (!readKnot(item, world, toLocal, state.knots[index], knot, error)) { failCall("update: " + error); return; }
            if (has(item, "tangent_out") && !has(item, "tangent_in")) spline::applyTangentMode(knot, spline::SplineHandle::Out);
            else if (has(item, "tangent_in") && !has(item, "tangent_out")) spline::applyTangentMode(knot, spline::SplineHandle::In);
            state.knots[index] = knot;
        }
        if (kind == SplineEditKind::Edit) kind = SplineEditKind::MoveKnots;
        edited = true;
    }
    if (edited) {
        step.add(new SplineEditCmd(project, sp->id, entity, std::move(state), kind));
    }
    if (has(args, "stroke")) {
        const Json& stroke = args["stroke"];
        if (!stroke.is_object() || !has(stroke, "points") || !stroke["points"].is_array()) { failCall("stroke is {points, ...}"); return; }
        std::vector<Vector3> points;
        for (const Json& v : stroke["points"]) {
            Vector3 p;
            if (!readVec3(v, p)) { failCall("stroke.points are [x, y, z]"); return; }
            points.push_back(world ? p : toWorld * p);
        }
        const bool is2D = sp->scene->getComponent<SplineComponent>(entity).spline.upAxis().z != 0.0f;
        const float tolerance = has(stroke, "tolerance") && stroke["tolerance"].is_number() ? stroke["tolerance"].get<float>() : (is2D ? 0.05f : 0.5f);
        const unsigned smooth = has(stroke, "smooth") && stroke["smooth"].is_number_integer() ? stroke["smooth"].get<unsigned>() : 2u;
        SplineSnap snap = SplineSnap::None;
        if (has(stroke, "snap")) {
            const std::string s = stroke["snap"].is_string() ? stroke["snap"].get<std::string>() : "";
            if (s == "terrain") snap = SplineSnap::Terrain;
            else if (s == "surface") snap = SplineSnap::Surface;
            else if (s != "none") { failCall("stroke.snap is none, terrain or surface"); return; }
        }
        const bool extend = !has(stroke, "extend") || (stroke["extend"].is_boolean() && stroke["extend"].get<bool>());
        if (!SplineEditor::applyStroke(project, sp->id, entity, points, tolerance, smooth, snap, extend, std::max(tolerance * 2.0f, 1.0f), &error)) {
            failCall("stroke: " + error);
            return;
        }
    }
    step.commit();
    if (std::shared_ptr<SplineSystem> system = sp->scene->findSystem<SplineSystem>()) {
        system->rebuildNow(entity);
    }
    Json result = splineSummary(sp, entity, world);
    call->ok("Spline " + McpRefs::entityPath(sp, entity) + ": " + std::to_string(result["knots"].size()) + " knots, length " +
             std::to_string(result["length"].get<float>()) + ".", result);
}

// ------------------------------------------------------------------ sample_spline

void sampleSplineTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sp = sceneOf(call, error);
    if (!sp || !sp->scene) { call->fail(error.empty() ? "The scene is not loaded." : error); return; }
    // reads follow the running game
    sp = McpRefs::runtimeScene(project, sp);
    const Entity entity = splineEntityOf(sp, args, error);
    if (entity == NULL_ENTITY) { call->fail(error); return; }
    const Spline spline(sp->scene, entity);
    Json result = {{"entity", McpRefs::entityJson(sp, entity)}, {"length", spline.getLength()}};
    auto numbers = [&](const char* key, std::vector<float>& out) {
        if (!has(args, key)) return true;
        if (!args[key].is_array() || args[key].size() > MAX_QUERIES) { error = std::string(key) + " is a list of numbers"; return false; }
        for (const Json& v : args[key]) {
            if (!v.is_number()) { error = std::string(key) + " is a list of numbers"; return false; }
            out.push_back(v.get<float>());
        }
        return true;
    };
    std::vector<float> ts, distances;
    if (!numbers("t", ts) || !numbers("distance", distances)) { call->fail(error); return; }
    if (!ts.empty()) {
        Json frames = Json::array();
        for (float t : ts) frames.push_back(frameJson(spline.evaluate(t)));
        result["at_t"] = frames;
    }
    if (!distances.empty()) {
        Json frames = Json::array();
        for (float d : distances) frames.push_back(frameJson(spline.evaluateAtDistance(d)));
        result["at_distance"] = frames;
    }
    if (has(args, "count")) {
        if (!args["count"].is_number_integer() || args["count"].get<long long>() < 2 || args["count"].get<size_t>() > MAX_QUERIES) {
            call->fail("count is 2-" + std::to_string(MAX_QUERIES));
            return;
        }
        Json frames = Json::array();
        for (const spline::SplineFrame& f : spline.sampleUniform(args["count"].get<unsigned>())) frames.push_back(frameJson(f));
        result["samples"] = frames;
    }
    if (has(args, "query_points")) {
        const Json& list = args["query_points"];
        if (!list.is_array() || list.size() > MAX_QUERIES) { call->fail("query_points is a list of [x, y, z]"); return; }
        Json queries = Json::array();
        for (const Json& v : list) {
            Vector3 p;
            if (!readVec3(v, p)) { call->fail("query_points are [x, y, z]"); return; }
            const spline::SplineHit hit = spline.getClosestPoint(p);
            queries.push_back({{"closest", {{"t", hit.t}, {"distance", hit.distance}, {"position", vec(hit.position)}, {"segment", hit.segment}}},
                               {"signed_distance", spline.getSignedDistance(p)},
                               {"signed_distance_xz", spline.getSignedDistanceXZ(p.x, p.z)},
                               {"signed_distance_band_xz", spline.getSignedDistanceBandXZ(p.x, p.z)}});
        }
        result["queries"] = queries;
    }
    call->ok("Spline " + McpRefs::entityPath(sp, entity) + ": length " + std::to_string(spline.getLength()) + ".", result);
}

// ------------------------------------------------------------------ build_spline

// the component's encode with the given (camelCase) keys replaced, as one ComponentValuesCmd
bool setComponentValues(Project* project, SceneProject* sp, Entity entity, ComponentType type, const Json& values, UndoStep& step,
                        std::string& error) {
    YAML::Node node = EntityClipboard::encodeComponent(sp->scene, entity, type);
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!StreamSchema::isKnownFieldKey(type, it.key())) {
            error = "\"" + it.key() + "\" is not a field of " + Catalog::getComponentName(type, true) + " (see jasper://docs/splines)";
            return false;
        }
        node[it.key()] = McpValues::jsonToYaml(it.value());
    }
    auto clip = std::make_shared<ComponentClip>();
    clip->type = type;
    clip->entity = entity;
    clip->sceneId = sp->id;
    clip->value = node;
    step.add(new ComponentValuesCmd(project, sp->id, {entity}, type, clip));
    return true;
}

bool ensureComponent(Project* project, SceneProject* sp, Entity entity, ComponentType type, UndoStep& step) {
    if (McpRefs::hasComponent(sp, entity, type)) return true;
    return step.add(new AddComponentCmd(project, sp->id, entity, type));
}

void buildSplineTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sp = sceneOf(call, error);
    if (!sp || !sp->scene) { call->fail(error.empty() ? "The scene is not loaded." : error); return; }
    if (sp->playState != ScenePlayState::STOPPED) { call->fail("The scene is playing: stop it to build splines."); return; }
    const Entity entity = splineEntityOf(sp, args, error);
    if (entity == NULL_ENTITY) { call->fail(error); return; }
    Scene* scene = sp->scene;
    UndoStep step(sp->id);
    auto failCall = [&](const std::string& message) {
        step.rollback();
        call->fail(message);
    };

    if (has(args, "preset")) {
        SplinePreset preset;
        if (!args["preset"].is_string() || !SplinePresets::fromName(args["preset"].get<std::string>(), preset)) {
            std::string names;
            for (const std::string& n : SplinePresets::names()) names += (names.empty() ? "" : ", ") + n;
            call->fail("preset is one of: " + names);
            return;
        }
        const SplinePresetSetup setup = SplinePresets::make(project, preset, sp->sceneType == SceneType::SCENE_2D);
        // the spline's own knots stay; its terrain settings, up axis and an empty spline's shape are the preset's
        const SplineComponent& current = scene->getComponent<SplineComponent>(entity);
        YAML::Node splineNode = componentYaml(ComponentType::SplineComponent, &setup.spline);
        if (current.spline.knotCount() > 0) {
            const YAML::Node own = componentYaml(ComponentType::SplineComponent, &current);
            splineNode["knots"] = own["knots"];
            splineNode["closed"] = current.spline.closed();
            splineNode["type"] = own["type"] ? own["type"] : YAML::Node(reflect::enumToken(reflect::enums::kSplineType, static_cast<int>(current.spline.type())));
        }
        Json splineValues = McpValues::yamlToJson(splineNode);
        if (!setComponentValues(project, sp, entity, ComponentType::SplineComponent, splineValues, step, error)) { failCall(error); return; }
        if (setup.hasExtrude) {
            if (!ensureComponent(project, sp, entity, ComponentType::MeshComponent, step) ||
                !ensureComponent(project, sp, entity, ComponentType::SplineExtrudeComponent, step)) {
                failCall("Could not add the Spline Extrude.");
                return;
            }
            MeshComponent mesh;
            mesh.numSubmeshes = 1;
            mesh.submeshes[0].material = setup.material;
            mesh.castShadows = setup.castShadows;
            mesh.receiveShadows = setup.receiveShadows;
            auto clip = std::make_shared<ComponentClip>();
            clip->type = ComponentType::MeshComponent;
            clip->entity = entity;
            clip->sceneId = sp->id;
            clip->value = Stream::encodeMeshComponent(mesh, false, true, false);
            step.add(new ComponentValuesCmd(project, sp->id, {entity}, ComponentType::MeshComponent, clip));
            if (!setComponentValues(project, sp, entity, ComponentType::SplineExtrudeComponent,
                                    McpValues::yamlToJson(componentYaml(ComponentType::SplineExtrudeComponent, &setup.extrude)), step, error)) {
                failCall(error);
                return;
            }
        }
        if (setup.hasInstantiate) {
            if (!ensureComponent(project, sp, entity, ComponentType::SplineInstantiateComponent, step) ||
                !setComponentValues(project, sp, entity, ComponentType::SplineInstantiateComponent,
                                    McpValues::yamlToJson(componentYaml(ComponentType::SplineInstantiateComponent, &setup.instantiate)), step, error)) {
                failCall(error.empty() ? "Could not add the Spline Instantiate." : error);
                return;
            }
        }
    }
    if (has(args, "extrude")) {
        if (!args["extrude"].is_object()) { failCall("extrude is {field: value} (Spline Extrude's fields)"); return; }
        if (!ensureComponent(project, sp, entity, ComponentType::MeshComponent, step) ||
            !ensureComponent(project, sp, entity, ComponentType::SplineExtrudeComponent, step) ||
            !setComponentValues(project, sp, entity, ComponentType::SplineExtrudeComponent, args["extrude"], step, error)) {
            failCall("extrude: " + error);
            return;
        }
    }
    if (has(args, "instantiate")) {
        if (!args["instantiate"].is_object()) { failCall("instantiate is {field: value} (Spline Instantiate's fields)"); return; }
        if (!ensureComponent(project, sp, entity, ComponentType::SplineInstantiateComponent, step) ||
            !setComponentValues(project, sp, entity, ComponentType::SplineInstantiateComponent, args["instantiate"], step, error)) {
            failCall("instantiate: " + error);
            return;
        }
    }
    if (has(args, "terrain")) {
        if (!args["terrain"].is_object()) { failCall("terrain is {field: value} (the Spline's terrain settings)"); return; }
        Json values = McpValues::yamlToJson(componentYaml(ComponentType::SplineComponent, &scene->getComponent<SplineComponent>(entity)));
        Json terrain = values.contains("terrain") && values["terrain"].is_object() ? values["terrain"] : Json::object();
        for (auto it = args["terrain"].begin(); it != args["terrain"].end(); ++it) terrain[it.key()] = it.value();
        if (!setComponentValues(project, sp, entity, ComponentType::SplineComponent, Json{{"terrain", terrain}}, step, error)) {
            failCall("terrain: " + error);
            return;
        }
    }

    std::shared_ptr<SplineSystem> system = scene->findSystem<SplineSystem>();
    if (system) system->rebuildNow(entity);

    Json result = {{"entity", McpRefs::entityJson(sp, entity)}};
    if (has(args, "apply_terrain") && args["apply_terrain"].is_boolean() && args["apply_terrain"].get<bool>()) {
        TerrainBakeChanges changes;
        if (!TerrainSplineBake::apply(project, sp->id, entity, changes, error)) { failCall("apply_terrain: " + error); return; }
        auto rect = [](const TerrainMapRegion& r) {
            return r.empty() ? Json(nullptr) : Json{{"x", r.minX}, {"y", r.minY}, {"width", r.width()}, {"height", r.height()}};
        };
        result["terrain"] = {{"heights", rect(changes.heights)}, {"blend", rect(changes.blend)}, {"warnings", changes.warnings}};
        // the consumers conform to the baked terrain
        if (system) system->rebuildNow(entity);
    }
    step.commit();

    if (system) {
        auto warnings = [](const std::vector<SplineWarning>& list) {
            Json out = Json::array();
            for (const SplineWarning& w : list) {
                out.push_back(w.knot >= 0 ? Json{{"knot", w.knot}, {"message", w.message}} : Json{{"message", w.message}});
            }
            return out;
        };
        if (scene->findComponent<SplineExtrudeComponent>(entity)) {
            const SplineBuildStats stats = system->getExtrudeStats(entity);
            result["extrude"] = {{"vertices", stats.vertices}, {"triangles", stats.triangles}, {"chunks", stats.chunks},
                                 {"length", stats.length}, {"warnings", warnings(system->getExtrudeWarnings(entity))}};
        }
        if (scene->findComponent<SplineInstantiateComponent>(entity)) {
            const SplineBuildStats stats = system->getInstantiateStats(entity);
            result["instantiate"] = {{"instances", stats.instances}, {"chunks", stats.chunks},
                                     {"warnings", warnings(system->getInstantiateWarnings(entity))}};
        }
    }
    call->ok("Built " + McpRefs::entityPath(sp, entity) + ".", result);
}

}  // namespace

void registerSplineTools(McpRegistry& registry) {
    using namespace schema;
    const Json numberItem = Json{{"type", "number"}};
    const Json knotItem = Json{{"type", {"array", "object"}}};

    ToolDef edit;
    edit.name = "edit_spline";
    edit.title = "Edit Spline";
    edit.group = "splines";
    edit.attributesUndo = true;
    edit.allowedWhilePlaying = false;
    edit.description =
        "Creates or edits a Spline, one undo step. Order: create, type, closed, set_knots, remove, insert, update, stroke "
        "(freehand points: simplified, smoothed, snapped; starting at an end extends it). Knot: [x,y,z] or {position, "
        "tangent_in, tangent_out, radius, roll, mode}; indices 0-based. Returns knots and length. jasper://docs/splines";
    edit.inputSchema = object({
        {"scene", sceneRef()},
        {"entity", entityRef()},
        {"create", object({{"name", string("")}, {"parent", entityRef()}, {"dimension", stringEnum("", {"2d", "3d"})}})},
        {"space", withDefault(stringEnum("", {"world", "local"}), "world")},
        {"type", stringEnum("", {"linear", "catmull_rom", "bezier"})},
        {"closed", boolean("")},
        {"set_knots", array(knotItem, "")},
        {"insert", array(object({{"index", integer("")}, {"knot", knotItem}}), "")},
        {"update", array(Json{{"type", "object"}}, "{index, ...knot fields}")},
        {"remove", array(Json{{"type", "integer"}}, "")},
        {"stroke", object({{"points", array(array(numberItem, ""), "")}, {"tolerance", number("")}, {"smooth", integer("")},
                           {"snap", stringEnum("", {"none", "terrain", "surface"})}, {"extend", boolean("")}})}
    });
    edit.handler = editSplineTool;
    registry.addTool(std::move(edit));

    ToolDef sample;
    sample.name = "sample_spline";
    sample.title = "Sample Spline";
    sample.group = "splines";
    sample.annotations.readOnly = true;
    sample.annotations.idempotent = true;
    sample.description =
        "Reads a Spline in world space (the Lua Spline API's numbers; the running game in Play): length, frames at t (0-1) "
        "and distance, count uniform samples, and per query point the closest point and signed distances (3D, XZ, XZ band).";
    sample.inputSchema = object({
        {"scene", sceneRef()},
        {"entity", entityRef()},
        {"t", array(numberItem, "")},
        {"distance", array(numberItem, "")},
        {"count", integer("")},
        {"query_points", array(array(numberItem, ""), "")}
    });
    sample.handler = sampleSplineTool;
    registry.addTool(std::move(sample));

    ToolDef build;
    build.name = "build_spline";
    build.title = "Build Spline";
    build.group = "splines";
    build.attributesUndo = true;
    build.allowedWhilePlaying = false;
    build.description =
        "Configures what a Spline builds, one undo step: preset, then extrude / instantiate / terrain values (their scene-file "
        "fields, e.g. {shape: crown, conform: terrain_height}), then apply_terrain bakes the terrain (flatten, paint, clear "
        "foliage). Returns vertices, chunks, instances, the terrain rects changed and warnings. Other fields: set_properties.";
    build.inputSchema = object({
        {"scene", sceneRef()},
        {"entity", entityRef()},
        {"preset", stringEnum("", {"road", "path", "wall", "fence", "river", "lake", "circle", "square", "scatter", "edge2d", "fill2d",
                                   "circle2d", "empty"})},
        {"extrude", Json{{"type", "object"}}},
        {"instantiate", Json{{"type", "object"}}},
        {"terrain", Json{{"type", "object"}}},
        {"apply_terrain", boolean("")}
    });
    build.handler = buildSplineTool;
    registry.addTool(std::move(build));
}

}
