// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor's one entry point to an entity's vertex colors (docs/specs/V1-vertex-colors.md R1, R6,
// R26, R97, R100, R107, R120, R132): targets, live values, the atomic multi-target edit as one undo
// step (on a job when large), the component-level actions.

#include "VertexColorEditing.h"
#include "VertexColorEditorInternal.h"
#include "VertexColorProject.h"

#include "EditorHost.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/CommandNames.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ModelComponent.h"
#include "component/VertexColorsComponent.h"
#include "jobs/JobSystem.h"
#include "subsystem/VertexColorScene.h"
#include "texture/Material.h"
#include "util/Prefab.h"
#include "vertexcolor/VertexColorSplit.h"

#include <algorithm>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace vcd = jasperengine::editor::vertexcolor::detail;

namespace {

    constexpr ComponentType kVcType = ComponentType::VertexColorsComponent;
    // R97: ops over more elements than this go to a job
    constexpr size_t kVcJobElements = 250000;

    std::string vcEntityName(Project* project, uint32_t sceneId, Entity entity) {
        return CommandNames::entity(project, sceneId, entity);
    }

    bool vcIsBake(jasperengine::vertexcolor::OpType type) {
        return type == jasperengine::vertexcolor::OpType::FromTexture || type == jasperengine::vertexcolor::OpType::BakeAO || type == jasperengine::vertexcolor::OpType::BakeThickness ||
               type == jasperengine::vertexcolor::OpType::BakeCurvature || type == jasperengine::vertexcolor::OpType::Transfer;
    }

    jasperengine::vertexcolor::ChannelMask vcPaintedChannels(const std::vector<jasperengine::vertexcolor::Op>& ops) {
        jasperengine::vertexcolor::ChannelMask mask = 0;
        for (const jasperengine::vertexcolor::Op& op : ops) mask |= op.channels;
        return mask;
    }

    // a model's asset path ("" when the entity draws no model)
    std::string vcModelPath(Scene* scene, Entity entity) {
        const ModelComponent* model = scene ? scene->findComponent<ModelComponent>(entity) : nullptr;
        return model ? model->filename.path : std::string();
    }

    // ---- an edit's work: copies of the targets, run anywhere (a job), committed on the main thread

    struct VcWorkItem {
        jasperengine::editor::vertexcolor::TargetRef ref;
        std::shared_ptr<const jasperengine::vertexcolor::MeshView> mesh;
        jasperengine::vertexcolor::TargetData data;              // the copy the ops run on
        std::vector<jasperengine::editor::vertexcolor::vc::Op> ops;          // the request's ops, or this target's own (perTargetOps)
        bool painted = false;             // the live file had the target
        uint64_t startHash = 0;           // the live target's hash at the start (0: not painted)
        jasperengine::vertexcolor::ApplyOptions options;
    };

    struct VcEditPlan {
        std::vector<VcWorkItem> items;
        bool dryRun = false;
        std::string undoName;
    };

