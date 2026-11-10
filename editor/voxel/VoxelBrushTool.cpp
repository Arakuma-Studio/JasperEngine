// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelBrushTool.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/VoxelEditCmd.h"
#include "component/VoxelVolumeComponent.h"
#include "subsystem/VoxelSystem.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    constexpr float BRUSH_MIN_RADIUS = 0.1f;
    constexpr float BRUSH_MAX_RADIUS = 256.0f;
    // A dry run per hover stays well under a frame up to about this many samples (R60's strip)
    constexpr int64_t STAMP_DRY_RUN_SAMPLES = 64 * 64 * 64;

    std::shared_ptr<VoxelSystem> brushSystemOf(Project* project, uint32_t sceneId) {
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        return sceneProject && sceneProject->scene ? sceneProject->scene->findSystem<VoxelSystem>() : nullptr;
    }

    // Tools that read rather than write, or that stamp without a shape's source, cannot stroke
    const char* strokeProblem(const VoxelBrushSettings& settings) {
        if ((settings.shape == VoxelBrushShape::Mesh || settings.shape == VoxelBrushShape::Stamp) && !settings.field) {
            return settings.shape == VoxelBrushShape::Mesh ? "Choose a mesh for the Mesh shape first." : "Choose a .voxelvolume for the Stamp shape first.";
        }
        return nullptr;
    }

    std::string describeEditError(voxel::EditError code) {
        switch (code) {
            case voxel::EditError::BudgetExceeded: return "This stamp is over the edit budget: use a smaller brush.";
            case voxel::EditError::InvalidMaterial: return "The brush's material slot is not one of this volume's: pick another in Materials.";
            case voxel::EditError::InvalidShape: return "The brush's shape is not valid: check its size.";
            case voxel::EditError::MissingAsset: return "The brush's shape has no source: choose a mesh or a stamp.";
            case voxel::EditError::NotLoaded: return "The volume is not loaded.";
            case voxel::EditError::OutOfBounds:
            case voxel::EditError::None: break;
        }
        return {};
    }

}

VoxelBrushTool& VoxelBrushTool::get() {
    static VoxelBrushTool tool;
    return tool;
}

void VoxelBrushTool::reset() {
    cancelStroke();
    hideCursor();
    project = nullptr;
    sceneId = 0;
    owner = NULL_ENTITY;
    armed = false;
    alt.cancel();
    error.clear();
}

void VoxelBrushTool::setTarget(Project* targetProject, uint32_t targetScene, Entity targetOwner) {
    if (project == targetProject && sceneId == targetScene && owner == targetOwner) return;
    endStroke();
    hideCursor();
    project = targetProject;
    sceneId = targetScene;
    owner = targetOwner;
    armed = false;
    error.clear();
}

void VoxelBrushTool::arm(VoxelTool tool) {
    if (tool == VoxelTool::Pick && brush.tool != VoxelTool::Pick) toolBeforePick = brush.tool;
    brush.tool = tool;
    armed = true;
    error.clear();
}

void VoxelBrushTool::disarm() {
    endStroke();
    armed = false;
    hideCursor();
}

void VoxelBrushTool::adjustSize(float factor) {
    brush.radius = std::clamp(brush.radius * factor, BRUSH_MIN_RADIUS, BRUSH_MAX_RADIUS);
    decalRev++;
}

bool VoxelBrushTool::isEditingScene(Scene* scene) const {
    if (!armed || !project || !scene || owner == NULL_ENTITY) return false;
    if (project->isAnyScenePlaying()) return false;
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || sceneProject->scene != scene) return false;
    if (!scene->isEntityCreated(owner) || !scene->findComponent<VoxelVolumeComponent>(owner)) return false;
    std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>();
    return system && system->getStore(owner);
}

