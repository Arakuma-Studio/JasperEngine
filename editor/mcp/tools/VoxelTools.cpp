// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Voxel Volume tools (docs/specs/T6-voxel-volumes.md R66-R71, R82): edit_voxels (with create),
// sample_voxels, visualize_voxels and generate_voxels. Op and shape fields are open in the schemas and closed here,
// where an unknown key can name the right ones. Positions are world space. A call is one undo step
// whatever its op count (D15); in Play an edit reaches only the running copy and has no undo (R30).
//
// Long work runs on a private clone of the store's tables (R6: blocks are shared and immutable, a
// clone's tables belong to whoever holds it), so the main thread keeps drawing: an edit's clone
// hands back the delta to apply, a read's clone just answers.
#include "McpTools.h"

#include "EditorHost.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "mcp/tools/PlayViewCommon.h"

#include "App.h"
#include "Catalog.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/VoxelEditCmd.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "object/voxel/GroundQuery.h"
#include "subsystem/VoxelSystem.h"
#include "util/Base64.h"
#include "util/FileUtils.h"
#include "util/MeshTriangles.h"
#include "voxel/VoxelAssetFile.h"
#include "voxel/VoxelCreate.h"
#include "voxel/VoxelEdit.h"
#include "voxel/VoxelGenerationTask.h"
#include "voxel/VoxelOverlay.h"
#include "voxel/VoxelProjectSettings.h"
#include "voxel/VoxelSample.h"
#include "voxel/VoxelSliceRender.h"
#include "voxel/VoxelVoxelize.h"
#include "window/SceneWindow.h"

#include "stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine::editor::mcp {

// Named, as unity builds merge the anonymous helpers of every tool source
namespace voxeltools {

namespace {

namespace fs = std::filesystem;
using voxel::EditError;
using voxel::EditKind;
using voxel::EditOp;
using voxel::EditOptions;
using voxel::EditResult;
using voxel::Int3;
using voxel::IntBox;
using voxel::ShapeKind;
using voxel::Store;
using voxel::Vec3d;

constexpr const char* DOCS = "jasper://docs/voxels";
constexpr size_t MAX_POINTS = 1024;        // R69
constexpr int64_t MAX_BOX_VALUES = 64 * 64 * 64;   // R69
constexpr size_t MAX_BLOCK_HASHES = 4096;
constexpr size_t MAX_COMPONENTS = 64;
constexpr int DEFAULT_SLICE_WIDTH = 512;

// R68: an edit over about a second becomes a job. Samples a call's shapes reach, as measured on this
// host's single core at roughly 20 M samples a second; smoothing reads each one once per iteration.
constexpr int64_t EDIT_JOB_COST = 16 * 1000 * 1000;
// R69: reads that must resolve blocks become a job. A base or cold block costs up to a millisecond,
// a resident block's scan about a sixty-fourth of that.
constexpr int64_t READ_JOB_COST = 64 * 512;
constexpr int64_t READ_COST_UNRESOLVED = 64;
constexpr int64_t READ_COST_RESIDENT = 1;

bool has(const Json& args, const char* key) {
    return args.is_object() && args.contains(key) && !args[key].is_null();
}

// Fails naming the valid keys: the schema leaves these objects open, so this is where a mistyped
// field is caught
bool closedKeys(const Json& object, std::initializer_list<const char*> keys, const std::string& where, std::string& error) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const char* key : keys) known = known || it.key() == key;
        if (known) continue;
        std::string names;
        for (const char* key : keys) names += (names.empty() ? "" : ", ") + std::string(key);
        error = where + ": \"" + it.key() + "\" is not a field; use " + names + " (" + DOCS + ")";
        return false;
    }
    return true;
}

Json vecJson(const Vector3& v) {
    return Json::array({v.x, v.y, v.z});
}

Json intsJson(const Int3& v) {
    return Json::array({v.x, v.y, v.z});
}

std::string hexHash(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

bool readVec3(const Json& value, Vector3& out) {
    return McpValues::parseVector3(value, out) && std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

Vec3d toVec3d(const Vector3& v) {
    return Vec3d{v.x, v.y, v.z};
}

void failCode(const CallPtr& call, const std::string& code, const std::string& message, Json extra = Json::object()) {
    extra["error"] = code;
    extra["message"] = message;
    call->fail(code + ": " + message, std::move(extra));
}

// The volume's frame as plain values, so a worker can convert without the system
struct Frame {
    Matrix4 model;
    Matrix4 inverse;
    Quaternion inverseRotation;

    Vec3d local(const Vector3& world) const { return toVec3d(inverse * world); }
    Vector3 world(const Vec3d& point) const {
        return model * Vector3(static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z));
    }
    voxel::Quatd localRotation(const Quaternion& rotation) const {
        const Quaternion r = inverseRotation * rotation;
        return voxel::Quatd{r.w, r.x, r.y, r.z};
    }
};

Frame frameFrom(const Matrix4& model, const Quaternion& worldRotation) {
    Frame frame;
    frame.model = model;
    frame.inverse = model.inverse();
    frame.inverseRotation = worldRotation.inverse();
    return frame;
}

// Samples to metres in world space, over the outer corners of the box
Json worldBoxJson(const Frame& frame, const IntBox& box, double voxelSize) {
    if (box.empty()) return nullptr;
    const float size = static_cast<float>(voxelSize);
    const AABB local(box.min.x * size, box.min.y * size, box.min.z * size, box.max.x * size, box.max.y * size, box.max.z * size);
    const AABB world = frame.model * local;
    return {{"min", vecJson(world.getMinimum())}, {"max", vecJson(world.getMaximum())}};
}

Json voxelBoxJson(const IntBox& box) {
    if (box.empty()) return nullptr;
    return {{"min", intsJson(box.min)}, {"max", intsJson(box.max)}};
}

// ------------------------------------------------------------------ which volume

struct Target {
    SceneProject* sourceScene = nullptr;
    SceneProject* sp = nullptr;          // the running copy in Play
    Entity entity = NULL_ENTITY;
    std::shared_ptr<VoxelSystem> system;
    Store* store = nullptr;
    std::shared_ptr<VoxelAsset> asset;
    Frame frame;
    bool playing = false;
};

bool resolveScene(const CallPtr& call, Target& target) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    target.sourceScene = McpRefs::resolveScene(project, has(args, "scene") ? args["scene"] : Json(), error);
    if (!target.sourceScene || !target.sourceScene->scene) {
        call->fail(error.empty() ? "The scene is not loaded." : error);
        return false;
    }
    target.sp = McpRefs::runtimeScene(project, target.sourceScene);
    target.playing = target.sp != target.sourceScene || target.sp->playState != ScenePlayState::STOPPED;
    target.system = target.sp->scene->findSystem<VoxelSystem>();
    if (!target.system) {
        call->fail("This scene has no voxel system.");
        return false;
    }
    return true;
}

// The GameObject: entity, else the only one with a Voxel Volume
bool resolveEntity(const CallPtr& call, Target& target) {
    const Json& args = call->args();
    Scene* scene = target.sp->scene;
    std::string error;
    if (has(args, "entity")) {
        target.entity = McpRefs::resolveEntity(target.sp, args["entity"], error);
        if (target.entity == NULL_ENTITY) {
            call->fail(error);
            return false;
        }
        if (!scene->findComponent<VoxelVolumeComponent>(target.entity)) {
            call->fail(McpRefs::entityPath(target.sp, target.entity) + " has no Voxel Volume: edit_voxels {create} makes a GameObject with one.");
            return false;
        }
        return true;
    }
    for (Entity entity : target.sp->entities) {
        if (!scene->findComponent<VoxelVolumeComponent>(entity)) continue;
        if (target.entity != NULL_ENTITY) {
            call->fail("Several GameObjects have a Voxel Volume: pass entity.");
            return false;
        }
        target.entity = entity;
    }
    if (target.entity == NULL_ENTITY) {
        call->fail("No GameObject here has a Voxel Volume: edit_voxels {create} makes one.");
        return false;
    }
    return true;
}

// A volume added or enabled this frame is known to the system after its next update (R19): a tool
// must not depend on when the frame falls
bool resolveStore(const CallPtr& call, Target& target) {
    target.store = target.system->getStore(target.entity);
    if (!target.store) {
        target.system->update(0.0);
        target.store = target.system->getStore(target.entity);
    }
    if (!target.store) {
        const std::string why = target.system->getError(target.entity);
        failCode(call, "not_loaded", why.empty() ? "The volume is disabled, hidden or has no Volume Data." : why);
        return false;
    }
    target.asset = target.system->getAsset(target.entity);
    const Transform* transform = target.sp->scene->findComponent<Transform>(target.entity);
    target.frame = frameFrom(target.system->modelMatrixOf(target.entity), transform ? transform->worldRotation : Quaternion());
    return true;
}

bool resolveVolume(const CallPtr& call, Target& target) {
    return resolveScene(call, target) && resolveEntity(call, target) && resolveStore(call, target);
}

// ------------------------------------------------------------------ jobs

// Every volume a job is working on, by asset: two GameObjects can show one file (R66)
std::map<std::string, JobPtr>& busyVolumes() {
    static std::map<std::string, JobPtr> volumes;
    return volumes;
}

JobPtr busyJob(const std::string& assetPath) {
    auto& volumes = busyVolumes();
    const auto found = volumes.find(assetPath);
    if (found == volumes.end()) return nullptr;
    if (!found->second->finished()) return found->second;
    volumes.erase(found);
    return nullptr;
}

bool refuseIfBusy(const CallPtr& call, const std::string& assetPath) {
    const JobPtr job = busyJob(assetPath);
    if (!job) return false;
    failCode(call, "volume_busy", "Job " + job->id() + " (" + job->title() + ") is working on this volume.",
             {{"job_id", job->id()}, {"next", "get_job {job_id: \"" + job->id() + "\"} until it ends, then call again"}});
    return true;
}

// Worker's result, looked at on the main thread once per editor frame, as VatTools' bake does
void whenWorkerDone(McpContext* ctx, std::shared_ptr<std::atomic<bool>> done, std::function<void()> finish) {
    ctx->app().requestRedraw();
    getEditorHost().enqueueMainThreadTask([ctx, done, finish]() {
        if (!done->load()) {
            whenWorkerDone(ctx, done, finish);
            return;
        }
        // a continuation writes beneath the project, as the tool's run did
        McpWriteScope writeScope;
        finish();
    });
}

void replyJobStarted(const CallPtr& call, const JobPtr& job) {
    Json data = {{"job_id", job->id()}, {"status", jobStatusName(job->status())},
                 {"next", "get_job {job_id: \"" + job->id() + "\"} until it ends; cancel_job stops it"}};
    call->ok("Job " + job->id() + " (" + job->title() + ") started.", std::move(data));
}

// A read on a private clone: nothing to bring back but its answer
void runReadJob(const CallPtr& call, const Store& store, const std::string& kind, const std::string& title,
                std::function<Json(Store&)> compute) {
    JobPtr job = call->ctx().jobs().create(kind, title);
    std::shared_ptr<Store> clone(store.clone());
    job->setProgress(-1.0, "reading");
    std::thread([job, clone, compute = std::move(compute)]() {
        try {
            Json data = compute(*clone);
            const std::string text = data.value("summary", std::string("Done."));
            job->succeed(text, std::move(data));
        } catch (const std::exception& e) {
            job->fail(e.what());
        }
    }).detach();
    replyJobStarted(call, job);
}

// ------------------------------------------------------------------ edit_voxels: parsing

const char* editKindName(EditKind kind) {
    switch (kind) {
        case EditKind::Dig: return "Dig";
        case EditKind::Add: return "Add";
        case EditKind::Smooth: return "Smooth";
        case EditKind::Paint: return "Paint";
    }
    return "Edit";
}