    bool vcPlan(Project* project, uint32_t sceneId, const jasperengine::editor::vertexcolor::EditRequest& request, VcEditPlan& plan, jasperengine::editor::vertexcolor::EditResult& result) {
        plan.dryRun = request.dryRun;
        plan.undoName = request.undoName.empty() ? "Paint Vertex Colors" : request.undoName;
        if (request.targets.empty()) {
            result.error = "targets: no target given; name the GameObject and the mesh to paint.";
            return false;
        }
        if (!request.perTargetOps.empty() && request.perTargetOps.size() != request.targets.size()) {
            result.error = "perTargetOps: " + std::to_string(request.perTargetOps.size()) + " lists for " + std::to_string(request.targets.size()) +
                           " targets; give one list per target, or none and `ops`.";
            return false;
        }
        if (request.perTargetOps.empty() && request.ops.empty()) {
            result.error = "ops: no operation given.";
            return false;
        }
        Scene* scene = vcd::sceneOf(project, sceneId);
        if (!scene) {
            result.error = "scene " + std::to_string(sceneId) + " is not open.";
            return false;
        }
        for (size_t i = 0; i < request.targets.size(); i++) {
            const jasperengine::editor::vertexcolor::TargetRef& ref = request.targets[i];
            const std::string where = "targets[" + std::to_string(i) + "]";
            for (size_t j = 0; j < i; j++) {
                if (request.targets[j] == ref) {
                    result.error = where + ": the same target as targets[" + std::to_string(j) + "]; name each once.";
                    return false;
                }
            }
            std::string why;
            if (!jasperengine::editor::vertexcolor::canPaint(project, sceneId, ref.entity, &why)) {
                result.error = where + ": " + why;
                return false;
            }
            const std::vector<jasperengine::editor::vertexcolor::vc::SceneTarget> targets = jasperengine::vertexcolor::sceneTargets(scene, ref.entity);
            if (std::none_of(targets.begin(), targets.end(), [&](const jasperengine::vertexcolor::SceneTarget& t) { return t.key == ref.key; })) {
                result.error = where + ".key: " + vcEntityName(project, sceneId, ref.entity) + " has no " + ref.key.describe() + " (" +
                               std::to_string(targets.size()) + " target" + (targets.size() == 1 ? "" : "s") + ": list them first).";
                return false;
            }
            VcWorkItem item;
            item.ref = ref;
            item.ops = request.perTargetOps.empty() ? request.ops : request.perTargetOps[i];
            if (item.ops.empty()) {
                result.error = "perTargetOps[" + std::to_string(i) + "]: no operation for this target.";
                return false;
            }
            item.mesh = jasperengine::vertexcolor::sceneMeshView(scene, ref.entity, ref.key);
            if (!item.mesh) {
                result.error = where + ": the mesh of " + vcEntityName(project, sceneId, ref.entity) + " is not loaded yet; try again once it shows.";
                return false;
            }
            std::string error;
            const std::shared_ptr<jasperengine::editor::vertexcolor::vc::Data> data = vcd::resolve(project, sceneId, ref.entity, &error);
            if (!data && !error.empty()) {
                result.error = where + ": " + error;
                return false;
            }
            const jasperengine::vertexcolor::TargetData* live = data ? data->find(ref.key) : nullptr;
            if (live && !jasperengine::vertexcolor::targetMatchesMesh(scene, ref.entity, ref.key, false)) {
                result.error = where + ": " + ref.key.describe() + " of " + vcEntityName(project, sceneId, ref.entity) +
                               " was painted on another version of its mesh and could not be transferred (vertex_colors_unmatched). "
                               "Clear its vertex colors to paint it again.";
                return false;
            }
            item.painted = live != nullptr;
            item.startHash = live ? jasperengine::vertexcolor::hashTarget(*live) : 0;
            item.data = jasperengine::editor::vertexcolor::targetDataOrInitial(project, sceneId, ref.entity, ref.key);
            item.options = jasperengine::editor::vertexcolor::applyOptions(project, sceneId, ref.entity, ref.key, request.world);
            item.options.visible = request.visible;
            plan.items.push_back(std::move(item));
        }
        return true;
    }

    // R26: every target or none; the copies hold the results
    void vcCompute(VcEditPlan& plan, jasperengine::editor::vertexcolor::EditResult& result, const std::atomic<bool>* cancel, const std::function<void(float)>& progress) {
        result.ok = true;
        for (size_t i = 0; i < plan.items.size(); i++) {
            VcWorkItem& item = plan.items[i];
            item.options.cancel = cancel;
            if (progress) {
                const float base = (float)i / (float)plan.items.size();
                const float span = 1.0f / (float)plan.items.size();
                item.options.progress = [progress, base, span](float done) { progress(base + span * done); };
            }
            jasperengine::editor::vertexcolor::TargetResult target;
            target.target = item.ref;
            target.result = jasperengine::vertexcolor::applyOps(item.data, *item.mesh, item.ops, item.options, false);
            if (target.result.cancelled || (cancel && cancel->load())) {
                result.ok = false;
                result.error = "cancelled: nothing was written.";
                return;
            }
            if (!target.result.ok) {
                result.ok = false;
                result.error = target.result.error + " (targets[" + std::to_string(i) + "], " + item.ref.key.describe() + ")";
                result.failedOp = target.result.failedOp;
                result.targets.clear();
                return;
            }
            if (item.data.hasOverrides()) target.splitVertices = jasperengine::vertexcolor::buildSplit(item.data, *item.mesh, {}).added;
            for (const jasperengine::vertexcolor::Warning& warning : target.result.warnings) result.warnings.push_back(warning);
            result.targets.push_back(std::move(target));
        }
    }

