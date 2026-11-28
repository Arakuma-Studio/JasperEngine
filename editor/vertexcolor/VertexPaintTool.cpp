// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vertexcolor/VertexPaintTool.h"

#include "Project.h"
#include "Scene.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "picking/MeshRaycast.h"
#include "vertexcolor/VertexPaintImages.h"
#include "render/SceneRender.h"
#include "subsystem/VertexColorScene.h"

#include <algorithm>
#include <cmath>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::vertexcolor {

namespace {

    // the meshes and terrains that hide a surface point; never the tools' own lines and icons
    MeshRaycastOptions occluderOptions() {
        MeshRaycastOptions options;
        options.kinds = MeshRaycast::MESH | MeshRaycast::INSTANCE | MeshRaycast::TERRAIN;
        return options;
    }

    // the largest axis scale of a matrix: a world radius in the target's local units
    float maxScale(const Matrix4& model) {
        const Vector3 o = model * Vector3(0.0f, 0.0f, 0.0f);
        const float sx = (model * Vector3(1.0f, 0.0f, 0.0f) - o).length();
        const float sy = (model * Vector3(0.0f, 1.0f, 0.0f) - o).length();
        const float sz = (model * Vector3(0.0f, 0.0f, 1.0f) - o).length();
        return std::max(sx, std::max(sy, sz));
    }

    std::string targetLabel(Scene* scene, Entity entity, const std::vector<TargetInfo>& infos, const vc::TargetKey& key) {
        std::string name = scene ? scene->getEntityName(entity) : std::string();
        if (infos.size() > 1) {
            for (const TargetInfo& info : infos) {
                if (info.key == key) name += " / " + info.name;
            }
        }
        return name;
    }

}

// ==================================================================== state

VertexPaintTools& VertexPaintTools::get() {
    static VertexPaintTools tools;
    return tools;
}

void VertexPaintTools::setProject(Project* project) {
    if (projectPtr == project) return;
    reset();
    projectPtr = project;
}

void VertexPaintTools::reset() {
    if (activeTool && projectPtr) SceneTools::deactivate(activeTool->scene());
    cancelStroke();
    hideCursor();
    activeTool = nullptr;
    view = ViewMemory();
    lastClicked = ClickedFace();
    clearMessages();
}

SceneProject* VertexPaintTools::sceneProject(uint32_t sceneId) const {
    return projectPtr ? projectPtr->getScene(sceneId) : nullptr;
}

void VertexPaintTools::arm(PaintTool tool) {
    if (!projectPtr) return;
    armIn(projectPtr->getSelectedSceneId(), tool);
}

void VertexPaintTools::armIn(uint32_t sceneId, PaintTool tool) {
    if (!projectPtr) return;
    SceneProject* scene = sceneProject(sceneId);
    if (!scene || !scene->scene || scene->playState != ScenePlayState::STOPPED) return;
    if (tool == PaintTool::Pick && brush.tool != PaintTool::Pick) toolBeforePick = brush.tool;
    if (stroke.active && tool != brush.tool) endStroke();
    brush.tool = tool;
    // one view at a time: the tool of another scene's view goes first
    if (activeTool && toolScene != sceneId) SceneTools::deactivate(toolScene);
    if (!activeTool) {
        SceneTools::activate(sceneId, std::make_unique<VertexPaintSceneTool>(sceneId));
    }
    enterView(sceneId);
    cursorRev++;
}

void VertexPaintTools::exit() {
    if (activeTool) SceneTools::deactivate(activeTool->scene());
}

bool VertexPaintTools::isArmedIn(uint32_t sceneId) const {
    return activeTool && toolScene == sceneId;
}

void VertexPaintTools::detach(VertexPaintSceneTool* tool) {
    if (activeTool != tool) return;
    cancelStroke();
    hideCursor();
    leaveView();
    // R50: the element selection goes with the tool unless pinned
    if (!brush.pinSelection && projectPtr && sceneProject(toolScene)) clearElementSelection(projectPtr, toolScene);
    activeTool = nullptr;
}

// ---- R102: the Draw Mode while armed

void VertexPaintTools::enterView(uint32_t sceneId) {
    SceneProject* scene = sceneProject(sceneId);
    if (!scene) return;
    if (view.held && view.sceneId == sceneId) return;
    if (view.held) leaveView();
    view.held = true;
    view.sceneId = sceneId;
    view.previousMode = scene->displaySettings.drawMode;
    view.previousChannel = scene->displaySettings.vertexColorChannel;
    view.setMode = ViewDrawMode::VERTEX_COLOR;
    view.setChannel = ViewVertexColorChannel::RGB;
    scene->displaySettings.drawMode = view.setMode;
    scene->displaySettings.vertexColorChannel = view.setChannel;
    scene->needUpdateRender = true;
}

void VertexPaintTools::leaveView() {
    if (!view.held) return;
    view.held = false;
    SceneProject* scene = sceneProject(view.sceneId);
    if (!scene) return;
    // the user changed the view meanwhile (the toolbar): it is theirs now
    const bool untouched = scene->displaySettings.drawMode == view.setMode &&
        (view.setMode != ViewDrawMode::VERTEX_COLOR || scene->displaySettings.vertexColorChannel == view.setChannel);
    if (!untouched) return;
    scene->displaySettings.drawMode = view.previousMode;
    scene->displaySettings.vertexColorChannel = view.previousChannel;
    scene->needUpdateRender = true;
}

void VertexPaintTools::setViewChannel(ViewVertexColorChannel channel) {
    const uint32_t sceneId = view.held ? view.sceneId : (projectPtr ? projectPtr->getSelectedSceneId() : 0);
    SceneProject* scene = sceneProject(sceneId);
    if (!scene) return;
    scene->displaySettings.drawMode = ViewDrawMode::VERTEX_COLOR;
    scene->displaySettings.vertexColorChannel = channel;
    scene->needUpdateRender = true;
    if (view.held) {
        view.setMode = ViewDrawMode::VERTEX_COLOR;
        view.setChannel = channel;
    }
}

void VertexPaintTools::setViewOff() {
    if (!view.held) return;
    SceneProject* scene = sceneProject(view.sceneId);
    if (!scene) return;
    scene->displaySettings.drawMode = view.previousMode == ViewDrawMode::VERTEX_COLOR ? ViewDrawMode::SHADED : view.previousMode;
    scene->needUpdateRender = true;
    view.setMode = scene->displaySettings.drawMode;
}

bool VertexPaintTools::viewIsOff() const {
    if (!view.held) return true;
    const SceneProject* scene = projectPtr ? projectPtr->getScene(view.sceneId) : nullptr;
    return !scene || scene->displaySettings.drawMode != ViewDrawMode::VERTEX_COLOR;
}

void VertexPaintTools::cycleViewChannel() {
    const uint32_t sceneId = view.held ? view.sceneId : (projectPtr ? projectPtr->getSelectedSceneId() : 0);
    SceneProject* scene = sceneProject(sceneId);
    if (!scene) return;
    int next = scene->displaySettings.drawMode == ViewDrawMode::VERTEX_COLOR ? ((int)scene->displaySettings.vertexColorChannel + 1) % 5 : 0;
    setViewChannel((ViewVertexColorChannel)next);
}

