// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// An interactive stroke (docs/specs/V1-vertex-colors.md R30, R31, R106, R107): many applyOps calls on
// the live values, so the Scene view shows it as it goes, one undo step at the end. The strength cap
// holds over the whole stroke through each target's StrokeState. Cancel puts the values back and
// takes back what the stroke added (the component, its file, a private copy).

#include "VertexColorEditing.h"
#include "VertexColorEditorInternal.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "subsystem/VertexColorScene.h"

#include <algorithm>
#include <optional>
#include <set>
#include <span>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::vertexcolor {

namespace vcd = jasperengine::editor::vertexcolor::detail;

struct StrokeSession::Impl {
    struct Target {
        TargetRef ref;
        std::shared_ptr<const vc::MeshView> mesh;
        vc::StrokeState state;
        bool touched = false;                   // the stroke wrote it
        std::optional<vc::TargetData> before;   // the live target at the first write (none: not painted)
        vc::TargetData scratch;                 // the values until the first write that changes something
        bool scratchReady = false;
    };

    Project* project = nullptr;
    uint32_t sceneId = 0;
    std::string undoName;
    std::vector<Target> targets;
    size_t mark = 0;
    bool active = false;
    std::set<Entity> prepared;

    // the live target to write, preparing the entity first (one undoable step each, grouped at end())
    vc::TargetData* liveTarget(Target& target, std::string* error) {
        if (!prepared.count(target.ref.entity)) {
            vcd::Prepared state;
            if (!vcd::prepareForWrite(project, sceneId, target.ref.entity, state, error)) return nullptr;
            prepared.insert(target.ref.entity);
        }
        const std::shared_ptr<vc::Data> data = vcd::resolve(project, sceneId, target.ref.entity, error);
        return data ? data->find(target.ref.key) : nullptr;
    }
};

StrokeSession::StrokeSession(): impl(std::make_unique<Impl>()) {}

StrokeSession::~StrokeSession() {
    if (active()) end();
}

bool StrokeSession::begin(Project* project, uint32_t sceneId, std::vector<TargetRef> targets, const std::string& undoName, std::string* error) {
    if (active()) end();
    Impl& s = *impl;
    s = Impl();
    s.project = project;
    s.sceneId = sceneId;
    s.undoName = undoName.empty() ? "Paint Vertex Colors" : undoName;
    if (targets.empty()) {
        if (error) *error = "nothing to paint: no target.";
        return false;
    }
    for (const TargetRef& ref : targets) {
        std::string why;
        if (!canPaint(project, sceneId, ref.entity, &why)) {
            if (error) *error = why;
            return false;
        }
        Impl::Target target;
        target.ref = ref;
        target.mesh = meshView(project, sceneId, ref.entity, ref.key);
        if (!target.mesh) {
            if (error) *error = "the mesh of " + ref.key.describe() + " is not loaded yet.";
            return false;
        }
        if (targetData(project, sceneId, ref.entity, ref.key) && !vc::targetMatchesMesh(vcd::sceneOf(project, sceneId), ref.entity, ref.key, false)) {
            if (error) *error = ref.key.describe() + " was painted on another version of its mesh (vertex_colors_unmatched): clear it to paint it again.";
            return false;
        }
        s.targets.push_back(std::move(target));
    }
    s.mark = CommandHandle::get(sceneId)->mark();
    s.active = true;
    return true;
}