    void vcCommit(Project* project, uint32_t sceneId, VcEditPlan& plan, jasperengine::editor::vertexcolor::EditResult& result) {
        if (!result.ok || plan.dryRun) return;
        // a job's result applies only to what it started from
        for (const VcWorkItem& item : plan.items) {
            const std::shared_ptr<jasperengine::editor::vertexcolor::vc::Data> data = vcd::resolve(project, sceneId, item.ref.entity);
            const jasperengine::vertexcolor::TargetData* live = data ? data->find(item.ref.key) : nullptr;
            if ((live != nullptr) != item.painted || (live && jasperengine::vertexcolor::hashTarget(*live) != item.startHash)) {
                result.ok = false;
                result.error = "the vertex colors of " + vcEntityName(project, sceneId, item.ref.entity) +
                               " changed while this ran: nothing was written. Run it again.";
                result.targets.clear();
                return;
            }
        }
        CommandHistory* history = CommandHandle::get(sceneId);
        const size_t mark = history->mark();
        auto rollback = [&](const std::string& error) {
            while (history->position() > mark && history->canUndo() && history->undo()) {}
            history->dropRedo();
            result.ok = false;
            result.error = error;
            result.targets.clear();
            result.createdComponent = result.createdFile = false;
        };
        std::set<Entity> prepared;
        for (const VcWorkItem& item : plan.items) {
            if (!prepared.insert(item.ref.entity).second) continue;
            vcd::Prepared state;
            std::string error;
            if (!vcd::prepareForWrite(project, sceneId, item.ref.entity, state, &error)) {
                rollback(error);
                return;
            }
            result.createdComponent = result.createdComponent || state.createdComponent;
            result.createdFile = result.createdFile || state.createdFile;
        }
        auto* cmd = new vcd::VertexColorEditCmd(project, sceneId, plan.undoName, true);
        for (VcWorkItem& item : plan.items) {
            const std::shared_ptr<jasperengine::editor::vertexcolor::vc::Data> data = vcd::resolve(project, sceneId, item.ref.entity);
            if (!data) {
                delete cmd;
                rollback("the Vertex Colors file of " + vcEntityName(project, sceneId, item.ref.entity) + " could not be made.");
                return;
            }
            jasperengine::vertexcolor::TargetData* live = data->find(item.ref.key);
            if (live) {
                const jasperengine::vertexcolor::TargetData before = *live;
                item.data.snapshotPositions = std::move(live->snapshotPositions);
                item.data.snapshotNormals = std::move(live->snapshotNormals);
                item.data.snapshotIndices = std::move(live->snapshotIndices);
                *live = std::move(item.data);
                cmd->addTarget(item.ref.entity, &before, live);
            } else {
                vcd::fillSnapshot(item.data, *item.mesh);
                jasperengine::vertexcolor::TargetData& added = data->add(item.ref.key);
                added = std::move(item.data);
                cmd->addTarget(item.ref.entity, nullptr, &added);
            }
        }
        for (Entity entity : prepared) vcd::touched(project, sceneId, entity);
        history->addCommandNoMerge(cmd);
        history->groupSince(mark, plan.undoName);
        if (result.createdComponent) result.warnings.push_back({"created", "Added a Vertex Colors component and its .vcolors file."});
        // Table 2: a painted channel the material does not read
        for (Entity entity : prepared) {
            jasperengine::vertexcolor::ChannelMask painted = 0;
            for (const VcWorkItem& item : plan.items) {
                if (item.ref.entity == entity) painted |= vcPaintedChannels(item.ops);
            }
            for (const jasperengine::vertexcolor::Warning& warning : jasperengine::editor::vertexcolor::materialWarnings(project, sceneId, entity, painted)) result.warnings.push_back(warning);
        }
    }