const char* shapeKindName(ShapeKind kind) {
    switch (kind) {
        case ShapeKind::Sphere: return "Sphere";
        case ShapeKind::Box: return "Box";
        case ShapeKind::Capsule: return "Capsule";
        case ShapeKind::Mesh: return "Mesh";
        case ShapeKind::Stamp: return "Stamp";
    }
    return "Shape";
}

// A parsed op. A mesh or a stamp makes its field where the work runs, since voxelizing is the long
// part of such an op (R36); what it reads of the scene it read while parsing.
struct ParsedOp {
    EditOp op;
    std::function<std::shared_ptr<const voxel::StampField>()> buildField;
    int64_t fieldCost = 0;
};

struct ParseFailure {
    std::string code;
    std::string message;
};

// A stamp's level 0: its SDF is the shape (R26). The clone keeps reading it safe wherever this runs.
std::shared_ptr<const voxel::StampField> stampFieldOf(const std::shared_ptr<Store>& store) {
    const Int3 samples = store->levelSamples(0);
    auto field = std::make_shared<voxel::StampField>();
    field->size = samples;
    field->voxelSize = store->desc().voxelSize;
    field->sdf.resize(static_cast<size_t>(samples.x) * samples.y * samples.z);
    size_t i = 0;
    for (int z = 0; z < samples.z; z++) {
        for (int y = 0; y < samples.y; y++) {
            for (int x = 0; x < samples.x; x++) field->sdf[i++] = store->sdfAt(0, x, y, z);
        }
    }
    field->originOffset[0] = samples.x * field->voxelSize * 0.5;
    field->originOffset[1] = samples.y * field->voxelSize * 0.5;
    field->originOffset[2] = samples.z * field->voxelSize * 0.5;
    return field;
}

bool readShapeNumber(const Json& object, const char* key, double& out) {
    if (!object[key].is_number()) return false;
    out = object[key].get<double>();
    return std::isfinite(out);
}

bool parseMeshShape(SceneProject* sp, const Json& shapeJson, const Frame& frame, double voxelSize, ParsedOp& parsed, ParseFailure& failure) {
    std::string error;
    const Entity source = McpRefs::resolveEntity(sp, shapeJson["mesh"], error);
    MeshComponent* mesh = source != NULL_ENTITY ? sp->scene->findComponent<MeshComponent>(source) : nullptr;
    if (!mesh) {
        failure = {"missing_asset", "shape.mesh is a GameObject with a Mesh: " + (error.empty() ? std::string("this one has none") : error)};
        return false;
    }
    const Transform* transform = sp->scene->findComponent<Transform>(source);
    if (!transform) {
        failure = {"missing_asset", "shape.mesh has no Transform."};
        return false;
    }
    const Vector3 scale = transform->worldScale;
    if (std::abs(scale.x - scale.y) > 1e-4f || std::abs(scale.x - scale.z) > 1e-4f) {
        failure = {"invalid_shape", "A mesh shape scales uniformly (R26), and this GameObject's scale is not uniform."};
        return false;
    }
    auto vertices = std::make_shared<std::vector<Vector3>>();
    auto indices = std::make_shared<std::vector<uint32_t>>();
    MeshTriangles::appendMeshTriangles(mesh, Matrix4(), *vertices, *indices);
    if (indices->empty()) {
        failure = {"missing_asset", "shape.mesh has no triangles yet."};
        return false;
    }
    parsed.op.shape.center = frame.local(transform->worldPosition);
    parsed.op.shape.rotation = frame.localRotation(transform->worldRotation);
    parsed.op.shape.scale *= scale.x;
    // The cost of a voxelization: every triangle's neighbourhood, then the box's sign rays
    AABB bounds;
    for (const Vector3& v : *vertices) bounds.merge(v);
    const Vector3 size = (bounds.getMaximum() - bounds.getMinimum()) / static_cast<float>(voxelSize);
    parsed.fieldCost = static_cast<int64_t>(indices->size() / 3) * 512 + static_cast<int64_t>((size.x + 1.0f) * (size.y + 1.0f) * (size.z + 1.0f));
    parsed.buildField = [vertices, indices, voxelSize]() -> std::shared_ptr<const voxel::StampField> {
        std::vector<float> positions;
        positions.reserve(vertices->size() * 3);
        for (const Vector3& v : *vertices) {
            positions.push_back(v.x);
            positions.push_back(v.y);
            positions.push_back(v.z);
        }
        voxel::VoxelizeOptions options;
        options.voxelSize = voxelSize;
        return voxel::voxelizeMesh(positions, *indices, options);
    };
    return true;
}

bool parseStampShape(Project* project, const Json& shapeJson, ParsedOp& parsed, ParseFailure& failure) {
    if (!shapeJson["stamp"].is_string()) {
        failure = {"invalid_shape", "shape.stamp is the path of a .voxelvolume."};
        return false;
    }
    const std::string path = shapeJson["stamp"].get<std::string>();
    fs::path absolute;
    std::string error;
    const bool found = McpRefs::resolveProjectPath(project, path, absolute, error, true);
    const std::shared_ptr<VoxelAsset> asset = found ? VoxelAssets::acquire(FileUtils::pathToUtf8(absolute.lexically_proximate(project->getAssetsPath()))) : nullptr;
    if (!asset || !asset->store) {
        failure = {"missing_asset", "shape.stamp \"" + path + "\" did not load" + (error.empty() ? std::string(".") : ": " + error)};
        return false;
    }
    std::shared_ptr<Store> clone(asset->store->clone());
    const Int3 samples = clone->levelSamples(0);
    parsed.fieldCost = static_cast<int64_t>(samples.x) * samples.y * samples.z;
    parsed.buildField = [clone]() { return stampFieldOf(clone); };
    return true;
}

bool parseShape(const CallPtr& call, SceneProject* sp, const Json& shapeJson, const Frame& frame, double voxelSize, ParsedOp& parsed, ParseFailure& failure) {
    std::string error;
    if (!shapeJson.is_object() || !has(shapeJson, "type") || !shapeJson["type"].is_string()) {
        failure = {"invalid_shape", "shape is {type: sphere|box|capsule|mesh|stamp, ...} (" + std::string(DOCS) + ")"};
        return false;
    }
    const std::string type = shapeJson["type"].get<std::string>();
    voxel::Shape& shape = parsed.op.shape;
    if (type == "sphere") {
        shape.kind = ShapeKind::Sphere;
        if (!closedKeys(shapeJson, {"type", "center", "radius"}, "sphere", error)) { failure = {"invalid_shape", error}; return false; }
    } else if (type == "box") {
        shape.kind = ShapeKind::Box;
        if (!closedKeys(shapeJson, {"type", "center", "half_extents", "rotation"}, "box", error)) { failure = {"invalid_shape", error}; return false; }
    } else if (type == "capsule") {
        shape.kind = ShapeKind::Capsule;
        if (!closedKeys(shapeJson, {"type", "from", "to", "radius"}, "capsule", error)) { failure = {"invalid_shape", error}; return false; }
    } else if (type == "mesh") {
        shape.kind = ShapeKind::Mesh;
        if (!closedKeys(shapeJson, {"type", "mesh", "scale"}, "mesh", error)) { failure = {"invalid_shape", error}; return false; }
    } else if (type == "stamp") {
        shape.kind = ShapeKind::Stamp;
        if (!closedKeys(shapeJson, {"type", "stamp", "center", "rotation", "scale"}, "stamp", error)) { failure = {"invalid_shape", error}; return false; }
    } else {
        failure = {"invalid_shape", "shape.type \"" + type + "\" is not sphere, box, capsule, mesh or stamp."};
        return false;
    }

    const auto needed = [&](const char* key) {
        if (has(shapeJson, key)) return true;
        failure = {"invalid_shape", type + " needs " + key + "."};
        return false;
    };
    const auto point = [&](const char* key, Vec3d& out) {
        Vector3 world;
        if (!readVec3(shapeJson[key], world)) {
            failure = {"invalid_shape", std::string(key) + " must be [x, y, z] in world metres."};
            return false;
        }
        out = frame.local(world);
        return true;
    };
    const auto rotation = [&]() {
        if (!has(shapeJson, "rotation")) {
            shape.rotation = frame.localRotation(Quaternion());
            return true;
        }
        Quaternion q;
        if (!McpValues::parseQuaternion(shapeJson["rotation"], q)) {
            failure = {"invalid_shape", "rotation is [x,y,z,w], {euler: [x,y,z]} in degrees or {quat: [x,y,z,w]}."};
            return false;
        }
        shape.rotation = frame.localRotation(q);
        return true;
    };
    const auto radius = [&]() {
        if (!needed("radius")) return false;
        if (!readShapeNumber(shapeJson, "radius", shape.radius)) {
            failure = {"invalid_shape", "radius must be metres."};
            return false;
        }
        return true;
    };
    const auto scale = [&]() {
        if (!has(shapeJson, "scale")) return true;
        if (!readShapeNumber(shapeJson, "scale", shape.scale)) {
            failure = {"invalid_shape", "scale must be a number."};
            return false;
        }
        return true;
    };

    switch (shape.kind) {
        case ShapeKind::Sphere:
            return needed("center") && point("center", shape.center) && radius();
        case ShapeKind::Box: {
            if (!needed("center") || !needed("half_extents") || !point("center", shape.center) || !rotation()) return false;
            Vector3 half;
            if (!readVec3(shapeJson["half_extents"], half)) {
                failure = {"invalid_shape", "half_extents must be [x, y, z] in metres."};
                return false;
            }
            shape.halfExtents = toVec3d(half);
            return true;
        }
        case ShapeKind::Capsule:
            return needed("from") && needed("to") && point("from", shape.from) && point("to", shape.to) && radius();
        case ShapeKind::Mesh:
            return needed("mesh") && scale() && parseMeshShape(sp, shapeJson, frame, voxelSize, parsed, failure);
        case ShapeKind::Stamp:
            return needed("stamp") && needed("center") && point("center", shape.center) && rotation() && scale() &&
                   parseStampShape(call->ctx().project(), shapeJson, parsed, failure);
    }
    return false;
}

bool parseOps(const CallPtr& call, SceneProject* sp, const Json& opsJson, const Frame& frame, double voxelSize, std::vector<ParsedOp>& out, ParseFailure& failure, int& failedOp) {
    if (!opsJson.is_array() || opsJson.empty()) {
        failedOp = -1;
        failure = {"invalid_shape", "ops is a non-empty list of {op, shape, ...} (" + std::string(DOCS) + ")"};
        return false;
    }
    for (size_t i = 0; i < opsJson.size(); i++) {
        failedOp = static_cast<int>(i);
        const Json& opJson = opsJson[i];
        std::string error;
        if (!opJson.is_object()) {
            failure = {"invalid_shape", "An op is {op, shape, ...}."};
            return false;
        }
        if (!closedKeys(opJson, {"op", "shape", "blend", "material", "weight", "falloff", "amount", "iterations"}, "op", error)) {
            failure = {"invalid_shape", error};
            return false;
        }
        ParsedOp parsed;
        const std::string name = has(opJson, "op") && opJson["op"].is_string() ? opJson["op"].get<std::string>() : "";
        if (name == "dig") parsed.op.kind = EditKind::Dig;
        else if (name == "add") parsed.op.kind = EditKind::Add;
        else if (name == "smooth") parsed.op.kind = EditKind::Smooth;
        else if (name == "paint") parsed.op.kind = EditKind::Paint;
        else {
            failure = {"invalid_shape", "op is dig, add, smooth or paint" + (name.empty() ? std::string(".") : std::string(", not \"") + name + "\".")};
            return false;
        }
        if (!has(opJson, "shape")) {
            failure = {"invalid_shape", "An op needs a shape."};
            return false;
        }
        const auto number = [&](const char* key, double& value) {
            if (!has(opJson, key)) return true;
            if (readShapeNumber(opJson, key, value)) return true;
            failure = {"invalid_shape", std::string(key) + " must be a number."};
            return false;
        };
        if (!number("blend", parsed.op.blend) || !number("weight", parsed.op.weight) || !number("falloff", parsed.op.falloff) ||
            !number("amount", parsed.op.amount)) {
            return false;
        }
        if (has(opJson, "material")) {
            if (!opJson["material"].is_number_integer()) {
                failure = {"invalid_material", "material is a slot number."};
                return false;
            }
            parsed.op.material = opJson["material"].get<int>();
        }
        if (has(opJson, "iterations")) {
            if (!opJson["iterations"].is_number_integer()) {
                failure = {"invalid_shape", "iterations is an integer, 1-8."};
                return false;
            }
            parsed.op.iterations = opJson["iterations"].get<int>();
        }
        if (!parseShape(call, sp, opJson["shape"], frame, voxelSize, parsed, failure)) return false;
        out.push_back(std::move(parsed));
    }
    failedOp = -1;
    return true;
}