bool VoxelBrushTool::hitVolume(VoxelSystem& system, Entity owner, const Ray& ray, bool floorFallback, voxel::RaycastHit& hit, voxel::Store* surface) {
    voxel::Store* store = surface ? surface : system.getStore(owner);
    if (!store) return false;
    // The ray runs from origin to origin + direction: it spans the view, and the volume's frame keeps metres
    const Vector3 start = ray.getOrigin();
    const Vector3 end = start + ray.getDirection();
    voxel::Vec3d from, to;
    if (!system.worldToLocal(owner, start, from) || !system.worldToLocal(owner, end, to)) return false;
    const voxel::Vec3d along{to.x - from.x, to.y - from.y, to.z - from.z};
    const double distance = std::sqrt(along.x * along.x + along.y * along.y + along.z * along.z);
    if (!(distance > 0.0)) return false;
    hit = voxel::raycast(*store, from, along, distance);
    if (hit.hit || !floorFallback || along.y >= 0.0) return hit.hit;

    const double t = -from.y / along.y;
    if (t < 0.0 || t > 1.0) return false;
    const voxel::VolumeDesc& desc = store->desc();
    const voxel::Vec3d point{from.x + along.x * t, 0.0, from.z + along.z * t};
    if (point.x < 0.0 || point.z < 0.0 || point.x > desc.dimensions.x * desc.voxelSize || point.z > desc.dimensions.z * desc.voxelSize) return false;
    hit.hit = true;
    hit.point = point;
    hit.normal = voxel::Vec3d{0.0, 1.0, 0.0};
    hit.distance = t * distance;
    return true;
}

void VoxelBrushTool::openStroke(uint32_t scene, Entity volume, const VoxelBrushSettings& settings, VoxelTool effective, double spacing) {
    stroke.active = true;
    stroke.sceneId = scene;
    stroke.owner = volume;
    stroke.effective = effective;
    stroke.shape = settings.shape;
    stroke.settings = settings;
    stroke.stamps.clear();
    stroke.stamper = VoxelStrokeStamper(spacing);
    CommandHistory* history = CommandHandle::get(scene);
    // not merged with the stroke before it (R60: one stroke, one undo)
    history->mark();
    stroke.edit = history->beginInteractive(std::string());
}

bool VoxelBrushTool::applyStamp(const voxel::Vec3d& center, const voxel::Vec3d& end) {
    std::shared_ptr<VoxelSystem> system = brushSystemOf(project, stroke.sceneId);
    if (!system) return false;
    const voxel::EditOp op = voxelStampOp(stroke.settings, stroke.effective, center, end);
    voxel::EditOptions options;
    options.captureDelta = true;
    voxel::EditResult result = system->edit(stroke.owner, std::span<const voxel::EditOp>(&op, 1), options);
    if (!result.ok) {
        // past the volume's edge is a brush going off the volume, which a drag does all the time
        const std::string why = describeEditError(result.error);
        if (!why.empty()) error = why;
        return false;
    }
    if (result.delta.empty()) return true;
    CommandHistory* history = CommandHandle::get(stroke.sceneId);
    history->addCommand(new VoxelEditCmd(project, stroke.sceneId, stroke.owner, voxelStrokeName(stroke.effective, stroke.shape), std::move(result.delta), true));
    return true;
}

void VoxelBrushTool::applyStamps() {
    for (const voxel::Vec3d& center : stroke.stamps) applyStamp(center, center);
    stroke.stamps.clear();
}

bool VoxelBrushTool::strokeCancelled() {
    if (!stroke.active || stroke.edit.isOpen()) return false;
    // Esc reached the history (App's interactive cancel): the edits went with it
    stroke.active = false;
    stroke.edit = InteractiveEdit();
    stroke.surface.reset();
    return true;
}

void VoxelBrushTool::finishStroke() {
    CommandHistory* history = CommandHandle::get(stroke.sceneId);
    stroke.edit.commit();
    stroke.edit = InteractiveEdit();
    // what comes next starts its own step
    history->mark();
    stroke.active = false;
    stroke.surface.reset();
}