    // what Add Component gives (R6): see vcd::initialFor
    jasperengine::vertexcolor::InitialValue vcDefaultInitial(Scene* scene, Entity entity, Vector4* custom) {
        const int layers = vcd::vertexBlendLayers(scene, entity);
        if (custom) *custom = layers >= 5 ? Vector4(0.0f, 0.0f, 0.0f, 0.0f) : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        if (layers >= 5) return jasperengine::vertexcolor::InitialValue::Custom;
        return layers > 0 ? jasperengine::vertexcolor::InitialValue::Black : jasperengine::vertexcolor::InitialValue::Asset;
    }

    bool vcAddComponent(Project* project, uint32_t sceneId, Entity entity, std::string* error) {
        Scene* scene = vcd::sceneOf(project, sceneId);
        CommandHistory* history = CommandHandle::get(sceneId);
        if (!history->addCommandNoMerge(new AddComponentCmd(project, sceneId, entity, kVcType)) || !vcd::componentOf(project, sceneId, entity)) {
            if (error) *error = "The Vertex Colors component could not be added to " + vcEntityName(project, sceneId, entity) + ".";
            return false;
        }
        // vertex_blend reads unpainted white as every layer at once: its meshes start on the base layer
        Vector4 custom;
        const jasperengine::vertexcolor::InitialValue initial = vcDefaultInitial(scene, entity, &custom);
        if (initial == jasperengine::vertexcolor::InitialValue::Custom) {
            history->addCommandNoMerge(new PropertyCmd<Vector4>(project, sceneId, entity, kVcType, "customInitial", custom));
        }
        if (initial != jasperengine::vertexcolor::InitialValue::Asset) {
            history->addCommandNoMerge(new PropertyCmd<int>(project, sceneId, entity, kVcType, "initialValue", (int)initial));
        }
        const std::string model = vcModelPath(scene, entity);
        if (!model.empty()) paintedModelChanged(*project, model);
        return true;
    }

    // a private file holding `data` for the entity, set as its Data (one undoable step on the history)
    bool vcAssignNewFile(Project* project, uint32_t sceneId, Entity entity, std::shared_ptr<jasperengine::editor::vertexcolor::vc::Data> data, std::string* error) {
        SceneProject* sceneProject = vcd::sceneProjectOf(project, sceneId);
        const std::string path = vcd::newFilePath(*project, *sceneProject, sceneProject->scene->getEntityName(entity));
        AssetRef ref;
        if (!vcd::writeNewFile(*project, path, *data, ref, error)) return false;
        vcd::registerData(*project, path, std::move(data), false);
        CommandHistory* history = CommandHandle::get(sceneId);
        // the values follow the path at once, both ways: an undo back to no file draws without them
        auto* set = new PropertyCmd<AssetRef>(project, sceneId, entity, kVcType, "data", ref, [project, sceneId, entity]() {
            VertexColorsComponent* component = vcd::componentOf(project, sceneId, entity);
            if (component && component->data.path.empty() && component->loaded) {
                component->loaded.reset();
                component->loadedPath.clear();
                component->revision++;
                jasperengine::vertexcolor::markVertexColorsChanged(vcd::sceneOf(project, sceneId), entity);
            }
            vcd::resolve(project, sceneId, entity);
        });
        if (!history->addCommandNoMerge(set)) {
            if (error) *error = "The Data of " + vcEntityName(project, sceneId, entity) + "'s Vertex Colors could not be set.";
            return false;
        }
        return vcd::resolve(project, sceneId, entity, error) != nullptr;
    }

}

// ---- detail (shared with the other vertex color sources)

jasperengine::vertexcolor::InitialValue vcd::initialFor(Project* project, uint32_t sceneId, Entity entity, Vector4* custom) {
    if (const VertexColorsComponent* component = componentOf(project, sceneId, entity)) {
        if (custom) *custom = component->customInitial;
        return component->initialValue;
    }
    return vcDefaultInitial(sceneOf(project, sceneId), entity, custom);
}

int vcd::vertexBlendLayers(Scene* scene, Entity entity) {
    if (!scene) return 0;
    int layers = 0;
    for (const jasperengine::vertexcolor::SceneTarget& target : jasperengine::vertexcolor::sceneTargets(scene, entity)) {
        const Material* found = jasperengine::vertexcolor::sceneTargetMaterial(scene, target);
        if (!found) continue;
        const Material& material = *found;
        if (material.shader.path != "library:vertex_blend") continue;
        const std::vector<std::string>& features = material.shaderFeatures;
        auto has = [&](const char* define) { return std::find(features.begin(), features.end(), define) != features.end(); };
        layers = std::max(layers, has("LAYERS_FIVE") ? 5 : has("LAYERS_FOUR") ? 4 : has("LAYERS_THREE") ? 3 : 2);
    }
    return layers;
}