std::vector<EditOp> finishedOps(std::vector<ParsedOp>& parsed) {
    std::vector<EditOp> ops;
    ops.reserve(parsed.size());
    for (ParsedOp& item : parsed) {
        if (item.buildField) item.op.shape.field = item.buildField();
        ops.push_back(item.op);
    }
    return ops;
}

// ------------------------------------------------------------------ edit_voxels: results

Json editResultJson(const EditResult& result, const Frame& frame, double voxelSize) {
    Json removed = Json::array();
    for (double value : result.removedByMaterial) removed.push_back(value);
    Json data = {
        {"voxels_changed", result.voxelsChanged},
        {"surface_voxels_changed", result.surfaceVoxelsChanged},
        {"blocks_touched", result.blocksTouched},
        {"blocks_allocated", result.blocksAllocated},
        {"blocks_collapsed", result.blocksCollapsed},
        {"memory_delta", result.memoryDelta},
        {"clamped_voxels", result.clampedVoxels},
        {"removed_volume", result.removedVolume},
        {"added_volume", result.addedVolume},
        {"removed_by_material", std::move(removed)},
        {"holes_cut", result.holesCut},
        {"holes_filled", result.holesFilled},
        {"changed_bounds", {{"voxels", voxelBoxJson(result.changedVoxels)}, {"world", worldBoxJson(frame, result.changedVoxels, voxelSize)}}},
    };
    if (result.hasHash) data["content_hash_after"] = hexHash(result.contentHashAfter);
    return data;
}

// What the agent reacts to, where it reads it (R82)
void addEditHints(Json& data, const EditResult& result) {
    if (result.entranceNarrow) {
        data["entrance_narrow"] = true;
        data["next"] = "The cave mouth is narrower than a terrain hole can follow: widen it with a larger dig at the mouth.";
    }
}

Json editSummaryJson(const EditResult& result, const Frame& frame, double voxelSize) {
    Json data = editResultJson(result, frame, voxelSize);
    if (result.perOp.size() > 1) {
        Json perOp = Json::array();
        for (const EditResult& one : result.perOp) perOp.push_back(editResultJson(one, frame, voxelSize));
        data["per_op"] = std::move(perOp);
    }
    addEditHints(data, result);
    return data;
}

std::string editErrorFix(EditError error, int slotCount) {
    switch (error) {
        case EditError::OutOfBounds:
            return "A shape lies wholly outside the volume's editable region. Positions are world metres: sample_voxels {mode: bounds} gives the box.";
        case EditError::InvalidShape:
            return "A shape or parameter is out of range: radius, half_extents and scale above 0, blend from 0, weight, falloff and amount 0-1, iterations 1-8.";
        case EditError::InvalidMaterial:
            return "material is a slot of this volume, 0-" + std::to_string(slotCount - 1) + "; paint needs one.";
        case EditError::MissingAsset:
            return "A mesh or stamp shape has no usable asset.";
        case EditError::BudgetExceeded:
            return "Over the memory budget or one call's budget: split it into smaller shapes. sample_voxels {mode: stats} shows the budgets.";
        case EditError::NotLoaded:
            return "The volume is not loaded.";
        case EditError::None:
            break;
    }
    return "";
}

std::string undoName(const std::vector<ParsedOp>& ops) {
    if (ops.size() != 1) return "Edit Voxels";
    return std::string(editKindName(ops[0].op.kind)) + " " + shapeKindName(ops[0].op.shape.kind);
}

std::string editText(const EditResult& result, const std::string& what) {
    return what + ": " + std::to_string(result.voxelsChanged) + " voxels changed, " + std::to_string(result.blocksTouched) + " blocks touched.";
}

// What a call's shapes reach, as the budget of R30 counts it, with smoothing's repeated passes
int64_t editCost(const Store& store, const std::vector<ParsedOp>& ops) {
    const Int3 dimensions = store.desc().dimensions;
    int64_t cost = 0;
    for (const ParsedOp& item : ops) {
        const IntBox box = voxel::shapeSampleBox(store.desc(), item.op.shape);
        const int64_t x = std::max(0, std::min(box.max.x, dimensions.x) - std::max(box.min.x, 0) + 1);
        const int64_t y = std::max(0, std::min(box.max.y, dimensions.y) - std::max(box.min.y, 0) + 1);
        const int64_t z = std::max(0, std::min(box.max.z, dimensions.z) - std::max(box.min.z, 0) + 1);
        const bool field = item.op.shape.kind == ShapeKind::Mesh || item.op.shape.kind == ShapeKind::Stamp;
        // a field shape's box is only known once it is built: it can reach all of the volume
        const int64_t samples = field ? static_cast<int64_t>(dimensions.x) * dimensions.y * dimensions.z : x * y * z;
        cost += samples * (item.op.kind == EditKind::Smooth ? item.op.iterations : 1) + item.fieldCost;
    }
    return cost;
}

// R64's count of what a read must first resolve, over the blocks of a box of samples
int64_t readCost(const Store& store, const IntBox& box) {
    const Int3 blocks = store.blockCount(0);
    int64_t cost = 0;
    const int minX = std::max(0, box.min.x >> voxel::BLOCK_SHIFT), maxX = std::min(blocks.x - 1, box.max.x >> voxel::BLOCK_SHIFT);
    const int minY = std::max(0, box.min.y >> voxel::BLOCK_SHIFT), maxY = std::min(blocks.y - 1, box.max.y >> voxel::BLOCK_SHIFT);
    const int minZ = std::max(0, box.min.z >> voxel::BLOCK_SHIFT), maxZ = std::min(blocks.z - 1, box.max.z >> voxel::BLOCK_SHIFT);
    for (int z = minZ; z <= maxZ; z++) {
        for (int y = minY; y <= maxY; y++) {
            for (int x = minX; x <= maxX; x++) {
                const voxel::Slot::Kind kind = store.slot(voxel::BlockKey{0, x, y, z}).kind();
                if (kind == voxel::Slot::Base || kind == voxel::Slot::Cold) cost += READ_COST_UNRESOLVED;
                else if (kind == voxel::Slot::Hot) cost += READ_COST_RESIDENT;
            }
        }
    }
    return cost;
}

IntBox wholeVolume(const Store& store) {
    const Int3 samples = store.levelSamples(0);
    IntBox box;
    box.min = Int3{0, 0, 0};
    box.max = Int3{samples.x - 1, samples.y - 1, samples.z - 1};
    return box;
}

// ------------------------------------------------------------------ edit_voxels: applying

// The whole call on a clone, on a worker; the main thread then applies the delta in one step
void runEditJob(const CallPtr& call, Target target, std::vector<ParsedOp> parsed, bool dryRun, bool wantHash) {
    McpContext* ctx = &call->ctx();
    JobPtr job = ctx->jobs().create("edit_voxels", undoName(parsed) + " (" + target.sp->scene->getEntityName(target.entity) + ")");
    std::string name = undoName(parsed);
    busyVolumes()[target.asset->path] = job;

    struct Work {
        std::shared_ptr<Store> clone;
        std::vector<ParsedOp> parsed;
        EditOptions options;
        EditResult result;
        std::string exception;
    };
    auto work = std::make_shared<Work>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    work->clone = std::shared_ptr<Store>(target.store->clone());
    work->parsed = std::move(parsed);
    work->options.dryRun = dryRun;
    work->options.hash = wantHash;
    work->options.captureDelta = !dryRun;
    // what's left of R7's shared budget for this store, which the clone's edit must also respect
    if (const std::shared_ptr<voxel::MemoryBudget>& shared = target.store->budget()) {
        work->clone->setBudget(std::make_shared<voxel::MemoryBudget>(shared->capacity() - shared->used() + target.store->memory().total()));
    }
    const VoxelVolumeComponent* component = target.sp->scene->findComponent<VoxelVolumeComponent>(target.entity);
    if (component && !component->materialSlots.empty()) work->options.slotCount = std::min<int>(static_cast<int>(component->materialSlots.size()), voxel::MAX_SLOTS);
    else if (target.asset->slotCount > 0) work->options.slotCount = target.asset->slotCount;
    const uint64_t version = target.store->version();
    const int slotCount = work->options.slotCount;
    const double voxelSize = target.store->desc().voxelSize;
    const uint32_t sceneId = target.sp->id;
    const Entity entity = target.entity;
    const Frame frame = target.frame;
    const std::shared_ptr<VoxelAsset> asset = target.asset;

    job->setProgress(-1.0, "editing");
    // no cancel handler would mark it cancelled at once, with the worker still on the clone
    job->setCancelHandler([]() {});
    std::thread([work, done]() {
        try {
            const std::vector<EditOp> ops = finishedOps(work->parsed);
            work->result = voxel::applyEdit(*work->clone, ops, work->options);
        } catch (const std::exception& e) {
            work->exception = e.what();
        }
        done->store(true);
    }).detach();

    whenWorkerDone(ctx, done, [ctx, job, work, version, slotCount, voxelSize, sceneId, entity, frame, asset, name, dryRun]() {
        busyVolumes().erase(asset->path);
        work->clone.reset();
        if (!work->exception.empty()) {
            job->fail("The edit failed, nothing changed: " + work->exception);
            return;
        }
        const EditResult& result = work->result;
        if (!result.ok) {
            const std::string code = voxel::editErrorCode(result.error);
            job->fail(code + ": " + editErrorFix(result.error, slotCount), {{"error", code}});
            return;
        }
        if (job->cancelRequested()) {
            job->markCancelled("Cancelled; nothing changed.");
            return;
        }
        Json data = editSummaryJson(result, frame, voxelSize);
        if (dryRun) {
            data["dry_run"] = true;
            job->succeed(editText(result, "Dry run"), std::move(data));
            return;
        }
        Project* project = ctx->project();
        SceneProject* sp = project ? project->getScene(sceneId) : nullptr;
        std::shared_ptr<VoxelSystem> system = sp && sp->scene ? sp->scene->findSystem<VoxelSystem>() : nullptr;
        // a person's stroke or undo in the meantime: the delta was made over other content
        if (!system || system->getAsset(entity) != asset || !system->getStore(entity) || system->getStore(entity)->version() != version) {
            job->fail("The volume changed while the job ran, nothing was applied: edit_voxels again.");
            return;
        }
        system->applyDelta(entity, result.delta, false);
        CommandHandle::get(sceneId)->addCommandNoMerge(new VoxelEditCmd(project, sceneId, entity, name, result.delta));
        job->succeed(editText(result, name), std::move(data));
    });
    replyJobStarted(call, job);
}

