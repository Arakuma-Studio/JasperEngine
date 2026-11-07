// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// edit_terrain and visualize_terrain (group terrain, T1 R68), as T5 needs them: this is the smallest
// form that carries T5's operations (T5 Deviation 2). T1's rest adds its built-in ops, create and
// its overlays here; the ops T5 registers, their results and the job path stay what they are.

#include "FoliageTools.h"
#include "McpTools.h"

#include "foliage/FoliageOpRunner.h"
#include "component/TerrainComponent.h"
#include "window/TerrainEditWindow.h"
#include "App.h"
#include "Backend.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpJson.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "subsystem/FoliageSystem.h"
#include "terrain/TerrainOps.h"
#include "terrain/TerrainShape.h"
#include "util/ImpostorBaker.h"

#include <functional>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;
using namespace jasperengine::terrain;

namespace {

    OpParam paramFromJson(const Json& value) {
        OpParam param;
        if (value.is_boolean()) {
            param.kind = OpParam::Kind::Bool;
            param.boolean = value.get<bool>();
        } else if (value.is_number()) {
            param.kind = OpParam::Kind::Number;
            param.number = value.get<double>();
        } else if (value.is_string()) {
            param.kind = OpParam::Kind::String;
            param.text = value.get<std::string>();
        } else if (value.is_array()) {
            param.kind = OpParam::Kind::List;
            for (const Json& item : value) param.items.push_back(paramFromJson(item));
        } else if (value.is_object()) {
            param.kind = OpParam::Kind::Object;
            for (auto it = value.begin(); it != value.end(); ++it) param.fields.emplace_back(it.key(), paramFromJson(it.value()));
        }
        return param;
    }

    Json paramToJson(const OpParam& param) {
        switch (param.kind) {
            case OpParam::Kind::Bool: return param.boolean;
            case OpParam::Kind::Number: return param.number;
            case OpParam::Kind::String: return param.text;
            case OpParam::Kind::List: {
                Json list = Json::array();
                for (const OpParam& item : param.items) list.push_back(paramToJson(item));
                return list;
            }
            case OpParam::Kind::Object: {
                Json object = Json::object();
                for (const auto& [key, value] : param.fields) object[key] = paramToJson(value);
                return object;
            }
            case OpParam::Kind::None: break;
        }
        return nullptr;
    }

    // T1's shape, in world or local XZ: all, circle or rect are what T5's ops take here
    // toLocal: a world point to the Terrain's local space, or null for points already local
    bool shapeFromJson(const Json& value, const std::function<Vector3(const Vector3&)>& toLocal, Shape& shape, std::string& error) {
        if (!value.is_object()) {
            error = "shape must be {type, ...}: all, circle {center, radius}, rect {min, max}, polyline {points, radius}.";
            return false;
        }
        const auto point = [&](const Json& json, Vector2& out) {
            if (!json.is_array() || json.size() != 2 || !json[0].is_number() || !json[1].is_number()) return false;
            const Vector3 given(json[0].get<float>(), 0.0f, json[1].get<float>());
            const Vector3 local = toLocal ? toLocal(given) : given;
            out = Vector2(local.x, local.z);
            return true;
        };
        const std::string type = value.value("type", std::string("all"));
        if (type == "all") {
            shape.type = ShapeType::All;
        } else if (type == "circle") {
            shape.type = ShapeType::Circle;
            if (!value.contains("center") || !point(value["center"], shape.center)) {
                error = "shape.center expects [x, z].";
                return false;
            }
            shape.radius = value.value("radius", 0.0f);
            if (!(shape.radius > 0.0f)) {
                error = "shape.radius must be above 0.";
                return false;
            }
        } else if (type == "rect") {
            shape.type = ShapeType::Rect;
            if (!value.contains("min") || !value.contains("max") || !point(value["min"], shape.min) || !point(value["max"], shape.max)) {
                error = "shape.min and shape.max expect [x, z].";
                return false;
            }
        } else if (type == "polyline") {
            shape.type = ShapeType::Polyline;
            shape.radius = value.value("radius", 0.0f);
            if (!value.contains("points") || !value["points"].is_array()) {
                error = "shape.points expects [[x, z], ...].";
                return false;
            }
            for (const Json& item : value["points"]) {
                Vector2 p;
                if (!point(item, p)) {
                    error = "shape.points expects [[x, z], ...].";
                    return false;
                }
                shape.points.push_back(p);
            }
        } else {
            error = "shape.type: valid here are all, circle, rect, polyline.";
            return false;
        }
        return true;
    }

    Json entryJson(const OpEntry& entry) {
        Json out = {{"op", entry.op}, {"changed_texels", entry.changedTexels}};
        if (entry.seed) out["seed"] = *entry.seed;
        for (const auto& [key, value] : entry.fields) out[key] = paramToJson(value);
        return out;
    }