// ==================================================================== targets

std::vector<TargetRef> VertexPaintTools::targets(uint32_t sceneId) const {
    std::vector<TargetRef> out;
    SceneProject* scene = sceneProject(sceneId);
    if (!scene || !scene->scene) return out;
    for (Entity entity : projectPtr->getSelectedEntities(sceneId)) {
        if (!canPaint(projectPtr, sceneId, entity, nullptr)) continue;
        for (const TargetInfo& info : listTargets(projectPtr, sceneId, entity)) {
            out.push_back(TargetRef{entity, info.key});
        }
    }
    return out;
}

std::vector<TargetRef> VertexPaintTools::actingTargets(uint32_t sceneId) const {
    std::vector<TargetRef> all = targets(sceneId);
    if (targetFilter >= 0 && targetFilter < (int)all.size()) return {all[(size_t)targetFilter]};
    return all;
}

TargetHit VertexPaintTools::hitTargets(uint32_t sceneId, const Ray& ray) const {
    TargetHit best;
    SceneProject* scene = sceneProject(sceneId);
    if (!scene || !scene->scene) return best;
    Scene* s = scene->scene;
    const Vector3 origin = ray.getOrigin();
    const Vector3 direction = ray.getDirection();
    const float rayLength = direction.length();
    if (!(rayLength > 0.0f)) return best;
    std::map<Entity, std::vector<vc::SceneTarget>> sceneTargetsOf;
    std::map<Entity, std::vector<TargetInfo>> infosOf;
    for (const TargetRef& ref : actingTargets(sceneId)) {
        std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, ref.entity, ref.key);
        if (!mesh || mesh->triangleCount() == 0) continue;
        const Matrix4 model = targetModel(projectPtr, sceneId, ref.entity, ref.key);
        if (!sceneTargetsOf.count(ref.entity)) sceneTargetsOf[ref.entity] = vc::sceneTargets(s, ref.entity);
        const std::vector<vc::SceneTarget>& list = sceneTargetsOf[ref.entity];
        MeshHit hit;
        bool bvhMissed = false;
        // what is drawn (a posed skin too) through the picking BVH, then the triangle in the bind pose (D12)
        for (const vc::SceneTarget& target : list) {
            if (target.key != ref.key) continue;
            const MeshRaycastHit picked = MeshRaycast::raycastEntity(s, target.meshEntity, ray);
            bvhMissed = !picked.hit;
            if (!picked.hit || picked.submesh != target.submesh) break;
            // the BVH numbers triangles across the mesh's submeshes, by its own count of each
            uint32_t first = 0, count = 0;
            if (!pickTriangleRange(s, target.meshEntity, target.submesh, first, count) || count != mesh->triangleCount()) break;
            if (picked.triangle < first || picked.triangle - first >= count) break;
            hit.hit = true;
            hit.triangle = picked.triangle - first;
            hit.barycentric = picked.barycentric;
            hit.point = surfacePoint(*mesh, model, hit.triangle, hit.barycentric);
            hit.distance = (hit.point - origin).dotProduct(direction / rayLength);
            hit.normal = picked.faceNormal.length() > 0.0f ? picked.faceNormal.normalized() : Vector3(0.0f, 1.0f, 0.0f);
            break;
        }
        // the BVH's triangle could not be mapped, or it saw nothing: the bind-pose triangles themselves,
        // unless the mesh is too big to test each frame (a BVH miss is a miss then)
        constexpr uint32_t BRUTE_FORCE_TRIANGLES = 20000;
        if (!hit.hit && (!bvhMissed || mesh->triangleCount() <= BRUTE_FORCE_TRIANGLES)) hit = raycastMesh(*mesh, model, origin, direction, brush.xray);
        if (!hit.hit || (best.hit && hit.distance >= best.distance)) continue;
        if (!infosOf.count(ref.entity)) infosOf[ref.entity] = listTargets(projectPtr, sceneId, ref.entity);
        best.hit = true;
        best.target = ref;
        best.name = targetLabel(s, ref.entity, infosOf[ref.entity], ref.key);
        best.triangle = hit.triangle;
        best.barycentric = hit.barycentric;
        best.point = hit.point;
        best.normal = hit.normal;
        best.distance = hit.distance;
    }
    return best;
}

std::function<bool(const Vector3&)> VertexPaintTools::visibilityFor(uint32_t sceneId, const Vector3& eye, float tolerance) const {
    SceneProject* scene = sceneProject(sceneId);
    if (!scene || !scene->scene) return nullptr;
    Scene* s = scene->scene;
    return [s, eye, tolerance](const Vector3& point) {
        Vector3 toward = point - eye;
        const float distance = toward.length();
        if (!(distance > tolerance)) return true;
        toward /= distance;
        const MeshRaycastHit hit = MeshRaycast::raycast(s, eye, toward, distance, LAYER_MASK_DEFAULT_RAYCAST, occluderOptions());
        return !hit.hit || hit.distance >= distance - tolerance;
    };
}

// ==================================================================== strokes

vc::Op VertexPaintTools::strokeOpAt(const Vector3& point, float pressure, const Vector3& eye) const {
    const vc::StrokePoint sample{point, pressureOf(pressure)};
    vc::Op op = strokeOp(brush, stroke.mode, stroke.secondary, std::span<const vc::StrokePoint>(&sample, 1));
    if (brush.frontFacesOnly) {
        Vector3 toward = eye - point;
        if (toward.length() > 0.0f) op.filters.push_back(facingFilter(toward.normalized()));
    }
    return op;
}

bool VertexPaintTools::beginStroke(uint32_t sceneId, const Ray& ray, bool ctrl, bool shift, float pressure) {
    endStroke();
    error.clear();
    const std::vector<TargetRef> refs = actingTargets(sceneId);
    if (refs.empty()) {
        error = "Select a Model or a Mesh to paint.";
        return false;
    }
    stroke.mode = effectiveStrokeMode(brush.tool, ctrl);
    stroke.secondary = shift;
    stroke.sceneId = sceneId;
    stroke.eye = ray.getOrigin();
    stroke.session = std::make_unique<StrokeSession>();
    std::string why;
    if (!stroke.session->begin(projectPtr, sceneId, refs, strokeUndoName(stroke.mode), &why)) {
        error = why.empty() ? "The stroke could not start." : why;
        stroke.session.reset();
        return false;
    }
    stroke.active = true;
    continueStroke(ray, pressure);
    return true;
}

void VertexPaintTools::continueStroke(const Ray& ray, float pressure) {
    if (!stroke.active || !stroke.session) return;
    const TargetHit hit = hitTargets(stroke.sceneId, ray);
    if (!hit.hit) {
        // off the surface: the return starts a new segment, not a capsule across the gap (the cap holds)
        stroke.session->lift();
        return;
    }
    stroke.eye = ray.getOrigin();
    const vc::Op op = strokeOpAt(hit.point, pressure, stroke.eye);
    const float tolerance = std::max(brush.radius * 0.05f, 1e-3f);
    stroke.session->apply(op, brush.skipOccluded ? visibilityFor(stroke.sceneId, stroke.eye, tolerance) : nullptr);
    if (SceneProject* scene = sceneProject(stroke.sceneId)) scene->needUpdateRender = true;
}