// One step for the whole call. False: the call has been failed and nothing changed.
bool applyOps(const CallPtr& call, Target& target, std::vector<ParsedOp>& parsed, bool dryRun, bool wantHash, Json& data, std::string& text) {
    std::vector<EditOp> ops = finishedOps(parsed);
    EditOptions options;
    options.dryRun = dryRun;
    options.hash = wantHash;
    // No Play edit is undoable (R30)
    options.captureDelta = !dryRun && !target.playing;
    const EditResult result = target.system->edit(target.entity, ops, options);
    if (!result.ok) {
        const VoxelVolumeComponent* component = target.sp->scene->findComponent<VoxelVolumeComponent>(target.entity);
        const int slots = component && !component->materialSlots.empty() ? static_cast<int>(component->materialSlots.size())
                                                                         : (target.asset->slotCount > 0 ? target.asset->slotCount : voxel::MAX_SLOTS);
        failCode(call, voxel::editErrorCode(result.error), editErrorFix(result.error, slots));
        return false;
    }
    const double voxelSize = target.store->desc().voxelSize;
    data.update(editSummaryJson(result, target.frame, voxelSize));
    text = editText(result, dryRun ? "Dry run" : undoName(parsed));
    if (!dryRun && !target.playing && !result.delta.empty()) {
        CommandHandle::get(target.sp->id)->addCommandNoMerge(new VoxelEditCmd(call->ctx().project(), target.sp->id, target.entity, undoName(parsed), result.delta));
    }
    return true;
}

// ------------------------------------------------------------------ edit_voxels: create

struct CreateRequest {
    std::string name = "Voxel Volume";
    Entity parent = NULL_ENTITY;
    Vector3 position;
    Quaternion rotation;
    bool hasRotation = false;
    voxel::VolumeDesc desc;
    voxel::GeneratorDesc generator;
    VoxelCreateBase base = VoxelCreateBase::Empty;
    bool generated = false;             // the base takes generate_voxels' steps, as a job
    double blendWidth = 0.0;
    Entity terrain = NULL_ENTITY;
    std::string assetPath;              // relative to the assets folder
    std::vector<VoxelMaterialSlot> materials;
};

// R67: the dialog's estimate (R59), so the two never disagree
Json createEstimateJson(const CreateRequest& request, const VoxelProjectSettings& settings, int64_t& bytes) {
    VoxelCreateOptions options;
    options.preset = VoxelCreatePreset::Custom;
    options.dimensions = request.desc.dimensions;
    options.voxelSize = request.desc.voxelSize;
    options.base = request.base;
    options.terrain = request.terrain;
    const VoxelCreateEstimate estimate = estimateVoxelCreate(options, settings);
    bytes = estimate.bytes;
    Json targets = Json::array();
    for (const std::string& target : estimate.overBudgetTargets) targets.push_back(target);
    for (const std::string& target : estimate.oversizeTargets) {
        if (std::find(targets.begin(), targets.end(), target) == targets.end()) targets.push_back(target);
    }
    return targets;
}

// A new file's path, relative to the assets folder; the project's sandbox decides what it may be
bool chooseAssetPath(Project* project, const Json& create, const std::string& name, std::string& out, std::string& error) {
    const fs::path assets = project->getAssetsPath();
    if (has(create, "asset_path")) {
        if (!create["asset_path"].is_string()) {
            error = "asset_path is a project-relative path ending in .voxelvolume.";
            return false;
        }
        const std::string relative = create["asset_path"].get<std::string>();
        fs::path absolute;
        if (!McpRefs::resolveProjectWritePath(project, relative, absolute, error)) return false;
        if (absolute.extension() != ".voxelvolume") {
            error = "asset_path must end in .voxelvolume.";
            return false;
        }
        if (fs::exists(absolute)) {
            error = "asset_path \"" + relative + "\" exists; choose another path.";
            return false;
        }
        out = FileUtils::pathToUtf8(absolute.lexically_normal().lexically_proximate(assets));
        if (out.rfind("..", 0) == 0) {
            error = "asset_path must be inside the project's assets folder.";
            return false;
        }
        return true;
    }
    std::string stem;
    for (char c : name) stem += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    for (int i = 0; i < 10000; i++) {
        const std::string candidate = "voxels/" + stem + (i ? "_" + std::to_string(i) : "") + ".voxelvolume";
        if (!fs::exists(assets / FileUtils::pathFromUtf8(candidate))) {
            out = candidate;
            return true;
        }
    }
    error = "No free file name for this volume: pass asset_path.";
    return false;
}

bool parseMaterials(const Json& materials, std::vector<VoxelMaterialSlot>& out, std::string& error) {
    if (!materials.is_array() || materials.size() > voxel::MAX_SLOTS) {
        error = "materials is a list of up to " + std::to_string(voxel::MAX_SLOTS) + " {name, color, roughness, metallic, linked_terrain_layer}.";
        return false;
    }
    for (const Json& item : materials) {
        if (!item.is_object() || !closedKeys(item, {"name", "color", "roughness", "metallic", "linked_terrain_layer"}, "material", error)) {
            if (error.empty()) error = "A material is {name, color, roughness, metallic, linked_terrain_layer}.";
            return false;
        }
        VoxelMaterialSlot slot;
        if (has(item, "name") && item["name"].is_string()) slot.name = item["name"].get<std::string>();
        if (has(item, "color")) {
            if (!McpValues::parseVector4(item["color"], slot.layer.colorFactor)) {
                Vector3 rgb;
                if (!readVec3(item["color"], rgb)) {
                    error = "material color is [r, g, b] or [r, g, b, a], linear 0-1.";
                    return false;
                }
                slot.layer.colorFactor = Vector4(rgb.x, rgb.y, rgb.z, 1.0f);
            }
        }
        if (has(item, "roughness") || has(item, "metallic")) slot.layer.pbr = true;
        if (has(item, "roughness") && item["roughness"].is_number()) slot.layer.roughnessFactor = item["roughness"].get<float>();
        if (has(item, "metallic") && item["metallic"].is_number()) slot.layer.metallicFactor = item["metallic"].get<float>();
        if (has(item, "linked_terrain_layer") && item["linked_terrain_layer"].is_number_integer()) slot.linkedTerrainLayer = item["linked_terrain_layer"].get<int>();
        out.push_back(std::move(slot));
    }
    return true;
}

// Everything checked before anything exists (R67). False: the call has been failed.
bool parseCreate(const CallPtr& call, Target& target, const Json& create, CreateRequest& request) {
    std::string error;
    if (!create.is_object()) {
        failCode(call, "invalid_arguments", "create is {size: [x, y, z], ...} (" + std::string(DOCS) + ")");
        return false;
    }
    if (has(create, "scale")) {
        Vector3 scale;
        const bool unit = readVec3(create["scale"], scale) && scale.x == 1.0f && scale.y == 1.0f && scale.z == 1.0f;
        if (!unit) {
            failCode(call, "invalid_arguments", "A volume's scale stays 1 (R1): choose voxel_size and size instead.");
            return false;
        }
    }
    if (!closedKeys(create, {"name", "parent", "position", "rotation", "scale", "size", "voxel_size", "base", "seed", "height", "terrain", "blend_width", "asset_path", "materials"}, "create", error)) {
        failCode(call, "invalid_arguments", error);
        return false;
    }
    SceneProject* sp = target.sp;
    if (has(create, "name")) {
        if (!create["name"].is_string() || create["name"].get<std::string>().empty()) {
            failCode(call, "invalid_arguments", "create.name is a non-empty string.");
            return false;
        }
        request.name = create["name"].get<std::string>();
    }
    if (has(create, "parent")) {
        request.parent = McpRefs::resolveEntity(sp, create["parent"], error);
        if (request.parent == NULL_ENTITY) {
            failCode(call, "invalid_arguments", error);
            return false;
        }
        const Transform* parent = sp->scene->findComponent<Transform>(request.parent);
        if (parent && (std::abs(parent->worldScale.x - 1.0f) > 1e-4f || std::abs(parent->worldScale.y - 1.0f) > 1e-4f || std::abs(parent->worldScale.z - 1.0f) > 1e-4f)) {
            failCode(call, "invalid_arguments", "A volume's scale stays 1 (R1), and this parent's world scale is not.");
            return false;
        }
    }
    if (has(create, "position") && !readVec3(create["position"], request.position)) {
        failCode(call, "invalid_arguments", "create.position must be [x, y, z] in world metres.");
        return false;
    }
    if (has(create, "rotation")) {
        if (!McpValues::parseQuaternion(create["rotation"], request.rotation)) {
            failCode(call, "invalid_arguments", "create.rotation is [x,y,z,w], {euler: [x,y,z]} in degrees or {quat: [x,y,z,w]}.");
            return false;
        }
        request.hasRotation = true;
    }
    if (!has(create, "size") || !create["size"].is_array() || create["size"].size() != 3) {
        failCode(call, "invalid_arguments", "create.size is [x, y, z] in voxels, multiples of 16, for example [64, 64, 64].");
        return false;
    }
    int size[3] = {0, 0, 0};
    const int maxDimension = voxelActiveLimits().maxDimension;
    for (int axis = 0; axis < 3; axis++) {
        const Json& value = create["size"][axis];
        if (!value.is_number_integer() || value.get<int64_t>() < voxel::BLOCK_SIZE || value.get<int64_t>() > maxDimension || value.get<int64_t>() % voxel::BLOCK_SIZE != 0) {
            failCode(call, "invalid_arguments", "create.size is in voxels: each a multiple of 16, from 16 to " + std::to_string(maxDimension) + ".");
            return false;
        }
        size[axis] = value.get<int>();
    }
    request.desc.dimensions = Int3{size[0], size[1], size[2]};
    request.desc.levels = voxel::MAX_LEVELS;
    if (has(create, "voxel_size")) {
        if (!create["voxel_size"].is_number() || !(create["voxel_size"].get<double>() > 0.0) || !std::isfinite(create["voxel_size"].get<double>())) {
            failCode(call, "invalid_arguments", "create.voxel_size is metres per voxel, above 0.");
            return false;
        }
        request.desc.voxelSize = create["voxel_size"].get<float>();
    }

    // R38: a terrain means its surface, unless the call says otherwise
    const std::string base = has(create, "base") && create["base"].is_string() ? create["base"].get<std::string>() : has(create, "terrain") ? "terrain" : "empty";
    if (base == "empty") request.base = VoxelCreateBase::Empty;
    else if (base == "solid_ground") request.base = VoxelCreateBase::SolidGround;
    else if (base == "terrain") request.base = VoxelCreateBase::FromTerrain;
    else if (base == "caves") request.base = VoxelCreateBase::Caves;
    else if (base == "overhangs") request.base = VoxelCreateBase::Overhangs;
    else if (base == "cave_entrance") request.base = VoxelCreateBase::CaveEntrance;
    else {
        failCode(call, "invalid_arguments", "create.base is empty, solid_ground, terrain, caves, overhangs or cave_entrance; "
                 "a Noise Graph or a mesh goes through generate_voxels once the volume exists.");
        return false;
    }
    request.generated = request.base != VoxelCreateBase::Empty && request.base != VoxelCreateBase::SolidGround;
    if (request.generated && has(call->args(), "ops")) {
        failCode(call, "invalid_arguments", "This base is generated as a job, so it can't take ops in the same call: create it, wait for get_job, then edit.");
        return false;
    }
    if ((request.base == VoxelCreateBase::FromTerrain || request.base == VoxelCreateBase::CaveEntrance) && !has(create, "terrain")) {
        failCode(call, "invalid_arguments", "create.base " + base + " needs terrain.");
        return false;
    }
    request.generator.kind = request.generated ? "empty" : base;
    if (has(create, "seed")) {
        if (!create["seed"].is_number_integer()) {
            failCode(call, "invalid_arguments", "create.seed is an integer.");
            return false;
        }
        request.generator.seed = create["seed"].get<uint64_t>();
    }
    if (base == "solid_ground") {
        request.generator.height = request.desc.dimensions.y * request.desc.voxelSize * 0.5;
        if (has(create, "height")) {
            if (!create["height"].is_number() || !std::isfinite(create["height"].get<double>())) {
                failCode(call, "invalid_arguments", "create.height is metres above the volume's origin.");
                return false;
            }
            request.generator.height = create["height"].get<double>();
        }
    } else if (has(create, "height")) {
        failCode(call, "invalid_arguments", "create.height belongs to base solid_ground.");
        return false;
    }
    if (has(create, "blend_width")) {
        if (!(create["blend_width"].is_number() && create["blend_width"].get<double>() >= 0.0)) {
            failCode(call, "invalid_arguments", "create.blend_width is metres, from 0.");
            return false;
        }
        request.blendWidth = create["blend_width"].get<double>();
    }
    if (has(create, "terrain")) {
        request.terrain = McpRefs::resolveEntity(sp, create["terrain"], error);
        if (request.terrain == NULL_ENTITY || !sp->scene->findComponent<TerrainComponent>(request.terrain)) {
            failCode(call, "missing_asset", error.empty() ? "create.terrain has no Terrain." : error);
            return false;
        }
        // R1: a volume on a terrain stays axis-aligned with it
        if (request.hasRotation && (request.rotation.x != 0.0f || request.rotation.y != 0.0f || request.rotation.z != 0.0f)) {
            failCode(call, "invalid_arguments", "A volume with a terrain must have no rotation (R1).");
            return false;
        }
        request.desc.bounds = voxel::BoundsMode::Open;
    }
    if (has(create, "materials") && !parseMaterials(create["materials"], request.materials, error)) {
        failCode(call, "invalid_material", error);
        return false;
    }
    if (request.materials.empty() && request.terrain != NULL_ENTITY) {
        // R38: one linked slot per terrain layer
        const TerrainComponent* terrain = sp->scene->findComponent<TerrainComponent>(request.terrain);
        const size_t count = std::min<size_t>(terrain->surfaceLayers.size(), voxel::MAX_SLOTS);
        for (size_t i = 0; i < count; i++) {
            VoxelMaterialSlot slot;
            slot.name = "Layer " + std::to_string(i);
            slot.linkedTerrainLayer = static_cast<int>(i);
            request.materials.push_back(std::move(slot));
        }
    }
    if (request.materials.empty()) {
        VoxelMaterialSlot slot;
        slot.name = "Default";
        request.materials.push_back(std::move(slot));
    }
    if (!chooseAssetPath(call->ctx().project(), create, request.name, request.assetPath, error)) {
        failCode(call, "invalid_arguments", error);
        return false;
    }
    return true;
}