bool StrokeSession::apply(const vc::Op& op, std::function<bool(const Vector3&)> visible) {
    Impl& s = *impl;
    if (!s.active) return false;
    bool ok = true;
    for (Impl::Target& target : s.targets) {
        vc::ApplyOptions options = applyOptions(s.project, s.sceneId, target.ref.entity, target.ref.key, true);
        options.visible = visible;
        options.stroke = &target.state;
        options.stats = false;
        if (!target.touched) {
            // until a stamp reaches it, the target is worked on a copy: a stroke that never reaches a
            // GameObject adds it nothing
            if (!target.scratchReady) {
                target.scratch = targetDataOrInitial(s.project, s.sceneId, target.ref.entity, target.ref.key);
                target.scratchReady = true;
            }
            const vc::OpsResult result = vc::applyOps(target.scratch, *target.mesh, std::span<const vc::Op>(&op, 1), options);
            if (!result.ok) {
                ok = false;
                continue;
            }
            const bool changed = !result.ops.empty() && (result.ops[0].changedVertices > 0 || result.ops[0].changedCorners > 0);
            if (!changed) continue;
            if (const vc::TargetData* live = targetData(s.project, s.sceneId, target.ref.entity, target.ref.key)) target.before = *live;
            std::string error;
            vc::TargetData* live = s.liveTarget(target, &error);
            if (!live) {
                const std::shared_ptr<vc::Data> data = vcd::resolve(s.project, s.sceneId, target.ref.entity, &error);
                if (!data) {
                    ok = false;
                    continue;
                }
                live = &data->add(target.ref.key);
                *live = std::move(target.scratch);
            } else {
                target.scratch.snapshotPositions = std::move(live->snapshotPositions);
                target.scratch.snapshotNormals = std::move(live->snapshotNormals);
                target.scratch.snapshotIndices = std::move(live->snapshotIndices);
                *live = std::move(target.scratch);
            }
            target.touched = true;
            vcd::touched(s.project, s.sceneId, target.ref.entity, false);
            continue;
        }
        const std::shared_ptr<vc::Data> data = vcd::resolve(s.project, s.sceneId, target.ref.entity);
        vc::TargetData* live = data ? data->find(target.ref.key) : nullptr;
        if (!live) {
            ok = false;
            continue;
        }
        const vc::OpsResult result = vc::applyOps(*live, *target.mesh, std::span<const vc::Op>(&op, 1), options);
        if (!result.ok) {
            ok = false;
            continue;
        }
        if (!result.ops.empty() && (result.ops[0].changedVertices > 0 || result.ops[0].changedCorners > 0)) {
            vcd::touched(s.project, s.sceneId, target.ref.entity, false);
        }
    }
    return ok;
}

void StrokeSession::lift() {
    // the next stamp starts where the pointer comes back; the per-vertex cap state stays (R31)
    for (Impl::Target& target : impl->targets) {
        target.state.hasLastPoint = false;
        target.state.hasLastStamp = false;
        target.state.carry = 0.0f;
    }
}

void StrokeSession::end() {
    Impl& s = *impl;
    if (!s.active) return;
    s.active = false;
    CommandHistory* history = CommandHandle::get(s.sceneId);
    auto* cmd = new vcd::VertexColorEditCmd(s.project, s.sceneId, s.undoName, true);
    for (Impl::Target& target : s.targets) {
        if (!target.touched) continue;
        const vc::TargetData* live = targetData(s.project, s.sceneId, target.ref.entity, target.ref.key);
        cmd->addTarget(target.ref.entity, target.before ? &*target.before : nullptr, live);
        // the stroke's file write, once (R8)
        vcd::touched(s.project, s.sceneId, target.ref.entity);
    }
    if (cmd->empty()) {
        delete cmd;
    } else {
        history->addCommandNoMerge(cmd);
    }
    history->groupSince(s.mark, s.undoName);
}

void StrokeSession::cancel() {
    Impl& s = *impl;
    if (!s.active) return;
    s.active = false;
    for (Impl::Target& target : s.targets) {
        if (!target.touched) continue;
        const std::shared_ptr<vc::Data> data = vcd::resolve(s.project, s.sceneId, target.ref.entity);
        if (!data) continue;
        vc::TargetData* live = data->find(target.ref.key);
        if (target.before) {
            if (live) *live = *target.before;
        } else if (live) {
            const vc::TargetKey key = target.ref.key;
            data->targets.erase(std::remove_if(data->targets.begin(), data->targets.end(), [&key](const vc::TargetData& t) { return t.key == key; }),
                                data->targets.end());
        }
        vcd::touched(s.project, s.sceneId, target.ref.entity);
    }
    // the component, file or copy the stroke made goes with it
    CommandHistory* history = CommandHandle::get(s.sceneId);
    while (history->position() > s.mark && history->canUndo() && history->undo()) {}
    history->dropRedo();
}

bool StrokeSession::active() const {
    return impl && impl->active;
}

}  // namespace jasperengine::editor::vertexcolor
