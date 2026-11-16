// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R120-R124: edit_mesh_colors (the op engine over MCP, through the
// editor facade: one undo step, all-or-nothing, dry_run, jobs for large work), sample_mesh_colors
// (read-only queries, the element selection on request) and bake_mesh_colors (Export as Model, Bake
// to Texture). Op items are open in the schema and closed by the engine's op reader (D10, S47 D4),
// whose errors name ops[i].field and the fix.

#include "MeshColorTools.h"
#include "McpTools.h"

#include "EditorHost.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "Project.h"
#include "Scene.h"
#include "subsystem/VertexColorScene.h"
#include "util/FileUtils.h"
#include "vertexcolor/VertexColorEditing.h"
#include "vertexcolor/VertexColorOpsJson.h"
#include "vertexcolor/VertexPaintImages.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace mesh_color_tools {

namespace fs = std::filesystem;
namespace vc = jasperengine::vertexcolor;
namespace vce = jasperengine::editor::vertexcolor;

constexpr size_t MAX_OPS = 256;
constexpr size_t MAX_POINTS = 64;
constexpr int64_t DEFAULT_MAX_ELEMENTS = 1024;
constexpr int64_t LIMIT_MAX_ELEMENTS = 65536;

bool mcHas(const Json& args, const char* key) {
    return args.is_object() && args.contains(key) && !args[key].is_null();
}

bool mcBool(const Json& args, const char* key, bool fallback) {
    return mcHas(args, key) && args[key].is_boolean() ? args[key].get<bool>() : fallback;
}

double mcRound(double v, double scale = 1e4) {
    if (!std::isfinite(v)) return 0.0;
    return std::round(v * scale) / scale;
}

Json valueJson(const Vector4& v) {
    return Json::array({mcRound(v.x), mcRound(v.y), mcRound(v.z), mcRound(v.w)});
}