Frame frameOfNew(const CreateRequest& request) {
    return frameFrom(Matrix4::translateMatrix(request.position) * request.rotation.getRotationMatrix(), request.rotation);
}

// The GameObject and its component in one command, whose redo brings the values back with them
bool createVolumeEntity(const CallPtr& call, Target& target, const CreateRequest& request, const AssetRef& ref) {
    Project* project = call->ctx().project();
    SceneProject* sp = target.sp;
    auto* create = new CreateEntityCmd(project, sp->id, request.name, EntityCreationType::VOXEL_VOLUME, request.parent);
    Vector3 localPosition = request.position;
    Quaternion localRotation = request.rotation;
    if (const Transform* parent = request.parent != NULL_ENTITY ? sp->scene->findComponent<Transform>(request.parent) : nullptr) {
        localPosition = parent->modelMatrix.inverse() * request.position;
        localRotation = parent->worldRotation.inverse() * request.rotation;
    }
    create->addProperty<Vector3>(ComponentType::Transform, "position", localPosition);
    create->addProperty<Quaternion>(ComponentType::Transform, "rotation", localRotation);
    create->addProperty<AssetRef>(ComponentType::VoxelVolumeComponent, "volumeData", ref);
    create->addProperty<std::vector<VoxelMaterialSlot>>(ComponentType::VoxelVolumeComponent, "materialSlots", request.materials);
    if (request.terrain != NULL_ENTITY) {
        create->addProperty<Entity>(ComponentType::VoxelVolumeComponent, "terrain", request.terrain);
        create->addProperty<int>(ComponentType::VoxelVolumeComponent, "terrainCut", static_cast<int>(VoxelTerrainCut::Automatic));
    }
    if (!CommandHandle::get(sp->id)->addCommandNoMerge(create)) {
        call->fail("Could not create the GameObject.");
        return false;
    }
    target.entity = create->getEntity();
    McpValues::refreshTransforms(sp);
    return resolveStore(call, target);
}

// ------------------------------------------------------------------ generation

// The worker of VoxelGenerationTask polled once per editor frame; it makes the undo step (R71).
// foldFrom: the position to fold that step down to, when a create in the same call made the one before.
void pollGeneration(McpContext* ctx, std::shared_ptr<VoxelGenerationTask> task, JobPtr job, std::string assetPath, uint32_t sceneId, size_t foldFrom, size_t foldOnlyAt) {
    ctx->app().requestRedraw();
    getEditorHost().enqueueMainThreadTask([ctx, task, job, assetPath, sceneId, foldFrom, foldOnlyAt]() {
        job->setProgress(task->progress());
        CommandHistory* history = CommandHandle::get(sceneId);
        const size_t position = history->position();
        VoxelGenerateResult result;
        McpWriteScope writeScope;
        if (!task->poll(result)) {
            pollGeneration(ctx, task, job, assetPath, sceneId, foldFrom, foldOnlyAt);
            return;
        }
        busyVolumes().erase(assetPath);
        if (job->cancelRequested()) {
            job->markCancelled("Cancelled; the volume is as it was.");
            return;
        }
        if (!result.ok) {
            job->fail(result.error + (result.message.empty() ? std::string() : ": " + result.message), {{"error", result.error}, {"message", result.message}});
            return;
        }
        // nothing else was done in between, or the fold would take it along
        if (foldOnlyAt != 0 && position == foldOnlyAt) history->groupSince(foldFrom, "Create Voxel Volume");
        Json data = {{"blocks_non_uniform", result.blocksNonUniform}, {"memory", result.memory}, {"triangles_estimate", result.trianglesEstimate},
                     {"content_hash", hexHash(result.contentHash)}, {"base_changed", result.baseChanged}, {"non_watertight", result.nonWatertight}};
        if (result.estimate) data["estimate"] = true;
        job->succeed(std::string(result.estimate ? "Estimated" : "Generated") + ": " + std::to_string(result.blocksNonUniform) + " non-uniform blocks, " +
                     std::to_string(result.memory) + " bytes.", std::move(data));
    });
}

// False, having failed the call, when the steps refuse. The job's id goes in `data`.
bool startGeneration(const CallPtr& call, Target& target, const VoxelGenerateRequest& request, const std::string& title, size_t foldFrom, size_t foldOnlyAt, Json& data) {
    McpContext* ctx = &call->ctx();
    auto task = std::make_shared<VoxelGenerationTask>();
    std::string error;
    if (!task->start(ctx->project(), target.sp->id, target.entity, request, true, error)) {
        failCode(call, "generate_refused", error);
        return false;
    }
    JobPtr job = ctx->jobs().create("generate_voxels", title);
    busyVolumes()[target.asset->path] = job;
    job->setProgress(0.0, "generating");
    // the task leaves everything as it was; the poll then ends the job
    job->setCancelHandler([task]() { task->cancel(); });
    pollGeneration(ctx, task, job, target.asset->path, target.sp->id, foldFrom, foldOnlyAt);
    data["job_id"] = job->id();
    data["status"] = jobStatusName(job->status());
    data["next"] = "get_job {job_id: \"" + job->id() + "\"} until it ends; cancel_job stops it";
    return true;
}

void editVoxelsTool(const CallPtr& call) {
    const Json& args = call->args();
    Target target;
    if (!resolveScene(call, target)) return;
    const bool dryRun = args.value("dry_run", false);
    const bool wantHash = args.value("hash", false);
    const bool hasCreate = has(args, "create");
    const bool hasOps = has(args, "ops");
    if (!hasCreate && !hasOps) {
        failCode(call, "invalid_arguments", "Pass ops, create, or both (" + std::string(DOCS) + ").");
        return;
    }
    if (hasCreate && has(args, "entity")) {
        failCode(call, "invalid_arguments", "create makes a new volume: leave entity out.");
        return;
    }

    if (hasCreate) {
        if (target.playing) {
            failCode(call, "invalid_arguments", "A volume is created in edit mode: stop Play first.");
            return;
        }
        CreateRequest request;
        if (!parseCreate(call, target, args["create"], request)) return;
        std::shared_ptr<VoxelAsset> asset = VoxelAssets::create(request.assetPath, request.desc, request.generator, static_cast<int>(request.materials.size()));
        int64_t memory = 0;
        Json targets = createEstimateJson(request, call->ctx().project()->getVoxelSettings(), memory);
        // project-relative, as every path a tool takes
        const std::string assetFile = FileUtils::pathToUtf8((call->ctx().project()->getAssetsPath() / FileUtils::pathFromUtf8(request.assetPath)).lexically_relative(call->ctx().project()->getProjectPath()));
        Json data = {{"asset", assetFile}, {"dimensions", intsJson(request.desc.dimensions)}, {"voxel_size", request.desc.voxelSize},
                     {"estimated_memory", memory}, {"over_budget_targets", std::move(targets)}};
        // the ops are checked on the new store before anything is written (R67)
        std::vector<ParsedOp> parsed;
        if (hasOps) {
            ParseFailure failure;
            int failedOp = -1;
            const Frame frame = frameOfNew(request);
            if (!parseOps(call, target.sp, args["ops"], frame, request.desc.voxelSize, parsed, failure, failedOp)) {
                VoxelAssets::forget(request.assetPath);
                failCode(call, failure.code, failure.message, failedOp >= 0 ? Json{{"op", failedOp}} : Json::object());
                return;
            }
            EditOptions options;
            options.dryRun = true;
            options.slotCount = asset->slotCount;
            const std::vector<EditOp> ops = finishedOps(parsed);
            const EditResult check = voxel::applyEdit(*asset->store, ops, options);
            if (!check.ok) {
                VoxelAssets::forget(request.assetPath);
                failCode(call, voxel::editErrorCode(check.error), editErrorFix(check.error, asset->slotCount));
                return;
            }
            if (dryRun) data.update(editSummaryJson(check, frame, request.desc.voxelSize));
        }
        if (dryRun) {
            VoxelAssets::forget(request.assetPath);
            data["dry_run"] = true;
            call->ok("Dry run: " + request.name + " would be about " + std::to_string(memory) + " bytes.", std::move(data));
            return;
        }
        std::error_code ignored;
        fs::create_directories((call->ctx().project()->getAssetsPath() / FileUtils::pathFromUtf8(request.assetPath)).parent_path(), ignored);
        AssetRef ref;
        std::string error;
        if (!saveNewVoxelAsset(*call->ctx().project(), *asset, ref, &error)) {
            VoxelAssets::forget(request.assetPath);
            failCode(call, "invalid_arguments", "Could not write the volume: " + error);
            return;
        }
        CommandHistory* history = CommandHandle::get(target.sp->id);
        const size_t mark = history->mark();
        if (!createVolumeEntity(call, target, request, ref)) return;
        std::string text = "Created " + request.name + " (" + assetFile + ").";
        if (request.generated) {
            VoxelGenerateRequest generate;
            generate.source = request.base == VoxelCreateBase::FromTerrain ? "terrain" : "preset";
            generate.preset = request.base == VoxelCreateBase::Caves ? "caves" : request.base == VoxelCreateBase::Overhangs ? "overhangs" : request.base == VoxelCreateBase::CaveEntrance ? "cave_entrance" : "";
            generate.seed = request.generator.seed;
            generate.terrain = request.terrain;
            generate.blendWidth = request.blendWidth;
            generate.keepEdits = false;
            if (!startGeneration(call, target, generate, "Generate " + request.name, mark, history->position(), data)) {
                history->undo();
                return;
            }
            text += " Generating its base as job " + data["job_id"].get<std::string>() + ".";
        }
        if (hasOps) {
            std::string opsText;
            if (!applyOps(call, target, parsed, false, wantHash, data, opsText)) {
                while (history->position() > mark && history->canUndo()) history->undo();
                return;
            }
            history->groupSince(mark, "Create Voxel Volume");
            text += " " + opsText;
        }
        data["entity"] = McpRefs::entityJson(target.sp, target.entity);
        call->ok(text, std::move(data));
        return;
    }

    if (!resolveEntity(call, target) || !resolveStore(call, target)) return;
    if (target.asset && refuseIfBusy(call, target.asset->path)) return;
    std::vector<ParsedOp> parsed;
    ParseFailure failure;
    int failedOp = -1;
    const double voxelSize = target.store->desc().voxelSize;
    if (!parseOps(call, target.sp, args["ops"], target.frame, voxelSize, parsed, failure, failedOp)) {
        failCode(call, failure.code, failure.message, failedOp >= 0 ? Json{{"op", failedOp}} : Json::object());
        return;
    }
    // R30 keeps a Play edit within a frame's budget, so only edit mode's can run long
    const bool hashIsLong = wantHash && readCost(*target.store, wholeVolume(*target.store)) > READ_JOB_COST;
    if (!target.playing && (editCost(*target.store, parsed) > EDIT_JOB_COST || hashIsLong)) {
        runEditJob(call, target, std::move(parsed), dryRun, wantHash);
        return;
    }
    Json data = {{"entity", McpRefs::entityJson(target.sp, target.entity)}};
    std::string text;
    if (!applyOps(call, target, parsed, dryRun, wantHash, data, text)) return;
    if (dryRun) data["dry_run"] = true;
    if (target.playing) {
        data["play_only"] = true;
        text += " Play only: Stop restores the volume.";
    }
    call->ok(text, std::move(data));
}