void VertexPaintTools::endStroke() {
    if (!stroke.active) return;
    if (stroke.session) stroke.session->end();
    stroke.session.reset();
    stroke.active = false;
}

void VertexPaintTools::cancelStroke() {
    if (!stroke.active) return;
    if (stroke.session) stroke.session->cancel();
    stroke.session.reset();
    stroke.active = false;
    if (SceneProject* scene = sceneProject(stroke.sceneId)) scene->needUpdateRender = true;
}

bool VertexPaintTools::scriptedStroke(Project* project, uint32_t sceneId, const std::vector<TargetRef>& targets, const VertexPaintSettings& settings,
                                      vc::StrokeMode mode, bool secondary, const std::vector<vc::StrokePoint>& points, std::string& why) {
    if (points.empty()) {
        why = "A stroke needs a point.";
        return false;
    }
    if (targets.empty()) {
        why = "A stroke needs a target.";
        return false;
    }
    StrokeSession session;
    if (!session.begin(project, sceneId, targets, strokeUndoName(mode), &why)) return false;
    // one call per point, as a drag sends them: the session carries the cap and the spacing across
    for (const vc::StrokePoint& point : points) {
        const vc::Op op = strokeOp(settings, mode, secondary, std::span<const vc::StrokePoint>(&point, 1));
        if (!session.apply(op)) {
            session.cancel();
            why = "The stroke was refused.";
            return false;
        }
    }
    session.end();
    why.clear();
    return true;
}

// ==================================================================== edits

bool VertexPaintTools::runEdit(uint32_t sceneId, std::vector<vc::Op> ops, const std::string& undoName, std::function<void(const EditResult&)> done,
                               const std::atomic<bool>* cancel, std::function<void(float)> progress) {
    error.clear();
    notice.clear();
    EditRequest request;
    request.targets = actingTargets(sceneId);
    if (request.targets.empty()) {
        error = "Select a Model or a Mesh to paint.";
        return false;
    }
    request.ops = std::move(ops);
    request.undoName = undoName;
    request.cancel = cancel;
    request.progress = std::move(progress);
    if (done) {
        request.done = [this, done](const EditResult& result) {
            if (!result.ok) error = result.error;
            else if (!result.warnings.empty()) notice = result.warnings.front().message;
            done(result);
        };
    }
    const EditResult result = applyEdit(projectPtr, sceneId, request);
    if (!result.ok && !result.async) {
        error = result.error.empty() ? "The edit was refused." : result.error;
        return false;
    }
    if (!result.async && !result.warnings.empty()) notice = result.warnings.front().message;
    if (SceneProject* scene = sceneProject(sceneId)) scene->needUpdateRender = true;
    return true;
}

bool VertexPaintTools::pickAt(uint32_t sceneId, const Ray& ray, bool secondary) {
    const TargetHit hit = hitTargets(sceneId, ray);
    if (!hit.hit) return false;
    std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, hit.target.entity, hit.target.key);
    if (!mesh) return false;
    // R108: the stored corner values at the hit, not the screen's pixel
    const vc::TargetData data = targetDataOrInitial(projectPtr, sceneId, hit.target.entity, hit.target.key);
    Vector4 value = vc::valueAt(data, *mesh, hit.triangle, hit.barycentric);
    if (secondary) brush.secondary = value;
    else brush.primary = value;
    if (brush.tool == PaintTool::Pick) brush.tool = toolBeforePick;
    cursorRev++;
    return true;
}

bool VertexPaintTools::fillAt(uint32_t sceneId, const Ray& ray, bool ctrl, bool secondary) {
    const TargetHit hit = hitTargets(sceneId, ray);
    if (!hit.hit) return false;
    std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, hit.target.entity, hit.target.key);
    if (!mesh) return false;
    vc::Target target;
    std::string undo = "Fill Vertex Colors";
    if (ctrl) {
        // the faces of the clicked colour (Table 1: fill on a color target)
        const vc::TargetData data = targetDataOrInitial(projectPtr, sceneId, hit.target.entity, hit.target.key);
        target.kind = vc::TargetKind::Color;
        target.color = brush.similar;
        target.color.value = vc::valueAt(data, *mesh, hit.triangle, hit.barycentric);
        target.element = brush.element;
    } else if (!selectionTarget(sceneId, hit.target.entity, hit.target.key, target)) {
        // the connected part under the cursor (bucket fill)
        vc::Selection seed;
        seed.kind = vc::ElementKind::Face;
        seed.ids = {hit.triangle};
        vc::Selection part = vc::linked(*mesh, seed);
        if (brush.element == vc::Element::Vertex) part = vc::convert(*mesh, part, vc::ElementKind::Vertex);
        target = vc::toTarget(part);
    }
    EditRequest request;
    request.targets = {hit.target};
    request.ops = {fillOp(brush, target, secondary)};
    request.undoName = undo;
    const EditResult result = applyEdit(projectPtr, sceneId, request);
    if (!result.ok && !result.async) {
        error = result.error;
        return false;
    }
    if (SceneProject* scene = sceneProject(sceneId)) scene->needUpdateRender = true;
    return true;
}

bool VertexPaintTools::gradient(uint32_t sceneId, const Vector3& from, const Vector3& to) {
    if ((to - from).length() <= 0.0f) return false;
    EditRequest request;
    for (const TargetRef& ref : actingTargets(sceneId)) {
        vc::Target target;
        if (!selectionTarget(sceneId, ref.entity, ref.key, target)) target.kind = vc::TargetKind::All;
        request.targets.push_back(ref);
        request.perTargetOps.push_back({gradientOp(brush, target, from, to)});
    }
    if (request.targets.empty()) return false;
    request.undoName = "Gradient Vertex Colors";
    const EditResult result = applyEdit(projectPtr, sceneId, request);
    if (!result.ok && !result.async) {
        error = result.error;
        return false;
    }
    if (SceneProject* scene = sceneProject(sceneId)) scene->needUpdateRender = true;
    return true;
}

// ==================================================================== selection

bool VertexPaintTools::setSelection(uint32_t sceneId, std::vector<ElementSelection> selection, const std::string& undoName) {
    if (!projectPtr) return false;
    setElementSelection(projectPtr, sceneId, std::move(selection), undoName);
    if (SceneProject* scene = sceneProject(sceneId)) scene->needUpdateRender = true;
    return true;
}

size_t VertexPaintTools::selectedCount(uint32_t sceneId) const {
    size_t count = 0;
    for (const ElementSelection& item : elementSelection(sceneId)) count += item.selection.ids.size();
    return count;
}