    const char* mapName(MapKind kind) {
        switch (kind) {
            case MapKind::Height: return "height";
            case MapKind::Blend: return "blend";
            case MapKind::Hole: return "hole";
            case MapKind::Density: return "density";
        }
        return "";
    }

    Json outcomeJson(const FoliageOpRunner::Outcome& outcome, const std::string& undoName, bool dryRun) {
        Json data = {{"dry_run", dryRun}, {"undo_name", undoName}};
        Json ops = Json::array();
        for (const OpEntry& entry : outcome.result.ops) ops.push_back(entryJson(entry));
        data["ops"] = ops;
        Json changed = Json::array();
        for (const MapChangeReport& change : outcome.result.changed) {
            std::string name = mapName(change.map.kind);
            if (change.map.kind == MapKind::Density || change.map.kind == MapKind::Blend) name += ":" + std::to_string(change.map.index);
            changed.push_back({{"map", name}, {"rect", {change.rect.x, change.rect.y, change.rect.w, change.rect.h}},
                               {"local", {{"min", {change.localMin.x, change.localMin.y}}, {"max", {change.localMax.x, change.localMax.y}}}},
                               {"texels", change.texels}});
        }
        data["changed"] = changed;
        Json warnings = Json::array();
        for (const OpWarning& warning : outcome.result.warnings) {
            warnings.push_back({{"op", warning.op}, {"code", warning.code}, {"message", warning.message}});
        }
        data["warnings"] = warnings;
        data["trees"] = outcome.treesAfter;

        // R67: hints an agent must see go in the result
        Json next = Json::array();
        const FoliageTierLimits low = foliageTierLimits(FoliageTier::Low);
        if (outcome.treesAfter > static_cast<int64_t>(low.maxVisibleTrees)) {
            next.push_back("The Terrain has " + std::to_string(outcome.treesAfter) + " trees, past the Mobile budget of " + std::to_string(low.maxVisibleTrees) +
                           " visible: bake_impostors, and check sample_terrain foliage on a Low tier.");
        }
        if (!next.empty()) data["next"] = next;
        return data;
    }

    void editTerrainTool(const CallPtr& call) {
        TerrainTarget target;
        if (!resolveTerrainTarget(call, target)) return;
        if (target.scene->playState != ScenePlayState::STOPPED) {
            call->fail("edit_terrain edits the scene: stop Play first.");
            return;
        }
        const Json& args = call->args();
        Scene* scene = target.engineScene();
        const bool world = args.value("space", std::string("world")) != "local";
        std::function<Vector3(const Vector3&)> toLocal;
        if (world) toLocal = [scene, entity = target.entity](const Vector3& point) { return FoliageOpRunner::toTerrainLocal(scene, entity, point); };

        if (!args.contains("ops") || !args["ops"].is_array() || args["ops"].empty()) {
            call->fail("ops must be a non-empty array of {op, ...}. Ops and fields: jasper://docs/terrain.");
            return;
        }

        std::vector<Op> ops;
        bool spansFrames = false;
        for (size_t i = 0; i < args["ops"].size(); i++) {
            const Json& item = args["ops"][i];
            const std::string index = "ops[" + std::to_string(i) + "]";
            if (!item.is_object() || !item.contains("op") || !item["op"].is_string()) {
                call->fail(index + ".op: every entry names its op.");
                return;
            }
            const std::string name = item["op"].get<std::string>();
            if (!registeredOpExists(name)) {
                call->fail(index + ".op: \"" + name + "\" is not available in edit_terrain here; valid: scatter_by_rules, clear_foliage, place_trees, remove_trees, convert_placed_to_trees, bake_impostors. Height and paint edits: sculpt_terrain.");
                return;
            }
            spansFrames = spansFrames || registeredOpSpansFrames(name);
            Op op;
            op.type = OpType::Registered;
            op.registeredName = name;
            op.params.kind = OpParam::Kind::Object;
            for (auto it = item.begin(); it != item.end(); ++it) {
                if (it.key() == "op") continue;
                if (it.key() == "shape") {
                    std::string error;
                    if (!shapeFromJson(it.value(), toLocal, op.shape, error)) {
                        call->fail(index + "." + error);
                        return;
                    }
                    continue;
                }
                if (it.key() == "seed") {
                    op.seed = it.value().get<uint32_t>();
                    continue;
                }
                op.params.fields.emplace_back(it.key(), paramFromJson(it.value()));
            }
            ops.push_back(std::move(op));
        }

        // A layer just added has no density map: scatter_by_rules on it starts from an empty one. In
        // memory only, so a dry run writes nothing either.
        if (!spansFrames) {
            TerrainEditWindow* window = Backend::getApp().getTerrainEditWindow();
            TerrainComponent& component = scene->getComponent<TerrainComponent>(target.entity);
            for (const Json& item : args["ops"]) {
                if (!item.contains("target") || !item["target"].is_object() || !item["target"].contains("detail_layer") || !item["target"]["detail_layer"].is_number_integer()) continue;
                const int layer = item["target"]["detail_layer"].get<int>();
                if (window && layer >= 0 && layer < static_cast<int>(component.foliageLayers.size()) && component.foliageLayers[layer].densityMap.empty()) {
                    window->ensureDensityMap(target.project, target.scene, target.entity, layer);
                }
            }
        }

        if (spansFrames) {
            bakeImpostorsFromOps(call, target, ops, args.value("dry_run", false));
            return;
        }

        FoliageOpRunner::Request request;
        request.sceneId = target.scene->id;
        request.terrain = target.entity;
        request.ops = std::move(ops);
        request.dryRun = args.value("dry_run", false);
        const std::string terrainName = scene->getEntityName(target.entity);
        const std::string& first = request.ops.front().registeredName;
        request.undoName = std::string(first == "scatter_by_rules" ? "Scatter Foliage" : first == "clear_foliage" ? "Clear Foliage" : first == "convert_placed_to_trees" ? "Convert Placed Instances" : first == "remove_trees" ? "Erase Trees" : "Paint Trees") + " (" + terrainName + ")";
        const std::string undoName = request.undoName;
        const bool dryRun = request.dryRun;

        std::shared_ptr<McpJob> job = call->ctx().jobs().create("edit_terrain", undoName);
        std::shared_ptr<FoliageOpRunner> runner = FoliageOpRunner::start(target.project, std::move(request), [job, undoName, dryRun](const FoliageOpRunner::Outcome& outcome) {
            Json data = outcomeJson(outcome, undoName, dryRun);
            if (!outcome.error.empty()) {
                job->fail(outcome.error, data);
            } else if (outcome.cancelled) {
                job->markCancelled("Cancelled; nothing changed.");
            } else {
                job->succeed((dryRun ? std::string("Dry run of ") : std::string()) + undoName + ": " + std::to_string(outcome.treesAfter) + " trees.", data);
            }
        });
        std::weak_ptr<FoliageOpRunner> weak = runner;
        job->setCancelHandler([weak]() { if (auto alive = weak.lock()) alive->cancel(); });
        call->detach();
        replyWhenJobDone(call, job, args.value("wait", true), args.value("timeout_seconds", 120.0), [](const McpJob& finished) {
            Json result = finished.result();
            result["job_id"] = finished.id();
            result["status"] = jobStatusName(finished.status());
            if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
            return ToolResult::success(finished.message(), result);
        });
    }