// ------------------------------------------------------------------ sample_voxels

Json valueAtPoint(Store& store, const Frame& frame, const Vector3& world) {
    const Vec3d local = frame.local(world);
    const double voxelSize = store.desc().voxelSize;
    const voxel::MaterialSample material = voxel::materialAtPoint(store, local);
    const double distance = voxel::valueAt(store, local);
    Json out = {{"distance", distance}, {"slot_a", material.slot}, {"slot_b", material.second}, {"weight", material.weight}, {"inside", distance < 0.0}};
    const Int3 samples = store.levelSamples(0);
    const int x = static_cast<int>(std::floor(local.x / voxelSize)), y = static_cast<int>(std::floor(local.y / voxelSize)), z = static_cast<int>(std::floor(local.z / voxelSize));
    if (x >= 0 && y >= 0 && z >= 0 && x < samples.x && y < samples.y && z < samples.z) {
        const voxel::BlockKey key{0, x >> voxel::BLOCK_SHIFT, y >> voxel::BLOCK_SHIFT, z >> voxel::BLOCK_SHIFT};
        out["source"] = store.slot(key).modified() ? "modified" : "base";
    } else {
        out["source"] = nullptr;
    }
    return out;
}

const char* groundSourceName(voxel::GroundSource source) {
    switch (source) {
        case voxel::GroundSource::Terrain: return "terrain";
        case voxel::GroundSource::Voxel: return "voxel";
        case voxel::GroundSource::None: break;
    }
    return "none";
}

bool readPoints(const CallPtr& call, std::vector<Vector3>& points) {
    const Json& args = call->args();
    if (!has(args, "points") || !args["points"].is_array() || args["points"].empty() || args["points"].size() > MAX_POINTS) {
        call->fail("points is a list of 1-" + std::to_string(MAX_POINTS) + " [x, y, z] in world metres.");
        return false;
    }
    for (const Json& value : args["points"]) {
        Vector3 point;
        if (!readVec3(value, point)) {
            call->fail("points is a list of [x, y, z] in world metres.");
            return false;
        }
        points.push_back(point);
    }
    return true;
}

void sampleValues(const CallPtr& call, Target& target) {
    std::vector<Vector3> points;
    if (!readPoints(call, points)) return;
    if (call->args().value("ground", false)) {
        Json list = Json::array();
        for (const Vector3& point : points) {
            const voxel::GroundSample ground = voxel::sampleGround(target.sp->scene, point.x, point.z, point.y);
            list.push_back({{"height", ground.height}, {"normal", vecJson(ground.normal)}, {"source", groundSourceName(ground.source)}});
        }
        call->ok(std::to_string(list.size()) + " ground samples.", {{"ground", std::move(list)}});
        return;
    }
    const Frame frame = target.frame;
    const auto compute = [points, frame](Store& store) {
        Json list = Json::array();
        for (const Vector3& point : points) list.push_back(valueAtPoint(store, frame, point));
        return Json{{"values", std::move(list)}, {"summary", std::to_string(points.size()) + " samples."}};
    };
    // up to eight blocks around each point, all of them possibly unresolved
    const double voxelSize = target.store->desc().voxelSize;
    std::set<std::array<int, 3>> blocks;
    const Int3 counts = target.store->blockCount(0);
    int64_t cost = 0;
    for (const Vector3& point : points) {
        const Vec3d local = frame.local(point);
        const std::array<int, 3> key = {static_cast<int>(std::floor(local.x / voxelSize)) >> voxel::BLOCK_SHIFT,
                                        static_cast<int>(std::floor(local.y / voxelSize)) >> voxel::BLOCK_SHIFT,
                                        static_cast<int>(std::floor(local.z / voxelSize)) >> voxel::BLOCK_SHIFT};
        if (key[0] < 0 || key[1] < 0 || key[2] < 0 || key[0] >= counts.x || key[1] >= counts.y || key[2] >= counts.z) continue;
        if (!blocks.insert(key).second) continue;
        const voxel::Slot::Kind kind = target.store->slot(voxel::BlockKey{0, key[0], key[1], key[2]}).kind();
        if (kind == voxel::Slot::Base || kind == voxel::Slot::Cold) cost += READ_COST_UNRESOLVED;
    }
    if (cost > READ_JOB_COST) {
        runReadJob(call, *target.store, "sample_voxels", "Sample Voxels", compute);
        return;
    }
    Json data = compute(*target.store);
    const std::string summary = data["summary"].get<std::string>();
    data.erase("summary");
    call->ok(summary, std::move(data));
}

void sampleBox(const CallPtr& call, Target& target) {
    const Json& args = call->args();
    Vector3 worldMin, worldMax;
    if (!has(args, "min") || !has(args, "max") || !readVec3(args["min"], worldMin) || !readVec3(args["max"], worldMax)) {
        call->fail("mode box needs min and max: [x, y, z] in world metres.");
        return;
    }
    const double voxelSize = target.store->desc().voxelSize;
    const Int3 samples = target.store->levelSamples(0);
    AABB local;
    const AABB world(worldMin, worldMax);
    local = target.frame.inverse * world;
    IntBox box;
    box.min = Int3{std::max(0, static_cast<int>(std::floor(local.getMinimum().x / voxelSize))), std::max(0, static_cast<int>(std::floor(local.getMinimum().y / voxelSize))),
                   std::max(0, static_cast<int>(std::floor(local.getMinimum().z / voxelSize)))};
    box.max = Int3{std::min(samples.x - 1, static_cast<int>(std::ceil(local.getMaximum().x / voxelSize))), std::min(samples.y - 1, static_cast<int>(std::ceil(local.getMaximum().y / voxelSize))),
                   std::min(samples.z - 1, static_cast<int>(std::ceil(local.getMaximum().z / voxelSize)))};
    if (box.empty()) {
        failCode(call, "out_of_bounds", "The box lies wholly outside the volume: sample_voxels {mode: bounds} gives its extent.");
        return;
    }
    const Frame frame = target.frame;
    int stride = 0;
    if (has(args, "stride")) {
        if (!args["stride"].is_number_integer() || args["stride"].get<int>() < 1) {
            call->fail("stride is an integer from 1: every value at this spacing in voxels.");
            return;
        }
        stride = args["stride"].get<int>();
        const int64_t count = static_cast<int64_t>((box.max.x - box.min.x) / stride + 1) * ((box.max.y - box.min.y) / stride + 1) * ((box.max.z - box.min.z) / stride + 1);
        if (count > MAX_BOX_VALUES) {
            call->fail("That is " + std::to_string(count) + " values, over " + std::to_string(MAX_BOX_VALUES) + ": raise stride or shrink the box.");
            return;
        }
    }
    const auto compute = [box, stride, frame, voxelSize](Store& store) {
        Json out = {{"voxels", voxelBoxJson(box)}, {"world", worldBoxJson(frame, box, voxelSize)}};
        if (stride > 0) {
            Json values = Json::array();
            int64_t count = 0;
            for (int z = box.min.z; z <= box.max.z; z += stride) {
                for (int y = box.min.y; y <= box.max.y; y += stride) {
                    for (int x = box.min.x; x <= box.max.x; x += stride) {
                        values.push_back(store.sdfAt(0, x, y, z) * voxelSize / voxel::SDF_UNITS_PER_VOXEL);
                        count++;
                    }
                }
            }
            out["stride"] = stride;
            out["counts"] = Json::array({(box.max.x - box.min.x) / stride + 1, (box.max.y - box.min.y) / stride + 1, (box.max.z - box.min.z) / stride + 1});
            out["values"] = std::move(values);
            out["summary"] = std::to_string(count) + " values, x fastest, in metres.";
            return out;
        }
        const voxel::BoxStats stats = voxel::boxStats(store, box);
        Json slots = Json::array();
        for (int slot = 0; slot < voxel::MAX_SLOTS; slot++) {
            if (stats.slotsSeen & (1u << slot)) slots.push_back(slot);
        }
        out.update({{"samples", stats.samples}, {"min_distance", stats.minDistance * voxelSize}, {"max_distance", stats.maxDistance * voxelSize},
                    {"solid_fraction", stats.solidFraction}, {"air_fraction", stats.airFraction}, {"surface_samples", stats.surfaceSamples}, {"slots", std::move(slots)}});
        char line[96];
        std::snprintf(line, sizeof(line), "%lld samples, %.3f solid.", static_cast<long long>(stats.samples), stats.solidFraction);
        out["summary"] = std::string(line);
        return out;
    };
    if (readCost(*target.store, box) > READ_JOB_COST) {
        runReadJob(call, *target.store, "sample_voxels", "Sample Voxels Box", compute);
        return;
    }
    Json data = compute(*target.store);
    const std::string summary = data["summary"].get<std::string>();
    data.erase("summary");
    call->ok(summary, std::move(data));
}

void sampleBounds(const CallPtr& call, Target& target) {
    const double voxelSize = target.store->desc().voxelSize;
    const Frame frame = target.frame;
    const IntBox whole = wholeVolume(*target.store);
    const auto compute = [whole, frame, voxelSize](Store& store) {
        const voxel::VolumeBounds bounds = voxel::solidBounds(store);
        return Json{{"volume", {{"voxels", voxelBoxJson(whole)}, {"world", worldBoxJson(frame, whole, voxelSize)}}},
                    {"solid", {{"voxels", voxelBoxJson(bounds.solid)}, {"world", worldBoxJson(frame, bounds.solid, voxelSize)}}},
                    {"surface", {{"voxels", voxelBoxJson(bounds.surface)}, {"world", worldBoxJson(frame, bounds.surface, voxelSize)}}},
                    {"summary", bounds.solid.empty() ? std::string("The volume holds no solid.") : std::string("Bounds of the volume, its solid and its surface.")}};
    };
    if (readCost(*target.store, whole) > READ_JOB_COST) {
        runReadJob(call, *target.store, "sample_voxels", "Sample Voxels Bounds", compute);
        return;
    }
    Json data = compute(*target.store);
    const std::string summary = data["summary"].get<std::string>();
    data.erase("summary");
    call->ok(summary, std::move(data));
}