bool VertexPaintTools::selectAt(uint32_t sceneId, const Ray& ray, bool add, bool remove) {
    const TargetHit hit = hitTargets(sceneId, ray);
    std::vector<ElementSelection> next = elementSelection(sceneId);
    if (!hit.hit) {
        // a click on nothing without a modifier clears, as in every selection
        if (add || remove || next.empty()) return false;
        return setSelection(sceneId, {}, "Select None");
    }
    std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, hit.target.entity, hit.target.key);
    if (!mesh) return false;
    lastClicked.valid = true;
    lastClicked.target = hit.target;
    lastClicked.triangle = hit.triangle;
    lastClicked.normal = hit.triangle < mesh->faceNormals.size() ? mesh->faceNormals[hit.triangle] : Vector3(0.0f, 1.0f, 0.0f);
    vc::Selection picked;
    picked.kind = brush.selectionMode;
    picked.ids = {elementAtHit(*mesh, brush.selectionMode, hit.triangle, hit.barycentric)};
    if (!add && !remove) next.clear();
    bool found = false;
    for (ElementSelection& item : next) {
        if (item.entity != hit.target.entity || item.key != hit.target.key) continue;
        item.selection = combineSelection(item.selection, picked, add, remove);
        found = true;
    }
    if (!found && !remove) next.push_back(ElementSelection{hit.target.entity, hit.target.key, picked});
    next.erase(std::remove_if(next.begin(), next.end(), [](const ElementSelection& item) { return item.selection.empty(); }), next.end());
    return setSelection(sceneId, std::move(next), std::string("Select ") + elementKindName(brush.selectionMode, true));
}

bool VertexPaintTools::selectRegion(uint32_t sceneId, const std::vector<Vector2>& region, bool lasso, bool add, bool remove, const ProjectFn& project,
                                    const Vector3& eye) {
    if (region.size() < 2) return false;
    std::vector<ElementSelection> next = elementSelection(sceneId);
    if (!add && !remove) next.clear();
    for (const TargetRef& ref : actingTargets(sceneId)) {
        std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, ref.entity, ref.key);
        if (!mesh) continue;
        const Matrix4 model = targetModel(projectPtr, sceneId, ref.entity, ref.key);
        VisibleFn visible;
        if (!brush.xray) {
            // R51: only what the view shows, by depth test; a point a hair off the surface toward the eye
            const float tolerance = std::max(1e-3f, (mesh->bounds.getMaximum() - mesh->bounds.getMinimum()).length() * maxScale(model) * 1e-3f);
            const std::function<bool(const Vector3&)> sees = visibilityFor(sceneId, eye, tolerance);
            visible = [sees, eye](const Vector3& world, const Vector3& normal) {
                if (normal.dotProduct(eye - world) < 0.0f) return false;
                return !sees || sees(world);
            };
        }
        const vc::Selection picked = lasso ? selectInLasso(*mesh, model, brush.selectionMode, region, project, visible)
                                           : selectInRect(*mesh, model, brush.selectionMode, region.front(), region.back(), project, visible);
        bool found = false;
        for (ElementSelection& item : next) {
            if (item.entity != ref.entity || item.key != ref.key) continue;
            item.selection = combineSelection(item.selection, picked, add, remove);
            found = true;
        }
        if (!found && !remove && !picked.empty()) next.push_back(ElementSelection{ref.entity, ref.key, picked});
    }
    next.erase(std::remove_if(next.begin(), next.end(), [](const ElementSelection& item) { return item.selection.empty(); }), next.end());
    return setSelection(sceneId, std::move(next), std::string("Select ") + elementKindName(brush.selectionMode, true));
}

bool VertexPaintTools::selectCommand(uint32_t sceneId, SelectCommand command) {
    std::vector<ElementSelection> current = elementSelection(sceneId);
    std::vector<ElementSelection> next;
    const vc::ElementKind kind = brush.selectionMode;
    const char* undo = "Select";
    for (const TargetRef& ref : actingTargets(sceneId)) {
        std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, ref.entity, ref.key);
        if (!mesh) continue;
        vc::Selection mine;
        mine.kind = kind;
        for (const ElementSelection& item : current) {
            if (item.entity == ref.entity && item.key == ref.key) mine = item.selection.kind == kind ? item.selection : vc::convert(*mesh, item.selection, kind);
        }
        vc::Selection out;
        out.kind = kind;
        switch (command) {
            case SelectCommand::All: out = vc::selectAll(*mesh, kind); undo = "Select All"; break;
            case SelectCommand::None: undo = "Select None"; break;
            case SelectCommand::Invert: out = vc::invert(*mesh, mine); undo = "Invert Selection"; break;
            case SelectCommand::Grow: out = vc::grow(*mesh, mine); undo = "Grow Selection"; break;
            case SelectCommand::Shrink: out = vc::shrink(*mesh, mine); undo = "Shrink Selection"; break;
            case SelectCommand::Linked: out = vc::linked(*mesh, mine); undo = "Select Linked"; break;
            case SelectCommand::FlatRegion: {
                undo = "Select Flat Region";
                // the clicked face, else the selection's first face
                int32_t seed = -1;
                if (lastClicked.valid && lastClicked.target == ref) seed = (int32_t)lastClicked.triangle;
                else if (!mine.empty()) {
                    const vc::Selection faces = vc::convert(*mesh, mine, vc::ElementKind::Face, true);
                    if (!faces.empty()) seed = (int32_t)faces.ids.front();
                }
                if (seed < 0) { out = mine; break; }
                out = vc::flatRegion(*mesh, (uint32_t)seed, brush.flatRegionAngle);
                if (kind != vc::ElementKind::Face) out = vc::convert(*mesh, out, kind);
                out = vc::unite(mine, out);
                break;
            }
            case SelectCommand::Similar: {
                undo = "Select Similar Color";
                if (mine.empty()) { out = mine; break; }
                const vc::TargetData data = targetDataOrInitial(projectPtr, sceneId, ref.entity, ref.key);
                vc::ColorMatch match = brush.similar;
                match.value = vc::averageValue(data, *mesh, mine);
                out = vc::unite(mine, vc::similarColor(data, *mesh, kind, match));
                break;
            }
            case SelectCommand::ByNormal: {
                undo = "Select by Normal";
                Vector3 direction(0.0f, 1.0f, 0.0f);
                if (lastClicked.valid && lastClicked.target == ref) direction = lastClicked.normal;
                out = vc::byNormal(*mesh, kind, direction, brush.byNormalAngle);
                break;
            }
            case SelectCommand::ByChannelRange: {
                undo = "Select by Channel Range";
                const vc::TargetData data = targetDataOrInitial(projectPtr, sceneId, ref.entity, ref.key);
                out = vc::byChannelRange(data, *mesh, kind, brush.rangeChannel, brush.rangeMin, brush.rangeMax);
                break;
            }
        }
        if (!out.empty()) next.push_back(ElementSelection{ref.entity, ref.key, out});
    }
    return setSelection(sceneId, std::move(next), undo);
}