    void visualizeTerrainTool(const CallPtr& call) {
        TerrainTarget target;
        if (!resolveTerrainTarget(call, target)) return;
        visualizeFoliage(call, target);
    }

}

namespace jasperengine::editor::mcp {

void registerTerrainTools(McpRegistry& registry) {
    using namespace schema;
    registerFoliageOps();
    // T5 R56: editor only, so Lua's applyOps does not know it
    ImpostorBaker::registerOp();

    {
        ToolDef tool;
        tool.name = "edit_terrain";
        tool.title = "Edit Terrain";
        tool.description =
            "Declarative terrain edits, all-or-nothing, one undo step per call, no rates: absolute values, seeded. dry_run changes "
            "nothing and returns the same result. Big runs are jobs (get_job). For a look use visualize_terrain, for numbers "
            "sample_terrain. Ops and fields: jasper://docs/terrain.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef("Default: the only terrain.")},
            {"space", withDefault(stringEnum("", {"world", "local"}), "world")},
            {"ops", Json{{"type", "array"}, {"maxItems", 256}, {"items", Json{{"type", "object"},
                {"properties", {{"op", string("scatter_by_rules, clear_foliage, place_trees, remove_trees, bake_impostors...")},
                                {"shape", anyObject("all, circle, rect or polyline.")}}},
                {"required", Json::array({Json("op")})}}}}},
            {"dry_run", withDefault(boolean(""), false)},
            {"wait", withDefault(boolean("A job waits for its end."), true)},
            {"timeout_seconds", number("", 0, 3600)}
        }, {"ops"});
        tool.handler = editTerrainTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "visualize_terrain";
        tool.title = "Visualize Terrain";
        tool.annotations.readOnly = true;
        tool.description =
            "Show a terrain overlay in the Scene view (none hides it), frame the terrain and return a Scene view PNG with the "
            "overlay's legend and the camera. The view stays as set; no project data changes. Overlay names: jasper://docs/terrain.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef("Default: the only terrain.")},
            {"overlay", string("none, density, tree_density...")},
            {"layer", integer("", 0, 64)},
            {"opacity", withDefault(number("", 0, 1), 0.7)},
            {"framing", withDefault(stringEnum("", {"keep", "fit", "top"}), "fit")},
            {"foliage_quality", string("Tier for this capture.")},
            {"screenshot", withDefault(boolean(""), true)},
            {"max_width", withDefault(integer("", 0, 4096), 1024)}
        });
        tool.handler = visualizeTerrainTool;
        registry.addTool(std::move(tool));
    }
}

}