bool VoxelBrushTool::beginStroke(Scene* scene, const Ray& ray, VoxelBrushModifiers modifiers) {
    if (!isEditingScene(scene)) return false;
    endStroke();
    const VoxelTool effective = voxelEffectiveTool(brush.tool, modifiers.ctrl, modifiers.shift);
    if (effective == VoxelTool::Pick) {
        if (pick(scene, ray)) brush.tool = toolBeforePick;
        return true;
    }
    if (const char* problem = strokeProblem(brush)) {
        error = problem;
        return false;
    }
    std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>();
    voxel::RaycastHit hit;
    if (!hitVolume(*system, owner, ray, effective == VoxelTool::Add, hit)) return false;
    error.clear();
    const double spacing = voxelStrokeSpacing(brush.radius, system->getStore(owner)->desc().voxelSize);
    openStroke(sceneId, owner, brush, effective, spacing);
    stroke.surface = system->getStore(owner)->clone();
    if (stroke.shape == VoxelBrushShape::Capsule) {
        stroke.capsuleStart = hit.point;
        stroke.capsuleEnd = hit.point;
        return true;
    }
    stroke.stamper.start(hit.point, stroke.stamps);
    applyStamps();
    return true;
}

bool VoxelBrushTool::paintStroke(Scene* scene, const Ray& ray) {
    if (!stroke.active || strokeCancelled() || !isEditingScene(scene)) return false;
    std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>();
    voxel::RaycastHit hit;
    if (!hitVolume(*system, stroke.owner, ray, stroke.effective == VoxelTool::Add, hit, stroke.surface.get())) {
        stroke.stamper.lift();
        return false;
    }
    if (stroke.shape == VoxelBrushShape::Capsule) {
        stroke.capsuleEnd = hit.point;
        return true;
    }
    stroke.stamper.moveTo(hit.point, stroke.stamps);
    applyStamps();
    return true;
}

void VoxelBrushTool::endStroke() {
    if (!stroke.active || strokeCancelled()) return;
    // a capsule is one shape, placed when the drag ends
    if (stroke.shape == VoxelBrushShape::Capsule) applyStamp(stroke.capsuleStart, stroke.capsuleEnd);
    finishStroke();
}

void VoxelBrushTool::cancelStroke() {
    if (!stroke.active || strokeCancelled()) return;
    stroke.edit.cancel();
    stroke.edit = InteractiveEdit();
    stroke.active = false;
    stroke.surface.reset();
}

bool VoxelBrushTool::altRelease(Scene* scene, const Ray& ray) {
    if (!alt.release() || !isEditingScene(scene)) return false;
    return pick(scene, ray);
}

bool VoxelBrushTool::pick(Scene* scene, const Ray& ray) {
    std::shared_ptr<VoxelSystem> system = scene ? scene->findSystem<VoxelSystem>() : nullptr;
    voxel::RaycastHit hit;
    if (!system || !hitVolume(*system, owner, ray, false, hit)) return false;
    brush.material = hit.slot;
    return true;
}