void VertexPaintTools::setSelectionMode(uint32_t sceneId, vc::ElementKind kind) {
    if (brush.selectionMode == kind) return;
    brush.selectionMode = kind;
    std::vector<ElementSelection> next = elementSelection(sceneId);
    if (next.empty()) return;
    for (ElementSelection& item : next) {
        std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, item.entity, item.key);
        if (mesh) item.selection = vc::convert(*mesh, item.selection, kind);
    }
    next.erase(std::remove_if(next.begin(), next.end(), [](const ElementSelection& item) { return item.selection.empty(); }), next.end());
    setSelection(sceneId, std::move(next), std::string("Select ") + elementKindName(kind, true));
}

bool VertexPaintTools::applyColorToSelection(uint32_t sceneId, const Vector4& value) {
    const Vector4 keep = brush.primary;
    brush.primary = value;
    const bool ok = applyToSelection(sceneId, false);
    brush.primary = keep;
    return ok;
}

bool VertexPaintTools::applyToSelection(uint32_t sceneId, bool secondary) {
    error.clear();
    EditRequest request;
    size_t count = 0;
    vc::ElementKind kind = brush.selectionMode;
    for (const ElementSelection& item : elementSelection(sceneId)) {
        if (item.selection.empty()) continue;
        request.targets.push_back(TargetRef{item.entity, item.key});
        request.perTargetOps.push_back({applyOp(brush, vc::toTarget(item.selection), secondary)});
        count += item.selection.ids.size();
        kind = item.selection.kind;
    }
    if (request.targets.empty()) {
        error = "Select vertices, faces or corners first (Select tool).";
        return false;
    }
    request.undoName = applyUndoName(kind, count);
    const EditResult result = applyEdit(projectPtr, sceneId, request);
    if (!result.ok && !result.async) {
        error = result.error;
        return false;
    }
    if (SceneProject* scene = sceneProject(sceneId)) scene->needUpdateRender = true;
    return true;
}

bool VertexPaintTools::gradientAcross(uint32_t sceneId) {
    // the selection's world extent; the gradient runs along its longest axis
    bool any = false;
    Vector3 lo, hi;
    for (const ElementSelection& item : elementSelection(sceneId)) {
        std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, item.entity, item.key);
        if (!mesh) continue;
        const Matrix4 model = targetModel(projectPtr, sceneId, item.entity, item.key);
        const vc::Selection vertices = vc::convert(*mesh, item.selection, vc::ElementKind::Vertex);
        for (uint32_t l : vertices.ids) {
            const Vector3 p = model * mesh->logicalPositions[l];
            if (!any) { lo = hi = p; any = true; }
            lo.makeFloor(p);
            hi.makeCeil(p);
        }
    }
    if (!any) {
        error = "Select vertices, faces or corners first (Select tool).";
        return false;
    }
    const Vector3 extent = hi - lo;
    int axis = 0;
    if (extent.y > extent[(unsigned)axis]) axis = 1;
    if (extent.z > extent[(unsigned)axis]) axis = 2;
    const Vector3 center = (lo + hi) * 0.5f;
    Vector3 from = center, to = center;
    from[(unsigned)axis] = lo[(unsigned)axis];
    to[(unsigned)axis] = hi[(unsigned)axis];
    if ((to - from).length() <= 0.0f) {
        error = "The selection has no extent to run a gradient across.";
        return false;
    }
    return gradient(sceneId, from, to);
}

bool VertexPaintTools::copyColorFromSelection(uint32_t sceneId, std::string* hex) {
    Vector4 sum(0.0f, 0.0f, 0.0f, 0.0f);
    size_t weight = 0;
    for (const ElementSelection& item : elementSelection(sceneId)) {
        std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, item.entity, item.key);
        if (!mesh || item.selection.empty()) continue;
        const vc::TargetData data = targetDataOrInitial(projectPtr, sceneId, item.entity, item.key);
        const Vector4 average = vc::averageValue(data, *mesh, item.selection);
        const size_t n = item.selection.ids.size();
        sum = sum + average * (float)n;
        weight += n;
    }
    if (weight == 0) {
        error = "Select vertices, faces or corners first (Select tool).";
        return false;
    }
    brush.primary = sum / (float)weight;
    if (hex) *hex = vc::toHex(brush.primary, brush.primary.w < 1.0f);
    cursorRev++;
    return true;
}

bool VertexPaintTools::recolor(uint32_t sceneId, const Vector4& from, const Vector4& to) {
    return runEdit(sceneId, {recolorOp(from, to)}, "Recolor Vertex Colors");
}

bool VertexPaintTools::selectByColor(uint32_t sceneId, const Vector4& value) {
    std::vector<ElementSelection> next;
    for (const TargetRef& ref : actingTargets(sceneId)) {
        std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, ref.entity, ref.key);
        if (!mesh) continue;
        const vc::TargetData data = targetDataOrInitial(projectPtr, sceneId, ref.entity, ref.key);
        vc::ColorMatch match;
        match.value = value;
        match.tolerance = 0.5f / 255.0f + 1e-5f;
        match.ignoreAlpha = false;
        match.coverage = 1.0f;
        const vc::Selection picked = vc::similarColor(data, *mesh, brush.selectionMode, match);
        if (!picked.empty()) next.push_back(ElementSelection{ref.entity, ref.key, picked});
    }
    return setSelection(sceneId, std::move(next), "Select by Color");
}

// ==================================================================== the cursor

void VertexPaintTools::hideCursor() {
    if (cursorShown) cursorRev++;
    cursorShown = false;
    cursorAlpha = 0.0f;
    cursorKey.valid = false;
    affected.clear();
    hasRay = false;
}

void VertexPaintTools::hover(uint32_t sceneId, const Ray& ray, bool ctrl, bool shift) {
    const bool sameRay = hasRay && rayOrigin == ray.getOrigin() && rayDirection == ray.getDirection();
    if (!sameRay) pointerMoved = std::chrono::steady_clock::now();
    hasRay = true;
    rayOrigin = ray.getOrigin();
    rayDirection = ray.getDirection();

    const PaintTool tool = brush.tool;
    const bool brushTool = tool == PaintTool::Paint || tool == PaintTool::Smooth;
    const TargetHit hit = hitTargets(sceneId, ray);
    status.clear();
    if (!hit.hit) {
        if (cursorShown) cursorRev++;
        cursorShown = false;
        cursorKey.valid = false;
        affected.clear();
        if (actingTargets(sceneId).empty()) status = "Select a Model or a Mesh to paint";
        return;
    }
    std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, hit.target.entity, hit.target.key);
    if (!mesh) return;
    const Matrix4 model = targetModel(projectPtr, sceneId, hit.target.entity, hit.target.key);
    // R109
    if (density.mesh != mesh.get() || density.radius != brush.radius) {
        density.mesh = mesh.get();
        density.radius = brush.radius;
        density.low = isLowDensity(*mesh, model, brush.radius);
    }
    if (brushTool && density.low) status = lowDensityText(hit.name, mesh->logicalCount());
    else status = hit.name + ": " + std::to_string(mesh->logicalCount()) + " vertices";
    if (!brushTool) {
        if (cursorShown) cursorRev++;
        cursorShown = false;
        affected.clear();
        return;
    }
    const vc::StrokeMode mode = stroke.active ? stroke.mode : effectiveStrokeMode(tool, ctrl);
    rebuildCursor(sceneId, hit, mode, stroke.active ? stroke.secondary : shift);
}