void sampleStats(const CallPtr& call, Target& target, Project* project) {
    const VoxelStats stats = target.system->getStats(target.entity);
    const voxel::MemoryStats& memory = stats.memory;
    Json budgets = Json::object();
    const BuildPlatform targets[] = {BuildPlatform::Linux, BuildPlatform::Android, BuildPlatform::Web};
    const char* names[] = {"desktop", "mobile", "web"};
    for (size_t i = 0; i < 3; i++) {
        const VoxelLimits limits = lowerVoxelLimits(voxelTargetLimits(targets[i]), project->getVoxelSettings());
        budgets[names[i]] = {{"max_dimension", limits.maxDimension}, {"cpu_budget", limits.cpuBudget}, {"mesh_budget", limits.meshBudget},
                             {"cpu_over", memory.total() > limits.cpuBudget}, {"mesh_over", stats.meshBytes > limits.meshBudget}};
    }
    const std::shared_ptr<VoxelAsset>& asset = target.asset;
    Json data = {
        {"blocks", {{"total", stats.blocks.total}, {"uniform", stats.blocks.uniform}, {"non_uniform", stats.blocks.nonUniform},
                    {"base", stats.blocks.base}, {"cold", stats.blocks.cold}, {"modified", stats.blocks.modified}}},
        {"memory", {{"hot", memory.hotBytes}, {"cold", memory.coldBytes}, {"tables", memory.tableBytes}, {"total", memory.total()}, {"mesh", stats.meshBytes}}},
        {"budgets", std::move(budgets)},
        {"triangles", stats.triangles},
        {"chunks", stats.chunks},
        {"draws", stats.draws},
        {"jobs_pending", stats.jobsPending},
        {"collision_bodies", stats.collisionBodies},
        {"last_mesh_ms", stats.lastMeshMs},
        {"last_latency_frames", stats.lastLatencyFrames},
        {"levels", stats.levels},
        {"dimensions", intsJson(target.store->desc().dimensions)},
        {"voxel_size", target.store->desc().voxelSize},
        {"base", asset ? asset->generator.kind : std::string()},
        {"base_stale", isVoxelBaseStale(target.sp->scene, target.entity)},
    };
    call->ok(std::to_string(stats.chunks) + " chunks, " + std::to_string(stats.triangles) + " triangles, " + std::to_string(memory.total()) + " bytes.", std::move(data));
}

void sampleHash(const CallPtr& call, Target& target) {
    const bool perBlock = call->args().value("per_block", false);
    const auto compute = [perBlock](Store& store) {
        const uint64_t hash = voxel::contentHash(store);
        Json out = {{"content_hash", hexHash(hash)}, {"summary", "Content hash " + hexHash(hash) + "."}};
        if (!perBlock) return out;
        Json blocks = Json::array();
        int64_t uniform = 0;
        bool truncated = false;
        const Int3 counts = store.blockCount(0);
        for (int z = 0; z < counts.z; z++) {
            for (int y = 0; y < counts.y; y++) {
                for (int x = 0; x < counts.x; x++) {
                    const voxel::BlockKey key{0, x, y, z};
                    const voxel::BlockContent content = store.readBlock(key);
                    if (content.isFullyUniform()) {
                        uniform++;
                    } else if (blocks.size() < MAX_BLOCK_HASHES) {
                        blocks.push_back({{"block", Json::array({x, y, z})}, {"hash", hexHash(voxel::blockContentHash(content))}});
                    } else {
                        truncated = true;
                    }
                }
            }
        }
        out["blocks"] = std::move(blocks);
        out["uniform_blocks"] = uniform;
        if (truncated) out["truncated"] = true;
        return out;
    };
    if (readCost(*target.store, wholeVolume(*target.store)) > READ_JOB_COST) {
        runReadJob(call, *target.store, "sample_voxels", "Hash Voxels", compute);
        return;
    }
    Json data = compute(*target.store);
    const std::string summary = data["summary"].get<std::string>();
    data.erase("summary");
    call->ok(summary, std::move(data));
}

void sampleConnectivity(const CallPtr& call, Target& target) {
    const Json& args = call->args();
    const double voxelSize = target.store->desc().voxelSize;
    const Frame frame = target.frame;
    const auto readVoxel = [&](const char* key, Int3& out) {
        Vector3 world;
        if (!readVec3(args[key], world)) {
            call->fail(std::string(key) + " must be [x, y, z] in world metres.");
            return false;
        }
        const Vec3d local = frame.local(world);
        out = Int3{static_cast<int>(std::lround(local.x / voxelSize)), static_cast<int>(std::lround(local.y / voxelSize)), static_cast<int>(std::lround(local.z / voxelSize))};
        return true;
    };
    bool hasFrom = false, hasTo = false;
    Int3 from, to;
    if (has(args, "from")) {
        if (!readVoxel("from", from)) return;
        hasFrom = true;
    }
    if (has(args, "to")) {
        if (!readVoxel("to", to)) return;
        hasTo = true;
    }
    const auto compute = [hasFrom, hasTo, from, to, frame, voxelSize](Store& store) {
        const voxel::ConnectivityResult result = voxel::connectivity(store, hasFrom ? &from : nullptr, hasTo ? &to : nullptr);
        std::vector<size_t> order(result.components.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return result.components[a].voxels > result.components[b].voxels; });
        Json components = Json::array();
        bool openToSky = false;
        const double cell = voxelSize * voxelSize * voxelSize;
        for (const voxel::AirComponent& component : result.components) openToSky = openToSky || component.openToSky;
        for (size_t i = 0; i < order.size() && i < MAX_COMPONENTS; i++) {
            const voxel::AirComponent& component = result.components[order[i]];
            components.push_back({{"index", order[i]}, {"voxels", component.voxels}, {"volume", component.voxels * cell}, {"voxel_bounds", voxelBoxJson(component.bounds)},
                                  {"bounds", worldBoxJson(frame, component.bounds, voxelSize)}, {"open_to_sky", component.openToSky}});
        }
        Json out = {{"connected", result.connected}, {"air_components", std::move(components)}, {"component_count", result.components.size()}, {"open_to_sky", openToSky},
                    {"from_in_air", result.fromInAir}, {"to_in_air", result.toInAir}, {"from_component", result.fromComponent}, {"to_component", result.toComponent}};
        std::string summary = std::to_string(result.components.size()) + " air components";
        if (hasFrom && hasTo) summary += result.connected ? "; from and to are connected." : "; from and to are not connected.";
        else summary += ".";
        out["summary"] = summary;
        return out;
    };
    if (readCost(*target.store, wholeVolume(*target.store)) > READ_JOB_COST) {
        runReadJob(call, *target.store, "sample_voxels", "Voxel Connectivity", compute);
        return;
    }
    Json data = compute(*target.store);
    const std::string summary = data["summary"].get<std::string>();
    data.erase("summary");
    call->ok(summary, std::move(data));
}

void sampleVoxelsTool(const CallPtr& call) {
    const std::string mode = call->args().value("mode", std::string());
    Target target;
    if (!resolveVolume(call, target)) return;
    if (mode == "values") sampleValues(call, target);
    else if (mode == "box") sampleBox(call, target);
    else if (mode == "bounds") sampleBounds(call, target);
    else if (mode == "stats") sampleStats(call, target, call->ctx().project());
    else if (mode == "hash") sampleHash(call, target);
    else if (mode == "connectivity") sampleConnectivity(call, target);
    else call->fail("mode is values, box, bounds, stats, hash or connectivity.");
}

// ------------------------------------------------------------------ generate_voxels

bool parseGenerateRequest(const CallPtr& call, Target& target, VoxelGenerateRequest& request) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    SceneProject* sp = target.sp;
    std::string error;
    request.source = args.value("source", std::string());
    if (request.source != "noise_graph" && request.source != "terrain" && request.source != "mesh" && request.source != "preset") {
        failCode(call, "invalid_source", "source is noise_graph, terrain, mesh or preset (" + std::string(DOCS) + ").");
        return false;
    }
    const auto existingPath = [&](const std::string& path, std::string& out) {
        fs::path absolute;
        if (!McpRefs::resolveProjectPath(project, path, absolute, error, true)) return false;
        out = path;
        return true;
    };
    if (request.source == "noise_graph") {
        if (!has(args, "graph") || !args["graph"].is_string()) {
            failCode(call, "invalid_source", "source noise_graph needs graph: the path of a .noisegraph.");
            return false;
        }
        if (!existingPath(args["graph"].get<std::string>(), request.graphPath)) {
            failCode(call, "missing_asset", error);
            return false;
        }
    }
    if (has(args, "seed")) {
        if (!args["seed"].is_number_integer()) {
            failCode(call, "invalid_source", "seed is an integer.");
            return false;
        }
        request.seed = args["seed"].get<uint64_t>();
    }
    if (has(args, "terrain")) {
        request.terrain = McpRefs::resolveEntity(sp, args["terrain"], error);
        if (request.terrain == NULL_ENTITY || !sp->scene->findComponent<TerrainComponent>(request.terrain)) {
            failCode(call, "missing_asset", error.empty() ? "terrain has no Terrain." : error);
            return false;
        }
    } else if (const VoxelVolumeComponent* component = sp->scene->findComponent<VoxelVolumeComponent>(target.entity)) {
        request.terrain = component->terrain;
    }
    if (request.source == "terrain" && request.terrain == NULL_ENTITY) {
        failCode(call, "missing_asset", "source terrain needs terrain: a GameObject with a Terrain.");
        return false;
    }
    if (has(args, "blend_width")) {
        if (!(args["blend_width"].is_number() && args["blend_width"].get<double>() >= 0.0)) {
            failCode(call, "invalid_source", "blend_width is metres, from 0.");
            return false;
        }
        request.blendWidth = args["blend_width"].get<double>();
    }
    if (request.source == "mesh") {
        if (!has(args, "mesh")) {
            failCode(call, "invalid_source", "source mesh needs mesh: a GameObject with a Mesh, or a model's path.");
            return false;
        }
        std::string meshError;
        const Entity entity = McpRefs::resolveEntity(sp, args["mesh"], meshError);
        if (entity != NULL_ENTITY && sp->scene->findComponent<MeshComponent>(entity)) {
            request.mesh = entity;
        } else if (!args["mesh"].is_string() || !existingPath(args["mesh"].get<std::string>(), request.meshPath)) {
            failCode(call, "missing_asset", "mesh is a GameObject with a Mesh or a model's path: " + (error.empty() ? meshError : error));
            return false;
        }
    }
    if (has(args, "transform")) {
        if (!args["transform"].is_array() || args["transform"].size() != 16) {
            failCode(call, "invalid_source", "transform is 16 numbers, column major.");
            return false;
        }
        for (size_t i = 0; i < 16; i++) {
            if (!args["transform"][i].is_number()) {
                failCode(call, "invalid_source", "transform is 16 numbers, column major.");
                return false;
            }
            request.meshTransform[i] = args["transform"][i].get<double>();
        }
    }
    if (request.source == "preset") {
        request.preset = args.value("preset", std::string());
        bool known = false;
        std::string names;
        for (const VoxelPresetInfo& info : voxelPresets()) {
            known = known || info.name == request.preset;
            names += (names.empty() ? "" : ", ") + info.name;
        }
        if (!known) {
            failCode(call, "invalid_source", "preset is " + names + ".");
            return false;
        }
    }
    if (has(args, "params")) {
        const Json& params = args["params"];
        if (!params.is_object() || !closedKeys(params, {"size_scale"}, "params", error)) {
            failCode(call, "invalid_source", error.empty() ? "params is {size_scale}." : error);
            return false;
        }
        if (has(params, "size_scale")) {
            if (!params["size_scale"].is_number() || !(params["size_scale"].get<double>() > 0.0)) {
                failCode(call, "invalid_source", "params.size_scale is a number above 0.");
                return false;
            }
            request.sizeScale = params["size_scale"].get<double>();
        }
    }
    request.keepEdits = args.value("keep_edits", true);
    request.dryRun = args.value("dry_run", false);
    if (has(args, "region")) {
        const Json& region = args["region"];
        Vector3 regionMin, regionMax;
        if (!region.is_object() || !has(region, "min") || !has(region, "max") || !readVec3(region["min"], regionMin) || !readVec3(region["max"], regionMax)) {
            failCode(call, "invalid_source", "region is {min, max}: [x, y, z] in world metres.");
            return false;
        }
        const double voxelSize = target.store->desc().voxelSize;
        const AABB local = target.frame.inverse * AABB(regionMin, regionMax);
        request.hasRegion = true;
        request.region.min = Int3{static_cast<int>(std::floor(local.getMinimum().x / voxelSize)), static_cast<int>(std::floor(local.getMinimum().y / voxelSize)),
                                  static_cast<int>(std::floor(local.getMinimum().z / voxelSize))};
        request.region.max = Int3{static_cast<int>(std::ceil(local.getMaximum().x / voxelSize)), static_cast<int>(std::ceil(local.getMaximum().y / voxelSize)),
                                  static_cast<int>(std::ceil(local.getMaximum().z / voxelSize))};
    }
    return true;
}

