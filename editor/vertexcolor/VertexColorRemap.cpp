// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Stale targets (docs/specs/V1-vertex-colors.md R10): a painted target whose mesh no longer matches
// its fingerprint, a reimport having changed the source, is transferred from its snapshot (the mesh
// it was painted on, R9) by nearest point with a normal filter (R60), its corner overrides by the
// nearest old corner on the same side of the hard edge. The scene becomes modified, the
// file keeps its bytes until the scene is saved, and the issue is logged once. A target with no
// snapshot, or no vertex within reach, stays as it was (unmatched): the file is never made poorer.

#include "VertexColorEditing.h"
#include "VertexColorEditorInternal.h"
#include "VertexColorProject.h"

#include "Out.h"
#include "Project.h"
#include "component/VertexColorsComponent.h"
#include "subsystem/VertexColorScene.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <span>
#include <tuple>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vertexcolor;
namespace vcd = jasperengine::editor::vertexcolor::detail;

namespace {

    // the transfer reaches this share of the mesh's bounds diagonal: a reimport moves little
    constexpr float kVcRemapReach = 0.02f;

    using VcTargetId = std::tuple<uint32_t, Entity, int32_t, int32_t, int32_t>;

    VcTargetId vcTargetId(uint32_t sceneId, Entity entity, const vc::TargetKey& key) {
        return {sceneId, entity, key.node, key.primitive, key.submesh};
    }

    // the mesh each target was last checked against (the same pointer until its mesh reloads)
    std::map<VcTargetId, const vc::MeshView*>& vcChecked() {
        static std::map<VcTargetId, const vc::MeshView*> checked;
        return checked;
    }

    std::map<uint32_t, std::vector<RemapIssue>>& vcIssues() {
        static std::map<uint32_t, std::vector<RemapIssue>> issues;
        return issues;
    }

    void vcRecord(uint32_t sceneId, const RemapIssue& issue) {
        std::vector<RemapIssue>& list = vcIssues()[sceneId];
        for (RemapIssue& known : list) {
            if (known.entity == issue.entity && known.key == issue.key) {
                known = issue;
                return;
            }
        }
        list.push_back(issue);
    }

    // true when transferred; `issue` says how it went either way
    bool vcRemap(Project* project, SceneProject& sceneProject, Entity entity, vc::TargetData& target, const vc::MeshView& mesh, RemapIssue& issue) {
        issue.entity = entity;
        issue.key = target.key;
        issue.unmatched = true;
        if (target.snapshotPositions.empty() || target.snapshotPositions.size() != target.base.size()) return false;
        auto source = std::make_shared<vc::TransferSource>();
        source->positions = target.snapshotPositions;
        if (target.snapshotNormals.size() == target.snapshotPositions.size()) source->normals = target.snapshotNormals;
        source->values = target.base;
        // the hard edges go with their faces (R10, R60): each old corner's value at its triangle
        if (target.hasOverrides() && target.snapshotIndices.size() == target.fingerprint.indexCount) {
            source->indices = target.snapshotIndices;
            source->cornerValues.resize(source->indices.size());
            for (uint32_t c = 0; c < (uint32_t)source->indices.size(); c++) source->cornerValues[c] = target.cornerValue(c, source->indices);
        }

        vc::TargetData fresh;
        fresh.key = target.key;
        fresh.fingerprint = mesh.fingerprint();
        Vector4 custom;
        const vc::InitialValue initial = vcd::initialFor(project, sceneProject.id, entity, &custom);
        vc::initializeTarget(fresh, mesh, initial, custom);

        vc::Op op;
        op.type = vc::OpType::Transfer;
        op.channels = vc::CHANNELS_RGBA;
        op.source = source;
        const Vector3 size = mesh.bounds.isNull() ? Vector3(1.0f, 1.0f, 1.0f) : mesh.bounds.getSize();
        op.maxDistance = std::max(1e-4f, kVcRemapReach * std::sqrt(size.x * size.x + size.y * size.y + size.z * size.z));
        op.normalAngle = 60.0f;
        op.unmatched = vc::Unmatched::Keep;
        // local space on both sides: the snapshot is the mesh's own
        vc::ApplyOptions options;
        options.model = Matrix4();
        options.world = false;
        options.initial = initial;
        options.customInitial = custom;
        options.format = vcd::componentOf(project, sceneProject.id, entity)->format;
        options.stats = false;
        const vc::OpsResult result = vc::applyOps(fresh, mesh, std::span<const vc::Op>(&op, 1), options);
        if (!result.ok || result.ops.empty() || result.ops[0].unmatched >= mesh.vertexCount()) return false;
        issue.unmatched = false;
        issue.meanDistance = result.ops[0].meanDistance;
        vcd::fillSnapshot(fresh, mesh);
        target = std::move(fresh);
        return true;
    }

}

void vcd::resetRemap() {
    vcChecked().clear();
    vcIssues().clear();
}

std::vector<RemapIssue> editor::vertexcolor::remapIssues(Project* project, uint32_t sceneId) {
    (void)project;
    const auto it = vcIssues().find(sceneId);
    return it == vcIssues().end() ? std::vector<RemapIssue>() : it->second;
}

void editor::tickVertexColors(Project* project) {
    if (!project) return;
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        Scene* scene = sceneProject->scene;
        if (!scene || sceneProject->playState != ScenePlayState::STOPPED) continue;
        auto array = scene->getComponentArray<VertexColorsComponent>();
        for (size_t i = 0; i < array->size(); i++) {
            const Entity entity = array->getEntity(i);
            const VertexColorsComponent& component = array->getComponentFromIndex(i);
            if (component.data.path.empty()) continue;
            const std::shared_ptr<vc::Data> data = component.loaded && component.loadedPath == component.data.path
                                                       ? component.loaded
                                                       : vcd::resolve(project, sceneProject->id, entity);
            if (!data) continue;
            bool remapped = false;
            for (vc::TargetData& target : data->targets) {
                const std::shared_ptr<const vc::MeshView> mesh = vc::sceneMeshView(scene, entity, target.key);
                if (!mesh) continue;
                const vc::MeshView*& checked = vcChecked()[vcTargetId(sceneProject->id, entity, target.key)];
                if (checked == mesh.get()) continue;
                checked = mesh.get();
                if (mesh->fingerprint() == target.fingerprint) continue;
                RemapIssue issue;
                const bool transferred = vcRemap(project, *sceneProject, entity, target, *mesh, issue);
                remapped = remapped || transferred;
                vcRecord(sceneProject->id, issue);
                const std::string name = scene->getEntityName(entity);
                if (transferred) {
                    Out::warning("Vertex Colors of '%s' [vertex_colors_remapped]: %s was painted on another version of its mesh and was "
                                 "transferred to the new one (mean distance %.4g). Check it with sample_mesh_colors and repaint what moved; "
                                 "save the scene to keep it.", name.c_str(), target.key.describe().c_str(), (double)issue.meanDistance);
                } else {
                    Out::warning("Vertex Colors of '%s' [vertex_colors_unmatched]: %s was painted on another version of its mesh and nothing "
                                 "on the new one matches it, so it draws with its Initial Value. Repaint it (Clear, then paint) or reimport the "
                                 "model it was painted on.", name.c_str(), target.key.describe().c_str());
                }
            }
            if (remapped) {
                // R10: the old file stays as it is until the scene is saved
                vcd::holdUntilSave(*project, component.data.path);
                vcd::touched(project, sceneProject->id, entity);
                sceneProject->isModified = true;
            }
        }
    }
    forgetUnusedVertexColorFiles(*project);
}