void VertexPaintTools::rebuildCursor(uint32_t sceneId, const TargetHit& hit, vc::StrokeMode mode, bool secondary) {
    const Vector4 tint = cursorTint(brush, mode, secondary, 1.0f);
    if (cursorKey.valid && cursorKey.target == hit.target && cursorKey.point == hit.point && cursorKey.radius == brush.radius &&
        cursorKey.hardness == brush.hardness && cursorKey.strength == brush.strength && cursorKey.tint == tint) {
        cursorShown = true;
        return;
    }
    cursorKey = CursorKey{hit.target, hit.triangle, hit.point, brush.radius, brush.hardness, brush.strength, tint, true};
    SceneProject* scene = sceneProject(sceneId);
    Scene* s = scene ? scene->scene : nullptr;
    std::vector<vc::SceneTarget> list = s ? vc::sceneTargets(s, hit.target.entity) : std::vector<vc::SceneTarget>();
    Entity meshEntity = NULL_ENTITY;
    for (const vc::SceneTarget& target : list) {
        if (target.key == hit.target.key) meshEntity = target.meshEntity;
    }
    const float radius = brush.radius;
    // each ring point dropped onto the surface along the normal, a hair above it (terrain's drape)
    auto drape = [s, meshEntity, radius](const Vector3& point, const Vector3& normal) {
        if (!s || meshEntity == NULL_ENTITY) return point + normal * (radius * 0.01f);
        const Vector3 from = point + normal * radius;
        const Ray ray(from, normal * (-2.0f * radius));
        const MeshRaycastHit picked = MeshRaycast::raycastEntity(s, meshEntity, ray);
        if (!picked.hit) return point + normal * (radius * 0.01f);
        return picked.point + normal * (radius * 0.01f);
    };
    buildCursor(hit.point, hit.normal, radius, brush.hardness, brush.strength, tint, drape, cursor);

    // R103: the vertices the next stamp moves, front faces only when the brush says so
    affected.clear();
    if (brush.showAffected) {
        std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, hit.target.entity, hit.target.key);
        if (mesh) {
            const Matrix4 model = targetModel(projectPtr, sceneId, hit.target.entity, hit.target.key);
            const Matrix4 inverse = model.inverse();
            const float scale = maxScale(model);
            std::vector<uint32_t> found;
            mesh->queryLogical(inverse * hit.point, scale > 0.0f ? radius / scale : radius, found);
            const Matrix4 normalMatrix = inverse.transpose();
            for (uint32_t l : found) {
                if (affected.size() >= AFFECTED_DOTS_CAP) break;
                const Vector3 world = model * mesh->logicalPositions[l];
                if ((world - hit.point).length() > radius) continue;
                if (brush.frontFacesOnly && l < mesh->logicalNormals.size()) {
                    const Vector3& n = mesh->logicalNormals[l];
                    const Vector4 wn = normalMatrix * Vector4(n.x, n.y, n.z, 0.0f);
                    if (Vector3(wn.x, wn.y, wn.z).dotProduct(rayOrigin - world) < 0.0f) continue;
                }
                affected.push_back(world);
            }
        }
    }
    cursorShown = true;
    cursorRev++;
}

void VertexPaintTools::rebuildSelectionLines(uint32_t sceneId) {
    SceneProject* scene = sceneProject(sceneId);
    // what the lines are drawn from: the selection, the targets, the wire toggle, and the history
    // (an undo or a move changes them)
    uint64_t signature = 1469598103934665603ull;
    auto mix = [&signature](uint64_t v) { signature = (signature ^ v) * 1099511628211ull; };
    const std::vector<ElementSelection>& selection = elementSelection(sceneId);
    for (const ElementSelection& item : selection) {
        mix(item.entity);
        mix((uint64_t)(uint32_t)item.key.node << 32 | (uint32_t)item.key.primitive);
        mix((uint64_t)(uint32_t)item.key.submesh);
        mix((uint64_t)item.selection.kind);
        mix(item.selection.ids.size());
        for (uint32_t id : item.selection.ids) mix(id);
    }
    const bool wire = brush.wireOnSelection && brush.tool == PaintTool::Select;
    const std::vector<TargetRef> acting = wire ? actingTargets(sceneId) : std::vector<TargetRef>();
    for (const TargetRef& ref : acting) mix(ref.entity);
    mix(wire ? 1 : 0);
    if (CommandHistory* history = scene ? CommandHandle::get(sceneId) : nullptr) mix(history->position());
    if (signature == selectionSignature) return;
    selectionSignature = signature;
    selectionRev++;
    selectionDrawn.points.clear();
    selectionDrawn.colors.clear();
    constexpr size_t MAX_SEGMENTS = 3800;
    auto segment = [this](const Vector3& a, const Vector3& b, const Vector4& color) {
        if (selectionDrawn.colors.size() >= MAX_SEGMENTS) return;
        selectionDrawn.points.push_back(a);
        selectionDrawn.points.push_back(b);
        selectionDrawn.colors.push_back(color);
    };
    const Vector4 highlight(1.0f, 0.62f, 0.12f, 1.0f);
    for (const ElementSelection& item : selection) {
        std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, item.entity, item.key);
        if (!mesh) continue;
        const Matrix4 model = targetModel(projectPtr, sceneId, item.entity, item.key);
        const float diagonal = (mesh->bounds.getMaximum() - mesh->bounds.getMinimum()).length();
        const float lift = diagonal * 0.002f;
        const float mark = diagonal * 0.012f;
        auto lifted = [&](const Vector3& local, const Vector3& normal) { return model * (local + normal * lift); };
        for (uint32_t id : item.selection.ids) {
            switch (item.selection.kind) {
                case vc::ElementKind::Face: {
                    if (id >= mesh->triangleCount()) break;
                    const Vector3 n = id < mesh->faceNormals.size() ? mesh->faceNormals[id] : Vector3();
                    for (uint32_t k = 0; k < 3; k++) {
                        segment(lifted(mesh->positions[mesh->indices[id * 3 + k]], n), lifted(mesh->positions[mesh->indices[id * 3 + (k + 1) % 3]], n), highlight);
                    }
                    break;
                }
                case vc::ElementKind::Vertex: {
                    if (id >= mesh->logicalCount()) break;
                    const Vector3& p = mesh->logicalPositions[id];
                    const Vector3 n = id < mesh->logicalNormals.size() ? mesh->logicalNormals[id] : Vector3();
                    segment(lifted(p - Vector3(mark, 0, 0), n), lifted(p + Vector3(mark, 0, 0), n), highlight);
                    segment(lifted(p - Vector3(0, mark, 0), n), lifted(p + Vector3(0, mark, 0), n), highlight);
                    segment(lifted(p - Vector3(0, 0, mark), n), lifted(p + Vector3(0, 0, mark), n), highlight);
                    break;
                }
                case vc::ElementKind::Corner: {
                    if (id >= mesh->indices.size()) break;
                    const uint32_t t = id / 3;
                    const Vector3 n = t < mesh->faceNormals.size() ? mesh->faceNormals[t] : Vector3();
                    const Vector3& v = mesh->positions[mesh->indices[id]];
                    const Vector3 centroid = (mesh->positions[mesh->indices[t * 3]] + mesh->positions[mesh->indices[t * 3 + 1]] + mesh->positions[mesh->indices[t * 3 + 2]]) / 3.0f;
                    segment(lifted(v, n), lifted(v + (centroid - v) * 0.4f, n), highlight);
                    break;
                }
            }
        }
    }
    // the wire of the targets under the Select tool: where the elements are
    const Vector4 wireColor(0.75f, 0.78f, 0.85f, 0.35f);
    for (const TargetRef& ref : acting) {
        std::shared_ptr<const vc::MeshView> mesh = meshView(projectPtr, sceneId, ref.entity, ref.key);
        if (!mesh) continue;
        const Matrix4 model = targetModel(projectPtr, sceneId, ref.entity, ref.key);
        const float lift = (mesh->bounds.getMaximum() - mesh->bounds.getMinimum()).length() * 0.001f;
        for (uint32_t t = 0; t < mesh->triangleCount() && selectionDrawn.colors.size() < MAX_SEGMENTS; t++) {
            const Vector3 n = t < mesh->faceNormals.size() ? mesh->faceNormals[t] : Vector3();
            for (uint32_t k = 0; k < 3; k++) {
                const uint32_t a = mesh->indices[t * 3 + k];
                const uint32_t b = mesh->indices[t * 3 + (k + 1) % 3];
                // each edge once: from its lower logical vertex
                if (mesh->logical[a] > mesh->logical[b]) continue;
                segment(model * (mesh->positions[a] + n * lift), model * (mesh->positions[b] + n * lift), wireColor);
            }
        }
    }
    if (scene) scene->needUpdateRender = true;
}