void generateVoxelsTool(const CallPtr& call) {
    Target target;
    if (!resolveVolume(call, target)) return;
    if (target.asset && refuseIfBusy(call, target.asset->path)) return;
    VoxelGenerateRequest request;
    if (!parseGenerateRequest(call, target, request)) return;
    Json data = {{"entity", McpRefs::entityJson(target.sp, target.entity)}};
    if (!startGeneration(call, target, request, "Generate " + target.sp->scene->getEntityName(target.entity), 0, 0, data)) return;
    // the text first: the argument that moves data may be evaluated before it
    const std::string text = "Generating as job " + data["job_id"].get<std::string>() + ".";
    call->ok(text, std::move(data));
}

// ------------------------------------------------------------------ visualize_voxels

bool encodeSlice(const VoxelSliceImage& image, ToolImage& out) {
    std::vector<unsigned char> encoded;
    auto append = [](void* context, void* bytes, int size) {
        auto* target = static_cast<std::vector<unsigned char>*>(context);
        const unsigned char* begin = static_cast<const unsigned char*>(bytes);
        target->insert(target->end(), begin, begin + size);
    };
    if (!stbi_write_png_to_func(append, &encoded, image.width, image.height, 4, image.rgba.data(), image.width * 4)) return false;
    out.data = Base64::encode(encoded.data(), encoded.size());
    out.mimeType = "image/png";
    return true;
}

void visualizeSlice(const CallPtr& call, Target& target) {
    const Json& args = call->args();
    VoxelSliceRequest request;
    const std::string axis = args.value("axis", std::string("y"));
    if (axis == "x") request.axis = VoxelSliceAxis::X;
    else if (axis == "y") request.axis = VoxelSliceAxis::Y;
    else if (axis == "z") request.axis = VoxelSliceAxis::Z;
    else {
        call->fail("axis is x, y or z.");
        return;
    }
    const double voxelSize = target.store->desc().voxelSize;
    if (has(args, "position")) {
        if (!args["position"].is_number()) {
            call->fail("position is the plane along axis, in world metres.");
            return;
        }
        // The plane through that world coordinate, on the volume's own axis
        const Vector3 origin = target.frame.world(Vec3d{});
        const int index = request.axis == VoxelSliceAxis::X ? 0 : request.axis == VoxelSliceAxis::Y ? 1 : 2;
        const double along = args["position"].get<double>() - (index == 0 ? origin.x : index == 1 ? origin.y : origin.z);
        const int samples = request.axis == VoxelSliceAxis::X ? target.store->levelSamples(0).x : request.axis == VoxelSliceAxis::Y ? target.store->levelSamples(0).y : target.store->levelSamples(0).z;
        request.position = std::clamp(static_cast<int>(std::lround(along / voxelSize)), 0, samples - 1);
    }
    request.maxWidth = std::clamp(args.value("max_width", DEFAULT_SLICE_WIDTH), 16, 4096);
    Scene* scene = target.sp->scene;
    const VoxelVolumeComponent* component = scene->findComponent<VoxelVolumeComponent>(target.entity);
    if (component && component->terrain != NULL_ENTITY) {
        const Frame frame = target.frame;
        request.terrainHeight = [scene, frame](double x, double z, double& height) {
            const Vector3 world = frame.world(Vec3d{x, 0.0, z});
            const voxel::GroundSample ground = voxel::sampleGround(scene, world.x, world.z, 1e6f);
            if (ground.source != voxel::GroundSource::Terrain) return false;
            height = frame.local(Vector3(world.x, ground.height, world.z)).y;
            return true;
        };
    }
    VoxelSliceImage image;
    if (!renderVoxelSlice(*target.store, request, image)) {
        call->fail("The volume has no samples to draw.");
        return;
    }
    ToolResult reply;
    ToolImage png;
    if (!encodeSlice(image, png)) {
        call->fail("Encoding the slice failed.");
        return;
    }
    reply.images.push_back(std::move(png));
    Json data = {{"mode", "slice"}, {"axis", axis}, {"position_voxels", image.position}, {"width", image.width}, {"height", image.height},
                 {"iso_line_length", image.isoLineLength}, {"solid_fraction", image.solidFraction}, {"air_fraction", image.airFraction}};
    char text[128];
    std::snprintf(text, sizeof(text), "Slice %s = %d voxels: %.3f solid, iso-line %.2f m.", axis.c_str(), image.position, image.solidFraction, image.isoLineLength);
    reply.text = text;
    reply.structured = std::move(data);
    call->reply(std::move(reply));
}

void visualizeVoxelsTool(const CallPtr& call) {
    const Json& args = call->args();
    const std::string mode = args.value("mode", std::string());
    Target target;
    if (!resolveVolume(call, target)) return;
    if (mode == "slice") {
        visualizeSlice(call, target);
        return;
    }
    VoxelOverlay overlay = VoxelOverlay::None;
    if (mode == "wireframe") overlay = VoxelOverlay::Wireframe;
    else if (mode == "lod") overlay = VoxelOverlay::LodColours;
    else if (mode == "materials") overlay = VoxelOverlay::DominantMaterial;
    else if (mode == "blocks") overlay = VoxelOverlay::Blocks;
    else if (mode == "collision") overlay = VoxelOverlay::Collision;
    else if (mode == "holes") overlay = VoxelOverlay::Holes;
    else if (mode != "off") {
        call->fail("mode is slice, wireframe, lod, materials, blocks, collision, holes or off.");
        return;
    }
    setVoxelOverlay(target.entity, overlay);
    target.sp->needUpdateRender = true;
    if (args.value("frame", false)) {
        if (SceneWindow* window = call->ctx().sceneWindow()) window->frameEntities(target.sp->id, target.sp->id, {target.entity});
    }
    Json data = {{"mode", mode}, {"overlay", voxelOverlayName(overlay)}};
    const std::string text = "Voxel overlay " + std::string(voxelOverlayName(overlay)) + ".";
    if (overlay == VoxelOverlay::None) {
        call->ok(text, std::move(data));
        return;
    }
    w3::CaptureRequest request;
    request.view = "scene";
    request.scene = target.sp->id;
    request.maxWidth = std::clamp(args.value("max_width", 768), 0, 4096);
    w3::captureView(call, request, [call, text, data](w3::CaptureResult result) mutable {
        if (!result.ok) {
            call->fail(text + " The Scene view image failed: " + result.error, data);
            return;
        }
        ToolResult reply = ToolResult::success(text, data);
        reply.images.push_back(std::move(result.image));
        call->reply(std::move(reply));
    });
}

}

}

void registerVoxelTools(McpRegistry& registry) {
    using namespace schema;
    using namespace voxeltools;
    const Json point = Json{{"type", "array"}, {"items", Json{{"type", "number"}}}};

    ToolDef edit;
    edit.name = "edit_voxels";
    edit.title = "Edit Voxels";
    edit.group = "voxels";
    edit.attributesUndo = true;
    edit.allowedWhilePlaying = true;
    edit.description =
        "Creates a Voxel Volume (caves, overhangs, ground that digs; a plain hill belongs on a Terrain) or edits one in world space: "
        "dig, add, smooth or paint shapes. All ops are checked first, one undo step. In Play it changes only the running copy. "
        "dry_run changes nothing. Fields and error codes: jasper://docs/voxels. Check with sample_voxels.";
    edit.inputSchema = object({
        {"scene", sceneRef()},
        {"entity", entityRef()},
        {"create", Json{{"type", "object"}}},
        {"ops", array(Json{{"type", "object"}}, "")},
        {"dry_run", boolean("")},
        {"hash", boolean("")}
    });
    edit.handler = editVoxelsTool;
    registry.addTool(std::move(edit));

    ToolDef sample;
    sample.name = "sample_voxels";
    sample.title = "Sample Voxels";
    sample.group = "voxels";
    sample.annotations.readOnly = true;
    sample.annotations.idempotent = true;
    sample.allowedWhilePlaying = true;
    sample.description =
        "Reads a Voxel Volume in world space: distances and materials at points (ground: the ground under them), a box's stats or "
        "values, bounds, stats against each target's budget, content hash, air connectivity. Large reads return a job: get_job.";
    sample.inputSchema = object({
        {"scene", sceneRef()},
        {"entity", entityRef()},
        {"mode", stringEnum("", {"values", "box", "bounds", "stats", "hash", "connectivity"})},
        {"points", array(point, "")},
        {"ground", boolean("")},
        {"min", vec3("")},
        {"max", vec3("")},
        {"stride", integer("")},
        {"per_block", boolean("")},
        {"from", vec3("")},
        {"to", vec3("")}
    }, {"mode"});
    sample.handler = sampleVoxelsTool;
    registry.addTool(std::move(sample));

    ToolDef visualize;
    visualize.name = "visualize_voxels";
    visualize.title = "Visualize Voxels";
    visualize.group = "voxels";
    visualize.annotations.idempotent = true;
    visualize.activity = ToolActivity::Control;
    visualize.allowedWhilePlaying = true;
    visualize.description =
        "Shows a Voxel Volume as an image: slice draws one plane on the CPU, the same bytes every time; the other modes turn on "
        "that Scene view overlay and return the view (off clears it).";
    visualize.inputSchema = object({
        {"scene", sceneRef()},
        {"entity", entityRef()},
        {"mode", stringEnum("", {"slice", "wireframe", "lod", "materials", "blocks", "collision", "holes", "off"})},
        {"axis", stringEnum("", {"x", "y", "z"})},
        {"position", number("")},
        {"max_width", integer("", 16, 4096)},
        {"frame", boolean("")}
    }, {"mode"});
    visualize.handler = visualizeVoxelsTool;
    registry.addTool(std::move(visualize));

    ToolDef generate;
    generate.name = "generate_voxels";
    generate.title = "Generate Voxels";
    generate.group = "voxels";
    generate.attributesUndo = true;
    generate.allowedWhilePlaying = false;
    generate.description =
        "Gives a Voxel Volume a new base as a job (get_job polls it, cancel_job leaves everything as it was), one undo step: a Noise "
        "Graph, a Terrain, a mesh or a preset. Rather than building a landscape from edit_voxels ops. keep_edits defaults to true; "
        "dry_run estimates from a coarse level. Fields: jasper://docs/voxels";
    generate.inputSchema = object({
        {"scene", sceneRef()},
        {"entity", entityRef()},
        {"source", stringEnum("", {"noise_graph", "terrain", "mesh", "preset"})},
        {"graph", string("")},
        {"seed", integer("")},
        {"terrain", entityRef()},
        {"blend_width", number("")},
        {"mesh", Json{{"type", {"integer", "string", "object"}}}},
        {"transform", array(Json{{"type", "number"}}, "")},
        {"preset", string("")},
        {"params", Json{{"type", "object"}}},
        {"region", Json{{"type", "object"}}},
        {"keep_edits", boolean("")},
        {"dry_run", boolean("")}
    }, {"source"});
    generate.handler = generateVoxelsTool;
    registry.addTool(std::move(generate));
}

}