bool vcd::fileShared(Project* project, uint32_t sceneId, Entity entity) {
    const VertexColorsComponent* component = componentOf(project, sceneId, entity);
    return component && !component->data.path.empty() && fileUserCount(project, component->data.path) > 1;
}

bool vcd::prepareForWrite(Project* project, uint32_t sceneId, Entity entity, Prepared& prepared, std::string* error) {
    if (!componentOf(project, sceneId, entity)) {
        if (!vcAddComponent(project, sceneId, entity, error)) return false;
        prepared.createdComponent = true;
    }
    VertexColorsComponent* component = componentOf(project, sceneId, entity);
    if (component->data.path.empty()) {
        auto data = std::make_shared<jasperengine::vertexcolor::Data>();
        data->format = component->format;
        if (!vcAssignNewFile(project, sceneId, entity, std::move(data), error)) return false;
        prepared.createdFile = true;
        return true;
    }
    const std::shared_ptr<jasperengine::vertexcolor::Data> live = resolve(project, sceneId, entity, error);
    if (!live) return false;
    if (fileShared(project, sceneId, entity)) {
        // R132: the first edit of a copy gives it its own file
        if (!vcAssignNewFile(project, sceneId, entity, std::make_shared<jasperengine::vertexcolor::Data>(*live), error)) return false;
        prepared.copied = true;
    }
    return true;
}

// ---- VertexColorEditing.h

bool editor::vertexcolor::canPaint(Project* project, uint32_t sceneId, Entity entity, std::string* why) {
    SceneProject* sceneProject = vcd::sceneProjectOf(project, sceneId);
    if (!sceneProject) {
        if (why) *why = "scene " + std::to_string(sceneId) + " is not open.";
        return false;
    }
    if (entity == NULL_ENTITY || !sceneProject->scene->isEntityCreated(entity)) {
        if (why) *why = "no GameObject " + std::to_string(entity) + " in this scene.";
        return false;
    }
    if (sceneProject->playState != ScenePlayState::STOPPED) {
        if (why) *why = "the scene is playing: stop Play to paint vertex colors (Lua's VertexColors:paint paints at runtime).";
        return false;
    }
    const std::string refusal = jasperengine::vertexcolor::paintRefusal(sceneProject->scene, entity);
    if (!refusal.empty()) {
        if (why) *why = refusal;
        return false;
    }
    return true;
}

std::vector<jasperengine::editor::vertexcolor::TargetInfo> editor::vertexcolor::listTargets(Project* project, uint32_t sceneId, Entity entity) {
    std::vector<TargetInfo> out;
    if (!canPaint(project, sceneId, entity, nullptr)) return out;
    Scene* scene = vcd::sceneOf(project, sceneId);
    const std::shared_ptr<jasperengine::vertexcolor::Data> data = vcd::resolve(project, sceneId, entity);
    for (const jasperengine::vertexcolor::SceneTarget& target : jasperengine::vertexcolor::sceneTargets(scene, entity)) {
        TargetInfo info;
        info.key = target.key;
        info.name = target.name;
        info.vertexCount = target.vertexCount;
        info.triangleCount = target.triangleCount;
        info.hasAssetColors = target.hasAssetColors;
        info.material = target.material;
        const jasperengine::vertexcolor::TargetData* painted = data ? data->find(target.key) : nullptr;
        info.painted = painted != nullptr;
        info.stale = painted && !jasperengine::vertexcolor::targetMatchesMesh(scene, entity, target.key, false);
        info.splitVertices = jasperengine::vertexcolor::boundSplitVertices(scene, entity, target.key);
        out.push_back(std::move(info));
    }
    return out;
}

std::shared_ptr<const jasperengine::vertexcolor::MeshView> editor::vertexcolor::meshView(Project* project, uint32_t sceneId, Entity entity, const jasperengine::vertexcolor::TargetKey& key) {
    Scene* scene = vcd::sceneOf(project, sceneId);
    return scene ? jasperengine::vertexcolor::sceneMeshView(scene, entity, key) : nullptr;
}