void VertexPaintTools::tick(bool cameraMoving) {
    if (activeTool) rebuildSelectionLines(toolScene);
    float wanted = 0.0f;
    if (cursorShown) {
        const double rested = std::chrono::duration<double>(std::chrono::steady_clock::now() - pointerMoved).count();
        // the stroke's own feedback does not fade under a held button
        wanted = stroke.active ? 1.0f : voxelDecalAlpha(rested, cameraMoving);
    }
    if (wanted != cursorAlpha) {
        cursorAlpha = wanted;
        cursorRev++;
        if (SceneProject* scene = sceneProject(toolScene)) scene->needUpdateRender = true;
    }
}

// ==================================================================== the SceneTool

VertexPaintSceneTool::VertexPaintSceneTool(uint32_t sceneId) : sceneId(sceneId) {
    VertexPaintTools& tools = VertexPaintTools::get();
    tools.activeTool = this;
    tools.toolScene = sceneId;
}

VertexPaintSceneTool::~VertexPaintSceneTool() {
    VertexPaintTools& tools = VertexPaintTools::get();
    if (tools.activeTool == this) tools.activeTool = nullptr;
}

const char* VertexPaintSceneTool::label() const {
    labelText = std::string("Vertex Paint: ") + paintToolName(VertexPaintTools::get().currentTool());
    return labelText.c_str();
}

bool VertexPaintSceneTool::available(const SceneProject& sceneProject) const {
    // Esc with nothing to cancel leaves the tool; edits in Play would not be saved
    return !exitRequested && sceneProject.playState == ScenePlayState::STOPPED;
}

SceneToolPress VertexPaintSceneTool::press(const SceneToolEvent& event) {
    VertexPaintTools& tools = VertexPaintTools::get();
    gesture = Gesture::None;
    pressAt = Vector2(event.x, event.y);
    pressRay = event.ray;
    modAdd = event.shift;
    modRemove = event.ctrl;
    switch (tools.currentTool()) {
        case PaintTool::Paint:
        case PaintTool::Smooth:
            // a press off every surface still belongs to the tool: no selection click under it
            if (tools.beginStroke(sceneId, event.ray, event.ctrl, event.shift, event.pressure)) gesture = Gesture::Stroke;
            return SceneToolPress::Captured;
        case PaintTool::Select:
            gesture = Gesture::SelectRegion;
            region = {pressAt};
            regionMoved = false;
            return SceneToolPress::Captured;
        case PaintTool::Gradient: {
            const TargetHit hit = tools.hitTargets(sceneId, event.ray);
            gradientHasStart = hit.hit;
            if (hit.hit) {
                gradientFrom = gradientTo = hit.point;
                gesture = Gesture::Gradient;
            }
            return SceneToolPress::Captured;
        }
        case PaintTool::Fill:
        case PaintTool::Pick:
            gesture = Gesture::Click;
            return SceneToolPress::Captured;
    }
    return SceneToolPress::NotMine;
}

void VertexPaintSceneTool::drag(const SceneToolEvent& event) {
    VertexPaintTools& tools = VertexPaintTools::get();
    switch (gesture) {
        case Gesture::Stroke:
            tools.continueStroke(event.ray, event.pressure);
            tools.hover(sceneId, event.ray, event.ctrl, event.shift);
            break;
        case Gesture::SelectRegion: {
            const Vector2 at(event.x, event.y);
            if ((at - pressAt).length() > 3.0f) regionMoved = true;
            if (tools.settings().selectShape == SelectShape::Lasso) {
                if (region.empty() || (region.back() - at).length() >= 2.0f) region.push_back(at);
            } else {
                region = {pressAt, at};
            }
            break;
        }
        case Gesture::Gradient: {
            const TargetHit hit = tools.hitTargets(sceneId, event.ray);
            if (hit.hit) {
                gradientTo = hit.point;
            } else {
                // off the surface: the point at the start's depth along the ray
                const Vector3 o = event.ray.getOrigin();
                Vector3 d = event.ray.getDirection();
                if (d.length() > 0.0f) {
                    d.normalize();
                    gradientTo = o + d * (gradientFrom - o).length();
                }
            }
            break;
        }
        case Gesture::Click:
        case Gesture::None:
            break;
    }
}

void VertexPaintSceneTool::release(const SceneToolEvent& event) {
    VertexPaintTools& tools = VertexPaintTools::get();
    const Gesture done = gesture;
    gesture = Gesture::None;
    switch (done) {
        case Gesture::Stroke:
            tools.endStroke();
            break;
        case Gesture::SelectRegion: {
            if (!regionMoved) {
                tools.selectAt(sceneId, pressRay, modAdd, modRemove);
                break;
            }
            Project* project = tools.project();
            SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
            if (!sceneProject || !sceneProject->sceneRender) break;
            SceneRender* render = sceneProject->sceneRender;
            const ProjectFn project2d = [render](const Vector3& world, Vector2& screen) { return render->worldToScreen(world, screen); };
            if (tools.settings().selectShape == SelectShape::Box) region = {pressAt, Vector2(event.x, event.y)};
            tools.selectRegion(sceneId, region, tools.settings().selectShape == SelectShape::Lasso, modAdd, modRemove, project2d, pressRay.getOrigin());
            break;
        }
        case Gesture::Gradient:
            if (gradientHasStart) tools.gradient(sceneId, gradientFrom, gradientTo);
            gradientHasStart = false;
            break;
        case Gesture::Click:
            if (tools.currentTool() == PaintTool::Fill) tools.fillAt(sceneId, event.ray, event.ctrl, event.shift);
            else if (tools.currentTool() == PaintTool::Pick) tools.pickAt(sceneId, event.ray, event.shift);
            break;
        case Gesture::None:
            break;
    }
    region.clear();
}