void VoxelBrushTool::updateCursor(Scene* scene, const Ray& ray) {
    if (!isEditingScene(scene)) {
        hideCursor();
        return;
    }
    std::shared_ptr<VoxelSystem> system = scene->findSystem<VoxelSystem>();
    voxel::Store* store = system->getStore(owner);
    voxel::RaycastHit hit;
    const VoxelTool effective = stroke.active ? stroke.effective : liveEffective;

    // The view reports the pointer every frame. Resting means the same ray: the decal must fade (T1 R55),
    // and the rays draping it and the dry run behind the strip wait for something to change.
    const bool sameRay = cursor.valid && cursor.origin == ray.getOrigin() && cursor.direction == ray.getDirection();
    if (!sameRay) pointerMoved = std::chrono::steady_clock::now();
    if (sameRay && cursor.version == store->version() && cursor.tool == effective && cursor.radius == brush.radius && cursor.shape == brush.shape) return;
    cursor = CursorKey{true, ray.getOrigin(), ray.getDirection(), store->version(), effective, brush.radius, brush.shape};

    if (!hitVolume(*system, owner, ray, effective == VoxelTool::Add, hit, stroke.active ? stroke.surface.get() : nullptr)) {
        hideCursor();
        return;
    }
    hasCursorHit = true;
    cursorHit = hit;
    const voxel::Vec3d* capsuleStart = stroke.active && stroke.shape == VoxelBrushShape::Capsule ? &stroke.capsuleStart : nullptr;
    buildVoxelDecal(*store, brush, effective, hit, capsuleStart, cursorDecal);
    decalShown = true;
    decalRev++;

    info.valid = true;
    info.position = hit.point;
    info.value = voxel::valueAt(*store, hit.point);
    info.material = voxel::materialAtPoint(*store, hit.point);
    info.memoryBytes = store->memory().total();
    if (effective == VoxelTool::Pick) {
        info.stampVoxels = 0;
        info.stampEstimated = false;
        return;
    }
    const voxel::EditOp op = voxelStampOp(brush, effective, hit.point, hit.point);
    const voxel::IntBox box = voxel::shapeSampleBox(store->desc(), op.shape);
    const int64_t samples = box.empty() ? 0 : int64_t(box.max.x - box.min.x + 1) * (box.max.y - box.min.y + 1) * (box.max.z - box.min.z + 1);
    info.stampEstimated = samples > STAMP_DRY_RUN_SAMPLES;
    if (info.stampEstimated) {
        info.stampVoxels = samples;
    } else {
        voxel::EditOptions options;
        options.dryRun = true;
        const voxel::EditResult result = system->edit(owner, std::span<const voxel::EditOp>(&op, 1), options);
        info.stampVoxels = result.ok ? result.voxelsChanged : 0;
    }
}

void VoxelBrushTool::tick(VoxelBrushModifiers modifiers, bool cameraMoving) {
    if (stroke.active) strokeCancelled();
    const VoxelTool now = stroke.active ? stroke.effective : voxelEffectiveTool(brush.tool, modifiers.ctrl, modifiers.shift);
    if (now != liveEffective) {
        liveEffective = now;
        cursorDecal.colour = voxelDecalColour(now);
        decalRev++;
    }
    if (!decalShown) {
        alpha = 0.0f;
        return;
    }
    const double rested = std::chrono::duration<double>(std::chrono::steady_clock::now() - pointerMoved).count();
    // the stroke's own feedback: the decal does not fade under a held button
    const float wanted = stroke.active ? 1.0f : voxelDecalAlpha(rested, cameraMoving);
    if (wanted != alpha) {
        alpha = wanted;
        decalRev++;
    }
}

void VoxelBrushTool::hideCursor() {
    cursor.valid = false;
    hasCursorHit = false;
    info.valid = false;
    if (decalShown) decalRev++;
    decalShown = false;
    alpha = 0.0f;
}

bool VoxelBrushTool::scriptedStroke(Project* project, uint32_t sceneId, Entity owner, const VoxelBrushSettings& settings, VoxelTool effective,
                                    const std::vector<voxel::Vec3d>& points, std::string& why) {
    std::shared_ptr<VoxelSystem> system = brushSystemOf(project, sceneId);
    voxel::Store* store = system ? system->getStore(owner) : nullptr;
    if (!store) {
        why = "not_loaded";
        return false;
    }
    if (effective == VoxelTool::Pick) {
        why = "Pick writes nothing";
        return false;
    }
    if (points.empty()) {
        why = "A stroke needs a point.";
        return false;
    }
    if (const char* problem = strokeProblem(settings)) {
        why = problem;
        return false;
    }

    VoxelBrushTool runner;
    runner.project = project;
    runner.openStroke(sceneId, owner, settings, effective, voxelStrokeSpacing(settings.radius, store->desc().voxelSize));
    if (settings.shape == VoxelBrushShape::Capsule) {
        runner.applyStamp(points.front(), points.back());
    } else {
        runner.stroke.stamper.start(points.front(), runner.stroke.stamps);
        runner.applyStamps();
        for (size_t i = 1; i < points.size(); i++) {
            runner.stroke.stamper.moveTo(points[i], runner.stroke.stamps);
            runner.applyStamps();
        }
    }
    runner.finishStroke();
    why = runner.error;
    return why.empty();
}