Matrix4 editor::vertexcolor::targetModel(Project* project, uint32_t sceneId, Entity entity, const jasperengine::vertexcolor::TargetKey& key) {
    Scene* scene = vcd::sceneOf(project, sceneId);
    return scene ? jasperengine::vertexcolor::sceneTargetModel(scene, entity, key) : Matrix4();
}

const jasperengine::vertexcolor::TargetData* editor::vertexcolor::targetData(Project* project, uint32_t sceneId, Entity entity, const jasperengine::vertexcolor::TargetKey& key) {
    const std::shared_ptr<jasperengine::vertexcolor::Data> data = vcd::resolve(project, sceneId, entity);
    return data ? data->find(key) : nullptr;
}

jasperengine::vertexcolor::TargetData editor::vertexcolor::targetDataOrInitial(Project* project, uint32_t sceneId, Entity entity, const jasperengine::vertexcolor::TargetKey& key) {
    if (const jasperengine::vertexcolor::TargetData* live = targetData(project, sceneId, entity, key)) return *live;
    jasperengine::vertexcolor::TargetData data;
    data.key = key;
    const std::shared_ptr<const jasperengine::vertexcolor::MeshView> mesh = meshView(project, sceneId, entity, key);
    if (!mesh) return data;
    data.fingerprint = mesh->fingerprint();
    Vector4 custom;
    const jasperengine::vertexcolor::InitialValue initial = vcd::initialFor(project, sceneId, entity, &custom);
    jasperengine::vertexcolor::initializeTarget(data, *mesh, initial, custom);
    return data;
}

std::shared_ptr<const jasperengine::vertexcolor::TransferSource> editor::vertexcolor::transferSource(Project* project, uint32_t sceneId, Entity entity, std::string* error) {
    struct Part {
        std::shared_ptr<const jasperengine::vertexcolor::MeshView> mesh;
        jasperengine::vertexcolor::TargetData data;
        Matrix4 model;
    };
    std::vector<Part> parts;
    bool corners = false;
    for (const TargetInfo& info : listTargets(project, sceneId, entity)) {
        Part part;
        part.mesh = meshView(project, sceneId, entity, info.key);
        if (!part.mesh) continue;
        part.data = targetDataOrInitial(project, sceneId, entity, info.key);
        // a stale target's overrides name corners of another index buffer: its vertices only
        if (!part.data.fingerprint.sameTopology(part.mesh->fingerprint())) part.data.corners.clear();
        part.model = targetModel(project, sceneId, entity, info.key);
        corners = corners || part.data.hasOverrides();
        parts.push_back(std::move(part));
    }
    auto source = std::make_shared<jasperengine::vertexcolor::TransferSource>();
    bool normals = true;
    for (const Part& part : parts) {
        const jasperengine::vertexcolor::MeshView& mesh = *part.mesh;
        const uint32_t first = (uint32_t)source->positions.size();
        const uint32_t count = std::min<uint32_t>(mesh.vertexCount(), (uint32_t)part.data.base.size());
        const Matrix4 normalMatrix = part.model.inverse().transpose();
        normals = normals && mesh.normals.size() == mesh.positions.size();
        for (uint32_t v = 0; v < count; v++) {
            source->positions.push_back(part.model * mesh.positions[v]);
            if (normals) {
                const Vector3& n = mesh.normals[v];
                const Vector4 w = normalMatrix * Vector4(n.x, n.y, n.z, 0.0f);
                source->normals.push_back(Vector3(w.x, w.y, w.z).normalized());
            }
            source->values.push_back(part.data.base[v]);
        }
        // with any hard edge, every triangle: a corner then finds its own side wherever it lands
        if (!corners) continue;
        for (uint32_t t = 0; t < mesh.triangleCount(); t++) {
            const uint32_t a = mesh.indices[t * 3], b = mesh.indices[t * 3 + 1], c = mesh.indices[t * 3 + 2];
            if (a >= count || b >= count || c >= count) continue;
            for (uint32_t k = 0; k < 3; k++) {
                source->indices.push_back(first + mesh.indices[t * 3 + k]);
                source->cornerValues.push_back(part.data.cornerValue(t * 3 + k, mesh.indices));
            }
        }
    }
    if (!normals) source->normals.clear();
    if (source->positions.empty()) {
        if (error) *error = CommandNames::entity(project, sceneId, entity) + " has no mesh loaded to transfer from.";
        return nullptr;
    }
    return source;
}