void VertexPaintSceneTool::hover(const SceneToolEvent& event) {
    VertexPaintTools::get().hover(sceneId, event.ray, event.ctrl, event.shift);
}

bool VertexPaintSceneTool::key(ImGuiKey key, const SceneToolEvent&) {
    if (key != ImGuiKey_Escape) return false;
    VertexPaintTools& tools = VertexPaintTools::get();
    // R106: the stroke first (its values come back), then a drag in progress, then the tool itself
    if (tools.isStrokeActive()) {
        tools.cancelStroke();
        gesture = Gesture::None;
        return true;
    }
    if (gesture != Gesture::None) {
        gesture = Gesture::None;
        region.clear();
        gradientHasStart = false;
        return true;
    }
    exitRequested = true;
    return true;
}

bool VertexPaintSceneTool::altClick(const SceneToolEvent& event) {
    // R104: Alt+click picks in every tool
    return VertexPaintTools::get().pickAt(sceneId, event.ray, event.shift);
}

bool VertexPaintSceneTool::rightDrag(const SceneToolEvent& event, float dx, float dy) {
    VertexPaintTools& tools = VertexPaintTools::get();
    const PaintTool tool = tools.currentTool();
    if (tool != PaintTool::Paint && tool != PaintTool::Smooth) return false;
    if (!event.ctrl && !event.shift) return false;
    applyRightDrag(tools.settings(), event.ctrl, event.shift && !event.ctrl, dx, dy);
    tools.cursorKey.valid = false;
    return true;
}

void VertexPaintSceneTool::cancel() {
    // the framework calls this right before the tool goes (exit button, Esc, another tool, scene change)
    VertexPaintTools::get().detach(this);
}

const SceneToolDecal* VertexPaintSceneTool::decal() const {
    const VertexPaintTools& tools = VertexPaintTools::get();
    const bool cursorOn = tools.cursorVisible();
    const PaintCursorLines& selection = tools.selectionLines();
    if (!cursorOn && selection.points.empty()) return nullptr;
    const uint64_t revision = (tools.cursorRevision() << 1) * 1000003ull + tools.selectionRevision() * 2654435761ull + (cursorOn ? 1 : 0);
    if (decalBuilt != revision) {
        decalLines.points = selection.points;
        decalLines.colors = selection.colors;
        if (cursorOn) {
            const PaintCursorLines& lines = tools.cursorLines();
            const float alpha = tools.cursorOpacity();
            decalLines.points.insert(decalLines.points.end(), lines.points.begin(), lines.points.end());
            for (Vector4 color : lines.colors) {
                color.w *= alpha;
                decalLines.colors.push_back(color);
            }
        }
        decalLines.revision = revision;
        decalBuilt = revision;
    }
    return &decalLines;
}

void VertexPaintSceneTool::drawOverlay(SceneRender& render, ImDrawList& list) {
    VertexPaintTools& tools = VertexPaintTools::get();
    // R103: the vertices the stamp moves
    if (tools.cursorVisible()) {
        const Vector4 tint = cursorTint(tools.settings(), tools.isStrokeActive() ? tools.strokeMode() : vc::StrokeMode::Paint, false, tools.cursorOpacity());
        const ImU32 dot = IM_COL32((int)(tint.x * 255.0f), (int)(tint.y * 255.0f), (int)(tint.z * 255.0f), (int)(tint.w * 220.0f));
        const ImU32 rim = IM_COL32(0, 0, 0, (int)(tint.w * 160.0f));
        for (const Vector3& p : tools.affectedVertices()) {
            ImVec2 screen;
            if (!SceneTools::worldToScreen(sceneId, render, p, screen)) continue;
            list.AddCircleFilled(screen, 2.5f, rim);
            list.AddCircleFilled(screen, 1.6f, dot);
        }
    }
    // the drag of the Select tool and the Gradient tool's line
    Project* project = tools.project();
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (gesture == Gesture::SelectRegion && regionMoved && region.size() >= 2 && sceneProject) {
        ImVec2 a, b;
        auto toScreen = [&](const Vector2& p) { return SceneTools::canvasToScreen(sceneId, p.x, p.y); };
        const ImU32 fill = IM_COL32(90, 160, 255, 40);
        const ImU32 edge = IM_COL32(90, 160, 255, 220);
        if (tools.settings().selectShape == SelectShape::Box) {
            a = toScreen(region.front());
            b = toScreen(region.back());
            list.AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), fill);
            list.AddRect(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), edge);
        } else {
            std::vector<ImVec2> points;
            for (const Vector2& p : region) points.push_back(toScreen(p));
            list.AddPolyline(points.data(), (int)points.size(), edge, ImDrawFlags_Closed, 1.5f);
        }
    }
    if (gesture == Gesture::Gradient && gradientHasStart) {
        ImVec2 a, b;
        if (SceneTools::worldToScreen(sceneId, render, gradientFrom, a) && SceneTools::worldToScreen(sceneId, render, gradientTo, b)) {
            const Vector4& p = tools.settings().primary;
            const Vector4& s = tools.settings().secondary;
            auto col = [](const Vector4& c) {
                return IM_COL32((int)(vc::linearToSrgb(std::clamp(c.x, 0.0f, 1.0f)) * 255.0f), (int)(vc::linearToSrgb(std::clamp(c.y, 0.0f, 1.0f)) * 255.0f),
                                (int)(vc::linearToSrgb(std::clamp(c.z, 0.0f, 1.0f)) * 255.0f), 255);
            };
            list.AddLine(a, b, IM_COL32(0, 0, 0, 200), 4.0f);
            list.AddLine(a, b, IM_COL32(255, 255, 255, 230), 2.0f);
            list.AddCircleFilled(a, 6.0f, col(p));
            list.AddCircle(a, 6.0f, IM_COL32(0, 0, 0, 220), 0, 1.5f);
            list.AddCircleFilled(b, 6.0f, col(s));
            list.AddCircle(b, 6.0f, IM_COL32(0, 0, 0, 220), 0, 1.5f);
        }
    }
}

std::string VertexPaintSceneTool::status() const {
    const VertexPaintTools& tools = VertexPaintTools::get();
    if (!tools.lastError().empty()) return tools.lastError();
    if (tools.currentTool() == PaintTool::Select) {
        const size_t count = tools.selectedCount(sceneId);
        if (count > 0) return selectionCountText(tools.settings().selectionMode, count);
    }
    return tools.statusText();
}

}  // namespace jasperengine::editor::vertexcolor