std::string hashHex(uint64_t hash) {
    char text[24];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// ---------------------------------------------------------------- the entity and its targets

struct ColorCall {
    Project* project = nullptr;
    SceneProject* scene = nullptr;
    Entity entity = NULL_ENTITY;
    std::string name;
    std::vector<vce::TargetInfo> targets;
};

// the entity of `entity`, paintable, with its targets listed (false: the call failed)
bool resolveColorCall(const CallPtr& call, ColorCall& out) {
    const Json& args = call->args();
    out.project = call->ctx().project();
    std::string error;
    out.scene = McpRefs::resolveScene(out.project, mcHas(args, "scene") ? args["scene"] : Json(), error);
    if (!out.scene || !out.scene->scene) {
        call->fail(error.empty() ? "The scene is not loaded." : error);
        return false;
    }
    if (!mcHas(args, "entity")) {
        call->fail("entity is required: the GameObject holding the Model or Mesh to color.");
        return false;
    }
    out.entity = McpRefs::resolveEntity(out.scene, args["entity"], error);
    if (out.entity == NULL_ENTITY) {
        call->fail(error);
        return false;
    }
    out.name = out.scene->scene->getEntityName(out.entity);
    std::string why;
    if (!vce::canPaint(out.project, out.scene->id, out.entity, &why)) {
        call->fail("entity: \"" + out.name + "\" cannot hold vertex colors: " + (why.empty() ? std::string("it has no Model or Mesh.") : why));
        return false;
    }
    out.targets = vce::listTargets(out.project, out.scene->id, out.entity);
    if (out.targets.empty()) {
        call->fail("entity: \"" + out.name + "\" has no mesh loaded yet (a Model still loading?): call again once get_model_info lists its meshes.");
        return false;
    }
    return true;
}

Json targetJson(size_t index, const vce::TargetInfo& info) {
    Json out = {{"index", index}, {"name", info.name}};
    if (info.key.submesh >= 0) {
        out["submesh"] = info.key.submesh;
    } else {
        out["node"] = info.key.node;
        out["primitive"] = info.key.primitive;
    }
    return out;
}

std::string targetList(const std::vector<vce::TargetInfo>& targets) {
    std::string out;
    for (size_t i = 0; i < targets.size() && i < 8; i++) {
        out += (i ? ", " : "") + std::to_string(i) + " \"" + targets[i].name + "\"";
    }
    if (targets.size() > 8) out += ", ... (" + std::to_string(targets.size()) + " targets; get_model_info lists them)";
    return out;
}

size_t findTarget(const std::vector<vce::TargetInfo>& targets, const vc::TargetKey& key) {
    for (size_t i = 0; i < targets.size(); i++) {
        if (targets[i].key == key) return i;
    }
    return targets.size();
}

// one selector: an index, or {index} / {node, primitive} / {submesh}
bool parseTargetSelector(const Json& item, const std::string& field, const std::vector<vce::TargetInfo>& targets, size_t& out, std::string& error) {
    const std::string valid = "; the targets are " + targetList(targets) + ".";
    if (item.is_number_integer()) {
        const int64_t index = item.get<int64_t>();
        if (index < 0 || index >= static_cast<int64_t>(targets.size())) {
            error = field + ": " + std::to_string(index) + " is not a target index" + valid;
            return false;
        }
        out = static_cast<size_t>(index);
        return true;
    }
    if (!item.is_object()) {
        error = field + ": give a target index or {node, primitive} or {submesh}" + valid;
        return false;
    }
    for (auto it = item.begin(); it != item.end(); ++it) {
        if (it.key() != "index" && it.key() != "node" && it.key() != "primitive" && it.key() != "submesh" && it.key() != "name") {
            error = field + "." + it.key() + ": is not a field of a target; give index, node and primitive, or submesh" + valid;
            return false;
        }
    }
    auto intField = [&](const char* key, int64_t& value) {
        if (!mcHas(item, key)) return false;
        value = item[key].is_number_integer() ? item[key].get<int64_t>() : -1;
        return true;
    };
    int64_t index = -1, node = -1, primitive = -1, submesh = -1;
    if (intField("index", index)) return parseTargetSelector(Json(index), field + ".index", targets, out, error);
    const bool byNode = intField("node", node);
    const bool byPrimitive = intField("primitive", primitive);
    const bool bySubmesh = intField("submesh", submesh);
    for (size_t i = 0; i < targets.size(); i++) {
        const vc::TargetKey& key = targets[i].key;
        if (bySubmesh && !byNode && key.submesh == submesh) { out = i; return true; }
        if (byNode && !bySubmesh && key.node == node && (byPrimitive ? key.primitive == primitive : key.primitive == 0)) { out = i; return true; }
    }
    if (mcHas(item, "name") && item["name"].is_string()) {
        for (size_t i = 0; i < targets.size(); i++) {
            if (targets[i].name == item["name"].get<std::string>()) { out = i; return true; }
        }
    }
    error = field + ": no target " + item.dump() + valid;
    return false;
}

// `targets` (edit) or `target` (sample, bake): the indices chosen, every target when absent
bool parseTargets(const Json& args, const char* key, const std::vector<vce::TargetInfo>& targets, std::vector<size_t>& out, std::string& error) {
    out.clear();
    if (!mcHas(args, key)) {
        for (size_t i = 0; i < targets.size(); i++) out.push_back(i);
        return true;
    }
    const Json& value = args[key];
    if (!value.is_array()) {
        size_t index = 0;
        if (!parseTargetSelector(value, key, targets, index, error)) return false;
        out.push_back(index);
        return true;
    }
    for (size_t i = 0; i < value.size(); i++) {
        size_t index = 0;
        if (!parseTargetSelector(value[i], std::string(key) + "[" + std::to_string(i) + "]", targets, index, error)) return false;
        if (std::find(out.begin(), out.end(), index) == out.end()) out.push_back(index);
    }
    if (out.empty()) {
        error = std::string(key) + ": the list is empty; leave it out for every target.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- what the op reader cannot do alone

// a project-relative texture path, checked inside the project, read by the shared loader (VertexPaintImages)
std::shared_ptr<const vc::Image> imageFromProjectPath(Project* project, const std::string& path, std::optional<bool> colorData, std::string& error) {
    fs::path absolute;
    if (!McpRefs::resolveProjectPath(project, path, absolute, error, true)) return nullptr;
    return vce::loadPaintImage(project, FileUtils::pathToUtf8(absolute), colorData, &error);
}

// R60: another entity's values in world space (every target it has, its own colors or its initial value)
std::shared_ptr<const vc::TransferSource> transferFrom(Project* project, SceneProject* scene, Entity self, const Json& ref, std::string& error) {
    const Entity source = McpRefs::resolveEntity(scene, ref, error);
    if (source == NULL_ENTITY) return nullptr;
    if (source == self) {
        error = "names the entity being painted; give another GameObject.";
        return nullptr;
    }
    std::string why;
    if (!vce::canPaint(project, scene->id, source, &why)) {
        error = "\"" + scene->scene->getEntityName(source) + "\" has no colors to transfer: " + why;
        return nullptr;
    }
    bool colored = false;
    for (const vce::TargetInfo& info : vce::listTargets(project, scene->id, source)) colored = colored || info.painted || info.hasAssetColors;
    // every target's values and, with hard edges, its corners (they carry over, R60)
    std::shared_ptr<const vc::TransferSource> out = colored ? vce::transferSource(project, scene->id, source, nullptr) : nullptr;
    if (!out) {
        error = "\"" + scene->scene->getEntityName(source) + "\" has no vertex colors (none painted, no COLOR_0 in its mesh); paint it first.";
        return nullptr;
    }
    return out;
}

// the hooks of one target's parse; images and sources are shared between targets
struct HookCache {
    std::map<std::string, std::shared_ptr<const vc::Image>> images;
    std::map<std::string, std::shared_ptr<const vc::TransferSource>> sources;
    // from_texture with material_slot and no uv_set: the slot's own UV set, by op index
    std::map<size_t, int> slotTexCoords;
};

vc::OpJsonHooks colorHooks(const ColorCall& c, const vc::TargetKey& key, const std::shared_ptr<HookCache>& cache, size_t* opIndex) {
    vc::OpJsonHooks hooks;
    Project* project = c.project;
    SceneProject* scene = c.scene;
    const Entity entity = c.entity;
    hooks.loadImage = [project, scene, entity, key, cache, opIndex](const Json& item, std::string& field, std::string& error) -> std::shared_ptr<const vc::Image> {
        if (mcHas(item, "texture")) {
            field = "texture";
            const std::string path = item["texture"].get<std::string>();
            auto found = cache->images.find("file:" + path);
            if (found != cache->images.end()) return found->second;
            auto image = imageFromProjectPath(project, path, std::nullopt, error);
            if (image) cache->images["file:" + path] = image;
            return image;
        }
        field = "material_slot";
        int texCoord = 0;
        auto image = vce::materialImage(project, scene->id, vce::TargetRef{entity, key}, item["material_slot"].get<std::string>(), &error, &texCoord);
        if (image && opIndex && !mcHas(item, "uv_set")) cache->slotTexCoords[*opIndex] = texCoord;
        return image;
    };
    hooks.loadStencil = [project, cache](const std::string& path, std::string& error) -> std::shared_ptr<const vc::Image> {
        auto found = cache->images.find("stencil:" + path);
        if (found != cache->images.end()) return found->second;
        auto image = imageFromProjectPath(project, path, false, error);
        if (image) cache->images["stencil:" + path] = image;
        return image;
    };
    hooks.transferSource = [project, scene, entity, cache](const Json& value, std::string& error) -> std::shared_ptr<const vc::TransferSource> {
        const std::string id = value.dump();
        auto found = cache->sources.find(id);
        if (found != cache->sources.end()) return found->second;
        auto source = transferFrom(project, scene, entity, value, error);
        if (source) cache->sources[id] = source;
        return source;
    };
    const uint32_t sceneId = scene->id;
    hooks.selection = [sceneId, entity, key](vc::Target& target, std::string& error) {
        if (vce::selectionTarget(sceneId, entity, key, target)) return true;
        error = "the editor has no element selection on " + key.describe() + ": select with sample_mesh_colors {query: \"elements\", select: true}, "
                "or name the ids with a vertices, faces or corners target.";
        return false;
    };
    return hooks;
}

// ops[] through the engine's reader with this target's hooks
bool parseOps(const ColorCall& c, const vc::TargetKey& key, const Json& list, const std::shared_ptr<HookCache>& cache,
              std::vector<vc::Op>& ops, std::string& error) {
    ops.clear();
    size_t index = 0;
    vc::OpJsonParser parser(colorHooks(c, key, cache, &index));
    for (index = 0; index < list.size(); index++) {
        vc::Op op;
        if (!parser.parse(list[index], index, op)) {
            error = parser.error;
            return false;
        }
        auto slot = cache->slotTexCoords.find(index);
        if (slot != cache->slotTexCoords.end()) op.uvSet = std::clamp(slot->second, 0, 1);
        ops.push_back(std::move(op));
    }
    return true;
}

// an op whose meaning depends on the target (its selection, its material): parsed once per target
bool targetDependent(const Json& list) {
    for (const Json& item : list) {
        if (!item.is_object()) continue;
        if (mcHas(item, "material_slot")) return true;
        if (mcHas(item, "target") && item["target"].is_object() && item["target"].value("kind", std::string()) == "selection") return true;
    }
    return false;
}

// ---------------------------------------------------------------- results

Json leanStats(const vc::ChannelStats& stats) {
    Json full = vc::toJson(stats);
    return Json{{"count", full["count"]}, {"min", full["min"]}, {"mean", full["mean"]}, {"max", full["max"]}};
}

void addWarning(Json& warnings, const std::string& code, const std::string& message) {
    for (const Json& w : warnings) {
        if (w.value("code", std::string()) == code && w.value("message", std::string()) == message) return;
    }
    warnings.push_back(Json{{"code", code}, {"message", message}});
}

bool hasWarning(const Json& warnings, const std::string& code) {
    for (const Json& w : warnings) {
        if (w.value("code", std::string()) == code) return true;
    }
    return false;
}

struct EditSummary {
    Json out = Json::object();
    std::string text;
};

EditSummary editSummary(const ColorCall& c, const vce::EditResult& result, bool dryRun, bool wantHash, const std::string& undoName) {
    EditSummary s;
    Json& out = s.out;
    out["entity"] = McpRefs::entityJson(c.scene, c.entity);
    out["dry_run"] = dryRun;
    out["created"] = Json{{"component", result.createdComponent}, {"file", result.createdFile}};
    Json targets = Json::array();
    Json warnings = Json::array();
    uint64_t vertices = 0, corners = 0, splits = 0;
    for (const vce::TargetResult& t : result.targets) {
        const size_t index = findTarget(c.targets, t.target.key);
        Json entry = Json::object();
        entry["target"] = index < c.targets.size() ? targetJson(index, c.targets[index]) : Json{{"name", t.target.key.describe()}};
        uint64_t tv = 0, tc = 0, ts = 0;
        for (const vc::OpResult& op : t.result.ops) {
            tv += op.changedVertices;
            tc += op.changedCorners;
            ts += op.splitAdded;
        }
        entry["changed"] = Json{{"vertices", tv}, {"corners", tc}, {"split_added", ts}};
        entry["split_vertices"] = t.splitVertices;
        Json opsJson = vc::toJson(t.result);
        entry["ops"] = opsJson["ops"];
        entry["stats"] = Json{{"before", leanStats(t.result.before)}, {"after", leanStats(t.result.after)}};
        if (wantHash) entry["hash"] = hashHex(t.result.hash);
        for (const vc::Warning& w : t.result.warnings) addWarning(warnings, w.code, w.message);
        targets.push_back(entry);
        vertices += tv;
        corners += tc;
        splits += ts;
    }
    for (const vc::Warning& w : result.warnings) addWarning(warnings, w.code, w.message);
    out["targets"] = targets;
    out["changed"] = Json{{"vertices", vertices}, {"corners", corners}, {"split_added", splits}};
    out["warnings"] = warnings;
    const std::string id = std::to_string(c.entity);
    if (dryRun) {
        out["next"] = "Nothing changed. Call again without dry_run to apply.";
    } else {
        out["undo_name"] = undoName;
        std::string next;
        if (hasWarning(warnings, "material_ignores_channels")) next = "The material does not show some painted channels (see warnings): fix that first. ";
        next += "Check the values with sample_mesh_colors {entity: " + id + ", query: \"stats\"}, then look with set_scene_camera "
                "{display: {draw_mode: \"vertex_color\"}} and screenshot {view: \"scene\"}: a shaded view hides what the material does not use.";
        out["next"] = next;
    }
    s.text = std::string(dryRun ? "Dry run: " : "") + "\"" + c.name + "\": " + std::to_string(vertices) + " vertices and " + std::to_string(corners) +
             " corners " + (dryRun ? "would change" : "changed") + " in " + std::to_string(result.targets.size()) + " target(s)" +
             (dryRun ? "; nothing was written." : " (one undo step).");
    if (splits > 0) s.text += " " + std::to_string(splits) + " corner override(s) made hard edges.";
    return s;
}

std::string failText(const std::string& error) {
    return error + (error.empty() || error.back() == '.' ? "" : ".") + " Nothing was changed.";
}

// ---------------------------------------------------------------- edit_mesh_colors

void startEditJob(const CallPtr& call, const ColorCall& c, vce::EditRequest request, bool dryRun, bool wantHash, bool wait, double timeout);

void editMeshColorsTool(const CallPtr& call) {
    ColorCall c;
    if (!resolveColorCall(call, c)) return;
    const Json& args = call->args();
    std::string error;
    std::vector<size_t> chosen;
    if (!parseTargets(args, "targets", c.targets, chosen, error)) {
        call->fail(error);
        return;
    }
    if (!mcHas(args, "ops") || !args["ops"].is_array() || args["ops"].empty()) {
        call->fail("ops is required: a list such as [{\"op\": \"fill\", \"target\": {\"kind\": \"all\"}, \"value\": \"#8FB04A\"}] (fields: jasper://docs/meshes).");
        return;
    }
    if (args["ops"].size() > MAX_OPS) {
        call->fail("ops: " + std::to_string(args["ops"].size()) + " ops, over 256 a call; split them into several calls.");
        return;
    }
    if (mcHas(args, "space") && args["space"] != "world" && args["space"] != "local") {
        call->fail("space must be world or local.");
        return;
    }
    const bool world = !(mcHas(args, "space") && args["space"] == "local");
    const bool dryRun = mcBool(args, "dry_run", false);
    const bool wantHash = mcBool(args, "hash", false);
    const bool wait = mcBool(args, "wait", true);
    const double timeout = mcHas(args, "timeout_seconds") && args["timeout_seconds"].is_number() ? args["timeout_seconds"].get<double>() : 120.0;
    const std::string undoName = "Edit Vertex Colors (" + c.name + ")";
    auto cache = std::make_shared<HookCache>();

    // one op list for every target, unless an op means something else on each (its selection, its
    // material): then each target's own list, still one request and one undo step
    vce::EditRequest request;
    for (size_t index : chosen) request.targets.push_back(vce::TargetRef{c.entity, c.targets[index].key});
    if (chosen.size() == 1 || !targetDependent(args["ops"])) {
        if (!parseOps(c, c.targets[chosen[0]].key, args["ops"], cache, request.ops, error)) {
            call->fail(failText(error));
            return;
        }
    } else {
        for (size_t index : chosen) {
            std::vector<vc::Op> ops;
            if (!parseOps(c, c.targets[index].key, args["ops"], cache, ops, error)) {
                call->fail(failText(c.targets[index].name + ": " + error));
                return;
            }
            request.perTargetOps.push_back(std::move(ops));
        }
    }
    request.world = world;
    request.dryRun = dryRun;
    request.undoName = undoName;
    startEditJob(call, c, std::move(request), dryRun, wantHash, wait, timeout);
}

// R97: the facade sends large edits and bakes to a job and hands the result back on the main thread
void startEditJob(const CallPtr& call, const ColorCall& c, vce::EditRequest request, bool dryRun, bool wantHash, bool wait, double timeout) {
    struct Pending {
        JobPtr job;
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
        std::optional<vce::EditResult> early;
        bool returned = false;
    };
    auto pending = std::make_shared<Pending>();
    const ColorCall context = c;
    const std::string undoName = request.undoName;
    auto finish = [pending, context, dryRun, wantHash, undoName](const vce::EditResult& result) {
        if (!pending->job) {
            pending->early = result;
            return;
        }
        if (pending->job->cancelRequested() || pending->cancel->load()) {
            pending->job->markCancelled("Cancelled: nothing was applied.");
            return;
        }
        if (!result.ok) {
            pending->job->fail(failText(result.error));
            return;
        }
        EditSummary summary = editSummary(context, result, dryRun, wantHash, undoName);
        pending->job->succeed(summary.text, summary.out);
    };
    request.done = [pending, finish](const vce::EditResult& result) {
        // a request that ran at once answers through applyEdit's return value
        if (pending->returned && !pending->job) return;
        finish(result);
    };
    request.cancel = pending->cancel.get();
    std::weak_ptr<Pending> weak = pending;
    request.progress = [weak](float progress) {
        if (auto p = weak.lock(); p && p->job) p->job->setProgress(progress, "editing vertex colors");
    };

    vce::EditResult result = vce::applyEdit(c.project, c.scene->id, request);
    pending->returned = true;
    if (!result.async) {
        if (!result.ok) {
            call->fail(failText(result.error));
            return;
        }
        EditSummary summary = editSummary(c, result, dryRun, wantHash, undoName);
        call->ok(summary.text, summary.out);
        return;
    }
    pending->job = call->ctx().jobs().create("edit_mesh_colors", undoName);
    pending->job->setProgress(-1.0, "editing vertex colors");
    pending->job->setCancelHandler([cancel = pending->cancel]() { cancel->store(true); });
    if (pending->early) {
        const vce::EditResult early = *pending->early;
        pending->early.reset();
        finish(early);
    }
    call->detach();
    replyWhenJobDone(call, pending->job, wait, timeout, [](const McpJob& finished) {
        Json out = finished.result();
        out["job_id"] = finished.id();
        out["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), out);
        return ToolResult::success(finished.message(), out);
    });
}

// ---------------------------------------------------------------- sample_mesh_colors

// a region and filters in the op target's own shape, read by the op reader (errors renamed)
bool parseRegion(const ColorCall& c, const vc::TargetKey& key, const Json& args, vc::Op& op, std::string& error) {
    Json item = {{"op", "set"}, {"value", Json::array({1, 1, 1})}};
    item["target"] = mcHas(args, "region") ? args["region"] : Json{{"kind", "all"}};
    if (mcHas(args, "filter")) item["filter"] = args["filter"];
    vc::OpJsonParser parser(colorHooks(c, key, std::make_shared<HookCache>(), nullptr));
    if (parser.parse(item, 0, op)) return true;
    error = parser.error;
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{{"ops[0].target", "region"}, {"ops[0].filter", "filter"}, {"ops[0]", "region"}}) {
        if (error.rfind(from, 0) == 0) {
            error = to + error.substr(from.size());
            break;
        }
    }
    return false;
}

// R108: the closest point of triangle abc to p, as barycentric weights (Ericson, Real-Time Collision Detection 5.1.5)
Vector3 closestBarycentric(const Vector3& p, const Vector3& a, const Vector3& b, const Vector3& c) {
    const Vector3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = ab.dotProduct(ap), d2 = ac.dotProduct(ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return Vector3(1.0f, 0.0f, 0.0f);
    const Vector3 bp = p - b;
    const float d3 = ab.dotProduct(bp), d4 = ac.dotProduct(bp);
    if (d3 >= 0.0f && d4 <= d3) return Vector3(0.0f, 1.0f, 0.0f);
    const float vc3 = d1 * d4 - d3 * d2;
    if (vc3 <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        return Vector3(1.0f - v, v, 0.0f);
    }
    const Vector3 cp = p - c;
    const float d5 = ab.dotProduct(cp), d6 = ac.dotProduct(cp);
    if (d6 >= 0.0f && d5 <= d6) return Vector3(0.0f, 0.0f, 1.0f);
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        return Vector3(1.0f - w, 0.0f, w);
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return Vector3(0.0f, 1.0f - w, w);
    }
    const float denom = 1.0f / (va + vb + vc3);
    const float v = vb * denom, w = vc3 * denom;
    return Vector3(1.0f - v - w, v, w);
}

Vector4 elementValue(const vc::TargetData& data, const vc::MeshView& mesh, vc::ElementKind kind, uint32_t id) {
    switch (kind) {
        case vc::ElementKind::Vertex: {
            const uint32_t gpu = mesh.members[mesh.memberStart[id]];
            return gpu < data.base.size() ? data.base[gpu] : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        }
        case vc::ElementKind::Face: {
            Vector4 sum(0.0f, 0.0f, 0.0f, 0.0f);
            for (uint32_t k = 0; k < 3; k++) sum = sum + data.cornerValue(id * 3 + k, mesh.indices);
            return Vector4(sum.x / 3.0f, sum.y / 3.0f, sum.z / 3.0f, sum.w / 3.0f);
        }
        case vc::ElementKind::Corner:
        default:
            return data.cornerValue(id, mesh.indices);
    }
}

const char* elementKindName(vc::ElementKind kind) {
    switch (kind) {
        case vc::ElementKind::Vertex: return "vertices";
        case vc::ElementKind::Face: return "faces";
        case vc::ElementKind::Corner:
        default: return "corners";
    }
}

void sampleMeshColorsTool(const CallPtr& call) {
    ColorCall c;
    if (!resolveColorCall(call, c)) return;
    const Json& args = call->args();
    std::string error;
    std::vector<size_t> chosen;
    if (!parseTargets(args, "target", c.targets, chosen, error)) {
        call->fail(error);
        return;
    }
    const std::string query = mcHas(args, "query") && args["query"].is_string() ? args["query"].get<std::string>() : "stats";
    if (query != "stats" && query != "palette" && query != "points" && query != "elements" && query != "hash") {
        call->fail("query must be stats, palette, points, elements or hash.");
        return;
    }
    const bool select = mcBool(args, "select", false);
    if (select && query != "elements") {
        call->fail("select: works with query \"elements\": it makes the elements found the editor's selection.");
        return;
    }
    if (query != "elements" && (mcHas(args, "region") || mcHas(args, "filter"))) {
        call->fail("region and filter narrow query \"elements\"; stats, palette and hash read the whole target.");
        return;
    }
    const bool world = !(mcHas(args, "space") && args["space"] == "local");
    int64_t maxElements = DEFAULT_MAX_ELEMENTS;
    if (mcHas(args, "max_elements")) {
        if (!args["max_elements"].is_number_integer() || args["max_elements"].get<int64_t>() < 1 || args["max_elements"].get<int64_t>() > LIMIT_MAX_ELEMENTS) {
            call->fail("max_elements must be a whole number from 1 to 65536.");
            return;
        }
        maxElements = args["max_elements"].get<int64_t>();
    }
    std::vector<Vector3> points;
    if (query == "points") {
        if (!mcHas(args, "points") || !args["points"].is_array() || args["points"].empty()) {
            call->fail("points is required for query \"points\": world positions [[x, y, z], ...] (local with space: local).");
            return;
        }
        if (args["points"].size() > MAX_POINTS) {
            call->fail("points: at most 64 a call.");
            return;
        }
        for (size_t i = 0; i < args["points"].size(); i++) {
            Vector3 p;
            if (!McpValues::parseVector3(args["points"][i], p)) {
                call->fail("points[" + std::to_string(i) + "] must be [x, y, z].");
                return;
            }
            points.push_back(p);
        }
    }

    Json targets = Json::array();
    int64_t total = 0;
    std::vector<vce::ElementSelection> selection;
    Json pointsOut = Json::array();
    std::vector<double> bestDistance(points.size(), std::numeric_limits<double>::max());
    for (const Json& p : args.value("points", Json::array())) pointsOut.push_back(Json{{"point", p}, {"value", nullptr}});
    for (size_t index : chosen) {
        const vce::TargetInfo& info = c.targets[index];
        std::shared_ptr<const vc::MeshView> view = vce::meshView(c.project, c.scene->id, c.entity, info.key);
        if (!view) {
            call->fail(info.name + ": its geometry is not loaded yet; call again in a moment.");
            return;
        }
        const vc::TargetData data = vce::targetDataOrInitial(c.project, c.scene->id, c.entity, info.key);
        Json entry = Json::object();
        entry["target"] = targetJson(index, info);
        entry["painted"] = info.painted;
        if (query == "stats") {
            entry["stats"] = vc::toJson(vc::computeStats(data, *view));
        } else if (query == "hash") {
            entry["hash"] = hashHex(vc::hashTarget(data));
        } else if (query == "palette") {
            Json list = Json::array();
            for (const vc::PaletteEntry& e : vc::palette(data, *view, 64)) {
                list.push_back(Json{{"value", valueJson(e.value)}, {"hex", vc::toHex(e.value, true)}, {"corners", e.corners}});
            }
            entry["palette"] = list;
        } else if (query == "points") {
            const Matrix4 model = vce::targetModel(c.project, c.scene->id, c.entity, info.key);
            const Matrix4 toLocal = model.inverse();
            for (size_t i = 0; i < points.size(); i++) {
                const Vector3 local = world ? toLocal * points[i] : points[i];
                double best = std::numeric_limits<double>::max();
                uint32_t bestTriangle = 0;
                Vector3 bestBary(1.0f, 0.0f, 0.0f);
                for (uint32_t t = 0; t < view->triangleCount(); t++) {
                    const Vector3& a = view->positions[view->indices[t * 3]];
                    const Vector3& b = view->positions[view->indices[t * 3 + 1]];
                    const Vector3& cc = view->positions[view->indices[t * 3 + 2]];
                    const Vector3 bary = closestBarycentric(local, a, b, cc);
                    const Vector3 q = a * bary.x + b * bary.y + cc * bary.z;
                    const Vector3 hit = world ? model * q : q;
                    const Vector3 d = hit - (world ? points[i] : local);
                    const double dist = std::sqrt(static_cast<double>(d.squaredLength()));
                    if (dist < best) {
                        best = dist;
                        bestTriangle = t;
                        bestBary = bary;
                    }
                }
                if (view->triangleCount() == 0 || best >= bestDistance[i]) continue;
                bestDistance[i] = best;
                const Vector4 value = vc::valueAt(data, *view, bestTriangle, bestBary);
                pointsOut[i] = Json{{"point", args["points"][i]}, {"value", valueJson(value)}, {"hex", vc::toHex(value, true)},
                                    {"distance", mcRound(best)}, {"target", index}, {"face", bestTriangle}};
            }
        } else {
            vc::Op region;
            if (!parseRegion(c, info.key, args, region, error)) {
                call->fail(error);
                return;
            }
            const vc::ApplyOptions options = vce::applyOptions(c.project, c.scene->id, c.entity, info.key, world);
            vc::ElementKind kind = vc::ElementKind::Vertex;
            std::vector<uint32_t> ids = vc::resolveElements(data, *view, region.target, region.filters, options, kind);
            total += static_cast<int64_t>(ids.size());
            if (total > maxElements) {
                std::string narrower = chosen.size() > 1 ? "target one primitive (" + targetList(c.targets) + "), " : std::string();
                call->fail("region_too_large: the region holds more than max_elements (" + std::to_string(maxElements) + ") elements (" +
                           std::to_string(ids.size()) + " " + elementKindName(kind) + " in " + info.name + "). Narrow it: " + narrower +
                           "a sphere or box region, or a filter; query stats or palette gives totals without ids.");
                return;
            }
            Json values = Json::array();
            for (uint32_t id : ids) values.push_back(valueJson(elementValue(data, *view, kind, id)));
            entry["kind"] = elementKindName(kind);
            entry["ids"] = ids;
            entry["values"] = values;
            if (select) {
                vce::ElementSelection s;
                s.entity = c.entity;
                s.key = info.key;
                s.selection.kind = kind;
                s.selection.ids = ids;
                selection.push_back(std::move(s));
            }
        }
        targets.push_back(entry);
    }

    Json out = {{"entity", McpRefs::entityJson(c.scene, c.entity)}, {"query", query}};
    std::string text = "\"" + c.name + "\": ";
    if (query == "points") {
        out["points"] = pointsOut;
        text += std::to_string(points.size()) + " point(s) sampled.";
    } else {
        out["targets"] = targets;
        text += query + " of " + std::to_string(targets.size()) + " target(s).";
    }
    if (query == "elements") {
        out["count"] = total;
        text = "\"" + c.name + "\": " + std::to_string(total) + " element(s).";
        if (select) {
            vce::setElementSelection(c.project, c.scene->id, std::move(selection), "Select Mesh Elements");
            out["selected"] = true;
            out["next"] = "The editor selects them now: edit_mesh_colors ops with target {kind: \"selection\"} act on exactly these.";
            text += " They are the editor's element selection now.";
        } else {
            out["next"] = "edit_mesh_colors acts on these with target {kind: \"" + std::string(targets.empty() ? "faces" : targets[0].value("kind", "faces")) +
                          "\", ids}, or pass select: true to show them to the user.";
        }
    }
    call->ok(text, out);
}

// ---------------------------------------------------------------- bake_mesh_colors

void bakeMeshColorsTool(const CallPtr& call) {
    ColorCall c;
    if (!resolveColorCall(call, c)) return;
    const Json& args = call->args();
    const std::string output = mcHas(args, "output") && args["output"].is_string() ? args["output"].get<std::string>() : "";
    if (output != "model" && output != "texture") {
        call->fail("output is required: model (a .glb with COLOR_0) or texture (a PNG in UV space).");
        return;
    }
    if (!mcHas(args, "path") || !args["path"].is_string()) {
        call->fail(std::string("path is required: the project-relative ") + (output == "model" ? ".glb" : ".png") + " to write.");
        return;
    }
    const std::string path = args["path"].get<std::string>();
    std::string error;
    fs::path absolute;
    if (!McpRefs::resolveProjectWritePath(c.project, path, absolute, error)) {
        call->fail("path: " + error);
        return;
    }
    std::string extension = absolute.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (output == "model" && extension != ".glb") {
        call->fail("path: a model bake writes a .glb; name one such as models/rock_painted.glb.");
        return;
    }
    if (output == "texture" && extension != ".png") {
        call->fail("path: a texture bake writes a .png; name one such as textures/rock_colors.png.");
        return;
    }
    // the facade's paths are the assets folder's; MCP's are the project's
    if (!c.project->isInsideAssetsPath(absolute.lexically_normal())) {
        call->fail("path: '" + path + "' is outside the project's assets folder; write the file inside it.");
        return;
    }
    const std::string assetPath = FileUtils::pathToGenericUtf8(c.project->normalizeToAssetsRelative(absolute.lexically_normal()));
    bool anyPainted = false;
    for (const vce::TargetInfo& info : c.targets) anyPainted = anyPainted || info.painted;
    if (!anyPainted) {
        call->fail("\"" + c.name + "\" has no painted vertex colors to bake: paint it with edit_mesh_colors first.");
        return;
    }

    vce::BakeTextureOptions texture;
    vce::ExportModelOptions model;
    model.path = assetPath;
    model.replaceInScene = mcBool(args, "replace_in_scene", false);
    model.splitHardEdges = mcBool(args, "split_hard_edges", true);
    if (output == "texture") {
        std::vector<size_t> chosen;
        if (!parseTargets(args, "target", c.targets, chosen, error)) {
            call->fail(error);
            return;
        }
        if (chosen.size() != 1) {
            call->fail("target: a texture holds one target's UVs; pick one of " + targetList(c.targets) + ".");
            return;
        }
        texture.key = c.targets[chosen[0]].key;
        texture.path = assetPath;
        auto intArg = [&](const char* key, int& out, int lo, int hi) {
            if (!mcHas(args, key)) return true;
            if (!args[key].is_number_integer() || args[key].get<int64_t>() < lo || args[key].get<int64_t>() > hi) {
                call->fail(std::string(key) + " must be a whole number from " + std::to_string(lo) + " to " + std::to_string(hi) + ".");
                return false;
            }
            out = static_cast<int>(args[key].get<int64_t>());
            return true;
        };
        if (!intArg("size", texture.size, 16, 8192) || !intArg("padding", texture.padding, 0, 64) || !intArg("uv_set", texture.uvSet, 0, 1)) return;
        texture.srgb = mcBool(args, "srgb", true);
    } else if (mcHas(args, "target") || mcHas(args, "size") || mcHas(args, "uv_set")) {
        call->fail("target, size and uv_set are for output texture; a model bake writes every target.");
        return;
    }

    const std::string title = output == "model" ? "Export as Model (" + c.name + ")" : "Bake to Texture (" + c.name + ")";
    JobPtr job = call->ctx().jobs().create("bake_mesh_colors", title);
    job->setProgress(-1.0, output == "model" ? "writing the model" : "rasterizing the colors");
    Project* project = c.project;
    const uint32_t sceneId = c.scene->id;
    const Entity entity = c.entity;
    const std::string name = c.name;
    // the facade writes on the main thread: the call returns its job first, so wait: false answers at once
    editor::getEditorHost().enqueueMainThreadTask([job, project, sceneId, entity, output, model, texture, path, name]() {
        if (job->cancelRequested()) {
            job->markCancelled("Cancelled: nothing was written.");
            return;
        }
        std::string error;
        const bool done = output == "model" ? vce::exportAsModel(project, sceneId, entity, model, &error)
                                            : vce::bakeToTexture(project, sceneId, entity, texture, &error);
        if (!done) {
            job->fail(error.empty() ? std::string("The bake failed; the console has details.") : error);
            return;
        }
        Json out = {{"output", output}, {"path", path}};
        if (output == "model") {
            out["replaced_in_scene"] = model.replaceInScene;
            out["next"] = model.replaceInScene
                ? "\"" + name + "\" now loads " + path + " (its colors in COLOR_0) without a Vertex Colors component; undo puts both back."
                : "set_model {path: \"" + path + "\"} loads it on a Model; get_model_info shows has_vertex_colors on its submeshes.";
        } else {
            out["next"] = "Assign it as a material's albedo map (update_material), or view it with view_asset.";
        }
        job->succeed("Wrote " + path + ".", out);
    });
    call->detach();
    replyWhenJobDone(call, job, mcBool(args, "wait", true),
                     mcHas(args, "timeout_seconds") && args["timeout_seconds"].is_number() ? args["timeout_seconds"].get<double>() : 120.0,
                     [](const McpJob& finished) {
        Json out = finished.result();
        out["job_id"] = finished.id();
        out["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), out);
        return ToolResult::success(finished.message(), out);
    });
}

} // namespace mesh_color_tools

Json vertexColorTargetsJson(Project* project, uint32_t sceneId, Entity entity) {
    namespace vce = jasperengine::editor::vertexcolor;
    namespace vc = jasperengine::vertexcolor;
    if (!project || !vce::canPaint(project, sceneId, entity, nullptr)) return nullptr;
    SceneProject* scene = project->getScene(sceneId);
    if (!scene || !scene->scene) return nullptr;
    const std::vector<vce::TargetInfo> targets = vce::listTargets(project, sceneId, entity);
    if (targets.empty()) return nullptr;
    const std::vector<vc::SceneTarget> placed = vc::sceneTargets(scene->scene, entity);
    Json out = Json::array();
    for (size_t i = 0; i < targets.size(); i++) {
        const vce::TargetInfo& info = targets[i];
        Json entry = mesh_color_tools::targetJson(i, info);
        for (const vc::SceneTarget& target : placed) {
            if (target.key != info.key) continue;
            entry["mesh"] = target.meshEntity;
            entry["mesh_submesh"] = target.submesh;
        }
        entry["vertex_count"] = info.vertexCount;
        entry["has_vertex_colors"] = info.hasAssetColors || info.painted;
        entry["painted"] = info.painted;
        if (info.painted) {
            entry["split_vertices"] = info.splitVertices;
            entry["stale"] = info.stale;
        }
        out.push_back(entry);
    }
    return out;
}

void registerMeshColorTools(McpRegistry& registry) {
    using namespace schema;
    const Json target = anyOf("", {integer("Index."), anyObject("{node, primitive} or {submesh}")});

    {
        ToolDef tool;
        tool.name = "edit_mesh_colors";
        tool.title = "Edit Mesh Colors";
        tool.group = "meshes";
        tool.attributesUndo = true;
        tool.allowedWhilePlaying = false;
        tool.description =
            "Paint a Model's or Mesh's vertex colors with ops: all-or-nothing, one undo step a call; adds the Vertex Colors "
            "component. Vertex targets blend; faces/corners (or element: face) give hard edges. dry_run changes nothing. "
            "sample_mesh_colors reads. Ops: jasper://docs/meshes.";
        Json op = anyObject("{op, target: {kind: all|vertices|faces|corners|selection|sphere|box|capsule|color}, value, strength...}");
        op["properties"] = Json{{"op", stringEnum("", {"set", "fill", "stroke", "gradient", "smooth", "sharpen", "noise", "adjust", "channel",
                                                        "replace_color", "from_texture", "bake_ao", "bake_thickness", "bake_curvature",
                                                        "by_direction", "transfer", "clear"})}};
        op["required"] = Json::array({"op"});
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"targets", array(target, "")},
            {"ops", Json{{"type", "array"}, {"maxItems", 256}, {"items", op}}},
            {"space", withDefault(stringEnum("", {"world", "local"}), "world")},
            {"dry_run", withDefault(boolean(""), false)},
            {"hash", withDefault(boolean(""), false)},
            {"wait", withDefault(boolean("Large work runs as a job: false returns its job_id."), true)},
            {"timeout_seconds", withDefault(number("", 0, 3600), 120)}
        }, {"entity", "ops"});
        tool.handler = mesh_color_tools::editMeshColorsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "sample_mesh_colors";
        tool.title = "Sample Mesh Colors";
        tool.group = "meshes";
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.description =
            "Read a mesh's vertex colors: stats per channel, palette, points (values at world points), elements (ids and values in "
            "a region, max_elements; region_too_large asks for a narrower one) or hash. Changes nothing, but select: true makes "
            "the elements the editor's selection. Edit with edit_mesh_colors.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"target", target},
            {"query", withDefault(stringEnum("", {"stats", "palette", "points", "elements", "hash"}), "stats")},
            {"points", array(vec3(""), "")},
            {"region", anyObject("An op target; default all.")},
            {"filter", array(anyObject(""), "")},
            {"space", withDefault(stringEnum("", {"world", "local"}), "world")},
            {"max_elements", withDefault(integer("", 1, 65536), 1024)},
            {"select", withDefault(boolean(""), false)}
        }, {"entity"});
        tool.handler = mesh_color_tools::sampleMeshColorsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "bake_mesh_colors";
        tool.title = "Bake Mesh Colors";
        tool.group = "meshes";
        tool.attributesUndo = true;
        tool.allowedWhilePlaying = false;
        tool.description =
            "Write painted vertex colors to a file, as a job: model is a .glb with COLOR_0 (replace_in_scene swaps the Model to "
            "it and drops the component), texture a PNG of one target in UV space. Paint with edit_mesh_colors first.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef()},
            {"output", stringEnum("", {"model", "texture"})},
            {"path", string("Project-relative.")},
            {"replace_in_scene", withDefault(boolean(""), false)},
            {"split_hard_edges", withDefault(boolean(""), true)},
            {"target", target},
            {"size", withDefault(integer("", 16, 8192), 1024)},
            {"padding", withDefault(integer("", 0, 64), 4)},
            {"uv_set", withDefault(integer("", 0, 1), 0)},
            {"srgb", withDefault(boolean(""), true)},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number("", 0, 3600), 120)}
        }, {"entity", "output", "path"});
        tool.handler = mesh_color_tools::bakeMeshColorsTool;
        registry.addTool(std::move(tool));
    }
}

}