jasperengine::vertexcolor::ApplyOptions editor::vertexcolor::applyOptions(Project* project, uint32_t sceneId, Entity entity, const jasperengine::vertexcolor::TargetKey& key, bool world) {
    jasperengine::vertexcolor::ApplyOptions options;
    options.model = targetModel(project, sceneId, entity, key);
    options.world = world;
    options.initial = vcd::initialFor(project, sceneId, entity, &options.customInitial);
    const VertexColorsComponent* component = vcd::componentOf(project, sceneId, entity);
    options.format = component ? component->format : jasperengine::vertexcolor::Format::RGBA8;
    return options;
}

jasperengine::editor::vertexcolor::EditResult editor::vertexcolor::applyEdit(Project* project, uint32_t sceneId, const EditRequest& request) {
    EditResult result;
    auto plan = std::make_shared<VcEditPlan>();
    if (!vcPlan(project, sceneId, request, *plan, result)) return result;

    size_t elements = 0;
    bool bake = false;
    for (const VcWorkItem& item : plan->items) {
        elements += item.mesh->vertexCount();
        for (const jasperengine::vertexcolor::Op& op : item.ops) bake = bake || vcIsBake(op.type);
    }
    if (!request.done || (!bake && elements <= kVcJobElements)) {
        vcCompute(*plan, result, request.cancel, request.progress);
        vcCommit(project, sceneId, *plan, result);
        if (request.done) request.done(result);
        return result;
    }

    // R97: on a job; the result lands on the main thread, through the EditorHost queue (an idle editor
    // draws no frame), and is refused there if the values changed meanwhile
    result.ok = true;
    result.async = true;
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "vertex-colors-edit";
    const std::atomic<bool>* cancel = request.cancel;
    const std::function<void(float)> progress = request.progress;
    const std::function<void(const EditResult&)> done = request.done;
    jobs::submit([project, sceneId, plan, cancel, progress, done]() {
        auto computed = std::make_shared<EditResult>();
        try {
            vcCompute(*plan, *computed, cancel, progress);
        } catch (const std::exception& e) {
            computed->ok = false;
            computed->error = std::string("the edit failed: ") + e.what();
        }
        getEditorHost().enqueueMainThreadTask([project, sceneId, plan, computed, done]() {
            vcCommit(project, sceneId, *plan, *computed);
            done(*computed);
        });
    }, options);
    return result;
}

// ---- component-level actions

bool editor::vertexcolor::ensureComponent(Project* project, uint32_t sceneId, Entity entity, std::string* error) {
    if (vcd::componentOf(project, sceneId, entity)) return false;
    if (!canPaint(project, sceneId, entity, error)) return false;
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    if (!vcAddComponent(project, sceneId, entity, error)) {
        while (history->position() > mark && history->canUndo() && history->undo()) {}
        return false;
    }
    history->groupSince(mark, "Add Vertex Colors (" + vcEntityName(project, sceneId, entity) + ")");
    return true;
}

bool editor::vertexcolor::clearAll(Project* project, uint32_t sceneId, Entity entity, std::string* error) {
    VertexColorsComponent* component = vcd::componentOf(project, sceneId, entity);
    if (!component) {
        if (error) *error = vcEntityName(project, sceneId, entity) + " has no Vertex Colors.";
        return false;
    }
    if (component->data.path.empty()) return true;
    std::string why;
    if (!vcd::resolve(project, sceneId, entity, &why)) {
        if (error) *error = why;
        return false;
    }
    const std::string name = "Clear Vertex Colors (" + vcEntityName(project, sceneId, entity) + ")";
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    vcd::Prepared prepared;
    if (!vcd::prepareForWrite(project, sceneId, entity, prepared, error)) {
        while (history->position() > mark && history->canUndo() && history->undo()) {}
        return false;
    }
    const std::shared_ptr<jasperengine::vertexcolor::Data> data = vcd::resolve(project, sceneId, entity);
    auto* cmd = new vcd::VertexColorEditCmd(project, sceneId, name, true);
    for (const jasperengine::vertexcolor::TargetData& target : data->targets) cmd->addTarget(entity, &target, nullptr);
    data->targets.clear();
    vcd::touched(project, sceneId, entity);
    history->addCommandNoMerge(cmd);
    history->groupSince(mark, name);
    return true;
}

bool editor::vertexcolor::setFormat(Project* project, uint32_t sceneId, Entity entity, jasperengine::vertexcolor::Format format, std::string* error) {
    VertexColorsComponent* component = vcd::componentOf(project, sceneId, entity);
    if (!component) {
        if (error) *error = vcEntityName(project, sceneId, entity) + " has no Vertex Colors.";
        return false;
    }
    if (component->format == format) return true;
    const std::string name = std::string("Set Vertex Colors Format to ") + (format == jasperengine::vertexcolor::Format::RGBA16F ? "High Precision" : "RGBA8") + " (" +
                             vcEntityName(project, sceneId, entity) + ")";
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    auto fail = [&]() {
        while (history->position() > mark && history->canUndo() && history->undo()) {}
        return false;
    };
    if (!component->data.path.empty()) {
        vcd::Prepared prepared;
        if (!vcd::prepareForWrite(project, sceneId, entity, prepared, error)) return fail();
        const std::shared_ptr<jasperengine::vertexcolor::Data> data = vcd::resolve(project, sceneId, entity);
        auto* cmd = new vcd::VertexColorEditCmd(project, sceneId, name, true);
        cmd->setFormatChange(entity, data->format, format);
        for (jasperengine::vertexcolor::TargetData& target : data->targets) {
            const jasperengine::vertexcolor::TargetData before = target;
            for (Vector4& value : target.base) value = jasperengine::vertexcolor::quantize(value, format);
            for (jasperengine::vertexcolor::CornerValue& corner : target.corners) corner.value = jasperengine::vertexcolor::quantize(corner.value, format);
            cmd->addTarget(entity, &before, &target);
        }
        data->format = format;
        vcd::touched(project, sceneId, entity);
        history->addCommandNoMerge(cmd);
    }
    history->addCommandNoMerge(new PropertyCmd<int>(project, sceneId, entity, kVcType, "format", (int)format));
    history->groupSince(mark, name);
    return true;
}

bool editor::vertexcolor::makeUnique(Project* project, uint32_t sceneId, Entity entity, std::string* error) {
    VertexColorsComponent* component = vcd::componentOf(project, sceneId, entity);
    if (!component || component->data.path.empty()) {
        if (error) *error = vcEntityName(project, sceneId, entity) + " has no Vertex Colors file to copy.";
        return false;
    }
    const std::shared_ptr<jasperengine::vertexcolor::Data> live = vcd::resolve(project, sceneId, entity, error);
    if (!live) return false;
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    if (!vcAssignNewFile(project, sceneId, entity, std::make_shared<jasperengine::vertexcolor::Data>(*live), error)) {
        while (history->position() > mark && history->canUndo() && history->undo()) {}
        return false;
    }
    history->groupSince(mark, "Make Vertex Colors Unique (" + vcEntityName(project, sceneId, entity) + ")");
    return true;
}

int editor::vertexcolor::fileUserCount(Project* project, const std::string& assetPath) {
    if (!project || assetPath.empty()) return 0;
    int users = 0;
    auto count = [&](const EntityRegistry& registry) {
        auto array = registry.getComponentArray<VertexColorsComponent>();
        for (size_t i = 0; i < array->size(); i++) {
            if (array->getComponentFromIndex(i).data.path == assetPath) users++;
        }
    };
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (sceneProject->scene) count(*sceneProject->scene);
    }
    // a prefab's component is the default of its instances (R132): an instance edit gets its own file
    project->forEachPrefabAsset([&](const std::filesystem::path&, Prefab& prefab) {
        if (prefab.registry) count(*prefab.registry);
    });
    return users;
}

std::function<void(Project* project, uint32_t sceneId, Entity entity)>& editor::vertexcolor::paintToolHook() {
    static std::function<void(Project*, uint32_t, Entity)> hook;
    return hook;
}
